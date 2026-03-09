/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "iop/drawlayer/io.h"

#include "common/colorspaces.h"
#include "common/image.h"

#include <glib/gstdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

/** @file
 *  @brief TIFF sidecar I/O implementation for drawlayer layers.
 */

/** @brief Clamp scalar to [0,1]. */
static inline float _clamp01(const float value)
{
  return fminf(fmaxf(value, 0.0f), 1.0f);
}

typedef union dt_drawlayer_io_fp32_t
{
  uint32_t u;
  float f;
} dt_drawlayer_io_fp32_t;

/** @brief Convert IEEE-754 binary16 to float32. */
static inline float _half_to_float(const uint16_t h)
{
  static const dt_drawlayer_io_fp32_t magic = { 113u << 23 };
  static const uint32_t shifted_exp = 0x7c00u << 13;
  dt_drawlayer_io_fp32_t out;

  out.u = (h & 0x7fffu) << 13;
  const uint32_t exp = shifted_exp & out.u;
  out.u += (127u - 15u) << 23;

  if(exp == shifted_exp)
    out.u += (128u - 16u) << 23;
  else if(exp == 0)
  {
    out.u += 1u << 23;
    out.f -= magic.f;
  }

  out.u |= (h & 0x8000u) << 16;
  return out.f;
}

/** @brief Convert float32 to IEEE-754 binary16 with rounding. */
static inline uint16_t _float_to_half(float value)
{
  dt_drawlayer_io_fp32_t in = { .f = value };
  const uint32_t sign = (in.u >> 16) & 0x8000u;
  const uint32_t exponent = (in.u >> 23) & 0xffu;
  uint32_t mantissa = in.u & 0x007fffffu;

  if(exponent == 0xffu)
  {
    if(mantissa) return (uint16_t)(sign | 0x7e00u);
    return (uint16_t)(sign | 0x7c00u);
  }

  const int32_t half_exponent = (int32_t)exponent - 127 + 15;

  if(half_exponent >= 0x1f) return (uint16_t)(sign | 0x7c00u);
  if(half_exponent <= 0)
  {
    if(half_exponent < -10) return (uint16_t)sign;

    mantissa |= 0x00800000u;
    const uint32_t shift = (uint32_t)(1 - half_exponent);
    uint32_t half_mantissa = mantissa >> (shift + 13);
    const uint32_t round_bit = 1u << (shift + 12);

    if((mantissa & round_bit) && ((mantissa & (round_bit - 1u)) || (half_mantissa & 1u)))
      half_mantissa++;

    return (uint16_t)(sign | half_mantissa);
  }

  uint32_t half_exp = (uint32_t)half_exponent << 10;
  uint32_t half_mantissa = mantissa >> 13;

  if((mantissa & 0x00001000u) && ((mantissa & 0x00001fffu) || (half_mantissa & 1u)))
  {
    half_mantissa++;

    if(half_mantissa == 0x0400u)
    {
      half_mantissa = 0;
      half_exp += 0x0400u;
      if(half_exp >= 0x7c00u) return (uint16_t)(sign | 0x7c00u);
    }
  }

  return (uint16_t)(sign | half_exp | half_mantissa);
}

/** @brief Clear uint16 RGBA row/buffer to transparent black. */
static inline void _clear_transparent_half(uint16_t *pixels, const size_t pixel_count)
{
  if(!pixels) return;
  memset(pixels, 0, pixel_count * 4 * sizeof(uint16_t));
}

/** @brief Clear float RGBA row/buffer to transparent black. */
static inline void dt_drawlayer_cache_clear_transparent_float(float *pixels, const size_t pixel_count)
{
  if(!pixels) return;
  memset(pixels, 0, pixel_count * 4 * sizeof(float));
}

/** @brief Load one half-float RGBA texel to float RGBA. */
static inline void _load_half_pixel_rgba(const uint16_t *src, float rgba[4])
{
  rgba[0] = _half_to_float(src[0]);
  rgba[1] = _half_to_float(src[1]);
  rgba[2] = _half_to_float(src[2]);
  rgba[3] = _half_to_float(src[3]);
}

