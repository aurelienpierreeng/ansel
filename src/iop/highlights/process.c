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
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

// Top-level Bayer/X-Trans CPU drivers and the hybrid OpenCL driver. (implementation; see process.h for the public
// API.)

#include "common/logging.h"
#include "system/macros.h"
#include "system/openmp.h"
#include "system/simd.h"
#include "system/target_clones.h"
#include "common/pixelpipe_cache_alloc.h"
#include "pixel/distance_transform.h"
#include "math/sparse_cholesky_cl.h"
#include "iop/highlights/blur.h"
#include "iop/highlights/gather.h"
#include "iop/highlights/knee.h"
#include "iop/highlights/process.h"
#include "iop/highlights/region.h"
#include "iop/highlights/segmentation.h"
#include "iop/highlights/selftests.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

__DT_CLONE_TARGETS__
int process_harmonic(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                     const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                     void *const restrict ovoid, const dt_iop_roi_t *const roi_in,
                     const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips)
{
  int err_code = 0;

  // Every CFA helper below (normalization, knee estimate/apply, Bayer gather, remosaic) reads
  // FC(row, col, filters) with tile-local row/col (0-based within this buffer, no roi offset
  // added), so filters must be pre-shifted for roi_in's crop position here -- mirrors
  // demosaic.c's tile-local algorithms. dt_dev_get_roi_filters() returns the shifted Bayer word,
  // 9u unchanged for X-Trans, and 0 for already-demosaiced (non-raw / sRAW) input. The
  // reconstruction between the gather and the remosaic is CFA-agnostic; only those two endpoints
  // branch on `cfa` below.
  const uint32_t filters = dt_dev_get_roi_filters(piece, roi_in);
  const dt_hl_cfa_t cfa = _hl_cfa_strategy(filters);
  const uint8_t(*const xtrans)[6]
      = (cfa == HL_CFA_XTRANS) ? (const uint8_t(*const)[6])piece->dsc_in.xtrans : NULL;

  const size_t height = roi_in->height;
  const size_t width = roi_in->width;
  const size_t size = roi_in->width * roi_in->height;

  float *const restrict interpolated
      = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe); // [R, G, B, norm] for each pixel
  float *const restrict clipping_mask
      = dt_pixelpipe_cache_alloc_align_float(size * 4, pipe); // [R, G, B, norm] for each pixel

  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask))
  {
    err_code = 1;
    goto error;
  }

  const float *const restrict input = (const float *const restrict)ivoid;
  float *const restrict output = (float *const restrict)ovoid;
  dt_aligned_pixel_t normalization = { 1.f, 1.f, 1.f, 1.f };
  _compute_laplacian_normalization(input, roi_in, filters, xtrans, normalization);

  // Rolloff estimation FIRST (raw-based, mask-independent): its engagement decides, per
  // channel, whether the detection extends into the band (the band override,
  // DT_HL_BAND_OVR = 0.9, compile-time). Only channels with a
  // MEASURED rolloff get the override -- on hard-clipping sensors the band is trustworthy
  // data and stays valid. The knee reads `input` as a raw mosaic; it is meaningless for
  // already-demosaiced input and would misread a 4-channel buffer, so it never runs in the
  // passthrough case (knee stays disengaged there, det_scale stays unit).
  const gboolean allow_knee = (cfa != HL_CFA_PASSTHROUGH);
  _hl_knee_curve_t knee[3] = { 0 };
  dt_aligned_pixel_t clipvaln = { 1.f, 1.f, 1.f, 1.f };
  dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
  for(int c = 0; c < 3; c++)
  {
    clipvaln[c] = clips[c] / (DT_HL_KNEE_DET * fmaxf(normalization[c], 1e-9f));
    knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;
  }

  // FLOW step 2 (knee): estimate the per-channel sensor-rolloff inverse from the raw mosaic (step-2 maths
  // annotated on _hl_knee_estimate below). Runs on the raw values, before the gather, so the correction
  // is mask-independent; applied to the interpolated planes just below via _hl_knee_apply_interpolated.
  int knee_on = 0;
  if(allow_knee)
  {
    _hl_knee_estimate(input, width, height, filters, roi_in, xtrans, knee_clipraw, knee, pipe);
    knee_on = knee[0].engaged || knee[1].engaged || knee[2].engaged;
  }

  dt_aligned_pixel_t det_scale = { 1.f, 1.f, 1.f, 1.f };
  for(int c = 0; c < 3; c++)
    if(knee[c].engaged) det_scale[c] = DT_HL_BAND_OVR;

  dt_aligned_pixel_t eff_clips;
  for_four_channels(c) eff_clips[c] = clips[c] * det_scale[c];

  // FLOW step 1a (gather): bilinear interpolation of the raw mosaic into [R, G, B, norm] planes + the
  // binary per-channel clip masks -- the article's "interpolate + masks" node, input to every later step.
  // Only this endpoint branches on the CFA: the Bayer gather multiplies clips by det_scale internally,
  // the X-Trans gather takes the pre-multiplied eff_clips and interpolates through a 6x6 lookup.
  switch(cfa)
  {
    case HL_CFA_BAYER:
      _interpolate_and_mask(input, interpolated, clipping_mask, clips, det_scale, normalization, filters, width,
                            height);
      break;
    case HL_CFA_XTRANS:
    {
      int32_t lookup[6][6][32] = { { { 0 } } };
      _build_xtrans_bilinear_lookup(lookup, roi_in, xtrans);
      _interpolate_and_mask_xtrans(input, interpolated, clipping_mask, eff_clips, normalization, roi_in, lookup,
                                   xtrans, width, height);
      break;
    }
    case HL_CFA_PASSTHROUGH:
      // Non-raw / sRAW: no demosaic, just copy the RGB planes through + build masks. Knee is off, so
      // clips is the plain threshold (eff_clips == clips here).
      _interpolate_and_mask_passthrough(input, interpolated, clipping_mask, clips, normalization, width, height);
      break;
  }
  // No mask feathering in this mode: the masks stay BINARY end to end. The per-channel
  // validity masks define measurement validity for every fit (feathering them reclassified
  // rim-clipped photosites -- raw values biased at the detection threshold -- as valid anchors
  // and dragged oblique rims toward the clip level), and the compositing alpha is a hard
  // switch (measured equivalent to the feathered composite once validity is binary and clipped
  // raw values are floors -- see the graveyard of the companion article).

  // Rolloff pre-correction of the working planes (the estimation ran before the gather; the
  // lift is value-based and independent of the mask, so band values -- including any the
  // override reclassified as reconstructable -- carry their corrected level, which the region
  // gather then freezes into the per-pixel floors clip0).
  if(knee_on) _hl_knee_apply_interpolated(interpolated, size, clipvaln, normalization, knee);

  // MATHS BRIDGE -- Step 1 (segmentation + depth), article "The algorithm" step 1: the any-clip mask's
  // Euclidean distance transform gives each clipped pixel its depth delta(x) (distance to the nearest
  // valid pixel); connected-component segmentation then groups clipped pixels into regions, each
  // carrying its reconstruction radius R = max delta over the region.
  //
  // Per-pixel reconstruction depth = distance from each clipped pixel to the nearest valid one
  // (Euclidean distance transform of the any-clip mask). A hole's reconstruction radius is the max
  // of this over the hole -- its true "reach needed", independent of the bbox shape.
  const size_t npix = (size_t)width * height;
  float *const restrict depth = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
  if(!depth)
  {
    err_code = 1;
    goto error;
  }
  uint8_t *const restrict maskb = (uint8_t *)dt_pixelpipe_cache_alloc_align(npix, pipe);
  if(!maskb)
  {
    dt_pixelpipe_cache_free_align(depth);
    err_code = 1;
    goto error;
  }
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < npix; i++)
  {
    // seed the distance transform: clipped pixels = +inf (to be filled with delta), valid = 0
    depth[i] = (clipping_mask[i * 4 + 3] > 0.5f) ? (float)DT_DISTANCE_TRANSFORM_MAX : 0.f;
    maskb[i] = (clipping_mask[i * 4 + 3] >= 1e-3f); // binary any-clip mask for the connected-component pass
  }
  dt_image_distance_transform(NULL, depth, width, height, 0.f,
                              DT_DISTANCE_TRANSFORM_NONE); // depth[] <- delta(x) (EDT)

  // Segment the clipped areas into connected regions and reconstruct each at full resolution with a
  // coarse->fine full-value guided filter (only clipped neighbourhoods are touched). Each region is
  // padded by its reconstruction radius (the deepest clip-to-valid distance), so the padding gives
  // the colour-line fit a valid rim as far out as the deepest pixel needs, and no farther.
  const dt_iop_highlights_data_t *const data = (const dt_iop_highlights_data_t *)piece->data;
  _hl_region_t *regions = NULL;
  // 8-neighbour connected components; pad = ceil(1.25 * R) clamped to [8, 256] px around each region
  const int nreg = _segment_clipped_regions(maskb, depth, width, height, 1.25f, 8, 256, &regions);


  // FLOW steps 3-8 (per region): reconstruct each connected clipped region on its padded window. Regions
  // are independent (their padded read boxes were merged when they overlapped, in _segment_clipped_regions),
  // so this loop is embarrassingly parallel across regions and linear in the total padded area.
  for(int region_index = 0; region_index < nreg; region_index++)
    _region_guided_filter(interpolated, clipping_mask, depth, width, &regions[region_index], pipe,
                          data->solid_color, data->iterations, data->noise_level, _hl_floor_gate(clips));


  free(regions);
  dt_pixelpipe_cache_free_align(maskb);
  dt_pixelpipe_cache_free_align(depth);

  // The composition reads `input` back for unmasked pixels, so the band correction must also go
  // through a corrected CFA copy -- otherwise the output band would keep the biased values the
  // reconstruction no longer agrees with (the seam would reappear at the detection contour).
  const float *remosaic_input = input;
  float *input_corr = NULL;

  if(knee_on)
  {
    input_corr = dt_pixelpipe_cache_alloc_align_float(size, pipe);

    if(!IS_NULL_PTR(input_corr))
    {
      _hl_knee_apply_cfa(input, input_corr, width, height, filters, roi_in, xtrans, knee_clipraw, knee);
      remosaic_input = input_corr;
    }
  }

  // FLOW: remosaic + composite (the flowchart's terminal node). Scatter the reconstructed RGB back onto
  // the CFA grid: out = opacity*rec + (1 - opacity)*base with opacity the binary any-clip mask, and
  // (clip_is_floor = TRUE here) base = max(raw, rec) on a clipped photosite -- so the reconstruction can
  // only lift a rolloff-biased sample toward its true level, never pull a valid one down. remosaic_input
  // is the knee-corrected CFA when the knee engaged (so unmasked pixels match the reconstruction's basis).
  // Second and last CFA-branching endpoint, mirroring the gather above.
  switch(cfa)
  {
    case HL_CFA_BAYER:
      _remosaic_and_replace(remosaic_input, input, interpolated, clipping_mask, output, normalization, clips,
                            TRUE, filters, width, height);
      break;
    case HL_CFA_XTRANS:
      _remosaic_and_replace_xtrans(remosaic_input, input, interpolated, clipping_mask, output, normalization,
                                   clips, TRUE, roi_in, xtrans, width, height);
      break;
    case HL_CFA_PASSTHROUGH:
      // Non-raw / sRAW: composite the reconstructed RGB straight back, per channel. remosaic_input ==
      // input here (the knee never ran, so no corrected CFA copy exists).
      _remosaic_and_replace_passthrough(remosaic_input, input, interpolated, clipping_mask, output,
                                        normalization, clips, TRUE, width, height);
      break;
  }

  if(!IS_NULL_PTR(input_corr)) dt_pixelpipe_cache_free_align(input_corr);

