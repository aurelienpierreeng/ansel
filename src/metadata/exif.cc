/*
   This file is part of darktable,
   Copyright (C) 2009-2014, 2016 johannes hanika.
   Copyright (C) 2010-2012 Henrik Andersson.
   Copyright (C) 2010, 2012-2015 Pascal de Bruijn.
   Copyright (C) 2010-2018 Tobias Ellinghaus.
   Copyright (C) 2011-2012, 2018 Edouard Gomez.
   Copyright (C) 2011 Jochen Schroeder.
   Copyright (C) 2011 Johanes Schneider.
   Copyright (C) 2011-2012 Jérémy Rosen.
   Copyright (C) 2011 Kanstantsin Shautsou.
   Copyright (C) 2011 Robert Bieber.
   Copyright (C) 2011, 2013 Simon Spannagel.
   Copyright (C) 2011 Uli Scholler.
   Copyright (C) 2012-2013, 2020 Aldric Renaudin.
   Copyright (C) 2012 Alexander Clausen.
   Copyright (C) 2012 James C. McPherson.
   Copyright (C) 2012-2013 José Carlos García Sogo.
   Copyright (C) 2012 Richard Wonka.
   Copyright (C) 2012 Sergey Pavlov.
   Copyright (C) 2012-2014, 2016 Ulrich Pegelow.
   Copyright (C) 2012 Wolfgang Kühnel.
   Copyright (C) 2013 Dennis Gnad.
   Copyright (C) 2013 Jacek Naglak.
   Copyright (C) 2013-2015, 2018-2022 Pascal Obry.
   Copyright (C) 2013-2017 Roman Lebedev.
   Copyright (C) 2014-2015 Jan Niklas Fingerle.
   Copyright (C) 2014 Michael Neumann.
   Copyright (C) 2014-2016 Pedro Côrte-Real.
   Copyright (C) 2015 Jake Probst.
   Copyright (C) 2015, 2018 Marcel Müller.
   Copyright (C) 2015 Tom Vijlbrief.
   Copyright (C) 2015 Torsten Bronger.
   Copyright (C) 2016 Matthieu Volat.
   Copyright (C) 2017, 2020 David-Tillmann Schaefer.
   Copyright (C) 2017, 2019, 2021 luzpaz.
   Copyright (C) 2018-2019 Andreas Schneider.
   Copyright (C) 2018-2019 Edgardo Hoszowski.
   Copyright (C) 2018 Fabian Wenzel.
   Copyright (C) 2019 codingdave.
   Copyright (C) 2019-2021 Hanno Schwalm.
   Copyright (C) 2019-2022 Philippe Weyland.
   Copyright (C) 2020 Chris Elston.
   Copyright (C) 2020-2021 Hubert Kowalski.
   Copyright (C) 2020 JP Verrue.
   Copyright (C) 2020 Matt Maguire.
   Copyright (C) 2020-2022 Miloš Komarčević.
   Copyright (C) 2020-2021 Ralf Brown.
   Copyright (C) 2021, 2023, 2025-2026 Aurélien PIERRE.
   Copyright (C) 2021 Daniel Vogelbacher.
   Copyright (C) 2021 Victor Forsiuk.
   Copyright (C) 2022 gi-man.
   Copyright (C) 2022 Martin Bařinka.
   Copyright (C) 2022 paolodepetrillo.
   Copyright (C) 2023, 2025 Alynx Zhou.
   Copyright (C) 2023 Ricky Moon.
   Copyright (C) 2025-2026 Guillaume Stutin.
   
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

#define __STDC_FORMAT_MACROS

/** @file metadata/exif.cc
 *
 * @brief The EXIF/IPTC/XMP tag half of what used to be `common/exif.cc`.
 *
 * @details Cut out of a 4775-line translation unit that was doing two unrelated jobs. This
 * one reads and writes what a photograph says about itself; the other -- the XMP sidecar
 * carrying the development -- is `common/xmp_sidecar.cc`. See `metadata/exif.h`.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"   // conditional-ok: generated only when the build system defines HAVE_CONFIG_H
#endif

#include <glib.h>
#include <sys/stat.h>
#include <time.h>
#include <zlib.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

#include <exiv2/exiv2.hpp>

#if defined(_WIN32) && defined(EXV_UNICODE_PATH)
  #define WIDEN(s) pugi::as_wide(s)
#else
  #define WIDEN(s) (s)
#endif

#include <pugixml.hpp>

#include "metadata/exif.h"
#include "metadata/exif_internal.h"

#include "common/conf.h"
#include "common/datetime.h"
#include "common/dng_opcode.h"
#include "common/logging.h"
#include "common/utility.h"
#include "database/tag_repository.h"
#include "math/math.h"
#include "metadata/colorlabels.h"
#include "metadata/metadata.h"
#include "metadata/notify.h"
#include "metadata/tags.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

using namespace std;

#if EXIV2_TEST_VERSION(0,28,0)
#define AnyError Error
#define toLong toInt64
#endif

// persistent list of exiv2 tags. set up in dt_init()
static GList *exiv2_taglist = NULL;

static const char *_get_exiv2_type(const int type)
{
  switch(type)
  {
    case 1:
      return "Byte";
    case 2:
      return "Ascii";
    case 3:
      return "Short";
    case 4:
      return "Long";
    case 5:
      return "Rational"; // two LONGs: numerator and denumerator of a fraction
    case 6:
      return "SByte";
    case 7:
      return "Undefined";
    case 8:
      return "SShort";
    case 9:
      return "SLong";
    case 10:
      return "SRational"; // two SLONGs: numerator and denumerator of a fraction.
    case 11:
      return "Float"; //  single precision (4-byte) IEEE format
    case 12:
      return "Double"; // double precision (8-byte) IEEE format.
    case 13:
      return "Ifd";  // 32-bit (4-byte) unsigned integer
    case 16:
      return "LLong"; // 64-bit (8-byte) unsigned integer
    case 17:
      return "LLong"; // 64-bit (8-byte) signed integer
    case 18:
      return "Ifd8"; // 64-bit (8-byte) unsigned integer
    case 0x10000:
      return "String";
    case 0x10001:
      return "Date";
    case 0x10002:
      return "Time";
    case 0x10003:
      return "Comment";
    case 0x10004:
      return "Directory";
    case 0x10005:
      return "XmpText";
    case 0x10006:
      return "XmpAlt";
    case 0x10007:
      return "XmpBag";
    case 0x10008:
      return "XmpSeq";
    case 0x10009:
      return "LangAlt";
    case 0x1fffe:
      return "Invalid";
    case 0x1ffff:
      return "LastType";
    default:
      return "Invalid";
  }
}

static void _get_xmp_tags(const char *prefix, GList **taglist)
{
  const Exiv2::XmpPropertyInfo *pl = Exiv2::XmpProperties::propertyList(prefix);
  if(pl)
  {
    for (int i = 0; pl[i].name_ != 0; ++i)
    {
      char *tag = dt_util_dstrcat(NULL, "Xmp.%s.%s,%s", prefix, pl[i].name_, _get_exiv2_type(pl[i].typeId_));
      *taglist = g_list_prepend(*taglist, tag);
    }
  }
}

static int _illu_to_temp(dt_dng_illuminant_t illu)
{
  switch(illu)
  {
    case DT_LS_StandardLightA:
    case DT_LS_Tungsten:
      return 2850;
    case DT_LS_ISOStudioTungsten:
      return 3200;
    case DT_LS_StandardLightB:
      return 4871;
    case DT_LS_StandardLightC:
      return 6774;
    case DT_LS_D50:
      return 5000;
    case DT_LS_D55:
    case DT_LS_Daylight:
    case DT_LS_FineWeather:
    case DT_LS_Flash:
      return 5500;
    case DT_LS_D65:
    case DT_LS_CloudyWeather:
      return 6500;
    case DT_LS_D75:
    case DT_LS_Shade:
      return 7500;
    case DT_LS_DaylightFluorescent:
      return 6430;
    case DT_LS_DayWhiteFluorescent:
      return 5000;
    case DT_LS_CoolWhiteFluorescent:
      return 4150;
    case DT_LS_Fluorescent:
      return 4230;
    case DT_LS_WhiteFluorescent:
      return 3450;
    case DT_LS_WarmWhiteFluorescent:
      return 2940;
    default:
      return 0;
  }
}

void dt_exif_set_exiv2_taglist()
{
  if(exiv2_taglist) return;

  try
  {
    const Exiv2::GroupInfo *groupList = Exiv2::ExifTags::groupList();
    if(groupList)
    {
      while(groupList->tagList_)
      {
        const std::string groupName(groupList->groupName_);
        if(groupName.substr(0, 3) != "Sub" &&
            groupName != "Image2" &&
            groupName != "Image3" &&
            groupName != "Thumbnail"
            )
        {
          const Exiv2::TagInfo *tagInfo = groupList->tagList_();
          while(tagInfo->tag_ != 0xFFFF)
          {
            char *tag = dt_util_dstrcat(NULL, "Exif.%s.%s,%s", groupList->groupName_, tagInfo->name_, _get_exiv2_type(tagInfo->typeId_));
            exiv2_taglist = g_list_prepend(exiv2_taglist, tag);
            tagInfo++;
          }
        }
      groupList++;
      }
    }

    const Exiv2::DataSet *iptcEnvelopeList = Exiv2::IptcDataSets::envelopeRecordList();
    while(iptcEnvelopeList->number_ != 0xFFFF)
    {
      char *tag = dt_util_dstrcat(NULL, "Iptc.Envelope.%s,%s%s", iptcEnvelopeList->name_,
                                  _get_exiv2_type(iptcEnvelopeList->type_),
                                  iptcEnvelopeList->repeatable_ ? "-R" : "");
      exiv2_taglist = g_list_prepend(exiv2_taglist, tag);
      iptcEnvelopeList++;
    }

    const Exiv2::DataSet *iptcApplication2List = Exiv2::IptcDataSets::application2RecordList();
    while(iptcApplication2List->number_ != 0xFFFF)
    {
      char *tag = dt_util_dstrcat(NULL, "Iptc.Application2.%s,%s%s", iptcApplication2List->name_,
                                  _get_exiv2_type(iptcApplication2List->type_),
                                  iptcApplication2List->repeatable_ ? "-R" : "");
      exiv2_taglist = g_list_prepend(exiv2_taglist, tag);
      iptcApplication2List++;
    }

    _get_xmp_tags("dc", &exiv2_taglist);
    _get_xmp_tags("xmp", &exiv2_taglist);
    _get_xmp_tags("xmpRights", &exiv2_taglist);
    _get_xmp_tags("xmpMM", &exiv2_taglist);
    _get_xmp_tags("xmpBJ", &exiv2_taglist);
    _get_xmp_tags("xmpTPg", &exiv2_taglist);
    _get_xmp_tags("xmpDM", &exiv2_taglist);
    _get_xmp_tags("pdf", &exiv2_taglist);
    _get_xmp_tags("photoshop", &exiv2_taglist);
    _get_xmp_tags("crs", &exiv2_taglist);
    _get_xmp_tags("tiff", &exiv2_taglist);
    _get_xmp_tags("exif", &exiv2_taglist);
    _get_xmp_tags("exifEX", &exiv2_taglist);
    _get_xmp_tags("aux", &exiv2_taglist);
    _get_xmp_tags("iptc", &exiv2_taglist);
    _get_xmp_tags("iptcExt", &exiv2_taglist);
    _get_xmp_tags("plus", &exiv2_taglist);
    _get_xmp_tags("mwg-rs", &exiv2_taglist);
    _get_xmp_tags("mwg-kw", &exiv2_taglist);
    _get_xmp_tags("dwc", &exiv2_taglist);
    _get_xmp_tags("dcterms", &exiv2_taglist);
    _get_xmp_tags("digiKam", &exiv2_taglist);
    _get_xmp_tags("kipi", &exiv2_taglist);
    _get_xmp_tags("GPano", &exiv2_taglist);
    _get_xmp_tags("lr", &exiv2_taglist);
    _get_xmp_tags("MP", &exiv2_taglist);
    _get_xmp_tags("MPRI", &exiv2_taglist);
    _get_xmp_tags("MPReg", &exiv2_taglist);
    _get_xmp_tags("acdsee", &exiv2_taglist);
    _get_xmp_tags("mediapro", &exiv2_taglist);
    _get_xmp_tags("expressionmedia", &exiv2_taglist);
    _get_xmp_tags("MicrosoftPhoto", &exiv2_taglist);
  }
  catch (Exiv2::AnyError& e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 taglist] " << s << std::endl;
  }
}

const GList* dt_exif_get_exiv2_taglist()
{
  if(!exiv2_taglist)
    dt_exif_set_exiv2_taglist();
  return exiv2_taglist;
}

static void _exif_import_tags(dt_image_t *img, Exiv2::XmpData::iterator &pos);

// inspired by ufraw_exiv2.cc:

static void dt_strlcpy_to_utf8(char *dest, size_t dest_max, Exiv2::ExifData::const_iterator &pos,
                               Exiv2::ExifData &exifData)
{
  std::string str = pos->print(&exifData);

  char *s = g_locale_to_utf8(str.c_str(), str.length(), NULL, NULL, NULL);
  if(!IS_NULL_PTR(s))
  {
    g_strlcpy(dest, s, dest_max);
    dt_free(s);
  }
  else
  {
    g_strlcpy(dest, str.c_str(), dest_max);
  }
}

void dt_remove_exif_keys(Exiv2::ExifData &exif, const char *keys[], unsigned int n_keys)
{
  for(unsigned int i = 0; i < n_keys; i++)
  {
    try
    {
      Exiv2::ExifData::iterator pos;
      while((pos = exif.findKey(Exiv2::ExifKey(keys[i]))) != exif.end())
        exif.erase(pos);
    }
    catch(const std::exception &e)
    {
      // the only exception we may get is "invalid" tag, which is not
      // important enough to either stop the function, or even display
      // a message (it's probably the tag is not implemented in the
      // exiv2 version used)
    }
  }
}

static bool dt_exif_read_xmp_tag(Exiv2::XmpData &xmpData, Exiv2::XmpData::iterator *pos, string key)
{
  try
  {
    return (*pos = xmpData.findKey(Exiv2::XmpKey(key))) != xmpData.end() && (*pos)->size();
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 read_xmp_tag] " << s << std::endl;
    return false;
  }
}
#define FIND_XMP_TAG(key) dt_exif_read_xmp_tag(xmpData, &pos, key)


// FIXME: according to http://www.exiv2.org/doc/classExiv2_1_1Metadatum.html#63c2b87249ba96679c29e01218169124
// there is no need to pass xmpData
// version = -1 -> version ignored
bool dt_exif_decode_xmp_data(dt_image_t *img, Exiv2::XmpData &xmpData, int version,
                                  bool exif_read)
{
  // as this can be called several times during the image lifetime, clean up first
  GList *imgs = NULL;
  imgs = g_list_prepend(imgs, GINT_TO_POINTER(img->id));
  try
  {
    Exiv2::XmpData::iterator pos;

    // older darktable version did not write this data correctly:
    // the reasoning behind strdup'ing all the strings before passing it to sqlite3 is, that
    // they are somehow corrupt after the call to sqlite3_prepare_v2() -- don't ask me
    // why for they don't get passed to that function.
    if(version == -1 || version > 0)
    {
      if(!exif_read) dt_metadata_clear(imgs, FALSE);
      for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
      {
        const gchar *key = dt_metadata_get_key(i);
        if(FIND_XMP_TAG(key))
        {
          char *value = strdup(pos->toString().c_str());
          char *adr = value;
          // skip any lang="" or charset=xxx
          while(!strncmp(value, "lang=", 5) || !strncmp(value, "charset=", 8))
          {
            while(*value != ' ' && *value) value++;
            while(*value == ' ') value++;
          }
          dt_metadata_set_import(img->id, key, value);
          dt_free(adr);
        }
      }
    }

    if(FIND_XMP_TAG("Xmp.xmp.Rating"))
    {
      const int stars = pos->toLong();
      dt_image_set_xmp_rating(img, stars);
    }
    else
      dt_image_set_xmp_rating(img, -2);

    if(!exif_read) dt_colorlabels_remove_labels(img->id);
    if(FIND_XMP_TAG("Xmp.xmp.Label"))
    {
      std::string label = pos->toString();
      if(label == "Red") // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 0);
      else if(label == "Yellow") // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 1);
      else if(label == "Green")
        dt_colorlabels_set_label(img->id, 2);
      else if(label == "Blue") // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 3);
      else if(label == "Purple") // Is it really called like that in XMP files?
        dt_colorlabels_set_label(img->id, 4);
    }
    // if Xmp.xmp.Label not managed from an external app use dt colors
    else if(FIND_XMP_TAG("Xmp.darktable.colorlabels"))
    {
      // color labels
      const int cnt = pos->count();
      for(int i = 0; i < cnt; i++)
      {
        dt_colorlabels_set_label(img->id, pos->toLong(i));
      }
    }

    if((dt_image_get_xmp_mode()) ||
       dt_conf_get_bool("ui_last/import_last_tags_imported"))
    {
      GList *tags = NULL;
      // preserve dt tags which are not saved in xmp file
      if(!exif_read) dt_tag_set_tags(tags, imgs, TRUE, TRUE, FALSE);
      if(FIND_XMP_TAG("Xmp.lr.hierarchicalSubject"))
        _exif_import_tags(img, pos);
      else if(FIND_XMP_TAG("Xmp.dc.subject"))
        _exif_import_tags(img, pos);
    }

    /* read gps location */
    if(FIND_XMP_TAG("Xmp.exif.GPSLatitude"))
    {
      img->geoloc.latitude = dt_util_gps_string_to_number(pos->toString().c_str());
    }

    if(FIND_XMP_TAG("Xmp.exif.GPSLongitude"))
    {
      img->geoloc.longitude = dt_util_gps_string_to_number(pos->toString().c_str());
    }

    if(FIND_XMP_TAG("Xmp.exif.GPSAltitude"))
    {
      Exiv2::XmpData::const_iterator ref = xmpData.findKey(Exiv2::XmpKey("Xmp.exif.GPSAltitudeRef"));
      if(ref != xmpData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double elevation = 0.0;
        if(dt_util_gps_elevation_to_number(pos->toRational(0).first, pos->toRational(0).second, sign[0], &elevation))
          img->geoloc.elevation = elevation;
      }
    }

    /* read lens type from Xmp.exifEX.LensModel */
    if(FIND_XMP_TAG("Xmp.exifEX.LensModel"))
    {
      // lens model
      char *lens = strdup(pos->toString().c_str());
      char *adr =  lens;
      if(strncmp(lens, "lang=", 5) == 0)
      {
        lens = strchr(lens, ' ');
        if(!IS_NULL_PTR(lens)) lens++;
      }
      // no need to do any Unicode<->locale conversion, the field is specified as ASCII
      g_strlcpy(img->exif_lens, lens, sizeof(img->exif_lens));
      dt_free(adr);
    }

    /* read timestamp from Xmp.exif.DateTimeOriginal */
    if(FIND_XMP_TAG("Xmp.exif.DateTimeOriginal")
       || FIND_XMP_TAG("Xmp.photoshop.DateCreated"))
    {
      char *datetime = strdup(pos->toString().c_str());
      if(datetime[0] != '\0') dt_datetime_exif_to_img(img, datetime);
      dt_free(datetime);
    }

    if(imgs)
    {
      g_list_free(imgs);
      imgs = NULL;
    }
    imgs = NULL;
    return true;
  }
  catch(const std::exception &e)
  {
    if(imgs)
    {
      g_list_free(imgs);
      imgs = NULL;
    }
    imgs = NULL;
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_decode_xmp_data] " << img->filename << ": " << s << std::endl;
    return false;
  }
}

