/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Guillaume Stutin.

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

/**
 * @file flatfield.c
 * @brief Flat-field correction from a reference raw chosen by the user.
 *
 * The reference is a raw of an evenly lit, featureless field -- a light table with no film on it, a
 * diffuser over the lens -- shot with the same camera, lens and aperture as the images it corrects.
 * Whatever is uneven in it (vignetting, a light source brighter in its centre, a colour drift
 * across the field, dust) is what the optics and the light did, and dividing by it undoes that.
 *
 * The module works on the sensor mosaic, between rawprepare and everything else, so the division
 * happens per sensel and per CFA colour before any of them are mixed. The full design, its
 * assumptions and its limits are in doc/flatfield.md.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "caches/pixelpipe_cache.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "common/hash.h"
#include "common/image.h"
#include "common/imagebuf.h"
#include "common/module_versioning.h"
#include "common/opencl.h"
#include "control/user_message.h"
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/application.h"
#include "imageio/imageio_core.h"
#include "pixel/box_filters.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "system/openmp.h"
#include "widgets/bauhaus.h"
#include "widgets/button.h"
#include "widgets/label.h"
#include "widgets/paint.h"

#include "iop/iop_api.h"

#include <float.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_flatfield_params_t)

#define DT_IOP_FLATFIELD_MAX_PATH 1024

typedef struct dt_iop_flatfield_params_t
{
  char flat[DT_IOP_FLATFIELD_MAX_PATH]; // the reference raw, absolute path; empty disables the module
  char dark[DT_IOP_FLATFIELD_MAX_PATH]; // an optional raw shot without light, absolute path
  float radius;   // $MIN: 0.0 $MAX: 512.0 $DEFAULT: 32.0 $DESCRIPTION: "smoothing"
  gboolean color; // $DEFAULT: TRUE $DESCRIPTION: "correct color"
  float strength; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 1.0 $DESCRIPTION: "strength"
} dt_iop_flatfield_params_t;

/**
 * Everything the gain map is a function of, and nothing else: the strength is applied per pixel,
 * so moving its slider reuses the map. Hashed as one block of bytes, so it is always zeroed before
 * being filled.
 */
typedef struct dt_iop_flatfield_source_t
{
  char flat[DT_IOP_FLATFIELD_MAX_PATH];
  char dark[DT_IOP_FLATFIELD_MAX_PATH];
  int64_t flat_mtime, flat_size; // a file replaced in place under the same name is a new source
  int64_t dark_mtime, dark_size;
  float radius;
  int32_t color;
  int32_t width, height;         // the module's whole input: the sensor after rawprepare's border trim
  int32_t crop_x, crop_y;        // that trim, which the reference must share
  uint32_t filters;
  uint8_t xtrans[6][6];
  char camera[128];              // the image's camera_makermodel, empty when its decoder gave none
} dt_iop_flatfield_source_t;

typedef struct dt_iop_flatfield_data_t
{
  dt_iop_flatfield_source_t source; // width and height are filled in process(), from piece->buf_in
  float strength;
  uint64_t failed_hash; // the last source this pipe could not build a map from, not decoded again
} dt_iop_flatfield_data_t;

typedef struct dt_iop_flatfield_gui_data_t
{
  GtkWidget *flat, *dark; // labels naming the chosen files
  GtkWidget *radius, *color, *strength;
} dt_iop_flatfield_gui_data_t;

typedef struct dt_iop_flatfield_global_data_t
{
  int kernel_flatfield;
} dt_iop_flatfield_global_data_t;

typedef enum dt_iop_flatfield_status_t
{
  DT_FLATFIELD_OK = 0,
  DT_FLATFIELD_UNREADABLE,
  DT_FLATFIELD_MISMATCH,
  DT_FLATFIELD_BLACK,
  DT_FLATFIELD_NO_MEMORY,
} dt_iop_flatfield_status_t;

/** What decoding a reference file says about it, besides its sensels. */
typedef struct dt_iop_flatfield_reference_t
{
  size_t clipped;   // sensels at the sensor's saturation, see _count_clipped()
  char camera[128]; // its camera_makermodel, empty when its decoder gave none
} dt_iop_flatfield_reference_t;

/**
 * The gain map as the shared pixelpipe cache holds it: this header, then one gain per sensel of
 * the module's input, then -- when there is a dark frame -- one dark value per sensel, both in the
 * input's own row-major layout. Aligned on a cache line so the floats that follow are too.
 */
