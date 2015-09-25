/*
 * Copyright (C) 2005-2015 Christoph Rupp (chris@crupp.de).
 * All Rights Reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * See the file COPYING for License information.
 */

#include "0root/root.h"

// Always verify that a file of level N does not include headers > N!
#include "2compressor/compressor_factory.h"
#include "2compressor/compressor_zlib.h"
#include "2compressor/compressor_snappy.h"
#include "2compressor/compressor_lzf.h"
#include "2compressor/compressor_lzop.h"

#ifndef UPS_ROOT_H
#  error "root.h was not included"
#endif

namespace hamsterdb {

bool
CompressorFactory::is_available(int type)
{
#ifdef UPS_ENABLE_COMPRESSION
  switch (type) {
    case UPS_COMPRESSOR_UINT32_VARBYTE:
    case UPS_COMPRESSOR_UINT32_SIMDCOMP:
    case UPS_COMPRESSOR_UINT32_GROUPVARINT:
    case UPS_COMPRESSOR_UINT32_STREAMVBYTE:
    case UPS_COMPRESSOR_UINT32_MASKEDVBYTE:
    case UPS_COMPRESSOR_UINT32_BLOCKINDEX:
    case UPS_COMPRESSOR_UINT32_FOR:
    case UPS_COMPRESSOR_UINT32_SIMDFOR:
      return (true);
    case UPS_COMPRESSOR_ZLIB:
#ifdef HAVE_ZLIB_H
      return (true);
#else
      return (false);
#endif
    case UPS_COMPRESSOR_SNAPPY:
#ifdef HAVE_SNAPPY_H
      return (true);
#else
      return (false);
#endif
    case UPS_COMPRESSOR_LZO:
#ifdef HAVE_LZO_LZO1X_H
      return (true);
#else
      return (false);
#endif
    case UPS_COMPRESSOR_LZF:
      // this is always available
      return (true);
    default:
      return (false);
  }
#endif // UPS_ENABLE_COMPRESSION
  return (false);
}

Compressor *
CompressorFactory::create(int type)
{
#ifdef UPS_ENABLE_COMPRESSION
  switch (type) {
    case UPS_COMPRESSOR_ZLIB:
#ifdef HAVE_ZLIB_H
      return (new ZlibCompressor());
#else
      ups_log(("hamsterdb was built without support for zlib compression"));
      throw Exception(UPS_INV_PARAMETER);
#endif
    case UPS_COMPRESSOR_SNAPPY:
#ifdef HAVE_SNAPPY_H
      return (new SnappyCompressor());
#else
      ups_log(("hamsterdb was built without support for snappy compression"));
      throw Exception(UPS_INV_PARAMETER);
#endif
    case UPS_COMPRESSOR_LZO:
#ifdef HAVE_LZO_LZO1X_H
      return (new LzopCompressor());
#else
      ups_log(("hamsterdb was built without support for lzop compression"));
      throw Exception(UPS_INV_PARAMETER);
#endif
    case UPS_COMPRESSOR_LZF:
      // this is always available
      return (new LzfCompressor());
    default:
      ups_log(("Unknown compressor type %d", type));
      throw Exception(UPS_INV_PARAMETER);
  }
#endif // UPS_ENABLE_COMPRESSION
  ups_log(("hamsterdb was built without compression"));
  throw Exception(UPS_INV_PARAMETER);
}

}; // namespace hamsterdb