/** @brief Resolve embedded ICC blob from serialized work-profile key. */
static gboolean _icc_blob_from_profile_key(const char *work_profile, uint8_t **icc_data, uint32_t *icc_len)
{
  if(icc_data) *icc_data = NULL;
  if(icc_len) *icc_len = 0;
  if(!work_profile || work_profile[0] == '\0' || !icc_data || !icc_len) return FALSE;

  const char *sep0 = strchr(work_profile, '|');
  if(!sep0) return FALSE;
  const char *sep1 = strchr(sep0 + 1, '|');
  if(!sep1) return FALSE;

  char *endptr = NULL;
  const long type_long = strtol(work_profile, &endptr, 10);
  if(endptr != sep0) return FALSE;

  const dt_colorspaces_color_profile_type_t type = (dt_colorspaces_color_profile_type_t)type_long;
  const char *filename = sep1 + 1;
  const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_ANY);
  if(!profile || !profile->profile) return FALSE;

  cmsUInt32Number len = 0;
  cmsSaveProfileToMem(profile->profile, NULL, &len);
  if(len == 0) return FALSE;

  uint8_t *buf = g_malloc(len);
  if(!buf) return FALSE;

  if(!cmsSaveProfileToMem(profile->profile, buf, &len) || len == 0)
  {
    dt_free(buf);
    return FALSE;
  }

  *icc_data = buf;
  *icc_len = (uint32_t)len;
  return TRUE;
}

/** @brief Write TIFF directory tags for one RGBA half-float page. */
static gboolean _set_directory_tags(TIFF *tiff, const uint32_t width, const uint32_t height, const char *name,
                                    const char *work_profile)
{
  const uint16_t extrasamples[] = { EXTRASAMPLE_ASSOCALPHA };

  if(!TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 4)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, extrasamples)) return FALSE;

  if(!TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE)
     && !TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE)
     && !TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_LZW))
    TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);

  TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff, 0));
  if(name && name[0]) TIFFSetField(tiff, TIFFTAG_PAGENAME, name);
  if(work_profile && work_profile[0]) TIFFSetField(tiff, TIFFTAG_IMAGEDESCRIPTION, work_profile);
  return TRUE;
}

/** @brief Read one TIFF scanline into half-float RGBA buffer. */
static gboolean _read_scanline_rgba(TIFF *tiff, const uint32_t width, const uint32_t row, uint16_t *out)
{
  uint16_t bpp = 0;
  uint16_t spp = 0;
  uint16_t sampleformat = SAMPLEFORMAT_UINT;

  TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bpp);
  TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleformat);

  if(spp != 4) return FALSE;

  const tsize_t scanline = TIFFScanlineSize(tiff);
  tdata_t buffer = _TIFFmalloc((tsize_t)scanline);
  if(!buffer) return FALSE;

  const int ok = TIFFReadScanline(tiff, buffer, row, 0);
  if(ok == -1)
  {
    _TIFFfree(buffer);
    return FALSE;
  }

  if(bpp == 16 && sampleformat == SAMPLEFORMAT_IEEEFP)
    memcpy(out, buffer, (size_t)width * 4 * sizeof(uint16_t));
  else
  {
    _TIFFfree(buffer);
    return FALSE;
  }

  _TIFFfree(buffer);
  return TRUE;
}

/** @brief Write one half-float RGBA scanline into TIFF page. */
static gboolean _write_scanline_rgba(TIFF *tiff, const uint32_t row, const uint16_t *in)
{
  return TIFFWriteScanline(tiff, (tdata_t)in, row, 0) != -1;
}

/** @brief Overlay one clipped float patch span into a half-float TIFF row. */
static void _overlay_patch_row_rgba(uint16_t *dst_row, const uint32_t width, const int offset_x,
                                    const int raw_y, const dt_drawlayer_io_patch_t *patch)
{
  if(!dst_row || !patch || !patch->pixels || raw_y < patch->y || raw_y >= patch->y + patch->height) return;

  const int dst_x0 = MAX(0, patch->x - offset_x);
  const int dst_x1 = MIN((int)width, patch->x + patch->width - offset_x);
  if(dst_x0 >= dst_x1) return;

  const float *patch_row = patch->pixels + 4 * (size_t)(raw_y - patch->y) * patch->width;

#ifdef _OPENMP
  #pragma omp parallel for simd default(none) schedule(static) shared(dst_row, patch_row) firstprivate(dst_x0, dst_x1, offset_x, patch)
#endif
  for(int dst_x = dst_x0; dst_x < dst_x1; dst_x++)
  {
    const float *src_pixel = patch_row + 4 * (size_t)(dst_x + offset_x - patch->x);
    dst_row[4 * dst_x + 0] = _float_to_half(src_pixel[0]);
    dst_row[4 * dst_x + 1] = _float_to_half(src_pixel[1]);
    dst_row[4 * dst_x + 2] = _float_to_half(src_pixel[2]);
    dst_row[4 * dst_x + 3] = _float_to_half(src_pixel[3]);
  }
}