static bool dt_exif_read_iptc_tag(Exiv2::IptcData &iptcData, Exiv2::IptcData::const_iterator *pos, string key)
{
  try
  {
    return (*pos = iptcData.findKey(Exiv2::IptcKey(key))) != iptcData.end() && (*pos)->size();
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 read_iptc_tag] " << s << std::endl;
    return false;
  }
}
#define FIND_IPTC_TAG(key) dt_exif_read_iptc_tag(iptcData, &pos, key)


// FIXME: according to http://www.exiv2.org/doc/classExiv2_1_1Metadatum.html#63c2b87249ba96679c29e01218169124
// there is no need to pass iptcData
static bool _exif_decode_iptc_data(dt_image_t *img, Exiv2::IptcData &iptcData)
{
  try
  {
    Exiv2::IptcData::const_iterator pos;
    iptcData.sortByKey(); // this helps to quickly find all Iptc.Application2.Keywords

    if((pos = iptcData.findKey(Exiv2::IptcKey("Iptc.Application2.Keywords"))) != iptcData.end())
    {
      while(pos != iptcData.end())
      {
        std::string key = pos->key();
        if(g_strcmp0(key.c_str(), "Iptc.Application2.Keywords")) break;
        std::string str = pos->print();
        char *tag = dt_util_foo_to_utf8(str.c_str());
        guint tagid = 0;
        dt_tag_new(tag, &tagid);
        dt_tag_attach(tagid, img->id, FALSE, FALSE);
        dt_free(tag);
        ++pos;
      }
      dt_metadata_tags_changed();
    }
    if(FIND_IPTC_TAG("Iptc.Application2.Caption"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.description", str.c_str());
    }
    if(FIND_IPTC_TAG("Iptc.Application2.Copyright"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.rights", str.c_str());
    }
    if(FIND_IPTC_TAG("Iptc.Application2.Byline"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if(FIND_IPTC_TAG("Iptc.Application2.Writer"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if(FIND_IPTC_TAG("Iptc.Application2.Contact"))
    {
      std::string str = pos->print(/*&iptcData*/);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }
    if(FIND_IPTC_TAG("Iptc.Application2.DateCreated"))
    {
      // exiv2 already converts IPTC date and time into ISO 8601 format
      GString *datetime = g_string_new(pos->toString().c_str());

      datetime = g_string_append(datetime, "T");
      if(FIND_IPTC_TAG("Iptc.Application2.TimeCreated"))
      {
        gchar *time = g_strdup(pos->toString().c_str());
        datetime = g_string_append(datetime, time);
        dt_free(time);
      }
      else
        datetime = g_string_append(datetime, "00:00:00");

      if(datetime->str[0] != '\0') dt_datetime_exif_to_img(img, datetime->str);
      g_string_free(datetime, TRUE);
    }

    return true;
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 _exif_decode_iptc_data] " << img->filename << ": " << s << std::endl;
    return false;
  }
}

bool dt_exif_read_exif_tag(Exiv2::ExifData &exifData,
                                Exiv2::ExifData::const_iterator *pos, string key)
{
  try
  {
    return (*pos = exifData.findKey(Exiv2::ExifKey(key)))
      != exifData.end() && (*pos)->size();
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 read_exif_tag] " << s << std::endl;
    return false;
  }
}
#define FIND_EXIF_TAG(key) dt_exif_read_exif_tag(exifData, &pos, key)

// Two candidate copies of DefaultUserCrop are compared edge by edge with this absolute
// tolerance: the same rectangle can be encoded as different rationals in different IFDs, so
// bitwise equality would report a conflict where none exists.
#define DT_IMAGE_USERCROP_EPSILON 1e-6f

/** Read one DefaultUserCrop candidate and classify it.
 *
 * The DNG spec (tag 51125 / 0xC7B5) defines four unsigned rationals in (top, left, bottom,
 * right) order, normalized against the DefaultCropOrigin/DefaultCropSize rectangle, with
 * (0, 0, 1, 1) as the identity. exiv2 only knows the tag by name from 0.27.4, hence the
 * lookup by tag number.
 *
 * Validation is the spec's own contract and nothing else. In particular there is deliberately
 * no minimum-size heuristic: the inherited "each side must span more than 5%" test rejected
 * standards-valid narrow crops while still accepting out-of-range ones, so it was never tag
 * validation. Malformed values are reported as such, never clamped into a plausible rectangle.
 */
static dt_image_usercrop_status_t _read_usercrop_candidate(Exiv2::ExifData &exifData, const char *key,
                                                           dt_boundingbox_t crop)
{
  crop[0] = crop[1] = 0.f;
  crop[2] = crop[3] = 1.f;

  Exiv2::ExifData::const_iterator pos = exifData.findKey(Exiv2::ExifKey(key));
  if(pos == exifData.end()) return DT_IMAGE_USERCROP_ABSENT;

  if(pos->count() != 4 || !pos->size()) return DT_IMAGE_USERCROP_MALFORMED;

  for(int i = 0; i < 4; i++)
  {
    crop[i] = pos->toFloat(i);
    if(!std::isfinite(crop[i])) return DT_IMAGE_USERCROP_MALFORMED;
  }

  // 0 <= top < bottom <= 1 and 0 <= left < right <= 1
  if(!(crop[0] >= 0.f && crop[0] < crop[2] && crop[2] <= 1.f)) return DT_IMAGE_USERCROP_MALFORMED;
  if(!(crop[1] >= 0.f && crop[1] < crop[3] && crop[3] <= 1.f)) return DT_IMAGE_USERCROP_MALFORMED;

  if(crop[0] == 0.f && crop[1] == 0.f && crop[2] == 1.f && crop[3] == 1.f)
    return DT_IMAGE_USERCROP_IDENTITY;

  return DT_IMAGE_USERCROP_VALID;
}

static bool _usercrop_candidates_agree(const dt_boundingbox_t a, const dt_boundingbox_t b)
{
  for(int i = 0; i < 4; i++)
    if(fabsf(a[i] - b[i]) > DT_IMAGE_USERCROP_EPSILON) return false;

  return true;
}

/** Resolve img->usercrop / img->usercrop_status from the file's DefaultUserCrop candidates.
 *
 * Returns TRUE only for a valid, non-identity framing — the caller uses that to decide whether
 * to advertise the file as carrying usable crop metadata. Every other outcome leaves the box at
 * identity, so no failure mode can produce an automatic crop.
 */
static bool _check_usercrop(Exiv2::ExifData &exifData, dt_image_t *img)
{
  // Exif.SubImage1 holds the raw IFD of a DNG with an embedded preview; DNGs without one carry
  // the raw image tags under Exif.Image instead. Both are documented compatibility candidates,
  // neither is proof of which IFD the decoder actually selected -- so when both are present and
  // disagree, refuse rather than silently pick one.
  dt_boundingbox_t sub_image_crop, image_crop;
  const dt_image_usercrop_status_t sub_image_status
      = _read_usercrop_candidate(exifData, "Exif.SubImage1.0xc7b5", sub_image_crop);
  const dt_image_usercrop_status_t image_status
      = _read_usercrop_candidate(exifData, "Exif.Image.0xc7b5", image_crop);

  dt_image_usercrop_status_t status;
  const float *crop;

  if(sub_image_status == DT_IMAGE_USERCROP_VALID && image_status == DT_IMAGE_USERCROP_VALID
     && !_usercrop_candidates_agree(sub_image_crop, image_crop))
  {
    status = DT_IMAGE_USERCROP_CONFLICT;
    crop = NULL;
  }
  else if(sub_image_status != DT_IMAGE_USERCROP_ABSENT)
  {
    status = sub_image_status;
    crop = sub_image_crop;
  }
  else
  {
    status = image_status;
    crop = image_crop;
  }

  img->usercrop_status = status;
  img->usercrop[0] = img->usercrop[1] = 0.f;
  img->usercrop[2] = img->usercrop[3] = 1.f;

  if(status == DT_IMAGE_USERCROP_VALID)
  {
    for(int i = 0; i < 4; i++) img->usercrop[i] = crop[i];
    dt_print(DT_DEBUG_IMAGEIO,
             "[exif] image %d: DNG DefaultUserCrop (top, left, bottom, right) = (%f, %f, %f, %f)\n",
             img->id, crop[0], crop[1], crop[2], crop[3]);
    return TRUE;
  }

  // ABSENT and IDENTITY are the normal case for most files: stay silent about them.
  if(status == DT_IMAGE_USERCROP_MALFORMED || status == DT_IMAGE_USERCROP_CONFLICT)
    dt_print(DT_DEBUG_ALWAYS,
             "[exif] image %d: ignoring DNG DefaultUserCrop, the camera framing metadata is %s\n",
             img->id, (status == DT_IMAGE_USERCROP_CONFLICT) ? "contradictory between IFDs" : "malformed");

  return FALSE;
}

static gboolean _check_dng_opcodes(Exiv2::ExifData &exifData, dt_image_t *img)
{
  gboolean has_opcodes = FALSE;
  Exiv2::ExifData::const_iterator pos = exifData.findKey(Exiv2::ExifKey("Exif.SubImage1.OpcodeList2"));
  // DNGs without an embedded preview have the opcodes under Exif.Image instead of Exif.SubImage1
  if(pos == exifData.end())
    pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.OpcodeList2"));
  if(pos != exifData.end())
  {
    uint8_t *data = (uint8_t *)g_malloc(pos->size());
    pos->copy(data, Exiv2::invalidByteOrder);
    dt_dng_opcode_process_opcode_list_2(data, pos->size(), img);
    dt_free(data);
    has_opcodes = TRUE;
  }
  else
  {
    dt_vprint(DT_DEBUG_IMAGEIO, "DNG OpcodeList2 tag not found\n");
  }
  return has_opcodes;
}

void dt_exif_read_usercrop(dt_image_t *img, const char *filename)
{
  // Leave a definite answer even when the file cannot be read, so callers that use
  // DT_IMAGE_USERCROP_UNKNOWN as a "not looked at yet" marker do not retry on every request.
  img->usercrop_status = DT_IMAGE_USERCROP_ABSENT;
  img->usercrop[0] = img->usercrop[1] = 0.f;
  img->usercrop[2] = img->usercrop[3] = 1.f;

  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(filename)));
    if(!image.get()) return;
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();
    if(exifData.empty()) return;
    _check_usercrop(exifData, img);
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_read_usercrop] " << filename << ": " << s << std::endl;
  }
}

