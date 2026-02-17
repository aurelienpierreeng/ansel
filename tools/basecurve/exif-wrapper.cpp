/*
    This file is part of darktable,
    Copyright (C) 2014-2015 Edouard Gomez.
    Copyright (C) 2019 Bertrand Antoine.
    Copyright (C) 2023 Alynx Zhou.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <exiv2/exif.hpp>
#include <exiv2/error.hpp>
#include <exiv2/image.hpp>
#include <exiv2/version.hpp>

#include <cstdio>
#include <cassert>

using namespace std;

extern "C" int
exif_get_ascii_datafield(
  const char* filename,
  const char* key,
  char* buf,
  size_t buflen)
{
  int ret = -1;

  try
  {
#if EXIV2_TEST_VERSION(0,28,0)
    Exiv2::Image::UniquePtr image = Exiv2::ImageFactory::open(filename);
#else
    Exiv2::Image::AutoPtr image = Exiv2::ImageFactory::open(filename);
#endif
    assert(image.get() != 0);
    image->readMetadata();

    Exiv2::ExifData &exifData = image->exifData();

#if EXIV2_TEST_VERSION(0,28,0)
    Exiv2::Value::UniquePtr val = exifData[key].getValue();
#else
    Exiv2::Value::AutoPtr val = exifData[key].getValue();
#endif

    if (val->typeId() != Exiv2::asciiString)
    {
      ret = -1;
      goto exit;
    }

    if ((size_t)val->size() >= buflen)
    {
      ret = val->size()+1;
      goto exit;
    }

    snprintf(buf, buflen, "%s", val->toString().c_str());

    ret = 0;
  }
  catch (Exiv2::Error& e)
  {
    ret = -1;
  }

exit:

  return ret;
}

