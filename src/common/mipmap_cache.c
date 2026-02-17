/*
    This file is part of darktable,
    Copyright (C) 2011-2012, 2015 Edouard Gomez.
    Copyright (C) 2011-2012 Henrik Andersson.
    Copyright (C) 2011-2017 johannes hanika.
    Copyright (C) 2011-2012 José Carlos García Sogo.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012, 2014 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2017, 2019-2020 Tobias Ellinghaus.
    Copyright (C) 2012, 2014 Ulrich Pegelow.
    Copyright (C) 2013-2014, 2019-2021 Pascal Obry.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014, 2016 Dan Torop.
    Copyright (C) 2014-2015 parafin.
    Copyright (C) 2014-2016 Pedro Côrte-Real.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015 Matthias Gehre.
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2017 luzpaz.
    Copyright (C) 2019-2021 Aldric Renaudin.
    Copyright (C) 2019, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020-2022 Hanno Schwalm.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2023 Ricky Moon.
    Copyright (C) 2024 Alynx Zhou.
    Copyright (C) 2025 Guillaume Stutin.
    
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

#include "common/mipmap_cache.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/file_location.h"
#include "common/grealpath.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_jpeg.h"
#include "common/imageio_module.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "develop/imageop_math.h"
#include "dtgtk/thumbtable_info.h"
#include "gui/gtk.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif

#if !defined(_WIN32)
#include <sys/statvfs.h>
#else
//statvfs does not exist in Windows, providing implementation
#include "win/statvfs.h"
#endif

#define DT_MIPMAP_CACHE_FILE_MAGIC 0xD71337
#define DT_MIPMAP_CACHE_FILE_VERSION 23
#define DT_MIPMAP_CACHE_DEFAULT_FILE_NAME "mipmaps"

typedef enum dt_mipmap_buffer_dsc_flags
{
  DT_MIPMAP_BUFFER_DSC_FLAG_NONE = 0,
  DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE = 1 << 0,
  DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE = 1 << 1
} dt_mipmap_buffer_dsc_flags;

// the embedded Exif data to tag thumbnails as sRGB or AdobeRGB
static const uint8_t dt_mipmap_cache_exif_data_srgb[] = {
  0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x69,
  0x87, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x01, 0xa0, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const uint8_t dt_mipmap_cache_exif_data_adobergb[] = {
  0x45, 0x78, 0x69, 0x66, 0x00, 0x00, 0x49, 0x49, 0x2a, 0x00, 0x08, 0x00, 0x00, 0x00, 0x01, 0x00, 0x69,
  0x87, 0x04, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
  0x01, 0xa0, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};
static const int dt_mipmap_cache_exif_data_srgb_length
                      = sizeof(dt_mipmap_cache_exif_data_srgb) / sizeof(*dt_mipmap_cache_exif_data_srgb);
static const int dt_mipmap_cache_exif_data_adobergb_length
                      = sizeof(dt_mipmap_cache_exif_data_adobergb) / sizeof(*dt_mipmap_cache_exif_data_adobergb);

struct dt_mipmap_buffer_dsc
{
  uint32_t width;
  uint32_t height;
  float iscale;
  size_t size;
  dt_mipmap_buffer_dsc_flags flags;
  dt_colorspaces_color_profile_type_t color_space;

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
  // do not touch!
  // must be the last element.
  // must be no less than 16bytes
  char redzone[16];
#endif

  /* NB: sizeof must be a multiple of 4*sizeof(float) */
} __attribute__((packed, aligned(DT_CACHELINE_BYTES)));

#if __has_feature(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
static const size_t dt_mipmap_buffer_dsc_size __attribute__((unused))
= sizeof(struct dt_mipmap_buffer_dsc) - sizeof(((struct dt_mipmap_buffer_dsc *)0)->redzone);
#else
static const size_t dt_mipmap_buffer_dsc_size __attribute__((unused)) = sizeof(struct dt_mipmap_buffer_dsc);
#endif

static uint8_t * _get_buffer_from_dsc(struct dt_mipmap_buffer_dsc *dsc);

static inline void *dead_image_8(struct dt_mipmap_buffer_dsc *dsc)
{
  dsc->width = dsc->height = 8;
  dsc->iscale = 1.0f;
  dsc->color_space = DT_COLORSPACE_DISPLAY;
  assert(dsc->size > 64 * sizeof(uint32_t));
  const uint32_t X = 0xffffffffu;
  const uint32_t o = 0u;
  const uint32_t image[]
      = { o, o, o, o, o, o, o, o, o, o, X, X, X, X, o, o, o, X, o, X, X, o, X, o, o, X, X, X, X, X, X, o,
          o, o, X, o, o, X, o, o, o, o, o, o, o, o, o, o, o, o, X, X, X, X, o, o, o, o, o, o, o, o, o, o };
  memcpy(_get_buffer_from_dsc(dsc), image, sizeof(uint32_t) * 64);
  return _get_buffer_from_dsc(dsc);
}

static inline int32_t get_key(const int32_t imgid, const dt_mipmap_size_t size)
{
  // imgid can't be >= 2^28 (~250 million images)
  return (((int32_t)size) << 28) | (imgid - 1);
}

static inline uint32_t get_imgid(const uint32_t key)
{
  return (key & 0xfffffff) + 1;
}

static inline dt_mipmap_size_t get_size(const uint32_t key)
{
  return (dt_mipmap_size_t)(key >> 28);
}

static int dt_mipmap_cache_get_filename(gchar *mipmapfilename, size_t size)
{
  int r = -1;
  char *abspath = NULL;

  // Directory
  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  // Build the mipmap filename fram hashing the path of the library DB
  const gchar *dbfilename = dt_database_get_path(darktable.db);

  if(!strcmp(dbfilename, ":memory:"))
  {
    mipmapfilename[0] = '\0';
    r = 0;
    goto exit;
  }

  abspath = g_realpath(dbfilename);
  if(!abspath) abspath = g_strdup(dbfilename);

  GChecksum *chk = g_checksum_new(G_CHECKSUM_SHA1);
  g_checksum_update(chk, (guchar *)abspath, strlen(abspath));
  const gchar *filename = g_checksum_get_string(chk);

  if(!filename || filename[0] == '\0')
    snprintf(mipmapfilename, size, "%s/%s", cachedir, DT_MIPMAP_CACHE_DEFAULT_FILE_NAME);
  else
    snprintf(mipmapfilename, size, "%s/%s-%s", cachedir, DT_MIPMAP_CACHE_DEFAULT_FILE_NAME, filename);

  g_checksum_free(chk);
  r = 0;

exit:
  g_free(abspath);

  return r;
}

static gboolean _mipmap_get_thumb_info(const int32_t imgid, dt_thumbnail_image_info_t *info);
static gboolean _mipmap_local_copy_full_path_from_info(const dt_thumbnail_image_info_t *info, char *pathname,
                                                       size_t pathname_len);
static gboolean _mipmap_full_path_from_info(const dt_thumbnail_image_info_t *info, char *pathname,
                                            size_t pathname_len, gboolean *from_cache);

/**
 * @brief Check if an image should be written to disk, if the thumbnail should be computed from embedded JPEG,
 * and optionaly return intermediate checks to get there, like whether the input is a JPEG file and if it exists on
 * the filesystem.
 *
 * @param imgid Database SQL ID of the image
 * @param filename Filename of the input file. Can be NULL.
 * @param ext Extension of the input file. Can be NULL.
 * @param input_exists Whether the file can be found on the filesystem. Can be NULL.
 * @param is_jpg_input Whether the file is a JPEG. Can be NULL.
 * @param use_embedded_jpg Whether the lighttable should use the embedded JPEG thumbnail. Can be NULL.
 * @param write_to_disk Whether the cached thumbnail should be flushed to disk before being flushed from RAM. Can
 * be NULL.
 */
