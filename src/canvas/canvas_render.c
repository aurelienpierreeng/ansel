/*
    This file is part of Ansel,
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

#include "canvas/canvas_render.h"

#include "caches/image_cache.h"
#include "colorprofiles/colorspaces.h"
#include "common/image.h"
#include "control/control.h"
#include "control/jobs.h"
#include "imageio/imageio_core.h"
#include "imageio/imageio_jpeg.h"
#include "imageio/imageio_module.h"
#include "system/atomic.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <glib/gi18n.h>
#include <lcms2.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* --- identity --------------------------------------------------------------- */

gboolean dt_canvas_render_describe_source(int32_t imgid, dt_canvas_image_t *image)
{
  if(IS_NULL_PTR(image) || imgid <= 0) return FALSE;
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img)) return FALSE;

  image->imgid = img->id;
  image->version = img->version;
  image->film_id = img->film_id;
  image->history_hash = img->history_hash;
  image->source_width = img->width;
  image->source_height = img->height;
  image->orientation = (int32_t)img->orientation;
  dt_image_film_roll_directory(img, image->folder, sizeof(image->folder));
  g_strlcpy(image->filename, img->filename, sizeof(image->filename));
  g_strlcpy(image->exif_maker, img->exif_maker, sizeof(image->exif_maker));
  g_strlcpy(image->exif_model, img->exif_model, sizeof(image->exif_model));
  g_strlcpy(image->exif_lens, img->exif_lens, sizeof(image->exif_lens));
  image->exif_exposure = img->exif_exposure;
  image->exif_aperture = img->exif_aperture;
  image->exif_iso = img->exif_iso;
  image->exif_focal_length = img->exif_focal_length;
  image->exif_exposure_bias = img->exif_exposure_bias;
  image->exif_datetime_taken = (int64_t)img->exif_datetime_taken;
  dt_image_cache_read_release(img);
  return TRUE;
}

/** Does the library image `imgid` still name the file the frame was rendered from? */
static gboolean _same_file(const int32_t imgid, const dt_canvas_image_t *image)
{
  if(imgid <= 0) return FALSE;
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img)) return FALSE;
  char folder[DT_PATH_MAX];
  dt_image_film_roll_directory(img, folder, sizeof(folder));
  const gboolean same = g_strcmp0(img->filename, image->filename) == 0 && g_strcmp0(folder, image->folder) == 0
                        && img->version == image->version;
  dt_image_cache_read_release(img);
  return same;
}

int32_t dt_canvas_render_locate_source(const dt_canvas_image_t *image)
{
  if(IS_NULL_PTR(image)) return -1;
  // The id is the fast answer while it still names the same file and version.
  if(_same_file(image->imgid, image)) return image->imgid;
  // Otherwise the file may have been re-imported under a new id: ask by path.
  if(image->folder[0] == '\0' || image->filename[0] == '\0') return -1;
  gchar *full_path = g_build_filename(image->folder, image->filename, NULL);
  int32_t found = dt_image_get_id_full_path(full_path);
  dt_free(full_path);
  if(found <= 0) return -1;
  // Several versions share a path: prefer the one the frame was rendered from, if it exists.
  if(image->version != 0)
  {
    const dt_image_t *img = dt_image_cache_get(found, 'r');
    if(!IS_NULL_PTR(img))
    {
      const gboolean version_matches = img->version == image->version;
      dt_image_cache_read_release(img);
      if(!version_matches)
      {
        // Walk the duplicates of that film roll and file name.
        const int32_t candidate = dt_image_get_id(image->film_id, image->filename);
        if(_same_file(candidate, image)) return candidate;
      }
    }
  }
  return found;
}

