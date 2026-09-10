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

#include "develop/masks_cutout.h"

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

#include <curl/curl.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <lcms2.h>
#ifdef HAVE_MAP
#include <osm-gps-map.h> // conditional-ok: the provider table below is inside the same HAVE_MAP block
#endif
#include "common/file_location.h"
#include "common/logging.h"
#include "config.h"
#include <math.h>
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

static uint8_t *_profile_bytes(const dt_colorspaces_color_profile_type_t type, uint32_t *length)
{
  *length = 0;
  const dt_colorspaces_color_profile_t *entry = dt_colorspaces_get_profile(type, "", DT_PROFILE_ROLE_OUTPUT);
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

static GBytes *_encode_jpeg(const uint8_t *rgba, const int width, const int height, const int quality,
                            const dt_colorspaces_color_profile_type_t profile_type)
{
  dt_canvas_jpeg_error_t error;
  struct jpeg_compress_struct cinfo;
  unsigned char *out_buffer = NULL;
  unsigned long out_size = 0;
  JSAMPROW row = NULL;
  uint32_t icc_length = 0;
  uint8_t *icc = _profile_bytes(profile_type, &icc_length);

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
                                                  DT_COLORSPACE_ADOBERGB, "", DT_INTENT_PERCEPTUAL, NULL, NULL, 1, 1,
                                                  NULL, &_cancel_requested);
  if(status == 0 && !IS_NULL_PTR(data.pixels) && data.head.width > 0 && data.head.height > 0)
  {
    // Adobe RGB, the canvas's own encoding: wider than sRGB and what a print can use; the
    // profile travels in the JPEG so the file stands on its own.
    params->jpeg = _encode_jpeg(data.pixels, data.head.width, data.head.height, params->quality, DT_COLORSPACE_ADOBERGB);
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

/* --- maps: providers ---------------------------------------------------------- */

#define MAP_TILE_SIZE 256
#define MAP_MAX_TILES 64
#define MAP_DEFAULT_URI "https://tile.openstreetmap.org/#Z/#X/#Y.png"

typedef struct dt_canvas_map_provider_t
{
  uint32_t id;
  const char *name;
  const char *uri;   ///< with #X, #Y, #Z (and #R for a random server) placeholders
  const char *attribution;
  int max_zoom;
} dt_canvas_map_provider_t;

/** The providers: OpenStreetMap always, and the map view's when it is built. */
static int _map_providers(dt_canvas_map_provider_t *providers, const int capacity)
{
  int count = 0;
#ifdef HAVE_MAP
  for(int source = 1; source < OSM_GPS_MAP_SOURCE_LAST && count < capacity; source++)
  {
    if(!osm_gps_map_source_is_valid(source)) continue;
    const char *uri = osm_gps_map_source_get_repo_uri(source);
    if(IS_NULL_PTR(uri) || IS_NULL_PTR(strstr(uri, "#X"))) continue;
    providers[count].id = (uint32_t)source;
    providers[count].name = osm_gps_map_source_get_friendly_name(source);
    providers[count].uri = uri;
    providers[count].attribution = strstr(uri, "openstreetmap") != NULL ? "© OpenStreetMap contributors"
                                                                        : osm_gps_map_source_get_friendly_name(source);
    providers[count].max_zoom = osm_gps_map_source_get_max_zoom(source);
    count++;
  }
#endif
  if(count == 0 && capacity > 0)
  {
    providers[0].id = 0;
    providers[0].name = "OpenStreetMap";
    providers[0].uri = MAP_DEFAULT_URI;
    providers[0].attribution = "© OpenStreetMap contributors";
    providers[0].max_zoom = 19;
    count = 1;
  }
  return count;
}

int dt_canvas_map_source_count(void)
{
  dt_canvas_map_provider_t providers[64];
  return _map_providers(providers, 64);
}

uint32_t dt_canvas_map_source_id(int index)
{
  dt_canvas_map_provider_t providers[64];
  const int count = _map_providers(providers, 64);
  return (index >= 0 && index < count) ? providers[index].id : providers[0].id;
}

const char *dt_canvas_map_source_name(int index)
{
  dt_canvas_map_provider_t providers[64];
  const int count = _map_providers(providers, 64);
  return (index >= 0 && index < count) ? providers[index].name : providers[0].name;
}

int dt_canvas_map_source_index(uint32_t source)
{
  dt_canvas_map_provider_t providers[64];
  const int count = _map_providers(providers, 64);
  for(int idx = 0; idx < count; idx++)
  {
    if(providers[idx].id == source) return idx;
  }
  return 0;
}

const char *dt_canvas_map_source_attribution(uint32_t source)
{
  dt_canvas_map_provider_t providers[64];
  const int count = _map_providers(providers, 64);
  for(int idx = 0; idx < count; idx++)
  {
    if(providers[idx].id == source) return providers[idx].attribution;
  }
  return providers[0].attribution;
}

/* --- maps: tiles ----------------------------------------------------------------- */

static size_t _curl_to_string(char *data, size_t size, size_t count, void *user)
{
  GString *body = (GString *)user;
  const size_t bytes = size * count;
  if(body->len + bytes > 8 * 1024 * 1024) return 0;
  g_string_append_len(body, data, bytes);
  return bytes;
}

/** Replace every `placeholder` in `text` by `value`. (GLib's own arrives in 2.68; the tree pins 2.64.) */
static gchar *_replace_all(const gchar *text, const gchar *placeholder, const gchar *value)
{
  gchar **parts = g_strsplit(text, placeholder, -1);
  gchar *joined = g_strjoinv(value, parts);
  g_strfreev(parts);
  return joined;
}

/** A tile's URL from the provider's template. */
static gchar *_tile_url(const dt_canvas_map_provider_t *provider, const int zoom, const int x, const int y)
{
  const struct
  {
    const char *placeholder;
    int value;
  } fields[] = { { "#Z", zoom }, { "#X", x }, { "#Y", y }, { "#R", (x + y) % 4 }, { "#S", 17 - zoom } };
  gchar *url = g_strdup(provider->uri);
  for(size_t idx = 0; idx < G_N_ELEMENTS(fields); idx++)
  {
    gchar *value = g_strdup_printf("%d", fields[idx].value);
    gchar *next = _replace_all(url, fields[idx].placeholder, value);
    dt_free(value);
    dt_free(url);
    url = next;
  }
  return url;
}

/** One HTTP GET into `body`. TRUE on a 200 with content. */
static gboolean _http_get(const char *url, GString *body)
{
  char agent[128];
  snprintf(agent, sizeof(agent), "Ansel/%s (canvas)", darktable_package_version);
  CURL *curl = curl_easy_init();
  if(IS_NULL_PTR(curl)) return FALSE;
  g_string_truncate(body, 0);
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, agent);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _curl_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
  curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
  const CURLcode result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  const gboolean ok = result == CURLE_OK && status == 200 && body->len > 0;
  if(!ok)
    dt_print(DT_DEBUG_CONTROL, "[canvas] tile %s failed: %s (HTTP %ld)\n", url,
             result == CURLE_OK ? "unexpected status" : curl_easy_strerror(result), status);
  return ok;
}

/**
 * Fetch one tile, from the disk cache when it is there, else over HTTP and into the cache.
 * The providers' templates are old and spell plain http; the servers have since moved to
 * TLS and several refuse plain connections outright, so https is tried first.
 */
static GdkPixbuf *_tile_fetch(const dt_canvas_map_provider_t *provider, const int zoom, const int x, const int y)
{
  char cache_dir[DT_PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cache_dir, sizeof(cache_dir));
  gchar *directory = g_strdup_printf("%s/canvas-maps/%u/%d/%d", cache_dir, provider->id, zoom, x);
  gchar *path = g_strdup_printf("%s/%d.tile", directory, y);
  GdkPixbuf *pixbuf = NULL;
  if(g_file_test(path, G_FILE_TEST_IS_REGULAR)) pixbuf = gdk_pixbuf_new_from_file(path, NULL);
  if(IS_NULL_PTR(pixbuf))
  {
    gchar *url = _tile_url(provider, zoom, x, y);
    GString *body = g_string_sized_new(64 * 1024);
    gboolean fetched = FALSE;
    if(g_str_has_prefix(url, "http://"))
    {
      gchar *secure = g_strconcat("https://", url + strlen("http://"), NULL);
      fetched = _http_get(secure, body);
      dt_free(secure);
    }
    if(!fetched) fetched = _http_get(url, body);
    if(fetched)
    {
      GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
      if(gdk_pixbuf_loader_write(loader, (const guchar *)body->str, body->len, NULL)
         && gdk_pixbuf_loader_close(loader, NULL))
      {
        pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
        if(!IS_NULL_PTR(pixbuf)) g_object_ref(pixbuf);
      }
      g_object_unref(loader);
      if(!IS_NULL_PTR(pixbuf))
      {
        g_mkdir_with_parents(directory, 0755);
        g_file_set_contents(path, body->str, body->len, NULL);
      }
    }
    g_string_free(body, TRUE);
    dt_free(url);
  }
  dt_free(path);
  dt_free(directory);
  return pixbuf;
}

typedef struct dt_canvas_map_job_t
{
  uint32_t object_id;
  uint64_t token;
  double latitude;
  double longitude;
  int32_t zoom;
  uint32_t source;
  int32_t pixel_width;
  int32_t pixel_height;
  int32_t quality;
  dt_canvas_render_done_t done;
  gpointer user_data;
} dt_canvas_map_job_t;

static int32_t _map_job_run(dt_job_t *job)
{
  dt_canvas_map_job_t *params = dt_control_job_get_params(job);
  if(IS_NULL_PTR(params)) return 1;
  dt_canvas_render_job_t *result = g_new0(dt_canvas_render_job_t, 1);
  result->object_id = params->object_id;
  result->token = params->token;
  result->done = params->done;
  result->user_data = params->user_data;

  dt_canvas_map_provider_t providers[64];
  const int count = _map_providers(providers, 64);
  const dt_canvas_map_provider_t *provider = &providers[0];
  for(int idx = 0; idx < count; idx++)
  {
    if(providers[idx].id == params->source) provider = &providers[idx];
  }
  const int zoom = CLAMP(params->zoom, 1, provider->max_zoom > 0 ? provider->max_zoom : 19);
  dt_control_job_set_progress_message(job, _("fetching map tiles for the canvas"));

  // Web Mercator: the centre in pixels of the world at this zoom.
  const double scale = (double)(1 << zoom) * MAP_TILE_SIZE;
  const double latitude = CLAMP(params->latitude, -85.0511, 85.0511) * M_PI / 180.0;
  const double center_x = (params->longitude + 180.0) / 360.0 * scale;
  const double center_y = (1.0 - log(tan(latitude) + 1.0 / cos(latitude)) / M_PI) / 2.0 * scale;
  const int width = CLAMP(params->pixel_width, 64, 4096);
  const int height = CLAMP(params->pixel_height, 64, 4096);
  const double left = center_x - width * 0.5;
  const double top = center_y - height * 0.5;
  const int first_tile_x = (int)floor(left / MAP_TILE_SIZE);
  const int first_tile_y = (int)floor(top / MAP_TILE_SIZE);
  const int last_tile_x = (int)floor((left + width) / MAP_TILE_SIZE);
  const int last_tile_y = (int)floor((top + height) / MAP_TILE_SIZE);
  const int tiles = (last_tile_x - first_tile_x + 1) * (last_tile_y - first_tile_y + 1);

  gboolean ok = tiles <= MAP_MAX_TILES && !dt_atomic_get_int(&_cancel_requested);
  cairo_surface_t *surface = ok ? cairo_image_surface_create(CAIRO_FORMAT_RGB24, width, height) : NULL;
  if(!IS_NULL_PTR(surface) && cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS)
  {
    cairo_t *cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    cairo_paint(cr);
    const int tile_count_max = 1 << zoom;
    int fetched = 0;
    for(int tile_y = first_tile_y; tile_y <= last_tile_y && ok; tile_y++)
    {
      for(int tile_x = first_tile_x; tile_x <= last_tile_x && ok; tile_x++)
      {
        if(dt_atomic_get_int(&_cancel_requested) || dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED)
        {
          ok = FALSE;
          break;
        }
        if(tile_y < 0 || tile_y >= tile_count_max) continue;
        const int wrapped_x = ((tile_x % tile_count_max) + tile_count_max) % tile_count_max;
        GdkPixbuf *pixbuf = _tile_fetch(provider, zoom, wrapped_x, tile_y);
        if(IS_NULL_PTR(pixbuf)) continue;
        cairo_save(cr);
        cairo_translate(cr, tile_x * MAP_TILE_SIZE - left, tile_y * MAP_TILE_SIZE - top);
        const double tile_scale = (double)MAP_TILE_SIZE / gdk_pixbuf_get_width(pixbuf);
        cairo_scale(cr, tile_scale, tile_scale);
        gdk_cairo_set_source_pixbuf(cr, pixbuf, 0.0, 0.0);
        cairo_paint(cr);
        cairo_restore(cr);
        g_object_unref(pixbuf);
        fetched++;
        dt_control_job_set_progress(job, (double)fetched / tiles);
      }
    }
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    if(ok && fetched > 0)
    {
      // BGRx to RGBA for the encoder.
      const uint8_t *bgra = cairo_image_surface_get_data(surface);
      const int stride = cairo_image_surface_get_stride(surface);
      uint8_t *rgba = g_malloc((size_t)width * height * 4);
      for(int y = 0; y < height; y++)
      {
        for(int x = 0; x < width; x++)
        {
          const uint8_t *pixel = bgra + (size_t)y * stride + (size_t)x * 4;
          uint8_t *out = rgba + ((size_t)y * width + x) * 4;
          out[0] = pixel[2];
          out[1] = pixel[1];
          out[2] = pixel[0];
          out[3] = 255;
        }
      }
      // Tiles are sRGB and stay so: they are converted when decoded, like any older render.
      result->jpeg = _encode_jpeg(rgba, width, height, params->quality, DT_COLORSPACE_SRGB);
      result->pixel_width = width;
      result->pixel_height = height;
      dt_free(rgba);
    }
  }
  if(!IS_NULL_PTR(surface)) cairo_surface_destroy(surface);
  dt_control_job_set_progress(job, 1.0);
  g_main_context_invoke(NULL, _render_deliver, result);
  dt_free(params);
  return 0;
}

gboolean dt_canvas_render_map_start(uint32_t object_id, uint64_t token, double latitude, double longitude,
                                    int32_t zoom, uint32_t source, int32_t pixel_width, int32_t pixel_height,
                                    int32_t quality, dt_canvas_render_done_t done, gpointer user_data)
{
  if(IS_NULL_PTR(done)) return FALSE;
  dt_job_t *job = dt_control_job_create(&_map_job_run, "canvas map %u", object_id);
  if(IS_NULL_PTR(job)) return FALSE;
  dt_canvas_map_job_t *params = g_new0(dt_canvas_map_job_t, 1);
  params->object_id = object_id;
  params->token = token;
  params->latitude = latitude;
  params->longitude = longitude;
  params->zoom = zoom;
  params->source = source;
  params->pixel_width = pixel_width;
  params->pixel_height = pixel_height;
  params->quality = quality > 0 ? quality : 92;
  params->done = done;
  params->user_data = user_data;
  dt_control_job_set_params(job, params, NULL);
  dt_atomic_set_int(&_cancel_requested, 0);
  dt_control_job_add_progress(job, _("canvas map"), TRUE);
  return dt_control_add_job(dt_control_get_global(), DT_JOB_QUEUE_USER_BG, job) == 0;
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

/* sRGB to the layer encoding, Adobe RGB (1998): the sRGB curve out, the standard matrix
 * (both spaces share the D65 white), the 563/256 gamma in. The decode table is per code; the
 * encode table is indexed by the square root of the value, dense at the dark end where the
 * gamma curve is steepest, so every code of a neutral round trip lands where it started. */
#define LAYER_GAMMA (563.0f / 256.0f)
#define LAYER_OETF_STEPS 16383
static float _srgb_eotf_lut[256];
static uint8_t _layer_oetf_lut[LAYER_OETF_STEPS + 1];
static gsize _layer_luts_ready = 0;

static void _layer_luts_init(void)
{
  if(g_once_init_enter(&_layer_luts_ready))
  {
    for(int idx = 0; idx < 256; idx++)
    {
      const float value = (float)idx / 255.0f;
      _srgb_eotf_lut[idx] = value <= 0.04045f ? value / 12.92f : powf((value + 0.055f) / 1.055f, 2.4f);
    }
    for(int idx = 0; idx <= LAYER_OETF_STEPS; idx++)
    {
      const float root = (float)idx / (float)LAYER_OETF_STEPS;
      _layer_oetf_lut[idx] = (uint8_t)lrintf(powf(root * root, 1.0f / LAYER_GAMMA) * 255.0f);
    }
    g_once_init_leave(&_layer_luts_ready, 1);
  }
}

static inline uint8_t _layer_encode(const float linear)
{
  return _layer_oetf_lut[(int)lrintf(sqrtf(CLAMP(linear, 0.0f, 1.0f)) * (float)LAYER_OETF_STEPS)];
}

void dt_canvas_render_srgb8_to_layer8(uint8_t *pixels, const size_t count, const int channels)
{
  if(IS_NULL_PTR(pixels) || channels < 3) return;
  _layer_luts_init();
  for(size_t idx = 0; idx < count; idx++)
  {
    uint8_t *pixel = pixels + idx * channels;
    const float red = _srgb_eotf_lut[pixel[0]];
    const float green = _srgb_eotf_lut[pixel[1]];
    const float blue = _srgb_eotf_lut[pixel[2]];
    pixel[0] = _layer_encode(0.715166f * red + 0.284837f * green);
    pixel[1] = _layer_encode(green);
    pixel[2] = _layer_encode(0.041171f * green + 0.958829f * blue);
  }
}

cairo_surface_t *dt_canvas_render_decode(GBytes *jpeg, const uint32_t colorspace)
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
  // Every surface is in the layer encoding: the compositor manages colour once, on the finished
  // canvas, so what blends is one space and not one per input.
  if(colorspace != DT_CANVAS_COLORSPACE_ADOBERGB) dt_canvas_render_srgb8_to_layer8(rgba, pixels, 4);
  _rgba_to_bgra(rgba, bgra, pixels);
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
  // The colour as cairo must paint it: in the layer encoding, whatever the target. Doubles, not
  // codes: cairo quantises for itself, and the encode goes through the same curve as the tables.
  (void)for_display;
  const float srgb[3] = { CLAMP(color->red, 0.0f, 1.0f), CLAMP(color->green, 0.0f, 1.0f), CLAMP(color->blue, 0.0f, 1.0f) };
  float linear[3];
  for(int channel = 0; channel < 3; channel++)
    linear[channel] = srgb[channel] <= 0.04045f ? srgb[channel] / 12.92f : powf((srgb[channel] + 0.055f) / 1.055f, 2.4f);
  const float working[3] = { 0.715166f * linear[0] + 0.284837f * linear[1], linear[1],
                             0.041171f * linear[1] + 0.958829f * linear[2] };
  for(int channel = 0; channel < 3; channel++) out[channel] = pow(CLAMP(working[channel], 0.0f, 1.0f), 1.0 / LAYER_GAMMA);
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

typedef struct dt_canvas_cached_mask_t
{
  uint64_t hash;
  int width;   ///< the frame's raster size, without the margin
  int height;
  int margin;
  cairo_surface_t *surface;
  int band_radius;        ///< the border band's dilation, pixels; 0 when there is none
  cairo_surface_t *band;
  cairo_surface_t *support; ///< where the cutout has any coverage at all, hard-edged
} dt_canvas_cached_mask_t;

static void _cached_mask_free(gpointer data)
{
  dt_canvas_cached_mask_t *entry = (dt_canvas_cached_mask_t *)data;
  if(IS_NULL_PTR(entry)) return;
  if(!IS_NULL_PTR(entry->surface)) cairo_surface_destroy(entry->surface);
  if(!IS_NULL_PTR(entry->band)) cairo_surface_destroy(entry->band);
  if(!IS_NULL_PTR(entry->support)) cairo_surface_destroy(entry->support);
  dt_free(entry);
}

struct dt_canvas_surface_cache_t
{
  gboolean for_display;
  size_t budget;
  size_t used;
  uint64_t clock;
  GHashTable *entries; ///< object id -> dt_canvas_cached_surface_t
  GHashTable *masks;   ///< object id -> dt_canvas_cached_mask_t
  void *scratch;
  size_t scratch_bytes;
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
  cache->masks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, _cached_mask_free);
  return cache;
}

void dt_canvas_surface_cache_free(dt_canvas_surface_cache_t *cache)
{
  if(IS_NULL_PTR(cache)) return;
  g_hash_table_destroy(cache->entries);
  g_hash_table_destroy(cache->masks);
  dt_free_align(cache->scratch);
  dt_free(cache);
}

void dt_canvas_surface_cache_clear(dt_canvas_surface_cache_t *cache)
{
  if(IS_NULL_PTR(cache)) return;
  g_hash_table_remove_all(cache->entries);
  g_hash_table_remove_all(cache->masks);
  cache->used = 0;
}

void *dt_canvas_surface_cache_scratch(dt_canvas_surface_cache_t *cache, const size_t bytes)
{
  if(IS_NULL_PTR(cache)) return NULL;
  if(cache->scratch_bytes < bytes || IS_NULL_PTR(cache->scratch))
  {
    dt_free_align(cache->scratch);
    // Grow in steps: a viewport resized by a pixel must not reallocate every frame.
    const size_t granted = bytes + bytes / 4;
    cache->scratch = dt_alloc_align(granted);
    cache->scratch_bytes = IS_NULL_PTR(cache->scratch) ? 0 : granted;
  }
  return cache->scratch;
}

/**
 * The cutout as floats, through the masks module, over the frame grown by `margin` pixels on
 * every side -- a shape may reach past the frame, and its border and feather do -- and at
 * `factor` times the resolution, for the anti-aliasing the box filter below makes of it. The
 * shape is described in the frame's unit square; here it is re-described in the grown
 * raster's: positions shifted and scaled, radii scaled by the ratio of the shorter sides.
 */
static float *_mask_raster_fine(const dt_canvas_object_t *object, const int width, const int height, const int margin,
                                const int factor, int *fine_width, int *fine_height)
{
  if(IS_NULL_PTR(object) || object->mask.shape == DT_CANVAS_MASK_NONE || width <= 0 || height <= 0) return NULL;
  const double grown_width = width + 2.0 * margin;
  const double grown_height = height + 2.0 * margin;
  const double radius_scale = (double)MIN(width, height) / MIN(grown_width, grown_height);
  dt_masks_cutout_t cutout;
  memset(&cutout, 0, sizeof(cutout));
  cutout.shape = (dt_masks_cutout_shape_t)object->mask.shape;
  cutout.center[0] = (float)((object->mask.center_x * width + margin) / grown_width);
  cutout.center[1] = (float)((object->mask.center_y * height + margin) / grown_height);
  cutout.radius[0] = (float)(object->mask.radius_x * radius_scale);
  // The ellipse's second radius is a length; the gradient's "curvature" is not.
  cutout.radius[1] = object->mask.shape == DT_CANVAS_MASK_GRADIENT ? object->mask.radius_y
                                                                    : (float)(object->mask.radius_y * radius_scale);
  cutout.rotation = object->mask.rotation;
  cutout.feather = (float)(object->mask.feather * radius_scale);
  cutout.invert = (object->mask.flags & DT_CANVAS_MASK_INVERT) != 0;
  float *nodes = NULL;
  if(object->mask.shape == DT_CANVAS_MASK_POLYGON && object->mask.node_count > 0 && !IS_NULL_PTR(object->mask.nodes))
  {
    const size_t floats = (size_t)object->mask.node_count * DT_CANVAS_MASK_NODE_FLOATS;
    nodes = g_new(float, floats);
    memcpy(nodes, object->mask.nodes, floats * sizeof(float));
    for(uint32_t idx = 0; idx < object->mask.node_count; idx++)
    {
      float *node = nodes + (size_t)idx * DT_CANVAS_MASK_NODE_FLOATS;
      for(int point = 0; point < 3; point++)
      {
        node[2 * point] = (float)((node[2 * point] * width + margin) / grown_width);
        node[2 * point + 1] = (float)((node[2 * point + 1] * height + margin) / grown_height);
      }
    }
    cutout.node_count = object->mask.node_count;
    cutout.node_stride = DT_CANVAS_MASK_NODE_FLOATS;
    cutout.nodes = nodes;
  }
  *fine_width = (int)lround(grown_width) * factor;
  *fine_height = (int)lround(grown_height) * factor;
  float *raster = dt_masks_cutout_rasterise(&cutout, *fine_width, *fine_height);
  dt_free(nodes);
  return raster;
}

/** How many samples per pixel a raster of this size gets: three where it is cheap, two otherwise. */
static int _mask_factor(const int width, const int height, const int margin)
{
  const double pixels = (double)(width + 2 * margin) * (height + 2 * margin);
  return pixels <= 1024.0 * 1024.0 ? 3 : 2;
}

/** Box-filter a fine raster down by `factor`: the anti-aliasing of every mask surface. */
static float *_mask_downsample(const float *fine, const int fine_width, const int fine_height, const int factor)
{
  const int width = fine_width / factor;
  const int height = fine_height / factor;
  float *coarse = dt_alloc_align_float((size_t)width * height);
  if(IS_NULL_PTR(coarse)) return NULL;
  const float norm = 1.0f / (float)(factor * factor);
  for(int row = 0; row < height; row++)
  {
    for(int col = 0; col < width; col++)
    {
      float sum = 0.0f;
      for(int sub_row = 0; sub_row < factor; sub_row++)
      {
        const float *source = fine + (size_t)(row * factor + sub_row) * fine_width + (size_t)col * factor;
        for(int sub_col = 0; sub_col < factor; sub_col++) sum += source[sub_col];
      }
      coarse[(size_t)row * width + col] = sum * norm;
    }
  }
  return coarse;
}

/** Floats in [0, 1] to an 8-bit alpha surface. */
static cairo_surface_t *_alpha_surface(const float *raster, const int width, const int height)
{
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_A8, width, height);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    cairo_surface_destroy(surface);
    return NULL;
  }
  cairo_surface_flush(surface);
  uint8_t *pixels = cairo_image_surface_get_data(surface);
  const int stride = cairo_image_surface_get_stride(surface);
  for(int row = 0; row < height; row++)
  {
    const float *source = raster + (size_t)row * width;
    uint8_t *target = pixels + (size_t)row * stride;
    for(int col = 0; col < width; col++) target[col] = (uint8_t)lrintf(CLAMP(source[col], 0.0f, 1.0f) * 255.0f);
  }
  cairo_surface_mark_dirty(surface);
  return surface;
}