static void _write_mipmap_to_disk(const int32_t imgid, char *filename, char *ext, gboolean *input_exists,
                                  gboolean *is_jpg_input, gboolean *use_embedded_jpg, gboolean *write_to_disk)
{
  // Get file name
  gboolean from_cache = TRUE;
  char _filename[PATH_MAX] = { 0 };
  dt_thumbnail_image_info_t info = { 0 };
  const gboolean have_info = _mipmap_get_thumb_info(imgid, &info);

  if(filename || ext || input_exists || is_jpg_input)
  {
    // Avoid per-image SQL by building paths from the thumbtable LUT when available.
    if(!(have_info && _mipmap_full_path_from_info(&info, _filename, sizeof(_filename), &from_cache)))
      dt_image_full_path(imgid, _filename, sizeof(_filename), &from_cache, __FUNCTION__);
    if(filename) strncpy(filename, _filename, PATH_MAX);
    if(input_exists) *input_exists = *_filename && g_file_test(_filename, G_FILE_TEST_EXISTS);
  }

  // Get the file extension
  if(ext || is_jpg_input)
  {
    char *_ext = _filename + strlen(_filename);
    while(*_ext != '.' && _ext > _filename) _ext--;
    if(ext) strncpy(ext, _ext, 5);
    if(is_jpg_input) *is_jpg_input = !strcasecmp(_ext, ".jpg") || !strcasecmp(_ext, ".jpeg");
  }

  // embedded JPG mode:
  // 0 = never use embedded thumbnail
  // 1 = only on unedited pics,
  // 2 = always use embedded thumbnail
  int mode = dt_conf_get_int("lighttable/embedded_jpg");
  gboolean altered = FALSE;
  if(mode == 1)
  {
    if(have_info)
    {
      altered = dt_thumbtable_info_is_altered(&info);
    }
    else
    {
      altered = dt_image_altered(imgid);
    }
  }
  const gboolean _use_embedded_jpg
      = (mode == 2                           // always use embedded
         || (mode == 1 && !altered));        // use embedded only on unaltered images
  if(use_embedded_jpg) *use_embedded_jpg = _use_embedded_jpg;

  // Save to cache only if the option is enabled by user and the thumbnail is not the embedded thumbnail
  // The rationale is getting the un-processed JPEG thumbnail is cheap and not worth saving on disk.
  // This allows fast toggling between JPEG and processed RAW thumbnail from GUI.
  if(write_to_disk)
  {
    *write_to_disk = dt_conf_get_bool("cache_disk_backend") && !_use_embedded_jpg;
  }
}


static void _init_f(dt_mipmap_buffer_t *mipmap_buf, float *buf, uint32_t *width, uint32_t *height, float *iscale,
                    const int32_t imgid);
static void _init_8(uint8_t *buf, uint32_t *width, uint32_t *height, float *iscale,
                    dt_colorspaces_color_profile_type_t *color_space, const int32_t imgid,
                    const dt_mipmap_size_t size);
static gboolean _mipmap_get_thumb_info(const int32_t imgid, dt_thumbnail_image_info_t *info)
{
  if(!info || !darktable.gui || !darktable.gui->ui) return FALSE;

  // Prefer the lightweight thumbtable LUT to avoid image-cache SQL in hot paths.
  if(dt_thumbtable_get_thumbnail_info(darktable.gui->ui->thumbtable_lighttable, imgid, info))
    return TRUE;
  if(dt_thumbtable_get_thumbnail_info(darktable.gui->ui->thumbtable_filmstrip, imgid, info))
    return TRUE;

  return FALSE;
}

static gboolean _mipmap_local_copy_full_path_from_info(const dt_thumbnail_image_info_t *info, char *pathname,
                                                       size_t pathname_len)
{
  if(!info || !pathname) return FALSE;
  if(!info->folder[0] || !info->filename[0]) return FALSE;

  char filename[PATH_MAX] = { 0 };
  g_snprintf(filename, sizeof(filename), "%s" G_DIR_SEPARATOR_S "%s", info->folder, info->filename);

  char *md5_filename = g_compute_checksum_for_string(G_CHECKSUM_MD5, filename, strlen(filename));
  if(!md5_filename) return FALSE;

  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));

  // Keep the same extension semantics as the DB-backed path builder.
  char *c = filename + strlen(filename);
  while(*c != '.' && c > filename) c--;

  // cache filename old format: <cachedir>/img-<id>-<MD5>.<ext>
  snprintf(pathname, pathname_len, "%s/img-%d-%s%s", cachedir, info->imgid, md5_filename, c);
  if(!g_file_test(pathname, G_FILE_TEST_EXISTS))
  {
    // cache filename format: <cachedir>/img-<MD5>.<ext>
    snprintf(pathname, pathname_len, "%s/img-%s%s", cachedir, md5_filename, c);
  }

  g_free(md5_filename);
  return TRUE;
}

static gboolean _mipmap_full_path_from_info(const dt_thumbnail_image_info_t *info, char *pathname,
                                            size_t pathname_len, gboolean *from_cache)
{
  if(!info || !pathname || !from_cache) return FALSE;
  if(!info->folder[0] || !info->filename[0]) return FALSE;

  g_snprintf(pathname, pathname_len, "%s" G_DIR_SEPARATOR_S "%s", info->folder, info->filename);

  if(*from_cache)
  {
    char lc_pathname[PATH_MAX] = { 0 };
    if(_mipmap_local_copy_full_path_from_info(info, lc_pathname, sizeof(lc_pathname))
       && g_file_test(lc_pathname, G_FILE_TEST_EXISTS))
    {
      g_strlcpy(pathname, lc_pathname, pathname_len);
    }
    else
    {
      *from_cache = FALSE;
    }
  }

  return TRUE;
}


/**
 * @file: mipmap_cache.c
 *
 * @brief shit ahead.
 *
 * So, `(dt_mipmap_buffer_t *buf)->cache_entry` holds a back-reference to the cache entry `dt_cache_entry_t
 * *entry`.
 * `(dt_cache_entry_t *entry)->data` is a reference to the `dt_mipmap_buffer_dsc *dsc`, which
 * is mostly a duplicate of `dt_mipmap_buffer_t *buf`, adding state flags and
 * then 64-bytes memory padding.
 *
 * `entry->data` is an opaque, manually-handled, memory buffer holding (in this order):
 *  - the `dt_mipmap_buffer_dsc dsc` structure,
 *  - memory padding for 64-bytes alignment,
 *  - the actual pixel buffer `buf->buf` (aligned to 64 bytes);
 *
 * So the only way of accessing the `buf->buf` memory is by shifting the `entry->data`
 * pointer by the size of one `dt_mipmap_buffer_dsc` structure.
 *
 * But we need to resync the `dt_mipmap_buffer_dsc dsc` values with those in
 * `dt_mipmap_buffer_t *buf` all the fucking time (namely, width, height, scale, color space).
 * And then, we need to resync the buffer sizes between `entry->data_size` and
 * `dsc->size`. But beware, because `buf->size` is actually the integer enum member
 * of mipmap fixed sizes.
 *
 * All in all, this clever design guarantees a maximum number of opportunities for mistakes,
 * along with non-uniforms accesses, sometimes to `dsc`, sometimes to `buf`.
 *
 * From the rest of the app, we access only `dt_mipmap_buffer_t *buf`,
 * so this is our way of communicating stuff out there.
 *
 */