dt_canvas_sync_status_t dt_canvas_render_check(dt_canvas_image_t *image)
{
  if(IS_NULL_PTR(image)) return DT_CANVAS_SYNC_UNKNOWN;
  const int32_t imgid = dt_canvas_render_locate_source(image);
  if(imgid <= 0)
  {
    image->sync_status = DT_CANVAS_SYNC_MISSING;
    return image->sync_status;
  }
  const dt_image_t *img = dt_image_cache_get(imgid, 'r');
  if(IS_NULL_PTR(img))
  {
    image->sync_status = DT_CANVAS_SYNC_MISSING;
    return image->sync_status;
  }
  const uint64_t library_hash = img->history_hash;
  dt_image_cache_read_release(img);
  const gboolean rendered = !IS_NULL_PTR(image->jpeg);
  image->sync_status = (rendered && library_hash == image->history_hash) ? DT_CANVAS_SYNC_CURRENT
                                                                         : DT_CANVAS_SYNC_STALE;
  return image->sync_status;
}

/* --- rendering: the in-memory export format ---------------------------------- */

typedef struct dt_canvas_export_format_t
{
  dt_imageio_module_data_t head;
  uint8_t *pixels; ///< RGBA8, head.width * head.height * 4, written by write_image()
} dt_canvas_export_format_t;

static int _format_bpp(dt_imageio_module_data_t *data)
{
  return 8;
}

static int _format_levels(dt_imageio_module_data_t *data)
{
  return IMAGEIO_RGB | IMAGEIO_INT8;
}

static const char *_format_mime(dt_imageio_module_data_t *data)
{
  return "memory";
}

static int _format_flags(dt_imageio_module_data_t *data)
{
  return 0;
}

static int _format_write_image(dt_imageio_module_data_t *data, const char *filename, const void *in,
                               dt_colorspaces_color_profile_type_t over_type, const char *over_filename,
                               void *exif, int exif_len, int32_t imgid, int num, int total,
                               struct dt_dev_pixelpipe_t *pipe, const gboolean export_masks)
{
  dt_canvas_export_format_t *format = (dt_canvas_export_format_t *)data;
  const size_t size = (size_t)format->head.width * (size_t)format->head.height * 4;
  format->pixels = g_try_malloc(size);
  if(IS_NULL_PTR(format->pixels)) return 1;
  memcpy(format->pixels, in, size);
  return 0;
}

/* --- rendering: JPEG encoding with the sRGB profile ---------------------------- */

#define ICC_MARKER (JPEG_APP0 + 2)
#define ICC_OVERHEAD_LEN 14
#define MAX_BYTES_IN_MARKER 65533
#define MAX_DATA_BYTES_IN_MARKER (MAX_BYTES_IN_MARKER - ICC_OVERHEAD_LEN)

typedef struct dt_canvas_jpeg_error_t
{
  struct jpeg_error_mgr pub;
  jmp_buf setjmp_buffer;
} dt_canvas_jpeg_error_t;

static void _jpeg_error_exit(j_common_ptr cinfo)
{
  dt_canvas_jpeg_error_t *error = (dt_canvas_jpeg_error_t *)cinfo->err;
  longjmp(error->setjmp_buffer, 1);
}

/** The APP2 ICC_PROFILE marker sequence, as libjpeg's own example writes it. */
static void _jpeg_write_icc(j_compress_ptr cinfo, const uint8_t *icc_data, uint32_t icc_length)
{
  int marker_index = 1;
  while(icc_length > 0)
  {
    uint32_t chunk = icc_length;
    if(chunk > MAX_DATA_BYTES_IN_MARKER) chunk = MAX_DATA_BYTES_IN_MARKER;
    icc_length -= chunk;
    const uint32_t remaining_markers = icc_length / MAX_DATA_BYTES_IN_MARKER + (icc_length % MAX_DATA_BYTES_IN_MARKER ? 1 : 0);
    jpeg_write_m_header(cinfo, ICC_MARKER, chunk + ICC_OVERHEAD_LEN);
    const char identifier[12] = "ICC_PROFILE";
    for(int idx = 0; idx < 12; idx++) jpeg_write_m_byte(cinfo, (int)identifier[idx]);
    jpeg_write_m_byte(cinfo, marker_index);
    jpeg_write_m_byte(cinfo, marker_index + (int)remaining_markers);
    for(uint32_t idx = 0; idx < chunk; idx++) jpeg_write_m_byte(cinfo, icc_data[idx]);
    icc_data += chunk;
    marker_index++;
  }
}

