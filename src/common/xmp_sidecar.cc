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

/** @file common/xmp_sidecar.cc
 *
 * @brief The XMP-sidecar half of what used to be `common/exif.cc`.
 *
 * @details Cut out of a 4775-line translation unit that was doing two unrelated jobs. This
 * one serialises the *development* -- history stack, mask shapes, module order -- into and
 * out of the XMP document; the other, the photograph's own tags, is `metadata/exif.cc`.
 * See `common/xmp_sidecar.h`, including why this is not in `src/metadata`.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>
#include "common/paths.h"   // DT_PATH_MAX
#include <glib.h>
#include <glib/gstdio.h>
#include <time.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <exiv2/exiv2.hpp>

#include <pugixml.hpp>

#include "common/xmp_sidecar.h"
#include "metadata/exif.h"
#include "metadata/exif_internal.h"

#include "caches/image_cache.h"
#include "common/conf.h"
#include "common/datetime.h"
#include "common/deprecations.h"
#include "history/history.h"
#include "common/utility.h"
#include "common/variables.h"
#include "database/colorlabel_repository.h"
#include "database/database.h"
#include "database/history_repository.h"
#include "database/image_repository.h"
#include "database/metadata_repository.h"
#include "develop/blend.h"
#include "develop/iop_order.h"
#include "develop/masks.h"
#include "imageio/imageio_core.h"
#include "metadata/metadata.h"
#include "metadata/notify.h"
#include "metadata/tags.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

using namespace std;

#define DT_XMP_EXIF_VERSION 5

#if EXIV2_TEST_VERSION(0,28,0)
#define AnyError Error
#define toLong toInt64
#endif

static const char *_exif_get_exiv2_tag_type(const char *tagname)
{
  if(IS_NULL_PTR(tagname)) return NULL;

  const size_t tagname_len = strlen(tagname);

  // the list lives in metadata/exif.cc and is built on first use; it was a file-static
  // shared with this code when the two were one translation unit
  for(const GList *tag = dt_exif_get_exiv2_taglist(); tag; tag = g_list_next(tag))
  {
    const char *t = (const char *)tag->data;
    if(IS_NULL_PTR(t)) continue;   // was tested AFTER t had already been dereferenced

    // each entry is "<tagname>,<type>". The prefix comparison having succeeded is what
    // makes t[tagname_len] in bounds: it is either the ',' or the terminating NUL.
    if(strncmp(t, tagname, tagname_len) == 0 && t[tagname_len] == ',')
      return t + tagname_len + 1;
  }
  return NULL;
}

static void read_xmp_timestamps(Exiv2::XmpData &xmpData, dt_image_t *img, const int xmp_version);

// this array should contain all XmpBag and XmpSeq keys used by dt
const char *dt_xmp_keys[]
    = { "Xmp.dc.subject", "Xmp.lr.hierarchicalSubject", "Xmp.darktable.colorlabels", "Xmp.darktable.history",
        "Xmp.darktable.history_modversion", "Xmp.darktable.history_enabled", "Xmp.darktable.history_end",
        "Xmp.darktable.iop_order_version", "Xmp.darktable.iop_order_list",
        "Xmp.darktable.history_operation", "Xmp.darktable.history_params", "Xmp.darktable.blendop_params",
        "Xmp.darktable.blendop_version", "Xmp.darktable.multi_priority", "Xmp.darktable.multi_name",
        "Xmp.darktable.iop_order",
        "Xmp.darktable.xmp_version", "Xmp.darktable.raw_params", "Xmp.darktable.auto_presets_applied",
        "Xmp.darktable.mask_id", "Xmp.darktable.mask_type", "Xmp.darktable.mask_name",
        "Xmp.darktable.masks_history", "Xmp.darktable.mask_num", "Xmp.darktable.mask_points",
        "Xmp.darktable.mask_version", "Xmp.darktable.mask", "Xmp.darktable.mask_nb", "Xmp.darktable.mask_src",
        "Xmp.darktable.history_basic_hash", "Xmp.darktable.history_auto_hash", "Xmp.darktable.history_current_hash",
        "Xmp.darktable.import_timestamp", "Xmp.darktable.change_timestamp",
        "Xmp.darktable.export_timestamp", "Xmp.darktable.print_timestamp",
        "Xmp.acdsee.notes", "Xmp.darktable.version_name",
        "Xmp.dc.creator", "Xmp.dc.publisher", "Xmp.dc.title", "Xmp.dc.description", "Xmp.dc.rights",
        "Xmp.xmpMM.DerivedFrom" };

static const guint dt_xmp_keys_n = G_N_ELEMENTS(dt_xmp_keys); // the number of XmpBag XmpSeq keys that dt uses

/** @brief Read an XMP packet off disk into @p packet, FALSE if the file cannot be read.
 *
 * @details `Exiv2::readFile()` would do this, but only through exiv2's own path handling --
 * which is exactly the capability 0.28 dropped on Windows (see WIDEN() in
 * `metadata/exif_internal.h`). These are small sidecar files and Ansel already has a reader
 * that goes through `g_fopen()`, so there is no reason to route them back out through exiv2
 * and depend on something it may not be able to do. It also drops the 0.27-vs-0.28 fork over
 * DataBuf's members that each of those call sites carried.
 */
static bool _read_xmp_packet(const char *filename, std::string &packet)
{
  size_t length = 0;
  errno = 0;
  char *content = dt_read_file(filename, &length);

  if(IS_NULL_PTR(content))
  {
    fprintf(stderr, "cannot read xmp file '%s': '%s'\n", filename, strerror(errno));
    return false;
  }

  packet.assign(content, length);
  dt_free(content);
  return true;
}

// function to remove known dt keys and subtrees from xmpdata, so not to append them twice
// this should work because dt first reads all known keys
static void dt_remove_known_keys(Exiv2::XmpData &xmp)
{
  xmp.sortByKey();
  for(unsigned int i = 0; i < dt_xmp_keys_n; i++)
  {
    Exiv2::XmpData::iterator pos = xmp.findKey(Exiv2::XmpKey(dt_xmp_keys[i]));

    while(pos != xmp.end())
    {
      std::string key = pos->key();
      const char *ckey = key.c_str();
      size_t len = key.size();
      // stop iterating once the key no longer matches what we are trying to delete. this assumes sorted input
      if(!(g_str_has_prefix(ckey, dt_xmp_keys[i]) && (ckey[len] == '[' || ckey[len] == '\0')))
        break;
      pos = xmp.erase(pos);
    }
  }
}