typedef struct __attribute__((aligned(64))) dt_iop_flatfield_map_t
{
  int32_t status;   // dt_iop_flatfield_status_t: a map that failed is kept until its waiters read why
  int32_t has_dark;
} dt_iop_flatfield_map_t;


const char *name()
{
  return _("Raw flat field");
}

const char *aliases()
{
  return _("vignetting|shading|light falloff|dark frame");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("Correct the uneven light and color across the frame, as recorded on a "
                                        "reference raw of an evenly lit field"),
                                _("corrective"), _("linear, raw, scene-referred"), _("linear, raw"),
                                _("linear, raw, scene-referred"));
}

int default_group()
{
  return IOP_GROUP_REPAIR;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE | IOP_FLAGS_ALLOW_TILING;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RAW;
}

void input_format(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece,
                  dt_iop_buffer_dsc_t *dsc)
{
  default_input_format(self, pipe, piece, dsc);
  dsc->channels = 1;
  dt_iop_buffer_dsc_update_bpp(dsc);
}

/**
 * @brief Write one decoded raw into @p out, normalised the way rawprepare normalises the image:
 * black level subtracted per 2x2 site, divided by the white point's distance to it. The reference
 * is read through the same border trim as the image, so both share one sensel grid. When @p dark
 * is given it is subtracted, sensel by sensel.
 */
static void _normalize(const dt_image_t *const img, const void *const raw,
                       const dt_iop_flatfield_source_t *const source, const float *const restrict dark,
                       float *const restrict out)
{
  const size_t width = source->width;
  const size_t stride = img->width;
  const size_t x0 = img->crop_x;
  const size_t y0 = img->crop_y;
  const gboolean is_float = img->dsc.datatype == TYPE_FLOAT;
  float black[4], scale[4];
  for(int k = 0; k < 4; k++)
  {
    black[k] = img->raw_black_level_separate[k];
    scale[k] = 1.0f / ((float)img->raw_white_point - black[k]);
  }

  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < (size_t)source->height; row++)
  {
    // the 2x2 site of the black level is counted on the untrimmed sensor, as rawprepare does
    const size_t site_row = ((row + y0) & 1) << 1;
    for(size_t col = 0; col < width; col++)
    {
      const size_t in = (row + y0) * stride + col + x0;
      const size_t k = site_row | ((col + x0) & 1);
      const float value = is_float ? ((const float *)raw)[in] : (float)((const uint16_t *)raw)[in];
      const size_t o = row * width + col;
      out[o] = (value - black[k]) * scale[k] - (dark ? dark[o] : 0.0f);
    }
  }
}

/**
 * @brief Count the sensels of one decoded raw that sat at its sensor's saturation.
 *
 * A clipped sensel reads the saturation code, the same for all of them, so clipping shows as a pile
 * of sensels at the very top of a site's values, where an unclipped field has a smooth tail that
 * only a few reach. They are counted per 2x2 site, from the site's own maximum down to one
 * thousandth of its range. The declared white point is not used: it can sit below where the sensor
 * saturates, and a field brighter than it is not clipped for that.
 */
static size_t _count_clipped(const dt_image_t *const img, const void *const raw,
                             const dt_iop_flatfield_source_t *const source)
{
  const size_t width = source->width;
  const size_t stride = img->width;
  const size_t x0 = img->crop_x;
  const size_t y0 = img->crop_y;
  const gboolean is_float = img->dsc.datatype == TYPE_FLOAT;

  float top0 = -FLT_MAX, top1 = -FLT_MAX, top2 = -FLT_MAX, top3 = -FLT_MAX;
  __OMP_PARALLEL_FOR__(reduction(max:top0, top1, top2, top3))
  for(size_t row = 0; row < (size_t)source->height; row++)
  {
    const size_t site_row = ((row + y0) & 1) << 1;
    float top[4] = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for(size_t col = 0; col < width; col++)
    {
      const size_t in = (row + y0) * stride + col + x0;
      const size_t k = site_row | ((col + x0) & 1);
      const float value = is_float ? ((const float *)raw)[in] : (float)((const uint16_t *)raw)[in];
      top[k] = fmaxf(top[k], value);
    }
    top0 = fmaxf(top0, top[0]);
    top1 = fmaxf(top1, top[1]);
    top2 = fmaxf(top2, top[2]);
    top3 = fmaxf(top3, top[3]);
  }

  const float top[4] = { top0, top1, top2, top3 };
  float floor[4];
  for(int k = 0; k < 4; k++)
    floor[k] = top[k] - 1e-3f * (top[k] - (float)img->raw_black_level_separate[k]);

  size_t clipped = 0;
  __OMP_PARALLEL_FOR__(reduction(+:clipped))
  for(size_t row = 0; row < (size_t)source->height; row++)
  {
    const size_t site_row = ((row + y0) & 1) << 1;
    for(size_t col = 0; col < width; col++)
    {
      const size_t in = (row + y0) * stride + col + x0;
      const size_t k = site_row | ((col + x0) & 1);
      const float value = is_float ? ((const float *)raw)[in] : (float)((const uint16_t *)raw)[in];
      clipped += value >= floor[k];
    }
  }
  return clipped;
}

