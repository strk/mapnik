/*****************************************************************************
 *
 * This file is part of Mapnik (c++ mapping toolkit)
 *
 * Copyright (C) 2014 Sandro Santilli <strk@keybit.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *****************************************************************************/

#include "pgraster_wkb_reader.hpp"

// mapnik
#include <mapnik/global.hpp>
#include <mapnik/debug.hpp>
#include <mapnik/ctrans.hpp>
#include <mapnik/raster.hpp>
#include <mapnik/image_data.hpp>
#include <mapnik/util/conversions.hpp>
#include <mapnik/util/trim.hpp>
#include <mapnik/box2d.hpp> // for box2d

// boost
#include <boost/cstdint.hpp> // for boost::int8_t
#include <boost/make_shared.hpp>

namespace {

uint8_t
read_uint8(const uint8_t** from) {
    return *(*from)++;
}

uint16_t
read_uint16(const boost::uint8_t** from, boost::uint8_t littleEndian) {
    uint16_t ret = 0;

    if (littleEndian) {
        ret = (*from)[0] |
                (*from)[1] << 8;
    } else {
        /* big endian */
        ret = (*from)[0] << 8 |
                (*from)[1];
    }
    *from += 2;
    return ret;
}

double
read_float64(const boost::uint8_t** from, boost::uint8_t littleEndian) {

    union {
        double d;
        uint64_t i;
    } ret;

    if (littleEndian) {
        ret.i = (uint64_t) ((*from)[0] & 0xff) |
                (uint64_t) ((*from)[1] & 0xff) << 8 |
                (uint64_t) ((*from)[2] & 0xff) << 16 |
                (uint64_t) ((*from)[3] & 0xff) << 24 |
                (uint64_t) ((*from)[4] & 0xff) << 32 |
                (uint64_t) ((*from)[5] & 0xff) << 40 |
                (uint64_t) ((*from)[6] & 0xff) << 48 |
                (uint64_t) ((*from)[7] & 0xff) << 56;
    } else {
        /* big endian */
        ret.i = (uint64_t) ((*from)[7] & 0xff) |
                (uint64_t) ((*from)[6] & 0xff) << 8 |
                (uint64_t) ((*from)[5] & 0xff) << 16 |
                (uint64_t) ((*from)[4] & 0xff) << 24 |
                (uint64_t) ((*from)[3] & 0xff) << 32 |
                (uint64_t) ((*from)[2] & 0xff) << 40 |
                (uint64_t) ((*from)[1] & 0xff) << 48 |
                (uint64_t) ((*from)[0] & 0xff) << 56;
    }

    *from += 8;
    return ret.d;
}

uint32_t
read_uint32(const boost::uint8_t** from, boost::uint8_t littleEndian) {
    uint32_t ret = 0;

    if (littleEndian) {
        ret = (uint32_t) ((*from)[0] & 0xff) |
                (uint32_t) ((*from)[1] & 0xff) << 8 |
                (uint32_t) ((*from)[2] & 0xff) << 16 |
                (uint32_t) ((*from)[3] & 0xff) << 24;
    } else {
        /* big endian */
        ret = (uint32_t) ((*from)[3] & 0xff) |
                (uint32_t) ((*from)[2] & 0xff) << 8 |
                (uint32_t) ((*from)[1] & 0xff) << 16 |
                (uint32_t) ((*from)[0] & 0xff) << 24;
    }

    *from += 4;
    return ret;
}

int32_t
read_int32(const boost::uint8_t** from, boost::uint8_t littleEndian) {

    return read_uint32(from, littleEndian);
}

typedef enum {
    PT_1BB=0,     /* 1-bit boolean            */
    PT_2BUI=1,    /* 2-bit unsigned integer   */
    PT_4BUI=2,    /* 4-bit unsigned integer   */
    PT_8BSI=3,    /* 8-bit signed integer     */
    PT_8BUI=4,    /* 8-bit unsigned integer   */
    PT_16BSI=5,   /* 16-bit signed integer    */
    PT_16BUI=6,   /* 16-bit unsigned integer  */
    PT_32BSI=7,   /* 32-bit signed integer    */
    PT_32BUI=8,   /* 32-bit unsigned integer  */
    PT_32BF=10,   /* 32-bit float             */
    PT_64BF=11,   /* 64-bit float             */
    PT_END=13
} rt_pixtype;

#define BANDTYPE_FLAGS_MASK 0xF0
#define BANDTYPE_PIXTYPE_MASK 0x0F
#define BANDTYPE_FLAG_OFFDB     (1<<7)
#define BANDTYPE_FLAG_HASNODATA (1<<6)
#define BANDTYPE_FLAG_ISNODATA  (1<<5)
#define BANDTYPE_FLAG_RESERVED3 (1<<4)

#define BANDTYPE_PIXTYPE_MASK 0x0F
#define BANDTYPE_PIXTYPE(x) ((x)&BANDTYPE_PIXTYPE_MASK)
#define BANDTYPE_IS_OFFDB(x) ((x)&BANDTYPE_FLAG_OFFDB)
#define BANDTYPE_HAS_NODATA(x) ((x)&BANDTYPE_FLAG_HASNODATA)
#define BANDTYPE_IS_NODATA(x) ((x)&BANDTYPE_FLAG_ISNODATA)

}