error:;
  dt_pixelpipe_cache_free_align(interpolated);
  dt_pixelpipe_cache_free_align(clipping_mask);
  _hl_gauss_cache_flush();
  (void)roi_out;
  return err_code;
}

// ============================ OpenCL ============================

#ifdef HAVE_OPENCL

// Shared host middle of the harmonic reconstruction: knee estimation/correction, distance
// transform, segmentation and the per-region rebuild -- everything between the gather and the
// remosaic, CFA-agnostic. Used by the OpenCL hybrid driver after its GPU gather; the CPU
// drivers keep their historical inline copies (same code, kept verbatim to avoid touching the
// validated path -- unify when the CPU drivers next change).
// On success *remosaic_input_out points to `input` or to a knee-corrected CFA copy
// (*input_corr_out, caller frees with dt_pixelpipe_cache_free_align).
//
// MATHS/PIPELINE BRIDGE -- the CPU "middle" of the OpenCL pipe (article §"The OpenCL pipe": GPU gather
// and GPU remosaic bracket a host middle). It runs the once-per-image steps BETWEEN the gather and the
// remosaic on host planes the GPU already produced: step 2 knee application (_hl_knee_apply_interpolated),
// step 1b depth + segmentation (distance transform + _segment_clipped_regions), then steps 3-8 per region
// via the CPU _region_guided_filter. Identical code to process_harmonic's middle (kept as a
// separate copy to avoid touching the validated CPU driver); it is the fallback the device middle
// (_harmonic_reconstruct_cl) drops to when the GPU middle cannot run.
__DT_CLONE_TARGETS__
static int _harmonic_reconstruct_host(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                      const dt_dev_pixelpipe_iop_t *piece, const float *const restrict input,
                                      float *const restrict interpolated, float *const restrict clipping_mask,
                                      const dt_iop_roi_t *const roi_in, const dt_aligned_pixel_t clips,
                                      const dt_aligned_pixel_t normalization, const float **remosaic_input_out,
                                      float **input_corr_out, const _hl_knee_curve_t knee_pre[3])
{
  // _hl_knee_apply_cfa below reads FC(row, col, filters) with tile-local row/col, so filters
  // must be pre-shifted for roi_in's crop position (mirrors process_harmonic).
  const uint32_t filters = dt_dev_get_roi_filters(piece, roi_in);
  const uint8_t(*const xtrans)[6] = (filters == 9u) ? (const uint8_t(*const)[6])piece->dsc_in.xtrans : NULL;
  const size_t width = roi_in->width;
  const size_t height = roi_in->height;
  const size_t size = width * height;

  *remosaic_input_out = input;
  *input_corr_out = NULL;

  // the knee was estimated by the caller BEFORE the gather (its engagement drives the band
  // override of the detection); reuse the curves here
  _hl_knee_curve_t knee[3];
  memcpy(knee, knee_pre, sizeof(knee));
  dt_aligned_pixel_t clipvaln = { 1.f, 1.f, 1.f, 1.f };
  dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
  for(int c = 0; c < 3; c++)
  {
    clipvaln[c] = clips[c] / (DT_HL_KNEE_DET * fmaxf(normalization[c], 1e-9f));
    knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;
  }
  const int knee_on = knee[0].engaged || knee[1].engaged || knee[2].engaged;

  if(knee_on) _hl_knee_apply_interpolated(interpolated, size, clipvaln, normalization, knee);

  const size_t npix = size;
  float *const restrict depth = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
  if(!depth) return 1;
  uint8_t *const restrict maskb = (uint8_t *)dt_pixelpipe_cache_alloc_align(npix, pipe);
  if(!maskb)
  {
    dt_pixelpipe_cache_free_align(depth);
    return 1;
  }
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < npix; i++)
  {
    depth[i] = (clipping_mask[i * 4 + 3] > 0.5f) ? (float)DT_DISTANCE_TRANSFORM_MAX : 0.f;
    maskb[i] = (clipping_mask[i * 4 + 3] >= 1e-3f);
  }
  dt_image_distance_transform(NULL, depth, width, height, 0.f, DT_DISTANCE_TRANSFORM_NONE);

  const dt_iop_highlights_data_t *const data = (const dt_iop_highlights_data_t *)piece->data;
  _hl_region_t *regions = NULL;
  const int nreg = _segment_clipped_regions(maskb, depth, width, height, 1.25f, 8, 256, &regions);

  // FLOW steps 3-8 (per region): CPU per-region reconstruction, same call as the CPU drivers.
  for(int region_index = 0; region_index < nreg; region_index++)
    _region_guided_filter(interpolated, clipping_mask, depth, width, &regions[region_index], pipe,
                          data->solid_color, data->iterations, data->noise_level, _hl_floor_gate(clips));


  free(regions);
  dt_pixelpipe_cache_free_align(maskb);
  dt_pixelpipe_cache_free_align(depth);

  if(knee_on)
  {
    float *input_corr = dt_pixelpipe_cache_alloc_align_float(size, pipe);
    if(!IS_NULL_PTR(input_corr))
    {
      _hl_knee_apply_cfa(input, input_corr, width, height, filters, roi_in, xtrans, knee_clipraw, knee);
      *remosaic_input_out = input_corr;
      *input_corr_out = input_corr;
    }
  }

  _hl_gauss_cache_flush();
  return 0;
}