/**
 * @brief Decode a reference file at full size into @p out (see _normalize()), after checking it
 * shares the image's sensor: same CFA, same border trim, same size. @p ref receives what else
 * the decoding said about the file.
 */
static dt_iop_flatfield_status_t _load_reference(const char *const path,
                                                 const dt_iop_flatfield_source_t *const source,
                                                 const float *const dark, float *const out,
                                                 dt_iop_flatfield_reference_t *const ref)
{
  dt_image_t *img = dt_alloc_align(sizeof(dt_image_t)); // dt_image_t is 64-aligned
  if(IS_NULL_PTR(img)) return DT_FLATFIELD_NO_MEMORY;
  dt_image_init(img);
  gchar *basename = g_path_get_basename(path);
  g_strlcpy(img->filename, basename, sizeof(img->filename));
  dt_free(basename);

  dt_mipmap_buffer_t buf;
  const dt_imageio_retval_t ret = dt_imageio_open_standalone(img, path, &buf);
  // the decoder already said so in its own window when memory ran short
  dt_iop_flatfield_status_t status = (ret == DT_IMAGEIO_CACHE_FULL) ? DT_FLATFIELD_NO_MEMORY : DT_FLATFIELD_UNREADABLE;

  const gboolean matches = ret == DT_IMAGEIO_OK && img->dsc.channels == 1 && img->dsc.filters == source->filters
                           && img->crop_x == source->crop_x && img->crop_y == source->crop_y
                           && img->p_width == source->width && img->p_height == source->height
                           && (source->filters != 9u || !memcmp(img->dsc.xtrans, source->xtrans, sizeof(source->xtrans)));
  if(ret == DT_IMAGEIO_OK) status = matches ? DT_FLATFIELD_OK : DT_FLATFIELD_MISMATCH;
  if(matches)
  {
    _normalize(img, buf.buf, source, dark, out);
    ref->clipped = _count_clipped(img, buf.buf, source);
    if(img->camera_model[0]) g_strlcpy(ref->camera, img->camera_makermodel, sizeof(ref->camera));
  }

  dt_imageio_close_standalone(&buf);
  g_list_free_full(img->dng_gain_maps, dt_free_gpointer);
  dt_free_align(img);
  return status;
}

/**
 * @brief Turn the normalised flat held in @p map into the gain of each sensel, in place.
 *
 * Nothing is mixed across CFA colours. Each phase of the colour filter pattern -- one sensel per
 * period-by-period tile, 4 of them on a 2x2 Bayer, 36 on a 6x6 X-Trans -- is gathered into a
 * regular single-colour image of its own, smoothed there, and divided by its own mean over the
 * centre of the frame. The centre therefore keeps its values, and with them its white balance.
 *
 * With colour correction, each phase is corrected by its own shape, so a light that drifts in hue
 * across the field is flattened too. Without it, every phase of a tile shares the average of
 * their shapes: the falloff is corrected and any colour drift is left alone.
 */