// Gets the address of the `buf->buf` pixel buffer, from the cache entry.
static uint8_t *_get_buffer_from_dsc(struct dt_mipmap_buffer_dsc *dsc)
{
  if(dsc)
    return (uint8_t *)(dsc + 1);
  else
    return NULL;
}


struct dt_mipmap_buffer_dsc *_get_dsc_from_entry(dt_cache_entry_t *entry)
{
  return (struct dt_mipmap_buffer_dsc *)entry->data;
}

// update buffer params with dsc
static void _sync_dsc_to_buf(dt_mipmap_buffer_t *buf, struct dt_mipmap_buffer_dsc *dsc, const int32_t imgid,
                             const dt_mipmap_size_t mip)
{
  buf->width = dsc->width;
  buf->height = dsc->height;
  buf->iscale = dsc->iscale;
  buf->color_space = dsc->color_space;
  buf->imgid = imgid;
  buf->size = mip;
  buf->buf = _get_buffer_from_dsc(dsc);
}


// flag a buffer as invalid and set it NULL
// This doesn't paint skulls
static void _invalidate_buffer(dt_mipmap_buffer_t *buf)
{
  buf->width = buf->height = 0;
  buf->iscale = 0.0f;
  buf->buf = NULL;
}


static size_t _get_entry_size(const size_t buffer_size)
{
  return buffer_size + dt_mipmap_buffer_dsc_size;
}

/**
 * @brief Resync all references to all references
 *
 * @param entry Cache entry.
 * @param dsc
 * @param buf
 */
void dt_mipmap_cache_update_buffer_addresses(dt_cache_entry_t *entry, struct dt_mipmap_buffer_dsc **dsc,
                                             const size_t width, const size_t height,
                                             const size_t buffer_size)
{
  if(!entry->data)
  {
    entry->data_size = 0;
    *dsc = NULL;
    return;
  }

  // Update the entry datasize
  entry->data_size = _get_entry_size(buffer_size);

  // Update the dsc parameters
  *dsc = _get_dsc_from_entry(entry);
  (*dsc)->width = width;
  (*dsc)->height = height;
  (*dsc)->iscale = 1.0f;
  (*dsc)->color_space = DT_COLORSPACE_NONE;
  (*dsc)->flags = DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
  (*dsc)->size = _get_entry_size(buffer_size);
}


// callback for the imageio core to allocate memory.
// only needed for _FULL buffers, as they change size
// with the input image. will allocate img->width*img->height*img->bpp bytes.
void *dt_mipmap_cache_alloc(dt_mipmap_buffer_t *buf, const dt_image_t *img)
{
  assert(buf);
  if(!buf) return NULL;

  assert(buf->size == DT_MIPMAP_FULL);

  if(buf->size != DT_MIPMAP_FULL)
  {
    fprintf(stderr, "trying to alloc a wrong mipmap size for %s: %i (should be: %i)\n", img->filename, buf->size, DT_MIPMAP_FULL);
    return NULL;
  }

  dt_cache_entry_t *entry = buf->cache_entry;
  assert(entry);

  if(!entry)
  {
    fprintf(stderr, "trying to alloc a buffer entry that has no back-reference to cache entry\n");
    return NULL;
  }

  // Free and reset everything
  if(entry->data) dt_free_align(entry->data);
  ASAN_POISON_MEMORY_REGION(entry->data, entry->data_size);

  // Get a new allocation
  const int wd = img->width;
  const int ht = img->height;
  const size_t bpp = dt_iop_buffer_dsc_to_bpp(&img->buf_dsc);
  const size_t buffer_size = wd * ht * bpp;
  const size_t min_buffer_size = 64 * 4 * sizeof(float);
  entry->data = dt_alloc_align(_get_entry_size(MAX(buffer_size, min_buffer_size)));
  if(entry->data == NULL) return NULL;

  // Update the references
  struct dt_mipmap_buffer_dsc *dsc = NULL;
  dt_mipmap_cache_update_buffer_addresses(entry, &dsc, wd, ht, buffer_size);

  // Unpoison the pixel buffer region
  ASAN_UNPOISON_MEMORY_REGION(buf->buf, dsc->size - dt_mipmap_buffer_dsc_size);

  // Unpoison the dsc region
  ASAN_UNPOISON_MEMORY_REGION(dsc, dt_mipmap_buffer_dsc_size);

  // So the padding region between dsc and buf->buf should be poisoned still
  assert(entry->data == dsc);

  if(dsc)
  {
    assert(entry->data_size == dsc->size);
  }

  // return pointer to start of payload
  return _get_buffer_from_dsc(dsc);
}

// callback for the cache backend to initialize payload pointers
// It's actually not dynamic at all, fixed size only.
void dt_mipmap_cache_allocate_dynamic(void *data, dt_cache_entry_t *entry)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;
  const dt_mipmap_size_t mip = get_size(entry->key);
  const int32_t imgid = get_imgid(entry->key);

  assert(mip < DT_MIPMAP_NONE);

  // Free and reset everything
  if(entry->data) dt_free_align(entry->data);

  // Get a new allocation
  const size_t buffer_size = (mip <= DT_MIPMAP_F) ? cache->buffer_size[mip] : _get_entry_size(sizeof(float) * 4 * 64);
  entry->data = dt_alloc_align(buffer_size);
  if(entry->data == NULL) return;

  // Update the references
  struct dt_mipmap_buffer_dsc *dsc = NULL;
  if(mip <= DT_MIPMAP_F)
    dt_mipmap_cache_update_buffer_addresses(entry, &dsc, cache->max_width[mip], cache->max_height[mip], buffer_size);
  else
    dt_mipmap_cache_update_buffer_addresses(entry, &dsc, 0, 0, buffer_size);

  assert(entry->data == dsc);

  if(!dsc) return;

  gboolean write_to_disk;
  _write_mipmap_to_disk(imgid, NULL, NULL, NULL, NULL, NULL, &write_to_disk);

  if(cache->cachedir[0] && write_to_disk && mip < DT_MIPMAP_F)
  {
    // try and load from disk, if successful set flag
    char filename[PATH_MAX] = {0};
    snprintf(filename, sizeof(filename), "%s.d/%d/%" PRIu32 ".jpg", cache->cachedir, (int)mip,
             get_imgid(entry->key));

    gboolean io_error = FALSE;
    gchar *error = NULL;
    uint8_t *blob = NULL;
    dt_colorspaces_color_profile_type_t color_space = DT_COLORSPACE_DISPLAY;
    dt_imageio_jpeg_t jpg;

    FILE *f = g_fopen(filename, "rb");
    if(f == NULL) goto finish; // file doesn't exist

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    if(len <= 0)
    {
      error = "empty file";
      io_error = TRUE;
      goto finish;
    }

    blob = (uint8_t *)dt_alloc_align(len);
    if(blob == NULL)
    {
      error = "out of memory";
      io_error = TRUE;
      goto finish;
    }

    fseek(f, 0, SEEK_SET);
    const size_t rd = fread(blob, sizeof(uint8_t), len, f);
    if(rd != len)
    {
      error = "corrupted file";
      io_error = TRUE;
      goto finish;
    }

    if(dt_imageio_jpeg_decompress_header(blob, len, &jpg))
    {
      error = "couldn't decompress header";
      io_error = TRUE;
      goto finish;
    }

    color_space = dt_imageio_jpeg_read_color_space(&jpg);

    if(dt_imageio_jpeg_decompress(&jpg, _get_buffer_from_dsc(dsc)))
    {
      error = "couldn't decompress JPEG";
      io_error = TRUE;
      goto finish;
    }

    dt_print(DT_DEBUG_CACHE, "[mipmap_cache] grab mip %d for image %" PRIu32 " (%ix%i) from disk cache\n", mip,
              imgid, jpg.width, jpg.height);

    dsc->width = jpg.width;
    dsc->height = jpg.height;
    dsc->iscale = 1.0f;
    dsc->color_space = color_space;
    dsc->flags = 0;

finish:
    if(f && io_error)
    {
      // Delete the file, we will regenerate it
      g_unlink(filename);
      fprintf(stderr, "[mipmap_cache] failed to open thumbnail for image %" PRIu32 " from `%s'. Reason: %s\n",
              imgid, filename, error);
    }

    if(blob) dt_free_align(blob);
    if(f) fclose(f);
  }

  // cost is just flat one for the buffer, as the buffers might have different sizes,
  // to make sure quota is meaningful.
  if(mip >= DT_MIPMAP_F)
    entry->cost = 1;
  else
    entry->cost = cache->buffer_size[mip];
}