#define HL_CL_RELEASE(mem_obj)                                                                                    \
  do                                                                                                              \
  {                                                                                                               \
    dt_opencl_release_mem_object(mem_obj);                                                                        \
    (mem_obj) = NULL;                                                                                             \
  } while(0)

// Harmonic transposition on an OpenCL pipe: hybrid CPU-orchestrated, stage 1.
// The reconstruction's heart is CPU by design (sparse Cholesky factorizations, per-region
// segmentation and orchestration), so the module roundtrips the single-channel raw through
// the host and runs the exact CPU pipeline -- the output is BIT-IDENTICAL to the CPU path
// by construction, and the pipe keeps its CL chain (up/downstream modules stay on the GPU,
// no scheduler-level fallback). Stage 2 (planned) slots GPU kernels into this driver where
// they pay: the gather/remosaic kernels already exist from the a-trous path, and the
// region moment blurs + harmonic fills are the dominant remaining cost -- at the price of
// bit-identity with the CPU, so it must go through the full validation protocol.
// Stage-1 fallback: full host roundtrip running the exact CPU driver (bit-identical to the
// CPU pipe by construction). Used when any GPU gather/remosaic step fails.
//
// PIPELINE BRIDGE (article §"The OpenCL pipe", the bit-identical fallback): the single-channel raw
// crosses the bus ONCE to the host (dt_opencl_copy_device_to_host), the whole CPU driver
// (process_harmonic -- all 8 steps, gather through remosaic, CFA selected internally) runs on it, and the
// result is written back once (dt_opencl_write_host_to_device). No GPU kernels of this mode are used, so
// the output
// is byte-for-byte the CPU path; the surrounding pipe still stays on the GPU (no scheduler-level fallback).
static cl_int _harmonic_cl_roundtrip(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                     const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                     const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                     const dt_aligned_pixel_t clips)
{
  const int devid = pipe->devid;
  const size_t n_in = (size_t)roi_in->width * roi_in->height;
  const size_t n_out = (size_t)roi_out->width * roi_out->height;

  float *host_in = dt_pixelpipe_cache_alloc_align_float(n_in, pipe);
  float *host_out = dt_pixelpipe_cache_alloc_align_float(n_out, pipe);
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;

  if(IS_NULL_PTR(host_in) || IS_NULL_PTR(host_out)) goto error;

  // bus crossing 1/2: pull the raw mosaic down to the host
  cl_err = dt_opencl_copy_device_to_host(devid, host_in, dev_in, roi_in->width, roi_in->height, sizeof(float));
  if(cl_err != CL_SUCCESS) goto error;

  // run the exact CPU driver (all 8 steps, CFA selected internally) on the host copy -> bit-identical to
  // the CPU pipe
  if(process_harmonic(self, pipe, piece, host_in, host_out, roi_in, roi_out, clips))
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto error;
  }

  // bus crossing 2/2: push the reconstructed CFA back to the device
  cl_err
      = dt_opencl_write_host_to_device(devid, host_out, dev_out, roi_out->width, roi_out->height, sizeof(float));

error:
  dt_pixelpipe_cache_free_align(host_in);
  dt_pixelpipe_cache_free_align(host_out);
  return cl_err;
}

// Stage 2: the gather (normalization reduce, bilinear interpolation + clip mask, mask
// feathering) and the scatter (remosaic) run on the GPU with the kernels shared with the
// a-trous path; the reconstruction middle (knee, segmentation, regions -- the solvers are CPU
// by design) runs on downloaded host planes. Any GPU failure falls back to the stage-1
// roundtrip above.