static dt_iop_flatfield_status_t _flat_to_gain(const dt_dev_pixelpipe_t *pipe,
                                               const dt_iop_flatfield_source_t *const source,
                                               float *const restrict map)
{
  const size_t period = (source->filters == 9u) ? 6 : 2;
  const size_t phases = period * period;
  const size_t width = source->width;
  const size_t height = source->height;
  const size_t pw = width / period;
  const size_t ph = height / period;
  const size_t plane = pw * ph;

  float *const restrict planes = dt_pixelpipe_cache_alloc_align_float(plane * phases, pipe);
  if(IS_NULL_PTR(planes)) return DT_FLATFIELD_NO_MEMORY;

  __OMP_PARALLEL_FOR__(collapse(2))
  for(size_t p = 0; p < phases; p++)
    for(size_t t = 0; t < plane; t++)
      planes[p * plane + t] = map[((t / pw) * period + p / period) * width + (t % pw) * period + p % period];

  // the radius is given in sensels; three box passes make a near-gaussian
  const int radius = (int)roundf(source->radius / (float)period);
  int err = 0;
  for(size_t p = 0; p < phases && !err && radius > 0; p++)
    err = dt_box_mean(planes + p * plane, ph, pw, 1, radius, 3);

  // the centre is the middle eighth of the frame on each axis
  const size_t x0 = pw * 7 / 16, x1 = MAX(pw * 9 / 16, x0 + 1);
  const size_t y0 = ph * 7 / 16, y1 = MAX(ph * 9 / 16, y0 + 1);
  gboolean black = FALSE;
  for(size_t p = 0; p < phases; p++)
  {
    float *const restrict phase = planes + p * plane;
    double sum = 0.0;
    for(size_t y = y0; y < y1; y++)
      for(size_t x = x0; x < x1; x++) sum += phase[y * pw + x];
    const float mean = (float)(sum / (double)((y1 - y0) * (x1 - x0)));
    black |= !(mean > 1e-6f); // also catches a NaN
    const float inv = 1.0f / mean;
    __OMP_PARALLEL_FOR_SIMD__()
    for(size_t t = 0; t < plane; t++) phase[t] *= inv;
  }

  // without colour correction, plane 0 takes the average shape and every phase reads it
  const size_t shared = source->color ? 1 : 0;
  if(!source->color)
  {
    __OMP_PARALLEL_FOR__()
    for(size_t t = 0; t < plane; t++)
    {
      float sum = 0.0f;
      for(size_t p = 0; p < phases; p++) sum += planes[p * plane + t];
      planes[t] = sum / (float)phases;
    }
  }

  // the sensels past the last whole tile, on the right and bottom edges, read the last tile
  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < height; row++)
  {
    const size_t tile_row = MIN(row / period, ph - 1) * pw;
    for(size_t col = 0; col < width; col++)
    {
      const size_t p = (row % period) * period + col % period;
      const float shape = planes[p * shared * plane + tile_row + MIN(col / period, pw - 1)];
      map[row * width + col] = shape > 0.0f ? 1.0f / shape : 1.0f;
    }
  }

  dt_pixelpipe_cache_free_align(planes);
  if(err) return DT_FLATFIELD_NO_MEMORY;
  return black ? DT_FLATFIELD_BLACK : DT_FLATFIELD_OK;
}

/**
 * @brief Say in a window that the reference file at @p path was shot with another camera model
 * than the image, when both models are known. Its sensor grid matches, or it would not be used.
 */
static void _warn_other_camera(const dt_iop_flatfield_source_t *const source,
                               const dt_iop_flatfield_reference_t *const ref, const char *const path)
{
  if(!source->camera[0] || !ref->camera[0] || !strcmp(source->camera, ref->camera)) return;
  gchar *item = g_strdup_printf(_("%s, shot with %s"), path, ref->camera);
  dt_control_alert(_("Flat field reference from another camera"),
                   _("A reference file of the flat field module was shot with another camera model than the "
                     "image. Its sensor has the same layout, so it is applied, but its correction may be wrong."),
                   item);
  dt_free(item);
}

/**
 * @brief Build the whole map of @p source into @p map, and say in a window why it could not be
 * when it could not. The dark frame is read first, so the flat can be read minus it.
 *
 * A flat with more than one sensel in 10000 at its sensor's saturation (see _count_clipped()) is
 * used all the same, but said to be clipped: where it saturated it did not record the falloff, and
 * its gain there is wrong. The margin lets hot pixels and a few specks of light through. A reference from another camera model
 * whose sensor grid matches is used too, and said to be.
 */