using mapnik::box2d;

void
pgraster_wkb_reader::read_grayscale(mapnik::raster_ptr raster)
{
  mapnik::image_data_32 & image = raster->data_;

  // Start with plain white (ABGR or RGBA depending on endiannes)
  image.set(0xffffffff);

  raster->premultiplied_alpha_ = true;

  uint8_t type = read_uint8(&ptr_);

  int pixtype = BANDTYPE_PIXTYPE(type);
  int offline = BANDTYPE_IS_OFFDB(type) ? 1 : 0;
  int hasnodata = BANDTYPE_HAS_NODATA(type) ? 1 : 0;

  MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: band type:"
        << pixtype << " offline:" << offline
        << " hasnodata:" << hasnodata;

  if ( offline ) {
    MAPNIK_LOG_WARN(pgraster) << "pgraster_featureset: offline band "
          " unsupported";
    return;
  }

  if ( pixtype > PT_8BUI || pixtype < PT_8BSI ) {
    MAPNIK_LOG_WARN(pgraster) << "pgraster_featureset: band "
          "type " << type << " unsupported";
    return;
  }

  uint8_t nodataval = read_uint8(&ptr_); // need to read anyway
  if ( hasnodata ) {
    MAPNIK_LOG_WARN(pgraster) <<
      "pgraster_featureset: nodata value unsupported";
  }

  int ps = 4; // sizeof(image_data::pixel_type)
  uint8_t * image_data = image.getBytes();
  for (int y=0; y<height_; ++y) {
    //uint8_t *optr = image_data + y * width_ * ps;
    for (int x=0; x<width_; ++x) {
      uint8_t val = read_uint8(&ptr_);
      int off = y * width_ * ps + x * ps;
      // Pixel space is RGBA
      image_data[off+0] = val;
      image_data[off+1] = val;
      image_data[off+2] = val;
    }
  }
}