static uint8_t *_srgb_profile_bytes(uint32_t *length)
{
  *length = 0;
  const dt_colorspaces_color_profile_t *entry = dt_colorspaces_get_profile(DT_COLORSPACE_SRGB, "", DT_PROFILE_ROLE_OUTPUT);
  if(IS_NULL_PTR(entry)) return NULL;
  uint8_t *bytes = NULL;
  dt_colorspaces_lock_profile(entry);
  if(!IS_NULL_PTR(entry->profile))
  {
    cmsUInt32Number size = 0;
    cmsSaveProfileToMem(entry->profile, NULL, &size);
    if(size > 0)
    {
      bytes = g_try_malloc(size);
      if(!IS_NULL_PTR(bytes) && cmsSaveProfileToMem(entry->profile, bytes, &size))
        *length = size;
      else
        dt_free(bytes);
    }
  }
  dt_colorspaces_unlock_profile(entry);
  return bytes;
}

static GBytes *_encode_jpeg(const uint8_t *rgba, const int width, const int height, const int quality)
{
  dt_canvas_jpeg_error_t error;
  struct jpeg_compress_struct cinfo;
  unsigned char *out_buffer = NULL;
  unsigned long out_size = 0;
  JSAMPROW row = NULL;
  uint32_t icc_length = 0;
  uint8_t *icc = _srgb_profile_bytes(&icc_length);

  cinfo.err = jpeg_std_error(&error.pub);
  error.pub.error_exit = _jpeg_error_exit;
  if(setjmp(error.setjmp_buffer))
  {
    jpeg_destroy_compress(&cinfo);
    dt_free(row);
    dt_free(icc);
    if(!IS_NULL_PTR(out_buffer)) free(out_buffer);
    return NULL;
  }
  jpeg_create_compress(&cinfo);
  jpeg_mem_dest(&cinfo, &out_buffer, &out_size);
  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, CLAMP(quality, 1, 100), TRUE);
  if(quality > 90) cinfo.comp_info[0].v_samp_factor = 1;
  if(quality > 92) cinfo.comp_info[0].h_samp_factor = 1;
  cinfo.optimize_coding = 1;
  jpeg_start_compress(&cinfo, TRUE);
  if(!IS_NULL_PTR(icc)) _jpeg_write_icc(&cinfo, icc, icc_length);

  row = g_malloc((size_t)width * 3);
  while(cinfo.next_scanline < cinfo.image_height)
  {
    const uint8_t *source = rgba + (size_t)cinfo.next_scanline * width * 4;
    for(int x = 0; x < width; x++)
    {
      row[3 * x + 0] = source[4 * x + 0];
      row[3 * x + 1] = source[4 * x + 1];
      row[3 * x + 2] = source[4 * x + 2];
    }
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  dt_free(row);
  dt_free(icc);
  // jpeg_mem_dest() allocates with malloc(), so the bytes carry free() as their destructor.
  return g_bytes_new_with_free_func(out_buffer, out_size, free, out_buffer);
}

/* --- rendering: the job ------------------------------------------------------ */

static dt_atomic_int _cancel_requested = 0;

typedef struct dt_canvas_render_job_t
{
  int32_t imgid;
  uint32_t object_id;
  uint64_t token;
  int32_t long_edge;
  int32_t quality;
  dt_canvas_render_done_t done;
  gpointer user_data;
  // result
  GBytes *jpeg;
  int32_t pixel_width;
  int32_t pixel_height;
  uint64_t history_hash;
} dt_canvas_render_job_t;

static gboolean _render_deliver(gpointer data)
{
  dt_canvas_render_job_t *params = (dt_canvas_render_job_t *)data;
  if(!IS_NULL_PTR(params->done))
    params->done(params->object_id, params->token, params->jpeg, params->pixel_width, params->pixel_height,
                 params->history_hash, params->user_data);
  if(!IS_NULL_PTR(params->jpeg)) g_bytes_unref(params->jpeg);
  dt_free(params);
  return G_SOURCE_REMOVE;
}