static dt_iop_flatfield_status_t _build_map(const dt_dev_pixelpipe_t *pipe,
                                            const dt_iop_flatfield_source_t *const source,
                                            dt_iop_flatfield_map_t *const map)
{
  const size_t sensels = (size_t)source->width * source->height;
  float *const gain = (float *)(map + 1);
  float *const dark = source->dark[0] ? gain + sensels : NULL;
  map->has_dark = !IS_NULL_PTR(dark);

  const char *path = source->dark;
  dt_iop_flatfield_reference_t dark_ref = { 0 }, flat_ref = { 0 };
  dt_iop_flatfield_status_t status = dark ? _load_reference(source->dark, source, NULL, dark, &dark_ref)
                                          : DT_FLATFIELD_OK;
  if(status == DT_FLATFIELD_OK)
  {
    path = source->flat;
    status = _load_reference(source->flat, source, dark, gain, &flat_ref);
  }
  if(status == DT_FLATFIELD_OK) status = _flat_to_gain(pipe, source, gain);

  static const char *const messages[] = {
    [DT_FLATFIELD_UNREADABLE] = N_("A reference file of the flat field module could not be read, "
                                   "so the module is not applied."),
    [DT_FLATFIELD_MISMATCH] = N_("A reference file of the flat field module does not match the image: it must be "
                                 "a raw file from the same camera. The module is not applied."),
    [DT_FLATFIELD_BLACK] = N_("The flat field reference is too dark to derive a correction from, "
                              "so the module is not applied."),
  };
  if(status != DT_FLATFIELD_OK && status != DT_FLATFIELD_NO_MEMORY)
    dt_control_alert(_("Module failed"), _(messages[status]), path);
  if(status != DT_FLATFIELD_OK) return status;

  if(flat_ref.clipped > sensels / 10000)
    dt_control_alert(_("Flat field reference clipped"),
                     _("The flat field reference is saturated in places, where the correction is wrong. "
                       "Its exposure level does not matter otherwise: shoot it again darker."),
                     source->flat);
  if(dark) _warn_other_camera(source, &dark_ref, source->dark);
  _warn_other_camera(source, &flat_ref, source->flat);
  return status;
}

/**
 * @brief Fetch the gain map of @p source from the shared pixelpipe cache, building it on a miss.
 *
 * Every pipe of an image asks for the same hash, so the first one builds the map while the others
 * wait for its write lock to go. On return no lock is held -- once its builder has released it, a
 * map is never written again -- but the caller holds one reference on `*entry` whenever it is not
 * NULL, and drops it with dt_dev_pixelpipe_cache_ref_count_entry(FALSE, *entry) when done reading.
 *
 * @return the map, whose status says whether it can be used, or NULL when memory ran short.
 */
static const dt_iop_flatfield_map_t *_get_map(const dt_dev_pixelpipe_t *pipe,
                                              const dt_iop_flatfield_source_t *const source, const uint64_t hash,
                                              struct dt_pixel_cache_entry_t **entry)
{
  const size_t sensels = (size_t)source->width * source->height;
  const size_t size = sizeof(dt_iop_flatfield_map_t) + sensels * sizeof(float) * (source->dark[0] ? 2 : 1);
  void *data = NULL;
  const int created = dt_dev_pixelpipe_cache_get(hash, size, "flatfield gain map", pipe->type, TRUE, &data, entry);
  if(IS_NULL_PTR(*entry)) return NULL;

  if(!created)
  {
    // built, or still being built by another pipe: taking its read lock waits for the write lock
    dt_dev_pixelpipe_cache_rdlock_entry(TRUE, *entry);
    dt_dev_pixelpipe_cache_rdlock_entry(FALSE, *entry);
    return data;
  }

  dt_iop_flatfield_map_t *map = data;
  if(!IS_NULL_PTR(map)) map->status = _build_map(pipe, source, map);
  // a map that could not be built is not kept: flagged while still held, the last release removes it
  if(IS_NULL_PTR(map) || map->status != DT_FLATFIELD_OK) dt_dev_pixelpipe_cache_flag_auto_destroy(*entry);
  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, *entry);
  return map;
}

/**
 * @brief The source of this run's map, in @p source: the one commit_params() saw, sized from the
 * module's whole input, which is only known once the ROI is planned. Returns its hash.
 */
static uint64_t _source_hash(const dt_dev_pixelpipe_iop_t *piece, dt_iop_flatfield_source_t *const source)
{
  *source = ((const dt_iop_flatfield_data_t *)piece->data)->source;
  source->width = piece->buf_in.width;
  source->height = piece->buf_in.height;
  return dt_hash(dt_hash(5381, "flatfield", 9), (const char *)source, sizeof(*source));
}