/** Any coverage at all: the feather's outer edge is where the shape's support ends. */
#define CANVAS_MASK_SUPPORT_THRESHOLD 0.002f

/** A fine raster, thresholded in place to the shape's support. */
static void _mask_threshold(float *fine, const size_t count)
{
  for(size_t idx = 0; idx < count; idx++) fine[idx] = fine[idx] > CANVAS_MASK_SUPPORT_THRESHOLD ? 1.0f : 0.0f;
}

/** The fine raster to an anti-aliased alpha surface at the frame's resolution. */
static cairo_surface_t *_mask_finish(float *fine, const int fine_width, const int fine_height, const int factor)
{
  float *coarse = _mask_downsample(fine, fine_width, fine_height, factor);
  dt_masks_cutout_free(fine);
  if(IS_NULL_PTR(coarse)) return NULL;
  cairo_surface_t *surface = _alpha_surface(coarse, fine_width / factor, fine_height / factor);
  dt_free_align(coarse);
  return surface;
}

cairo_surface_t *dt_canvas_render_mask(const dt_canvas_object_t *object, const int width, const int height,
                                       const int margin)
{
  const int factor = _mask_factor(width, height, margin);
  int fine_width = 0;
  int fine_height = 0;
  float *fine = _mask_raster_fine(object, width, height, margin, factor, &fine_width, &fine_height);
  if(IS_NULL_PTR(fine)) return NULL;
  return _mask_finish(fine, fine_width, fine_height, factor);
}

