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

/* GPU kernels for the neural raw denoiser. Buffers are channel-planar float32,
 * matching the CPU executor. Two groups live here:
 *   - the executor's own kernels (nn_conv, nn_conv3x3, nn_upsample), driven by
 *     common/nn_model.c;
 *   - the denoiser's glue (nn_assemble, nn_bin_planes, nn_upsample_n,
 *     nn_residual, the nn_*fuse* pyramid, nn_blend_crop), driven by
 *     iop/rawdenoiseai.c, each with a _k_* twin in that file.
 * A whole tile runs dev_in -> dev_out with no host round-trip. */

// exact GELU (erf form), matching torch nn.GELU(approximate='none') and the
// CPU _gelu() in common/nn_model.c
#define NN_GELU(x) (0.5f * (x) * (1.0f + erf((x) * 0.70710678118654752f)))

// Convolution with 4-way output-channel blocking, mirroring the CPU executor.
//
// Each work-item computes NN_OCB=4 output channels for its pixel, so every
// input value read from global memory feeds 4 FMAs — without this the input
// planes are re-streamed once per output channel (out_ch*in_ch*k*k*wh reads),
// which is what actually bounds this network on GPUs (gigabytes per layer at
// the full-resolution levels). The work-group additionally stages the 4
// weight slices into local memory when they fit (use_local; they always do
// for the wide shallow layers where weight traffic matters — the deep layers
// with in_ch 256/512 have 16-256x fewer pixels and read weights from global).
// Zero padding, any (k, stride); weight/bias args are float offsets into the
// single uploaded model blob. Work dim 1 indexes the output-channel block.
#define NN_OCB 4

__kernel void nn_conv(__global const float *in, __global const float *weights, __global float *out,
                      const int w, const int h, const int ow, const int oh, const int in_ch,
                      const int out_ch, const int k, const int stride, const int pad,
                      const int weight_off, const int bias_off, const int do_gelu,
                      const int use_local, __local float *wl)
{
  const int sp = get_global_id(0);      // spatial: oy*ow + ox
  const int oc0 = get_global_id(1) * NN_OCB; // first output channel of the block
  const int lid = get_local_id(0);
  const int lsize = get_local_size(0);
  const int nb = min(NN_OCB, out_ch - oc0);
  const int nw = in_ch * k * k;

  if(use_local)
  {
    // cooperative load of the block's weight slices (participation of all
    // items, including those outside the spatial range)
    const int total = nb * nw;
    const int wbase = weight_off + oc0 * nw;
    for(int i = lid; i < total; i += lsize) wl[i] = weights[wbase + i];
    barrier(CLK_LOCAL_MEM_FENCE);
  }

  if(sp >= ow * oh || oc0 >= out_ch) return;

  const int ox = sp % ow;
  const int oy = sp / ow;
  const int wh = w * h;

  float a0 = weights[bias_off + oc0];
  float a1 = (nb > 1) ? weights[bias_off + oc0 + 1] : 0.0f;
  float a2 = (nb > 2) ? weights[bias_off + oc0 + 2] : 0.0f;
  float a3 = (nb > 3) ? weights[bias_off + oc0 + 3] : 0.0f;

  for(int ic = 0; ic < in_ch; ic++)
  {
    __global const float *ip = in + (long)ic * wh;
    const int wic = ic * k * k;
    for(int ky = 0; ky < k; ky++)
    {
      const int iy = oy * stride + ky - pad;
      if(iy < 0 || iy >= h) continue;
      for(int kx = 0; kx < k; kx++)
      {
        const int ix = ox * stride + kx - pad;
        if(ix < 0 || ix >= w) continue;
        const float xv = ip[(long)iy * w + ix];
        const int wo = wic + ky * k + kx;
        if(use_local)
        {
          a0 += wl[wo] * xv;
          if(nb > 1) a1 += wl[nw + wo] * xv;
          if(nb > 2) a2 += wl[2 * nw + wo] * xv;
          if(nb > 3) a3 += wl[3 * nw + wo] * xv;
        }
        else
        {
          const int wg = weight_off + oc0 * nw + wo;
          a0 += weights[wg] * xv;
          if(nb > 1) a1 += weights[wg + nw] * xv;
          if(nb > 2) a2 += weights[wg + 2 * nw] * xv;
          if(nb > 3) a3 += weights[wg + 3 * nw] * xv;
        }
      }
    }
  }

  if(do_gelu)
  {
    a0 = NN_GELU(a0); a1 = NN_GELU(a1); a2 = NN_GELU(a2); a3 = NN_GELU(a3);
  }
  const long oplane = (long)ow * oh;
  const long o = (long)oy * ow + ox;
  out[(long)oc0 * oplane + o] = a0;
  if(nb > 1) out[((long)oc0 + 1) * oplane + o] = a1;
  if(nb > 2) out[((long)oc0 + 2) * oplane + o] = a2;
  if(nb > 3) out[((long)oc0 + 3) * oplane + o] = a3;
}