void dt_exif_img_check_additional_tags(dt_image_t *img, const char *filename)
{
  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(filename)));
    if(!image.get()) return;
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();
    if(!exifData.empty())
    {
      _check_usercrop(exifData, img);
      _check_dng_opcodes(exifData, img);
      // _check_lens_correction_data(exifData, img);
    }
    return;
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 reading DefaultUserCrop] " << filename << ": " << s << std::endl;
    return;
  }
}

static void _find_datetime_taken(Exiv2::ExifData &exifData,
                                 Exiv2::ExifData::const_iterator pos,
                                 char *exif_datetime_taken)
{
  if((FIND_EXIF_TAG("Exif.Image.DateTimeOriginal") || FIND_EXIF_TAG("Exif.Photo.DateTimeOriginal"))
     && pos->size() == DT_DATETIME_EXIF_LENGTH)
  {
    dt_strlcpy_to_utf8(exif_datetime_taken, DT_DATETIME_EXIF_LENGTH, pos, exifData);
    if(FIND_EXIF_TAG("Exif.Photo.SubSecTimeOriginal")
       && pos->size() > 1)
    {
      char msec[4];
      dt_strlcpy_to_utf8(msec, sizeof(msec), pos, exifData);
      dt_datetime_add_subsec_to_exif(exif_datetime_taken, DT_DATETIME_LENGTH, msec);
    }
  }
  else
  {
    *exif_datetime_taken = '\0';
  }
}