cairo_surface_t *dt_canvas_render_mask_support(const dt_canvas_object_t *object, const int width, const int height,
                                               const int margin)
{
  const int factor = _mask_factor(width, height, margin);
  int fine_width = 0;
  int fine_height = 0;
  float *fine = _mask_raster_fine(object, width, height, margin, factor, &fine_width, &fine_height);
  if(IS_NULL_PTR(fine)) return NULL;
  _mask_threshold(fine, (size_t)fine_width * fine_height);
  return _mask_finish(fine, fine_width, fine_height, factor);
}

/* One dimension of Felzenszwalb and Huttenlocher's squared Euclidean distance transform:
 * `f` in, `out` the lower envelope of the parabolas, `vertices`/`boundaries` scratch of n and n+1. */
static void _distance_1d(const float *f, float *out, int *vertices, float *boundaries, const int n)
{
  int k = 0;
  vertices[0] = 0;
  boundaries[0] = -INFINITY;
  boundaries[1] = INFINITY;
  for(int q = 1; q < n; q++)
  {
    float s = 0.0f;
    while(TRUE)
    {
      const int v = vertices[k];
      s = ((f[q] + (float)q * q) - (f[v] + (float)v * v)) / (2.0f * (q - v));
      if(s <= boundaries[k] && k > 0)
        k--;
      else
        break;
    }
    k++;
    vertices[k] = q;
    boundaries[k] = s;
    boundaries[k + 1] = INFINITY;
  }
  k = 0;
  for(int q = 0; q < n; q++)
  {
    while(boundaries[k + 1] < (float)q) k++;
    const float d = (float)q - (float)vertices[k];
    out[q] = d * d + f[vertices[k]];
  }
}

