/*
    This file is part of darktable,
    Copyright (C) 2022 paolodepetrillo.
    
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

#include <glib.h>
#include <stdio.h>
#include <string.h>

#include "dng_opcode.h"
#include "logging.h"

#define OPCODE_ID_GAINMAP (9)

static double get_double(uint8_t *ptr)
{
  guint64 in;
  union {
    guint64 out;
    double v;
  } u;
  memcpy(&in, ptr, sizeof(in));
  u.out = GUINT64_FROM_BE(in);
  return u.v;
}

static float get_float(uint8_t *ptr)
{
  guint32 in;
  union {
    guint32 out;
    float v;
  } u;
  memcpy(&in, ptr, sizeof(in));
  u.out = GUINT32_FROM_BE(in);
  return u.v;
}

static uint32_t get_long(uint8_t *ptr)
{
  uint32_t in;
  memcpy(&in, ptr, sizeof(in));
  return GUINT32_FROM_BE(in);
}

typedef enum _dng_opcode_iter_status_t
{
  _DNG_OPCODE_ITER_OK,
  _DNG_OPCODE_ITER_COUNT_TRUNCATED,
  _DNG_OPCODE_ITER_HEADER_TRUNCATED,
  _DNG_OPCODE_ITER_PAYLOAD_TRUNCATED,
} _dng_opcode_iter_status_t;

typedef struct _dng_opcode_envelope_t
{
  uint32_t opcode_id;
  uint32_t flags;
  uint8_t *payload;
  uint32_t payload_size;
} _dng_opcode_envelope_t;

typedef void (*_dng_opcode_handler_t)(const _dng_opcode_envelope_t *envelope, void *context);

static _dng_opcode_iter_status_t _dng_opcode_foreach(uint8_t *buf, uint32_t buf_size,
                                                      _dng_opcode_handler_t handler, void *context)
{
  if(buf_size < 4)
    return _DNG_OPCODE_ITER_COUNT_TRUNCATED;

  const uint32_t count = get_long(&buf[0]);
  uint64_t offset = 4;
  for(uint64_t index = 0; index < count; index++)
  {
    if(offset > buf_size || 16 > (uint64_t)buf_size - offset)
      return _DNG_OPCODE_ITER_HEADER_TRUNCATED;

    const uint64_t payload_offset = offset + 16;
    const uint32_t payload_size = get_long(&buf[offset + 12]);
    if(payload_size > (uint64_t)buf_size - payload_offset)
      return _DNG_OPCODE_ITER_PAYLOAD_TRUNCATED;

    const _dng_opcode_envelope_t envelope = {
      .opcode_id = get_long(&buf[offset]),
      .flags = get_long(&buf[offset + 8]),
      .payload = &buf[payload_offset],
      .payload_size = payload_size,
    };
    handler(&envelope, context);
    offset = payload_offset + payload_size;
  }

  return _DNG_OPCODE_ITER_OK;
}

static void _dng_opcode_process_gain_map(const _dng_opcode_envelope_t *envelope, void *context)
{
  dt_image_t *img = context;
  if(envelope->opcode_id != OPCODE_ID_GAINMAP)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList2 has unsupported %s opcode %d\n",
             envelope->flags & 1 ? "optional" : "mandatory", envelope->opcode_id);
    return;
  }

  if(envelope->payload_size < 76)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Undersized GainMap opcode in OpcodeList2\n");
    return;
  }

  uint8_t *param = envelope->payload;
  const uint64_t gain_count = (envelope->payload_size - 76) / 4;
  const uint32_t map_points_v = get_long(&param[32]);
  const uint32_t map_points_h = get_long(&param[36]);
  const uint32_t map_planes = get_long(&param[72]);
  uint64_t required_gain_count = (uint64_t)map_points_v * map_points_h;
  if((map_planes > 0 && required_gain_count > UINT64_MAX / map_planes)
     || (required_gain_count *= map_planes) > gain_count
     || gain_count > (G_MAXSIZE - sizeof(dt_dng_gain_map_t)) / sizeof(float))
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Undersized GainMap opcode in OpcodeList2\n");
    return;
  }

  const gsize gain_map_size = sizeof(dt_dng_gain_map_t) + (gsize)gain_count * sizeof(float);
  dt_dng_gain_map_t *gm = g_malloc(gain_map_size);
  gm->top = get_long(&param[0]);
  gm->left = get_long(&param[4]);
  gm->bottom = get_long(&param[8]);
  gm->right = get_long(&param[12]);
  gm->plane = get_long(&param[16]);
  gm->planes = get_long(&param[20]);
  gm->row_pitch = get_long(&param[24]);
  gm->col_pitch = get_long(&param[28]);
  gm->map_points_v = map_points_v;
  gm->map_points_h = map_points_h;
  gm->map_spacing_v = get_double(&param[40]);
  gm->map_spacing_h = get_double(&param[48]);
  gm->map_origin_v = get_double(&param[56]);
  gm->map_origin_h = get_double(&param[64]);
  gm->map_planes = map_planes;
  for(uint64_t i = 0; i < gain_count; i++)
    gm->map_gain[i] = get_float(&param[76 + 4 * i]);

  img->dng_gain_maps = g_list_append(img->dng_gain_maps, gm);
}

void dt_dng_opcode_process_opcode_list_2(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  g_list_free_full(img->dng_gain_maps, dt_free_gpointer);
  img->dng_gain_maps = NULL;

  const _dng_opcode_iter_status_t status = _dng_opcode_foreach(buf, buf_size,
                                                                 _dng_opcode_process_gain_map, img);
  if(status == _DNG_OPCODE_ITER_COUNT_TRUNCATED)
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList2 buffer too small for opcode count\n");
  else if(status == _DNG_OPCODE_ITER_HEADER_TRUNCATED)
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Truncated opcode header in OpcodeList2\n");
  else if(status == _DNG_OPCODE_ITER_PAYLOAD_TRUNCATED)
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList2\n");
}

#define OPCODE_ID_WARP_RECTILINEAR (1)
#define OPCODE_ID_VIGNETTE_RADIAL (3)

// DNG spec: WarpRectilinear param body is a uint32 plane count followed by that many
// groups of 6 doubles (radial kr0..kr3 + tangential kt0,kt1 = 48 bytes/plane), then 2
// doubles (cx, cy) at the end. dt_image_correction_dng_t::warp_coeffs is fixed at 3
// planes (per-channel TCA never needs more); anything outside [1,3] can't be
// represented and is rejected rather than silently truncated.
#define DNG_WARP_PLANES_MIN (1)
#define DNG_WARP_PLANES_MAX (3)
#define DNG_WARP_HEADER_SIZE (4)
#define DNG_WARP_PLANE_SIZE (48)
#define DNG_WARP_CENTER_SIZE (16)

// DNG spec: VignetteRadial param body is 5 doubles (k0..k4, 40 bytes) followed by 2
// doubles (cx, cy, 16 bytes).
#define DNG_VIGNETTE_COEFFS_SIZE (40)
#define DNG_VIGNETTE_CENTER_SIZE (16)

// Computes the minimum WarpRectilinear param size for a given plane count. Returns
// FALSE (and leaves *min_size untouched) if planes is outside the representable range,
// so callers reject the opcode before doing any plane-indexed arithmetic.
static gboolean _dng_warp_min_size(uint32_t planes, uint64_t *min_size)
{
  if(planes < DNG_WARP_PLANES_MIN || planes > DNG_WARP_PLANES_MAX) return FALSE;
  *min_size = (uint64_t)DNG_WARP_HEADER_SIZE + (uint64_t)planes * DNG_WARP_PLANE_SIZE + DNG_WARP_CENTER_SIZE;
  return TRUE;
}

// Parses a WarpRectilinear (id 1) param body already known to be `param_size` bytes
// long and fully inside the caller's buffer. Populates dng.warp_* only on success;
// leaves them untouched (still zeroed by the caller's reset) on any rejection.
static void _dng_process_warp_rectilinear(uint8_t *param, uint32_t param_size, dt_image_t *img)
{
  if(param_size < DNG_WARP_HEADER_SIZE)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Undersized WarpRectilinear in OpcodeList3\n");
    return;
  }

  const uint32_t planes = get_long(&param[0]);
  uint64_t min_size = 0;
  if(!_dng_warp_min_size(planes, &min_size) || param_size < min_size)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid or undersized WarpRectilinear planes in OpcodeList3\n");
    return;
  }

  dt_image_correction_dng_t *dng = &img->exif_correction_data.dng;
  uint32_t off = DNG_WARP_HEADER_SIZE;
  for(uint32_t p = 0; p < planes; p++)
  {
    for(int c = 0; c < 6; c++)
    {
      dng->warp_coeffs[p][c] = get_double(&param[off]);
      off += 8;
    }
  }
  dng->warp_cx = get_double(&param[off]);
  dng->warp_cy = get_double(&param[off + 8]);
  dng->warp_planes = planes;
  dng->has_warp = TRUE;
}

// Parses a VignetteRadial (id 3) param body already known to be `param_size` bytes long
// and fully inside the caller's buffer. Populates dng.vig_* only on success.
static void _dng_process_vignette_radial(uint8_t *param, uint32_t param_size, dt_image_t *img)
{
  const uint32_t min_size = DNG_VIGNETTE_COEFFS_SIZE + DNG_VIGNETTE_CENTER_SIZE;
  if(param_size < min_size)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Undersized VignetteRadial in OpcodeList3\n");
    return;
  }

  dt_image_correction_dng_t *dng = &img->exif_correction_data.dng;
  for(int c = 0; c < 5; c++) dng->vig_coeffs[c] = get_double(&param[c * 8]);
  dng->vig_cx = get_double(&param[DNG_VIGNETTE_COEFFS_SIZE]);
  dng->vig_cy = get_double(&param[DNG_VIGNETTE_COEFFS_SIZE + 8]);
  dng->has_vignette = TRUE;
}

static void _dng_opcode_process_list_3(const _dng_opcode_envelope_t *envelope, void *context)
{
  dt_image_t *img = context;
  if(envelope->opcode_id == OPCODE_ID_WARP_RECTILINEAR)
    _dng_process_warp_rectilinear(envelope->payload, envelope->payload_size, img);
  else if(envelope->opcode_id == OPCODE_ID_VIGNETTE_RADIAL)
    _dng_process_vignette_radial(envelope->payload, envelope->payload_size, img);
  else
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList3 has unsupported %s opcode %d\n",
             envelope->flags & 1 ? "optional" : "mandatory", envelope->opcode_id);
}

void dt_dng_opcode_process_opcode_list_3(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  memset(&img->exif_correction_data.dng, 0, sizeof(img->exif_correction_data.dng));

  const _dng_opcode_iter_status_t status = _dng_opcode_foreach(buf, buf_size,
                                                                 _dng_opcode_process_list_3, img);
  if(status == _DNG_OPCODE_ITER_COUNT_TRUNCATED)
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList3 buffer too small for opcode count\n");
  else if(status == _DNG_OPCODE_ITER_HEADER_TRUNCATED)
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Truncated opcode header in OpcodeList3\n");
  else if(status == _DNG_OPCODE_ITER_PAYLOAD_TRUNCATED)
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList3\n");
}