// Fast path for the 3x3 stride-1 pad-1 convolutions (~95% of the FLOPs):
// each work-item computes a 2x2 output-pixel quad for its 4 output channels.
// The quad's four 3x3 windows overlap into one 4x4 input window, so input
// traffic drops from 36 to 16 loads per (quad, input channel), and every
// weight fetched feeds 4 pixels x 4 channels = 16 FMAs. 16 accumulators live
// comfortably in registers; no barriers beyond the weight staging.
__kernel void nn_conv3x3(__global const float *in, __global const float *weights,
                         __global float *out, const int w, const int h, const int in_ch,
                         const int out_ch, const int weight_off, const int bias_off,
                         const int do_gelu, const int chunk, __local float *wl)
{
  const int ow2 = (w + 1) / 2, oh2 = (h + 1) / 2; // quad grid (output size == input size)
  const int qp = get_global_id(0);                // quad index: qy2*ow2 + qx2
  const int oc0 = get_global_id(1) * NN_OCB;
  const int lid = get_local_id(0);
  const int lsize = get_local_size(0);
  const int nb = min(NN_OCB, out_ch - oc0);
  const int nw = in_ch * 9;
  // Weights are ALWAYS staged in local memory, `chunk` input channels at a
  // time, so the deep layers (in_ch 256/512, whose full slices exceed the
  // local budget) never fall back to per-item global weight reads — that
  // fallback streamed gigabytes per layer. Inactive work-items participate in
  // the cooperative loads (barriers must stay uniform) and skip the math.
  const bool active = (qp < ow2 * oh2) && (oc0 < out_ch);

  const int qx = active ? (qp % ow2) * 2 : 0;
  const int qy = active ? (qp / ow2) * 2 : 0;
  const long wh = (long)w * h;

  // acc[channel][pixel], pixels in quad order (dy*2 + dx). A wider 4x2 octet
  // variant was measured SLOWER (23.6 -> 32.3 s end-to-end on a Maxwell
  // Quadro): 32 accumulators + a 4x6 window exceed the register budget and
  // occupancy collapses. 2x2 is the sweet spot.
  float acc[NN_OCB][4];
  for(int c = 0; c < NN_OCB; c++)
  {
    const float bias = (c < nb) ? weights[bias_off + oc0 + c] : 0.0f;
    acc[c][0] = acc[c][1] = acc[c][2] = acc[c][3] = bias;
  }

  for(int ic0 = 0; ic0 < in_ch; ic0 += chunk)
  {
    const int nic = min(chunk, in_ch - ic0);
    // stage this chunk's weight slices: wl[c * chunk * 9 + (ic - ic0) * 9 + tap]
    barrier(CLK_LOCAL_MEM_FENCE); // previous chunk fully consumed
    for(int i = lid; i < nb * nic * 9; i += lsize)
    {
      const int c = i / (nic * 9);
      const int r = i % (nic * 9);
      wl[c * chunk * 9 + r] = weights[weight_off + (oc0 + c) * nw + ic0 * 9 + r];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
    if(!active) continue;

    for(int ic = ic0; ic < ic0 + nic; ic++)
    {
      __global const float *ip = in + (long)ic * wh;
      // 4x4 zero-padded input window covering the quad's four 3x3 neighborhoods
      float w4[4][4];
      for(int r = 0; r < 4; r++)
      {
        const int iy = qy + r - 1;
        const bool yok = (iy >= 0 && iy < h);
        for(int cx = 0; cx < 4; cx++)
        {
          const int ix = qx + cx - 1;
          w4[r][cx] = (yok && ix >= 0 && ix < w) ? ip[(long)iy * w + ix] : 0.0f;
        }
      }
      const int wic = (ic - ic0) * 9;
      for(int ky = 0; ky < 3; ky++)
        for(int kx = 0; kx < 3; kx++)
        {
          const int wo = wic + ky * 3 + kx;
          float wv[NN_OCB];
          for(int c = 0; c < NN_OCB; c++)
            wv[c] = (c < nb) ? wl[c * chunk * 9 + wo] : 0.0f;
          for(int c = 0; c < NN_OCB; c++)
          {
            acc[c][0] += wv[c] * w4[ky][kx];
            acc[c][1] += wv[c] * w4[ky][kx + 1];
            acc[c][2] += wv[c] * w4[ky + 1][kx];
            acc[c][3] += wv[c] * w4[ky + 1][kx + 1];
          }
        }
    }
  }
  if(!active) return;

  const long oplane = wh;
  for(int c = 0; c < nb; c++)
  {
    __global float *op = out + (long)(oc0 + c) * oplane;
    for(int px = 0; px < 4; px++)
    {
      const int ox = qx + (px & 1);
      const int oy = qy + (px >> 1);
      if(ox >= w || oy >= h) continue;
      const float v = do_gelu ? NN_GELU(acc[c][px]) : acc[c][px];
      op[(long)oy * w + ox] = v;
    }
  }
}

// nearest-neighbour 2x upsampling, per (output pixel, channel)
__kernel void nn_upsample(__global const float *in, __global float *out, const int w, const int h,
                          const int ch)
{
  const int sp = get_global_id(0); // oy*(2w) + ox
  const int c = get_global_id(1);
  const int ow = 2 * w, oh = 2 * h;
  if(sp >= ow * oh || c >= ch) return;
  const int ox = sp % ow;
  const int oy = sp / ow;
  out[(long)c * ow * oh + oy * ow + ox] = in[(long)c * w * h + (oy / 2) * w + (ox / 2)];
}

/* ---- device-resident glue: everything between dev_in and dev_out runs on
 * the GPU with zero mid-tile host round-trips. Mirrors the host reference
 * implementations in iop/rawdenoiseai.c (_assemble_planes,
 * _assemble_coarse_planes, _apply_low_band_anchor) — keep them in sync. */

static int nn_reflect(int v, int n)
{
  if(n == 1) return 0;
  while(v < 0 || v >= n)
  {
    if(v < 0) v = -v;
    if(v >= n) v = 2 * n - 2 - v;
  }
  return v;
}

/* CFA color of a sensel. Bayer uses tile-local coordinates (the pipeline
 * pre-shifts `filters` per tile); X-Trans applies the roi offset, exactly
 * like FCxtrans() on the CPU path. */
static int nn_fc(const int row, const int col, const unsigned int filters,
                 __global const unsigned char *xtrans, const int is_xtrans,
                 const int roi_x, const int roi_y)
{
  /* X-Trans is self-correcting: raw table, offset added here. Bayer is
   * tile-local by design and the HOST hands it a `filters` word already
   * rotated to this ROI's phase (dt_dev_get_roi_filters), so it must NOT add
   * roi_x/roi_y again — doing so double-applies the offset. Keep the two
   * branches' conventions straight; mixing them up is how this class of bug
   * keeps coming back (see the CFA-phase rule in CLAUDE.md). */
  if(is_xtrans) return xtrans[((row + roi_y) % 6) * 6 + ((col + roi_x) % 6)];
  return (filters >> (((row << 1 & 14) + (col & 1)) << 1)) & 3;
}

/* build the 5 base planes [mosaic, R, G, B one-hot, sigma] reflect-padded */
__kernel void nn_assemble(__global const float *in, __global float *planes, const int width,
                          const int height, const int pw, const int ph, const unsigned int filters,
                          __global const unsigned char *xtrans, const int is_xtrans,
                          const int roi_x, const int roi_y,
                          const float a0, const float a1, const float a2,
                          const float b0, const float b1, const float b2,
                          const float s0, const float s1, const float s2)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= pw || y >= ph) return;
  const int sx = nn_reflect(x, width), sy = nn_reflect(y, height);
  const float v = in[sy * width + sx];
  int c = nn_fc(sy, sx, filters, xtrans, is_xtrans, roi_x, roi_y);
  if(c > 2) c = 1;
  const long plane = (long)pw * ph;
  const long i = (long)y * pw + x;
  const float a[3] = { a0, a1, a2 }, b[3] = { b0, b1, b2 }, s[3] = { s0, s1, s2 };
  planes[i] = v;
  planes[plane + i] = (c == 0) ? 1.0f : 0.0f;
  planes[2 * plane + i] = (c == 1) ? 1.0f : 0.0f;
  planes[3 * plane + i] = (c == 2) ? 1.0f : 0.0f;
  const float var = a[c] * fmax(v, 0.0f) + b[c];
  planes[4 * plane + i] = s[c] * sqrt(fmax(var, 1e-12f));
}