static void dt_mipmap_cache_unlink_ondisk_thumbnail(void *data, int32_t imgid, dt_mipmap_size_t mip)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;

  // also remove jpg backing (always try to do that, in case user just temporarily switched it off,
  // to avoid inconsistencies.
  // if(dt_conf_get_bool("cache_disk_backend"))
  if(cache->cachedir[0])
  {
    char filename[PATH_MAX] = { 0 };
    snprintf(filename, sizeof(filename), "%s.d/%d/%"PRIu32".jpg", cache->cachedir, (int)mip, imgid);
    g_unlink(filename);
  }
}

void dt_mipmap_cache_deallocate_dynamic(void *data, dt_cache_entry_t *entry)
{
  dt_mipmap_cache_t *cache = (dt_mipmap_cache_t *)data;
  const dt_mipmap_size_t mip = get_size(entry->key);
  if(mip < DT_MIPMAP_F)
  {
    const int32_t imgid = get_imgid(entry->key);
    gboolean write_to_disk;
    _write_mipmap_to_disk(imgid, NULL, NULL, NULL, NULL, NULL, &write_to_disk);

    struct dt_mipmap_buffer_dsc *dsc = _get_dsc_from_entry(entry);
    // don't write skulls:
    if(dsc->width > 8 && dsc->height > 8)
    {
      if(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE)
      {
        dt_mipmap_cache_unlink_ondisk_thumbnail(data, get_imgid(entry->key), mip);
      }
      else if(cache->cachedir[0] && write_to_disk && mip < DT_MIPMAP_F)
      {
        // serialize to disk
        char filename[PATH_MAX] = {0};
        snprintf(filename, sizeof(filename), "%s.d/%d", cache->cachedir, mip);
        const int mkd = g_mkdir_with_parents(filename, 0750);
        if(!mkd)
        {
          snprintf(filename, sizeof(filename), "%s.d/%d/%" PRIu32 ".jpg", cache->cachedir, (int)mip,
                   get_imgid(entry->key));
          // Don't write existing files as both performance and quality (lossy jpg) suffer
          FILE *f = NULL;
          if (!g_file_test(filename, G_FILE_TEST_EXISTS) && (f = g_fopen(filename, "wb")))
          {
            // first check the disk isn't full
            struct statvfs vfsbuf;
            if (!statvfs(filename, &vfsbuf))
            {
              const int64_t free_mb = ((vfsbuf.f_frsize * vfsbuf.f_bavail) >> 20);
              if (free_mb < 100)
              {
                fprintf(stderr, "Aborting image write as only %" PRId64 " MB free to write %s\n", free_mb, filename);
                goto write_error;
              }
            }
            else
            {
              fprintf(stderr, "Aborting image write since couldn't determine free space available to write %s\n", filename);
              goto write_error;
            }

            const int cache_quality = dt_conf_get_int("database_cache_quality");
            const uint8_t *exif = NULL;
            int exif_len = 0;
            if(dsc->color_space == DT_COLORSPACE_SRGB)
            {
              exif = dt_mipmap_cache_exif_data_srgb;
              exif_len = dt_mipmap_cache_exif_data_srgb_length;
            }
            else if(dsc->color_space == DT_COLORSPACE_ADOBERGB)
            {
              exif = dt_mipmap_cache_exif_data_adobergb;
              exif_len = dt_mipmap_cache_exif_data_adobergb_length;
            }
            if(dt_imageio_jpeg_write(filename, _get_buffer_from_dsc(dsc), dsc->width, dsc->height, MIN(100, MAX(10, cache_quality)), exif, exif_len))
            {
write_error:
              g_unlink(filename);
            }
          }
          if(f) fclose(f);
        }
      }
    }
  }
  dt_free_align(entry->data);
}

void dt_mipmap_cache_init(dt_mipmap_cache_t *cache)
{
  dt_mipmap_cache_get_filename(cache->cachedir, sizeof(cache->cachedir));

  // Fixed sizes for the thumbnail mip levels, selected for coverage of most screen sizes
  // Starting at 4K, we use 3:2 ratio of camera sensor instead of 16:9/16:10 of display,
  // in order to start caching full-resolution JPEG for 100% zoom in lighttable
  size_t mipsizes[DT_MIPMAP_F][2] = {
    { 360, 225 },             // mip0 - 1/2 size previous one
    { 720, 450 },             // mip1 - 1/2 size previous one
    { 1440, 900 },            // mip2 - covers HD, WXGA+
    { 1920, 1200 },           // mip3 - covers 1080p and 1600x1200
    { 2560, 1600 },           // mip4 - covers 2560x1440
    { 3840, 2560 },           // mip5 - covers 4K and UHD
    { 5120, 3414 },           // mip6 - covers 5K
    { 6144,	4096 },           // mip7 - covers 6K
    { 7680, 5120 },           // mip8 - covers 8K
  };
  // Set mipf for preview pipe to 1440x900
  cache->max_width[DT_MIPMAP_F] = mipsizes[DT_MIPMAP_2][0];
  cache->max_height[DT_MIPMAP_F] = mipsizes[DT_MIPMAP_2][1];
  for(int k = DT_MIPMAP_F-1; k >= 0; k--)
  {
    cache->max_width[k]  = mipsizes[k][0];
    cache->max_height[k] = mipsizes[k][1];
  }
    // header + buffer
  for(int k = DT_MIPMAP_F-1; k >= 0; k--)
    cache->buffer_size[k] = _get_entry_size(cache->max_width[k] * cache->max_height[k] * 4);

  // clear stats:
  cache->mip_thumbs.stats_requests = 0;
  cache->mip_thumbs.stats_near_match = 0;
  cache->mip_thumbs.stats_misses = 0;
  cache->mip_thumbs.stats_fetches = 0;
  cache->mip_thumbs.stats_standin = 0;
  cache->mip_f.stats_requests = 0;
  cache->mip_f.stats_near_match = 0;
  cache->mip_f.stats_misses = 0;
  cache->mip_f.stats_fetches = 0;
  cache->mip_f.stats_standin = 0;
  cache->mip_full.stats_requests = 0;
  cache->mip_full.stats_near_match = 0;
  cache->mip_full.stats_misses = 0;
  cache->mip_full.stats_fetches = 0;
  cache->mip_full.stats_standin = 0;

  dt_cache_init(&cache->mip_thumbs.cache, 0, dt_get_mipmap_mem());
  dt_cache_set_allocate_callback(&cache->mip_thumbs.cache, dt_mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_thumbs.cache, dt_mipmap_cache_deallocate_dynamic, cache);

  dt_cache_init(&cache->mip_full.cache, 0, darktable.num_openmp_threads + DT_CTL_WORKER_RESERVED);
  dt_cache_set_allocate_callback(&cache->mip_full.cache, dt_mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_full.cache, dt_mipmap_cache_deallocate_dynamic, cache);
  cache->buffer_size[DT_MIPMAP_FULL] = 0;

  dt_cache_init(&cache->mip_f.cache, 0, darktable.num_openmp_threads + DT_CTL_WORKER_RESERVED);
  dt_cache_set_allocate_callback(&cache->mip_f.cache, dt_mipmap_cache_allocate_dynamic, cache);
  dt_cache_set_cleanup_callback(&cache->mip_f.cache, dt_mipmap_cache_deallocate_dynamic, cache);
  cache->buffer_size[DT_MIPMAP_F]
      = _get_entry_size(4 * sizeof(float) * cache->max_width[DT_MIPMAP_F] * cache->max_height[DT_MIPMAP_F]);
}