static void _find_exif_maker(Exiv2::ExifData &exifData,
                             Exiv2::ExifData::const_iterator pos,
                             char *maker,
                             const size_t m_size)
{
    // look for maker & model first so we can use that info later
    if(FIND_EXIF_TAG("Exif.Image.Make"))
    {
    dt_strlcpy_to_utf8(maker, m_size, pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Make"))
    {
    dt_strlcpy_to_utf8(maker, m_size, pos, exifData);
    }

  for(char *c = maker + m_size - 1; c > maker; c--)
      if(*c != ' ' && *c != '\0')
      {
        *(c + 1) = '\0';
        break;
      }
}

static void _find_exif_model(Exiv2::ExifData &exifData,
                             Exiv2::ExifData::const_iterator pos,
                             char *model,
                             const size_t m_size)
{
    if(FIND_EXIF_TAG("Exif.Image.Model"))
    {
    dt_strlcpy_to_utf8(model, m_size, pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Model"))
    {
    dt_strlcpy_to_utf8(model, m_size, pos, exifData);
    }

  for(char *c = model + m_size - 1; c > model; c--)
      if(*c != ' ' && *c != '\0')
      {
        *(c + 1) = '\0';
        break;
      }
}

static bool _exif_decode_exif_data(dt_image_t *img, Exiv2::ExifData &exifData)
{
  try
  {
    /* List of tag names taken from exiv2's printSummary() in actions.cpp */
    Exiv2::ExifData::const_iterator pos;

    _find_exif_maker(exifData, pos, img->exif_maker, sizeof(img->exif_maker));
    _find_exif_model(exifData, pos, img->exif_model, sizeof(img->exif_model));

    // Make sure we copy the exif make and model to the correct place if needed
    dt_image_refresh_makermodel(img);

    /* Read shutter time */
    if((pos = Exiv2::exposureTime(exifData)) != exifData.end() && pos->size())
    {
      img->exif_exposure = pos->toFloat();
    }
    else if(FIND_EXIF_TAG("Exif.Photo.ShutterSpeedValue") || FIND_EXIF_TAG("Exif.Image.ShutterSpeedValue"))
    {
      // uf_strlcpy_to_utf8(uf->conf->shutterText, max_name, pos, exifData);
      img->exif_exposure = exp2f(-1.0f * pos->toFloat());  // convert from APEX value
    }

    // Read exposure bias
    if(FIND_EXIF_TAG("Exif.Photo.ExposureBiasValue") || FIND_EXIF_TAG("Exif.Image.ExposureBiasValue"))
    {
      img->exif_exposure_bias = pos->toFloat();
    }

    /* Read aperture */
    if((pos = Exiv2::fNumber(exifData)) != exifData.end() && pos->size())
    {
      img->exif_aperture = pos->toFloat();
    }
    else if(FIND_EXIF_TAG("Exif.Photo.ApertureValue") || FIND_EXIF_TAG("Exif.Image.ApertureValue"))
    {
      img->exif_aperture = exp2f(pos->toFloat() / 2.0f);  // convert from APEX value
    }

    /* Read ISO speed - Nikon happens to return a pair for Lo and Hi modes */
    if((pos = Exiv2::isoSpeed(exifData)) != exifData.end() && pos->size())
    {
      // if standard exif iso tag, use the old way of interpreting the return value to be more regression-save
      if(strcmp(pos->key().c_str(), "Exif.Photo.ISOSpeedRatings") == 0)
      {
        int isofield = pos->count() > 1 ? 1 : 0;
        img->exif_iso = pos->toFloat(isofield);
      }
      else
      {
        std::string str = pos->print();
        img->exif_iso = (float)std::atof(str.c_str());
      }
    }
    // some newer cameras support iso settings that exceed the 16 bit of exif's ISOSpeedRatings
    if(img->exif_iso == 65535 || img->exif_iso == 0)
    {
      if(FIND_EXIF_TAG("Exif.PentaxDng.ISO") || FIND_EXIF_TAG("Exif.Pentax.ISO"))
      {
        std::string str = pos->print();
        img->exif_iso = (float)std::atof(str.c_str());
      }
      else if((!g_strcmp0(img->exif_maker, "SONY") || !g_strcmp0(img->exif_maker, "Canon"))
        && FIND_EXIF_TAG("Exif.Photo.RecommendedExposureIndex"))
      {
        img->exif_iso = pos->toFloat();
      }
    }

    /* Read focal length  */
    if((pos = Exiv2::focalLength(exifData)) != exifData.end() && pos->size())
    {
      // This works around a bug in exiv2 the developers refuse to fix
      // For details see http://dev.exiv2.org/issues/1083
      if (pos->key() == "Exif.Canon.FocalLength" && pos->count() == 4)
        img->exif_focal_length = pos->toFloat(1);
      else
        img->exif_focal_length = pos->toFloat();
    }

    // Read focal length in 35mm if available and try to calculate crop factor.
    if(FIND_EXIF_TAG("Exif.Photo.FocalLengthIn35mmFilm"))
    {
      const float focal_length_35mm = pos->toFloat();
      if(focal_length_35mm > 0.0f && img->exif_focal_length > 0.0f)
        img->exif_crop = focal_length_35mm / img->exif_focal_length;
      else
        img->exif_crop = 0.0f;
    }

    // If the tag for the equivalent focal length is missing or contains zero,
    // let's try to get the crop factor by calculating the diagonal of the sensor:
    if(img->exif_crop == 0.0f && FIND_EXIF_TAG("Exif.Photo.FocalPlaneXResolution"))
    {
      float x_resolution = pos->toFloat();
      float y_resolution = 0.0f;
      if(FIND_EXIF_TAG("Exif.Photo.FocalPlaneYResolution"))
        y_resolution = pos->toFloat();
      guint res_unit = 1;
      if(FIND_EXIF_TAG("Exif.Photo.FocalPlaneResolutionUnit"))
        res_unit = pos->toLong();
      if(res_unit == 2) // inch
      {
        x_resolution /= 25.4f;
        y_resolution /= 25.4f;
      }
      else
      if(res_unit == 3) // centimeter
      {
        x_resolution /= 10.0f;
        y_resolution /= 10.0f;
      }
      guint image_width = 0;
      guint image_height = 0;
      // We are entering the zoo of image dimensions metadata.
      // Let's first try the Exif way of telling dimensions.
      // For Canon and Sigma cameras, these are the valid raw image
      // dimensions matching the the resolution tags.
      // For Fujifilm cameras, this will get the pixel dimensions
      // of the preview, not the sensor, because that's what the data
      // in the resolution tags on most Fujifilm cameras seems to be
      // calculated for.
      if(FIND_EXIF_TAG("Exif.Photo.PixelXDimension"))
        image_width = pos->toLong();
      if(FIND_EXIF_TAG("Exif.Photo.PixelYDimension"))
        image_height = pos->toLong();
      // Then try the Adobe DNG way of telling dimensions.
      // Exif.Image.ImageWidth/Length tags are also present in DNG files,
      // but may contain the pixel dimensions of the preview image, which in
      // this case will cause the diagonal calculation to be incorrect.
      if(image_width == 0 && FIND_EXIF_TAG("Exif.SubImage1.NewSubfileType"))
      {
        if(pos->toLong() == 0)  // Primary image
        {
          if(FIND_EXIF_TAG("Exif.SubImage1.ImageWidth"))
            image_width = pos->toLong();
          if(FIND_EXIF_TAG("Exif.SubImage1.ImageLength"))
            image_height = pos->toLong();
        }
      }
      // The following tags in certain formats may contain pixel dimensions of
      // the preview instead of the full image, while resolution is calculated
      // relative to the full image dimensions. So we check them last.
      if(image_width == 0)
      {
        if(FIND_EXIF_TAG("Exif.Image.ImageWidth"))
          image_width = pos->toLong();
        if(FIND_EXIF_TAG("Exif.Image.ImageLength"))
          image_height = pos->toLong();
      }

      const float x_size_mm = (float)image_width / x_resolution;
      const float y_size_mm = (float)image_height / y_resolution;
      if(image_width && image_height) // We've got the data and can calculate the crop factor
      {
        const float sensor_diagonal = dt_fast_hypotf(x_size_mm, y_size_mm);
        const float fullframe_diagonal = dt_fast_hypotf(36.0f, 24.0f);
        img->exif_crop = fullframe_diagonal / sensor_diagonal;
      }
      else
        img->exif_crop = 0.0f; // Will be shown as "no data" in the image information module
    }

    if(_check_usercrop(exifData, img))
    {
      img->flags |= DT_IMAGE_HAS_ADDITIONAL_DNG_TAGS;
        guint tagid = 0;
        char tagname[64];
        snprintf(tagname, sizeof(tagname), "darktable|mode|exif-crop");
        dt_tag_new(tagname, &tagid);
        dt_tag_attach(tagid, img->id, FALSE, FALSE);
    }

    if(_check_dng_opcodes(exifData, img))
    {
      img->flags |= DT_IMAGE_HAS_ADDITIONAL_DNG_TAGS;
    }

    /*
     * Get the focus distance in meters.
     */
    if(Exiv2::testVersion(0, 27, 4) && FIND_EXIF_TAG("Exif.NikonLd4.LensID") && pos->toLong() != 0)
    {
      // Z lens, need to specifically look for the second instance of Exif.NikonLd4.FocusDistance
      // unless using Exiv2 0.28.x and later (also expanded to 2 bytes of precision since 0.28.1).
#if EXIV2_TEST_VERSION(0, 28, 0)
      if(FIND_EXIF_TAG("Exif.NikonLd4.FocusDistance2"))
      {
        float value = pos->toFloat();
        if(Exiv2::testVersion(0, 28, 1)) value /= 256.0f;
#else
      pos = exifData.end();
      for(auto it = exifData.begin(); it != exifData.end(); it++)
      {
        if(it->key() == "Exif.NikonLd4.FocusDistance") pos = it;
      }
      if(pos != exifData.end() && pos->size())
    {
      float value = pos->toFloat();
#endif
        img->exif_focus_distance = 0.01f * pow(10.0f, value / 40.0f);
    }
    }
    else if(FIND_EXIF_TAG("Exif.NikonLd2.FocusDistance") || FIND_EXIF_TAG("Exif.NikonLd3.FocusDistance")
            || (Exiv2::testVersion(0, 27, 4) && FIND_EXIF_TAG("Exif.NikonLd4.FocusDistance")))
    {
      float value = pos->toFloat();
      img->exif_focus_distance = 0.01f * pow(10.0f, value / 40.0f);
    }
    else if(FIND_EXIF_TAG("Exif.OlympusFi.FocusDistance"))
    {
      /* the distance is stored as a rational (fraction). according to
       * http://www.dpreview.com/forums/thread/1173960?page=4
       * some Olympus cameras have a wrong denominator of 10 in there while the nominator is always in mm.
       * thus we ignore the denominator
       * and divide with 1000.
       * "I've checked a number of E-1 and E-300 images, and I agree that the FocusDistance looks like it is
       * in mm for the E-1. However,
       * it looks more like cm for the E-300.
       * For both cameras, this value is stored as a rational. With the E-1, the denominator is always 1,
       * while for the E-300 it is 10.
       * Therefore, it looks like the numerator in both cases is in mm (which makes a bit of sense, in an odd
       * sort of way). So I think
       * what I will do in ExifTool is to take the numerator and divide by 1000 to display the focus distance
       * in meters."
       *   -- Boardhead, dpreview forums in 2005
       */
      int nominator = pos->toRational(0).first;
      img->exif_focus_distance = fmax(0.0, (0.001 * nominator));
    }
    else if(FIND_EXIF_TAG("Exif.CanonFi.FocusDistanceUpper"))
    {
      const float FocusDistanceUpper = pos->toFloat();
      if(FocusDistanceUpper <= 0.0f || (int)FocusDistanceUpper >= 0xffff)
      {
        img->exif_focus_distance = 0.0f;
      }
      else
      {
        img->exif_focus_distance = FocusDistanceUpper / 100.0;
        if(FIND_EXIF_TAG("Exif.CanonFi.FocusDistanceLower"))
        {
          const float FocusDistanceLower = pos->toFloat();
          if(FocusDistanceLower > 0.0f && (int)FocusDistanceLower < 0xffff)
          {
            img->exif_focus_distance += FocusDistanceLower / 100.0;
            img->exif_focus_distance /= 2.0;
          }
        }
      }
    }
    else if(FIND_EXIF_TAG("Exif.CanonSi.SubjectDistance"))
    {
      img->exif_focus_distance = pos->toFloat() / 100.0;
    }
    else if((pos = Exiv2::subjectDistance(exifData)) != exifData.end() && pos->size())
    {
      img->exif_focus_distance = pos->toFloat();
    }
    else if(Exiv2::testVersion(0,27,2) && FIND_EXIF_TAG("Exif.Sony2Fp.FocusPosition2"))
    {
      const float focus_position = pos->toFloat();

      if (focus_position && FIND_EXIF_TAG("Exif.Photo.FocalLengthIn35mmFilm")) {
        const float focal_length_35mm = pos->toFloat();

        /* http://u88.n24.queensu.ca/exiftool/forum/index.php/topic,3688.msg29653.html#msg29653 */
        img->exif_focus_distance = (pow(2, focus_position / 16 - 5) + 1) * focal_length_35mm / 1000;
      }
    }

    /*
     * Read image orientation
     */
    if(FIND_EXIF_TAG("Exif.Image.Orientation"))
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }
    else if(FIND_EXIF_TAG("Exif.PanasonicRaw.Orientation"))
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }
    /* for e.g. Sinar backs the raw orientation is in a different subdirectory */
    if(FIND_EXIF_TAG("Exif.Thumbnail.PhotometricInterpretation") && (32803 == pos->toLong())
       && FIND_EXIF_TAG("Exif.Thumbnail.Orientation"))
    {
      img->orientation = dt_image_orientation_to_flip_bits(pos->toLong());
    }

    /* read gps location */
    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSLatitude"))
    {
      Exiv2::ExifData::const_iterator ref = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLatitudeRef"));
      if(ref != exifData.end() && ref->size() && pos->count() == 3)
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double latitude = 0.0;
        if(dt_util_gps_rationale_to_number(pos->toRational(0).first, pos->toRational(0).second,
                                           pos->toRational(1).first, pos->toRational(1).second,
                                           pos->toRational(2).first, pos->toRational(2).second, sign[0], &latitude))
          img->geoloc.latitude = latitude;
      }
    }

    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSLongitude"))
    {
      Exiv2::ExifData::const_iterator ref = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSLongitudeRef"));
      if(ref != exifData.end() && ref->size() && pos->count() == 3)
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double longitude = 0.0;
        if(dt_util_gps_rationale_to_number(pos->toRational(0).first, pos->toRational(0).second,
                                           pos->toRational(1).first, pos->toRational(1).second,
                                           pos->toRational(2).first, pos->toRational(2).second, sign[0], &longitude))
          img->geoloc.longitude = longitude;
      }
    }

    if(FIND_EXIF_TAG("Exif.GPSInfo.GPSAltitude"))
    {
      Exiv2::ExifData::const_iterator ref = exifData.findKey(Exiv2::ExifKey("Exif.GPSInfo.GPSAltitudeRef"));
      if(ref != exifData.end() && ref->size())
      {
        std::string sign_str = ref->toString();
        const char *sign = sign_str.c_str();
        double elevation = 0.0;
        if(dt_util_gps_elevation_to_number(pos->toRational(0).first, pos->toRational(0).second, sign[0], &elevation))
          img->geoloc.elevation = elevation;
      }
    }

    /* Read lens name */
    if((FIND_EXIF_TAG("Exif.CanonCs.LensType")
        && pos->toLong() != 61182   // prefer the other tag for RF lenses
        && pos->toLong() != 0
        && pos->toLong() != 65535)
       || FIND_EXIF_TAG("Exif.Canon.LensModel"))
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.PentaxDng.LensType"))
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.Panasonic.LensType"))
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if(FIND_EXIF_TAG("Exif.OlympusEq.LensType"))
    {
      /* For every Olympus camera Exif.OlympusEq.LensType is present. */
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);

      /* We have to check if Exif.OlympusEq.LensType has been translated by
       * exiv2. If it hasn't, fall back to Exif.OlympusEq.LensModel. */
      std::string lens(img->exif_lens);
      if(std::string::npos == lens.find_first_not_of(" 1234567890"))
      {
        /* Exif.OlympusEq.LensType contains only digits and spaces.
         * This means that exiv2 couldn't convert it to human readable
         * form. */
        if(FIND_EXIF_TAG("Exif.OlympusEq.LensModel"))
        {
          dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
        }
        /* Just in case Exif.OlympusEq.LensModel hasn't been found */
        else if(FIND_EXIF_TAG("Exif.Photo.LensModel"))
        {
          dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
        }
        fprintf(stderr, "[exif] Warning: lens \"%s\" unknown as \"%s\"\n", img->exif_lens, lens.c_str());
      }
    }
    else if(Exiv2::testVersion(0,27,4) && FIND_EXIF_TAG("Exif.NikonLd4.LensID") && pos->toLong() == 0)
    {
      /* Z body w/ FTZ adapter or recent F body (e.g. D780, D6) detected.
       * Prioritize the legacy ID lookup instead of Exif.Photo.LensModel included
       * in the default Exiv2::lensName() search below. */
      if(FIND_EXIF_TAG("Exif.NikonLd4.LensIDNumber"))
        dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    else if((pos = Exiv2::lensName(exifData)) != exifData.end() && pos->size())
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }

    /* Use pretty name for Canon RF & RF-S lenses (as exiftool/exiv2/lensfun) */
    if(g_str_has_prefix(img->exif_lens, "RF"))
    {
      char *pretty;
      if(img->exif_lens[2] == '-')
        pretty = g_strconcat("Canon RF-S ", &img->exif_lens[4], (char *)NULL);
      else
        pretty = g_strconcat("Canon RF ", &img->exif_lens[2], (char *)NULL);
      g_strlcpy(img->exif_lens, pretty, sizeof(img->exif_lens));
      dt_free(pretty);
    }

    /* Capitalize Nikon Z-mount lenses properly for UI presentation */
    if(g_str_has_prefix(img->exif_lens, "NIKKOR") || g_str_has_prefix(img->exif_lens, "TAMRON"))
    {
      for(size_t i = 1; i <= 5; ++i)
        img->exif_lens[i] = g_ascii_tolower(img->exif_lens[i]);
    }

    // finally the lens has only numbers and parentheses, let's try to use
    // Exif.Photo.LensModel if defined.

    std::string lens(img->exif_lens);
    if(std::string::npos == lens.find_first_not_of(" (1234567890)")
       && FIND_EXIF_TAG("Exif.Photo.LensModel"))
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }

#if 0
    /* Read flash mode */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.Flash")))
         != exifData.end() && pos->size())
    {
      uf_strlcpy_to_utf8(uf->conf->flashText, max_name, pos, exifData);
    }
    /* Read White Balance Setting */
    if ( (pos=exifData.findKey(Exiv2::ExifKey("Exif.Photo.WhiteBalance")))
         != exifData.end() && pos->size())
    {
      uf_strlcpy_to_utf8(uf->conf->whiteBalanceText, max_name, pos, exifData);
    }
