/*
    This file is part of darktable,
    Copyright (C) 2010 Alex Chateau.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2013, 2016-2017 johannes hanika.
    Copyright (C) 2010 José Carlos García Sogo.
    Copyright (C) 2010, 2012-2014 Pascal de Bruijn.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Bruce Guenter.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2018 Tobias Ellinghaus.
    Copyright (C) 2011, 2013-2014 Ulrich Pegelow.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2014-2016 Pedro Côrte-Real.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015 parafin.
    Copyright (C) 2016 Peter Budai.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2019 Matthias Vogelgesang.
    Copyright (C) 2019-2022 Pascal Obry.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020 a.
    Copyright (C) 2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2020 Dan Torop.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Miloš Komarčević.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023 Luca Zulberti.
    
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

/* Choosing an ICC profile FOR AN IMAGE, and reading the one the file carries.
 *
 * This is codec work, not colour-management work, which is why it lives here rather than with
 * the LittleCMS2 code in common/colorspaces.c. Deciding that a JPEG's embedded profile beats
 * its EXIF colorspace tag, or that a RAW should use its camera matrix, means knowing what each
 * format stores and where -- and it was the only reason colorspaces.c included six imageio
 * codec headers.
 */

#include "imageio/imageio_profile.h"

#include "colorprofiles/colorspaces.h"
#include "common/image.h"
#include "caches/image_cache.h"
#include "common/logging.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <lcms2.h>
#include <string.h>

#ifdef HAVE_OPENJPEG
#include "imageio/imageio_j2k.h"   // conditional-ok: only the HAVE_OPENJPEG branch below reads J2K
#endif
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_png.h"
#include "imageio/imageio_tiff.h"
#ifdef HAVE_LIBAVIF
#include "imageio/imageio_avif.h"   // conditional-ok: only the HAVE_LIBAVIF branch below reads AVIF
#endif
#ifdef HAVE_LIBHEIF
#include "imageio/imageio_heif.h"   // conditional-ok: only the HAVE_LIBHEIF branch below reads HEIF
#endif