// GPU middle of the harmonic pipeline: knee estimation + application, segmentation support
// (byte masks down, depth up -- the EDT and flood fill stay on the host, exact), and the
// per-region reconstruction, all on device buffers. Returns CL_SUCCESS when the whole middle
// ran on the GPU; any failure leaves the caller to run the host middle instead. corr_out
// receives the knee-corrected 1-channel CFA buffer when the knee engages (caller releases).
//
// PIPELINE BRIDGE (article §"The OpenCL pipe", the device-resident middle): the once-per-image steps
// run on device buffers -- step 2 knee, step 1b segmentation SUPPORT (only the byte seed/member masks
// come down and the depth plane goes up; the Euclidean distance transform and connected-component flood
// fill stay on the host because they are inherently serial), then steps 3-8 per region. The per-region
// loop ROUTES each region by size: big regions stay device-resident (_region_guided_filter_cl); regions
// at or below DT_HL_CL_CPU_REGION_PX cross the bus once and take the CPU path (_region_cpu_offload_cl),
// because a device region pays ~1000 kernel launches that a small hole cannot amortize. Only byte masks,
// the depth plane and small reduction partials ever cross the bus.
static cl_int _harmonic_reconstruct_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                       const dt_dev_pixelpipe_iop_t *piece, cl_mem raw_buf, cl_mem interp_buf,
                                       cl_mem mask_buf, cl_mem *corr_out, const dt_iop_roi_t *const roi_in,
                                       const dt_aligned_pixel_t clips, const dt_aligned_pixel_t norm,
                                       cl_mem dev_xtrans, const _hl_knee_curve_t knee_pre[3])
{
  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)self->global_data;
  const dt_iop_highlights_data_t *const data = (const dt_iop_highlights_data_t *)piece->data;
  const int devid = pipe->devid;
  const uint32_t filters = piece->dsc_in.filters;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t npix = (size_t)width * height;
  const int is_xtrans = (filters == 9u);
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;

  if(data->noise_level > 0.f) return cl_err; // grain epilogue is not ported

  size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem seed = NULL;
  cl_mem member = NULL;
  cl_mem depth_dev = NULL;
  cl_mem corr = NULL;
  uint8_t *h_seed = NULL;
  uint8_t *h_member = NULL;
  float *depth = NULL;
  _hl_region_t *regions = NULL;
  *corr_out = NULL;

  {
    // the knee was estimated by the caller BEFORE the gather (its engagement drives the band
    // override of the detection); reuse the curves here
    _hl_knee_curve_t knee[3];
    memcpy(knee, knee_pre, sizeof(knee));
    dt_aligned_pixel_t clipvaln = { 1.f, 1.f, 1.f, 1.f };
    dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
    for(int c = 0; c < 3; c++)
    {
      clipvaln[c] = clips[c] / (DT_HL_KNEE_DET * fmaxf(norm[c], 1e-9f));
      knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;
    }
    const int knee_on = knee[0].engaged || knee[1].engaged || knee[2].engaged;

    if(knee_on)
    {
      // band correction on the interpolated RGBN planes (reconstruction fits unbiased data)
      float lift[3 * DT_HL_KNEE_BINS];
      for(int c = 0; c < 3; c++) memcpy(lift + c * DT_HL_KNEE_BINS, knee[c].lift, sizeof(knee[c].lift));
      cl_mem dev_lift = _sp_cl_upload(devid, lift, sizeof(lift));
      if(!dev_lift)
      {
        cl_err = DT_OPENCL_DEFAULT_ERROR;
        goto out;
      }
      const int kernel = global_data->kernel_hl_knee_apply_interp;
      const cl_float4 clip4 = { { clipvaln[0], clipvaln[1], clipvaln[2], 1.f } };
      const cl_float4 wb4 = { { norm[0], norm[1], norm[2], 1.f } };
      const cl_int4 engaged_flags = { { knee[0].engaged, knee[1].engaged, knee[2].engaged, 0 } };
      const float knee_lo = DT_HL_KNEE_LO;
      const float knee_det = DT_HL_KNEE_DET;
      const int bins = DT_HL_KNEE_BINS;
      dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &interp_buf);
      dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(int), &width);
      dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &height);
      dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_float4), &clip4);
      dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_float4), &wb4);
      dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(cl_mem), &dev_lift);
      dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(cl_int4), &engaged_flags);
      dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(float), &knee_lo);
      dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(float), &knee_det);
      dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(int), &bins);
      cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
      dt_opencl_release_mem_object(dev_lift);
      if(cl_err != CL_SUCCESS) goto out;

      // corrected CFA copy for the remosaic composition
      corr = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
      if(!corr)
      {
        cl_err = DT_OPENCL_DEFAULT_ERROR;
        goto out;
      }
      cl_err = _hl_knee_apply_cfa_cl(devid, global_data, raw_buf, corr, width, height, filters, roi_in, dev_xtrans,
                                     is_xtrans, knee_clipraw, knee);
      if(cl_err != CL_SUCCESS) goto out;
    }
  }

  // ---- segmentation support: byte masks down, exact host EDT + flood fill, depth up ----
  seed = dt_opencl_alloc_device_buffer(devid, npix);
  member = dt_opencl_alloc_device_buffer(devid, npix);
  h_seed = (uint8_t *)dt_pixelpipe_cache_alloc_align(npix, pipe);
  h_member = (uint8_t *)dt_pixelpipe_cache_alloc_align(npix, pipe);
  depth = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
  if(!seed || !member || !h_seed || !h_member || !depth)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  {
    const int kernel = global_data->kernel_hl_mask_pack;
    dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &mask_buf);
    dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &seed);
    dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &member);
    dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(int), &height);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
    if(cl_err != CL_SUCCESS) goto out;
  }
  cl_err = dt_opencl_read_buffer_from_device(devid, h_seed, seed, 0, npix, CL_TRUE);
  if(cl_err == CL_SUCCESS) cl_err = dt_opencl_read_buffer_from_device(devid, h_member, member, 0, npix, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < npix; i++) depth[i] = h_seed[i] ? (float)DT_DISTANCE_TRANSFORM_MAX : 0.f;
  dt_image_distance_transform(NULL, depth, width, height, 0.f, DT_DISTANCE_TRANSFORM_NONE);

  const int nreg = _segment_clipped_regions(h_member, depth, width, height, 1.25f, 8, 256, &regions);

  depth_dev = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
  if(!depth_dev)
  {
    cl_err = DT_OPENCL_DEFAULT_ERROR;
    goto out;
  }
  cl_err = dt_opencl_write_buffer_to_device(devid, depth, depth_dev, 0, sizeof(float) * npix, CL_TRUE);
  if(cl_err != CL_SUCCESS) goto out;

  {
    // ROUTING THRESHOLD (article §"The OpenCL pipe"): DT_HL_CL_CPU_REGION_PX (~1 Mpx padded window),
    // env-overridable for tuning. It is the padded-window pixel count above which the ~1000-launch GPU
    // per-region path pays off; below it, one bus crossing to the CPU is cheaper.
    size_t cpu_px = DT_HL_CL_CPU_REGION_PX;
    const char *override_env = getenv("HL_CL_CPU_PX");
    if(override_env) cpu_px = (size_t)strtoull(override_env, NULL, 10);
    for(int region_index = 0; region_index < nreg && cl_err == CL_SUCCESS; region_index++)
    {
      // routing key = the PADDED read-window area (rx0..rx1 x ry0..ry1), the same window either path
      // reconstructs -- not the bare clipped bbox
      const size_t region_px = (size_t)(regions[region_index].rx1 - regions[region_index].rx0 + 1)
                               * (size_t)(regions[region_index].ry1 - regions[region_index].ry0 + 1);
      if(region_px <= cpu_px)
      {
        // small region: cross the bus once and reconstruct on the CPU (bit-identical to the CPU driver)
        cl_err = _region_cpu_offload_cl(devid, global_data, interp_buf, mask_buf, depth_dev, width,
                                        &regions[region_index], pipe, data->solid_color, data->iterations,
                                        data->noise_level, _hl_floor_gate(clips));
      }
      else
        // big region: stay device-resident for the whole rebuild
        cl_err = _region_guided_filter_cl(devid, global_data, interp_buf, mask_buf, depth_dev, width,
                                          &regions[region_index], pipe, data->solid_color, _hl_floor_gate(clips));
    }
  }

  if(cl_err == CL_SUCCESS)
  {
    dt_opencl_finish(devid);
  }

