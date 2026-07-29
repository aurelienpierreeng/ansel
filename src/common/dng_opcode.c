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

#include "debug.h"
#include "dng_opcode.h"

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

void dt_dng_opcode_process_opcode_list_2(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  g_list_free_full(img->dng_gain_maps, dt_free_gpointer);
  img->dng_gain_maps = NULL;

  if(buf_size < 4)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList2 buffer too small for opcode count\n");
    return;
  }

  uint32_t count = get_long(&buf[0]);
  uint64_t offset = 4;
  while(count > 0)
  {
    if(offset + 16 > buf_size)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Truncated opcode header in OpcodeList2\n");
      return;
    }

    const uint32_t opcode_id = get_long(&buf[offset]);
    const uint32_t flags = get_long(&buf[offset + 8]);
    const uint32_t param_size = get_long(&buf[offset + 12]);
    uint8_t *param = &buf[offset + 16];

    if(offset + 16 + (uint64_t)param_size > buf_size)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList2\n");
      return;
    }

    if(opcode_id == OPCODE_ID_GAINMAP)
    {
      if(param_size < 76)
      {
        dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Undersized GainMap opcode in OpcodeList2\n");
        goto next;
      }
      uint32_t gain_count = (param_size - 76) / 4;
      dt_dng_gain_map_t *gm = g_malloc(sizeof(dt_dng_gain_map_t) + gain_count * sizeof(float));
      gm->top = get_long(&param[0]);
      gm->left = get_long(&param[4]);
      gm->bottom = get_long(&param[8]);
      gm->right = get_long(&param[12]);
      gm->plane = get_long(&param[16]);
      gm->planes = get_long(&param[20]);
      gm->row_pitch = get_long(&param[24]);
      gm->col_pitch = get_long(&param[28]);
      gm->map_points_v = get_long(&param[32]);
      gm->map_points_h = get_long(&param[36]);
      gm->map_spacing_v = get_double(&param[40]);
      gm->map_spacing_h = get_double(&param[48]);
      gm->map_origin_v = get_double(&param[56]);
      gm->map_origin_h = get_double(&param[64]);
      gm->map_planes = get_long(&param[72]);
      for(uint32_t i = 0; i < gain_count; i++)
        gm->map_gain[i] = get_float(&param[76 + 4*i]);

      img->dng_gain_maps = g_list_append(img->dng_gain_maps, gm);
    }
    else
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList2 has unsupported %s opcode %d\n",
        flags & 1 ? "optional" : "mandatory", opcode_id);
    }

next:
    offset += 16 + (uint64_t)param_size;
    count--;
  }
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

// Parses OpcodeList3 (post-demosaic corrections): WarpRectilinear (id 1) and
// VignetteRadial (id 3), writing into img->exif_correction_data.dng. Independent of
// dt_dng_opcode_process_opcode_list_2() above -- does not touch img->dng_gain_maps and
// does not set img->exif_correction_type (the exif.cc caller owns that).
//
// Bounds guard, applied before every read: (a) buf_size < 4 is rejected before the
// opcode count is read; (b) each opcode's 16-byte header is validated against buf_size
// before any header field is read; (c) the header's declared param_size is validated
// against buf_size before the param body is touched; (d) each opcode class enforces its
// own fixed minimum param_size before indexing into the param bytes it uses. Unknown
// opcode ids are skipped (logged at DT_DEBUG_IMAGEIO); no correction is invented for a
// class that wasn't actually present in the data.
void dt_dng_opcode_process_opcode_list_3(uint8_t *buf, uint32_t buf_size, dt_image_t *img)
{
  memset(&img->exif_correction_data.dng, 0, sizeof(img->exif_correction_data.dng));

  if(buf_size < 4)
  {
    dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList3 buffer too small for opcode count\n");
    return;
  }

  uint32_t count = get_long(&buf[0]);
  uint64_t offset = 4;

  while(count > 0)
  {
    if(offset + 16 > buf_size)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Truncated opcode header in OpcodeList3\n");
      return;
    }

    const uint32_t opcode_id = get_long(&buf[offset]);
    const uint32_t flags = get_long(&buf[offset + 8]);
    const uint32_t param_size = get_long(&buf[offset + 12]);
    uint8_t *param = &buf[offset + 16];

    if(offset + 16 + (uint64_t)param_size > buf_size)
    {
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] Invalid opcode size in OpcodeList3\n");
      return;
    }

    if(opcode_id == OPCODE_ID_WARP_RECTILINEAR)
      _dng_process_warp_rectilinear(param, param_size, img);
    else if(opcode_id == OPCODE_ID_VIGNETTE_RADIAL)
      _dng_process_vignette_radial(param, param_size, img);
    else
      dt_print(DT_DEBUG_IMAGEIO, "[dng_opcode] OpcodeList3 has unsupported %s opcode %d\n",
               flags & 1 ? "optional" : "mandatory", opcode_id);

    offset += 16 + (uint64_t)param_size;
    count--;
  }
}
