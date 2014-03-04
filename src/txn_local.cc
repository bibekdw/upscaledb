/*
 * Copyright (C) 2005-2014 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 *
 */

#include "config.h"

#include <string.h>

#include "journal.h"
#include "txn_local.h"
#include "txn_factory.h"
#include "txn_cursor.h"
#include "env_local.h"
#include "btree_index.h"

namespace hamsterdb {

/* stuff for rb.h */
#ifndef __ssize_t_defined
typedef signed ssize_t;
#endif
#ifndef __cplusplus
typedef int bool;
#define true 1
#define false (!true)
#endif /* __cpluscplus */

int g_flush_threshold = 10;

static int
compare(void *vlhs, void *vrhs)
{
  TransactionNode *lhs = (TransactionNode *)vlhs;
  TransactionNode *rhs = (TransactionNode *)vrhs;
  LocalDatabase *db = lhs->get_db();

  if (lhs == rhs)
    return (0);

  return (db->get_btree_index()->compare_keys(lhs->get_key(), rhs->get_key()));
}

static void *
copy_key_data(ham_key_t *key)
{
  void *data = 0;

  if (key->data && key->size) {
    data = Memory::allocate<void>(key->size);
    if (!data)
      return (0);
    memcpy(data, key->data, key->size);
  }

  return (data);
}

rb_wrap(static, rbt_, TransactionIndex, TransactionNode, node, compare)

void
TransactionOperation::initialize(LocalTransaction *txn, TransactionNode *node,
            ham_u32_t flags, ham_u32_t orig_flags, ham_u64_t lsn,
            ham_record_t *record)
{
  memset(this, 0, sizeof(*this));

  m_txn = txn;
  m_node = node;
  m_flags = flags;
  m_lsn = lsn;
  m_orig_flags = orig_flags;

  /* create a copy of the record structure */
  if (record) {
    m_record = *record;
    if (record->size) {
      m_record.data = &m_data[0];
      memcpy(m_record.data, record->data, record->size);
    }
  }
}

void
TransactionOperation::destroy()
{
  m_record.data = 0;

  /* remove this operation from the two linked lists */
  TransactionOperation *next = get_next_in_node();
  TransactionOperation *prev = get_previous_in_node();
  if (next)
    next->set_previous_in_node(prev);
  if (prev)
    prev->set_next_in_node(next);

  next = get_next_in_txn();
  prev = get_previous_in_txn();
  if (next)
    next->set_previous_in_txn(prev);
  if (prev)
    prev->set_next_in_txn(next);

  /* remove this op from the node */
  // TODO should this be done in here??
  TransactionNode *node = get_node();
  if (node->get_oldest_op() == this)
    node->set_oldest_op(get_next_in_node());

  /* if the node is empty: remove the node from the tree */
  // TODO should this be done in here??
  if (node->get_oldest_op() == 0) {
    node->get_db()->get_txn_index()->remove(node);
    delete node;
  }

  Memory::release(this);
}

TransactionNode *
TransactionNode::get_next_sibling()
{
  return (rbt_next(get_db()->get_txn_index(), this));
}

TransactionNode *
TransactionNode::get_previous_sibling()
{
  return (rbt_prev(get_db()->get_txn_index(), this));
}

TransactionNode::TransactionNode(LocalDatabase *db, ham_key_t *key)
  : m_db(db), m_oldest_op(0), m_newest_op(0)
{
  /* make sure that a node with this key does not yet exist */
  // TODO re-enable this; currently leads to a stack overflow because
  // TransactionIndex::get() creates a new TransactionNode
  // ham_assert(TransactionIndex::get(key, 0) == 0);

  if (key) {
    m_key = *key;
    m_key.data = copy_key_data(key);
  }
  else
    memset(&m_key, 0, sizeof(m_key));
}

TransactionNode::~TransactionNode()
{
  Memory::release(m_key.data);
}

TransactionOperation *
TransactionNode::append(LocalTransaction *txn, ham_u32_t orig_flags,
      ham_u32_t flags, ham_u64_t lsn, ham_record_t *record)
{
  TransactionOperation *op = TransactionFactory::create_operation(txn,
                                    this, flags, orig_flags, lsn, record);

  /* store it in the chronological list which is managed by the node */
  if (!get_newest_op()) {
    ham_assert(get_oldest_op() == 0);
    set_newest_op(op);
    set_oldest_op(op);
  }
  else {
    TransactionOperation *newest = get_newest_op();
    newest->set_next_in_node(op);
    op->set_previous_in_node(newest);
    set_newest_op(op);
  }

  /* store it in the chronological list which is managed by the transaction */
  if (!txn->get_newest_op()) {
    ham_assert(txn->get_oldest_op() == 0);
    txn->set_newest_op(op);
    txn->set_oldest_op(op);
  }
  else {
    TransactionOperation *newest = txn->get_newest_op();
    newest->set_next_in_txn(op);
    op->set_previous_in_txn(newest);
    txn->set_newest_op(op);
  }

  return (op);
}

void
TransactionIndex::store(TransactionNode *node)
{
  rbt_insert(this, node);
}

void
TransactionIndex::remove(TransactionNode *node)
{
  rbt_remove(this, node);
}

LocalTransaction::LocalTransaction(LocalEnvironment *env, const char *name,
        ham_u32_t flags)
  : Transaction(env, name, flags), m_log_desc(0), m_oldest_op(0),
    m_newest_op(0)
{
  m_id = env->get_incremented_txn_id();

  /* append journal entry */
  if (env->get_flags() & HAM_ENABLE_RECOVERY
      && env->get_flags() & HAM_ENABLE_TRANSACTIONS
      && !(flags & HAM_TXN_TEMPORARY)) {
    env->get_journal()->append_txn_begin(this, env, name,
            env->get_incremented_lsn());
  }

  /* link this txn with the Environment */
  env->append_txn_at_tail(this);
}

LocalTransaction::~LocalTransaction()
{
  free_operations();
}

void
LocalTransaction::commit(ham_u32_t flags)
{
  /* are cursors attached to this txn? if yes, fail */
  if (get_cursor_refcount()) {
    ham_trace(("Transaction cannot be committed till all attached "
          "Cursors are closed"));
    throw Exception(HAM_CURSOR_STILL_OPEN);
  }

  LocalEnvironment *lenv = dynamic_cast<LocalEnvironment *>(m_env);

  /* this transaction is now committed! */
  m_flags |= kStateCommitted;

  /* append journal entry */
  if (m_env->get_flags() & HAM_ENABLE_RECOVERY
      && m_env->get_flags() & HAM_ENABLE_TRANSACTIONS
      && !(m_flags & HAM_TXN_TEMPORARY))
    lenv->get_journal()->append_txn_commit(this, lenv->get_incremented_lsn());

  /* flush committed transactions */
  if (m_id % g_flush_threshold == 0
        || (lenv->get_flags() & HAM_FLUSH_WHEN_COMMITTED))
    lenv->flush_committed_txns();
}

void
LocalTransaction::abort(ham_u32_t flags)
{
  /* are cursors attached to this txn? if yes, fail */
  if (get_cursor_refcount()) {
    ham_trace(("Transaction cannot be aborted till all attached "
          "Cursors are closed"));
    throw Exception(HAM_CURSOR_STILL_OPEN);
  }

  LocalEnvironment *lenv = dynamic_cast<LocalEnvironment *>(m_env);

  /* this transaction is now aborted!  */
  m_flags |= kStateAborted;

  /* append journal entry */
  if (m_env->get_flags() & HAM_ENABLE_RECOVERY
      && m_env->get_flags() & HAM_ENABLE_TRANSACTIONS
      && !(m_flags & HAM_TXN_TEMPORARY))
    lenv->get_journal()->append_txn_abort(this, lenv->get_incremented_lsn());

  /* immediately release memory of the cached operations */
  free_operations();

  /* clean up the changeset */
  if (lenv)
    lenv->get_changeset().clear();

  /* flush committed transactions; while this one was not committed,
   * we might have cleared the way now to flush other committed
   * transactions */
  if (m_id % g_flush_threshold == 0
        || (lenv->get_flags() & HAM_FLUSH_WHEN_COMMITTED))
    lenv->flush_committed_txns();
}

void
LocalTransaction::free_operations()
{
  TransactionOperation *n, *op = get_oldest_op();

  while (op) {
    n = op->get_next_in_txn();
    TransactionFactory::destroy_operation(op);
    op = n;
  }

  set_oldest_op(0);
  set_newest_op(0);
}

TransactionIndex::TransactionIndex(LocalDatabase *db)
  : m_db(db)
{
  rbt_new(this);

  // avoid warning about unused function
  if (0) {
    (void) (rbt_ppsearch(this, 0));
  }
}

TransactionIndex::~TransactionIndex()
{
  TransactionNode *node;

  while ((node = rbt_last(this))) {
    remove(node);
    delete node;
  }

  // re-initialize the tree
  rbt_new(this);
}

TransactionNode *
TransactionIndex::get(ham_key_t *key, ham_u32_t flags)
{
  TransactionNode *node = 0;
  int match = 0;

  /* create a temporary node that we can search for */
  TransactionNode tmp(m_db, key);

  /* search if node already exists - if yes, return it */
  if ((flags & HAM_FIND_GEQ_MATCH) == HAM_FIND_GEQ_MATCH) {
    node = rbt_nsearch(this, &tmp);
    if (node)
      match = compare(&tmp, node);
  }
  else if ((flags & HAM_FIND_LEQ_MATCH) == HAM_FIND_LEQ_MATCH) {
    node = rbt_psearch(this, &tmp);
    if (node)
      match = compare(&tmp, node);
  }
  else if (flags & HAM_FIND_GT_MATCH) {
    node = rbt_search(this, &tmp);
    if (node)
      node = node->get_next_sibling();
    else
      node = rbt_nsearch(this, &tmp);
    match = 1;
  }
  else if (flags & HAM_FIND_LT_MATCH) {
    node = rbt_search(this, &tmp);
    if (node)
      node = node->get_previous_sibling();
    else
      node = rbt_psearch(this, &tmp);
    match = -1;
  }
  else
    return (rbt_search(this, &tmp));

  /* tree is empty? */
  if (!node)
    return (0);

  /* approx. matching: set the key flag */
  if (match < 0)
    ham_key_set_intflags(key, (ham_key_get_intflags(key)
            & ~BtreeKey::kApproximate) | BtreeKey::kLower);
  else if (match > 0)
    ham_key_set_intflags(key, (ham_key_get_intflags(key)
            & ~BtreeKey::kApproximate) | BtreeKey::kGreater);

  return (node);
}

TransactionNode *
TransactionIndex::get_first()
{
  return (rbt_first(this));
}

TransactionNode *
TransactionIndex::get_last()
{
  return (rbt_last(this));
}

void
TransactionIndex::enumerate(TransactionIndex::Visitor *visitor)
{
  TransactionNode *node = rbt_first(this);

  while (node) {
    visitor->visit(node);
    node = rbt_next(this, node);
  }
}

struct KeyCounter : public TransactionIndex::Visitor
{
  KeyCounter(LocalDatabase *_db, LocalTransaction *_txn, ham_u32_t _flags)
    : counter(0), flags(_flags), txn(_txn), db(_db) {
  }