/* superpixel-bin planes 0-3 into the 6-plane coarse input [RGB, sigmaRGB] */
__kernel void nn_bin_planes(__global const float *planes, __global float *coarse_in,
                            const int pw, const int ph, const int bin,
                            const float a0, const float a1, const float a2,
                            const float b0, const float b1, const float b2,
                            const float s0, const float s1, const float s2)
{
  const int cx = get_global_id(0);
  const int cw = pw / bin, chh = ph / bin;
  const int cy = get_global_id(1) % chh, c = get_global_id(1) / chh;
  if(cx >= cw || c >= 3) return;
  const long plane = (long)pw * ph;
  __global const float *oh = planes + (long)(1 + c) * plane;
  float sum = 0.0f, cnt = 0.0f;
  for(int y = cy * bin; y < (cy + 1) * bin; y++)
    for(int x = cx * bin; x < (cx + 1) * bin; x++)
    {
      const long i = (long)y * pw + x;
      sum += planes[i] * oh[i];
      cnt += oh[i];
    }
  const float n = fmax(cnt, 1.0f);
  const float mean = sum / n;
  const long cplane = (long)cw * chh;
  const long ci = (long)cy * cw + cx;
  const float a[3] = { a0, a1, a2 }, b[3] = { b0, b1, b2 }, s[3] = { s0, s1, s2 };
  coarse_in[(long)c * cplane + ci] = mean;
  const float var = (a[c] * fmax(mean, 0.0f) + b[c]) / n;
  coarse_in[(long)(3 + c) * cplane + ci] = s[c] * sqrt(fmax(var, 1e-12f));
}