void dt_mipmap_cache_cleanup(dt_mipmap_cache_t *cache)
{
  dt_mipmap_cache_print(cache);
  dt_cache_cleanup(&cache->mip_thumbs.cache);
  dt_cache_cleanup(&cache->mip_full.cache);
  dt_cache_cleanup(&cache->mip_f.cache);
}

void dt_mipmap_cache_print(dt_mipmap_cache_t *cache)
{
  dt_print(DT_DEBUG_CACHE, "[mipmap_cache] thumbs fill %.2f/%.2f MB (%.2f%%)\n",
         cache->mip_thumbs.cache.cost / (1024.0 * 1024.0),
         cache->mip_thumbs.cache.cost_quota / (1024.0 * 1024.0),
         100.0f * (float)cache->mip_thumbs.cache.cost / (float)cache->mip_thumbs.cache.cost_quota);
  dt_print(DT_DEBUG_CACHE, "[mipmap_cache] float fill %"PRIu32"/%"PRIu32" slots (%.2f%%)\n",
         (uint32_t)cache->mip_f.cache.cost, (uint32_t)cache->mip_f.cache.cost_quota,
         100.0f * (float)cache->mip_f.cache.cost / (float)cache->mip_f.cache.cost_quota);
  dt_print(DT_DEBUG_CACHE, "[mipmap_cache] full  fill %"PRIu32"/%"PRIu32" slots (%.2f%%)\n",
         (uint32_t)cache->mip_full.cache.cost, (uint32_t)cache->mip_full.cache.cost_quota,
         100.0f * (float)cache->mip_full.cache.cost / (float)cache->mip_full.cache.cost_quota);

  uint64_t sum = 0;
  uint64_t sum_fetches = 0;
  uint64_t sum_standins = 0;
  sum += cache->mip_thumbs.stats_requests;
  sum_fetches += cache->mip_thumbs.stats_fetches;
  sum_standins += cache->mip_thumbs.stats_standin;
  sum += cache->mip_f.stats_requests;
  sum_fetches += cache->mip_f.stats_fetches;
  sum_standins += cache->mip_f.stats_standin;
  sum += cache->mip_full.stats_requests;
  sum_fetches += cache->mip_full.stats_fetches;
  sum_standins += cache->mip_full.stats_standin;
  dt_print(DT_DEBUG_CACHE, "[mipmap_cache] level | near match | miss | stand-in | fetches | total rq\n");
  dt_print(DT_DEBUG_CACHE, "[mipmap_cache] thumb | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%\n",
         100.0 * cache->mip_thumbs.stats_near_match / (float)cache->mip_thumbs.stats_requests,
         100.0 * cache->mip_thumbs.stats_misses / (float)cache->mip_thumbs.stats_requests,
         100.0 * cache->mip_thumbs.stats_standin / (float)sum_standins,
         100.0 * cache->mip_thumbs.stats_fetches / (float)sum_fetches,
         100.0 * cache->mip_thumbs.stats_requests / (float)sum);
  dt_print(DT_DEBUG_CACHE, "[mipmap_cache] float | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%\n",
         100.0 * cache->mip_f.stats_near_match / (float)cache->mip_f.stats_requests,
         100.0 * cache->mip_f.stats_misses / (float)cache->mip_f.stats_requests,
         100.0 * cache->mip_f.stats_standin / (float)sum_standins,
         100.0 * cache->mip_f.stats_fetches / (float)sum_fetches,
         100.0 * cache->mip_f.stats_requests / (float)sum);
  dt_print(DT_DEBUG_CACHE, "[mipmap_cache] full  | %6.2f%% | %6.2f%% | %6.2f%%  | %6.2f%% | %6.2f%%\n",
         100.0 * cache->mip_full.stats_near_match / (float)cache->mip_full.stats_requests,
         100.0 * cache->mip_full.stats_misses / (float)cache->mip_full.stats_requests,
         100.0 * cache->mip_full.stats_standin / (float)sum_standins,
         100.0 * cache->mip_full.stats_fetches / (float)sum_fetches,
         100.0 * cache->mip_full.stats_requests / (float)sum);
  dt_print(DT_DEBUG_CACHE, "\n\n");
}

static dt_mipmap_cache_one_t *_get_cache(dt_mipmap_cache_t *cache, const dt_mipmap_size_t mip)
{
  switch(mip)
  {
    case DT_MIPMAP_FULL:
      return &cache->mip_full;
    case DT_MIPMAP_F:
      return &cache->mip_f;
    default:
      return &cache->mip_thumbs;
  }
}

// if we get a zero-sized image, paint skulls to signal a missing image
static void _paint_skulls(dt_mipmap_buffer_t *buf, struct dt_mipmap_buffer_dsc *dsc, const dt_mipmap_size_t mip)
{
  if(dsc->width == 0 || dsc->height == 0)
  {
    // fprintf(stderr, "[mipmap cache get] got a zero-sized image for img %u mip %d!\n", imgid, mip);
    if(mip < DT_MIPMAP_F)
      buf->buf = dead_image_8(dsc);
    else
      buf->buf = NULL; // full images with NULL buffer have to be handled, indicates `missing image', but still return locked slot
  }
}

static void _validate_buffer(dt_mipmap_buffer_t *buf, struct dt_mipmap_buffer_dsc *dsc, const int32_t imgid,
                             const dt_mipmap_size_t mip)
{
  // Unpoison the dsc region
  ASAN_UNPOISON_MEMORY_REGION(dsc, dt_mipmap_buffer_dsc_size);

  // May update buf->buf:
  _sync_dsc_to_buf(buf, dsc, imgid, mip);

  // Unpoison the pixel buffer region
  ASAN_UNPOISON_MEMORY_REGION(buf->buf, dsc->size - dt_mipmap_buffer_dsc_size);
}

// Grab our own local copy of the image structure
static gboolean _get_image_copy(const int32_t imgid, dt_image_t *buffered_image)
{
  // load the image:
  // make sure we access the r/w lock as shortly as possible!
  gboolean no_buffer = TRUE;
  const dt_image_t *cimg = dt_image_cache_get(darktable.image_cache, imgid, 'r');

  if(cimg)
  {
    *buffered_image = *cimg;
    no_buffer = FALSE;
  }

  dt_image_cache_read_release(darktable.image_cache, cimg);

  return no_buffer;
}

