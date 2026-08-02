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
#pragma once

#include <stddef.h>

/*
 * Loader and CPU executor for the .anselnn neural denoising models trained by
 * https://github.com/aurelienpierreeng/ansel-denoise.
 *
 * Deliberately self-contained: no darktable.h, no pipeline types. The only
 * dependencies are json-glib (header parsing), libm and OpenMP. The pixelpipe
 * integration lives in the IOP module; this translation unit owns the model
 * data and the math, nothing else.
 *
 * File format (little-endian):
 *   8 bytes  magic "ANSELDN1"
 *   4 bytes  uint32 JSON header length N
 *   N bytes  JSON: {"cfg": {"arch": "unet", "base", "depth",
 *                           "in_channels", "out_channels"},
 *                  "tensors": [{"name", "shape", "offset", "size"}, ...]}
 *   payload  float32 tensor data, offsets relative to payload start
 *
 * Fixed topology ("unet"): depth encoder levels of two 3x3 conv + GELU with a
 * 2x2 stride-2 downsampling conv between levels, a two-conv bottleneck, and a
 * decoder of nearest x2 upsampling, 1x1 channel-reduction conv, skip
 * concatenation and two 3x3 conv + GELU per level; a final 3x3 conv predicts
 * the noise, subtracted from the input mosaic plane (residual output).
 */

typedef struct dt_nn_model_t dt_nn_model_t;

/* Load a .anselnn file. Returns NULL on failure and, if err is non-NULL,
 * writes a human-readable reason into err[0..err_len-1]. */
dt_nn_model_t *dt_nn_model_load(const char *path, char *err, size_t err_len);

void dt_nn_model_free(dt_nn_model_t *model);

/* Number of input planes the FINE net expects (5 for arch "unet": mosaic,
 * R, G, B one-hot, sigma; 5 + coarse_out for arch "unet-ms") and output
 * planes (1: denoised mosaic). */
int dt_nn_model_in_channels(const dt_nn_model_t *model);
int dt_nn_model_out_channels(const dt_nn_model_t *model);

/* Multi-scale ("unet-ms") accessors. bin: the superpixel factor for the
 * image's CFA family, 1 for a single-scale model (no coarse stage).
 * coarse_in/out: the coarse net's plane counts, 0 without a coarse stage. */
int dt_nn_model_bin(const dt_nn_model_t *model, const int is_xtrans);
int dt_nn_model_coarse_in_channels(const dt_nn_model_t *model);
int dt_nn_model_coarse_out_channels(const dt_nn_model_t *model);

/* Low-band anchor scale (sensor px) carried in the model cfg: below it the
 * module replaces the output's per-channel means with the noisy input's —
 * the n-averaged measurement is the true diluted estimate there, while a
 * denoiser's low band accumulates model error. 0 = no anchoring. */
int dt_nn_model_anchor(const dt_nn_model_t *model);

/* Scales of the low-band fusion pyramid, in sensor px — fixed by the training
 * reference (cfa.fuse_low_bands, scales=(16, 32, 64)), not by the model file.
 * A padded tile must divide by the coarsest one, or the pyramid loses a level
 * and the fusion no longer matches what the model was trained against; see
 * dt_nn_model_alignment(). */
#define DT_NN_FUSION_FINEST 16
#define DT_NN_FUSION_COARSEST 64

/* Width and height passed to dt_nn_unet_apply must be multiples of this:
 * 2^depth for arch "unet"; for "unet-ms" also every bin << coarse_depth so
 * the binned planes stay aligned for either CFA. The caller pads (reflect)
 * and crops. */
int dt_nn_model_alignment(const dt_nn_model_t *model);

/* Peak scratch memory in bytes needed by dt_nn_unet_apply for a w x h input,
 * for the caller's tiling budget arithmetic. */
size_t dt_nn_unet_scratch_bytes(const dt_nn_model_t *model, int width, int height);

/* Run the network. in: in_channels planar w*h float32 planes; out: one w*h
 * plane (may not alias in). Returns 0 on success, non-zero on allocation
 * failure or misaligned dimensions. Thread-safe for concurrent calls on the
 * same model (weights are read-only; scratch is per-call). */
int dt_nn_unet_apply(const dt_nn_model_t *model, const float *in, float *out, int width, int height);

/* Run one stage of a multi-scale model. stage 0 = fine (identical to
 * dt_nn_unet_apply); stage 1 = coarse: in is the 6-plane binned input
 * [R, G, B, sigmaR, sigmaG, sigmaB] at (width, height) COARSE resolution,
 * out receives the denoised coarse RGB (residual applied). */
/* apply_residual: non-zero subtracts the head prediction from the matching
 * input planes (what the torch reference does inside its forward); zero writes
 * the RAW head output — which is what dt_nn_unet_apply_stage_cl always does,
 * and what a caller wants when it applies the residual itself. */
int dt_nn_unet_apply_stage(const dt_nn_model_t *model, int stage, const float *in, float *out, int width,
                           int height, int apply_residual);

/* Superpixel-bin the assembled fine planes [mosaic, R, G, B one-hot, ...]:
 * out_rgb (3 planes at pw/bin x ph/bin) receives the count-weighted mean of
 * each block's same-channel sensels, out_cnt the per-channel sensel counts
 * (for the analytic coarse sigma). The exact contract of the training
 * repo's cfa.bin_mosaic_torch. */
void dt_nn_bin_planes(const float *planes, int pw, int ph, int bin, float *out_rgb, float *out_cnt);

/* Nearest-neighbour upsample of ch planar w*h planes by an integer factor
 * (the coarse guide injection). */
void dt_nn_upsample_nearest(const float *in, int ch, int w, int h, int factor, float *out);

#ifdef HAVE_OPENCL
#include "common/opencl.h"

/* OpenCL kernel handles for the U-Net, created once per session from the
 * rawdenoiseai.cl program. Opaque; owned by dt_nn_cl_create/destroy. */
typedef struct dt_nn_cl_t dt_nn_cl_t;

dt_nn_cl_t *dt_nn_cl_create(int program);
void dt_nn_cl_destroy(dt_nn_cl_t *cl);

/* GPU forward of one stage. Writes the RAW head output — stage 0 the predicted
 * noise, stage 1 the coarse head — never the residual: subtracting it is the
 * caller's business, on both devices, because "the net predicts what to
 * remove" is the consumer's convention and not this runtime's. The CPU twin
 * dt_nn_unet_apply_stage() takes apply_residual = 0 for the same contract. */
int dt_nn_unet_apply_stage_cl(const dt_nn_model_t *model, int stage, dt_nn_cl_t *cl, int devid, cl_mem dev_in,
                              cl_mem dev_out, int width, int height);

#endif