out:
  _hl_gauss_cache_flush(); // the CPU-offloaded regions run _region_blur on this thread
  dt_opencl_release_mem_object(seed);
  dt_opencl_release_mem_object(member);
  dt_opencl_release_mem_object(depth_dev);
  dt_pixelpipe_cache_free_align(h_seed);
  dt_pixelpipe_cache_free_align(h_member);
  dt_pixelpipe_cache_free_align(depth);
  free(regions);
  if(cl_err == CL_SUCCESS)
    *corr_out = corr;
  else
    dt_opencl_release_mem_object(corr);
  return cl_err;
}

cl_int process_harmonic_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                           const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                           const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                           const dt_aligned_pixel_t clips)
{
  _sp_chol_cl_selftest(pipe->devid, self->global_data, pipe);
  _region_blur_cl_selftest(pipe->devid, pipe);
  _cf_harmonic_fill_cl_selftest(pipe->devid, self->global_data, pipe);
  _cf_joint_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _cf_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _hf_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _selfdome_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _joint_core_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _knee_cl_selftest(pipe->devid, self->global_data, pipe);
  _aniso_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _chromaticity_gradient_stage_cl_selftest(pipe->devid, self->global_data, pipe);
  _region_guided_filter_cl_selftest(pipe->devid, self->global_data, pipe);

  dt_iop_highlights_global_data_t *global_data = (dt_iop_highlights_global_data_t *)self->global_data;
  const int devid = pipe->devid;
  // _hl_knee_estimate_cl and the hl_knee_* kernels are self-correcting: they take this raw
  // filters value PLUS roi_in->x/y as separate kernel args and add them themselves. The shared
  // interpolate_and_mask/remosaic_and_replace Bayer kernels (and the host-side
  // _compute_laplacian_normalization call below) have no roi offset arg at all -- they need
  // filters pre-shifted for roi_in's crop position instead (mirrors the CPU driver's fix).
  const uint32_t filters = piece->dsc_in.filters;
  const uint32_t filters_shifted = dt_dev_get_roi_filters(piece, roi_in);
  const int width = roi_in->width;
  const int height = roi_in->height;
  const size_t npix = (size_t)width * height;
  const int is_xtrans = (filters == 9u);
  // Non-raw / sRAW passthrough: already-demosaiced 4-channel RGB input. The gather is a device plane
  // copy, the remosaic a per-channel composite, and the knee is disabled (it would misread a 4-channel
  // buffer as a mosaic) -- which also makes the reconstruction middle fully CFA-agnostic and reusable.
  const int is_passthrough = (filters == 0u);
  const size_t in_channels = is_passthrough ? 4 : 1; // channel count of dev_in for the host download below

  size_t sizes[] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };

  cl_mem interpolated = NULL;
  cl_mem clipping_mask = NULL;
  cl_mem temp = NULL;
  cl_mem clips_cl = NULL;
  cl_mem normalization_final = NULL;
  cl_mem dev_xtrans = NULL;
  cl_mem lookup_cl = NULL;
  cl_mem corr_cl = NULL;
  cl_mem det_clips_cl = NULL;
  float *h_interp = NULL;
  float *h_mask = NULL;
  float *h_raw = NULL;
  float *input_corr = NULL;
  const float *remosaic_input = NULL;
  cl_int cl_err = DT_OPENCL_DEFAULT_ERROR;

  interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
  clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), (float *)clips);
  if(IS_NULL_PTR(interpolated) || IS_NULL_PTR(clipping_mask) || IS_NULL_PTR(temp) || IS_NULL_PTR(clips_cl))
    goto fallback;

  if(is_xtrans)
  {
    dev_xtrans = dt_opencl_copy_host_to_device_constant(devid, sizeof(piece->dsc_in.xtrans),
                                                        (void *)piece->dsc_in.xtrans);
    int32_t lookup[6][6][32] = { { { 0 } } };
    _build_xtrans_bilinear_lookup(lookup, roi_in, (const uint8_t(*const)[6])piece->dsc_in.xtrans);
    lookup_cl = dt_opencl_copy_host_to_device_constant(devid, sizeof(lookup), lookup);
    if(IS_NULL_PTR(dev_xtrans) || IS_NULL_PTR(lookup_cl)) goto fallback;
  }

  // ---- per-channel normalization: computed on the HOST with the exact CPU function ----
  // The raw is needed on the host anyway (knee estimation reads the mosaic), and the GPU
  // max-reduce kernels are not bit-faithful to _compute_laplacian_normalization: the tiny
  // normalization difference shifted the clip mask by a few hundred pixels and the whole
  // reconstruction with it. Downloading first keeps the mask identical to the CPU path.
  h_raw = dt_pixelpipe_cache_alloc_align_float(npix * in_channels, pipe);
  if(IS_NULL_PTR(h_raw)) goto fallback;
  cl_err = dt_opencl_copy_device_to_host(devid, h_raw, dev_in, width, height, in_channels * sizeof(float));
  if(cl_err != CL_SUCCESS) goto fallback;

  // filters_shifted is 0 for the passthrough case -> _compute_laplacian_normalization reads h_raw as a
  // 4-channel RGB buffer (its filters==0 branch), matching the CPU passthrough path exactly.
  dt_aligned_pixel_t norm_host = { 1.f, 1.f, 1.f, 1.f };
  _compute_laplacian_normalization(h_raw, roi_in, filters_shifted,
                                   is_xtrans ? (const uint8_t(*const)[6])piece->dsc_in.xtrans : NULL, norm_host);
  normalization_final = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), norm_host);
  if(IS_NULL_PTR(normalization_final)) goto fallback;

  // ---- rolloff estimation FIRST (raw-based): its per-channel engagement drives the band
  //      override of the detection thresholds, exactly like the CPU drivers ----
  // Zero-initialized so the passthrough path (which skips estimation) leaves every channel disengaged;
  // the knee is meaningless on already-demosaiced RGB and reads the raw as a mosaic, so it never runs.
  _hl_knee_curve_t knee[3] = { 0 };
  if(!is_passthrough)
  {
    dt_aligned_pixel_t knee_clipraw = { 1.f, 1.f, 1.f, 1.f };
    for(int c = 0; c < 3; c++) knee_clipraw[c] = clips[c] / DT_HL_KNEE_DET;

    // the knee kernels read the raw as a BUFFER; dev_in is an image2d -> copy first
    cl_mem knee_raw = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
    if(IS_NULL_PTR(knee_raw)) goto fallback;
    size_t korigin[3] = { 0, 0, 0 };
    size_t kregion[3] = { (size_t)width, (size_t)height, 1 };
    cl_err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, knee_raw, korigin, kregion, 0);
    if(cl_err != CL_SUCCESS)
    {
      dt_opencl_release_mem_object(knee_raw);
      goto fallback;
    }

    cl_err = _hl_knee_estimate_cl(devid, global_data, knee_raw, width, height, filters, roi_in, dev_xtrans,
                                  is_xtrans, knee_clipraw, knee, pipe);
    dt_opencl_release_mem_object(knee_raw);
    if(cl_err != CL_SUCCESS) goto fallback;
  }

  dt_aligned_pixel_t eff_clips;
  for_four_channels(c) eff_clips[c] = clips[c];
  for(int c = 0; c < 3; c++)
    if(knee[c].engaged) eff_clips[c] = clips[c] * DT_HL_BAND_OVR;
  det_clips_cl = dt_opencl_copy_host_to_device_constant(devid, 4 * sizeof(float), eff_clips);
  if(IS_NULL_PTR(det_clips_cl)) goto fallback;

  // ---- gather: bilinear interpolation + clip mask, then 5x5 feathering ----
  if(is_xtrans)
  {
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 0, sizeof(cl_mem),
                             &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 1, sizeof(cl_mem),
                             &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 2, sizeof(cl_mem),
                             &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 3, sizeof(cl_mem),
                             &det_clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 4, sizeof(cl_mem),
                             &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 5, sizeof(int),
                             &width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 6, sizeof(int),
                             &height);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 7, sizeof(int),
                             &roi_in->x);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 8, sizeof(int),
                             &roi_in->y);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 9, sizeof(cl_mem),
                             &dev_xtrans);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, 10, sizeof(cl_mem),
                             &lookup_cl);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, global_data->kernel_highlights_bilinear_and_mask_xtrans, sizes);
  }
  else if(is_passthrough)
  {
    // Non-raw / sRAW: plane copy + per-channel clip mask, written straight into clipping_mask (harmonic
    // masks stay binary -- no 5x5 feathering). The knee is off, so det_clips_cl holds the plain clips.
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_passthrough, 0,
                             sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_passthrough, 1,
                             sizeof(cl_mem), &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_passthrough, 2,
                             sizeof(cl_mem), &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_passthrough, 3,
                             sizeof(cl_mem), &det_clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_passthrough, 4,
                             sizeof(cl_mem), &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_passthrough, 5, sizeof(int),
                             &width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask_passthrough, 6, sizeof(int),
                             &height);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, global_data->kernel_highlights_bilinear_and_mask_passthrough,
                                         sizes);
  }
  else
  {
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 1, sizeof(cl_mem),
                             &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 2, sizeof(cl_mem),
                             &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 3, sizeof(cl_mem),
                             &det_clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 4, sizeof(cl_mem),
                             &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 5, sizeof(int),
                             &filters_shifted);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 6, sizeof(int),
                             &roi_out->width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_bilinear_and_mask, 7, sizeof(int),
                             &roi_out->height);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, global_data->kernel_highlights_bilinear_and_mask, sizes);
  }
  if(cl_err != CL_SUCCESS) goto fallback;

  // ---- GPU middle first: knee + segmentation support + per-region reconstruction on device
  //      buffers; only byte masks, the depth plane and reduction partials cross the bus ----
  {
    cl_mem raw_buf = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix);
    cl_mem interp_buf = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix * 4);
    cl_mem mask_buf = dt_opencl_alloc_device_buffer(devid, sizeof(float) * npix * 4);
    cl_mem corr_buf = NULL;
    size_t origin[3] = { 0, 0, 0 };
    size_t region1[3] = { (size_t)width, (size_t)height, 1 };
    cl_int gpu_err = (raw_buf && interp_buf && mask_buf) ? CL_SUCCESS : DT_OPENCL_DEFAULT_ERROR;
    // raw_buf holds the single-channel raw mosaic for the knee; the passthrough path has no knee and no
    // mosaic (dev_in is 4-channel), so skip this copy -- the middle never reads raw_buf when knee is off.
    if(gpu_err == CL_SUCCESS && !is_passthrough)
      gpu_err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, raw_buf, origin, region1, 0);
    if(gpu_err == CL_SUCCESS)
      gpu_err = dt_opencl_enqueue_copy_image_to_buffer(devid, interpolated, interp_buf, origin, region1, 0);
    if(gpu_err == CL_SUCCESS)
      gpu_err = dt_opencl_enqueue_copy_image_to_buffer(devid, clipping_mask, mask_buf, origin, region1, 0);
    int staged = 0; // 1 = the three images below were released and must be re-created
    if(gpu_err == CL_SUCCESS)
    {
      // the middle works on the buffers: release the three full-image images (~1.7 GB on a
      // 36 Mpx raw) so the region planes and stage temporaries fit in vRAM; the two that are
      // consumed downstream are re-created from the buffers right after. The HL_MIDDLE_AB
      // diagnostic keeps them alive instead: its reference run needs the PRISTINE planes.
      dt_opencl_finish(devid); // the async image->buffer copies must land first
      if(!getenv("HL_MIDDLE_AB"))
      {
        HL_CL_RELEASE(temp);
        HL_CL_RELEASE(interpolated);
        HL_CL_RELEASE(clipping_mask);
        staged = 1;
      }
      // preference 1: the device-resident middle (steps 2 + 1b + 3-8 on device, small regions offloaded)
      gpu_err = _harmonic_reconstruct_cl(self, pipe, piece, raw_buf, interp_buf, mask_buf, &corr_buf, roi_in,
                                         clips, norm_host, dev_xtrans, knee);
    }
    // materialize the knee-corrected mosaic BEFORE restoring the working images: a failure
    // here must take the same pristine re-gather road as a mid-middle failure. Falling
    // through with the reconstruction already copied back would hand the host middle
    // knee-lifted, partially reconstructed planes -- the knee would be applied twice and
    // the regions re-solved on reconstructed anchors, silently.
    if(gpu_err == CL_SUCCESS && corr_buf)
    {
      corr_cl = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float));
      if(corr_cl)
        gpu_err = dt_opencl_enqueue_copy_buffer_to_image(devid, corr_buf, corr_cl, 0, origin, region1);
      else
        gpu_err = DT_OPENCL_DEFAULT_ERROR;
      if(gpu_err != CL_SUCCESS) HL_CL_RELEASE(corr_cl);
    }
    // HL_MIDDLE_AB=1 (diagnostic): run the host middle on the pristine planes (kept alive
    // above) and print its divergence from the device middle; the device result still ships
    if(gpu_err == CL_SUCCESS && getenv("HL_MIDDLE_AB"))
    {
      float *gpu_interp = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
      float *host_interp = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
      float *host_mask = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
      float *host_raw = dt_pixelpipe_cache_alloc_align_float(npix, pipe);
      if(gpu_interp && host_interp && host_mask && host_raw
         && dt_opencl_read_buffer_from_device(devid, gpu_interp, interp_buf, 0, sizeof(float) * npix * 4, CL_TRUE)
                == CL_SUCCESS
         && dt_opencl_copy_device_to_host(devid, host_interp, interpolated, width, height, sizeof(float) * 4)
                == CL_SUCCESS
         && dt_opencl_copy_device_to_host(devid, host_mask, clipping_mask, width, height, sizeof(float) * 4)
                == CL_SUCCESS
         && dt_opencl_copy_device_to_host(devid, host_raw, dev_in, width, height, sizeof(float)) == CL_SUCCESS)
      {
        const float *remosaic_ptr = NULL;
        float *input_corr_ab = NULL;
        if(!_harmonic_reconstruct_host(self, pipe, piece, host_raw, host_interp, host_mask, roi_in, clips,
                                       norm_host, &remosaic_ptr, &input_corr_ab, knee))
        {
          float max_diff = 0.f;
          double sum_diff = 0.0;
          size_t arg_index = 0;
          for(size_t i = 0; i < npix * 4; i++)
          {
            const float diff = fabsf(gpu_interp[i] - host_interp[i]);
            if(diff > max_diff)
            {
              max_diff = diff;
              arg_index = i;
            }
            sum_diff += (double)diff;
          }
          fprintf(stderr, "[hl middle AB] max=%.3e mean=%.3e at px=(%llu,%llu) c=%llu gpu=%f cpu=%f\n", max_diff,
                  sum_diff / (double)(npix * 4), (unsigned long long)((arg_index / 4) % width),
                  (unsigned long long)((arg_index / 4) / width), (unsigned long long)(arg_index % 4),
                  gpu_interp[arg_index], host_interp[arg_index]);
        }
        dt_pixelpipe_cache_free_align(input_corr_ab);
      }
      dt_pixelpipe_cache_free_align(gpu_interp);
      dt_pixelpipe_cache_free_align(host_interp);
      dt_pixelpipe_cache_free_align(host_mask);
      dt_pixelpipe_cache_free_align(host_raw);
    }
    // restore the images for the downstream blend/remosaic -- ONLY when they were released:
    // an early staging failure leaves them alive and pristine, and reallocating over the
    // live handles would leak them (~1.7 GB) while doubling vRAM demand under the very
    // pressure that made staging fail. On success, interpolated carries
    // the reconstruction and the mask is untouched (buffer copy-back). On FAILURE the middle
    // may have partially scattered and knee-lifted interp_buf, so the host fallback must NOT
    // reuse it: re-run the interpolation + mask blur from the still-alive inputs instead.
    if(staged)
    {
      interpolated = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
      clipping_mask = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
      cl_int restore_err = (interpolated && clipping_mask) ? CL_SUCCESS : DT_OPENCL_DEFAULT_ERROR;
      if(restore_err == CL_SUCCESS && gpu_err == CL_SUCCESS)
      {
        restore_err = dt_opencl_enqueue_copy_buffer_to_image(devid, interp_buf, interpolated, 0, origin, region1);
        if(restore_err == CL_SUCCESS)
          restore_err = dt_opencl_enqueue_copy_buffer_to_image(devid, mask_buf, clipping_mask, 0, origin, region1);
      }
      else if(restore_err == CL_SUCCESS)
      {
        temp = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float) * 4);
        if(!temp) restore_err = DT_OPENCL_DEFAULT_ERROR;
        if(restore_err == CL_SUCCESS)
        {
          if(is_xtrans)
          {
            const int kernel = global_data->kernel_highlights_bilinear_and_mask_xtrans;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &interpolated);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &temp);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &clips_cl);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &normalization_final);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &roi_out->width);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &roi_out->height);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &roi_in->x);
            dt_opencl_set_kernel_arg(devid, kernel, 8, sizeof(int), &roi_in->y);
            dt_opencl_set_kernel_arg(devid, kernel, 9, sizeof(cl_mem), &dev_xtrans);
            dt_opencl_set_kernel_arg(devid, kernel, 10, sizeof(cl_mem), &lookup_cl);
            restore_err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
          }
          else
          {
            const int kernel = global_data->kernel_highlights_bilinear_and_mask;
            dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &dev_in);
            dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &interpolated);
            dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(cl_mem), &temp);
            dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(cl_mem), &clips_cl);
            dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), &normalization_final);
            dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(int), &filters_shifted);
            dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(int), &roi_out->width);
            dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(int), &roi_out->height);
            restore_err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
          }
        }
        if(restore_err == CL_SUCCESS)
        {
          const int kernel = global_data->kernel_highlights_box_blur;
          dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), &temp);
          dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), &clipping_mask);
          dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), &roi_out->width);
          dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), &roi_out->height);
          restore_err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
        }
      }
      if(restore_err != CL_SUCCESS)
      {
        dt_opencl_release_mem_object(raw_buf);
        dt_opencl_release_mem_object(interp_buf);
        dt_opencl_release_mem_object(mask_buf);
        dt_opencl_release_mem_object(corr_buf);
        cl_err = restore_err;
        goto fallback;
      }
    }
    else if(gpu_err == CL_SUCCESS)
    {
      // diagnostic mode kept the images alive: publish the device result into them now
      cl_int restore_err
          = dt_opencl_enqueue_copy_buffer_to_image(devid, interp_buf, interpolated, 0, origin, region1);
      if(restore_err == CL_SUCCESS)
        restore_err = dt_opencl_enqueue_copy_buffer_to_image(devid, mask_buf, clipping_mask, 0, origin, region1);
      if(restore_err != CL_SUCCESS)
      {
        dt_opencl_release_mem_object(raw_buf);
        dt_opencl_release_mem_object(interp_buf);
        dt_opencl_release_mem_object(mask_buf);
        dt_opencl_release_mem_object(corr_buf);
        cl_err = restore_err;
        goto fallback;
      }
    }
    dt_opencl_release_mem_object(raw_buf);
    dt_opencl_release_mem_object(interp_buf);
    dt_opencl_release_mem_object(mask_buf);
    dt_opencl_release_mem_object(corr_buf);
    if(gpu_err == CL_SUCCESS) goto remosaic; // device middle succeeded -> straight to the GPU remosaic
    // GPU middle unavailable (fp64 device, grain requested, oversized hole...): host middle below
    HL_CL_RELEASE(corr_cl);
    dt_print(DT_DEBUG_OPENCL, "[opencl_highlights] harmonic GPU middle failed (%i), using the host middle\n",
             gpu_err);
  }

  // ---- host-middle path (preference 2): the GPU gather succeeded but the device middle could not run.
  //      Pull the gathered working planes down (the raw h_raw is already resident) so the CPU middle can
  //      run the same steps 2 + 1b + 3-8 it would on a CPU pipe ----
  h_interp = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
  h_mask = dt_pixelpipe_cache_alloc_align_float(npix * 4, pipe);
  if(IS_NULL_PTR(h_interp) || IS_NULL_PTR(h_mask)) goto fallback;

  cl_err = dt_opencl_copy_device_to_host(devid, h_interp, interpolated, width, height, sizeof(float) * 4);
  if(cl_err != CL_SUCCESS) goto fallback;
  cl_err = dt_opencl_copy_device_to_host(devid, h_mask, clipping_mask, width, height, sizeof(float) * 4);
  if(cl_err != CL_SUCCESS) goto fallback;

  // ---- CPU middle: knee, segmentation, per-region reconstruction ----
  if(_harmonic_reconstruct_host(self, pipe, piece, h_raw, h_interp, h_mask, roi_in, clips, norm_host,
                                &remosaic_input, &input_corr, knee))
    goto fallback;

  // ---- upload the reconstructed planes back to the device so the GPU remosaic below closes the pipe ----
  cl_err = dt_opencl_write_host_to_device(devid, h_interp, interpolated, width, height, sizeof(float) * 4);
  if(cl_err != CL_SUCCESS) goto fallback;