dt_colorspaces_color_profile_type_t dt_image_find_best_color_profile(int32_t imgid, cmsHPROFILE *output, gboolean *new_profile)
{
  // Note : when the image has already been opened from cache on the current session,
  // the embedded color profile is already inited and stored in img->profile.

  // Untagged images should be assumed to be sRGB.
  dt_colorspaces_color_profile_type_t color_profile = DT_COLORSPACE_SRGB;
  *new_profile = FALSE;

  /* The `goto finish` below (invalid imgid -> no cache entry) skips every assignment to
   * *output, including the sRGB fallback at the end, so the caller was left returning an
   * indeterminate pointer that reached cmsCreateTransform. Establish the contract here:
   * *output is always written. */
  if(!IS_NULL_PTR(output)) *output = NULL;

  // Fetch filename for extension retrieval
  char filename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid,  filename,  sizeof(filename),  &from_cache, __FUNCTION__);

  const gchar *cc = filename + strlen(filename);
  for(; *cc != '.' && cc > filename; cc--);
  gchar *ext = g_ascii_strdown(cc + 1, -1);

  // Fetch actual image
  dt_image_t *img = dt_image_cache_get(imgid, 'w');
  if(IS_NULL_PTR(img)) goto finish;

  // Image codecs doing their own colorspace detection should set this to TRUE
  gboolean already_set = FALSE;

  dt_print(DT_DEBUG_COLORPROFILE, "Color profile type for %s: \n", filename);

  /* Both of these CREATE a profile. Calling them once in the condition and again in the
   * body -- which is what this cascade did -- leaked the first one every time the branch
   * was taken. Create once, here, and hand the same object out below. The second is built
   * only if the first failed, which is what the else-if short-circuit used to express. */
  cmsHPROFILE embedded_icc = NULL;
  if(img->profile && img->profile_size > 0)
    embedded_icc = dt_colorspaces_get_rgb_profile_from_mem(img->profile, img->profile_size);

  cmsHPROFILE exif_matrix = NULL;
  if(IS_NULL_PTR(embedded_icc) && !isnan(img->d65_color_matrix[0]))
    exif_matrix = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])img->d65_color_matrix);

  if(!IS_NULL_PTR(embedded_icc))
  {
    // Fast path : we already extracted ICC before. ICC profile is already inside.
    color_profile = DT_COLORSPACE_EMBEDDED_ICC;
    if(!IS_NULL_PTR(output))
    {
      *output = embedded_icc;
      embedded_icc = NULL;   // handed over
      *new_profile = TRUE;
    }
    dt_print(DT_DEBUG_COLORPROFILE, "Embedded ICC profile (inline)\n");
  }
  else if(!IS_NULL_PTR(exif_matrix))
  {
    // DNG and others : matrix inside EXIF
    color_profile = DT_COLORSPACE_EMBEDDED_MATRIX;
    if(!IS_NULL_PTR(output))
    {
      *output = exif_matrix;
      exif_matrix = NULL;    // handed over
      *new_profile = TRUE;
    }
    dt_print(DT_DEBUG_COLORPROFILE, "Embedded EXIF matrix\n");
  }
  else if(dt_image_is_monochrome(img))
  {
    // Monochrome RAW - colorspace doesn't matter
    color_profile = DT_COLORSPACE_LIN_REC709;
    if(!IS_NULL_PTR(output))
      *output = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_ROLE_INPUT)->profile;
    dt_print(DT_DEBUG_COLORPROFILE, "Monochrome RAW\n");
  }
  else if(dt_image_is_matrix_correction_supported(img))
  {
    // Color RAW
    color_profile = DT_COLORSPACE_STANDARD_MATRIX;
    if(!IS_NULL_PTR(output))
    {
      *output = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])img->adobe_XYZ_to_CAM);
      *new_profile = TRUE;
    }
    dt_print(DT_DEBUG_COLORPROFILE, "Typical RAW\n");
  }
  else if(img->flags & DT_IMAGE_4BAYER)
  {
    // 4Bayer images have been pre-converted to rec2020
    color_profile = DT_COLORSPACE_LIN_REC2020;
    if(!IS_NULL_PTR(output))
      *output = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC2020, "", DT_PROFILE_ROLE_INPUT)->profile;
    dt_print(DT_DEBUG_COLORPROFILE, "4Bayer RAW\n");
  }
  else if(img->colorspace == DT_IMAGE_COLORSPACE_SRGB)
  {
    // Images tagged explicitely with sRGB flag
    color_profile = DT_COLORSPACE_SRGB;
    dt_print(DT_DEBUG_COLORPROFILE, "Raster image tagged with sRGB\n");
  }
  else if(img->colorspace == DT_IMAGE_COLORSPACE_ADOBE_RGB)
  {
    // Images tagged explicitely with Adobe RGB flag
    color_profile = DT_COLORSPACE_ADOBERGB;
    if(!IS_NULL_PTR(output))
      *output = dt_colorspaces_get_profile(DT_COLORSPACE_ADOBERGB, "", DT_PROFILE_ROLE_INPUT)->profile;
    dt_print(DT_DEBUG_COLORPROFILE, "Raster image tagged with Adobe RGB\n");
  }
  else if(!strcmp(ext, "pfm"))
  {
    // PFM have no embedded color profile nor ICC tag, we can't know the color space
    // but we can assume the are linear since it's a floating point format
    color_profile = DT_COLORSPACE_LIN_REC709;
    if(!IS_NULL_PTR(output))
      *output = dt_colorspaces_get_profile(DT_COLORSPACE_LIN_REC709, "", DT_PROFILE_ROLE_INPUT)->profile;
    dt_print(DT_DEBUG_COLORPROFILE, "PFM untagged image\n");
  }
  else
  {
    // Images that need codecs.

    // First, extract embedded profiles from headers.
    // Done only once : if everything goes well, the next time we access this image from cache,
    // we will read img->profile directly (first branch here).

    if(!strcmp(ext, "jpg") || !strcmp(ext, "jpeg"))
    {
      dt_imageio_jpeg_t jpg;
      if(!dt_imageio_jpeg_read_header(filename, &jpg))
        img->profile_size = dt_imageio_jpeg_read_profile(&jpg, &img->profile);
    }
#ifdef HAVE_OPENJPEG
    else if(!strcmp(ext, "jp2") || !strcmp(ext, "j2k") || !strcmp(ext, "j2c") || !strcmp(ext, "jpc"))
    {
      img->profile_size = dt_imageio_j2k_read_profile(filename, &img->profile);
    }
#endif
    else if((!strcmp(ext, "tif") || !strcmp(ext, "tiff")))
    {
      img->profile_size = dt_imageio_tiff_read_profile(filename, &img->profile);
    }
    else if(!strcmp(ext, "png"))
    {
      img->profile_size = dt_imageio_png_read_profile(filename, &img->profile);
    }
#ifdef HAVE_LIBAVIF
    else if(!strcmp(ext, "avif"))
    {
      dt_colorspaces_cicp_t cicp;
      img->profile_size = dt_imageio_avif_read_profile(filename, &img->profile, &cicp);

      // try the nclx box before falling back to any ICC profile
      color_profile = dt_colorspaces_cicp_to_type(&cicp, filename);

      // If we found a basic RGB colorspace from private AVIF metadata,
      // bypass generic LCMS2 reading below
      if(color_profile != DT_COLORSPACE_NONE) already_set = TRUE;
    }
#endif
#ifdef HAVE_LIBHEIF
    else if(!strcmp(ext, "heif") || !strcmp(ext, "heic") || !strcmp(ext, "hif"))
    {
      dt_colorspaces_cicp_t cicp;
      img->profile_size = dt_imageio_heif_read_profile(filename, &img->profile, &cicp);

      // try the nclx box before falling back to any ICC profile
      color_profile = dt_colorspaces_cicp_to_type(&cicp, filename);

      // If we found a basic RGB colorspace from private AVIF metadata,
      // bypass generic LCMS2 reading below
      if(color_profile != DT_COLORSPACE_NONE) already_set = TRUE;
    }
#endif

    // Finally, read the prepared embedded profile
    /* Same double-create as above: build it once. */
    cmsHPROFILE extracted_icc = NULL;
    if(!already_set && img->profile && img->profile_size > 0)
      extracted_icc = dt_colorspaces_get_rgb_profile_from_mem(img->profile, img->profile_size);

    if(!IS_NULL_PTR(extracted_icc))
    {
      color_profile = DT_COLORSPACE_EMBEDDED_ICC;
      if(!IS_NULL_PTR(output))
      {
        *output = extracted_icc;
        extracted_icc = NULL;   // handed over
        *new_profile = TRUE;
      }
      dt_print(DT_DEBUG_COLORPROFILE, "Embedded ICC (extracted)\n");
    }
    else if(already_set && img->profile && img->profile_size > 0)
    {
      // This happens when AVIF/HEIF found a basic color profile into CICP fields
      if(!IS_NULL_PTR(output))
        *output = dt_colorspaces_get_profile(color_profile, "", DT_PROFILE_ROLE_INPUT)->profile;
      dt_print(DT_DEBUG_COLORPROFILE, "Embedded ICC (extracted)\n");
    }

    if(!IS_NULL_PTR(extracted_icc)) dt_colorspaces_cleanup_profile(extracted_icc);
  }

  // Handle the fallback to sRGB space
  if(color_profile == DT_COLORSPACE_NONE) color_profile = DT_COLORSPACE_SRGB;
  if(color_profile == DT_COLORSPACE_SRGB && !IS_NULL_PTR(output))
    *output = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_ROLE_INPUT)->profile;

  /* Anything built above but not handed to the caller is ours to close. This is the
   * `output == NULL` case -- a caller that wants only the resolved type -- and the branches
   * where a later one in the cascade won. Deliberately before the label: the `goto finish`
   * path runs before either is created. */
  if(!IS_NULL_PTR(embedded_icc)) dt_colorspaces_cleanup_profile(embedded_icc);
  if(!IS_NULL_PTR(exif_matrix)) dt_colorspaces_cleanup_profile(exif_matrix);