/** @brief Scan TIFF directories and resolve best match for name/order target. */
static void _scan_directories(TIFF *tiff, const char *target_name, const int target_order,
                              dt_drawlayer_io_layer_info_t *info)
{
  memset(info, 0, sizeof(*info));
  info->index = -1;

  if(!tiff) return;
  if(!TIFFSetDirectory(tiff, 0)) return;

  const gboolean has_target_name = (target_name && target_name[0] != '\0');
  const gboolean has_target_order = (target_order >= 0);
  gboolean exact_found = FALSE;
  gboolean named_found = FALSE;
  gboolean order_found = FALSE;
  dt_drawlayer_io_layer_info_t exact = { 0 };
  dt_drawlayer_io_layer_info_t named = { 0 };
  dt_drawlayer_io_layer_info_t ordered = { 0 };
  exact.index = -1;
  named.index = -1;
  ordered.index = -1;

  int index = 0;
  do
  {
    char *page_name = NULL;
    char *page_profile = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(tiff, TIFFTAG_PAGENAME, &page_name);
    TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &page_profile);

    const gboolean order_match = (has_target_order && index == target_order);
    const gboolean name_match = (has_target_name && !g_strcmp0(page_name ? page_name : "", target_name));
    const gboolean exact_match = (name_match && order_match);

    if(exact_match && !exact_found)
    {
      exact_found = TRUE;
      exact.found = TRUE;
      exact.index = index;
      exact.width = width;
      exact.height = height;
      g_strlcpy(exact.name, page_name ? page_name : "", sizeof(exact.name));
      g_strlcpy(exact.work_profile, page_profile ? page_profile : "", sizeof(exact.work_profile));
    }

    if(name_match && !named_found)
    {
      named_found = TRUE;
      named.found = TRUE;
      named.index = index;
      named.width = width;
      named.height = height;
      g_strlcpy(named.name, page_name ? page_name : "", sizeof(named.name));
      g_strlcpy(named.work_profile, page_profile ? page_profile : "", sizeof(named.work_profile));
    }

    if(order_match && !order_found)
    {
      order_found = TRUE;
      ordered.found = TRUE;
      ordered.index = index;
      ordered.width = width;
      ordered.height = height;
      g_strlcpy(ordered.name, page_name ? page_name : "", sizeof(ordered.name));
      g_strlcpy(ordered.work_profile, page_profile ? page_profile : "", sizeof(ordered.work_profile));
    }

    index++;
  } while(TIFFReadDirectory(tiff));

  if(exact_found)
  {
    info->found = TRUE;
    info->index = exact.index;
    info->width = exact.width;
    info->height = exact.height;
    g_strlcpy(info->name, exact.name, sizeof(info->name));
    g_strlcpy(info->work_profile, exact.work_profile, sizeof(info->work_profile));
  }
  else if(named_found)
  {
    info->found = TRUE;
    info->index = named.index;
    info->width = named.width;
    info->height = named.height;
    g_strlcpy(info->name, named.name, sizeof(info->name));
    g_strlcpy(info->work_profile, named.work_profile, sizeof(info->work_profile));
  }
  else if(order_found)
  {
    info->found = TRUE;
    info->index = ordered.index;
    info->width = ordered.width;
    info->height = ordered.height;
    g_strlcpy(info->name, ordered.name, sizeof(info->name));
    g_strlcpy(info->work_profile, ordered.work_profile, sizeof(info->work_profile));
  }

  info->count = index;
  TIFFSetDirectory(tiff, 0);
}