#endif

    char datetime[DT_DATETIME_LENGTH];
    _find_datetime_taken(exifData, pos, datetime);
    if(datetime[0] != '\0') dt_datetime_exif_to_img(img, datetime);

    if(FIND_EXIF_TAG("Exif.Image.Artist"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }
    else if(FIND_EXIF_TAG("Exif.Canon.OwnerName"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set_import(img->id, "Xmp.dc.creator", str.c_str());
    }

    // FIXME: Should the UserComment go into the description? Or do we need an extra field for this?
    if(FIND_EXIF_TAG("Exif.Photo.UserComment"))
    {
      std::string str = pos->print(&exifData);
      Exiv2::CommentValue value(str);
      std::string str2 = value.comment();
      if(str2 != "binary comment")
        dt_metadata_set_import(img->id, "Xmp.dc.description", str2.c_str());
    }
    else if(FIND_EXIF_TAG("Exif.Image.ImageDescription"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set_import(img->id, "Xmp.dc.description", str.c_str());
    }

    if(FIND_EXIF_TAG("Exif.Image.Copyright"))
    {
      std::string str = pos->print(&exifData);
      dt_metadata_set_import(img->id, "Xmp.dc.rights", str.c_str());
    }

    if(FIND_EXIF_TAG("Exif.Image.Rating"))
    {
      const int stars = pos->toLong();
      dt_image_set_xmp_rating(img, stars);
    }
    else if(FIND_EXIF_TAG("Exif.Image.RatingPercent"))
    {
      const int stars = pos->toLong() * 5. / 100;
      dt_image_set_xmp_rating(img, stars);
    }
    else
      dt_image_set_xmp_rating(img, -2);

    // read embedded color matrix as used in DNGs
    {
      float colmatrix[3][12];
      colmatrix[0][0] = colmatrix[1][0] = colmatrix[2][0] = NAN;
      dt_dng_illuminant_t illu[3] = { DT_LS_Unknown, DT_LS_Unknown, DT_LS_Unknown };
      img->d65_color_matrix[0] = NAN; // make sure for later testing

      // fallback later via `find_temperature_from_raw_coeffs` if there is no valid illuminant

      // The correction matrices are taken from
      // http://www.brucelindbloom.com - chromatic Adaption.
      // using Bradford method: found Illuminant -> D65
      const float correctmat[13][9] = {
        { 0.9555766, -0.0230393, 0.0631636, -0.0282895, 1.0099416, 0.0210077, 0.0122982, -0.0204830,
          1.3299098 }, // D50
        { 0.9726856, -0.0135482, 0.0361731, -0.0167463, 1.0049102, 0.0120598, 0.0070026, -0.0116372,
          1.1869548 }, // D55
        { 1.0206905, 0.0091588, -0.0228796, 0.0115005, 0.9984917, -0.0076762, -0.0043619, 0.0072053,
          0.8853432 }, // D75
        { 0.8446965, -0.1179225, 0.3948108, -0.1366303, 1.1041226, 0.1291718, 0.0798489, -0.1348999,
          3.1924009 }, // Standard light A
        { 0.9415037, -0.0321240, 0.0584672, -0.0428238, 1.0250998, 0.0203309, 0.0101511, -0.0161170,
          1.2847354 }, // Standard light B
        { 0.9904476, -0.0071683, -0.0116156, -0.0123712, 1.0155950, -0.0029282, -0.0035635, 0.0067697,
          0.9181569 }, //  Standard light C
        { 0.9212269, -0.0449128, 0.1211620, -0.0553723, 1.0277243, 0.0403563, 0.0235086, -0.0391019,
          1.6390644 }, // Fluorescent (F2)
        // The following are calculated using the same Bradford method,
        // with xy coord from DNG SDK as reference -> XYZ -> D65
        { 0.8663030, -0.0913083, 0.2771784, -0.1090504, 1.0746895, 0.0913841, 0.0550856, -0.0924636,
          2.5119387 }, // ISO Studio Tungsten (3200K first converted to xy as DNG SDK does)
        { 1.0096114, 0.0061501, 0.0068113, 0.0102539, 0.9888663, 0.0015575, 0.0023119, -0.0044823,
          1.0525915 }, // DaylightFluorescent (F1)
        { 0.9554129, -0.0231280, 0.0637169, -0.0283629, 1.0099053, 0.0211824, 0.0124188, -0.0206922,
          1.3330592 }, // DayWhiteFluorescent (F8)
        { 0.9147843, -0.0492842, 0.1202810, -0.0622085, 1.034984, 0.0404480, 0.0228014, -0.0375807,
          1.6259804 }, // CoolWhiteFluorescent (F9)
        { 0.8805388, -0.0774890, 0.2293784, -0.0932136, 1.0589267, 0.0757827, 0.0453660, -0.0760107,
          2.2417979 }, // WhiteFluorescent (F3)
        { 0.8488316, -0.1107439, 0.3471428, -0.1310107, 1.0986874, 0.1141548, 0.0694025, -0.1167541,
          2.9109462 }  // WarmWhiteFluorescent (F4)
      };

      Exiv2::ExifData::const_iterator cm1_pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.ColorMatrix1"));
      if((cm1_pos != exifData.end()) && (cm1_pos->count() == 9))
      {
        for(int i = 0; i < 9; i++) colmatrix[0][i] = cm1_pos->toFloat(i);

        if(FIND_EXIF_TAG("Exif.Image.CalibrationIlluminant1")) illu[0] = (dt_dng_illuminant_t) pos->toLong();
      }

      Exiv2::ExifData::const_iterator cm2_pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.ColorMatrix2"));
      if((cm2_pos != exifData.end()) && (cm2_pos->count() == 9))
      {
        for(int i = 0; i < 9; i++) colmatrix[1][i] = cm2_pos->toFloat(i);

        if(FIND_EXIF_TAG("Exif.Image.CalibrationIlluminant2")) illu[1] = (dt_dng_illuminant_t) pos->toLong();
      }

      // So far the Exif.Image.CalibrationIlluminant3 tag and friends have not been implemented and there are no images to test
#if EXIV2_TEST_VERSION(0,27,4)
      Exiv2::ExifData::const_iterator cm3_pos = exifData.findKey(Exiv2::ExifKey("Exif.Image.ColorMatrix3"));
      if((cm3_pos != exifData.end()) && (cm3_pos->count() == 9))
      {
        for(int i = 0; i < 9; i++) colmatrix[2][i] = cm3_pos->toFloat(i);

        if(FIND_EXIF_TAG("Exif.Image.CalibrationIlluminant3")) illu[2] = (dt_dng_illuminant_t) pos->toLong();
      }
#endif

      int sel_illu = -1;
      int sel_temp = 0;
      const int D65temp = _illu_to_temp(DT_LS_D65);
      int delta_min = D65temp;
      // Which illuminant will be used for the color matrix?
      // We first try to find D65 or take the next higher
      for(int i = 0; i < 3; ++i)
      {
        int temp_cur = _illu_to_temp(illu[i]);
        int delta_cur = abs(temp_cur - D65temp);
        if((temp_cur > sel_temp) && (delta_cur <= delta_min))
        {
          sel_illu = i;
          sel_temp = temp_cur;
          delta_min = delta_cur;
        }
      }
      // If there is none defined we'll use the first valid color matrix
      // without correction, i.e. assume D65 (keep dt < 3.8 behaviour)
      // TODO: "Other" illuminant is currently unsupported
      if(sel_illu == -1)
        for(int i = 0; i < 3; ++i)
        {
          if((illu[i] == DT_LS_Unknown) && !isnan(colmatrix[i][0]))
          {
            sel_illu = i;
            sel_temp = D65temp;
            break;
          }
        }

      if((sel_illu > -1) && (dt_get_debug_flags() & DT_DEBUG_IMAGEIO))
      {
        fprintf(stderr, "[exif] `%s` dng illuminant %i (%iK) selected from ", img->filename, illu[sel_illu], sel_temp);
        for(int i = 0; i < 3; i++)
          fprintf(stderr," -- [%i] %i (%iK)", i + 1, illu[i], _illu_to_temp(illu[i]));
        fprintf(stderr, "\n");
      }

      // Take the found CalibrationIlluminant / ColorMatrix pair.
      // D65: just copy. Otherwise multiply by the specific correction matrix.
      if(sel_illu > -1)
      {
       // If no supported Illuminant is found/assumed it's better NOT to use any matrix.
       // The colorin module will write an error message and use a fallback matrix
       // instead of showing wrong colors.
        switch(illu[sel_illu])
        {
          case DT_LS_D50:
            mat3mul(img->d65_color_matrix, correctmat[0], colmatrix[sel_illu]);
            break;
          case DT_LS_D55:
          case DT_LS_Daylight:
          case DT_LS_FineWeather:
          case DT_LS_Flash:
            mat3mul(img->d65_color_matrix, correctmat[1], colmatrix[sel_illu]);
            break;
          case DT_LS_D75:
          case DT_LS_Shade:
            mat3mul(img->d65_color_matrix, correctmat[2], colmatrix[sel_illu]);
            break;
          case DT_LS_Tungsten:
          case DT_LS_StandardLightA:
            mat3mul(img->d65_color_matrix, correctmat[3], colmatrix[sel_illu]);
            break;
          case DT_LS_StandardLightB:
            mat3mul(img->d65_color_matrix, correctmat[4], colmatrix[sel_illu]);
            break;
          case DT_LS_StandardLightC:
            mat3mul(img->d65_color_matrix, correctmat[5], colmatrix[sel_illu]);
            break;
          case DT_LS_Fluorescent:
            mat3mul(img->d65_color_matrix, correctmat[6], colmatrix[sel_illu]);
            break;
          case DT_LS_ISOStudioTungsten:
            mat3mul(img->d65_color_matrix, correctmat[7], colmatrix[sel_illu]);
            break;
          case DT_LS_DaylightFluorescent:
            mat3mul(img->d65_color_matrix, correctmat[8], colmatrix[sel_illu]);
            break;
          case DT_LS_DayWhiteFluorescent:
            mat3mul(img->d65_color_matrix, correctmat[9], colmatrix[sel_illu]);
            break;
          case DT_LS_CoolWhiteFluorescent:
            mat3mul(img->d65_color_matrix, correctmat[10], colmatrix[sel_illu]);
            break;
          case DT_LS_WhiteFluorescent:
            mat3mul(img->d65_color_matrix, correctmat[11], colmatrix[sel_illu]);
            break;
          case DT_LS_WarmWhiteFluorescent:
            mat3mul(img->d65_color_matrix, correctmat[12], colmatrix[sel_illu]);
            break;
          case DT_LS_D65:
          case DT_LS_CloudyWeather:
          case DT_LS_Unknown: // exceptional fallback to keep dt < 3.8 behaviour
            for(int i = 0; i < 9; i++) img->d65_color_matrix[i] = colmatrix[sel_illu][i];
            break;

          default:
            fprintf(stderr,"[exif] did not find a proper dng correction matrix for illuminant %i\n", illu[sel_illu]);
            break;
        }
      }
    }

    // Finding out about DNG hdr and monochrome images can be done here while reading exif data.
    if(FIND_EXIF_TAG("Exif.Image.DNGVersion"))
    {
      int format = 1;
      int bps = 0;
      int spp = 0;
      int phi = 0;

      if(FIND_EXIF_TAG("Exif.SubImage1.SampleFormat"))
        format = pos->toLong();
      else if(FIND_EXIF_TAG("Exif.Image.SampleFormat"))
        format = pos->toLong();

      if(FIND_EXIF_TAG("Exif.SubImage1.BitsPerSample"))
        bps = pos->toLong();
      else if(FIND_EXIF_TAG("Exif.Image.BitsPerSample"))
        bps = pos->toLong();

      if(FIND_EXIF_TAG("Exif.SubImage1.SamplesPerPixel"))
        spp = pos->toLong();
      else if(FIND_EXIF_TAG("Exif.Image.SamplesPerPixel"))
        spp = pos->toLong();

      if(FIND_EXIF_TAG("Exif.SubImage1.PhotometricInterpretation"))
        phi = pos->toLong();
      else if(FIND_EXIF_TAG("Exif.Image.PhotometricInterpretation"))
        phi = pos->toLong();

      if((format == 3) && (bps >= 16) && ((phi == 32803) || (phi == 34892))) 
        img->flags |= DT_IMAGE_HDR;

      if((spp == 1) && (phi == 34892)) 
        img->flags |= DT_IMAGE_MONOCHROME;
    }
    
    // some files have the display colorspace explicitly set. try to read that. The Exif.Photo.ColorSpace
    // tag only exists in display-referred integer images, so gate on "not raw and not HDR-float"
    // rather than on DT_IMAGE_LDR: at this point the dynamic range of an ambiguous container (TIFF,
    // AVIF, HEIF) is not yet known (the extension can't tell, the buffer is not decoded), so the LDR
    // flag may legitimately be unset here even for an integer image.
    // tag absent -> leave colorspace as none
    // 0x01   -> sRGB
    // 0x02   -> AdobeRGB
    // 0xffff -> Uncalibrated
    //          + Exif.Iop.InteroperabilityIndex of 'R03' -> AdobeRGB
    //          + Exif.Iop.InteroperabilityIndex of 'R98' -> sRGB
    if(!dt_image_is_raw(img) && FIND_EXIF_TAG("Exif.Photo.ColorSpace"))
    {
      int colorspace = pos->toLong();
      if(colorspace == 0x01)
        img->colorspace = DT_IMAGE_COLORSPACE_SRGB;
      else if(colorspace == 0x02)
        img->colorspace = DT_IMAGE_COLORSPACE_ADOBE_RGB;
      else if(colorspace == 0xffff)
      {
        if(FIND_EXIF_TAG("Exif.Iop.InteroperabilityIndex"))
        {
          std::string interop_index = pos->toString();
          if(interop_index == "R03")
            img->colorspace = DT_IMAGE_COLORSPACE_ADOBE_RGB;
          else if(interop_index == "R98")
            img->colorspace = DT_IMAGE_COLORSPACE_SRGB;
        }
      }
    }

    // Improve lens detection for Sony SAL lenses.
    if(FIND_EXIF_TAG("Exif.Sony2.LensID") && pos->toLong() != 65535 && pos->print().find('|') == std::string::npos)
    {
      dt_strlcpy_to_utf8(img->exif_lens, sizeof(img->exif_lens), pos, exifData);
    }
    // Workaround for an issue on newer Sony NEX cams.
    // The default EXIF field is not used by Sony to store lens data
    // http://dev.exiv2.org/issues/883
    // http://darktable.org/redmine/issues/8813
    // FIXME: This is still a workaround
    else if((!strncmp(img->exif_model, "NEX", 3)) || (!strncmp(img->exif_model, "ILCE", 4)))
    {
      snprintf(img->exif_lens, sizeof(img->exif_lens), "(unknown)");
      if(FIND_EXIF_TAG("Exif.Photo.LensModel"))
      {
        std::string str = pos->print(&exifData);
        snprintf(img->exif_lens, sizeof(img->exif_lens), "%s", str.c_str());
      }
    };

    img->exif_inited = TRUE;
    return true;
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 _exif_decode_exif_data] " << img->filename << ": " << s << std::endl;
    return false;
  }
}