/* out[i] = in[i] - head[i] over ch planes (residual application) */
__kernel void nn_residual(__global const float *in, __global const float *head,
                          __global float *out, const int n)
{
  const int i = get_global_id(0);
  if(i < n) out[i] = in[i] - head[i];
}

/* nearest x-factor upsample of ch planes into dst (guide injection) */
__kernel void nn_upsample_n(__global const float *in, __global float *dst, const int cw,
                            const int chh, const int factor, const int ch, const long dst_off)
{
  const int x = get_global_id(0);
  const int fw = cw * factor, fh = chh * factor;
  const int y = get_global_id(1) % fh, c = get_global_id(1) / fh;
  if(x >= fw || c >= ch) return;
  dst[dst_off + (long)c * fw * fh + (long)y * fw + x]
      = in[(long)c * cw * chh + (long)(y / factor) * cw + x / factor];
}

/* level-0 (16 px) grids of the fusion pyramid: count-weighted per-channel
 * means of the measurement (planes[0]) and the denoised plane */
__kernel void nn_bin16_mdv(__global const float *planes, __global const float *den,
                           __global float *M, __global float *D, __global float *V,
                           const int pw, const int ph)
{
  const int cx = get_global_id(0);
  const int cw = pw / 16, chh = ph / 16;
  const int cy = get_global_id(1) % chh, c = get_global_id(1) / chh;
  if(cx >= cw || c >= 3) return;
  const long plane = (long)pw * ph;
  __global const float *oh = planes + (long)(1 + c) * plane;
  __global const float *sig = planes + 4 * plane;
  float sm = 0.0f, sd = 0.0f, sv = 0.0f, cnt = 0.0f;
  for(int y = cy * 16; y < (cy + 1) * 16; y++)
    for(int x = cx * 16; x < (cx + 1) * 16; x++)
    {
      const long i = (long)y * pw + x;
      sm += planes[i] * oh[i];
      sd += den[i] * oh[i];
      sv += sig[i] * sig[i] * oh[i];
      cnt += oh[i];
    }
  const float n = fmax(cnt, 1.0f);
  const long ci = (long)c * cw * chh + (long)cy * cw + cx;
  M[ci] = sm / n;
  D[ci] = sd / n;
  V[ci] = sv / n;
}