/** @brief Write one output page, optionally copying from source and patch overlay. */
static gboolean _write_page(TIFF *dst, TIFF *src, const int src_dir, const char *name, const char *work_profile,
                            const dt_drawlayer_io_patch_t *patch, const int raw_width, const int raw_height)
{
  uint32_t width = (uint32_t)raw_width;
  uint32_t height = (uint32_t)raw_height;
  const char *page_profile = work_profile;
  uint8_t *icc_profile = NULL;
  uint32_t icc_profile_len = 0;

  if(src)
  {
    if(!TIFFSetDirectory(src, (tdir_t)src_dir)) return FALSE;
    TIFFGetField(src, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(src, TIFFTAG_IMAGELENGTH, &height);
    if(!name)
    {
      char *src_name = NULL;
      TIFFGetField(src, TIFFTAG_PAGENAME, &src_name);
      name = src_name;
    }
    if(!page_profile)
    {
      char *src_profile = NULL;
      TIFFGetField(src, TIFFTAG_IMAGEDESCRIPTION, &src_profile);
      page_profile = src_profile;
    }
  }

  if(!_set_directory_tags(dst, width, height, name, page_profile)) return FALSE;
  if(page_profile && page_profile[0] && _icc_blob_from_profile_key(page_profile, &icc_profile, &icc_profile_len))
    TIFFSetField(dst, TIFFTAG_ICCPROFILE, icc_profile_len, icc_profile);

  uint16_t *src_row = g_malloc_n((gsize)width * 4, sizeof(uint16_t));
  uint16_t *dst_row = g_malloc_n((gsize)width * 4, sizeof(uint16_t));
  if(!src_row || !dst_row)
  {
    dt_free(icc_profile);
    dt_free(src_row);
    dt_free(dst_row);
    return FALSE;
  }

  const int offset_x = (raw_width - (int)width) / 2;
  const int offset_y = (raw_height - (int)height) / 2;

  for(uint32_t y = 0; y < height; y++)
  {
    if(src)
    {
      if(!_read_scanline_rgba(src, width, y, src_row))
      {
        dt_free(icc_profile);
        dt_free(src_row);
        dt_free(dst_row);
        return FALSE;
      }
      memcpy(dst_row, src_row, (size_t)width * 4 * sizeof(uint16_t));
    }
    else
      _clear_transparent_half(dst_row, (size_t)width);

    if(patch)
    {
      const int raw_y = (int)y + offset_y;
      _overlay_patch_row_rgba(dst_row, width, offset_x, raw_y, patch);
    }

    if(!_write_scanline_rgba(dst, y, dst_row))
    {
      dt_free(icc_profile);
      dt_free(src_row);
      dt_free(dst_row);
      return FALSE;
    }
  }

  dt_free(icc_profile);
  dt_free(src_row);
  dt_free(dst_row);
  return TIFFWriteDirectory(dst) != 0;
}

/** @brief Rewrite sidecar with optional update/insert/delete of one target layer. */
static gboolean _rewrite_sidecar(const char *path, const char *target_name, const int target_order,
                                 const char *work_profile, const dt_drawlayer_io_patch_t *patch, const int raw_width,
                                 const int raw_height, const gboolean delete_target, const int insert_order,
                                 int *final_order)
{
  if(final_order) *final_order = -1;

  TIFF *src = NULL;
  dt_drawlayer_io_layer_info_t info;
  memset(&info, 0, sizeof(info));
  info.index = -1;

  if(g_file_test(path, G_FILE_TEST_EXISTS))
  {
    src = TIFFOpen(path, "rb");
    if(!src) return FALSE;
    _scan_directories(src, target_name, target_order, &info);
  }

  if(delete_target && (!src || !info.found))
  {
    if(src) TIFFClose(src);
    return TRUE;
  }

  if(delete_target && src && info.found && info.count == 1)
  {
    TIFFClose(src);
    return g_unlink(path) == 0;
  }

  gchar *tmp_path = g_strdup_printf("%s.tmp", path);
  TIFF *dst = TIFFOpen(tmp_path, "wb");
  if(!dst)
  {
    if(src) TIFFClose(src);
    dt_free(tmp_path);
    return FALSE;
  }

  int written_index = 0;
  gboolean ok = TRUE;

  if(src)
  {
    for(int dir = 0; dir < info.count && ok; dir++)
    {
      if(!delete_target && !info.found && insert_order >= 0 && written_index == insert_order)
      {
        ok = _write_page(dst, NULL, -1, target_name, work_profile, patch, raw_width, raw_height);
        if(ok && final_order) *final_order = written_index;
        if(ok) written_index++;
      }

      const gboolean is_target = info.found && dir == info.index;
      if(is_target && delete_target) continue;

      const dt_drawlayer_io_patch_t *page_patch = (is_target && !delete_target) ? patch : NULL;
      const char *page_label = is_target ? target_name : NULL;

      ok = _write_page(dst, src, dir, page_label, is_target ? work_profile : NULL, page_patch, raw_width, raw_height);
      if(ok && is_target && !delete_target && final_order) *final_order = written_index;
      if(ok) written_index++;
    }
  }

  if(ok && !delete_target && (!src || !info.found))
  {
    const gboolean append_at_end = (insert_order < 0 || insert_order >= written_index || !src);
    if(append_at_end)
    {
      ok = _write_page(dst, NULL, -1, target_name, work_profile, patch, raw_width, raw_height);
      if(ok && final_order) *final_order = written_index;
    }
  }

  TIFFClose(dst);
  if(src) TIFFClose(src);

  if(ok)
    ok = (g_rename(tmp_path, path) == 0);
  else
    g_unlink(tmp_path);

  dt_free(tmp_path);
  return ok;
}

/** @brief Test if a layer name already exists in sidecar TIFF. */
gboolean dt_drawlayer_io_layer_name_exists(const char *path, const char *candidate, const int ignore_index)
{
  if(!path || !candidate || candidate[0] == '\0' || !g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;

  gboolean exists = FALSE;
  int index = 0;
  if(TIFFSetDirectory(tiff, 0))
  {
    do
    {
      char *page_name = NULL;
      TIFFGetField(tiff, TIFFTAG_PAGENAME, &page_name);
      if(index != ignore_index && !g_strcmp0(page_name ? page_name : "", candidate))
      {
        exists = TRUE;
        break;
      }
      index++;
    } while(TIFFReadDirectory(tiff));
  }

  TIFFClose(tiff);
  return exists;
}

/** @brief Normalize/sanitize requested layer name with fallback handling. */
static void _sanitize_requested_layer_name(const char *requested, const char *fallback_name, char *name,
                                           const size_t name_size)
{
  if(!name || name_size == 0) return;
  name[0] = '\0';
  if(requested && requested[0]) g_strlcpy(name, requested, name_size);
  g_strstrip(name);
  if(name[0] == '\0' && fallback_name && fallback_name[0]) g_strlcpy(name, fallback_name, name_size);
}

/** @brief Build absolute sidecar path from image id. */
gboolean dt_drawlayer_io_sidecar_path(const int32_t imgid, char *path, const size_t path_size)
{
  gboolean from_cache = FALSE;
  if(!path || path_size == 0) return FALSE;
  dt_image_full_path(imgid, path, path_size, &from_cache, __FUNCTION__);
  if(path[0] == '\0') return FALSE;
  g_strlcat(path, ".ansel.tiff", path_size);
  return TRUE;
}

/** @brief Find target layer metadata in sidecar TIFF. */
gboolean dt_drawlayer_io_find_layer(const char *path, const char *target_name, const int target_order,
                                    dt_drawlayer_io_layer_info_t *info)
{
  if(info) memset(info, 0, sizeof(*info));
  if(!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;
  _scan_directories(tiff, target_name, target_order, info);
  TIFFClose(tiff);
  return info && info->found;
}

/** @brief Load one sidecar layer patch into float RGBA destination patch. */
gboolean dt_drawlayer_io_load_layer(const char *path, const char *target_name, const int target_order,
                                    const int raw_width, const int raw_height, dt_drawlayer_io_patch_t *patch)
{
  if(!patch || !patch->pixels || patch->width <= 0 || patch->height <= 0) return FALSE;
  dt_drawlayer_cache_clear_transparent_float(patch->pixels, (size_t)patch->width * patch->height);

  if(!g_file_test(path, G_FILE_TEST_EXISTS)) return TRUE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;

  dt_drawlayer_io_layer_info_t info;
  _scan_directories(tiff, target_name, target_order, &info);
  if(!info.found)
  {
    TIFFClose(tiff);
    return TRUE;
  }

  if(!TIFFSetDirectory(tiff, (tdir_t)info.index))
  {
    TIFFClose(tiff);
    return FALSE;
  }

  uint16_t *row = g_malloc_n((gsize)info.width * 4, sizeof(uint16_t));
  if(!row)
  {
    TIFFClose(tiff);
    return FALSE;
  }

  const int offset_x = (raw_width - (int)info.width) / 2;
  const int offset_y = (raw_height - (int)info.height) / 2;

  for(int py = 0; py < patch->height; py++)
  {
    const int raw_y = patch->y + py;
    const int src_y = raw_y - offset_y;
    if(src_y < 0 || src_y >= (int)info.height) continue;
    if(!_read_scanline_rgba(tiff, info.width, (uint32_t)src_y, row))
    {
      dt_free(row);
      TIFFClose(tiff);
      return FALSE;
    }

    for(int px = 0; px < patch->width; px++)
    {
      const int raw_x = patch->x + px;
      const int src_x = raw_x - offset_x;
      if(src_x < 0 || src_x >= (int)info.width) continue;
      float *dst = patch->pixels + 4 * ((size_t)py * patch->width + px);
      _load_half_pixel_rgba(row + 4 * src_x, dst);
    }
  }

  dt_free(row);
  TIFFClose(tiff);
  return TRUE;
}

/** @brief Load first TIFF page as flat RGBA float image. */
gboolean dt_drawlayer_io_load_flat_rgba(const char *path, float **pixels, int *width, int *height)
{
  if(pixels) *pixels = NULL;
  if(width) *width = 0;
  if(height) *height = 0;
  if(!path || !pixels || !width || !height || !g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;

  uint32_t w = 0, h = 0;
  uint16_t spp = 0, bpp = 0, sampleformat = SAMPLEFORMAT_UINT;
  TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &w);
  TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &h);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bpp);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleformat);

  if(w == 0 || h == 0 || spp < 1)
  {
    TIFFClose(tiff);
    return FALSE;
  }

  float *out = g_malloc_n((size_t)w * h * 4, sizeof(float));
  if(!out)
  {
    TIFFClose(tiff);
    return FALSE;
  }
  dt_drawlayer_cache_clear_transparent_float(out, (size_t)w * h);

  const tsize_t scanline = TIFFScanlineSize(tiff);
  tdata_t row = _TIFFmalloc((tsize_t)scanline);
  if(!row)
  {
    dt_free(out);
    TIFFClose(tiff);
    return FALSE;
  }

  gboolean ok = TRUE;
  for(uint32_t y = 0; y < h && ok; y++)
  {
    if(TIFFReadScanline(tiff, row, y, 0) == -1)
    {
      ok = FALSE;
      break;
    }

    for(uint32_t x = 0; x < w; x++)
    {
      float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

      if(bpp == 32 && sampleformat == SAMPLEFORMAT_IEEEFP)
      {
        const float *src = (const float *)row + (size_t)x * spp;
        if(spp >= 3)
        {
          r = src[0];
          g = src[1];
          b = src[2];
        }
        else
        {
          r = g = b = src[0];
        }
        if(spp >= 4) a = src[3];
      }
      else if(bpp == 16 && sampleformat == SAMPLEFORMAT_IEEEFP)
      {
        const uint16_t *src = (const uint16_t *)row + (size_t)x * spp;
        if(spp >= 3)
        {
          r = _half_to_float(src[0]);
          g = _half_to_float(src[1]);
          b = _half_to_float(src[2]);
        }
        else
        {
          r = g = b = _half_to_float(src[0]);
        }
        if(spp >= 4) a = _half_to_float(src[3]);
      }
      else if(bpp == 16)
      {
        const uint16_t *src = (const uint16_t *)row + (size_t)x * spp;
        if(spp >= 3)
        {
          r = src[0] / 65535.0f;
          g = src[1] / 65535.0f;
          b = src[2] / 65535.0f;
        }
        else
        {
          r = g = b = src[0] / 65535.0f;
        }
        if(spp >= 4) a = src[3] / 65535.0f;
      }
      else if(bpp == 8)
      {
        const uint8_t *src = (const uint8_t *)row + (size_t)x * spp;
        if(spp >= 3)
        {
          r = src[0] / 255.0f;
          g = src[1] / 255.0f;
          b = src[2] / 255.0f;
        }
        else
        {
          r = g = b = src[0] / 255.0f;
        }
        if(spp >= 4) a = src[3] / 255.0f;
      }
      else
      {
        ok = FALSE;
        break;
      }

      float *dst = out + 4 * ((size_t)y * w + x);
      dst[0] = r;
      dst[1] = g;
      dst[2] = b;
      dst[3] = _clamp01(a);
    }
  }

  _TIFFfree(row);
  TIFFClose(tiff);
  if(!ok)
  {
    dt_free(out);
    return FALSE;
  }

  *pixels = out;
  *width = (int)w;
  *height = (int)h;
  return TRUE;
}