int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  dt_iop_flatfield_data_t *const d = (dt_iop_flatfield_data_t *)piece->data;
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;

  dt_iop_flatfield_source_t source;
  const uint64_t hash = _source_hash(piece, &source);

  // a source that failed passes the image through, and is not decoded again by this pipe
  if(hash == d->failed_hash)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 1);
    return 0;
  }

  struct dt_pixel_cache_entry_t *entry = NULL;
  const dt_iop_flatfield_map_t *map = _get_map(pipe, &source, hash, &entry);
  if(IS_NULL_PTR(map))
  {
    if(entry) dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
    return 1;
  }

  if(map->status != DT_FLATFIELD_OK)
  {
    d->failed_hash = hash;
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, 1);
    dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
    return 0;
  }

  // the map spans the module's whole input and the ROI is a window into it, at scale 1 in the RAW
  // domain: a sensel's gain is found at its absolute position, whatever tile this is
  const size_t map_width = source.width;
  const float *const restrict gain = (const float *)(map + 1);
  const float *const restrict dark = map->has_dark ? gain + (size_t)source.width * source.height : NULL;
  const float strength = d->strength;
  const float *const restrict in = (const float *)ivoid;
  float *const restrict out = (float *)ovoid;
  const size_t width = roi_out->width;

  __OMP_PARALLEL_FOR__()
  for(size_t row = 0; row < (size_t)roi_out->height; row++)
  {
    const size_t m = (row + roi_in->y) * map_width + roi_in->x;
    for(size_t col = 0; col < width; col++)
    {
      const float value = in[row * width + col] - (dark ? dark[m + col] : 0.0f);
      out[row * width + col] = value * (1.0f + strength * (gain[m + col] - 1.0f));
    }
  }

  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
  return 0;
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out)
{
  dt_iop_flatfield_data_t *const d = (dt_iop_flatfield_data_t *)piece->data;
  const dt_iop_flatfield_global_data_t *const gd = (dt_iop_flatfield_global_data_t *)self->global_data;
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const int devid = pipe->devid;
  const int width = piece->roi_out.width;
  const int height = piece->roi_out.height;
  size_t origin[] = { 0, 0, 0 };
  size_t region[] = { width, height, 1 };

  dt_iop_flatfield_source_t source;
  const uint64_t hash = _source_hash(piece, &source);

  // a source that failed passes the image through, as in process()
  if(hash == d->failed_hash)
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region) == CL_SUCCESS;

  struct dt_pixel_cache_entry_t *entry = NULL;
  const dt_iop_flatfield_map_t *map = _get_map(pipe, &source, hash, &entry);
  if(IS_NULL_PTR(map))
  {
    if(entry) dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
    return FALSE;
  }

  if(map->status != DT_FLATFIELD_OK)
  {
    d->failed_hash = hash;
    dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region) == CL_SUCCESS;
  }

  // The maps go to the device whole -- gain, then dark, stacked as they are in host memory -- once
  // per entry and device: the image stays with the cache entry, where the next run finds it again
  // until a pipe's cleanup reclaims the device's idle buffers. A map taller than the device's largest
  // image fails to allocate, and the pipeline then runs process() instead.
  const int rows = source.height * (map->has_dark ? 2 : 1);
  const dt_iop_roi_t whole = { 0, 0, source.width, rows, 1.0f };
  gboolean reused = FALSE;
  cl_mem dev_map = dt_dev_pixelpipe_cache_get_cl_buffer(devid, NULL, &whole, sizeof(float), self,
                                                        "flatfield gain map", entry, &reused, NULL);
  cl_int err = -999;
  if(!IS_NULL_PTR(dev_map))
    err = reused ? CL_SUCCESS
                 : dt_opencl_write_host_to_device(devid, (void *)(map + 1), dev_map, source.width, rows, sizeof(float));

  if(err == CL_SUCCESS)
  {
    // the ROI is a window into the whole input, as in process()
    const int dark_row = map->has_dark ? source.height : 0;
    size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 2, sizeof(int), (void *)&width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 3, sizeof(int), (void *)&height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 4, sizeof(cl_mem), (void *)&dev_map);
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 5, sizeof(int), (void *)&roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 6, sizeof(int), (void *)&roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 7, sizeof(int), (void *)&dark_row);
    dt_opencl_set_kernel_arg(devid, gd->kernel_flatfield, 8, sizeof(float), (void *)&d->strength);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_flatfield, sizes);
  }

  // an image whose upload failed is not kept for the next run to take as filled
  dt_dev_pixelpipe_cache_release_cl_buffer((void **)&dev_map, entry, NULL, err == CL_SUCCESS);
  dt_dev_pixelpipe_cache_ref_count_entry(FALSE, entry);
  return err == CL_SUCCESS;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl
  dt_iop_flatfield_global_data_t *gd = (dt_iop_flatfield_global_data_t *)malloc(sizeof(dt_iop_flatfield_global_data_t));
  module->data = gd;
  gd->kernel_flatfield = dt_opencl_create_kernel(program, "flatfield");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_flatfield_global_data_t *gd = (dt_iop_flatfield_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_flatfield);
  dt_free(module->data);
}
#endif

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_flatfield_params_t *const p = (dt_iop_flatfield_params_t *)params;
  dt_iop_flatfield_data_t *const d = (dt_iop_flatfield_data_t *)piece->data;
  const dt_image_t *const img = &pipe->dev->image_storage;
  dt_iop_flatfield_source_t *const s = &d->source;

  memset(s, 0, sizeof(*s));
  g_strlcpy(s->flat, p->flat, sizeof(s->flat));
  g_strlcpy(s->dark, p->dark, sizeof(s->dark));
  // a file that cannot be stat'ed keeps zeroes here, and fails to decode with its own message
  GStatBuf st;
  if(p->flat[0] && g_stat(p->flat, &st) == 0)
  {
    s->flat_mtime = st.st_mtime;
    s->flat_size = st.st_size;
  }
  if(p->dark[0] && g_stat(p->dark, &st) == 0)
  {
    s->dark_mtime = st.st_mtime;
    s->dark_size = st.st_size;
  }
  s->radius = p->radius;
  s->color = p->color;
  s->crop_x = img->crop_x;
  s->crop_y = img->crop_y;
  s->filters = img->dsc.filters;
  memcpy(s->xtrans, img->dsc.xtrans, sizeof(s->xtrans));
  if(img->camera_model[0]) g_strlcpy(s->camera, img->camera_makermodel, sizeof(s->camera));
  d->strength = p->strength;

  // without a reference there is nothing to divide by
  if(!p->flat[0]) piece->enabled = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_flatfield_data_t));
  piece->data_size = sizeof(dt_iop_flatfield_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

/* The division needs a sensor mosaic: an already demosaiced raw has no CFA left to correct per
 * colour. Shared by reload_defaults(), force_enable() and gui_update(). */
static gboolean _flatfield_supported(const dt_image_t *img)
{
  return dt_image_needs_demosaic(img);
}

void reload_defaults(dt_iop_module_t *self)
{
  self->hide_enable_button = !_flatfield_supported(&self->dev->image_storage);
}

gboolean force_enable(struct dt_iop_module_t *self, const gboolean current_state)
{
  return current_state && _flatfield_supported(&self->dev->image_storage);
}

/** @brief Name the chosen file by its basename, its full path in the tooltip. */
static void _show_file(GtkWidget *label, const char *const path, const char *const none)
{
  gchar *text = path[0] ? g_path_get_basename(path) : g_strdup(none);
  gtk_label_set_text(GTK_LABEL(label), text);
  gtk_widget_set_tooltip_text(label, path[0] ? path : NULL);
  dt_free(text);
}

static void _show_files(dt_iop_module_t *self)
{
  dt_iop_flatfield_gui_data_t *g = (dt_iop_flatfield_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_flatfield_params_t *const p = (dt_iop_flatfield_params_t *)self->params;
  _show_file(g->flat, p->flat, _("No reference chosen"));
  _show_file(g->dark, p->dark, _("No dark frame"));
}

/** @brief Set @p target, one of the two paths of the params, to the empty string or to @p path. */
static void _set_path(dt_iop_module_t *self, char *const target, const char *const path)
{
  // zeroed first: params are compared and hashed as bytes, and a shorter path must not keep the
  // tail of the longer one it replaces
  memset(target, 0, DT_IOP_FLATFIELD_MAX_PATH);
  g_strlcpy(target, path, DT_IOP_FLATFIELD_MAX_PATH);
  _show_files(self);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void _choose_file(dt_iop_module_t *self, char *const target, const char *const title)
{
  GtkFileChooserNative *chooser = gtk_file_chooser_native_new(title, GTK_WINDOW(dt_gui_main_window()),
                                                              GTK_FILE_CHOOSER_ACTION_OPEN, _("_Select"),
                                                              _("_Cancel"));
  // open where the current file is, else beside the image being edited
  gchar *folder = g_path_get_dirname(target[0] ? target : self->dev->image_storage.fullpath);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), folder);
  dt_free(folder);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    _set_path(self, target, path);
    dt_free(path);
  }
  g_object_unref(chooser);
}