// Actually do the work: produce an image
static void _generate_blocking(dt_cache_entry_t *entry, dt_mipmap_buffer_t *buf,
                               const int32_t imgid, const dt_mipmap_size_t mip)
{
  struct dt_mipmap_buffer_dsc *dsc = _get_dsc_from_entry(entry);

  if(!(dsc->flags & DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE))
  {
    // Already in cache, no I/O needed
    dt_print(DT_DEBUG_CACHE, "[mipmap_cache] skipping I/O for %i at mip %i (%ix%i), found in cache.\n", imgid, mip,
             dsc->width, dsc->height);
    _validate_buffer(buf, dsc, imgid, mip);
    return;
  }

  if(mip == DT_MIPMAP_FULL)
  {
    dt_image_t buffered_image;
    if(_get_image_copy(imgid, &buffered_image)) return;

    // Get the input file path, possibly from our local cache of copied images
    char filename[PATH_MAX] = { 0 };
    gboolean from_cache = TRUE;
    dt_image_full_path(buffered_image.id,  filename,  sizeof(filename),  &from_cache, __FUNCTION__);

    dt_print(DT_DEBUG_CACHE,
      "[mipmap_cache] fetch mip %d float32 for image %i (%ix%i) from original file I/O \n", mip,
      imgid, dsc->width, dsc->height);

    // will call dt_mipmap_cache_alloc() internally:
    dt_imageio_retval_t ret = dt_imageio_open(&buffered_image, filename, buf);

    // We need to update dsc because dt_imageio_open re-alloc entry->data
    dsc = _get_dsc_from_entry(entry);

    if(ret == DT_IMAGEIO_OK)
    {
      // swap back new image data, may contain updated EXIF & colorspace
      dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'w');
      *img = buffered_image;
      dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
    }
    else
    {
      dsc->width = dsc->height = 0;
      dsc->iscale = 0.f;
    }
  }
  else if(mip == DT_MIPMAP_F)
  {
    dt_print(DT_DEBUG_CACHE,
             "[mipmap_cache] compute mip %d float32 for image %i (%ix%i) from original file \n", mip,
             imgid, dsc->width, dsc->height);
    _init_f(buf, (float *)_get_buffer_from_dsc(dsc), &dsc->width, &dsc->height, &dsc->iscale, imgid);
  }
  else
  {
    // 8-bit thumbs
    dt_print(DT_DEBUG_CACHE,
             "[mipmap_cache] compute mip %d uint8 for image %i (%ix%i) from original file \n", mip,
             imgid, dsc->width, dsc->height);
    _init_8((uint8_t *)_get_buffer_from_dsc(dsc), &dsc->width, &dsc->height, &dsc->iscale, &dsc->color_space, imgid, mip);
  }
  dsc->flags &= ~DT_MIPMAP_BUFFER_DSC_FLAG_GENERATE;
  _paint_skulls(buf, dsc, mip);
  _validate_buffer(buf, dsc, imgid, mip);

  dt_print(DT_DEBUG_CACHE, "%i (mip %i) got a %ix%i / %ix%i image at %p\n", imgid, mip, buf->width, buf->height, dsc->width, dsc->height, buf->buf);
}


void dt_mipmap_cache_get_with_caller(dt_mipmap_cache_t *cache, dt_mipmap_buffer_t *buf, const int32_t imgid,
                                     const dt_mipmap_size_t mip, const dt_mipmap_get_flags_t flags,
                                     const char mode, const char *file, int line)
{
  assert(mip <= DT_MIPMAP_NONE && mip >= DT_MIPMAP_0);

  const int32_t key = get_key(imgid, mip);

  // Allocation functions will check those if we run them
  buf->imgid = imgid;
  buf->size = mip;

  if(flags == DT_MIPMAP_TESTLOCK)
  {
    // simple case: only get and lock if it's there.
    dt_cache_entry_t *entry = dt_cache_testget(&_get_cache(cache, mip)->cache, key, mode);
    buf->cache_entry = entry;

    if(entry)
      _validate_buffer(buf, _get_dsc_from_entry(entry), imgid, mip);
    else
      _invalidate_buffer(buf);
  }
  else if(flags == DT_MIPMAP_BLOCKING)
  {
    // simple case: blocking get
    dt_cache_entry_t *entry =  dt_cache_get_with_caller(&_get_cache(cache, mip)->cache, key, mode, file, line);
    buf->cache_entry = entry;
    __sync_fetch_and_add(&(_get_cache(cache, mip)->stats_fetches), 1);
    _generate_blocking(entry, buf, imgid, mip);

    // image cache is leaving the write lock in place in case the image has been newly allocated.
    // this leads to a slight increase in thread contention, so we opt for dropping the write lock
    // and acquiring a read lock immediately after. since this opens a small window for other threads
    // to get in between, we need to take some care to re-init cache entries and dsc.
    // note that concurrencykit has rw locks that can be demoted from w->r without losing the lock in between.
    if(mode == 'r')
    {
      entry->_lock_demoting = 1;
      // drop the write lock
      dt_cache_release(&_get_cache(cache, mip)->cache, entry);
      // get a read lock
      entry = dt_cache_get(&_get_cache(cache, mip)->cache, key, mode);
      entry->_lock_demoting = 0;
    }

    buf->cache_entry = entry;

    // The cache can't be locked twice from the same thread,
    // because then it would never unlock.
#ifdef _DEBUG
    const pthread_t writer = dt_pthread_rwlock_get_writer(&(buf->cache_entry->lock));
    if(mode == 'w')
    {
      assert(pthread_equal(writer, pthread_self()));
    }
    else
    {
      assert(!pthread_equal(writer, pthread_self()));
    }
#endif
  }
  else
  {
    _invalidate_buffer(buf);
  }
}

void dt_mipmap_cache_write_get_with_caller(dt_mipmap_cache_t *cache, dt_mipmap_buffer_t *buf, const int32_t imgid, const int mip, const char *file, int line)
{
  dt_mipmap_cache_get_with_caller(cache, buf, imgid, mip, DT_MIPMAP_BLOCKING, 'w', file, line);
}

void dt_mipmap_cache_release_with_caller(dt_mipmap_cache_t *cache, dt_mipmap_buffer_t *buf, const char *file,
                                         int line)
{
  if(buf->size == DT_MIPMAP_NONE || !buf->cache_entry) return;
  assert(buf->imgid > 0);
  assert(buf->size >= DT_MIPMAP_0);
  assert(buf->size < DT_MIPMAP_NONE);
  dt_cache_release_with_caller(&_get_cache(cache, buf->size)->cache, buf->cache_entry, file, line);
  buf->size = DT_MIPMAP_NONE;
  buf->buf = NULL;
}


// return index dt_mipmap_size_t having at least width & height requested instead of minimum combined diff
// please note that the requested size is in pixels not dots.
dt_mipmap_size_t dt_mipmap_cache_get_matching_size(const dt_mipmap_cache_t *cache, const int32_t width,
                                                   const int32_t height)
{
  for(int k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
  {
    if((cache->max_width[k] >= width) && (cache->max_height[k] >= height))
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dt_mipmap_cache_get_matching_size] will load a mipmap of size %lux%lu px\n", cache->max_width[k], cache->max_height[k]);
      return k;
    }
  }
  return DT_MIPMAP_F - 1;
}