/**
 * The border band of a cutout: the shape's support pushed out by `radius` pixels on every
 * side -- a disc dilation, through the Euclidean distance to the support, at the fine
 * resolution -- and nothing inside the support, so the band is solid and starts where the
 * feather ends. NULL when there is no shape.
 */
cairo_surface_t *dt_canvas_render_mask_band(const dt_canvas_object_t *object, const int width, const int height,
                                            const int margin, const int radius)
{
  const int factor = _mask_factor(width, height, margin);
  int fine_width = 0;
  int fine_height = 0;
  float *fine = _mask_raster_fine(object, width, height, margin, factor, &fine_width, &fine_height);
  if(IS_NULL_PTR(fine)) return NULL;
  const size_t count = (size_t)fine_width * fine_height;
  float *distance = dt_alloc_align_float(count);
  const int longest = MAX(fine_width, fine_height);
  float *line = dt_alloc_align_float((size_t)longest);
  float *line_out = dt_alloc_align_float((size_t)longest);
  int *vertices = g_new(int, longest);
  float *boundaries = g_new(float, longest + 1);
  cairo_surface_t *surface = NULL;
  if(!IS_NULL_PTR(distance) && !IS_NULL_PTR(line) && !IS_NULL_PTR(line_out))
  {
    // Squared distance to the nearest pixel of the support: 0 inside, "infinite" outside, then the two passes.
    const float unreached = (float)longest * longest * 4.0f; // not `far`: a Windows macro
    for(size_t idx = 0; idx < count; idx++)
      distance[idx] = fine[idx] > CANVAS_MASK_SUPPORT_THRESHOLD ? 0.0f : unreached;
    for(int row = 0; row < fine_height; row++)
    {
      _distance_1d(distance + (size_t)row * fine_width, line_out, vertices, boundaries, fine_width);
      memcpy(distance + (size_t)row * fine_width, line_out, (size_t)fine_width * sizeof(float));
    }
    for(int col = 0; col < fine_width; col++)
    {
      for(int row = 0; row < fine_height; row++) line[row] = distance[(size_t)row * fine_width + col];
      _distance_1d(line, line_out, vertices, boundaries, fine_height);
      for(int row = 0; row < fine_height; row++) distance[(size_t)row * fine_width + col] = line_out[row];
    }
    const float fine_radius = (float)radius * factor;
    for(size_t idx = 0; idx < count; idx++)
    {
      const float reach = CLAMP(fine_radius + 0.5f - sqrtf(distance[idx]), 0.0f, 1.0f);
      fine[idx] = fine[idx] > CANVAS_MASK_SUPPORT_THRESHOLD ? 0.0f : reach;
    }
    surface = _mask_finish(fine, fine_width, fine_height, factor);
    fine = NULL;
  }
  dt_free(vertices);
  dt_free(boundaries);
  dt_free_align(line_out);
  dt_free_align(line);
  dt_free_align(distance);
  dt_masks_cutout_free(fine);
  return surface;
}