void
pgraster_wkb_reader::read_rgb(mapnik::raster_ptr raster)
{
  mapnik::image_data_32 & image = raster->data_;

  // Start with plain white (ABGR or RGBA depending on endiannes)
  image.set(0xffffffff);
  //raster->set_nodata(0xffffffff);

  raster->premultiplied_alpha_ = true;

  uint8_t nodataval;
  for (int bn=0; bn<numBands_; ++bn) {
    uint8_t type = read_uint8(&ptr_);

    int pixtype = BANDTYPE_PIXTYPE(type);
    int offline = BANDTYPE_IS_OFFDB(type) ? 1 : 0;
    int hasnodata = BANDTYPE_HAS_NODATA(type) ? 1 : 0;

    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: band " << bn
          << " type:" << pixtype << " offline:" << offline
          << " hasnodata:" << hasnodata;

    if ( offline ) {
      MAPNIK_LOG_WARN(pgraster) << "pgraster_featureset: offline band "
            << bn << " unsupported";
      continue;
    }

    if ( pixtype > PT_8BUI || pixtype < PT_8BSI ) {
      MAPNIK_LOG_WARN(pgraster) << "pgraster_featureset: band " << bn
            << " type " << type << " unsupported";
      continue;
    }

    uint8_t tmp = read_uint8(&ptr_);
    if ( ! bn ) nodataval = tmp;
    else if ( tmp != nodataval ) {
      MAPNIK_LOG_WARN(pgraster) << "pgraster_featureset: band " << bn
            << " nodataval " << tmp << " != band 0 nodataval " << nodataval;
    }

    int ps = 4; // sizeof(image_data::pixel_type)
    uint8_t * image_data = image.getBytes();
    for (int y=0; y<height_; ++y) {
      //uint8_t *optr = image_data + y * width_ * ps;
      for (int x=0; x<width_; ++x) {
        uint8_t val = read_uint8(&ptr_);
        // optr would now point to an ABGR pixel
        // we'll consider first band  (bn=0) to be R optr[4-0-1] = optr[3]
        // we'll consider second band (bn=1) to be G optr[4-1-1] = optr[2]
        // we'll consider third  band (bn=2) to be B optr[4-2-1] = optr[1]
        // optr[0] will be alpha
        //optr[0] = 255;
        ///////////optr[ps - bn - 1] = val;
        // y * width_ * ps is the row (ps is pixel size)
        // x * ps is the column
        int off = y * width_ * ps + x * ps;
        // Pixel space is RGBA
        image_data[off+bn] = val;
      }
    }
  }
}

mapnik::raster_ptr
pgraster_wkb_reader::get_raster() {

    /* Read endianness */
    boost::uint8_t endian = *ptr_;
    endian = *ptr_;
    ptr_ += 1;

    /* Read version of protocol */
    uint16_t version = read_uint16(&ptr_, endian);
    if (version != 0) {
       MAPNIK_LOG_WARN(pgraster) << "pgraster_featureset: WKB version "
          << version << " unsupported";
      return mapnik::raster_ptr();
    }

    numBands_ = read_uint16(&ptr_, endian);
    double scaleX = read_float64(&ptr_, endian);
    double scaleY = read_float64(&ptr_, endian);
    double ipX = read_float64(&ptr_, endian);
    double ipY = read_float64(&ptr_, endian);
    double skewX = read_float64(&ptr_, endian);
    double skewY = read_float64(&ptr_, endian);
    int32_t srid = read_int32(&ptr_, endian);
    width_ = read_uint16(&ptr_, endian);
    height_ = read_uint16(&ptr_, endian);

    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: numBands=" << numBands_;
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: scaleX=" << scaleX;
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: scaleY=" << scaleY;
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: ipX=" << ipX;
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: ipY=" << ipY;
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: skewX=" << skewX;
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: skewY=" << skewY;
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: srid=" << srid;
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: size="
      << width_ << "x" << height_;


    if ( skewX || skewY ) {
      MAPNIK_LOG_WARN(pgraster) << "pgraster_featureset: raster rotation is not supported";
      return mapnik::raster_ptr();
    }

    box2d<double> ext(ipX,ipY,ipX+(width_*scaleX),ipY+(height_*scaleY)); 
    MAPNIK_LOG_DEBUG(pgraster) << "pgraster_featureset: Raster extent=" << ext;

    mapnik::raster_ptr raster = boost::make_shared<mapnik::raster>(ext, width_, height_);

    switch (numBands_) {
      case 1:
        read_grayscale(raster);
        break;
      case 3:
        read_rgb(raster);
        break;
      default:
        MAPNIK_LOG_WARN(pgraster) << "pgraster_featureset: raster with "
          << numBands_ << " bands is not supported";
        return mapnik::raster_ptr();
    }

    return raster;

}