static int32_t _render_job_run(dt_job_t *job)
{
  dt_canvas_render_job_t *params = dt_control_job_get_params(job);
  if(IS_NULL_PTR(params)) return 1;

  if(dt_atomic_get_int(&_cancel_requested) || dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED)
  {
    g_main_context_invoke(NULL, _render_deliver, params);
    return 0;
  }

  // The history hash is read BEFORE the export so a commit landing during the render is
  // seen as "stale" by the next check rather than silently claimed by this render.
  const dt_image_t *img = dt_image_cache_get(params->imgid, 'r');
  if(IS_NULL_PTR(img))
  {
    g_main_context_invoke(NULL, _render_deliver, params);
    return 0;
  }
  params->history_hash = img->history_hash;
  char message[DT_MAX_FILENAME_LEN + 32];
  snprintf(message, sizeof(message), _("rendering `%s' for the canvas"), img->filename);
  dt_image_cache_read_release(img);
  dt_control_job_set_progress_message(job, message);

  dt_imageio_module_format_t format;
  memset(&format, 0, sizeof(format));
  format.mime = _format_mime;
  format.levels = _format_levels;
  format.bpp = _format_bpp;
  format.flags = _format_flags;
  format.write_image = _format_write_image;

  dt_canvas_export_format_t data;
  memset(&data, 0, sizeof(data));
  data.head.max_width = params->long_edge;
  data.head.max_height = params->long_edge;
  data.head.style[0] = '\0';

  const gboolean high_quality = TRUE;
  const gboolean is_scaling = FALSE;
  const int status = dt_imageio_export_with_flags(params->imgid, "unused", &format, (dt_imageio_module_data_t *)&data,
                                                  TRUE, FALSE, high_quality, is_scaling, FALSE, NULL, FALSE, FALSE,
                                                  DT_COLORSPACE_SRGB, "", DT_INTENT_PERCEPTUAL, NULL, NULL, 1, 1,
                                                  NULL, &_cancel_requested);
  if(status == 0 && !IS_NULL_PTR(data.pixels) && data.head.width > 0 && data.head.height > 0)
  {
    params->jpeg = _encode_jpeg(data.pixels, data.head.width, data.head.height, params->quality);
    params->pixel_width = data.head.width;
    params->pixel_height = data.head.height;
  }
  dt_free(data.pixels);
  dt_control_job_set_progress(job, 1.0);
  g_main_context_invoke(NULL, _render_deliver, params);
  return 0;
}

gboolean dt_canvas_render_start(int32_t imgid, uint32_t object_id, uint64_t token, int32_t long_edge,
                                int32_t quality, dt_canvas_render_done_t done, gpointer user_data)
{
  if(imgid <= 0 || IS_NULL_PTR(done)) return FALSE;
  dt_job_t *job = dt_control_job_create(&_render_job_run, "canvas render %d", imgid);
  if(IS_NULL_PTR(job)) return FALSE;
  dt_canvas_render_job_t *params = g_new0(dt_canvas_render_job_t, 1);
  params->imgid = imgid;
  params->object_id = object_id;
  params->token = token;
  params->long_edge = long_edge > 0 ? long_edge : 2048;
  params->quality = quality > 0 ? quality : 92;
  params->done = done;
  params->user_data = user_data;
  // The params are handed to the GUI thread by the run function and freed there; the job
  // itself owns nothing to free.
  dt_control_job_set_params(job, params, NULL);
  dt_atomic_set_int(&_cancel_requested, 0);
  dt_control_job_add_progress(job, _("canvas render"), TRUE);
  return dt_control_add_job(dt_control_get_global(), DT_JOB_QUEUE_USER_EXPORT, job) == 0;
}

void dt_canvas_render_cancel_all(void)
{
  dt_atomic_set_int(&_cancel_requested, 1);
}

/* --- decoding --------------------------------------------------------------- */