cairo_surface_t *dt_canvas_surface_cache_get_mask(dt_canvas_surface_cache_t *cache, const dt_canvas_object_t *object,
                                                  const int width, const int height, const int margin)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(object) || object->mask.shape == DT_CANVAS_MASK_NONE) return NULL;
  const uint64_t hash = dt_canvas_mask_hash(&object->mask);
  dt_canvas_cached_mask_t *entry = g_hash_table_lookup(cache->masks, GUINT_TO_POINTER(object->id));
  if(!IS_NULL_PTR(entry) && entry->hash == hash && entry->width == width && entry->height == height
     && entry->margin == margin)
    return entry->surface;
  cairo_surface_t *surface = dt_canvas_render_mask(object, width, height, margin);
  if(IS_NULL_PTR(surface))
  {
    g_hash_table_remove(cache->masks, GUINT_TO_POINTER(object->id));
    return NULL;
  }
  entry = g_new0(dt_canvas_cached_mask_t, 1);
  entry->hash = hash;
  entry->width = width;
  entry->height = height;
  entry->margin = margin;
  entry->surface = surface;
  g_hash_table_insert(cache->masks, GUINT_TO_POINTER(object->id), entry);
  return surface;
}

cairo_surface_t *dt_canvas_surface_cache_get_mask_support(dt_canvas_surface_cache_t *cache,
                                                          const dt_canvas_object_t *object, const int width,
                                                          const int height, const int margin)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(object) || object->mask.shape == DT_CANVAS_MASK_NONE) return NULL;
  if(IS_NULL_PTR(dt_canvas_surface_cache_get_mask(cache, object, width, height, margin))) return NULL;
  dt_canvas_cached_mask_t *entry = g_hash_table_lookup(cache->masks, GUINT_TO_POINTER(object->id));
  if(IS_NULL_PTR(entry)) return NULL;
  if(IS_NULL_PTR(entry->support)) entry->support = dt_canvas_render_mask_support(object, width, height, margin);
  return entry->support;
}