// TODO: can this blob also contain xmp and iptc data?
int dt_exif_read_from_blob(dt_image_t *img, uint8_t *blob, const int size)
{
  try
  {
    Exiv2::ExifData exifData;
    Exiv2::ExifParser::decode(exifData, blob, size);
    bool res = _exif_decode_exif_data(img, exifData);
    return res ? 0 : 1;
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_read_from_blob] " << img->filename << ": " << s << std::endl;
    return 1;
  }
}

/**
 * Get the largest possible thumbnail from the image
 */
int dt_exif_get_thumbnail(const char *path, uint8_t **buffer, size_t *size, char **mime_type, int *width, int *height, int min_width)
{
  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    if(!image.get()) return 1;
    read_metadata_threadsafe(image);

    // Get a list of preview images available in the image. The list is sorted
    // by the preview image pixel size, starting with the smallest preview.
    Exiv2::PreviewManager loader(*image);
    Exiv2::PreviewPropertiesList list = loader.getPreviewProperties();
    if(list.empty())
    {
      dt_print(DT_DEBUG_LIGHTTABLE, "[exiv2 dt_exif_get_thumbnail] couldn't find thumbnail for %s\n", path);
      return 1;
    }

    // Get the largest mipmap
    Exiv2::PreviewProperties selected = list.back();

    // Get the selected preview image
    Exiv2::PreviewImage preview = loader.getPreviewImage(selected);
    const unsigned  char *tmp = preview.pData();
    size_t _size = preview.size();

    *size = _size;
    *width = preview.width();
    *height = preview.height();
    *mime_type = strdup(preview.mimeType().c_str());
    *buffer = (uint8_t *)malloc(_size);
    if(IS_NULL_PTR(*buffer)) {
      std::cerr << "[exiv2 dt_exif_get_thumbnail] couldn't allocate memory for thumbnail for " << path << std::endl;
      return 1;
    }

    memcpy(*buffer, tmp, _size);

    return 0;
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_get_thumbnail] " << path << ": " << s << std::endl;
    return 1;
  }
}