/* 2x2 average pooling, 3 planes */
__kernel void nn_avg2x2(__global const float *in, __global float *out, const int sw, const int sh)
{
  const int x = get_global_id(0);
  const int w2 = sw / 2, h2 = sh / 2;
  const int y = get_global_id(1) % h2, c = get_global_id(1) / h2;
  if(x >= w2 || c >= 3) return;
  __global const float *p = in + (long)c * sw * sh;
  out[(long)c * w2 * h2 + (long)y * w2 + x]
      = 0.25f * (p[(long)(2 * y) * sw + 2 * x] + p[(long)(2 * y) * sw + 2 * x + 1]
                 + p[(long)(2 * y + 1) * sw + 2 * x] + p[(long)(2 * y + 1) * sw + 2 * x + 1]);
}

static float nn_bilerp(__global const float *p, const int w, const int h, float fx, float fy)
{
  fx = clamp(fx, 0.0f, (float)(w - 1));
  fy = clamp(fy, 0.0f, (float)(h - 1));
  const int x0 = (int)fx, y0 = (int)fy;
  const int x1 = min(x0 + 1, w - 1), y1 = min(y0 + 1, h - 1);
  const float ax = fx - x0, ay = fy - y0;
  const float top = p[(long)y0 * w + x0] * (1.0f - ax) + p[(long)y0 * w + x1] * ax;
  const float bot = p[(long)y1 * w + x0] * (1.0f - ax) + p[(long)y1 * w + x1] * ax;
  return top * (1.0f - ay) + bot * ay;
}

/* FLOOR band: structure-gated blend against the per-cell noise variance.
 * vn reads V, the sigma^2 binned on this same grid — NOT a whole-tile mean.
 * sigma^2 = a*x + b is signal-dependent, so a single per-tile scalar
 * over-states vn in shadows and under-states it in highlights, and made the
 * result depend on how the pipe happened to tile. Mirrors cfa.fuse_low_bands. */
__kernel void nn_floor_fuse(__global const float *M, __global const float *D,
                            __global float *fused, __global const float *V,
                            const int sw, const int sh, const int S,
                            const float dens0, const float dens1, const float dens2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1) % sh, c = get_global_id(1) / sh;
  if(x >= sw || c >= 3) return;
  const float dens[3] = { dens0, dens1, dens2 };
  const long pl = (long)sw * sh;
  const long i = (long)c * pl + (long)y * sw + x;
  // this cell's own mean sigma^2, not the tile's (cfa.fuse_low_bands)
  const float vn = V[i] / (dens[c] * S * S);
  __global const float *Mp = M + (long)c * pl;
  /* structure = blur3((M - blur3(M))^2): smoothed energy of the mean-removed
   * residual — insensitive to smooth gradients, unlike a plain window
   * variance. Exact match of the CPU/torch references. */
  float structure = 0.0f;
  for(int ny = -1; ny <= 1; ny++)
    for(int nx = -1; nx <= 1; nx++)
    {
      const int cy2 = clamp(y + ny, 0, sh - 1), cx2 = clamp(x + nx, 0, sw - 1);
      float mean = 0.0f;
      for(int dy = -1; dy <= 1; dy++)
        for(int dx = -1; dx <= 1; dx++)
        {
          const int yy = clamp(cy2 + dy, 0, sh - 1), xx = clamp(cx2 + dx, 0, sw - 1);
          mean += Mp[(long)yy * sw + xx];
        }
      const float mloc = Mp[(long)cy2 * sw + cx2] - mean / 9.0f;
      structure += mloc * mloc;
    }
  structure = structure / 9.0f - 2.5f * vn;
  structure = fmax(structure, 0.0f);
  const float w = structure / (structure + vn + 1e-20f);
  fused[i] = w * D[i] + (1.0f - w) * M[i];
}