void dt_mipmap_cache_remove_at_size(dt_mipmap_cache_t *cache, const int32_t imgid, const dt_mipmap_size_t mip, const gboolean flush_disk)
{
  if(mip >= DT_MIPMAP_F || mip < DT_MIPMAP_0) return;

  // get rid of all ldr thumbnails:
  const uint32_t key = get_key(imgid, mip);
  dt_cache_entry_t *entry = dt_cache_testget(&_get_cache(cache, mip)->cache, key, 'w');
  if(entry)
  {
    struct dt_mipmap_buffer_dsc *dsc = _get_dsc_from_entry(entry);
    ASAN_UNPOISON_MEMORY_REGION(dsc, dt_mipmap_buffer_dsc_size);
    if(flush_disk) dsc->flags |= DT_MIPMAP_BUFFER_DSC_FLAG_INVALIDATE;
    dt_cache_release(&_get_cache(cache, mip)->cache, entry);
    dt_cache_remove(&_get_cache(cache, mip)->cache, key);
  }
  else
  {
    // directly remove the file on disk cache even if we don't have a memory entry
    dt_mipmap_cache_unlink_ondisk_thumbnail(cache, imgid, mip);
  }
}

// get rid of all ldr thumbnails:
void dt_mipmap_cache_remove(dt_mipmap_cache_t *cache, const int32_t imgid, const gboolean flush_disk)
{
  for(dt_mipmap_size_t k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
    dt_mipmap_cache_remove_at_size(cache, imgid, k, flush_disk);
}

// write thumbnail to disc if not existing there
void dt_mimap_cache_evict(dt_mipmap_cache_t *cache, const int32_t imgid)
{
  for(dt_mipmap_size_t k = DT_MIPMAP_0; k < DT_MIPMAP_F; k++)
    dt_cache_remove(&_get_cache(cache, k)->cache, get_key(imgid, k));
}

static void _init_f(dt_mipmap_buffer_t *mipmap_buf, float *out, uint32_t *width, uint32_t *height, float *iscale,
                    const int32_t imgid)
{
  const uint32_t wd = *width, ht = *height;

  /* do not even try to process file if it isn't available */
  char filename[PATH_MAX] = { 0 };
  gboolean from_cache = TRUE;
  dt_image_full_path(imgid,  filename,  sizeof(filename),  &from_cache, __FUNCTION__);
  if(!*filename || !g_file_test(filename, G_FILE_TEST_EXISTS))
  {
    *width = *height = 0;
    *iscale = 0.0f;
    return;
  }

  dt_mipmap_buffer_t buf;
  dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');

  // lock image after we have the buffer, we might need to lock the image struct for
  // writing during raw loading, to write to width/height.
  const dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');

  dt_iop_roi_t roi_in, roi_out;
  roi_in.x = roi_in.y = 0;
  roi_in.width = image->width;
  roi_in.height = image->height;
  roi_in.scale = 1.0f;

  roi_out.x = roi_out.y = 0;

  // now let's figure out the scaling...

  // MIP_F is 4 channels, and we do not demosaic here
  roi_out.scale = fminf(((float)wd) / (float)image->width, ((float)ht) / (float)image->height);
  roi_out.width = roi_out.scale * roi_in.width;
  roi_out.height = roi_out.scale * roi_in.height;

  if(!buf.buf || buf.width == 0 || buf.height == 0)
  {
    dt_image_cache_read_release(darktable.image_cache, image);
    *width = *height = 0;
    *iscale = 0.0f;
    return;
  }

  mipmap_buf->color_space = DT_COLORSPACE_NONE; // TODO: do we need that information in this buffer?

  if(image->buf_dsc.filters)
  {
    if(image->buf_dsc.filters != 9u && image->buf_dsc.datatype == TYPE_FLOAT)
    {
      dt_iop_clip_and_zoom_mosaic_half_size_f((float *const)out, (const float *const)buf.buf, &roi_out, &roi_in,
                                              roi_out.width, roi_in.width, image->buf_dsc.filters);
    }
    else if(image->buf_dsc.filters != 9u && image->buf_dsc.datatype == TYPE_UINT16)
    {
      dt_iop_clip_and_zoom_mosaic_half_size((uint16_t * const)out, (const uint16_t *)buf.buf, &roi_out, &roi_in,
                                            roi_out.width, roi_in.width, image->buf_dsc.filters);
    }
    else if(image->buf_dsc.filters == 9u && image->buf_dsc.datatype == TYPE_UINT16)
    {
      dt_iop_clip_and_zoom_mosaic_third_size_xtrans((uint16_t * const)out, (const uint16_t *)buf.buf, &roi_out,
                                                    &roi_in, roi_out.width, roi_in.width, image->buf_dsc.xtrans);
    }
    else if(image->buf_dsc.filters == 9u && image->buf_dsc.datatype == TYPE_FLOAT)
    {
      dt_iop_clip_and_zoom_mosaic_third_size_xtrans_f(out, (const float *)buf.buf, &roi_out, &roi_in,
                                                      roi_out.width, roi_in.width, image->buf_dsc.xtrans);
    }
    else
    {
      dt_unreachable_codepath();
    }
  }
  else
  {
    // downsample
    dt_iop_clip_and_zoom(out, (const float *)buf.buf, &roi_out, &roi_in, roi_out.width, roi_in.width);
  }

  dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

  *width = roi_out.width;
  *height = roi_out.height;
  *iscale = (float)image->width / (float)roi_out.width;

  dt_image_cache_read_release(darktable.image_cache, image);
}


// dummy functions for `export' to mipmap buffers:
typedef struct _dummy_data_t
{
  dt_imageio_module_data_t head;
  uint8_t *buf;
} _dummy_data_t;

static int _levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static int _bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int _write_image(dt_imageio_module_data_t *data, const char *filename, const void *in,
                        dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                        void *exif, int exif_len, int32_t imgid, int num, int total, dt_dev_pixelpipe_t *pipe,
                        const gboolean export_masks)
{
  _dummy_data_t *d = (_dummy_data_t *)data;
  memcpy(d->buf, in, sizeof(uint32_t) * data->width * data->height);
  return 0;
}

static int _load_jpg(const char *filename, const int32_t imgid, const uint32_t wd, const uint32_t ht,
                     const dt_mipmap_size_t size, const dt_image_orientation_t orientation, uint8_t *buf,
                     uint32_t *width, uint32_t *height, dt_colorspaces_color_profile_type_t *color_space)
{
  int res = 1;
  dt_imageio_jpeg_t jpg;
  if(!dt_imageio_jpeg_read_header(filename, &jpg))
  {
    *color_space = dt_imageio_jpeg_read_color_space(&jpg);
    uint8_t *tmp = (uint8_t *)dt_alloc_align(sizeof(uint8_t) * jpg.width * jpg.height * 4);
    if(tmp && !dt_imageio_jpeg_read(&jpg, tmp))
    {
      // scale to fit
      dt_print(DT_DEBUG_CACHE, "[mipmap_cache] generate mip %d for image %d from jpeg\n", size, imgid);
      dt_iop_flip_and_zoom_8(tmp, jpg.width, jpg.height, buf, wd, ht, orientation, width, height);
      res = 0;
    }
    if(tmp) dt_free_align(tmp);
  }
  return res;
}

static int _find_sidecar_jpg(const char *filename, const char *ext, char *sidecar)
{
  const size_t filename_len = strlen(filename) - strlen(ext);
  const char *exts[4] = { ".jpg", ".JPG", ".jpeg", ".JPEG" };

  for(int i = 0; i < 4; i++)
  {
    // Damage control. Should never happen.
    if(filename_len + strlen(exts[i]) >= PATH_MAX)
      continue;

    // Construct the sidecar filename
    const size_t str_copy = g_snprintf(sidecar, PATH_MAX, "%.*s%s", (int)filename_len, filename, exts[i]);

    // Check if the filename was too long or if the copy failed.
    if (str_copy == 0 || str_copy >= PATH_MAX)
      continue;

    if(g_file_test(sidecar, G_FILE_TEST_EXISTS))
      return 1;
  }

  return 0;
}

static void _init_8(uint8_t *buf, uint32_t *width, uint32_t *height, float *iscale,
                    dt_colorspaces_color_profile_type_t *color_space, const int32_t imgid,
                    const dt_mipmap_size_t size)
{
  if(size >= DT_MIPMAP_F || *width < 16 || *height < 16) return;

  *iscale = 1.0f;
  const uint32_t wd = *width, ht = *height;

  char filename[PATH_MAX] = { 0 };
  char ext[6] = { 0 };
  gboolean input_exists, is_jpg_input, use_embedded_jpg;
  _write_mipmap_to_disk(imgid, filename, ext, &input_exists, &is_jpg_input, &use_embedded_jpg, NULL);

  /* do not even try to process file if it isn't available */
  if(!input_exists)
  {
    *width = *height = 0;
    *iscale = 0.0f;
    *color_space = DT_COLORSPACE_NONE;
    return;
  }

  int res = 1;

  // try to generate mip from larger mip
  // This expects that invalid mips will be flushed, so the assumption is:
  // if mip then it's valid (with regard to current history)
  if(res && size < DT_MIPMAP_F - 1)
  {
    for(dt_mipmap_size_t k = size + 1; k < DT_MIPMAP_F; k++)
    {
      dt_mipmap_buffer_t tmp;
      dt_mipmap_cache_get(darktable.mipmap_cache, &tmp, imgid, k, DT_MIPMAP_TESTLOCK, 'r');
      if(tmp.buf == NULL) continue;

      dt_print(DT_DEBUG_CACHE, "[mipmap_cache] generate mip %d for image %d from level %d\n", size, imgid, k);
      *color_space = tmp.color_space;
      // downsample
      dt_iop_flip_and_zoom_8(tmp.buf, tmp.width, tmp.height, buf, wd, ht, ORIENTATION_NONE, width, height);

      dt_mipmap_cache_release(darktable.mipmap_cache, &tmp);
      res = 0;
      break;
    }
  }

  // Orientation is only needed when loading embedded JPEGs; avoid extra SQL otherwise.
  dt_image_orientation_t orientation = ORIENTATION_NONE;
  if(use_embedded_jpg)
  {
    dt_thumbnail_image_info_t info = { 0 };
    if(_mipmap_get_thumb_info(imgid, &info))
    {
      orientation = (info.orientation != ORIENTATION_NULL) ? info.orientation : ORIENTATION_NONE;
    }
    else
    {
      orientation = dt_image_get_orientation(imgid);
    }
  }

  if(res && use_embedded_jpg)
  {
    char sidecar_filename[PATH_MAX] = { 0 };

    if(is_jpg_input)
    {
      // Input file is a JPEG
      res = _load_jpg(filename, imgid, wd, ht, size, orientation, buf, width, height, color_space);
    }
    else if(_find_sidecar_jpg(filename, ext, sidecar_filename))
    {
      // input file is a RAW but we have a companion JPEG file in the same folder:
      // use it in priority (it may be higher resolution/quality than embedded JPEG).
      res = _load_jpg(sidecar_filename, imgid, wd, ht, size, orientation, buf, width, height, color_space);
    }
    else
    {
      // input file is a RAW without companion JPEG:
      // try to load the embedded thumbnail. Might not be large enough though.
      uint8_t *tmp = NULL;
      int32_t thumb_width, thumb_height;
      res = dt_imageio_large_thumbnail(filename, &tmp, &thumb_width, &thumb_height, color_space, *width, *height);
      if(!res)
      {
        // We take the thumbnail no matter its size. It might be too small for the requested dimension,
        // and end up blurry. But it's less bad than the following scenario:
        // 1. user displays collections in grid of 5 columns (small thumbnail -> fetch embedded JPEG for performance, all good),
        // 2. user zooms in the grid, to 2 or 3 columns,
        // 3. suddently, embedded JPEG is too small so we ditch it for a full pipe recompute,
        // 4. but then, color/contrast/appearance unexpectedly changes and user doesn't understand WTF just happened.
        // Blurry is less bad than randomly inconsistent, plus user has a GUI way in lighttable to
        // change how thumbs are processed at runtime.
        dt_print(DT_DEBUG_CACHE, "[mipmap_cache] generate mip %d for image %d from embedded jpeg\n", size, imgid);
        dt_iop_flip_and_zoom_8(tmp, thumb_width, thumb_height, buf, wd, ht, orientation, width, height);
        dt_pixelpipe_cache_free_align(tmp);
      }
    }
  }

  if(res)
  {
    // try the real thing: rawspeed + pixelpipe
    dt_imageio_module_format_t format;
    _dummy_data_t dat;
    format.bpp = _bpp;
    format.write_image = _write_image;
    format.levels = _levels;
    dat.head.max_width = wd;
    dat.head.max_height = ht;
    dat.buf = buf;
    // export with flags: ignore exif (don't load from disk), don't swap byte order, don't do hq processing,
    // no upscaling and signal we want thumbnail export
    res = dt_imageio_export_with_flags(imgid, "unused", &format, (dt_imageio_module_data_t *)&dat, TRUE, FALSE, FALSE,
                                       FALSE, TRUE, NULL, FALSE, FALSE, DT_COLORSPACE_NONE, NULL, DT_INTENT_LAST, NULL,
                                       NULL, 1, 1, NULL);
    if(!res)
    {
      dt_print(DT_DEBUG_CACHE, "[mipmap_cache] generated mip %d for image %d from scratch\n", size, imgid);
      // might be smaller, or have a different aspect than what we got as input.
      *width = dat.head.width;
      *height = dat.head.height;
      *iscale = 1.0f;
      *color_space = DT_COLORSPACE_ADOBERGB;
    }
  }

  // any errors?
  if(res)
  {
    // fprintf(stderr, "[mipmap_cache] could not process thumbnail!\n");
    *width = *height = 0;
    *iscale = 0.0f;
    *color_space = DT_COLORSPACE_NONE;
  }
}

void dt_mipmap_cache_copy_thumbnails(const dt_mipmap_cache_t *cache, const uint32_t dst_imgid, const uint32_t src_imgid)
{
  gboolean write_to_disk_src, write_to_disk_dst;
  _write_mipmap_to_disk(src_imgid, NULL, NULL, NULL, NULL, NULL, &write_to_disk_src);
  _write_mipmap_to_disk(dst_imgid, NULL, NULL, NULL, NULL, NULL, &write_to_disk_dst);

  if(cache->cachedir[0] && write_to_disk_src && write_to_disk_dst)
  {
    for(dt_mipmap_size_t mip = DT_MIPMAP_0; mip < DT_MIPMAP_F; mip++)
    {
      // try and load from disk, if successful set flag
      char srcpath[PATH_MAX] = {0};
      char dstpath[PATH_MAX] = {0};
      snprintf(srcpath, sizeof(srcpath), "%s.d/%d/%"PRIu32".jpg", cache->cachedir, (int)mip, src_imgid);
      snprintf(dstpath, sizeof(dstpath), "%s.d/%d/%"PRIu32".jpg", cache->cachedir, (int)mip, dst_imgid);
      GFile *src = g_file_new_for_path(srcpath);
      GFile *dst = g_file_new_for_path(dstpath);
      GError *gerror = NULL;
      g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, &gerror);
      // ignore errors, we tried what we could.
      g_object_unref(dst);
      g_object_unref(src);
      g_clear_error(&gerror);
    }
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