/** read the metadata of an image.
 * XMP data trumps IPTC data trumps EXIF data
 */
int dt_exif_read(dt_image_t *img, const char *path)
{
  // Seed the provisional image-type flag (LDR / HDR / RAW, from the file extension) before we probe
  // dt_image_is_ldr() / dt_image_is_hdr() while decoding the EXIF below. This function can run on a
  // freshly dt_image_init()'d object (import preview, path-pattern expansion) long before the buffer
  // is decoded and dt_image_buffer_resolve_flags() sets the authoritative datatype-derived flags.
  // Only seed when nothing is classified yet, so a DB-loaded / already-resolved image is untouched.
  if(!(img->flags & (DT_IMAGE_LDR | DT_IMAGE_HDR | DT_IMAGE_RAW | DT_IMAGE_S_RAW)))
  {
    const char *ext = g_strrstr(path, ".");
    if(ext) img->flags |= dt_image_flags_from_extension(ext + 1);
  }

  // at least set datetime taken to something useful in case there is no exif data in this file (pfm, png,
  // ...)
  struct stat statbuf;

  if(!stat(path, &statbuf))
  {
    dt_datetime_unix_to_img(img, &statbuf.st_mtime);
  }

  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    if(!image.get()) return 1;
    read_metadata_threadsafe(image);
    bool res = true;

    // EXIF metadata
    Exiv2::ExifData &exifData = image->exifData();
    if(!exifData.empty())
    {
      res = _exif_decode_exif_data(img, exifData);
    }
    else
      img->exif_inited = 1;

    // IPTC metadata.
    Exiv2::IptcData &iptcData = image->iptcData();
    if(!iptcData.empty()) res = _exif_decode_iptc_data(img, iptcData) && res;

    // XMP metadata
    Exiv2::XmpData &xmpData = image->xmpData();
    if(!xmpData.empty())
      res = dt_exif_decode_xmp_data(img, xmpData, -1, true) && res;

    // Initialize size - don't wait for full raw to be loaded to get this
    // information. If use_embedded_thumbnail is set, it will take a
    // change in development history to have this information
    img->height = image->pixelHeight();
    img->width = image->pixelWidth();

    return res ? 0 : 1;
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_read] " << path << ": " << s << std::endl;
    return 1;
  }
}