static uint8_t *_decode_rgba(GBytes *jpeg, int *width, int *height)
{
  gsize size = 0;
  const void *data = g_bytes_get_data(jpeg, &size);
  dt_imageio_jpeg_t decoder;
  memset(&decoder, 0, sizeof(decoder));
  if(dt_imageio_jpeg_decompress_header(data, size, &decoder) != 0) return NULL;
  if(decoder.width <= 0 || decoder.height <= 0) return NULL;
  uint8_t *rgba = g_try_malloc((size_t)decoder.width * decoder.height * 4);
  if(IS_NULL_PTR(rgba)) return NULL;
  if(dt_imageio_jpeg_decompress(&decoder, rgba) != 0)
  {
    dt_free(rgba);
    return NULL;
  }
  *width = decoder.width;
  *height = decoder.height;
  return rgba;
}

static void _rgba_to_bgra(const uint8_t *in, uint8_t *out, const size_t pixels)
{
  for(size_t idx = 0; idx < pixels; idx++)
  {
    out[4 * idx + 0] = in[4 * idx + 2];
    out[4 * idx + 1] = in[4 * idx + 1];
    out[4 * idx + 2] = in[4 * idx + 0];
    out[4 * idx + 3] = 255;
  }
}

static void _surface_data_free(void *data)
{
  g_free(data);
}

cairo_surface_t *dt_canvas_render_decode(GBytes *jpeg, gboolean for_display)
{
  if(IS_NULL_PTR(jpeg)) return NULL;
  int width = 0;
  int height = 0;
  uint8_t *rgba = _decode_rgba(jpeg, &width, &height);
  if(IS_NULL_PTR(rgba)) return NULL;

  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, width);
  uint8_t *bgra = g_try_malloc((size_t)stride * height);
  if(IS_NULL_PTR(bgra))
  {
    dt_free(rgba);
    return NULL;
  }
  const size_t pixels = (size_t)width * height;
  // cairo's RGB24 stride for a 4-byte pixel is 4 * width, so a packed conversion lands in place.
  if(for_display)
  {
    if(!dt_colorprofiles_rgba8_to_display_bgra8(rgba, bgra, width, height, DT_COLORSPACE_SRGB))
    {
      // No display transform: the module only swapped bytes, which is what we would have done.
    }
  }
  else
  {
    _rgba_to_bgra(rgba, bgra, pixels);
  }
  dt_free(rgba);

  cairo_surface_t *surface = cairo_image_surface_create_for_data(bgra, CAIRO_FORMAT_RGB24, width, height, stride);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surface);
    dt_free(bgra);
    return NULL;
  }
  static cairo_user_data_key_t pixel_key;
  cairo_surface_set_user_data(surface, &pixel_key, bgra, _surface_data_free);
  return surface;
}

void dt_canvas_render_color(const dt_canvas_color_t *color, gboolean for_display, double out[3])
{
  if(IS_NULL_PTR(color) || IS_NULL_PTR(out)) return;
  out[0] = CLAMP(color->red, 0.0f, 1.0f);
  out[1] = CLAMP(color->green, 0.0f, 1.0f);
  out[2] = CLAMP(color->blue, 0.0f, 1.0f);
  if(!for_display) return;
  // One pixel through the same transform the frames take, so a border matches its image.
  const uint8_t rgba[4] = { (uint8_t)lround(out[0] * 255.0), (uint8_t)lround(out[1] * 255.0),
                            (uint8_t)lround(out[2] * 255.0), 255 };
  uint8_t bgra[4] = { 0, 0, 0, 255 };
  if(dt_colorprofiles_rgba8_to_display_bgra8(rgba, bgra, 1, 1, DT_COLORSPACE_SRGB))
  {
    out[0] = bgra[2] / 255.0;
    out[1] = bgra[1] / 255.0;
    out[2] = bgra[0] / 255.0;
  }
}

/* --- the surface cache -------------------------------------------------------- */

typedef struct dt_canvas_cached_surface_t
{
  uint32_t object_id;
  GBytes *jpeg;              ///< a reference, so identity comparison stays valid
  uint64_t profile_generation;
  cairo_surface_t *surface;
  size_t bytes;
  uint64_t last_use;
} dt_canvas_cached_surface_t;