finish:
  dt_image_cache_write_release(img, DT_IMAGE_CACHE_RELAXED);
  dt_free(ext);
  return color_profile;
}
dt_colorspaces_color_profile_type_t dt_colorspaces_get_input_profile_from_image(
    int32_t imgid,
    dt_colorspaces_color_profile_type_t requested,
    cmsHPROFILE *output,
    gboolean *new_profile)
{
  if(output) *output = NULL;
  if(new_profile) *new_profile = FALSE;

  if(requested == DT_COLORSPACE_NONE)
    return dt_image_find_best_color_profile(imgid, output, new_profile);

  if(requested != DT_COLORSPACE_EMBEDDED_ICC
     && requested != DT_COLORSPACE_EMBEDDED_MATRIX
     && requested != DT_COLORSPACE_STANDARD_MATRIX)
    return DT_COLORSPACE_NONE;

  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img)) return DT_COLORSPACE_NONE;

  if(!dt_image_is_matrix_correction_supported(img))
  {
    dt_image_cache_read_release(img);
    return dt_image_find_best_color_profile(imgid, output, new_profile);
  }

  gboolean have_embedded_icc = (img->profile && img->profile_size > 0);
  dt_image_cache_read_release(img);

  if(requested == DT_COLORSPACE_EMBEDDED_ICC && !have_embedded_icc)
  {
    // Try to extract embedded ICC into cache if needed.
    gboolean dummy_new_profile = FALSE;
    dt_image_find_best_color_profile(imgid, NULL, &dummy_new_profile);
  }

  img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img)) return DT_COLORSPACE_NONE;

  cmsHPROFILE profile = NULL;
  dt_colorspaces_color_profile_type_t type = requested;

  if(type == DT_COLORSPACE_EMBEDDED_ICC)
  {
    if(img->profile && img->profile_size > 0)
    {
      profile = dt_colorspaces_get_rgb_profile_from_mem(img->profile, img->profile_size);
      if(profile)
      {
        type = DT_COLORSPACE_EMBEDDED_ICC;
        goto finish;
      }
    }
    type = DT_COLORSPACE_EMBEDDED_MATRIX;
  }

  if(type == DT_COLORSPACE_EMBEDDED_MATRIX)
  {
    if(!isnan(img->d65_color_matrix[0]))
    {
      profile = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])img->d65_color_matrix);
      if(profile)
      {
        type = DT_COLORSPACE_EMBEDDED_MATRIX;
        goto finish;
      }
    }
    type = DT_COLORSPACE_STANDARD_MATRIX;
  }

  if(type == DT_COLORSPACE_STANDARD_MATRIX)
  {
    if(!isnan(img->adobe_XYZ_to_CAM[0][0]))
    {
      profile = dt_colorspaces_create_xyzimatrix_profile((float(*)[3])img->adobe_XYZ_to_CAM);
      if(profile)
      {
        type = DT_COLORSPACE_STANDARD_MATRIX;
        goto finish;
      }
    }
  }

  type = DT_COLORSPACE_LIN_REC709;