/* one fusion step: fused_f = bilerp2x(fused_c) + w*bandD + (1-w)*bandM with a
 * per-cell Wiener weight from the 3x3-smoothed band discrepancy (chi^2 guard
 * T = 2.5); mirrors cfa.fuse_low_bands in the training repo */
__kernel void nn_fuse_step(__global const float *fused_c, __global const float *Mf,
                           __global const float *Df, __global const float *Mc,
                           __global const float *Dc, __global float *fused_f,
                           __global const float *Vf,
                           const int fw, const int fh, const int sc,
                           const float dens0, const float dens1, const float dens2)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1) % fh, c = get_global_id(1) / fh;
  if(x >= fw || c >= 3) return;
  const int sw = fw / 2, sh = fh / 2;
  const long fpl = (long)fw * fh, spl = (long)sw * sh;
  const float dens[3] = { dens0, dens1, dens2 };
  /* Var(mean_s - up(mean_2s)) = Var(mean_s) * 3/4 once the covariance with the
   * 2x2 parent is folded in, which is what this reciprocal difference is; so
   * the scale-s cell mean is the right sigma^2 for the whole term. */
  const float vn = Vf[(long)c * fpl + (long)y * fw + x]
                   * (1.0f / (dens[c] * sc * sc) - 1.0f / (dens[c] * 4.0f * sc * sc));
  float acc = 0.0f;
  for(int dy = -1; dy <= 1; dy++)
    for(int dx = -1; dx <= 1; dx++)
    {
      const int yy = clamp(y + dy, 0, fh - 1), xx = clamp(x + dx, 0, fw - 1);
      const float sx = (xx + 0.5f) * 0.5f - 0.5f, sy = (yy + 0.5f) * 0.5f - 0.5f;
      const long j = (long)yy * fw + xx;
      const float bd = Df[(long)c * fpl + j] - nn_bilerp(Dc + (long)c * spl, sw, sh, sx, sy);
      const float bm = Mf[(long)c * fpl + j] - nn_bilerp(Mc + (long)c * spl, sw, sh, sx, sy);
      acc += (bd - bm) * (bd - bm);
    }
  float vm = acc / 9.0f - 2.5f * vn;
  vm = fmax(vm, 0.0f);
  const float w = vn / (vn + vm + 1e-20f);
  const float sx = (x + 0.5f) * 0.5f - 0.5f, sy = (y + 0.5f) * 0.5f - 0.5f;
  const long i = (long)y * fw + x;
  const float upf = nn_bilerp(fused_c + (long)c * spl, sw, sh, sx, sy);
  const float bd = Df[(long)c * fpl + i] - nn_bilerp(Dc + (long)c * spl, sw, sh, sx, sy);
  const float bm = Mf[(long)c * fpl + i] - nn_bilerp(Mc + (long)c * spl, sw, sh, sx, sy);
  fused_f[(long)c * fpl + i] = upf + w * bd + (1.0f - w) * bm;
}

/* distribute the bilinear-upsampled (fused - D16) correction onto sensels */
__kernel void nn_bilerp_add(__global const float *fused, __global const float *D16,
                            __global const float *planes, __global float *den,
                            const int pw, const int ph)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= pw || y >= ph) return;
  const int cw = pw / 16, chh = ph / 16;
  const long plane = (long)pw * ph, cpl = (long)cw * chh;
  const long i = (long)y * pw + x;
  int c = 0;
  if(planes[2 * plane + i] > 0.0f) c = 1;
  else if(planes[3 * plane + i] > 0.0f) c = 2;
  const float sx = (x + 0.5f) / 16.0f - 0.5f, sy = (y + 0.5f) / 16.0f - 0.5f;
  const float corr = nn_bilerp(fused + (long)c * cpl, cw, chh, sx, sy)
                     - nn_bilerp(D16 + (long)c * cpl, cw, chh, sx, sy);
  den[i] += corr;
}

/* final crop + strength blend straight into the pipeline output buffer */
__kernel void nn_blend_crop(__global const float *in, __global const float *den,
                            __global float *out, const int width, const int height,
                            const int pw, const float strength)
{
  const int x = get_global_id(0), y = get_global_id(1);
  if(x >= width || y >= height) return;
  const float orig = in[(long)y * width + x];
  const float d = den[(long)y * pw + x];
  out[(long)y * width + x] = orig + strength * (d - orig);
}