  void visit(TransactionNode *node) {
    BtreeIndex *be = db->get_btree_index();
    TransactionOperation *op;

    /*
     * look at each tree_node and walk through each operation
     * in reverse chronological order (from newest to oldest):
     * - is this op part of an aborted txn? then skip it
     * - is this op part of a committed txn? then include it
     * - is this op part of an txn which is still active? then include it
     * - if a committed txn has erased the item then there's no need
     *    to continue checking older, committed txns of the same key
     *
     * !!
     * if keys are overwritten or a duplicate key is inserted, then
     * we have to consolidate the btree keys with the txn-tree keys.
     */
    op = node->get_newest_op();
    while (op) {
      LocalTransaction *optxn = op->get_txn();
      if (optxn->is_aborted())
        ; // nop
      else if (optxn->is_committed() || txn == optxn) {
        if (op->get_flags() & TransactionOperation::kIsFlushed)
          ; // nop
        // if key was erased then it doesn't exist
        else if (op->get_flags() & TransactionOperation::kErase)
          return;
        else if (op->get_flags() & TransactionOperation::kInsert) {
          counter++;
          return;
        }
        // key exists - include it
        else if ((op->get_flags() & TransactionOperation::kInsert)
            || (op->get_flags() & TransactionOperation::kInsertOverwrite)) {
          // check if the key already exists in the btree - if yes,
          // we do not count it (it will be counted later)
          if (HAM_KEY_NOT_FOUND == be->find(0, 0, node->get_key(), 0, 0))
            counter++;
          return;
        }
        else if (op->get_flags() & TransactionOperation::kInsertDuplicate) {
          // check if btree has other duplicates
          if (0 == be->find(0, 0, node->get_key(), 0, 0)) {
            // yes, there's another one
            if (flags & HAM_SKIP_DUPLICATES)
              return;
            else
              counter++;
          }
          else {
            // check if other key is in this node
            counter++;
            if (flags & HAM_SKIP_DUPLICATES)
              return;
          }
        }
        else if (!(op->get_flags() & TransactionOperation::kNop)) {
          ham_assert(!"shouldn't be here");
          return;
        }
      }
      else { // txn is still active
        counter++;
      }

      op = op->get_previous_in_node();
    }
  }

  ham_u64_t counter;
  ham_u32_t flags;
  LocalTransaction *txn;
  LocalDatabase *db;
};

ham_u64_t
TransactionIndex::get_key_count(LocalTransaction *txn, ham_u32_t flags)
{
  KeyCounter k(m_db, txn, flags);
  enumerate(&k);
  return (k.counter);
}


} // namespace hamsterdb