static void dt_remove_xmp_keys(Exiv2::XmpData &xmp, const char *keys[], unsigned int n_keys)
{
  for(unsigned int i = 0; i < n_keys; i++)
  {
    try
    {
      Exiv2::XmpData::iterator pos;
      while((pos = xmp.findKey(Exiv2::XmpKey(keys[i]))) != xmp.end())
        xmp.erase(pos);
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

static void dt_remove_exif_geotag(Exiv2::ExifData &exifData)
{
  static const char *keys[] =
  {
    "Exif.GPSInfo.GPSLatitude",
    "Exif.GPSInfo.GPSLongitude",
    "Exif.GPSInfo.GPSAltitude",
    "Exif.GPSInfo.GPSLatitudeRef",
    "Exif.GPSInfo.GPSLongitudeRef",
    "Exif.GPSInfo.GPSAltitudeRef",
    "Exif.GPSInfo.GPSVersionID"
  };
  static const guint n_keys = G_N_ELEMENTS(keys);
  dt_remove_exif_keys(exifData, keys, n_keys);
}

int dt_exif_read_blob(uint8_t **buf, const char *path, const int32_t imgid, const int sRGB, const int out_width,
                      const int out_height, const int dng_mode)
{
  *buf = NULL;
  try
  {
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(path)));
    if(!image.get()) return 1;
    read_metadata_threadsafe(image);
    Exiv2::ExifData &exifData = image->exifData();

    // get rid of thumbnails
    Exiv2::ExifThumb(exifData).erase();
    Exiv2::ExifData::const_iterator pos;

    {
      static const char *keys[] = {
        "Exif.Image.ImageWidth",
        "Exif.Image.ImageLength",
        "Exif.Image.BitsPerSample",
        "Exif.Image.Compression",
        "Exif.Image.PhotometricInterpretation",
        "Exif.Image.FillOrder",
        "Exif.Image.SamplesPerPixel",
        "Exif.Image.StripOffsets",
        "Exif.Image.RowsPerStrip",
        "Exif.Image.StripByteCounts",
        "Exif.Image.TileWidth",
        "Exif.Image.TileLength",
        "Exif.Image.TileOffsets",
        "Exif.Image.TileByteCounts",
        "Exif.Image.PlanarConfiguration"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(exifData, keys, n_keys);
    }

      /* Many tags should be removed in all cases as they are simply wrong also for dng files */

      // remove subimage* trees, related to thumbnails or HDR usually; also UserCrop
    for(Exiv2::ExifData::iterator i = exifData.begin(); i != exifData.end();)
    {
      static const std::string needle = "Exif.SubImage";
      if(i->key().compare(0, needle.length(), needle) == 0)
        i = exifData.erase(i);
      else
        ++i;
    }

    {
      static const char *keys[] = {
        // Canon color space info
        "Exif.Canon.ColorSpace",
        "Exif.Canon.ColorData",

        // Nikon thumbnail data
        "Exif.Nikon3.Preview",
        "Exif.NikonPreview.JPEGInterchangeFormat",

        // TIFF/EP & Exif stuff irrelevant after developing
        "Exif.Image.CFARepeatPatternDim",
        "Exif.Image.CFAPattern",
        "Exif.Image.InterColorProfile",
        "Exif.Image.SpectralSensitivity",
        "Exif.Image.OECF",
        "Exif.Image.SpatialFrequencyResponse",
        "Exif.Image.Noise",
        "Exif.Image.SensingMethod",
        "Exif.Image.TIFFEPStandardID",
        "Exif.Photo.SpectralSensitivity",
        "Exif.Photo.OECF",
        "Exif.Photo.SpatialFrequencyResponse",
        "Exif.Photo.SensingMethod",
        "Exif.Photo.CFAPattern",

        // DNG stuff that is irrelevant or misleading
        "Exif.Image.DNGVersion",
        "Exif.Image.DNGBackwardVersion",
        "Exif.Image.DNGPrivateData",
        "Exif.Image.DefaultBlackRender",
        "Exif.Image.DefaultCropOrigin",
        "Exif.Image.DefaultCropSize",
        "Exif.Image.RawDataUniqueID",
        "Exif.Image.OriginalRawFileName",
        "Exif.Image.OriginalRawFileData",
        "Exif.Image.ActiveArea",
        "Exif.Image.MaskedAreas",
        "Exif.Image.AsShotICCProfile",
        "Exif.Image.OpcodeList1",
        "Exif.Image.OpcodeList2",
        "Exif.Image.OpcodeList3",
        "Exif.Image.AsShotNeutral",
        "Exif.Image.AsShotWhiteXY",
        "Exif.Image.BaselineExposure",
        "Exif.Image.BaselineNoise",
        "Exif.Image.BaselineSharpness",
        "Exif.Image.LinearResponseLimit",
        "Exif.Image.ShadowScale",
        "Exif.Image.PreviewApplicationName",
        "Exif.Image.PreviewApplicationVersion",
        "Exif.Image.PreviewSettingsDigest",
        "Exif.Image.PreviewColorSpace",
        "Exif.Image.PreviewDateTime",
        "Exif.Image.NoiseProfile",
        "Exif.Image.NewRawImageDigest",
        "Exif.Image.RawImageDigest",

        "Exif.Photo.MakerNote",

        // Pentax thumbnail data
        "Exif.Pentax.PreviewResolution",
        "Exif.Pentax.PreviewLength",
        "Exif.Pentax.PreviewOffset",
        "Exif.PentaxDng.PreviewResolution",
        "Exif.PentaxDng.PreviewLength",
        "Exif.PentaxDng.PreviewOffset",
        // Pentax color info
        "Exif.PentaxDng.ColorInfo",

        // Minolta thumbnail data
        "Exif.Minolta.Thumbnail",
        "Exif.Minolta.ThumbnailOffset",
        "Exif.Minolta.ThumbnailLength",

        // Sony thumbnail data
        "Exif.SonyMinolta.ThumbnailOffset",
        "Exif.SonyMinolta.ThumbnailLength",

        // Olympus thumbnail data
        "Exif.Olympus.Thumbnail",
        "Exif.Olympus.ThumbnailOffset",
        "Exif.Olympus.ThumbnailLength",

        "Exif.Image.BaselineExposureOffset",

        // Samsung makernote cleanup, the entries below have no
        // relevance for exported images
        "Exif.Samsung2.SensorAreas",
        "Exif.Samsung2.ColorSpace",
        "Exif.Samsung2.EncryptionKey",
        "Exif.Samsung2.WB_RGGBLevelsUncorrected",
        "Exif.Samsung2.WB_RGGBLevelsAuto",
        "Exif.Samsung2.WB_RGGBLevelsIlluminator1",
        "Exif.Samsung2.WB_RGGBLevelsIlluminator2",
        "Exif.Samsung2.WB_RGGBLevelsBlack",
        "Exif.Samsung2.ColorMatrix",
        "Exif.Samsung2.ColorMatrixSRGB",
        "Exif.Samsung2.ColorMatrixAdobeRGB",
        "Exif.Samsung2.ToneCurve1",
        "Exif.Samsung2.ToneCurve2",
        "Exif.Samsung2.ToneCurve3",
        "Exif.Samsung2.ToneCurve4"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(exifData, keys, n_keys);
    }

    static const char *dngkeys[] = {
        // Embedded color profile info
      "Exif.Image.ColorMatrix1",
      "Exif.Image.ColorMatrix2",
      "Exif.Image.CameraCalibration1",
      "Exif.Image.CameraCalibration2",
      "Exif.Image.ReductionMatrix1",
      "Exif.Image.ReductionMatrix2",
      "Exif.Image.AnalogBalance",
      "Exif.Image.CalibrationIlluminant1",
      "Exif.Image.CalibrationIlluminant2",
      "Exif.Image.CameraCalibrationSignature",
      "Exif.Image.ProfileCalibrationSignature",
      "Exif.Image.ExtraCameraProfiles",
      "Exif.Image.AsShotProfileName",
      "Exif.Image.ProfileName",
      "Exif.Image.ProfileHueSatMapDims",
      "Exif.Image.ProfileHueSatMapData1",
      "Exif.Image.ProfileHueSatMapData2",
      "Exif.Image.ProfileToneCurve",
      "Exif.Image.ProfileEmbedPolicy",
      "Exif.Image.ProfileCopyright",
      "Exif.Image.ForwardMatrix1",
      "Exif.Image.ForwardMatrix2",
      "Exif.Image.ProfileLookTableDims",
      "Exif.Image.ProfileLookTableData",
      "Exif.Image.ProfileLookTableEncoding",
      "Exif.Image.ProfileHueSatMapEncoding"
        };
      static const guint n_dngkeys = G_N_ELEMENTS(dngkeys);
    dt_remove_exif_keys(exifData, dngkeys, n_dngkeys);

    /* Write appropriate color space tag if using sRGB output */
    if(sRGB)
      exifData["Exif.Photo.ColorSpace"] = uint16_t(1); /* sRGB */
    else
      exifData["Exif.Photo.ColorSpace"] = uint16_t(0xFFFF); /* Uncalibrated */

    // we don't write the orientation here for dng as it is set in dt_imageio_dng_write_tiff_header
    // or might be defined in this blob.
    if(!dng_mode) exifData["Exif.Image.Orientation"] = uint16_t(1);

    /* Replace RAW dimension with output dimensions (for example after crop/scale, or orientation for dng
     * mode) */
    if(out_width > 0) exifData["Exif.Photo.PixelXDimension"] = (uint32_t)out_width;
    if(out_height > 0) exifData["Exif.Photo.PixelYDimension"] = (uint32_t)out_height;

    const int resolution = dt_conf_get_int("metadata/resolution");
    exifData["Exif.Image.XResolution"] = Exiv2::Rational(resolution, 1);
    exifData["Exif.Image.YResolution"] = Exiv2::Rational(resolution, 1);
    exifData["Exif.Image.ResolutionUnit"] = uint16_t(2); /* inches */

    exifData["Exif.Image.Software"] = darktable_package_string;

    // TODO: find a nice place for the missing metadata (tags, publisher, colorlabels?). Additionally find out
    // how to embed XMP data.
    //       And shall we add a description of the history stack to Exif.Image.ImageHistory?
    if(imgid >= 0)
    {
      /* Delete metadata taken from the original file if it's fields we manage in dt, too */
      static const char * keys[] = {
        "Exif.Image.Artist",
        "Exif.Image.ImageDescription",
        "Exif.Photo.UserComment",
        "Exif.Image.Copyright",
        "Exif.Image.Rating",
        "Exif.Image.RatingPercent",
        "Exif.Photo.SubSecTimeOriginal",
        "Exif.GPSInfo.GPSVersionID",
        "Exif.GPSInfo.GPSLongitudeRef",
        "Exif.GPSInfo.GPSLatitudeRef",
        "Exif.GPSInfo.GPSLongitude",
        "Exif.GPSInfo.GPSLatitude",
        "Exif.GPSInfo.GPSAltitudeRef",
        "Exif.GPSInfo.GPSAltitude"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_exif_keys(exifData, keys, n_keys);

      GList *res = dt_metadata_get(imgid, "Xmp.dc.creator", NULL);
      if(!IS_NULL_PTR(res))
      {
        exifData["Exif.Image.Artist"] = (char *)res->data;
        g_list_free_full(res, dt_free_gpointer);
        res = NULL;
      }

      res = dt_metadata_get(imgid, "Xmp.dc.description", NULL);
      if(!IS_NULL_PTR(res))
      {
        char *desc = (char *)res->data;
        if(g_str_is_ascii(desc))
          exifData["Exif.Image.ImageDescription"] = desc;
        else
          exifData["Exif.Photo.UserComment"] = desc;
        g_list_free_full(res, dt_free_gpointer);
        res = NULL;
      }
#if EXIV2_TEST_VERSION(0,27,4)
      else
        // mandatory tag for TIFF/EP and recommended for Exif, empty is ok (unknown)
        // but correctly written only by exiv2 >= 0.27.4
        exifData["Exif.Image.ImageDescription"] = "";
#endif

      res = dt_metadata_get(imgid, "Xmp.dc.rights", NULL);
      if(!IS_NULL_PTR(res))
      {
        exifData["Exif.Image.Copyright"] = (char *)res->data;
        g_list_free_full(res, dt_free_gpointer);
        res = NULL;
      }
#if EXIV2_TEST_VERSION(0,27,4)
      else
        // mandatory tag for TIFF/EP and optional for Exif, empty is ok (unknown)
        // but correctly written only by exiv2 >= 0.27.4
        exifData["Exif.Image.Copyright"] = "";
#endif

      res = dt_metadata_get(imgid, "Xmp.xmp.Rating", NULL);
      if(!IS_NULL_PTR(res))
      {
        const int rating = GPOINTER_TO_INT(res->data) + 1;
        exifData["Exif.Image.Rating"] = rating;
        g_list_free(res);
        res = NULL;
      }

      // GPS data
      dt_remove_exif_geotag(exifData);
      const dt_image_t *cimg = dt_image_cache_get(imgid, 'r');
      if(!isnan(cimg->geoloc.longitude) && !isnan(cimg->geoloc.latitude))
      {
        exifData["Exif.GPSInfo.GPSVersionID"] = "02 02 00 00";
        exifData["Exif.GPSInfo.GPSLongitudeRef"] = (cimg->geoloc.longitude < 0) ? "W" : "E";
        exifData["Exif.GPSInfo.GPSLatitudeRef"] = (cimg->geoloc.latitude < 0) ? "S" : "N";

        long long_deg = (int)floor(fabs(cimg->geoloc.longitude));
        long lat_deg = (int)floor(fabs(cimg->geoloc.latitude));
        long long_min = (int)floor((fabs(cimg->geoloc.longitude) - floor(fabs(cimg->geoloc.longitude))) * 60000000);
        long lat_min = (int)floor((fabs(cimg->geoloc.latitude) - floor(fabs(cimg->geoloc.latitude))) * 60000000);
        gchar *long_str = g_strdup_printf("%ld/1 %ld/1000000 0/1", long_deg, long_min);
        gchar *lat_str = g_strdup_printf("%ld/1 %ld/1000000 0/1", lat_deg, lat_min);
        exifData["Exif.GPSInfo.GPSLongitude"] = long_str;
        exifData["Exif.GPSInfo.GPSLatitude"] = lat_str;
        dt_free(long_str);
        dt_free(lat_str);
      }
      if(!isnan(cimg->geoloc.elevation))
      {
        exifData["Exif.GPSInfo.GPSVersionID"] = "02 02 00 00";
        exifData["Exif.GPSInfo.GPSAltitudeRef"] = (cimg->geoloc.elevation < 0) ? "1" : "0";

        long ele_dm = (int)floor(fabs(10.0 * cimg->geoloc.elevation));
        gchar *ele_str = g_strdup_printf("%ld/10", ele_dm);
        exifData["Exif.GPSInfo.GPSAltitude"] = ele_str;
        dt_free(ele_str);
      }

      // According to the Exif specs DateTime is to be set to the last modification time while
      // DateTimeOriginal is to be kept.
      // For us "keeping" it means to write out what we have in DB to support people adding a time offset in
      // the geotagging module.
      gchar new_datetime[DT_DATETIME_EXIF_LENGTH];
      dt_datetime_now_to_exif(new_datetime);
      exifData["Exif.Image.DateTime"] = new_datetime;
      gchar datetime[DT_DATETIME_LENGTH];
      dt_datetime_img_to_exif(datetime, sizeof(datetime), cimg);
      datetime[DT_DATETIME_EXIF_LENGTH - 1] = '\0';
      exifData["Exif.Image.DateTimeOriginal"] = datetime;
      exifData["Exif.Photo.DateTimeOriginal"] = datetime;
      if(g_strcmp0(&datetime[DT_DATETIME_EXIF_LENGTH], "000"))
        exifData["Exif.Photo.SubSecTimeOriginal"] = &datetime[DT_DATETIME_EXIF_LENGTH];

      dt_image_cache_read_release(cimg);
    }

    Exiv2::Blob blob;
    Exiv2::ExifParser::encode(blob, Exiv2::bigEndian, exifData);
    const size_t length = blob.size();
    *buf = (uint8_t *)malloc(length);
    if (IS_NULL_PTR(*buf))
    {
      return 0;
    }
    memcpy(*buf, &(blob[0]), length);
    return length;
  }
  catch(const std::exception &e)
  {
    // std::cerr.rdbuf(savecerr);
    std::string s(e.what());
    std::cerr << "[exiv2 dt_exif_read_blob] " << path << ": " << s << std::endl;
    dt_free(*buf);
    return 0;
  }
}

typedef struct history_entry_t
{
  char *operation;
  gboolean enabled;
  int modversion;
  unsigned char *params;
  int params_len;
  char *multi_name;
  // int multi_name_hand_edited;
  int multi_priority;
  int blendop_version;
  unsigned char *blendop_params;
  int blendop_params_len;
  int num;
  double iop_order; // kept for compatibility with xmp version < 4

  // sanity checking
  gboolean have_operation, have_params, have_modversion;
} history_entry_t;

// used for a hash table that maps mask_id to the mask data
typedef struct mask_entry_t
{
  int mask_id;
  int mask_type;
  char *mask_name;
  int mask_version;
  unsigned char *mask_points;
  int mask_points_len;
  int mask_nb;
  unsigned char *mask_src;
  int mask_src_len;
  gboolean already_added;
  int mask_num;
  int version;
} mask_entry_t;

static void print_history_entry(history_entry_t *entry) __attribute__((unused));
static void print_history_entry(history_entry_t *entry)
{
  if(IS_NULL_PTR(entry) || IS_NULL_PTR(entry->operation))
  {
    std::cout << "malformed entry" << std::endl;
    return;
  }

  std::cout << entry->operation << std::endl;
  std::cout << "  modversion      :" <<  entry->modversion                                    << std::endl;
  std::cout << "  enabled         :" <<  entry->enabled                                       << std::endl;
  std::cout << "  params          :" << (entry->params ? "<found>" : "<missing>")             << std::endl;
  std::cout << "  multi_name      :" << (entry->multi_name ? entry->multi_name : "<missing>") << std::endl;
  std::cout << "  multi_priority  :" <<  entry->multi_priority                                << std::endl;
  std::cout << "  iop_order       :" << entry->iop_order                                      << std::endl;
  std::cout << "  blendop_version :" <<  entry->blendop_version                               << std::endl;
  std::cout << "  blendop_params  :" << (entry->blendop_params ? "<found>" : "<missing>")     << std::endl;
  std::cout << std::endl;
}

static void free_history_entry(gpointer data)
{
  history_entry_t *entry = (history_entry_t *)data;
  dt_free(entry->operation);
  dt_free(entry->multi_name);
  dt_free(entry->params);
  dt_free(entry->blendop_params);
  dt_free(entry);
}

// we have to use pugixml as the old format could contain empty rdf:li elements in the multi_name array
// which causes problems when accessing it with libexiv2 :(
// superold is a flag indicating that data is wrapped in <rdf:Bag> instead of <rdf:Seq>.
static GList *read_history_v1(const std::string &xmpPacket, const char *filename, const int superold)
{
  GList *history_entries = NULL;

  pugi::xml_document doc;
#if defined(PUGIXML_VERSION) && PUGIXML_VERSION >= 150
  pugi::xml_parse_result result = doc.load_string(xmpPacket.c_str());
#else
  pugi::xml_parse_result result = doc.load(xmpPacket.c_str());
#endif

  if(!result)
  {
    std::cerr << "XML '" << filename << "' parsed with errors" << std::endl;
    std::cerr << "Error description: " << result.description() << std::endl;
    std::cerr << "Error offset: " << result.offset << std::endl;
    return NULL;
  }

  // get the old elements
  // select_single_node() is deprecated and just kept for old versions shipped in some distributions
#if defined(PUGIXML_VERSION) && PUGIXML_VERSION >= 150
  pugi::xpath_node modversion      = superold ?
    doc.select_node("//darktable:history_modversion/rdf:Bag"):
    doc.select_node("//darktable:history_modversion/rdf:Seq");
  pugi::xpath_node enabled         = superold ?
    doc.select_node("//darktable:history_enabled/rdf:Bag"):
    doc.select_node("//darktable:history_enabled/rdf:Seq");
  pugi::xpath_node operation       = superold ?
    doc.select_node("//darktable:history_operation/rdf:Bag"):
    doc.select_node("//darktable:history_operation/rdf:Seq");
  pugi::xpath_node params          = superold ?
    doc.select_node("//darktable:history_params/rdf:Bag"):
    doc.select_node("//darktable:history_params/rdf:Seq");
  pugi::xpath_node blendop_params  = superold ?
    doc.select_node("//darktable:blendop_params/rdf:Bag"):
    doc.select_node("//darktable:blendop_params/rdf:Seq");
  pugi::xpath_node blendop_version = superold ?
    doc.select_node("//darktable:blendop_version/rdf:Bag"):
    doc.select_node("//darktable:blendop_version/rdf:Seq");
  pugi::xpath_node multi_priority  = superold ?
    doc.select_node("//darktable:multi_priority/rdf:Bag"):
    doc.select_node("//darktable:multi_priority/rdf:Seq");
  pugi::xpath_node multi_name      = superold ?
    doc.select_node("//darktable:multi_name/rdf:Bag"):
    doc.select_node("//darktable:multi_name/rdf:Seq");
#else
  pugi::xpath_node modversion      = superold ?
    doc.select_single_node("//darktable:history_modversion/rdf:Bag"):
    doc.select_single_node("//darktable:history_modversion/rdf:Seq");
  pugi::xpath_node enabled         = superold ?
    doc.select_single_node("//darktable:history_enabled/rdf:Bag"):
    doc.select_single_node("//darktable:history_enabled/rdf:Seq");
  pugi::xpath_node operation       = superold ?
    doc.select_single_node("//darktable:history_operation/rdf:Bag"):
    doc.select_single_node("//darktable:history_operation/rdf:Seq");
  pugi::xpath_node params          = superold ?
    doc.select_single_node("//darktable:history_params/rdf:Bag"):
    doc.select_single_node("//darktable:history_params/rdf:Seq");
  pugi::xpath_node blendop_params  = superold ?
    doc.select_single_node("//darktable:blendop_params/rdf:Bag"):
    doc.select_single_node("//darktable:blendop_params/rdf:Seq");
  pugi::xpath_node blendop_version = superold ?
    doc.select_single_node("//darktable:blendop_version/rdf:Bag"):
    doc.select_single_node("//darktable:blendop_version/rdf:Seq");
  pugi::xpath_node multi_priority  = superold ?
    doc.select_single_node("//darktable:multi_priority/rdf:Bag"):
    doc.select_single_node("//darktable:multi_priority/rdf:Seq");
  pugi::xpath_node multi_name      = superold ?
    doc.select_single_node("//darktable:multi_name/rdf:Bag"):
    doc.select_single_node("//darktable:multi_name/rdf:Seq");
#endif

  // fill the list of history entries. we are iterating over history_operation as we know that it's there.
  // the other iters are taken care of manually.
  auto modversion_iter = modversion.node().children().begin();
  auto enabled_iter = enabled.node().children().begin();
  auto params_iter = params.node().children().begin();
  auto blendop_params_iter = blendop_params.node().children().begin();
  auto blendop_version_iter = blendop_version.node().children().begin();
  auto multi_priority_iter = multi_priority.node().children().begin();
  auto multi_name_iter = multi_name.node().children().begin();

  for(pugi::xml_node operation_iter: operation.node().children())
  {
    history_entry_t *current_entry = (history_entry_t *)calloc(1, sizeof(history_entry_t));
    current_entry->blendop_version = 1; // default version in case it's not specified
    history_entries = g_list_append(history_entries, current_entry);

    current_entry->operation = g_strdup(operation_iter.child_value());

    current_entry->enabled = g_strcmp0(enabled_iter->child_value(), "0") != 0;

    current_entry->modversion = atoi(modversion_iter->child_value());

    const char *params_text = params_iter->child_value();
    current_entry->params = dt_exif_xmp_decode(params_text, strlen(params_text),
                                               &current_entry->params_len);

    if(multi_name && multi_name_iter != multi_name.node().children().end())
    {
      current_entry->multi_name = g_strdup(multi_name_iter->child_value());
      multi_name_iter++;
    }

    if(multi_priority && multi_priority_iter != multi_priority.node().children().end())
    {
      current_entry->multi_priority = atoi(multi_priority_iter->child_value());
      multi_priority_iter++;
    }

    if(blendop_version && blendop_version_iter != blendop_version.node().children().end())
    {
      current_entry->blendop_version = atoi(blendop_version_iter->child_value());
      blendop_version_iter++;
    }

    if(blendop_params && blendop_params_iter != blendop_params.node().children().end())
    {
      const char *blendop_text = blendop_params_iter->child_value();
      current_entry->blendop_params = dt_exif_xmp_decode(blendop_text, strlen(blendop_text),
                                                         &current_entry->blendop_params_len);
      blendop_params_iter++;
    }

    current_entry->iop_order = -1.0;

    modversion_iter++;
    enabled_iter++;
    params_iter++;
  }

  return history_entries;
}

static GList *read_history_v2(Exiv2::XmpData &xmpData, const char *filename)
{
  GList *history_entries = NULL;
  history_entry_t *current_entry = NULL;

  for(auto history = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history")); history != xmpData.end(); history++)
  {
    // TODO: support human readable params via introspection with something like this:
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:name = width
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:value = 23

    std::string key_item = history->key();
    char *key = g_strdup(key_item.c_str());
    char *key_iter = key;
    if(g_str_has_prefix(key, "Xmp.darktable.history["))
    {
      key_iter += strlen("Xmp.darktable.history[");
      errno = 0;
      unsigned int n = strtol(key_iter, &key_iter, 10);
      if(errno)
      {
        std::cerr << "error reading history from '" << key << "' (" << filename << ")" << std::endl;
        g_list_free_full(history_entries, free_history_entry);
        history_entries = NULL;
        dt_free(key);
        return NULL;
      }

      // skip everything that isn't part of the actual array
      if(*(key_iter++) != ']')
      {
        std::cerr << "error reading history from '"
                  << key << "' (" << filename << ")" << std::endl;
        g_list_free_full(history_entries, free_history_entry);
        history_entries = NULL;
        dt_free(key);
        return NULL;
      }
      if(*(key_iter++) != '/') goto skip;
      if(*key_iter == '?') key_iter++;

      // make sure we are filling in the details of the correct entry
      unsigned int length = g_list_length(history_entries);
      if(n > length)
      {
        current_entry = (history_entry_t *)calloc(1, sizeof(history_entry_t));
        current_entry->blendop_version = 1; // default version in case it's not specified
        current_entry->iop_order = -1.0;
        history_entries = g_list_append(history_entries, current_entry);
      }
      else if(n < length)
      {
        // AFAICT this can't happen with regular exiv2 parsed XMP data, but better safe than sorry.
        // it can happen though when constructing things in a unusual order and then passing it to us without
        // serializing it in between
        current_entry = (history_entry_t *)g_list_nth_data(history_entries, n - 1); // XMP starts counting at 1!
      }

      // go on reading things into current_entry
      if(g_str_has_prefix(key_iter, "darktable:operation"))
      {
        current_entry->have_operation = TRUE;
        std::string value_item = history->toString();
        current_entry->operation = g_strdup(value_item.c_str());
      }
      else if(g_str_has_prefix(key_iter, "darktable:num"))
      {
        current_entry->num = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:enabled"))
      {
        current_entry->enabled = history->toLong() == 1;
      }
      else if(g_str_has_prefix(key_iter, "darktable:modversion"))
      {
        current_entry->have_modversion = TRUE;
        current_entry->modversion = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:params"))
      {
        current_entry->have_params = TRUE;
        std::string value_item = history->toString();
        current_entry->params = dt_exif_xmp_decode(value_item.c_str(),
                                                   history->size(),
                                                   &current_entry->params_len);
      }
      /*
      else if(g_str_has_prefix(key_iter, "darktable:multi_name_hand_edited"))
      {
        current_entry->multi_name_hand_edited = history->toLong() == 1;
      }
      */
      else if(g_str_has_prefix(key_iter, "darktable:multi_name"))
      {
        std::string value_item = history->toString();
        current_entry->multi_name = g_strdup(value_item.c_str());
      }
      else if(g_str_has_prefix(key_iter, "darktable:multi_priority"))
      {
        current_entry->multi_priority = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:iop_order"))
      {
        // we ensure reading the iop_order as a high precision float
        std::string value_item = history->toString();
        string str = g_strdup(value_item.c_str());
        static const std::locale& c_locale = std::locale("C");
        std::istringstream istring(str);
        istring.imbue(c_locale);
        istring >> current_entry->iop_order;
      }
      else if(g_str_has_prefix(key_iter, "darktable:blendop_version"))
      {
        current_entry->blendop_version = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:blendop_params"))
      {
        std::string value_item = history->toString();
        current_entry->blendop_params =
          dt_exif_xmp_decode(value_item.c_str(),
                                                           history->size(),
                                                           &current_entry->blendop_params_len);
      }
    }
skip:
    dt_free(key);
  }

  // a final sanity check
  for(GList *iter = history_entries; iter; iter = g_list_next(iter))
  {
    history_entry_t *entry = (history_entry_t *)iter->data;
    if(!(entry->have_operation && entry->have_params && entry->have_modversion))
    {
      std::cerr << "[exif] error: reading history from '" << filename << "' failed due to missing tags" << std::endl;
      g_list_free_full(history_entries, free_history_entry);
      history_entries = NULL;
      break;
    }
  }

  return history_entries;
}

void free_mask_entry(gpointer data)
{
  mask_entry_t *entry = (mask_entry_t *)data;
  dt_free(entry->mask_name);
  dt_free(entry->mask_points);
  dt_free(entry->mask_src);
  dt_free(entry);
}

static GHashTable *read_masks(Exiv2::XmpData &xmpData, const char *filename, const int version)
{
  GHashTable *mask_entries = g_hash_table_new_full(g_int_hash, g_int_equal, NULL, free_mask_entry);

  // TODO: turn that into something like Xmp.darktable.history!
  Exiv2::XmpData::iterator mask;
  Exiv2::XmpData::iterator mask_name;
  Exiv2::XmpData::iterator mask_type;
  Exiv2::XmpData::iterator mask_version;
  Exiv2::XmpData::iterator mask_id;
  Exiv2::XmpData::iterator mask_nb;
  Exiv2::XmpData::iterator mask_src;
  if((mask = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask"))) != xmpData.end()
    && (mask_src = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_src"))) != xmpData.end()
    && (mask_name = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_name"))) != xmpData.end()
    && (mask_type = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_type"))) != xmpData.end()
    && (mask_version = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_version"))) != xmpData.end()
    && (mask_id = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_id"))) != xmpData.end()
    && (mask_nb = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.mask_nb"))) != xmpData.end())
  {
    // fixes API change happened after exiv2 v0.27.2.1
    const size_t cnt = (size_t)mask->count();
    const size_t mask_src_cnt = (size_t)mask_src->count();
    const size_t mask_name_cnt = (size_t)mask_name->count();
    const size_t mask_type_cnt = (size_t)mask_type->count();
    const size_t mask_version_cnt = (size_t)mask_version->count();
    const size_t mask_id_cnt = (size_t)mask_id->count();
    const size_t mask_nb_cnt = (size_t)mask_nb->count();
    if(cnt == mask_src_cnt && cnt == mask_name_cnt && cnt == mask_type_cnt
       && cnt == mask_version_cnt && cnt == mask_id_cnt && cnt == mask_nb_cnt)
    {
      for(size_t i = 0; i < cnt; i++)
      {
        mask_entry_t *entry = (mask_entry_t *)calloc(1, sizeof(mask_entry_t));

        entry->version = version;
        entry->mask_id = mask_id->toLong(i);
        entry->mask_type = mask_type->toLong(i);
        std::string mask_name_str = mask_name->toString(i);
        if(mask_name_str.c_str() != NULL)
          entry->mask_name = g_strdup(mask_name_str.c_str());
        else
          entry->mask_name = g_strdup("form");

        entry->mask_version = mask_version->toLong(i);

        std::string mask_str = mask->toString(i);
        const char *mask_c = mask_str.c_str();
        const size_t mask_c_len = mask_str.size();
        entry->mask_points = dt_exif_xmp_decode(mask_c, mask_c_len, &entry->mask_points_len);

        entry->mask_nb = mask_nb->toLong(i);

        std::string mask_src_str = mask_src->toString(i);
        const char *mask_src_c = mask_src_str.c_str();
        const size_t mask_src_c_len = mask_src_str.size();
        entry->mask_src = dt_exif_xmp_decode(mask_src_c, mask_src_c_len, &entry->mask_src_len);

        g_hash_table_insert(mask_entries, &entry->mask_id, (gpointer)entry);
      }
    }
  }

  return mask_entries;
}

static GList *read_masks_v3(Exiv2::XmpData &xmpData, const char *filename, const int version)
{
  GList *history_entries = NULL;
  mask_entry_t *current_entry = NULL;

  for(auto history = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.masks_history")); history != xmpData.end(); history++)
  {
    // TODO: support human readable params via introspection with something like this:
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:name = width
    // XmpText: Xmp.darktable.history[1]/darktable:settings[1]/darktable:value = 23

    std::string key_item = history->key();
    char *key = g_strdup(key_item.c_str());
    char *key_iter = key;
    if(g_str_has_prefix(key, "Xmp.darktable.masks_history["))
    {
      key_iter += strlen("Xmp.darktable.masks_history[");
      errno = 0;
      unsigned int n = strtol(key_iter, &key_iter, 10);
      if(errno)
      {
        std::cerr << "error reading masks history from '" << key << "' (" << filename << ")" << std::endl;
        g_list_free_full(history_entries, free_mask_entry);
        history_entries = NULL;
        dt_free(key);
        return NULL;
      }

      // skip everything that isn't part of the actual array
      if(*(key_iter++) != ']')
      {
        std::cerr << "error reading masks history from '" << key << "' (" << filename << ")" << std::endl;
        g_list_free_full(history_entries, free_mask_entry);
        history_entries = NULL;
        dt_free(key);
        return NULL;
      }
      if(*(key_iter++) != '/') goto skip;
      if(*key_iter == '?') key_iter++;

      // make sure we are filling in the details of the correct entry
      unsigned int length = g_list_length(history_entries);
      if(n > length)
      {
        current_entry = (mask_entry_t *)calloc(1, sizeof(mask_entry_t));
        current_entry->version = version;
        history_entries = g_list_append(history_entries, current_entry);
      }
      else if(n < length)
      {
        // AFAICT this can't happen with regular exiv2 parsed XMP data, but better safe than sorry.
        // it can happen though when constructing things in a unusual order and then passing it to us without
        // serializing it in between
        current_entry = (mask_entry_t *)g_list_nth_data(history_entries, n - 1); // XMP starts counting at 1!
      }

      // go on reading things into current_entry
      if(g_str_has_prefix(key_iter, "darktable:mask_num"))
      {
        current_entry->mask_num = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_id"))
      {
        current_entry->mask_id = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_type"))
      {
        current_entry->mask_type = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_name"))
      {
        std::string value_item = history->toString();
        current_entry->mask_name = g_strdup(value_item.c_str());
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_version"))
      {
        current_entry->mask_version = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_points"))
      {
        std::string value_item = history->toString();
        current_entry->mask_points = dt_exif_xmp_decode(value_item.c_str(),
                                                        history->size(),
                                                        &current_entry->mask_points_len);
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_nb"))
      {
        current_entry->mask_nb = history->toLong();
      }
      else if(g_str_has_prefix(key_iter, "darktable:mask_src"))
      {
        std::string value_item = history->toString();
        current_entry->mask_src = dt_exif_xmp_decode(value_item.c_str(),
                                                     history->size(),
                                                     &current_entry->mask_src_len);
      }

    }
skip:
    dt_free(key);
  }

  return history_entries;
}

static void add_mask_entry_to_db(int32_t imgid, mask_entry_t *entry)
{
  // add the mask entry only once
  if(entry->already_added)
    return;

  const int mask_num = 0;

  const int mask_num_used = (entry->version < 3) ? mask_num : entry->mask_num;
  const gboolean inserted =
      dt_history_repository_write_mask_item(imgid, mask_num_used, entry->mask_id, entry->mask_type,
                                            entry->mask_name, entry->mask_version, entry->mask_points,
                                            entry->mask_points_len, entry->mask_nb, entry->mask_src,
                                            entry->mask_src_len);

  // Mark entry to true only after confirmation the sql insert was successful.
  //
  // This used to compare sqlite3_step()'s result against SQLITE_OK. An INSERT that runs to
  // completion returns SQLITE_DONE, so the test was never true, the flag was never set, and the
  // guard at the top of this function never fired. For a legacy sidecar (xmp_version < 3) every
  // non-clone mask is reached TWICE -- once by the hash-table pass over every entry, and again
  // by the per-history-item recursion through its group -- and once more per extra module
  // sharing that group. Each of those wrote another row into main.masks_history.
  if(inserted)
  {
    entry->already_added = TRUE;
  }
}

static void add_non_clone_mask_entries_to_db(gpointer key, gpointer value, gpointer user_data)
{
  int32_t imgid = *(int *)user_data;
  mask_entry_t *entry = (mask_entry_t *)value;
  if(!(entry->mask_type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))) add_mask_entry_to_db(imgid, entry);
}

static void add_mask_entries_to_db(int32_t imgid, GHashTable *mask_entries, int mask_id)
{
  if(mask_id <= 0) return;

  // look for mask_id in the hash table
  mask_entry_t *entry = (mask_entry_t *)g_hash_table_lookup(mask_entries, &mask_id);

  if(IS_NULL_PTR(entry)) return;

  // if it's a group: recurse into the children first
  if(entry->mask_type & DT_MASKS_GROUP)
  {
    dt_masks_form_group_t *group = (dt_masks_form_group_t *)entry->mask_points;
    if((int)(entry->mask_nb * sizeof(dt_masks_form_group_t)) != entry->mask_points_len)
    {
      fprintf(stderr, "[masks] error loading masks from xmp file, bad binary blob size.\n");
      return;
    }
    for(int i = 0; i < entry->mask_nb; i++)
      add_mask_entries_to_db(imgid, mask_entries, group[i].formid);
  }

  add_mask_entry_to_db(imgid, entry);
}

// get MAX multi_priority
int _get_max_multi_priority(GList *history, const char *operation)
{
  int max_prio = 0;

  for(GList *iter = history; iter; iter = g_list_next(iter))
  {
    history_entry_t *entry = (history_entry_t *)iter->data;

    if(!strcmp(entry->operation, operation))
      max_prio = MAX(max_prio, entry->multi_priority);
  }

  return max_prio;
}

// need a write lock on *img (non-const) to write stars (and soon color labels).
int dt_exif_xmp_read(dt_image_t *img, const char *filename, const int history_only)
{
  // Neither argument was checked, and this is a public entry point: common/image.c passes
  // a dt_image_cache_get() result straight in, which is nullable everywhere else in the
  // tree. Non-zero is this function's existing "did not read it" answer, and every caller
  // already tests for it.
  if(IS_NULL_PTR(img) || IS_NULL_PTR(filename)) return 1;

  // exclude pfm to avoid stupid errors on the console.
  // The length is checked BEFORE the pointer is formed: `filename + strlen(filename) - 4'
  // on a name shorter than four characters computes a pointer before the start of the
  // array, which is undefined behaviour -- only one-past-the-end is legal. The `c >=
  // filename' test that used to follow cannot rescue that; the pointer is already invalid.
  const size_t filename_len = strlen(filename);
  if(filename_len >= 4 && !strcmp(filename + filename_len - 4, ".pfm")) return 1;
  try
  {
    // read xmp sidecar
    std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(WIDEN(filename)));
    if(!image.get()) return 1;
    read_metadata_threadsafe(image);
    Exiv2::XmpData &xmpData = image->xmpData();

    Exiv2::XmpData::iterator pos;

    int xmp_version = 0;
    GList *iop_order_list = NULL;
    dt_iop_order_t iop_order_version = DT_IOP_ORDER_LEGACY;

    int num_masks = 0;
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.xmp_version"))) != xmpData.end())
      xmp_version = pos->toLong();

    if(!history_only)
    {
      // otherwise we ignore title, description, ... from non-dt xmp files :(
      const size_t ns_pos = image->xmpPacket().find("xmlns:darktable=\"http://darktable.sf.net/\"");
      const bool is_a_dt_xmp = (ns_pos != std::string::npos);
      dt_exif_decode_xmp_data(img, xmpData, is_a_dt_xmp ? xmp_version : -1, false);
    }


    // convert legacy flip bits (will not be written anymore, convert to flip history item here):
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.raw_params"))) != xmpData.end())
    {
      union {
          int32_t in;
          dt_image_raw_parameters_t out;
      } raw_params;
      raw_params.in = pos->toLong();
      const int32_t user_flip = raw_params.out.user_flip;
      img->legacy_flip.user_flip = user_flip;
      img->legacy_flip.legacy = 0;
    }

    int32_t preset_applied = 0;

    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.auto_presets_applied"))) != xmpData.end())
    {
      preset_applied = pos->toLong();

      // in any case, this is no legacy image.
      img->flags |= DT_IMAGE_NO_LEGACY_PRESETS;
    }
    else if(xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.xmp_version")) == xmpData.end())
    {
      // if there is no darktable xmp_version in the XMP, this XMP must have been generated by another
      // program; since this is the first time darktable sees it, there can't be legacy presets
      img->flags |= DT_IMAGE_NO_LEGACY_PRESETS;
    }
    else
    {
      // so we are legacy (thus have to clear the no-legacy flag)
      img->flags &= ~DT_IMAGE_NO_LEGACY_PRESETS;
    }
    // when we are reading the xmp data it doesn't make sense to flag the image as removed
    img->flags &= ~DT_IMAGE_REMOVE;

    if(xmp_version == 4 || xmp_version == 5)
    {
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.iop_order_version"))) != xmpData.end())
      {
        iop_order_version = (dt_iop_order_t)pos->toLong();
      }

      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.iop_order_list"))) != xmpData.end())
      {
        iop_order_list = dt_ioppr_deserialize_text_iop_order_list(pos->toString().c_str());
        // insert modules created after this edit's order was serialized, so
        // their history entries land in the right pipeline position
        if(iop_order_list) iop_order_list = dt_ioppr_insert_missing_modules(iop_order_list);
      }
      else
        iop_order_list = dt_ioppr_get_iop_order_list_version(iop_order_version);
    }
    else if(xmp_version == 3)
    {
      iop_order_version = DT_IOP_ORDER_LEGACY;

      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.iop_order_version"))) != xmpData.end())
      {
        //  All iop-order version before 3 are legacy one. Starting with version 3 we have the first
        //  attempts to propose the final v3 iop-order.
        iop_order_version = pos->toLong() < 3 ? DT_IOP_ORDER_LEGACY : DT_IOP_ORDER_ANSEL_RAW;
        iop_order_list = dt_ioppr_get_iop_order_list_version(iop_order_version);
      }
      else
        iop_order_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
    }
    else
    {
      iop_order_version = DT_IOP_ORDER_LEGACY;
      iop_order_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
    }

    // masks
    GHashTable *mask_entries = NULL;
    GList *mask_entries_v3 = NULL;

    // clean all old masks for this image
    dt_history_repository_delete_masks_history(img->id);

    // read the masks from the file first so we can add them to the db while reading history entries
    if(xmp_version < 3)
      mask_entries = read_masks(xmpData, filename, xmp_version);
    else
      mask_entries_v3 = read_masks_v3(xmpData, filename, xmp_version);

    // now add all masks that are not used for cloning. keeping them might be useful.
    // TODO: make this configurable? or remove it altogether?
    dt_database_start_transaction();

    if(xmp_version < 3)
    {
      g_hash_table_foreach(mask_entries, add_non_clone_mask_entries_to_db, &img->id);
    }
    else
    {
      for(GList *m_entries = g_list_first(mask_entries_v3); m_entries; m_entries = g_list_next(m_entries))
      {
        mask_entry_t *mask_entry = (mask_entry_t *)m_entries->data;

        add_mask_entry_to_db(img->id, mask_entry);
      }
    }

    dt_database_release_transaction();

    // history
    int num = 0;
    gboolean all_ok = TRUE;
    GList *history_entries = NULL;

    if(xmp_version < 2)
    {
      std::string &xmpPacket = image->xmpPacket();
      history_entries = read_history_v1(xmpPacket, filename, 0);
      if(!history_entries) // didn't work? try super old version with rdf:Bag
        history_entries = read_history_v1(xmpPacket, filename, 1);
    }
    else if(xmp_version == 2 || xmp_version == 3 || xmp_version == 4 || xmp_version == 5 )
      history_entries = read_history_v2(xmpData, filename);
    else
    {
      std::cerr << "error: Xmp schema version " << xmp_version << " in " << filename << " not supported" << std::endl;
      g_hash_table_destroy(mask_entries);
      return 1;
    }

    dt_database_start_transaction();

    if(!dt_history_repository_delete_history(img->id))
    {
      fprintf(stderr, "[exif] error deleting history for image %d\n", img->id);
      fprintf(stderr, "[exif]   %s\n", dt_database_get_last_error());
      all_ok = FALSE;
      goto end;
    }

    for(GList *iter = history_entries; iter; iter = g_list_next(iter))
    {
      history_entry_t *entry = (history_entry_t *)iter->data;
      const int db_num = (xmp_version < 3) ? num : entry->num;

      if(entry->blendop_params)
      {
        if(xmp_version < 3)
        {
          // check what mask entries belong to this iop and add them to the db
          const dt_develop_blend_params_t *blendop_params = (dt_develop_blend_params_t *)entry->blendop_params;
          add_mask_entries_to_db(img->id, mask_entries, blendop_params->mask_id);
        }
      }

      if(!dt_history_repository_write_item(img->id, db_num, entry->operation,
                                           entry->params, entry->params_len,
                                           entry->modversion, entry->enabled != 0,
                                           entry->blendop_params, entry->blendop_params_len,
                                           entry->blendop_version, entry->multi_priority,
                                           entry->multi_name ? entry->multi_name : ""))
      {
        fprintf(stderr, "[exif] error adding history entry for image %d\n", img->id);
        fprintf(stderr, "[exif]   %s\n", dt_database_get_last_error());
        all_ok = FALSE;
        goto end;
      }

      num++;
    }

    // we now need to create and store the proper iop-order taking into account all multi-instances
    // for previous xmp versions.

    if(xmp_version < 4)
    {
      // in this version we had iop-order, use it

      for(GList *iter = history_entries; iter; iter = g_list_next(iter))
      {
        history_entry_t *entry = (history_entry_t *)iter->data;

        dt_iop_order_entry_t *e = (dt_iop_order_entry_t *)malloc(sizeof(dt_iop_order_entry_t));
        memcpy(e->operation, entry->operation, sizeof(e->operation));
        e->instance = entry->multi_priority;

        if(xmp_version < 3)
        {
          // prior to v3 there was no iop-order, all multi instances where grouped, use the multièpriority
          // to restore the order.
          GList *base_order = dt_ioppr_get_iop_order_link(iop_order_list, entry->operation, -1);

          if(base_order)
            e->o.iop_order_f = ((dt_iop_order_entry_t *)(base_order->data))->o.iop_order_f
              - entry->multi_priority / 100.0f;
          else if(!dt_deprecated(entry->operation))
          {
            fprintf(stderr,
                    "[exif] cannot get iop-order for module '%s', XMP may be corrupted\n",
                    entry->operation);
            g_list_free_full(iop_order_list, dt_free_gpointer);
            iop_order_list = NULL;
            g_list_free_full(history_entries, free_history_entry);
            history_entries = NULL;
            g_list_free_full(mask_entries_v3, free_mask_entry);
            mask_entries_v3 = NULL;
            if(mask_entries) g_hash_table_destroy(mask_entries);
            dt_free(e);
            return 1;
          }
        }
        else
        {
          // otherwise use the iop_order for the entry
          e->o.iop_order_f = entry->iop_order; // legacy iop-order is used to insert item at the right location
        }

        // remove a current entry from the iop-order list if found as it will be replaced, possibly with another iop-order
        // with a new item in the history.

        GList *link = dt_ioppr_get_iop_order_link(iop_order_list, e->operation, e->instance);
        if(link) iop_order_list = g_list_delete_link(iop_order_list, link);

        iop_order_list = g_list_append(iop_order_list, e);
      }

      // and finally reorder the full list based on the iop-order

      iop_order_list = g_list_sort(iop_order_list, dt_sort_iop_list_by_order_f);
    }

    // if masks have been read, create a mask manager entry in history
    if(xmp_version < 3)
    {
      num_masks = dt_history_repository_count_mask_items(img->id);

      if(num_masks > 0)
      {
        // make room for mask_manager entry
        if(!dt_history_repository_shift_nums(img->id, 1))
        {
          fprintf(stderr, "[exif] error shifting history nums for image %d\n", img->id);
          all_ok = FALSE;
          goto end;
        }
        if(!dt_history_repository_write_item(img->id, 0, "mask_manager", NULL, 0, 1, 0, NULL, 0, 0, 0, ""))
        {
          fprintf(stderr, "[exif] error adding mask history entry for image %d\n", img->id);
          fprintf(stderr, "[exif]   %s\n", dt_database_get_last_error());
          all_ok = FALSE;
          goto end;
        }

        num++;
      }
    }

    // we shouldn't change history_end when no history was read!
    if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_end"))) != xmpData.end() && num > 0)
    {
      int history_end = MIN(pos->toLong(), num);
      if(num_masks > 0) history_end++;
      if((history_end < 1) && preset_applied) preset_applied = -1;
      if(!dt_history_repository_set_end(img->id, history_end))
      {
        fprintf(stderr, "[exif] error writing history_end for image %d\n", img->id);
        fprintf(stderr, "[exif]   %s\n", dt_database_get_last_error());
        all_ok = FALSE;
        goto end;
      }
    }
    else
    {
      if(preset_applied) preset_applied = -1;
      const int32_t history_end = dt_history_repository_get_next_num(img->id);
      if(!dt_history_repository_set_end(img->id, history_end))
      {
        fprintf(stderr, "[exif] error writing history_end for image %d\n", img->id);
        fprintf(stderr, "[exif]   %s\n", dt_database_get_last_error());
        all_ok = FALSE;
        goto end;
      }
    }
    if(!dt_ioppr_write_iop_order_list(iop_order_list, img->id))
    {
      fprintf(stderr, "[exif] error writing iop_list for image %d\n", img->id);
      fprintf(stderr, "[exif]   %s\n", dt_database_get_last_error());
      all_ok = FALSE;
      goto end;
    }

  end:

    read_xmp_timestamps(xmpData, img, xmp_version);

    // set or clear bit in image struct. ONLY set if the Xmp.darktable.auto_presets_applied was 1
    // AND there was a history in xmp
    if(preset_applied > 0)
    {
      img->flags |= DT_IMAGE_AUTO_PRESETS_APPLIED;
    }
    else
    {
      // not found for old or buggy xmp where it was found but history was 0
      img->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

      if(preset_applied < 0)
      {
        fprintf(stderr,"[exif] dt_exif_xmp_read for %s, id %i found auto_presets_applied but there was no history\n",filename,img->id);
      }
    }

    g_list_free_full(iop_order_list, dt_free_gpointer);
    iop_order_list = NULL;
    g_list_free_full(history_entries, free_history_entry);
    history_entries = NULL;
    g_list_free_full(mask_entries_v3, free_mask_entry);
    mask_entries_v3 = NULL;
    if(mask_entries) g_hash_table_destroy(mask_entries);

    if(all_ok)
    {
      dt_database_release_transaction();

      // history_hash (current only)
      if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.history_current_hash"))) != xmpData.end())
      {
        int hash_len = 0;
        // one temporary, measured once: toString() called twice returns two distinct
        // strings, so the length came from a different object than the pointer did --
        // same contents, so it worked, but nothing said it had to
        const std::string hash_str = pos->toString();
        unsigned char *decoded = dt_exif_xmp_decode(hash_str.c_str(), hash_str.size(), &hash_len);
        if(decoded && hash_len == (int)sizeof(uint64_t))
        {
          uint64_t be_hash = 0;
          memcpy(&be_hash, decoded, sizeof(be_hash));
          img->history_hash = GUINT64_FROM_BE(be_hash);
        }
        dt_free(decoded);
      }
    }
    else
    {
      std::cerr << "[exif] error reading history from '" << filename << "'" << std::endl;
      dt_database_rollback_transaction();
      return 1;
    }

  }
  catch(const std::exception &e)
  {
    // actually nobody's interested in that if the file doesn't exist:
    // std::string s(e.what());
    // std::cerr << "[exiv2] " << filename << ": " << s << std::endl;
    return 1;
  }
  return 0;
}

// add history metadata to XmpData
typedef struct _xmp_mask_ctx_t
{
  Exiv2::XmpData *xmpData;
  int num;
} _xmp_mask_ctx_t;

static void _xmp_append_mask(void *user_data, const int mask_num, const int mask_id,
                             const int mask_type, const char *mask_name, const int mask_version,
                             const void *points, const int points_len, const int mask_nb,
                             const void *source, const int source_len)
{
  _xmp_mask_ctx_t *ctx = (_xmp_mask_ctx_t *)user_data;
  Exiv2::XmpData &xmpData = *ctx->xmpData;
  char key[1024];
  const int num = ctx->num;

  char *mask_d = dt_exif_xmp_encode((const unsigned char *)points, points_len, NULL);
  char *mask_src = dt_exif_xmp_encode((const unsigned char *)source, source_len, NULL);

  snprintf(key, sizeof(key), "Xmp.darktable.masks_history[%d]/darktable:mask_num", num);
  xmpData[key] = mask_num;
  snprintf(key, sizeof(key), "Xmp.darktable.masks_history[%d]/darktable:mask_id", num);
  xmpData[key] = mask_id;
  snprintf(key, sizeof(key), "Xmp.darktable.masks_history[%d]/darktable:mask_type", num);
  xmpData[key] = mask_type;
  snprintf(key, sizeof(key), "Xmp.darktable.masks_history[%d]/darktable:mask_name", num);
  xmpData[key] = mask_name;
  snprintf(key, sizeof(key), "Xmp.darktable.masks_history[%d]/darktable:mask_version", num);
  xmpData[key] = mask_version;
  snprintf(key, sizeof(key), "Xmp.darktable.masks_history[%d]/darktable:mask_points", num);
  xmpData[key] = mask_d;
  snprintf(key, sizeof(key), "Xmp.darktable.masks_history[%d]/darktable:mask_nb", num);
  xmpData[key] = mask_nb;
  snprintf(key, sizeof(key), "Xmp.darktable.masks_history[%d]/darktable:mask_src", num);
  xmpData[key] = mask_src;

  dt_free(mask_d);
  dt_free(mask_src);

  ctx->num++;
}

typedef struct _xmp_history_ctx_t
{
  Exiv2::XmpData *xmpData;
  int num;
} _xmp_history_ctx_t;

static void _xmp_append_history(void *user_data, const int32_t row_imgid, const int hist_num,
                                const int modversion, const char *operation,
                                const void *op_params, const int params_len,
                                const gboolean enabled, const void *blendop_params_blob,
                                const int blendop_params_len, const int blendop_version,
                                const int multi_priority, const char *multi_name,
                                const char *preset_name)
{
  _xmp_history_ctx_t *ctx = (_xmp_history_ctx_t *)user_data;
  Exiv2::XmpData &xmpData = *ctx->xmpData;
  char key[1024];
  const int num = ctx->num;

  if(IS_NULL_PTR(operation)) return; // no op is fatal.

  char *params = dt_exif_xmp_encode((const unsigned char *)op_params, params_len, NULL);

  snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:num", num);
  xmpData[key] = hist_num;
  snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:operation", num);
  xmpData[key] = operation;
  snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:enabled", num);
  xmpData[key] = enabled;
  snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:modversion", num);
  xmpData[key] = modversion;
  snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:params", num);
  xmpData[key] = params;
  snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:multi_name", num);
  xmpData[key] = multi_name ? multi_name : "";
  snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:multi_priority", num);
  xmpData[key] = multi_priority;

  if(blendop_params_blob)
  {
    // this shouldn't fail in general, but reading is robust enough to allow it,
    // and flipping images from LT will result in this being left out
    char *blendop_params = dt_exif_xmp_encode((const unsigned char *)blendop_params_blob, blendop_params_len, NULL);
    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:blendop_version", num);
    xmpData[key] = blendop_version;
    snprintf(key, sizeof(key), "Xmp.darktable.history[%d]/darktable:blendop_params", num);
    xmpData[key] = blendop_params;
    dt_free(blendop_params);
  }

  dt_free(params);

  ctx->num++;
}

static void dt_set_xmp_dt_history(Exiv2::XmpData &xmpData, const int32_t imgid, int history_end)
{
  // masks history:
  int num = 1;

  // create an array:
  Exiv2::XmpTextValue tvm("");
  tvm.setXmpArrayType(Exiv2::XmpValue::xaSeq);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.masks_history"), &tvm);
  _xmp_mask_ctx_t mask_ctx = { &xmpData, num };
  dt_history_repository_foreach_mask_item(imgid, _xmp_append_mask, &mask_ctx);
  num = mask_ctx.num;

  // history stack:
  num = 1;

  // create an array:
  Exiv2::XmpTextValue tv("");
  tv.setXmpArrayType(Exiv2::XmpValue::xaSeq);
  xmpData.add(Exiv2::XmpKey("Xmp.darktable.history"), &tv);
  _xmp_history_ctx_t hist_ctx = { &xmpData, num };
  dt_history_repository_foreach_row(imgid, _xmp_append_history, &hist_ctx);
  num = hist_ctx.num;
  if(history_end == -1) history_end = num - 1;
  else history_end = MIN(history_end, num - 1); // safeguard for some old buggy libraries
  xmpData["Xmp.darktable.history_end"] = history_end;
}

// add timestamps to XmpData.
static void set_xmp_timestamps(Exiv2::XmpData &xmpData, const int32_t imgid)
{
  static const char *keys[] =
  {
    "Xmp.darktable.import_timestamp",
    "Xmp.darktable.change_timestamp",
    "Xmp.darktable.export_timestamp",
    "Xmp.darktable.print_timestamp"
  };
  static const guint n_keys = G_N_ELEMENTS(keys);
  dt_remove_xmp_keys(xmpData, keys, n_keys);

  dt_image_timestamps_t ts;
  if(dt_image_repository_get_timestamps(imgid, &ts))
  {
    if(ts.has_import) xmpData["Xmp.darktable.import_timestamp"] = ts.import_timestamp;
    if(ts.has_change) xmpData["Xmp.darktable.change_timestamp"] = ts.change_timestamp;
    if(ts.has_export) xmpData["Xmp.darktable.export_timestamp"] = ts.export_timestamp;
    if(ts.has_print)  xmpData["Xmp.darktable.print_timestamp"]  = ts.print_timestamp;
  }
}

GTimeSpan _convert_unix_to_gtimespan(const time_t unix)
{
  GDateTime *gdt = g_date_time_new_from_unix_utc(unix);
  if(gdt)
  {
    GTimeSpan gts = dt_datetime_gdatetime_to_gtimespan(gdt);
    g_date_time_unref(gdt);
    return gts;
  }
  return 0;
}

// read timestamps from XmpData
void read_xmp_timestamps(Exiv2::XmpData &xmpData, dt_image_t *img, const int xmp_version)
{
  Exiv2::XmpData::iterator pos;

  // Do not read for import_ts. It must be updated at each import.
  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.change_timestamp"))) != xmpData.end())
  {
    if(xmp_version > 5)
      img->change_timestamp = pos->toLong();
    else if(pos->toLong() >= 1)
      img->change_timestamp = _convert_unix_to_gtimespan(pos->toLong());
  }
  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.export_timestamp"))) != xmpData.end())
  {
    if(xmp_version > 5)
      img->export_timestamp = pos->toLong();
    else if(pos->toLong() >= 1)
      img->export_timestamp = _convert_unix_to_gtimespan(pos->toLong());
  }
  if((pos = xmpData.findKey(Exiv2::XmpKey("Xmp.darktable.print_timestamp"))) != xmpData.end())
  {
    if(xmp_version > 5)
      img->print_timestamp = pos->toLong();
    else if(pos->toLong() >= 1)
      img->print_timestamp = _convert_unix_to_gtimespan(pos->toLong());
  }
}

static void dt_remove_xmp_exif_geotag(Exiv2::XmpData &xmpData)
{
  static const char *keys[] =
  {
    "Xmp.exif.GPSVersionID",
    "Xmp.exif.GPSLongitude",
    "Xmp.exif.GPSLatitude",
    "Xmp.exif.GPSAltitudeRef",
    "Xmp.exif.GPSAltitude"
  };
  static const guint n_keys = G_N_ELEMENTS(keys);
  dt_remove_xmp_keys(xmpData, keys, n_keys);
}

static void dt_set_xmp_exif_geotag(Exiv2::XmpData &xmpData, double longitude, double latitude, double altitude)
{
  dt_remove_xmp_exif_geotag(xmpData);
  if(!isnan(longitude) && !isnan(latitude))
  {
    char long_dir = 'E', lat_dir = 'N';
    if(longitude < 0) long_dir = 'W';
    if(latitude < 0) lat_dir = 'S';

    longitude = fabs(longitude);
    latitude = fabs(latitude);

    int long_deg = (int)floor(longitude);
    int lat_deg = (int)floor(latitude);
    double long_min = (longitude - (double)long_deg) * 60.0;
    double lat_min = (latitude - (double)lat_deg) * 60.0;

    char *str = (char *)g_malloc(G_ASCII_DTOSTR_BUF_SIZE);

    g_ascii_formatd(str, G_ASCII_DTOSTR_BUF_SIZE, "%08f", long_min);
    gchar *long_str = g_strdup_printf("%d,%s%c", long_deg, str, long_dir);
    g_ascii_formatd(str, G_ASCII_DTOSTR_BUF_SIZE, "%08f", lat_min);
    gchar *lat_str = g_strdup_printf("%d,%s%c", lat_deg, str, lat_dir);

    xmpData["Xmp.exif.GPSVersionID"] = "2.2.0.0";
    xmpData["Xmp.exif.GPSLongitude"] = long_str;
    xmpData["Xmp.exif.GPSLatitude"] = lat_str;
    dt_free(long_str);
    dt_free(lat_str);
    dt_free(str);
  }
  if(!isnan(altitude))
  {
    xmpData["Xmp.exif.GPSAltitudeRef"] = (altitude < 0) ? "1" : "0";

    long ele_dm = (int)floor(fabs(10.0 * altitude));
    gchar *ele_str = g_strdup_printf("%ld/10", ele_dm);
    xmpData["Xmp.exif.GPSAltitude"] = ele_str;
    dt_free(ele_str);
  }
}

typedef struct _xmp_metadata_ctx_t
{
  Exiv2::XmpData *xmpData;
  gboolean export_flag;
} _xmp_metadata_ctx_t;

static void _xmp_append_metadata(void *user_data, const int keyid, const char *value)
{
  _xmp_metadata_ctx_t *ctx = (_xmp_metadata_ctx_t *)user_data;
  Exiv2::XmpData &xmpData = *ctx->xmpData;

  if(ctx->export_flag && (dt_metadata_get_type(keyid) != DT_METADATA_TYPE_INTERNAL))
  {
    const gchar *name = dt_metadata_get_name(keyid);
    gchar *setting = dt_util_dstrcat(NULL, "plugins/lighttable/metadata/%s_flag", name);
    const uint32_t flag =  dt_conf_get_int(setting);
    dt_free(setting);
    if(!(flag & (DT_METADATA_FLAG_PRIVATE | DT_METADATA_FLAG_HIDDEN)))
      xmpData[dt_metadata_get_key(keyid)] = value;
  }
  else
    xmpData[dt_metadata_get_key(keyid)] = value;
}

static void _xmp_append_colorlabel(void *user_data, const int color)
{
  Exiv2::Value *v = (Exiv2::Value *)user_data;
  char val[2048];
  snprintf(val, sizeof(val), "%d", color);
  v->read(val);
}

static void dt_set_xmp_dt_metadata(Exiv2::XmpData &xmpData, const int32_t imgid, const gboolean export_flag)
{
  // metadata
  _xmp_metadata_ctx_t meta_ctx = { &xmpData, export_flag };
  dt_metadata_repository_foreach(imgid, _xmp_append_metadata, &meta_ctx);

  // color labels
  std::unique_ptr<Exiv2::Value> v(Exiv2::Value::create(Exiv2::xmpSeq)); // or xmpBag or xmpAlt.

  /* Already initialized v = Exiv2::Value::create(Exiv2::xmpSeq); // or xmpBag or xmpAlt.*/
  dt_colorlabel_repository_foreach(imgid, _xmp_append_colorlabel, v.get());
  if(v->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.darktable.colorlabels"), v.get());
}

// helper to create an xmp data thing. throws exiv2 exceptions if stuff goes wrong.
static void _exif_xmp_append_history_hash(Exiv2::XmpData &xmpData, const int32_t imgid,
                                          const dt_image_t *image)
{
  const dt_image_t *cached = image;
  if(IS_NULL_PTR(cached))
    cached = dt_image_cache_get(imgid, 'r');

  if(cached)
  {
    if(cached->history_hash != UINT64_MAX)
    {
      const uint64_t be_hash = GUINT64_TO_BE(cached->history_hash);
      char *value = dt_exif_xmp_encode(reinterpret_cast<const unsigned char *>(&be_hash), sizeof(be_hash), NULL);
      if(value)
      {
        xmpData["Xmp.darktable.history_current_hash"] = value;
        dt_free(value);
      }
    }
    if(IS_NULL_PTR(image))
      dt_image_cache_read_release(cached);
  }
}

static void _exif_xmp_read_data(Exiv2::XmpData &xmpData, const int32_t imgid, const dt_image_t *image)
{
  const int xmp_version = DT_XMP_EXIF_VERSION;
  int stars = 1, raw_params = 0, history_end = -1;
  double longitude = NAN, latitude = NAN, altitude = NAN;
  gchar *filename = NULL;
  gchar *iop_order_list = NULL;
  GTimeSpan gts = 0;

  // get stars and raw params from db
  dt_image_xmp_row_t row;
  if(dt_image_repository_get_xmp_row(imgid, &row))
  {
    filename = row.filename;
    stars = row.flags;
    raw_params = row.raw_parameters;
    if(row.has_longitude) longitude = row.longitude;
    if(row.has_latitude) latitude = row.latitude;
    if(row.has_altitude) altitude = row.altitude;
    history_end = row.history_end;
    gts = row.datetime_taken;
  }

  // The pipe order only matters when history entries are exported. After
  // deleting history, avoid rebuilding a default order just to write an empty
  // sidecar: the default will be selected again when the image is opened.
  dt_iop_order_t iop_order_version = DT_IOP_ORDER_ANSEL_RAW;
  if(history_end > 0)
  {
    iop_order_version = dt_ioppr_get_iop_order_version(imgid);
    GList *iop_list = dt_ioppr_get_iop_order_list(imgid, TRUE);

    if(iop_order_version == DT_IOP_ORDER_CUSTOM || dt_ioppr_has_multiple_instances(iop_list))
    {
      iop_order_list = dt_ioppr_serialize_text_iop_order_list(iop_list);
    }
    g_list_free_full(iop_list, dt_free_gpointer);
    iop_list = NULL;
  }

  // Store datetime_taken as DateTimeOriginal to take into account the user's selected date/time
  gchar exif_datetime[DT_DATETIME_LENGTH];
  dt_datetime_gtimespan_to_exif(exif_datetime, sizeof(exif_datetime), gts);
  xmpData["Xmp.exif.DateTimeOriginal"] = exif_datetime;

  // We have to erase the old ratings first as exiv2 seems to not change it otherwise.
  Exiv2::XmpData::iterator pos = xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
  if(pos != xmpData.end()) xmpData.erase(pos);
  xmpData["Xmp.xmp.Rating"] = dt_image_get_xmp_rating_from_flags(stars);

  // The original file name
  if(filename) xmpData["Xmp.xmpMM.DerivedFrom"] = filename;

  // timestamps
  set_xmp_timestamps(xmpData, imgid);

  // GPS data
  dt_set_xmp_exif_geotag(xmpData, longitude, latitude, altitude);

  // the meta data
  dt_set_xmp_dt_metadata(xmpData, imgid, FALSE);

  // get tags from db, store in dublin core
  std::unique_ptr<Exiv2::Value> v1(Exiv2::Value::create(Exiv2::xmpBag));

  std::unique_ptr<Exiv2::Value> v2(Exiv2::Value::create(Exiv2::xmpBag));

  GList *tags = dt_tag_get_list(imgid);
  try
  {
    for(GList *tag = tags; tag; tag = g_list_next(tag))
    {
      v1->read((char *)tag->data);
    }
  }
  catch(...)
  {
    g_list_free_full(tags, dt_free_gpointer);
    throw;
  }
  if(v1->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.dc.subject"), v1.get());
  g_list_free_full(tags, dt_free_gpointer);
  tags = NULL;

  GList *hierarchical = dt_tag_get_hierarchical(imgid);
  try
  {
    for(GList *hier = hierarchical; hier; hier = g_list_next(hier))
    {
      v2->read((char *)hier->data);
    }
  }
  catch(...)
  {
    g_list_free_full(hierarchical, dt_free_gpointer);
    throw;
  }
  if(v2->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.lr.hierarchicalSubject"), v2.get());
  g_list_free_full(hierarchical, dt_free_gpointer);
  hierarchical = NULL;
  /* TODO: Add tags to IPTC namespace as well */

  xmpData["Xmp.darktable.xmp_version"] = xmp_version;
  xmpData["Xmp.darktable.raw_params"] = raw_params;
  if(stars & DT_IMAGE_AUTO_PRESETS_APPLIED)
    xmpData["Xmp.darktable.auto_presets_applied"] = 1;
  else
    xmpData["Xmp.darktable.auto_presets_applied"] = 0;
  dt_set_xmp_dt_history(xmpData, imgid, history_end);

  if(history_end > 0)
  {
    xmpData["Xmp.darktable.iop_order_version"] = iop_order_version;
    if(iop_order_list) xmpData["Xmp.darktable.iop_order_list"] = iop_order_list;
  }

  _exif_xmp_append_history_hash(xmpData, imgid, image);

  dt_free(iop_order_list);
  dt_image_repository_xmp_row_cleanup(&row);
}

// helper to create an xmp data thing. throws exiv2 exceptions if stuff goes wrong.
static void _exif_xmp_read_data_export(Exiv2::XmpData &xmpData, const int32_t imgid, dt_export_metadata_t *metadata)
{
  const int xmp_version = DT_XMP_EXIF_VERSION;
  int stars = 1, raw_params = 0, history_end = -1;
  double longitude = NAN, latitude = NAN, altitude = NAN;
  gchar *filename = NULL;
  GTimeSpan gts = 0;
  gchar *iop_order_list = NULL;

  // get stars and raw params from db
  dt_image_xmp_row_t row;
  if(dt_image_repository_get_xmp_row(imgid, &row))
  {
    filename = row.filename;
    stars = row.flags;
    raw_params = row.raw_parameters;
    if(row.has_longitude) longitude = row.longitude;
    if(row.has_latitude) latitude = row.latitude;
    if(row.has_altitude) altitude = row.altitude;
    history_end = row.history_end;
    gts = row.datetime_taken;
  }

  // The pipe order only matters when history entries are exported. After
  // deleting history, avoid rebuilding a default order just to write an empty
  // sidecar: the default will be selected again when the image is opened.
  dt_iop_order_t iop_order_version = DT_IOP_ORDER_ANSEL_RAW;
  if(history_end > 0)
  {
    iop_order_version = dt_ioppr_get_iop_order_version(imgid);
    GList *iop_list = dt_ioppr_get_iop_order_list(imgid, TRUE);

    if(iop_order_version == DT_IOP_ORDER_CUSTOM || dt_ioppr_has_multiple_instances(iop_list))
    {
      iop_order_list = dt_ioppr_serialize_text_iop_order_list(iop_list);
    }
    g_list_free_full(iop_list, dt_free_gpointer);
    iop_list = NULL;
  }

  if(metadata->flags & DT_META_METADATA)
  {
    // Store datetime_taken as DateTimeOriginal to take into account the user's selected date/time
    if (!(metadata->flags & DT_META_EXIF))
    {
      gchar exif_datetime[DT_DATETIME_LENGTH];
      dt_datetime_gtimespan_to_exif(exif_datetime, sizeof(exif_datetime), gts);
      xmpData["Xmp.exif.DateTimeOriginal"] = exif_datetime;
    }
    // We have to erase the old ratings first as exiv2 seems to not change it otherwise.
    Exiv2::XmpData::iterator pos = xmpData.findKey(Exiv2::XmpKey("Xmp.xmp.Rating"));
    if(pos != xmpData.end()) xmpData.erase(pos);
    xmpData["Xmp.xmp.Rating"] = dt_image_get_xmp_rating_from_flags(stars);

    // The original file name
    if(filename) xmpData["Xmp.xmpMM.DerivedFrom"] = filename;
  }

  // GPS data
  if (metadata->flags & DT_META_GEOTAG)
    dt_set_xmp_exif_geotag(xmpData, longitude, latitude, altitude);
  else
    dt_remove_xmp_exif_geotag(xmpData);


  // the meta data
  if (metadata->flags & DT_META_METADATA)
    dt_set_xmp_dt_metadata(xmpData, imgid, TRUE);

  // tags
  if (metadata->flags & DT_META_TAG)
  {
    // get tags from db, store in dublin core
    std::unique_ptr<Exiv2::Value> v1(Exiv2::Value::create(Exiv2::xmpBag));
    GList *tags = dt_tag_get_list_export(imgid, metadata->flags);
    try
    {
      for(GList *tag = tags; tag; tag = g_list_next(tag))
      {
        v1->read((char *)tag->data);
      }
    }
    catch(...)
    {
      g_list_free_full(tags, dt_free_gpointer);
      throw;
    }
    if(v1->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.dc.subject"), v1.get());
    g_list_free_full(tags, dt_free_gpointer);
    tags = NULL;
  }

  if (metadata->flags & DT_META_HIERARCHICAL_TAG)
  {
    std::unique_ptr<Exiv2::Value> v2(Exiv2::Value::create(Exiv2::xmpBag));
    GList *hierarchical = dt_tag_get_hierarchical_export(imgid, metadata->flags);
    try
    {
      for(GList *hier = hierarchical; hier; hier = g_list_next(hier))
      {
        v2->read((char *)hier->data);
      }
    }
    catch(...)
    {
      g_list_free_full(hierarchical, dt_free_gpointer);
      throw;
    }
    if(v2->count() > 0) xmpData.add(Exiv2::XmpKey("Xmp.lr.hierarchicalSubject"), v2.get());
    g_list_free_full(hierarchical, dt_free_gpointer);
    hierarchical = NULL;
  }

  if (metadata->flags & DT_META_DT_HISTORY)
  {
    xmpData["Xmp.darktable.xmp_version"] = xmp_version;
    xmpData["Xmp.darktable.raw_params"] = raw_params;
    if(stars & DT_IMAGE_AUTO_PRESETS_APPLIED)
      xmpData["Xmp.darktable.auto_presets_applied"] = 1;
    else
      xmpData["Xmp.darktable.auto_presets_applied"] = 0;
    dt_set_xmp_dt_history(xmpData, imgid, history_end);

    if(history_end > 0)
    {
      xmpData["Xmp.darktable.iop_order_version"] = iop_order_version;
      if(iop_order_list) xmpData["Xmp.darktable.iop_order_list"] = iop_order_list;
    }
    _exif_xmp_append_history_hash(xmpData, imgid, NULL);
  }

  dt_free(iop_order_list);
  dt_image_repository_xmp_row_cleanup(&row);
}

#if EXIV2_TEST_VERSION(0,27,0)
#define ERROR_CODE(a) (static_cast<Exiv2::ErrorCode>((a)))
#else
#define ERROR_CODE(a) (a)
#endif

char *dt_exif_xmp_read_string(const int32_t imgid)
{
  try
  {
    // Serialize the non-thread-safe exiv2/XMP toolkit (XmpParser::decode()/encode() below) against
    // all other exiv2 work. Recursive mutex, so nested helpers re-lock harmlessly.
    Lock lock;

    char input_filename[DT_PATH_MAX] = { 0 };
    gboolean from_cache = FALSE;
    dt_image_full_path(imgid,  input_filename,  sizeof(input_filename),  &from_cache, __FUNCTION__);

    // first take over the data from the source image
    Exiv2::XmpData xmpData;
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      std::string xmpPacket;

      if(_read_xmp_packet(input_filename, xmpPacket))
      {
        Exiv2::XmpParser::decode(xmpData, xmpPacket);
        // because XmpSeq or XmpBag are added to the list, we first have
        // to remove these so that we don't end up with a string of duplicates
        dt_remove_known_keys(xmpData);
      }
    }

    // now add whatever we have in the sidecar XMP. this overwrites stuff from the source image
    dt_image_path_append_version(imgid, input_filename, sizeof(input_filename));
    g_strlcat(input_filename, ".xmp", sizeof(input_filename));
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      Exiv2::XmpData sidecarXmpData;
      std::string xmpPacket;

      if(_read_xmp_packet(input_filename, xmpPacket))
      {
        Exiv2::XmpParser::decode(sidecarXmpData, xmpPacket);

        for(Exiv2::XmpData::const_iterator it = sidecarXmpData.begin(); it != sidecarXmpData.end(); ++it)
          xmpData.add(*it);
      }
    }

    dt_remove_known_keys(xmpData); // is this needed?

    // last but not least attach what we have in DB to the XMP. in theory that should be
    // the same as what we just copied over from the sidecar file, but you never know ...
    _exif_xmp_read_data(xmpData, imgid, NULL);

    // serialize the xmp data and output the xmp packet
    std::string xmpPacket;
    if(Exiv2::XmpParser::encode(xmpPacket, xmpData,
      Exiv2::XmpParser::useCompactFormat | Exiv2::XmpParser::omitPacketWrapper) != 0)
    {
      throw Exiv2::Error(ERROR_CODE(1), "[xmp_write] failed to serialize xmp data");
    }
    return g_strdup(xmpPacket.c_str());
  }
  catch(const std::exception &e)
  {
    std::cerr << "[xmp_read_blob] caught exiv2 exception '" << e.what() << "'\n";
    return NULL;
  }
}

static void dt_remove_xmp_key(Exiv2::XmpData &xmp, const char *key)
{
  try
  {
    Exiv2::XmpData::iterator pos = xmp.findKey(Exiv2::XmpKey(key));
    if (pos != xmp.end())
      xmp.erase(pos);
  }
  catch(const std::exception &e)
  {
  }
}

static void _remove_xmp_keys(Exiv2::XmpData &xmpData, const char *key)
{
  try
  {
    const std::string needle = key;
    for(Exiv2::XmpData::iterator i = xmpData.begin(); i != xmpData.end();)
    {
      if(i->key().compare(0, needle.length(), needle) == 0)
        i = xmpData.erase(i);
      else
        ++i;
    }
  }
  catch(const std::exception &e)
  {
  }
}

static void dt_remove_exif_key(Exiv2::ExifData &exif, const char *key)
{
  try
  {
    Exiv2::ExifData::iterator pos = exif.findKey(Exiv2::ExifKey(key));
    if (pos != exif.end())
      exif.erase(pos);
  }
  catch(const std::exception &e)
  {
  }
}

static void dt_remove_iptc_key(Exiv2::IptcData &iptc, const char *key)
{
  try
  {
    Exiv2::IptcData::iterator pos;
    while((pos = iptc.findKey(Exiv2::IptcKey(key))) != iptc.end())
      iptc.erase(pos);
  }
  catch(const std::exception &e)
  {
  }
}

int dt_exif_xmp_attach_export(const int32_t imgid, const char *filename, void *metadata)
{
  dt_export_metadata_t *m = (dt_export_metadata_t *)metadata;
  try
  {
    // Serialize the whole exiv2 region: this function mixes readMetadata(), XmpParser::decode() and
    // writeMetadata(), all of which touch the non-thread-safe exiv2/XMP toolkit and must not run
    // concurrently with other exiv2 work. The mutex is recursive, so the nested
    // read_metadata_threadsafe() calls (and any metadata read during variable expansion) re-lock
    // harmlessly.
    Lock lock;

    char input_filename[DT_PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(imgid,  input_filename,  sizeof(input_filename),  &from_cache, __FUNCTION__);

    std::unique_ptr<Exiv2::Image> img(Exiv2::ImageFactory::open(WIDEN(filename)));
    // unfortunately it seems we have to read the metadata, to not erase the exif (which we just wrote).
    // will make export slightly slower, oh well.
    // img->clearXmpPacket();
    read_metadata_threadsafe(img);

    try
    {
      // initialize XMP and IPTC data with the one from the original file
      std::unique_ptr<Exiv2::Image> input_image(Exiv2::ImageFactory::open(WIDEN(input_filename)));
      if(input_image.get() != 0)
      {
        read_metadata_threadsafe(input_image);
        img->setIptcData(input_image->iptcData());
        img->setXmpData(input_image->xmpData());
      }
    }
    catch(const std::exception &e)
    {
      std::cerr << "[xmp_attach] " << input_filename << ": caught exiv2 exception '" << e.what() << "'\n";
    }

    Exiv2::XmpData &xmpData = img->xmpData();

    // now add whatever we have in the sidecar XMP. this overwrites stuff from the source image
    dt_image_path_append_version(imgid, input_filename, sizeof(input_filename));
    g_strlcat(input_filename, ".xmp", sizeof(input_filename));
    if(g_file_test(input_filename, G_FILE_TEST_EXISTS))
    {
      Exiv2::XmpData sidecarXmpData;
      std::string xmpPacket;

      if(_read_xmp_packet(input_filename, xmpPacket))
      {
        Exiv2::XmpParser::decode(sidecarXmpData, xmpPacket);

        for(Exiv2::XmpData::const_iterator it = sidecarXmpData.begin(); it != sidecarXmpData.end(); ++it)
          xmpData.add(*it);
      }
    }

    dt_remove_known_keys(xmpData); // is this needed?

    {
      // We also want to make sure to not have some tags that might
      // have come in from XMP files created by digikam or similar
      static const char *keys[] = {
        "Xmp.tiff.Orientation"
      };
      static const guint n_keys = G_N_ELEMENTS(keys);
      dt_remove_xmp_keys(xmpData, keys, n_keys);
    }

    // last but not least attach what we have in DB to the XMP. in theory that should be
    // the same as what we just copied over from the sidecar file, but you never know ...
    // make sure to remove all geotags if necessary
    if(m)
    {
      Exiv2::ExifData exifOldData;
      Exiv2::ExifData &exifData = img->exifData();
      if(!(m->flags & DT_META_EXIF))
      {
        for(Exiv2::ExifData::const_iterator i = exifData.begin(); i != exifData.end() ; ++i)
        {
          exifOldData[i->key()] = i->value();
        }
        img->clearExifData();
      }

      _exif_xmp_read_data_export(xmpData, imgid, m);

      Exiv2::IptcData &iptcData = img->iptcData();

      if(!(m->flags & DT_META_GEOTAG))
        dt_remove_exif_geotag(exifData);
      // calculated metadata
      dt_variables_params_t *params;
      dt_variables_params_init(&params);
      params->filename = input_filename;
      params->jobcode = "infos";
      params->sequence = 0;
      params->imgid = imgid;

      dt_variables_set_tags_flags(params, m->flags);
      for (GList *tags = m->list; tags; tags = g_list_next(tags))
      {
        gchar *tagname = (gchar *)tags->data;
        tags = g_list_next(tags);
        if (IS_NULL_PTR(tags)) break;
        gchar *formula = (gchar *)tags->data;
        if (formula[0])
        {
          if(!(m->flags & DT_META_EXIF) && (formula[0] == '=') && g_str_has_prefix(tagname, "Exif."))
          {
            // remove this specific exif
            Exiv2::ExifData::const_iterator pos;
            if(dt_exif_read_exif_tag(exifOldData, &pos, tagname))
            {
              exifData[tagname] = pos->value();
            }
          }
          else
          {
            gchar *result = dt_variables_expand(params, formula, FALSE);
            if(result && result[0])
            {
              if(g_str_has_prefix(tagname, "Xmp."))
              {
                const char *type = _exif_get_exiv2_tag_type(tagname);
                // if xmpBag or xmpSeq, split the list when necessary
                // else provide the string as is (can be a list of strings)
                if(!g_strcmp0(type, "XmpBag") || !g_strcmp0(type, "XmpSeq"))
                {
                  char *tuple = g_strrstr(result, ",");
                  while(tuple)
                  {
                    tuple[0] = '\0';
                    tuple++;
                    xmpData[tagname] = tuple;
                    tuple = g_strrstr(result, ",");
                  }
                }
                xmpData[tagname] = result;
              }
              else if(g_str_has_prefix(tagname, "Iptc."))
              {
                const char *type = _exif_get_exiv2_tag_type(tagname);
                if(!g_strcmp0(type, "String-R"))
                {
                  // clean up the original tags before giving new values
                  dt_remove_iptc_key(iptcData, tagname);
                  // convert the input list (separator ", ") into different tags
                  // FIXME if an element of the list contains a ", " it is not correctly exported
                  Exiv2::IptcKey key(tagname);
                  Exiv2::Iptcdatum id(key);
                  gchar **values = g_strsplit(result, ", ", 0);
                  if(values)
                  {
                    gchar **entry = values;
                    while (*entry)
                    {
                      char *e = g_strstrip(*entry);
                      if(*e)
                      {
                        id.setValue(e);
                        iptcData.add(id);
                      }
                      entry++;
                    }
                  }
                g_strfreev(values);
                }
                else iptcData[tagname] = result;
              }
              else if(g_str_has_prefix(tagname, "Exif."))
              {
                const char *type = _exif_get_exiv2_tag_type(tagname);
                if((!g_strcmp0(type, "Rational") || !g_strcmp0(type, "SRational")) &&
                   (strstr(result, "/") == NULL))
                {
                  float float_value = (float)std::atof(result);
                  if(!isnan(float_value))
                  {
                    dt_free(result);
                    int int_value = (int)float_value;
                    int divisor = 1;
                    while(fabs(float_value - int_value) > 0.000001)
                    {
                      divisor *= 10;
                      float_value *= 10.0;
                      int_value = (int)float_value;
                    }
                    result = g_strdup_printf("%d/%d", (int)float_value, divisor);
                  }
                }
                exifData[tagname] = result;
              }
            }
            dt_free(result);
          }
        }
        else
        {
          if (g_str_has_prefix(tagname, "Xmp."))
            dt_remove_xmp_key(xmpData, tagname);
          else if (g_str_has_prefix(tagname, "Exif."))
            dt_remove_exif_key(exifData, tagname);
          else if (g_str_has_prefix(tagname, "Iptc."))
            dt_remove_iptc_key(iptcData, tagname);
        }
      }
      dt_variables_params_destroy(params);
    }

    try
    {
      img->writeMetadata();
    }
    catch(Exiv2::AnyError &e)
    {
#if EXIV2_TEST_VERSION(0,27,0)
      if(e.code() == Exiv2::ErrorCode::kerTooLargeJpegSegment)
#else
      if(e.code() == 37)
#endif
      {
        _remove_xmp_keys(xmpData, "Xmp.darktable.history");
        _remove_xmp_keys(xmpData, "Xmp.darktable.masks_history");
        _remove_xmp_keys(xmpData, "Xmp.darktable.auto_presets_applied");
        _remove_xmp_keys(xmpData, "Xmp.darktable.iop_order");
        try
        {
          img->writeMetadata();
        }
        catch(const std::exception &e2)
        {
          std::cerr << "[dt_exif_xmp_attach_export] without history " << filename << ": caught exiv2 exception '" << e2.what() << "'\n";
          return -1;
        }
      }
      else
        throw;
    }
    catch(const std::exception &e)
    {
      std::cerr << "[dt_exif_xmp_attach_export] " << filename << ": caught exception '" << e.what() << "'\n";
      return -1;
    }
    return 0;
  }
  catch(const std::exception &e)
  {
    std::cerr << "[dt_exif_xmp_attach_export] " << filename << ": caught exiv2 exception '" << e.what() << "'\n";
    return -1;
  }
}

// write xmp sidecar file:
int dt_exif_xmp_write_with_imgpath(const dt_image_t *image, const char *filename,
                                   const char *imgpath)
{
  // refuse to write sidecar for non-existent image:
  if(IS_NULL_PTR(image) || image->id <= 0) return 1;
  if(IS_NULL_PTR(imgpath) || !*imgpath) return 1;
  if(!g_file_test(imgpath, G_FILE_TEST_IS_REGULAR)) return 1;
  const int32_t imgid = image->id;

  try
  {
    // The Adobe XMP toolkit behind Exiv2::XmpParser::decode()/encode() keeps process-global
    // state and is NOT thread-safe. Sidecar writes run on the worker thread pool, so several
    // imports/writes can hit dt_exif_xmp_write_with_imgpath() at once (and race the locked
    // readMetadata() in dt_exif_read()), corrupting the heap -> SIGABRT in free(). Serialize the
    // whole exiv2 region on the same (recursive) mutex read_metadata_threadsafe() uses.
    Lock lock;

    Exiv2::XmpData xmpData;
    std::string xmpPacket;
    char *checksum_old = NULL;
    if(g_file_test(filename, G_FILE_TEST_EXISTS))
    {
      // we want to avoid writing the sidecar file if it didn't change to avoid issues when using the same images
      // from different computers. sample use case: images on NAS, several computers using them NOT AT THE SAME TIME and
      // the xmp crawler is used to find changed sidecars.
      errno = 0;
      size_t end;
      unsigned char *content = (unsigned char*)dt_read_file(filename, &end);
      if(content)
      {
        if(end > 1000000)
        {
          dt_metadata_notify(DT_METADATA_NOTICE_MESSAGE,
                             _("The XMP file \n'%s'\n weighs %.2f MB. Writing it will take some time."),
                             filename, (float)end / 1000000);
          fprintf(stdout, "The XMP file '%s' weighs %.2f MB. Writing it will take some time.\n", filename, (float)end / 1000000);
        }

        checksum_old = g_compute_checksum_for_data(G_CHECKSUM_MD5, content, end);

        // The packet the parser wants is the same bytes we just hashed, so decode those
        // rather than read the file a second time.
        xmpPacket.assign((const char *)content, end);
        dt_free(content);

        Exiv2::XmpParser::decode(xmpData, xmpPacket);
        // because XmpSeq or XmpBag are added to the list, we first have
        // to remove these so that we don't end up with a string of duplicates
        dt_remove_known_keys(xmpData);
      }
      else
      {
        fprintf(stderr, "cannot read xmp file '%s': '%s'\n", filename, strerror(errno));
        dt_metadata_notify(DT_METADATA_NOTICE_MESSAGE, _("cannot read xmp file '%s': '%s'"),
                           filename, strerror(errno));
      }
    }

    // initialize xmp data:
    _exif_xmp_read_data(xmpData, imgid, image);

    // serialize the xmp data and output the xmp packet
    if(Exiv2::XmpParser::encode(xmpPacket, xmpData,
       Exiv2::XmpParser::useCompactFormat | Exiv2::XmpParser::omitPacketWrapper) != 0)
    {
      throw Exiv2::Error(ERROR_CODE(1), "[xmp_write] failed to serialize xmp data");
    }

    // hash the new data and compare it to the old hash (if applicable)
    const char *xml_header = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    gboolean write_sidecar = TRUE;
    if(checksum_old)
    {
      GChecksum *checksum = g_checksum_new(G_CHECKSUM_MD5);
      if(checksum)
      {
        g_checksum_update(checksum, (unsigned char*)xml_header, -1);
        g_checksum_update(checksum, (unsigned char*)xmpPacket.c_str(), -1);
        const char *checksum_new = g_checksum_get_string(checksum);
        write_sidecar = g_strcmp0(checksum_old, checksum_new) != 0;
        g_checksum_free(checksum);
      }
      dt_free(checksum_old);
    }

    if(write_sidecar)
    {
      // using std::ofstream isn't possible here -- on Windows it doesn't support Unicode filenames with mingw
      errno = 0;
      FILE *fout = g_fopen(filename, "wb");
      if(fout)
      {
        fprintf(fout, "%s", xml_header);
        fprintf(fout, "%s", xmpPacket.c_str());
        fclose(fout);
      }
      else
      {
        fprintf(stderr, "cannot write xmp file '%s': '%s'\n", filename, strerror(errno));
        dt_metadata_notify(DT_METADATA_NOTICE_MESSAGE, _("cannot write xmp file '%s': '%s'"),
                           filename, strerror(errno));
        return -1;
      }
    }

    return 0;
  }
  catch(const std::exception &e)
  {
    std::cerr << "[dt_exif_xmp_write] " << filename << ": caught exiv2 exception '" << e.what() << "'\n";
    return -1;
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