remosaic:;
  // FLOW: GPU remosaic + composite (the pipe's terminal node, reached from either middle). Pick the base
  // CFA the composite reads on unmasked sites: the knee-corrected copy (corr_cl / input_corr) when the
  // knee engaged, else the pristine dev_in -- so valid pixels match the reconstruction's basis.
  cl_mem remosaic_in_cl = dev_in;
  if(corr_cl)
    remosaic_in_cl = corr_cl;
  else if(remosaic_input != h_raw && remosaic_input != NULL)
  {
    corr_cl = dt_opencl_alloc_device(devid, sizes[0], sizes[1], sizeof(float));
    if(IS_NULL_PTR(corr_cl)) goto fallback;
    cl_err = dt_opencl_write_host_to_device(devid, input_corr, corr_cl, width, height, sizeof(float));
    if(cl_err != CL_SUCCESS) goto fallback;
    remosaic_in_cl = corr_cl;
  }

  if(is_xtrans)
  {
    const int clip_floor_on = TRUE; // clipped raw values are floors, never blend targets
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 0, sizeof(cl_mem),
                             &remosaic_in_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 1, sizeof(cl_mem),
                             &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 2, sizeof(cl_mem),
                             &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 3, sizeof(cl_mem),
                             &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 4, sizeof(cl_mem),
                             &dev_out);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 5, sizeof(cl_mem),
                             &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 6, sizeof(cl_mem),
                             &clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 7, sizeof(int),
                             &clip_floor_on);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 8, sizeof(int),
                             &width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 9, sizeof(int),
                             &height);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 10, sizeof(int),
                             &roi_in->x);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 11, sizeof(int),
                             &roi_in->y);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, 12, sizeof(cl_mem),
                             &dev_xtrans);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, global_data->kernel_highlights_remosaic_and_replace_xtrans, sizes);
  }
  else if(is_passthrough)
  {
    // Non-raw / sRAW: per-channel composite straight back (no CFA). remosaic_in_cl == dev_in (no knee,
    // so no corrected copy). clip_is_floor stays TRUE, matching the CPU passthrough remosaic.
    const int clip_floor_on = TRUE;
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 0,
                             sizeof(cl_mem), &remosaic_in_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 1,
                             sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 2,
                             sizeof(cl_mem), &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 3,
                             sizeof(cl_mem), &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 4,
                             sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 5,
                             sizeof(cl_mem), &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 6,
                             sizeof(cl_mem), &clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 7,
                             sizeof(int), &clip_floor_on);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 8,
                             sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough, 9,
                             sizeof(int), &height);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, global_data->kernel_highlights_remosaic_and_replace_passthrough,
                                         sizes);
  }
  else
  {
    const int clip_floor_on = TRUE; // clipped raw values are floors, never blend targets
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 0, sizeof(cl_mem),
                             &remosaic_in_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 1, sizeof(cl_mem),
                             &dev_in);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 2, sizeof(cl_mem),
                             &interpolated);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 3, sizeof(cl_mem),
                             &clipping_mask);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 4, sizeof(cl_mem),
                             &dev_out);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 5, sizeof(cl_mem),
                             &normalization_final);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 6, sizeof(cl_mem),
                             &clips_cl);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 7, sizeof(int),
                             &clip_floor_on);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 8, sizeof(int),
                             &filters_shifted);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 9, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, global_data->kernel_highlights_remosaic_and_replace, 10, sizeof(int), &height);
    cl_err = dt_opencl_enqueue_kernel_2d(devid, global_data->kernel_highlights_remosaic_and_replace, sizes);
  }
  if(cl_err != CL_SUCCESS) goto fallback;

  // success: release and return
  HL_CL_RELEASE(clips_cl);
  HL_CL_RELEASE(det_clips_cl);
  HL_CL_RELEASE(normalization_final);
  HL_CL_RELEASE(interpolated);
  HL_CL_RELEASE(clipping_mask);
  HL_CL_RELEASE(temp);
  HL_CL_RELEASE(dev_xtrans);
  HL_CL_RELEASE(lookup_cl);
  HL_CL_RELEASE(corr_cl);
  dt_pixelpipe_cache_free_align(h_interp);
  dt_pixelpipe_cache_free_align(h_mask);
  dt_pixelpipe_cache_free_align(h_raw);
  dt_pixelpipe_cache_free_align(input_corr);
  return CL_SUCCESS;

fallback:
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_highlights] harmonic GPU gather failed (%i), falling back to the host roundtrip\n", cl_err);
  HL_CL_RELEASE(clips_cl);
  HL_CL_RELEASE(det_clips_cl);
  HL_CL_RELEASE(normalization_final);
  HL_CL_RELEASE(interpolated);
  HL_CL_RELEASE(clipping_mask);
  HL_CL_RELEASE(temp);
  HL_CL_RELEASE(dev_xtrans);
  HL_CL_RELEASE(lookup_cl);
  HL_CL_RELEASE(corr_cl);
  dt_pixelpipe_cache_free_align(h_interp);
  dt_pixelpipe_cache_free_align(h_mask);
  dt_pixelpipe_cache_free_align(h_raw);
  dt_pixelpipe_cache_free_align(input_corr);
  // preference 3 (last resort): a GPU gather/remosaic step failed -> the bit-identical host roundtrip.
  return _harmonic_cl_roundtrip(self, pipe, piece, dev_in, dev_out, roi_in, roi_out, clips);
}
#endif // HAVE_OPENCL