static void _flat_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_flatfield_params_t *p = (dt_iop_flatfield_params_t *)self->params;
  _choose_file(self, p->flat, _("Select the flat field reference"));
}

static void _dark_clicked(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_flatfield_params_t *p = (dt_iop_flatfield_params_t *)self->params;
  _choose_file(self, p->dark, _("Select the dark frame"));
}

static void _dark_removed(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_flatfield_params_t *p = (dt_iop_flatfield_params_t *)self->params;
  _set_path(self, p->dark, "");
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_flatfield_gui_data_t *g = (dt_iop_flatfield_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_flatfield_params_t *const p = (dt_iop_flatfield_params_t *)self->params;
  _show_files(self);
  // bauhaus sliders read their field when drawn; a check button has to be told
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->color), p->color);
  self->hide_enable_button = !_flatfield_supported(&self->dev->image_storage);
  gtk_stack_set_visible_child_name(GTK_STACK(self->gui->widget), self->hide_enable_button ? "non_raw" : "raw");
}

/** @brief One row: a button that opens the file chooser, then the label naming the file. */
static GtkWidget *_file_row(GtkWidget **label, const char *const tooltip, GCallback clicked, dt_iop_module_t *self)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  GtkWidget *button = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_NONE, NULL);
  gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect(G_OBJECT(button), "clicked", clicked, self);
  gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0);

  *label = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(*label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_xalign(GTK_LABEL(*label), 0.0f);
  gtk_box_pack_start(GTK_BOX(row), *label, TRUE, TRUE, 0);
  return row;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_flatfield_gui_data_t *g = IOP_GUI_ALLOC(flatfield);

  GtkWidget *box_raw = self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  GtkWidget *row = _file_row(&g->flat, _("Choose the reference: a raw of an evenly lit, featureless field, "
                                         "shot with the same camera, lens and aperture as the image"),
                             G_CALLBACK(_flat_clicked), self);
  gtk_box_pack_start(GTK_BOX(box_raw), row, FALSE, FALSE, 0);

  row = _file_row(&g->dark, _("Choose a dark frame: a raw shot without light, at the same settings as the "
                              "reference and the image. It is subtracted from both before dividing"),
                  G_CALLBACK(_dark_clicked), self);
  GtkWidget *remove = dtgtk_button_new(dtgtk_cairo_paint_remove, CPF_NONE, NULL);
  gtk_widget_set_tooltip_text(remove, _("Stop using a dark frame"));
  g_signal_connect(G_OBJECT(remove), "clicked", G_CALLBACK(_dark_removed), self);
  gtk_box_pack_end(GTK_BOX(row), remove, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box_raw), row, FALSE, FALSE, 0);

  g->radius = dt_bauhaus_slider_from_params(self, "radius");
  dt_bauhaus_slider_set_format(g->radius, " px");
  gtk_widget_set_tooltip_text(g->radius, _("Blur the reference before dividing by it, so its noise, its grain and "
                                           "the dust on its surface stay out of the image. "
                                           "At 0, every sensel is corrected by its own value"));

  g->color = dt_bauhaus_toggle_from_params(self, "color");
  gtk_widget_set_tooltip_text(g->color, _("Correct each color channel by its own shape, which also removes a "
                                          "color drift of the light across the field. "
                                          "Off, only the falloff common to all channels is corrected"));

  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  dt_bauhaus_slider_set_factor(g->strength, 100.0f);
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("How much of the correction is applied"));

  self->gui->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->gui->widget), FALSE);
  GtkWidget *label_non_raw = dt_ui_label_new(_("Flat field correction\nonly works for raw images."));
  gtk_stack_add_named(GTK_STACK(self->gui->widget), label_non_raw, "non_raw");
  gtk_stack_add_named(GTK_STACK(self->gui->widget), box_raw, "raw");
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