int dt_exif_write_blob(uint8_t *blob, uint32_t size, const char *path, const int compressed)
{
  try
  {
    // Serialize the whole exiv2 region (read + write): writeMetadata() below re-enters the
    // non-thread-safe exiv2/XMP toolkit, so it must not run concurrently with other exiv2 work.
    // The mutex is recursive, so the nested read_metadata_threadsafe() re-locks harmlessly.
    Lock lock;

    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    if(!image.get()) return 1;
    read_metadata_threadsafe(image);
    Exiv2::ExifData &imgExifData = image->exifData();
    Exiv2::ExifData blobExifData;
    Exiv2::ExifParser::decode(blobExifData, blob, size);
    Exiv2::ExifData::const_iterator end = blobExifData.end();
    Exiv2::ExifData::iterator it;
    for(Exiv2::ExifData::const_iterator i = blobExifData.begin(); i != end; ++i)
    {
      // add() does not override! we need to delete existing key first.
      Exiv2::ExifKey key(i->key());
      if((it = imgExifData.findKey(key)) != imgExifData.end()) imgExifData.erase(it);

      imgExifData.add(Exiv2::ExifKey(i->key()), &i->value());
    }

    {
      // Remove thumbnail
      static const char *keys[] = {
        "Exif.Thumbnail.Compression",
        "Exif.Thumbnail.XResolution",
        "Exif.Thumbnail.YResolution",
        "Exif.Thumbnail.ResolutionUnit",
        "Exif.Thumbnail.JPEGInterchangeFormat",
        "Exif.Thumbnail.JPEGInterchangeFormatLength"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(imgExifData, keys, n_keys);
    }

    // only compressed images may set PixelXDimension and PixelYDimension
    if(!compressed)
    {
      static const char *keys[] = {
        "Exif.Photo.PixelXDimension",
        "Exif.Photo.PixelYDimension"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(imgExifData, keys, n_keys);
    }

    imgExifData.sortByTag();
    image->writeMetadata();
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_write_blob] " << path << ": " << s << std::endl;
    return 0;
  }
  return 1;
}

// encode binary blob into text:
char *dt_exif_xmp_encode(const unsigned char *input, const int len, int *output_len)
{
#define COMPRESS_THRESHOLD 100

  gboolean do_compress = FALSE;

  // if input data field exceeds a certain size we compress it and convert to base64;
  // main reason for compression: make more xmp data fit into 64k segment within
  // JPEG output files.
  char *config = dt_conf_get_string("compress_xmp_tags");
  if(config)
  {
    if(!strcmp(config, "always"))
      do_compress = TRUE;
    else if((len > COMPRESS_THRESHOLD) && !strcmp(config, "only large entries"))
      do_compress = TRUE;
    else
      do_compress = FALSE;
    dt_free(config);
  }

  return dt_exif_xmp_encode_internal(input, len, output_len, do_compress);

#undef COMPRESS_THRESHOLD
}

char *dt_exif_xmp_encode_internal(const unsigned char *input, const int len, int *output_len, gboolean do_compress)
{
  char *output = NULL;

  if(do_compress)
  {
    int result;
    uLongf destLen = compressBound(len);
    unsigned char *buffer1 = (unsigned char *)malloc(destLen);

    result = compress(buffer1, &destLen, input, len);

    if(result != Z_OK)
    {
      dt_free(buffer1);
      return NULL;
    }

    // we store the compression factor
    const int factor = MIN(len / destLen + 1, 99);

    char *buffer2 = (char *)g_base64_encode(buffer1, destLen);
    dt_free(buffer1);
    if(IS_NULL_PTR(buffer2)) return NULL;

    int outlen = strlen(buffer2) + 5; // leading "gz" + compression factor + base64 string + trailing '\0'
    output = (char *)malloc(outlen);
    if(IS_NULL_PTR(output))
    {
      dt_free(buffer2);
      return NULL;
    }

    output[0] = 'g';
    output[1] = 'z';
    output[2] = factor / 10 + '0';
    output[3] = factor % 10 + '0';
    g_strlcpy(output + 4, buffer2, outlen);
    dt_free(buffer2);

    if(output_len) *output_len = outlen;
  }
  else
  {
    const char hex[16] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

    output = (char *)malloc(2 * len + 1);
    if(IS_NULL_PTR(output)) return NULL;

    if(output_len) *output_len = 2 * len + 1;

    for(int i = 0; i < len; i++)
    {
      const int hi = input[i] >> 4;
      const int lo = input[i] & 15;
      output[2 * i] = hex[hi];
      output[2 * i + 1] = hex[lo];
    }
    output[2 * len] = '\0';
  }

  return output;
}

// and back to binary
unsigned char *dt_exif_xmp_decode(const char *input, const int len, int *output_len)
{
  unsigned char *output = NULL;

  // check if data is in compressed format
  if(!strncmp(input, "gz", 2))
  {
    // we have compressed data in base64 representation with leading "gz"

    // get stored compression factor so we know the needed buffer size for uncompress
    const float factor = 10 * (input[2] - '0') + (input[3] - '0');

    // get a rw copy of input buffer omitting leading "gz" and compression factor
    unsigned char *buffer = (unsigned char *)strdup(input + 4);
    if(IS_NULL_PTR(buffer)) return NULL;

    // decode from base64 to compressed binary
    gsize compressed_size;
    g_base64_decode_inplace((char *)buffer, &compressed_size);

    // do the actual uncompress step
    int result = Z_BUF_ERROR;
    uLongf bufLen = factor * compressed_size;
    uLongf destLen;

    // we know the actual compression factor but if that fails we re-try with
    // increasing buffer sizes, eg. we don't know (unlikely) factors > 99
    do
    {
      if(output)
      {
        dt_free(output);
      }
      output = (unsigned char *)malloc(bufLen);
      if(IS_NULL_PTR(output)) break;

      destLen = bufLen;

      result = uncompress(output, &destLen, buffer, compressed_size);

      bufLen *= 2;

    } while(result == Z_BUF_ERROR);


    dt_free(buffer);

    if(result != Z_OK)
    {
      if(output)
      {
        dt_free(output);
      }
      return NULL;
    }

    if(output_len) *output_len = destLen;
  }
  else
  {
// we have uncompressed data in hexadecimal ascii representation

// ascii table:
// 48- 57 0-9
// 97-102 a-f
#define TO_BINARY(a) (a > 57 ? a - 97 + 10 : a - 48)

    // make sure that we don't find any unexpected characters indicating corrupted data
    if(strspn(input, "0123456789abcdef") != strlen(input)) return NULL;

    output = (unsigned char *)malloc(len / 2);
    if(IS_NULL_PTR(output)) return NULL;

    if(output_len) *output_len = len / 2;

    for(int i = 0; i < len / 2; i++)
    {
      const int hi = TO_BINARY(input[2 * i]);
      const int lo = TO_BINARY(input[2 * i + 1]);
      output[i] = (hi << 4) | lo;
    }
#undef TO_BINARY
  }

  return output;
}

static void _exif_import_tags(dt_image_t *img, Exiv2::XmpData::iterator &pos)
{
  // tags in array
  const int cnt = pos->count();

  for(int i = 0; i < cnt; i++)
  {
    char tagbuf[1024];
    std::string pos_str = pos->toString(i);
    g_strlcpy(tagbuf, pos_str.c_str(), sizeof(tagbuf));
    int tagid = -1;
    char *tag = tagbuf;
    while(tag)
    {
      char *next_tag = strstr(tag, ",");
      if(next_tag) *(next_tag++) = 0;
      // check if tag is available, get its id:
      for(int k = 0; k < 2; k++)
      {
        const guint found = dt_tag_repository_find_by_name(tag);
        if(found > 0) tagid = (int)found;

        if(tagid > 0) break;

        fprintf(stderr, "[xmp_import] creating tag: %s\n", tag);
        // create this tag (increment id, leave icon empty), retry.
        dt_tag_repository_insert(tag);
      }
      // associate image and tag.
      dt_tag_repository_attach(tagid, img->id);

      tag = next_tag;
    }
  }
}

dt_colorspaces_color_profile_type_t dt_exif_get_color_space(const uint8_t *data, size_t size)
{
  try
  {
    Exiv2::ExifData::const_iterator pos;
    Exiv2::ExifData exifData;
    Exiv2::ExifParser::decode(exifData, data, size);
    // clang-format off
    // 0x01   -> sRGB
    // 0x02   -> AdobeRGB
    // 0xffff -> Uncalibrated
    //          + Exif.Iop.InteroperabilityIndex of 'R03' -> AdobeRGB
    //          + Exif.Iop.InteroperabilityIndex of 'R98' -> sRGB
    // clang-format on
    if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Photo.ColorSpace"))) != exifData.end() && pos->size())
    {
      int colorspace = pos->toLong();
      if(colorspace == 0x01)
        return DT_COLORSPACE_SRGB;
      else if(colorspace == 0x02)
        return DT_COLORSPACE_ADOBERGB;
      else if(colorspace == 0xffff)
      {
        if((pos = exifData.findKey(Exiv2::ExifKey("Exif.Iop.InteroperabilityIndex"))) != exifData.end()
          && pos->size())
        {
          std::string interop_index = pos->toString();
          if(interop_index == "R03")
            return DT_COLORSPACE_ADOBERGB;
          else if(interop_index == "R98")
            return DT_COLORSPACE_SRGB;
        }
      }
    }

    return DT_COLORSPACE_DISPLAY; // nothing embedded
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_get_color_space] " << s << std::endl;
    return DT_COLORSPACE_DISPLAY;
  }
}

void dt_exif_get_datetime_taken(const uint8_t *data, size_t size, char *datetime_taken)
{
  try
  {
    Exiv2::ExifData::const_iterator pos;
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(data, size));
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();

    _find_datetime_taken(exifData, pos, datetime_taken);
  }
  catch(const std::exception &e)
  {
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_get_datetime_taken] " << s << std::endl;
  }
}

static void dt_exif_log_handler(int log_level, const char *message)
{
  if(log_level >= Exiv2::LogMsg::level())
  {
    // We don't seem to need \n in the format string as exiv2 includes it
    // in the messages themselves
    dt_print(DT_DEBUG_CAMERA_SUPPORT, "[exiv2] %s", message);
  }
}

void dt_exif_init()
{
  // preface the exiv2 messages with "[exiv2] "
  Exiv2::LogMsg::setHandler(&dt_exif_log_handler);

  // enable isobmff support if exiv2 was built with it
  // the enableBMFF function is deprecated from exiv2 0.28.0
  #if !EXIV2_TEST_VERSION(0,28,0)
  #ifdef HAVE_LIBEXIV2_WITH_ISOBMFF
  Exiv2::enableBMFF();
  #endif
  #endif

  // XmpParser init/term are required on older exiv2, but are deprecated no-ops from 0.28+.
  #if !EXIV2_TEST_VERSION(0,28,0)
  Exiv2::XmpParser::initialize();
  #endif
  // this has to stay with the old url (namespace already propagated outside dt)
  Exiv2::XmpProperties::registerNs("http://darktable.sf.net/", "darktable");
  // check is Exiv2 version already knows these prefixes
  try
  {
    Exiv2::XmpProperties::propertyList("lr");
  }
  catch(const std::exception &e)
  {
    // if lightroom is not known register it
    Exiv2::XmpProperties::registerNs("http://ns.adobe.com/lightroom/1.0/", "lr");
  }
  try
  {
    Exiv2::XmpProperties::propertyList("exifEX");
  }
  catch(const std::exception &e)
  {
    // if exifEX is not known register it
    Exiv2::XmpProperties::registerNs("http://cipa.jp/exif/1.0/", "exifEX");
  }
}

void dt_exif_cleanup()
{
  // Keep explicit termination for older exiv2; deprecated no-op in 0.28+.
  #if !EXIV2_TEST_VERSION(0,28,0)
  Exiv2::XmpParser::terminate();
  #endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