finish:
  dt_image_cache_read_release(img);

  if(profile)
  {
    if(output)
    {
      *output = profile;
      if(new_profile) *new_profile = TRUE;
    }
    else
    {
      dt_colorspaces_cleanup_profile(profile);
    }
  }

  return type;
}
const cmsHPROFILE dt_colorspaces_get_embedded_profile(const int32_t imgid, dt_colorspaces_color_profile_type_t *type, gboolean *new_profile)
{
  cmsHPROFILE output = NULL;
  *type = dt_image_find_best_color_profile(imgid, &output, new_profile);
  return output;
}
/* Build (or reuse) the parsed profile for an image's embedded ICC, and give it to the image.
 *
 * It used to be appended to dt_colorspaces_t.profiles instead. That list is built once by
 * dt_colorspaces_init() and then read from ~23 places without any lock, which is only safe
 * while it is immutable -- and this function made it mutable, from export jobs that run in
 * parallel. It also grew the list for the lifetime of the session (one entry per exported
 * image), and leaked the container outright whenever the profile was not newly created,
 * because only the new_profile branch ever registered it.
 *
 * An embedded profile is a property of one image, so the image owns it: stored under the image
 * cache entry's own lock, freed when the image is evicted, and reused on the next export of the
 * same image instead of built again.
 */