/** @brief Store/update/delete one layer entry in sidecar TIFF. */
gboolean dt_drawlayer_io_store_layer(const char *path, const char *target_name, const int target_order,
                                     const char *work_profile, const dt_drawlayer_io_patch_t *patch,
                                     const int raw_width, const int raw_height, const gboolean delete_target,
                                     int *final_order)
{
  if(!target_name || target_name[0] == '\0') return FALSE;
  return _rewrite_sidecar(path, target_name, target_order, work_profile, patch, raw_width, raw_height, delete_target,
                          -1, final_order);
}

/** @brief Insert a new layer after given order in sidecar TIFF. */
gboolean dt_drawlayer_io_insert_layer(const char *path, const char *target_name, const int insert_after_order,
                                      const char *work_profile, const dt_drawlayer_io_patch_t *patch,
                                      const int raw_width, const int raw_height, int *final_order)
{
  if(!target_name || target_name[0] == '\0') return FALSE;
  const int insert_order = (insert_after_order >= 0) ? (insert_after_order + 1) : -1;
  return _rewrite_sidecar(path, target_name, -1, work_profile, patch, raw_width, raw_height, FALSE, insert_order,
                          final_order);
}

/** @brief Create unique layer name with fallback if requested name is empty. */
void dt_drawlayer_io_make_unique_name(const char *path, const char *requested, const char *fallback_name, char *name,
                                      const size_t name_size)
{
  _sanitize_requested_layer_name(requested, fallback_name, name, name_size);
  if(name[0] == '\0') return;
  if(!dt_drawlayer_io_layer_name_exists(path, name, -1)) return;

  char base[DT_DRAWLAYER_IO_NAME_SIZE] = { 0 };
  g_strlcpy(base, name, sizeof(base));

  for(int suffix = 2; suffix < 100000; suffix++)
  {
    g_snprintf(name, name_size, "%.*s %d", MAX((int)name_size - 12, 1), base, suffix);
    if(!dt_drawlayer_io_layer_name_exists(path, name, -1)) return;
  }
}