struct dt_canvas_surface_cache_t
{
  gboolean for_display;
  size_t budget;
  size_t used;
  uint64_t clock;
  GHashTable *entries; ///< object id -> dt_canvas_cached_surface_t
};

static void _cached_surface_free(gpointer data)
{
  dt_canvas_cached_surface_t *entry = (dt_canvas_cached_surface_t *)data;
  if(IS_NULL_PTR(entry)) return;
  if(!IS_NULL_PTR(entry->surface)) cairo_surface_destroy(entry->surface);
  if(!IS_NULL_PTR(entry->jpeg)) g_bytes_unref(entry->jpeg);
  dt_free(entry);
}

dt_canvas_surface_cache_t *dt_canvas_surface_cache_new(gboolean for_display, size_t budget_bytes)
{
  dt_canvas_surface_cache_t *cache = g_new0(dt_canvas_surface_cache_t, 1);
  cache->for_display = for_display;
  cache->budget = budget_bytes;
  cache->entries = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, _cached_surface_free);
  return cache;
}

void dt_canvas_surface_cache_free(dt_canvas_surface_cache_t *cache)
{
  if(IS_NULL_PTR(cache)) return;
  g_hash_table_destroy(cache->entries);
  dt_free(cache);
}

void dt_canvas_surface_cache_clear(dt_canvas_surface_cache_t *cache)
{
  if(IS_NULL_PTR(cache)) return;
  g_hash_table_remove_all(cache->entries);
  cache->used = 0;
}

static uint64_t _display_generation(void)
{
  dt_colorprofiles_settings_t settings;
  dt_colorprofiles_get_settings(&settings);
  return settings.generation;
}

static void _cache_evict_to_budget(dt_canvas_surface_cache_t *cache, const uint32_t keep_id)
{
  while(cache->used > cache->budget && g_hash_table_size(cache->entries) > 1)
  {
    dt_canvas_cached_surface_t *oldest = NULL;
    GHashTableIter iter;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, cache->entries);
    while(g_hash_table_iter_next(&iter, NULL, &value))
    {
      dt_canvas_cached_surface_t *entry = (dt_canvas_cached_surface_t *)value;
      if(entry->object_id == keep_id) continue;
      if(IS_NULL_PTR(oldest) || entry->last_use < oldest->last_use) oldest = entry;
    }
    if(IS_NULL_PTR(oldest)) return;
    cache->used -= oldest->bytes;
    g_hash_table_remove(cache->entries, GUINT_TO_POINTER(oldest->object_id));
  }
}

cairo_surface_t *dt_canvas_surface_cache_get(dt_canvas_surface_cache_t *cache, const dt_canvas_object_t *object)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(object) || object->kind != DT_CANVAS_OBJECT_IMAGE) return NULL;
  if(IS_NULL_PTR(object->image.jpeg)) return NULL;
  const uint64_t generation = cache->for_display ? _display_generation() : 0;
  cache->clock++;

  dt_canvas_cached_surface_t *entry = g_hash_table_lookup(cache->entries, GUINT_TO_POINTER(object->id));
  if(!IS_NULL_PTR(entry))
  {
    if(entry->jpeg == object->image.jpeg && entry->profile_generation == generation)
    {
      entry->last_use = cache->clock;
      return entry->surface;
    }
    cache->used -= entry->bytes;
    g_hash_table_remove(cache->entries, GUINT_TO_POINTER(object->id));
  }

  cairo_surface_t *surface = dt_canvas_render_decode(object->image.jpeg, cache->for_display);
  if(IS_NULL_PTR(surface)) return NULL;
  entry = g_new0(dt_canvas_cached_surface_t, 1);
  entry->object_id = object->id;
  entry->jpeg = g_bytes_ref(object->image.jpeg);
  entry->profile_generation = generation;
  entry->surface = surface;
  entry->bytes = (size_t)cairo_image_surface_get_stride(surface) * cairo_image_surface_get_height(surface);
  entry->last_use = cache->clock;
  g_hash_table_insert(cache->entries, GUINT_TO_POINTER(object->id), entry);
  cache->used += entry->bytes;
  _cache_evict_to_budget(cache, object->id);
  return surface;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