cairo_surface_t *dt_canvas_surface_cache_get_mask_band(dt_canvas_surface_cache_t *cache,
                                                       const dt_canvas_object_t *object, const int width,
                                                       const int height, const int margin, const int radius)
{
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(object) || object->mask.shape == DT_CANVAS_MASK_NONE || radius <= 0) return NULL;
  // The mask entry is the band's home: getting it first settles the hash and the size.
  if(IS_NULL_PTR(dt_canvas_surface_cache_get_mask(cache, object, width, height, margin))) return NULL;
  dt_canvas_cached_mask_t *entry = g_hash_table_lookup(cache->masks, GUINT_TO_POINTER(object->id));
  if(IS_NULL_PTR(entry)) return NULL;
  if(!IS_NULL_PTR(entry->band) && entry->band_radius == radius) return entry->band;
  if(!IS_NULL_PTR(entry->band)) cairo_surface_destroy(entry->band);
  entry->band = dt_canvas_render_mask_band(object, width, height, margin, radius);
  entry->band_radius = IS_NULL_PTR(entry->band) ? 0 : radius;
  return entry->band;
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
  if(IS_NULL_PTR(cache) || IS_NULL_PTR(object)) return NULL;
  GBytes *jpeg = dt_canvas_object_raster(object);
  if(IS_NULL_PTR(jpeg)) return NULL;
  const uint64_t generation = cache->for_display ? _display_generation() : 0;
  cache->clock++;

  dt_canvas_cached_surface_t *entry = g_hash_table_lookup(cache->entries, GUINT_TO_POINTER(object->id));
  if(!IS_NULL_PTR(entry))
  {
    if(entry->jpeg == jpeg && entry->profile_generation == generation)
    {
      entry->last_use = cache->clock;
      return entry->surface;
    }
    cache->used -= entry->bytes;
    g_hash_table_remove(cache->entries, GUINT_TO_POINTER(object->id));
  }

  const uint32_t colorspace = object->kind == DT_CANVAS_OBJECT_IMAGE ? object->image.colorspace : DT_CANVAS_COLORSPACE_SRGB;
  cairo_surface_t *surface = dt_canvas_render_decode(jpeg, colorspace);
  if(IS_NULL_PTR(surface)) return NULL;
  entry = g_new0(dt_canvas_cached_surface_t, 1);
  entry->object_id = object->id;
  entry->jpeg = g_bytes_ref(jpeg);
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