static const dt_colorspaces_color_profile_t *_build_embedded_profile(const int32_t imgid,
                                                                     dt_colorspaces_color_profile_type_t *type)
{
  gboolean new_profile = FALSE;
  cmsHPROFILE profile = dt_colorspaces_get_embedded_profile(imgid, type, &new_profile);

  // -1 in all indices ensures it is hidden from the GUI: this profile belongs to one image and
  // has no place in any combo box.
  /* new_profile carries ownership: several branches of dt_image_find_best_color_profile()
   * return a profile borrowed from the application-wide list rather than a fresh one, and a
   * container that closed such a profile would double-free it. */
  dt_colorspaces_color_profile_t *container = dt_colorspaces_new_image_profile(*type, profile, new_profile);
  if(IS_NULL_PTR(container))
  {
    if(profile && new_profile) dt_colorspaces_cleanup_profile(profile);
    return NULL;
  }

  if(profile && new_profile)
  {
    char *lang = getenv("LANG");
    if(IS_NULL_PTR(lang)) lang = "en_US";
    dt_colorspaces_get_profile_name(profile, lang, lang + 3, container->name, sizeof(container->name));
  }

  dt_image_t *img = dt_image_cache_get(imgid, 'w');
  if(IS_NULL_PTR(img))
  {
    // No image to hand it to. Better to leak nothing and let the caller fall back than to put
    // it back on the global list.
    dt_colorspaces_free_image_profile(container);
    return NULL;
  }

  const dt_colorspaces_color_profile_t *result;
  if(img->embedded_profile)
  {
    // Another thread got here first while we were parsing. Theirs is as good as ours, and it is
    // the one every other borrower already holds.
    dt_colorspaces_free_image_profile(container);
    result = img->embedded_profile;
  }
  else
  {
    img->embedded_profile = container;
    result = container;
  }
  // Nothing persistent changed: this is a cached derivation of bytes already in the image.
  dt_image_cache_write_release(img, DT_IMAGE_CACHE_RELAXED);

  return result;
}
struct dt_colorspaces_color_profile_t *dt_image_get_input_profile(
    const int32_t imgid,
    const dt_colorspaces_color_profile_type_t requested,
    const char *camera_makermodel,
    dt_colorspaces_color_profile_type_t *resolved)
{
  if(resolved) *resolved = DT_COLORSPACE_NONE;

  dt_colorspaces_color_profile_type_t type = requested;
  cmsHPROFILE profile = NULL;
  gboolean owns = FALSE;

  /* The three built-from-the-camera types. Each falls over to the embedded-ICC branch when
   * this camera is not in the corresponding table -- which is what "matrix not found" means
   * to a user, and why the fallthrough below is a chain rather than a switch. */
  if(type == DT_COLORSPACE_ENHANCED_MATRIX)
  {
    profile = camera_makermodel ? dt_colorspaces_create_darktable_profile(camera_makermodel) : NULL;
    if(IS_NULL_PTR(profile)) type = DT_COLORSPACE_EMBEDDED_ICC;
    else owns = TRUE;
  }
  if(type == DT_COLORSPACE_VENDOR_MATRIX)
  {
    profile = camera_makermodel ? dt_colorspaces_create_vendor_profile(camera_makermodel) : NULL;
    if(IS_NULL_PTR(profile)) type = DT_COLORSPACE_EMBEDDED_ICC;
    else owns = TRUE;
  }
  if(type == DT_COLORSPACE_ALTERNATE_MATRIX)
  {
    profile = camera_makermodel ? dt_colorspaces_create_alternate_profile(camera_makermodel) : NULL;
    if(IS_NULL_PTR(profile)) type = DT_COLORSPACE_EMBEDDED_ICC;
    else owns = TRUE;
  }

  if(IS_NULL_PTR(profile)
     && (type == DT_COLORSPACE_EMBEDDED_ICC || type == DT_COLORSPACE_EMBEDDED_MATRIX
         || type == DT_COLORSPACE_STANDARD_MATRIX))
  {
    gboolean new_profile = FALSE;
    type = dt_colorspaces_get_input_profile_from_image(imgid, type, &profile, &new_profile);
    owns = new_profile;
  }

  if(IS_NULL_PTR(profile)) return NULL;

  struct dt_colorspaces_color_profile_t *container = dt_colorspaces_new_image_profile(type, profile, owns);
  if(IS_NULL_PTR(container))
  {
    /* Do not leak the handle just because the container allocation failed: only WE know at
     * this point whether it was created here or borrowed from the application list. */
    if(owns) dt_colorspaces_cleanup_profile(profile);
    return NULL;
  }

  if(resolved) *resolved = type;
  return container;
}