/** @brief Create unique layer name without fallback source. */
void dt_drawlayer_io_make_unique_name_plain(const char *path, const char *requested, char *name, const size_t name_size)
{
  if(!name || name_size == 0) return;
  name[0] = '\0';
  if(requested) g_strlcpy(name, requested, name_size);
  g_strstrip(name);
  if(name[0] == '\0') return;
  if(!dt_drawlayer_io_layer_name_exists(path, name, -1)) return;

  char base[DT_DRAWLAYER_IO_NAME_SIZE] = { 0 };
  g_strlcpy(base, name, sizeof(base));

  for(int suffix = 2; suffix < 100000; suffix++)
  {
    g_snprintf(name, name_size, "%.*s %d", MAX((int)name_size - 12, 1), base, suffix);
    if(!dt_drawlayer_io_layer_name_exists(path, name, -1)) return;
  }
}

/** @brief List all TIFF layer page names. */
gboolean dt_drawlayer_io_list_layer_names(const char *path, char ***names, int *count)
{
  if(names) *names = NULL;
  if(count) *count = 0;
  if(!path || !names || !count || !g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;

  GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
  if(TIFFSetDirectory(tiff, 0))
  {
    do
    {
      char *page_name = NULL;
      TIFFGetField(tiff, TIFFTAG_PAGENAME, &page_name);
      g_ptr_array_add(arr, g_strdup(page_name ? page_name : ""));
    } while(TIFFReadDirectory(tiff));
  }
  TIFFClose(tiff);

  *count = (int)arr->len;
  if(arr->len == 0)
  {
    g_ptr_array_free(arr, TRUE);
    *names = NULL;
    return TRUE;
  }

  char **out = g_new0(char *, arr->len);
  for(guint i = 0; i < arr->len; i++)
    out[i] = (char *)g_ptr_array_index(arr, i);
  g_ptr_array_free(arr, FALSE);
  *names = out;
  return TRUE;
}

/** @brief Free array returned by `dt_drawlayer_io_list_layer_names`. */
void dt_drawlayer_io_free_layer_names(char ***names, int *count)
{
  if(!names || !*names) return;
  const int n = (count && *count > 0) ? *count : 0;
  for(int i = 0; i < n; i++) dt_free((*names)[i]);
  dt_free(*names);
  if(count) *count = 0;
}