struct dt_colorspaces_color_profile_t *dt_image_get_embedded_output_profile(
    const int32_t imgid, dt_colorspaces_color_profile_type_t *type)
{
  if(IS_NULL_PTR(type)) return NULL;

  gboolean new_profile = FALSE;
  cmsHPROFILE profile = dt_colorspaces_get_embedded_profile(imgid, type, &new_profile);
  if(IS_NULL_PTR(profile)) return NULL;

  struct dt_colorspaces_color_profile_t *container
      = dt_colorspaces_new_image_profile(*type, profile, new_profile);
  if(IS_NULL_PTR(container) && new_profile) dt_colorspaces_cleanup_profile(profile);
  return container;
}

const dt_colorspaces_color_profile_t *dt_colorspaces_get_output_profile(const int32_t imgid,
                                                                        dt_colorspaces_color_profile_type_t *over_type,
                                                                        const char *over_filename)
{

  const dt_colorspaces_color_profile_t *p = NULL;

  // Special case if output is undefined or uses private image color spaces : use the embedded profile if any.
  // We need to read it from the original image and create it on-the-fly.
  // Note: we don't allow export with deprecated vendor/enhanced/alternate matrices
  if(*over_type == DT_COLORSPACE_NONE ||
     *over_type == DT_COLORSPACE_EMBEDDED_ICC ||
     *over_type == DT_COLORSPACE_STANDARD_MATRIX ||
     *over_type == DT_COLORSPACE_EMBEDDED_MATRIX)
  {
    p = _build_embedded_profile(imgid, over_type);
  }
  else
  {
    // return a pointer to the profile specified in export.
    // we have that in here to get rid of the if() check in all places calling this function.
    p = dt_colorspaces_get_profile(*over_type, over_filename, DT_PROFILE_ROLE_OUTPUT | DT_PROFILE_ROLE_MONITOR);
  }

  // if all else fails -> fall back to sRGB
  if(IS_NULL_PTR(p))
  {
    p = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_ROLE_OUTPUT);
    *over_type = DT_COLORSPACE_SRGB;
  }

  return p;
}
dt_colorspaces_color_profile_type_t dt_colorspaces_cicp_to_type(const dt_colorspaces_cicp_t *cicp, const char *filename)
{
  switch(cicp->color_primaries)
  {
    /* Give up immediately if unspecified */
    case DT_CICP_COLOR_PRIMARIES_UNSPECIFIED:
      if(cicp->transfer_characteristics == DT_CICP_TRANSFER_CHARACTERISTICS_UNSPECIFIED
         && cicp->matrix_coefficients == DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED)
        return DT_COLORSPACE_NONE;
      break; /* unspecified */

    /* REC709 */
    case DT_CICP_COLOR_PRIMARIES_REC709:

      switch(cicp->transfer_characteristics)
      {
        /* SRGB */
        case DT_CICP_TRANSFER_CHARACTERISTICS_SRGB:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC709:
            case DT_CICP_MATRIX_COEFFICIENTS_SYCC:
            case DT_CICP_MATRIX_COEFFICIENTS_REC601: /* support equivalents just in case of mistagging */
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL: /* support incorrectly tagged files */
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_SRGB;
            default:
              break;
          }

          break; /* SRGB */

        /* REC709 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_REC709:
        case DT_CICP_TRANSFER_CHARACTERISTICS_REC601:      /* support equivalents just in case of mistagging */
        case DT_CICP_TRANSFER_CHARACTERISTICS_REC2020_10B: /* support equivalents just in case of mistagging */
        case DT_CICP_TRANSFER_CHARACTERISTICS_REC2020_12B: /* support equivalents just in case of mistagging */

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC709:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_REC709;
            default:
              break;
          }

          break; /* REC709 */

        /* LINEAR REC709 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC709:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_LIN_REC709;
            default:
              break;
          }

          break; /* LINEAR REC709 */

        default:
          break;
      }

      break; /* REC709 */

    /* REC2020 */
    case DT_CICP_COLOR_PRIMARIES_REC2020:

      switch(cicp->transfer_characteristics)
      {
        /* LINEAR REC2020 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC2020_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_LIN_REC2020;
            default:
              break;
          }

          break; /* LINEAR REC2020 */

        /* PQ REC2020 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_PQ:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC2020_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_PQ_REC2020;
            default:
              break;
          }

          break; /* PQ REC2020 */

        /* HLG REC2020 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_HLG:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC2020_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_HLG_REC2020;
            default:
              break;
          }

          break; /* HLG REC2020 */

        default:
          break;
      }

      break; /* REC2020 */

    /* P3 */
    case DT_CICP_COLOR_PRIMARIES_P3:

      switch(cicp->transfer_characteristics)
      {
        /* PQ P3 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_PQ:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC709:
            case DT_CICP_MATRIX_COEFFICIENTS_SYCC:
            case DT_CICP_MATRIX_COEFFICIENTS_REC601:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_PQ_P3;
            default:
              break;
          }

          break; /* PQ P3 */

        /* HLG P3 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_HLG:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC709:
            case DT_CICP_MATRIX_COEFFICIENTS_SYCC:
            case DT_CICP_MATRIX_COEFFICIENTS_REC601:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_HLG_P3;
            default:
              break;
          }

          break; /* HLG P3 */

        /* Display P3 */
        case DT_CICP_TRANSFER_CHARACTERISTICS_SRGB:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY: /* support RGB (4:4:4 or lossless) */
            case DT_CICP_MATRIX_COEFFICIENTS_REC709:
            case DT_CICP_MATRIX_COEFFICIENTS_SYCC:
            case DT_CICP_MATRIX_COEFFICIENTS_REC601:
            case DT_CICP_MATRIX_COEFFICIENTS_CHROMA_DERIVED_NCL:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_DISPLAY_P3;
            default:
              break;
          }

          break; /* Display P3 */

        default:
          break;
      }

      break; /* P3 */

    /* XYZ */
    case DT_CICP_COLOR_PRIMARIES_XYZ:

      switch(cicp->transfer_characteristics)
      {
        /* LINEAR XYZ */
        case DT_CICP_TRANSFER_CHARACTERISTICS_LINEAR:

          switch(cicp->matrix_coefficients)
          {
            case DT_CICP_MATRIX_COEFFICIENTS_IDENTITY:
            case DT_CICP_MATRIX_COEFFICIENTS_UNSPECIFIED:
              return DT_COLORSPACE_XYZ;
            default:
              break;
          }

          break; /* LINEAR XYZ */

        default:
          break;
      }

      break; /* XYZ */

    default:
      break;
  }

  if(!IS_NULL_PTR(filename))
    dt_print(DT_DEBUG_IMAGEIO, "[colorin] unsupported CICP color profile for `%s': %d/%d/%d\n", filename,
             cicp->color_primaries, cicp->transfer_characteristics, cicp->matrix_coefficients);

  return DT_COLORSPACE_NONE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
