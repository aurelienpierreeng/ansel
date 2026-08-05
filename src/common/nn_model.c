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
#include "common/nn_model.h"

// the only dependency this unit takes beyond libm/glib/json-glib: the
// multi-versioning attribute, duplicated there so darktable.h stays out
#include "common/target_clones.h"

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NN_MAX_DEPTH 8
#define NN_MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct nn_conv_t
{
  const float *w; // (out_ch, in_ch, k, k), row-major
  const float *b; // (out_ch)
  int out_ch, in_ch, k;
} nn_conv_t;

#ifdef HAVE_OPENCL
#define NN_MAX_DEVICES 16
#endif

// one wired U-Net (weights point into the model's shared blob)
typedef struct nn_unet_t
{
  int base, depth, in_ch, out_ch;
  nn_conv_t enc1[NN_MAX_DEPTH], enc2[NN_MAX_DEPTH], down[NN_MAX_DEPTH];
  nn_conv_t bot1, bot2;
  nn_conv_t up[NN_MAX_DEPTH], dec1[NN_MAX_DEPTH], dec2[NN_MAX_DEPTH];
  nn_conv_t head;
} nn_unet_t;

struct dt_nn_model_t
{
  nn_unet_t fine;   // stage 0: the mosaic net (the only net for arch "unet")
  nn_unet_t coarse; // stage 1: the superpixel-RGB net (arch "unet-ms" only)
  int has_coarse;
  int bin_bayer, bin_xtrans; // superpixel bin factors per CFA family
  int anchor;                // low-band anchor scale in sensor px (0 = none)
  float *blob;               // whole payload, tensors point into it
  size_t blob_floats;        // number of floats in blob (for device upload)
#ifdef HAVE_OPENCL
  cl_mem dev_weights[NN_MAX_DEVICES]; // blob uploaded per device, lazily
  dt_pthread_mutex_t cl_lock;
#endif
};

/* injected pixel-buffer allocator (see nn_model.h); malloc fallback keeps the
 * standalone fixture test dependency-free */
static dt_nn_alloc_f _nn_alloc_fn = NULL;
static dt_nn_free_f _nn_free_fn = NULL;

void dt_nn_set_allocator(dt_nn_alloc_f alloc_fn, dt_nn_free_f free_fn)
{
  _nn_alloc_fn = alloc_fn;
  _nn_free_fn = free_fn;
}

/* Pixel buffers come from the injected arena — the pixelpipe cache memory
 * arena in the application — and ONLY from it: the arena is the application's
 * memory-budget control, and a malloc escape hatch would simply move the
 * failure to the OS OOM killer, which kills the process instead of one tile.
 * An arena refusal therefore fails the forward, and the caller falls back to
 * an unprocessed tile — the tiling engine's job is to plan tiles small enough
 * that this cannot happen (see dt_nn_unet_scratch_per_px and the module's
 * tiling_callback). The malloc path below exists solely for the standalone
 * fixture test, which runs outside the application with no allocator set. */

static void *_nn_alloc(size_t floats, int long_lived)
{
  const size_t bytes = floats * sizeof(float);
  return _nn_alloc_fn ? _nn_alloc_fn(bytes, long_lived) : malloc(bytes);
}

static void _nn_free(void *p)
{
  if(!p) return;
  if(_nn_free_fn)
    _nn_free_fn(p);
  else
    free(p);
}

static void _err(char *err, size_t err_len, const char *msg)
{
  if(err && err_len) snprintf(err, err_len, "%s", msg);
}

/* ------------------------------------------------------------------------
 * loader
 * ------------------------------------------------------------------------ */

typedef struct nn_header_t
{
  JsonArray *tensors;
  const float *payload;
  size_t payload_size;
} nn_header_t;

// resolve one conv layer's weight+bias by pytorch state-dict name prefix,
// validating dimensions against the expectation
static int _wire_conv(const nn_header_t *h, const char *prefix, int out_ch, int in_ch, int k, nn_conv_t *cv,
                      char *err, size_t err_len)
{
  char name[128];
  const float *w = NULL, *b = NULL;
  const guint n = json_array_get_length(h->tensors);
  for(int part = 0; part < 2; part++)
  {
    snprintf(name, sizeof(name), "%s.%s", prefix, part == 0 ? "weight" : "bias");
    const size_t want = (part == 0 ? (size_t)out_ch * in_ch * k * k : (size_t)out_ch) * sizeof(float);
    const float *found = NULL;
    for(guint i = 0; i < n; i++)
    {
      // the file comes from the user's config dir: never trust its structure.
      // A non-object element or a missing/typeless member must be a clean
      // reject, not a NULL dereference.
      JsonNode *node = json_array_get_element(h->tensors, i);
      if(!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
      JsonObject *t = json_node_get_object(node);
      if(!json_object_has_member(t, "name") || !json_object_has_member(t, "offset")
         || !json_object_has_member(t, "size"))
        continue;
      if(g_strcmp0(json_object_get_string_member(t, "name"), name)) continue;
      const gint64 offset = json_object_get_int_member(t, "offset");
      const gint64 size = json_object_get_int_member(t, "size");
      if(size != (gint64)want || offset < 0 || (size_t)(offset + size) > h->payload_size)
      {
        _err(err, err_len, "tensor with unexpected size or offset");
        return 1;
      }
      found = (const float *)((const uint8_t *)h->payload + offset);
      break;
    }
    if(!found)
    {
      if(err && err_len) snprintf(err, err_len, "missing tensor %s", name);
      return 1;
    }
    if(part == 0)
      w = found;
    else
      b = found;
  }
  cv->w = w;
  cv->b = b;
  cv->out_ch = out_ch;
  cv->in_ch = in_ch;
  cv->k = k;
  return 0;
}

// wire one full U-Net from the tensor table; stage_prefix is "" for arch
// "unet" and "coarse." / "fine." for arch "unet-ms" (pytorch submodule names)
static int _wire_unet(const nn_header_t *h, const char *stage_prefix, int base, int depth, int in_ch, int out_ch,
                      nn_unet_t *u, char *err, size_t err_len)
{
  u->base = base;
  u->depth = depth;
  u->in_ch = in_ch;
  u->out_ch = out_ch;
  char prefix[96];
  int bad = 0, cin = in_ch;
  for(int l = 0; l < depth && !bad; l++)
  {
    const int w = base << l;
    snprintf(prefix, sizeof(prefix), "%senc.%d.0", stage_prefix, l);
    bad |= _wire_conv(h, prefix, w, cin, 3, &u->enc1[l], err, err_len);
    snprintf(prefix, sizeof(prefix), "%senc.%d.2", stage_prefix, l);
    bad |= _wire_conv(h, prefix, w, w, 3, &u->enc2[l], err, err_len);
    snprintf(prefix, sizeof(prefix), "%sdown.%d", stage_prefix, l);
    bad |= _wire_conv(h, prefix, w, w, 2, &u->down[l], err, err_len);
    cin = w;
  }
  const int wb = base << depth;
  snprintf(prefix, sizeof(prefix), "%sbottleneck.0", stage_prefix);
  bad |= _wire_conv(h, prefix, wb, base << (depth - 1), 3, &u->bot1, err, err_len);
  snprintf(prefix, sizeof(prefix), "%sbottleneck.2", stage_prefix);
  bad |= _wire_conv(h, prefix, wb, wb, 3, &u->bot2, err, err_len);
  // decoder ModuleLists were built from the deepest level down: up.0/dec.0
  // operate on the bottleneck output, up.(depth-1)/dec.(depth-1) on level 0
  for(int i = 0; i < depth && !bad; i++)
  {
    const int w_skip = base << (depth - 1 - i);
    const int w_in = w_skip << 1;
    snprintf(prefix, sizeof(prefix), "%sup.%d", stage_prefix, i);
    bad |= _wire_conv(h, prefix, w_skip, w_in, 1, &u->up[i], err, err_len);
    snprintf(prefix, sizeof(prefix), "%sdec.%d.0", stage_prefix, i);
    bad |= _wire_conv(h, prefix, w_skip, 2 * w_skip, 3, &u->dec1[i], err, err_len);
    snprintf(prefix, sizeof(prefix), "%sdec.%d.2", stage_prefix, i);
    bad |= _wire_conv(h, prefix, w_skip, w_skip, 3, &u->dec2[i], err, err_len);
  }
  snprintf(prefix, sizeof(prefix), "%shead", stage_prefix);
  bad |= _wire_conv(h, prefix, out_ch, base, 3, &u->head, err, err_len);
  return bad;
}

// read {base, depth, in_channels, out_channels} from a (sub-)cfg object with
// range validation; out_ch_max 1 keeps the historic fine-stage contract
static int _read_net_cfg(JsonObject *cfg, int out_ch_max, int *base, int *depth, int *in_ch, int *out_ch)
{
  if(!cfg || !json_object_has_member(cfg, "base") || !json_object_has_member(cfg, "depth")
     || !json_object_has_member(cfg, "in_channels") || !json_object_has_member(cfg, "out_channels"))
    return 1;
  *base = (int)json_object_get_int_member(cfg, "base");
  *depth = (int)json_object_get_int_member(cfg, "depth");
  *in_ch = (int)json_object_get_int_member(cfg, "in_channels");
  *out_ch = (int)json_object_get_int_member(cfg, "out_channels");
  return *base < 1 || *base > 512 || *depth < 1 || *depth > NN_MAX_DEPTH || *in_ch < 1 || *in_ch > 16
         || *out_ch < 1 || *out_ch > out_ch_max;
}

dt_nn_model_t *dt_nn_model_load(const char *path, char *err, size_t err_len)
{
  FILE *f = g_fopen(path, "rb");
  if(!f)
  {
    _err(err, err_len, "cannot open model file");
    return NULL;
  }
  uint8_t magic[8];
  uint32_t header_len = 0;
  if(fread(magic, 1, 8, f) != 8 || memcmp(magic, "ANSELDN1", 8) || fread(&header_len, 4, 1, f) != 1
     || header_len == 0 || header_len > (64u << 20))
  {
    _err(err, err_len, "not an ANSELDN1 model file");
    fclose(f);
    return NULL;
  }
  char *header = malloc(header_len + 1);
  if(!header || fread(header, 1, header_len, f) != header_len)
  {
    _err(err, err_len, "truncated model header");
    free(header);
    fclose(f);
    return NULL;
  }
  header[header_len] = '\0';

  fseek(f, 0, SEEK_END);
  const long file_size = ftell(f);
  if(file_size < 12 + (long)header_len)
  {
    _err(err, err_len, "truncated model file");
    free(header);
    fclose(f);
    return NULL;
  }
  const size_t payload_size = (size_t)file_size - 12 - header_len;
  float *blob = malloc(payload_size);
  fseek(f, 12 + (long)header_len, SEEK_SET);
  const int payload_ok = blob && fread(blob, 1, payload_size, f) == payload_size;
  fclose(f);
  if(!payload_ok)
  {
    _err(err, err_len, "truncated model payload");
    free(header);
    free(blob);
    return NULL;
  }

  dt_nn_model_t *m = NULL;
  JsonParser *parser = json_parser_new();
  if(!json_parser_load_from_data(parser, header, header_len, NULL))
  {
    _err(err, err_len, "invalid model header JSON");
    goto out;
  }
  JsonObject *root = json_node_get_object(json_parser_get_root(parser));
  if(!root || !json_object_has_member(root, "cfg") || !json_object_has_member(root, "tensors"))
  {
    _err(err, err_len, "model header missing cfg or tensors");
    goto out;
  }
  JsonObject *cfg = json_object_get_object_member(root, "cfg");
  const char *arch = cfg ? json_object_get_string_member(cfg, "arch") : NULL;
  const int is_ms = !g_strcmp0(arch, "unet-ms");
  if(!is_ms && g_strcmp0(arch, "unet"))
  {
    _err(err, err_len, "unsupported model architecture");
    goto out;
  }
  int f_base = 0, f_depth = 0, f_in = 0, f_out = 0;
  int c_base = 0, c_depth = 0, c_in = 0, c_out = 0;
  int bin_bayer = 1, bin_xtrans = 1;
  if(is_ms)
  {
    if(!json_object_has_member(cfg, "coarse") || !json_object_has_member(cfg, "fine")
       || !json_object_has_member(cfg, "bin")
       || _read_net_cfg(json_object_get_object_member(cfg, "fine"), 1, &f_base, &f_depth, &f_in, &f_out)
       || _read_net_cfg(json_object_get_object_member(cfg, "coarse"), 8, &c_base, &c_depth, &c_in, &c_out))
    {
      _err(err, err_len, "model config out of supported range");
      goto out;
    }
    JsonObject *bin = json_object_get_object_member(cfg, "bin");
    if(!bin || !json_object_has_member(bin, "bayer") || !json_object_has_member(bin, "xtrans"))
    {
      _err(err, err_len, "model config missing bin factors");
      goto out;
    }
    bin_bayer = (int)json_object_get_int_member(bin, "bayer");
    bin_xtrans = (int)json_object_get_int_member(bin, "xtrans");
    if(bin_bayer < 2 || bin_bayer > 16 || bin_xtrans < 2 || bin_xtrans > 16)
    {
      _err(err, err_len, "model bin factors out of supported range");
      goto out;
    }
  }
  else if(_read_net_cfg(cfg, 1, &f_base, &f_depth, &f_in, &f_out))
  {
    _err(err, err_len, "model config out of supported range");
    goto out;
  }
  // the OpenCL path indexes weights with int offsets into the float blob
  if(payload_size / sizeof(float) > (size_t)INT_MAX)
  {
    _err(err, err_len, "model payload too large");
    goto out;
  }
  JsonNode *tensors_node = json_object_get_member(root, "tensors");
  if(!tensors_node || !JSON_NODE_HOLDS_ARRAY(tensors_node))
  {
    _err(err, err_len, "model header tensors is not an array");
    goto out;
  }

  m = calloc(1, sizeof(dt_nn_model_t));
  if(!m) goto out;
  m->has_coarse = is_ms;
  m->bin_bayer = bin_bayer;
  m->bin_xtrans = bin_xtrans;
  if(is_ms && json_object_has_member(cfg, "anchor"))
  {
    const int anchor = (int)json_object_get_int_member(cfg, "anchor");
    if(anchor >= 8 && anchor <= 256) m->anchor = anchor;
  }
  m->blob = blob;
  m->blob_floats = payload_size / sizeof(float);
  // cfg "sigma_calibration" documents the noise convention the weights were
  // trained under; it is deliberately NOT read here — the module's sigma
  // conditioning is carried entirely by user-visible GUI values, never by a
  // factor hidden inside the model file.
#ifdef HAVE_OPENCL
  dt_pthread_mutex_init(&m->cl_lock, NULL);
#endif

  const nn_header_t h
      = { .tensors = json_object_get_array_member(root, "tensors"), .payload = blob, .payload_size = payload_size };
  int bad = _wire_unet(&h, is_ms ? "fine." : "", f_base, f_depth, f_in, f_out, &m->fine, err, err_len);
  if(is_ms && !bad) bad = _wire_unet(&h, "coarse.", c_base, c_depth, c_in, c_out, &m->coarse, err, err_len);

  if(bad)
  {
#ifdef HAVE_OPENCL
    dt_pthread_mutex_destroy(&m->cl_lock);
#endif
    free(m); // blob freed below through the common error path
    m = NULL;
  }

out:
  if(!m) free(blob);
  free(header);
  g_object_unref(parser);
  return m;
}

#ifdef HAVE_OPENCL
static void dt_nn_model_free_cl(dt_nn_model_t *m)
{
  for(int d = 0; d < NN_MAX_DEVICES; d++)
    if(m->dev_weights[d])
    {
      dt_opencl_release_mem_object(m->dev_weights[d]);
      m->dev_weights[d] = NULL;
    }
}
#endif

void dt_nn_model_free(dt_nn_model_t *m)
{
  if(!m) return;
#ifdef HAVE_OPENCL
  dt_nn_model_free_cl(m);
  dt_pthread_mutex_destroy(&m->cl_lock);
#endif
  free(m->blob);
  free(m);
}

int dt_nn_model_in_channels(const dt_nn_model_t *m)
{
  return m->fine.in_ch;
}

int dt_nn_model_out_channels(const dt_nn_model_t *m)
{
  return m->fine.out_ch;
}

int dt_nn_model_bin(const dt_nn_model_t *m, const int is_xtrans)
{
  if(!m->has_coarse) return 1;
  return is_xtrans ? m->bin_xtrans : m->bin_bayer;
}

int dt_nn_model_coarse_in_channels(const dt_nn_model_t *m)
{
  return m->has_coarse ? m->coarse.in_ch : 0;
}

int dt_nn_model_coarse_out_channels(const dt_nn_model_t *m)
{
  return m->has_coarse ? m->coarse.out_ch : 0;
}

int dt_nn_model_anchor(const dt_nn_model_t *m)
{
  return m->anchor;
}

static int _lcm(int a, int b)
{
  if(a <= 0 || b <= 0) return a > b ? a : b; // degenerate inputs: no zero division
  int x = a, y = b;
  while(y)
  {
    const int t = x % y;
    x = y;
    y = t;
  }
  return a / x * b;
}

int dt_nn_model_alignment(const dt_nn_model_t *m)
{
  // a padded tile must divide by the fine net's stride pyramid AND, for a
  // multi-scale model, its binned version must divide by the coarse net's —
  // for either CFA family, since the model file is CFA-agnostic
  int align = 1 << m->fine.depth;
  if(m->has_coarse)
  {
    align = _lcm(align, m->bin_bayer << m->coarse.depth);
    align = _lcm(align, m->bin_xtrans << m->coarse.depth);
  }
  // ...and, when the model asks for the low-band fusion, by that pyramid too.
  // The fusion runs at 16/32/64 sensor px (DT_NN_FUSION_COARSEST); a tile the
  // coarsest band does not divide would silently fall back to a two-level
  // pyramid, which is NOT what the model was fused against at training time
  // and — since the tile grid depends on free RAM/vRAM — makes the rendered
  // result depend on the machine and on whether the pipe tiled at all.
  if(m->anchor > 0) align = _lcm(align, DT_NN_FUSION_COARSEST);
  return align;
}

/* ------------------------------------------------------------------------
 * executor
 * ------------------------------------------------------------------------ */

// Number of output channels computed together in the conv inner loop. Each
// input value is loaded once and reused across this many weight-broadcast FMAs
// (register-blocking), turning the memory-bound single-channel saxpy into
// arithmetic-bound work. 4 keeps the accumulators + broadcasts within the SIMD
// register file on AVX2/NEON. All layer widths here are multiples of 4 except
// the 1-channel head, which falls through to the scalar-remainder path.
#define NN_OC_BLOCK 4

/* NOTE on a measured dead end: an 8-wide-strip row-pair microkernel (2 output
 * rows x 4 output channels, the CPU translation of the GPU quad kernel) was
 * implemented and benchmarked 2.6x SLOWER than the long-row formulation below
 * (12.5 vs 4.9 s/MP at 512x512): the short strips defeat the compiler's
 * full-width row vectorization and the 64-float accumulator block spills.
 * With compiler-driven SIMD, long streaming rows win; beating them would take
 * explicit intrinsics, not restructuring. */

// out[oc] = bias[oc] + sum_ic conv(in[ic]); zero padding, any (k, stride).
//
// Output channels are blocked NN_OC_BLOCK at a time so each input load feeds
// that many accumulators (one per output channel) before moving on; the input
// plane is thus streamed out_ch/NN_OC_BLOCK times instead of out_ch times. The
// (oy, oc-block) collapse keeps the k input rows hot across the block. The
// per-output-element accumulation order (ic, ky, kx) is unchanged, so results
// match the reference bit-for-bit under identical FP settings. The naive
// formulation (shifted-plane saxpy, output revisited in_ch*k*k times) is
// memory-bound and was measured ~20x slower at 512x512 tiles.
__DT_CLONE_TARGETS__
static void _conv2d(const nn_conv_t *cv, const float *in, int w, int h, int stride, int pad, float *out)
{
  const int k = cv->k;
  const int ow = (w + 2 * pad - k) / stride + 1;
  const int oh = (h + 2 * pad - k) / stride + 1;
  const size_t inhw = (size_t)w * h;
  const size_t wstride = (size_t)cv->in_ch * k * k; // weight step between output channels
#ifdef _OPENMP
#pragma omp parallel
#endif
  {
    float *const acc = malloc(sizeof(float) * NN_OC_BLOCK * ow);
#ifdef _OPENMP
#pragma omp for collapse(2) schedule(static)
#endif
    for(int oy = 0; oy < oh; oy++)
      for(int ocb = 0; ocb < cv->out_ch; ocb += NN_OC_BLOCK)
      {
        const int nb = NN_MIN(NN_OC_BLOCK, cv->out_ch - ocb);
        for(int r = 0; r < nb; r++)
        {
          float *const ar = acc + (size_t)r * ow;
          const float bias = cv->b[ocb + r];
          for(int ox = 0; ox < ow; ox++) ar[ox] = bias;
        }
        for(int ic = 0; ic < cv->in_ch; ic++)
        {
          const float *const ip = in + (size_t)ic * inhw;
          for(int ky = 0; ky < k; ky++)
          {
            const int iy = oy * stride + ky - pad;
            if(iy < 0 || iy >= h) continue;
            const float *const irow = ip + (size_t)iy * w;
            for(int kx = 0; kx < k; kx++)
            {
              const int shift = kx - pad;
              int ox0 = 0, ox1 = ow;
              while(ox0 < ow && ox0 * stride + shift < 0) ox0++;
              while(ox1 > ox0 && (ox1 - 1) * stride + shift >= w) ox1--;
              const float *const wbase = cv->w + ((size_t)ocb * cv->in_ch + ic) * k * k + ky * k + kx;

              if(nb == NN_OC_BLOCK)
              {
                const float w0 = wbase[0], w1 = wbase[wstride];
                const float w2 = wbase[2 * wstride], w3 = wbase[3 * wstride];
                float *const a0 = acc, *const a1 = acc + ow;
                float *const a2 = acc + 2 * ow, *const a3 = acc + 3 * ow;
                if(stride == 1)
                {
                  const float *const is = irow + shift;
#ifdef _OPENMP
#pragma omp simd
#endif
                  for(int ox = ox0; ox < ox1; ox++)
                  {
                    const float xv = is[ox];
                    a0[ox] += w0 * xv;
                    a1[ox] += w1 * xv;
                    a2[ox] += w2 * xv;
                    a3[ox] += w3 * xv;
                  }
                }
                else
                  for(int ox = ox0; ox < ox1; ox++)
                  {
                    const float xv = irow[ox * stride + shift];
                    a0[ox] += w0 * xv;
                    a1[ox] += w1 * xv;
                    a2[ox] += w2 * xv;
                    a3[ox] += w3 * xv;
                  }
              }
              else // remainder block (out_ch not a multiple of NN_OC_BLOCK, e.g. the head)
                for(int r = 0; r < nb; r++)
                {
                  const float wv = wbase[(size_t)r * wstride];
                  float *const ar = acc + (size_t)r * ow;
                  if(stride == 1)
                  {
                    const float *const is = irow + shift;
#ifdef _OPENMP
#pragma omp simd
#endif
                    for(int ox = ox0; ox < ox1; ox++) ar[ox] += wv * is[ox];
                  }
                  else
                    for(int ox = ox0; ox < ox1; ox++) ar[ox] += wv * irow[ox * stride + shift];
                }
            }
          }
        }
        for(int r = 0; r < nb; r++)
          memcpy(out + ((size_t)(ocb + r) * oh + oy) * ow, acc + (size_t)r * ow, sizeof(float) * ow);
      }
    free(acc);
  }
}

// exact GELU, matching pytorch nn.GELU(approximate='none')
__DT_CLONE_TARGETS__
static void _gelu(float *x, size_t n)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(size_t i = 0; i < n; i++) x[i] = 0.5f * x[i] * (1.0f + erff(x[i] * (float)M_SQRT1_2));
}


/* Peak live scratch of one net's forward, in floats: the ledger of the EXACT
 * allocate/free sequence of the forwards. cl_variant selects which one:
 * the CPU path (cl_variant 0) reads dec1's concat in place via _conv2d_cat2
 * and allocates neither the physical concat nor an upsample staging buffer;
 * the CL path (cl_variant 1) materializes both (cat + us). Any edit to either
 * forward must be reflected here, or the tiling engine plans against the
 * wrong number. */
static size_t _unet_peak_floats(const nn_unet_t *u, size_t wh, int cl_variant)
{
  const size_t base = (size_t)u->base;
  size_t live = 0, peak = 0, cur = 0;
#define NN_LEDGER(delta)                                                                                      \
  do                                                                                                          \
  {                                                                                                           \
    live += (delta);                                                                                          \
    if(live > peak) peak = live;                                                                              \
  } while(0)
  // encoder
  for(int l = 0; l < u->depth; l++)
  {
    const size_t lvl = base * wh >> l;
    NN_LEDGER(lvl);      // tmp
    NN_LEDGER(lvl);      // skips[l] — stays live until its decoder concat
    NN_LEDGER(lvl >> 2); // next
    live -= lvl;         // tmp freed
    live -= cur;         // previous level's input freed
    cur = lvl >> 2;
  }
  // bottleneck
  const size_t bot = base * wh >> u->depth;
  NN_LEDGER(bot);
  NN_LEDGER(bot);
  live -= bot;
  live -= cur;
  cur = bot;
  // decoder
  for(int i = 0; i < u->depth; i++)
  {
    const int l = u->depth - 1 - i;
    const size_t half = base * wh >> l;
    NN_LEDGER(half >> 2); // v (1x1 output on the coarse grid)
    live -= cur;          // cur freed
    if(cl_variant)
    {
      NN_LEDGER(2 * half); // cat
      live -= half;        // skips[l] freed after its copy
      NN_LEDGER(half);     // us
      live -= half >> 2;   // v freed
      live -= half;        // us freed
      NN_LEDGER(half);     // d1
      live -= 2 * half;    // cat freed
    }
    else
    {
      NN_LEDGER(half);     // d1 (dec1 reads skip + v in place)
      live -= half >> 2;   // v freed
      live -= half;        // skips[l] freed
    }
    NN_LEDGER(half); // new cur (dec2 output)
    live -= half;    // d1 freed
    cur = half;
  }
  NN_LEDGER((size_t)u->out_ch * wh); // head
#undef NN_LEDGER
  return peak;
}

static float _scratch_per_px(const dt_nn_model_t *m, int cl_variant)
{
  /* A large reference area keeps the integer ledger exact (every term is
   * base*wh >> k); the result is the dimensionless factor of the input image
   * size the tiling engine wants — never absolute bytes. The coarse and fine
   * stages never run concurrently (the caller frees the coarse buffers before
   * the fine forward), so the model peak is the MAX of the two, not the sum. */
  const size_t ref = (size_t)1 << 24;
  float per_px = (float)_unet_peak_floats(&m->fine, ref, cl_variant) / (float)ref;
  if(m->has_coarse)
  {
    const int bin = NN_MIN(m->bin_bayer, m->bin_xtrans); // smaller bin = larger coarse buffer
    const float coarse
        = (float)_unet_peak_floats(&m->coarse, ref / ((size_t)bin * bin), cl_variant) / (float)ref;
    if(coarse > per_px) per_px = coarse;
  }
  return per_px;
}

float dt_nn_unet_scratch_per_px(const dt_nn_model_t *m)
{
  return _scratch_per_px(m, 0);
}

float dt_nn_unet_scratch_per_px_cl(const dt_nn_model_t *m)
{
  return _scratch_per_px(m, 1);
}

float dt_nn_unet_scratch_maxblock_per_px(const dt_nn_model_t *m)
{
  /* Largest SINGLE scratch tensor per input pixel, host path: one base*wh
   * plane (dec1's output / a skip / an encoder tmp — all equal at level 0).
   * The pixelpipe arena serves contiguous runs and its address space is
   * partitioned by entries pinned during the pipe recursion, so the largest
   * tensor — not the total — is what an allocation can actually get; the
   * module's tiling_callback uses this to keep it comfortably below the
   * planned budget. */
  float per_px = (float)m->fine.base;
  if(m->has_coarse)
  {
    const int bin = NN_MIN(m->bin_bayer, m->bin_xtrans);
    const float coarse = (float)m->coarse.base / (float)(bin * bin);
    if(coarse > per_px) per_px = coarse;
  }
  return per_px;
}

size_t dt_nn_unet_scratch_bytes(const dt_nn_model_t *m, int width, int height)
{
  const size_t wh = (size_t)width * height;
  size_t floats = _unet_peak_floats(&m->fine, wh, 0);
  if(m->has_coarse)
  {
    const int bin = NN_MIN(m->bin_bayer, m->bin_xtrans);
    const size_t coarse = _unet_peak_floats(&m->coarse, wh / ((size_t)bin * bin), 0);
    if(coarse > floats) floats = coarse;
  }
  return floats * sizeof(float);
}

/* dec1 variant of _conv2d for k=3, stride=1, pad=1: the input is the channel
 * concat [a (in_ch_a channels, full res) | b (cv->in_ch - in_ch_a channels,
 * HALF resolution, read through a nearest-x2 upsample view)]. Reading b in
 * place is bit-identical to materializing upsample(b) and running _conv2d on
 * a physical concat — nearest upsampling replicates values, so every tap
 * reads the same number in the same accumulation order — but the 2*base*wh
 * concat tensor, the module's largest single allocation and therefore the
 * arena's contiguity bottleneck, never exists. Keep the accumulation order
 * identical to _conv2d: same (ic, ky, kx) nesting, same OC blocking. */
__DT_CLONE_TARGETS__
static void _conv2d_cat2(const nn_conv_t *cv, const float *a, int in_ch_a, const float *b, int w, int h,
                         float *out)
{
  const int k = cv->k; // always 3 here, kept general for the tap arithmetic
  const int pad = 1;
  const int ow = w, oh = h;
  const int bw = w / 2;
  const size_t inhw = (size_t)w * h;
  const size_t bhw = (size_t)bw * (h / 2);
  const size_t wstride = (size_t)cv->in_ch * k * k;
#ifdef _OPENMP
#pragma omp parallel
#endif
  {
    float *const acc = malloc(sizeof(float) * NN_OC_BLOCK * ow);
#ifdef _OPENMP
#pragma omp for collapse(2) schedule(static)
#endif
    for(int oy = 0; oy < oh; oy++)
      for(int ocb = 0; ocb < cv->out_ch; ocb += NN_OC_BLOCK)
      {
        const int nb = NN_MIN(NN_OC_BLOCK, cv->out_ch - ocb);
        for(int r = 0; r < nb; r++)
        {
          float *const ar = acc + (size_t)r * ow;
          const float bias = cv->b[ocb + r];
          for(int ox = 0; ox < ow; ox++) ar[ox] = bias;
        }
        for(int ic = 0; ic < cv->in_ch; ic++)
        {
          const int from_b = ic >= in_ch_a;
          const float *const ip = from_b ? b + (size_t)(ic - in_ch_a) * bhw : a + (size_t)ic * inhw;
          for(int ky = 0; ky < k; ky++)
          {
            const int iy = oy + ky - pad;
            if(iy < 0 || iy >= h) continue;
            const float *const irow = from_b ? ip + (size_t)(iy >> 1) * bw : ip + (size_t)iy * w;
            for(int kx = 0; kx < k; kx++)
            {
              const int shift = kx - pad;
              int ox0 = 0, ox1 = ow;
              while(ox0 < ow && ox0 + shift < 0) ox0++;
              while(ox1 > ox0 && (ox1 - 1) + shift >= w) ox1--;
              const float *const wbase = cv->w + ((size_t)ocb * cv->in_ch + ic) * k * k + ky * k + kx;
              if(nb == NN_OC_BLOCK)
              {
                // 4-way output-channel blocking, mirroring _conv2d's fast
                // path: one pass over the row feeds four accumulators
                const float w0 = wbase[0], w1 = wbase[wstride];
                const float w2 = wbase[2 * wstride], w3 = wbase[3 * wstride];
                float *const a0 = acc, *const a1 = acc + ow;
                float *const a2 = acc + 2 * ow, *const a3 = acc + 3 * ow;
                if(from_b)
                  for(int ox = ox0; ox < ox1; ox++)
                  {
                    const float xv = irow[(ox + shift) >> 1];
                    a0[ox] += w0 * xv;
                    a1[ox] += w1 * xv;
                    a2[ox] += w2 * xv;
                    a3[ox] += w3 * xv;
                  }
                else
                {
                  const float *const is = irow + shift;
#ifdef _OPENMP
#pragma omp simd
#endif
                  for(int ox = ox0; ox < ox1; ox++)
                  {
                    const float xv = is[ox];
                    a0[ox] += w0 * xv;
                    a1[ox] += w1 * xv;
                    a2[ox] += w2 * xv;
                    a3[ox] += w3 * xv;
                  }
                }
              }
              else
                for(int r = 0; r < nb; r++)
                {
                  const float wr = wbase[(size_t)r * wstride];
                  float *const ar = acc + (size_t)r * ow;
                  if(from_b)
                    for(int ox = ox0; ox < ox1; ox++) ar[ox] += wr * irow[(ox + shift) >> 1];
                  else
                  {
                    const float *const is = irow + shift;
#ifdef _OPENMP
#pragma omp simd
#endif
                    for(int ox = ox0; ox < ox1; ox++) ar[ox] += wr * is[ox];
                  }
                }
            }
          }
        }
        for(int r = 0; r < nb; r++)
          memcpy(out + ((size_t)(ocb + r)) * inhw + (size_t)oy * ow, acc + (size_t)r * ow,
                 sizeof(float) * ow);
      }
    free(acc);
  }
}

/* Full forward pass of one U-Net.
 *
 * Memory discipline — this is where the module's RAM peak lives, so every
 * tensor is allocated at its EXACT size the moment it is needed and released
 * on its last use, instead of the former three worst-case ping-pong arenas
 * plus all skips held to the end (7.9*base*wh floats live). The live-set peak
 * is now dec1 at level 0: ~2.25*base*wh floats, with the largest single
 * allocation ONE base*wh plane — the binding constraint for the pixelpipe
 * arena, which serves contiguous runs. _unet_peak_floats()
 * is the ledger of this exact sequence — KEEP THE TWO IN SYNC.
 *
 * The 1x1 up-convs are applied BEFORE their nearest x2 upsample: a 1x1 conv
 * is per-pixel and nearest upsampling replicates pixels, so conv(up(x)) and
 * up(conv(x)) are the same values from the same FP operations — but computed
 * on 4x fewer pixels, and the (2*w_skip)@full-res tensor never exists.
 *
 * residual_ch > 0 subtracts the head from the input's first residual_ch
 * planes; residual_ch == 0 writes the raw head output. */
static int _unet_forward(const nn_unet_t *u, const float *in, float *out, int width, int height, int residual_ch)
{
  const int align = 1 << u->depth;
  if(width % align || height % align || width <= 0 || height <= 0) return 1;

  const size_t wh = (size_t)width * height;
  const size_t base = (size_t)u->base;
  float *skips[NN_MAX_DEPTH] = { NULL };

  // encoder: skip[l] = (base<<l) channels at (wh >> 2l) px = base*wh >> l floats
  const float *src = in;
  int cw = width, chh = height;
  float *cur = NULL;
  int ok = 1;
  for(int l = 0; l < u->depth && ok; l++)
  {
    const size_t lvl = base * wh >> l;
    float *tmp = _nn_alloc(lvl, 0);
    skips[l] = _nn_alloc(lvl, 1);
    float *next = _nn_alloc(lvl >> 2, 0);
    if(!tmp || !skips[l] || !next)
    {
      _nn_free(tmp);
      _nn_free(next);
      ok = 0;
      break;
    }
    _conv2d(&u->enc1[l], src, cw, chh, 1, 1, tmp);
    _gelu(tmp, (size_t)u->enc1[l].out_ch * cw * chh);
    _conv2d(&u->enc2[l], tmp, cw, chh, 1, 1, skips[l]);
    _gelu(skips[l], (size_t)u->enc2[l].out_ch * cw * chh);
    _nn_free(tmp);
    _conv2d(&u->down[l], skips[l], cw, chh, 2, 0, next);
    _nn_free(cur); // level l's input, dead now (never frees `in`: cur is NULL then)
    cur = next;
    cw /= 2;
    chh /= 2;
    src = cur;
  }

  // bottleneck: (base<<depth) channels at wh >> 2*depth px
  if(ok)
  {
    const size_t bot = base * wh >> u->depth;
    float *tmp = _nn_alloc(bot, 0);
    float *bout = _nn_alloc(bot, 0);
    if(!tmp || !bout)
    {
      _nn_free(tmp);
      _nn_free(bout);
      ok = 0;
    }
    else
    {
      _conv2d(&u->bot1, src, cw, chh, 1, 1, tmp);
      _gelu(tmp, (size_t)u->bot1.out_ch * cw * chh);
      _conv2d(&u->bot2, tmp, cw, chh, 1, 1, bout);
      _gelu(bout, (size_t)u->bot2.out_ch * cw * chh);
      _nn_free(tmp);
      _nn_free(cur);
      cur = bout;
    }
  }

  // decoder: up.i / dec.i #i pairs with encoder level (depth-1-i). The 1x1
  // up-conv runs on the coarse grid (see the doc comment), and dec1 reads its
  // concat input IN PLACE via _conv2d_cat2 — skip at full resolution, the 1x1
  // output through a nearest-upsample view — so the module's former largest
  // tensor (the physical 2*w_skip concat) is never allocated at all.
  for(int i = 0; i < u->depth && ok; i++)
  {
    const int l = u->depth - 1 - i;
    const size_t w_skip = base << l;
    const size_t half = w_skip * (size_t)(2 * cw) * (size_t)(2 * chh); // one concat half
    float *v = _nn_alloc(half >> 2, 1); // top end: must not split the big-tensor churn area
    if(!v) { ok = 0; break; }
    _conv2d(&u->up[i], cur, cw, chh, 1, 0, v);
    _nn_free(cur);
    cur = NULL;
    cw *= 2;
    chh *= 2;
    float *d1 = _nn_alloc(half, 0);
    if(!d1) { _nn_free(v); ok = 0; break; }
    _conv2d_cat2(&u->dec1[i], skips[l], (int)w_skip, v, cw, chh, d1);
    _gelu(d1, w_skip * (size_t)cw * chh);
    _nn_free(v);
    _nn_free(skips[l]);
    skips[l] = NULL;
    float *d2 = _nn_alloc(half, 0);
    if(!d2) { _nn_free(d1); ok = 0; break; }
    _conv2d(&u->dec2[i], d1, cw, chh, 1, 1, d2);
    _gelu(d2, w_skip * (size_t)cw * chh);
    _nn_free(d1);
    cur = d2;
  }

  if(ok)
  {
    float *head = _nn_alloc((size_t)u->out_ch * wh, 0);
    if(!head)
      ok = 0;
    else
    {
      _conv2d(&u->head, cur, width, height, 1, 1, head);
      if(residual_ch > 0)
      {
        // residual head: out = input planes - predicted noise
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
        for(size_t i = 0; i < (size_t)residual_ch * wh; i++) out[i] = in[i] - head[i];
      }
      else
        memcpy(out, head, (size_t)u->out_ch * wh * sizeof(float));
      _nn_free(head);
    }
  }

  for(int l = 0; l < u->depth; l++) _nn_free(skips[l]);
  _nn_free(cur);
  return ok ? 0 : 1;
}

int dt_nn_unet_apply(const dt_nn_model_t *m, const float *in, float *out, int width, int height)
{
  return _unet_forward(&m->fine, in, out, width, height, m->fine.out_ch);
}

int dt_nn_unet_apply_stage(const dt_nn_model_t *m, int stage, const float *in, float *out, int width,
                           int height, int apply_residual)
{
  if(stage == 1)
  {
    if(!m->has_coarse) return 1;
    // coarse stage: the head predicts a correction to its RGB planes
    return _unet_forward(&m->coarse, in, out, width, height, apply_residual ? m->coarse.out_ch : 0);
  }
  return _unet_forward(&m->fine, in, out, width, height, apply_residual ? m->fine.out_ch : 0);
}

__DT_CLONE_TARGETS__
void dt_nn_bin_planes(const float *planes, int pw, int ph, int bin, float *out_rgb, float *out_cnt)
{
  // planes = [mosaic, onehot_R, onehot_G, onehot_B, ...] as assembled for the
  // fine net. Each coarse pixel is the count-weighted mean of the block's
  // same-channel sensels — the exact contract of cfa.bin_mosaic_torch in the
  // training repo. With bin 4 (Bayer) / 6 (X-Trans) every count is > 0 by
  // construction; the max() is a numerical guard, not a fallback.
  const size_t plane = (size_t)pw * ph;
  const int cw = pw / bin, chh = ph / bin;
  const float *const mosaic = planes;
#ifdef _OPENMP
#pragma omp parallel for schedule(static) collapse(2)
#endif
  for(int c = 0; c < 3; c++)
    for(int cy = 0; cy < chh; cy++)
    {
      const float *const onehot = planes + (size_t)(1 + c) * plane;
      float *const orow = out_rgb + (size_t)c * cw * chh + (size_t)cy * cw;
      float *const crow = out_cnt + (size_t)c * cw * chh + (size_t)cy * cw;
      for(int cx = 0; cx < cw; cx++)
      {
        float sum = 0.0f, cnt = 0.0f;
        for(int y = cy * bin; y < (cy + 1) * bin; y++)
          for(int x = cx * bin; x < (cx + 1) * bin; x++)
          {
            const size_t i = (size_t)y * pw + x;
            sum += mosaic[i] * onehot[i];
            cnt += onehot[i];
          }
        crow[cx] = cnt;
        orow[cx] = sum / (cnt > 1.0f ? cnt : 1.0f);
      }
    }
}

__DT_CLONE_TARGETS__
void dt_nn_upsample_nearest(const float *in, int ch, int w, int h, int factor, float *out)
{
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
  for(int c = 0; c < ch; c++)
  {
    const float *const ip = in + (size_t)c * w * h;
    float *const op = out + (size_t)c * w * h * factor * factor;
    for(int y = 0; y < h * factor; y++)
    {
      const float *const irow = ip + (size_t)(y / factor) * w;
      float *const orow = op + (size_t)y * w * factor;
      for(int x = 0; x < w * factor; x++) orow[x] = irow[x / factor];
    }
  }
}

/* ------------------------------------------------------------------------
 * OpenCL executor
 * ------------------------------------------------------------------------ */
#ifdef HAVE_OPENCL

struct dt_nn_cl_t
{
  int kernel_conv;
  int kernel_conv3x3;
  int kernel_upsample;
};

dt_nn_cl_t *dt_nn_cl_create(int program)
{
  dt_nn_cl_t *cl = calloc(1, sizeof(dt_nn_cl_t));
  if(!cl) return NULL;
  cl->kernel_conv = dt_opencl_create_kernel(program, "nn_conv");
  cl->kernel_conv3x3 = dt_opencl_create_kernel(program, "nn_conv3x3");
  cl->kernel_upsample = dt_opencl_create_kernel(program, "nn_upsample");
  return cl;
}

void dt_nn_cl_destroy(dt_nn_cl_t *cl)
{
  if(!cl) return;
  dt_opencl_free_kernel(cl->kernel_conv);
  dt_opencl_free_kernel(cl->kernel_conv3x3);
  dt_opencl_free_kernel(cl->kernel_upsample);
  free(cl);
}


// upload the weight blob to the device once, cached per device
static cl_mem _weights_cl(const dt_nn_model_t *m, int devid)
{
  if(devid < 0 || devid >= NN_MAX_DEVICES) return NULL;
  dt_nn_model_t *mm = (dt_nn_model_t *)m; // cache mutation on a logically const model
  dt_pthread_mutex_lock(&mm->cl_lock);
  if(!mm->dev_weights[devid])
  {
    const size_t bytes = m->blob_floats * sizeof(float);
    cl_mem buf = dt_opencl_alloc_device_buffer(devid, bytes);
    if(buf && dt_opencl_write_buffer_to_device(devid, m->blob, buf, 0, bytes, CL_TRUE) == CL_SUCCESS)
      mm->dev_weights[devid] = buf;
    else if(buf)
      dt_opencl_release_mem_object(buf);
  }
  cl_mem w = mm->dev_weights[devid];
  dt_pthread_mutex_unlock(&mm->cl_lock);
  return w;
}

// enqueue one convolution (+ optional GELU) reading from `in`, writing `out`
static int _conv_cl(dt_nn_cl_t *cl, int devid, cl_mem weights, const float *blob_base, cl_mem in, cl_mem out,
                    int w, int h, const nn_conv_t *cv, int stride, int pad, int do_gelu)
{
  const int ow = (w + 2 * pad - cv->k) / stride + 1;
  const int oh = (h + 2 * pad - cv->k) / stride + 1;
  const int weight_off = (int)(cv->w - blob_base);
  const int bias_off = (int)(cv->b - blob_base);
  const size_t slice_bytes = sizeof(float) * 4 * cv->in_ch * cv->k * cv->k;
  const int use_local = slice_bytes <= (30 << 10);
  const size_t lx = 128;

  if(cv->k == 3 && stride == 1)
  {
    // fast quad kernel: 2x2 output pixels per work-item (see rawdenoiseai.cl).
    // Weights are staged in local memory `chunk` input channels at a time so
    // even the in_ch 256/512 layers never read weights per-item from global
    // (~24 KB of local keeps 2 work-groups per SM on common GPUs).
    const int chunk = NN_MIN(cv->in_ch, (24 << 10) / (int)(sizeof(float) * 4 * 9));
    const int K3 = cl->kernel_conv3x3;
    dt_opencl_set_kernel_arg(devid, K3, 0, sizeof(cl_mem), &in);
    dt_opencl_set_kernel_arg(devid, K3, 1, sizeof(cl_mem), &weights);
    dt_opencl_set_kernel_arg(devid, K3, 2, sizeof(cl_mem), &out);
    dt_opencl_set_kernel_arg(devid, K3, 3, sizeof(int), &w);
    dt_opencl_set_kernel_arg(devid, K3, 4, sizeof(int), &h);
    dt_opencl_set_kernel_arg(devid, K3, 5, sizeof(int), &cv->in_ch);
    dt_opencl_set_kernel_arg(devid, K3, 6, sizeof(int), &cv->out_ch);
    dt_opencl_set_kernel_arg(devid, K3, 7, sizeof(int), &weight_off);
    dt_opencl_set_kernel_arg(devid, K3, 8, sizeof(int), &bias_off);
    dt_opencl_set_kernel_arg(devid, K3, 9, sizeof(int), &do_gelu);
    dt_opencl_set_kernel_arg(devid, K3, 10, sizeof(int), &chunk);
    dt_opencl_set_kernel_arg(devid, K3, 11, sizeof(float) * 4 * 9 * chunk, NULL);
    const size_t quads = (size_t)((w + 1) / 2) * ((h + 1) / 2);
    size_t sizes3[3] = { (quads + lx - 1) / lx * lx, ((size_t)cv->out_ch + 3) / 4, 1 };
    size_t local3[3] = { lx, 1, 1 };
    return dt_opencl_enqueue_kernel_2d_with_local(devid, K3, sizes3, local3);
  }

  const int K = cl->kernel_conv;
  dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &in);
  dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &weights);
  dt_opencl_set_kernel_arg(devid, K, 2, sizeof(cl_mem), &out);
  dt_opencl_set_kernel_arg(devid, K, 3, sizeof(int), &w);
  dt_opencl_set_kernel_arg(devid, K, 4, sizeof(int), &h);
  dt_opencl_set_kernel_arg(devid, K, 5, sizeof(int), &ow);
  dt_opencl_set_kernel_arg(devid, K, 6, sizeof(int), &oh);
  dt_opencl_set_kernel_arg(devid, K, 7, sizeof(int), &cv->in_ch);
  dt_opencl_set_kernel_arg(devid, K, 8, sizeof(int), &cv->out_ch);
  dt_opencl_set_kernel_arg(devid, K, 9, sizeof(int), &cv->k);
  dt_opencl_set_kernel_arg(devid, K, 10, sizeof(int), &stride);
  dt_opencl_set_kernel_arg(devid, K, 11, sizeof(int), &pad);
  dt_opencl_set_kernel_arg(devid, K, 12, sizeof(int), &weight_off);
  dt_opencl_set_kernel_arg(devid, K, 13, sizeof(int), &bias_off);
  dt_opencl_set_kernel_arg(devid, K, 14, sizeof(int), &do_gelu);
  // 4 output channels per work-item (NN_OCB in the kernel); their weight
  // slices are staged in local memory when they fit the conservative 30 KB
  // budget — always true for the wide shallow layers where weight traffic
  // matters. (Staging the INPUT rows in local memory was tried and measured
  // slower: with the output-channel blocking in work dim 1, every oc-block
  // group re-stages the same rows and the per-channel barriers serialize,
  // while the L2 already absorbs the 3x3 neighbourhood overlap.)
  dt_opencl_set_kernel_arg(devid, K, 15, sizeof(int), &use_local);
  dt_opencl_set_kernel_arg(devid, K, 16, use_local ? slice_bytes : sizeof(float), NULL);
  size_t sizes[3] = { ((size_t)ow * oh + lx - 1) / lx * lx, ((size_t)cv->out_ch + 3) / 4, 1 };
  size_t local[3] = { lx, 1, 1 };
  return dt_opencl_enqueue_kernel_2d_with_local(devid, K, sizes, local);
}

static int _upsample_cl(dt_nn_cl_t *cl, int devid, cl_mem in, cl_mem out, int w, int h, int ch)
{
  const int K = cl->kernel_upsample;
  dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &in);
  dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &out);
  dt_opencl_set_kernel_arg(devid, K, 2, sizeof(int), &w);
  dt_opencl_set_kernel_arg(devid, K, 3, sizeof(int), &h);
  dt_opencl_set_kernel_arg(devid, K, 4, sizeof(int), &ch);
  size_t sizes[3] = { ROUNDUPDWD((size_t)4 * w * h, devid), ROUNDUPDHT(ch, devid), 1 };
  return dt_opencl_enqueue_kernel_2d(devid, K, sizes);
}

// forward one U-Net on the device. Writes the RAW head output (no residual):
// the fine stage's residual is applied by the caller after readback (the
// historic CPU/CL asymmetry), and the coarse stage's residual is applied
// host-side too so both paths share the same subtraction code bit-for-bit.
/* Device twin of _unet_forward: the same live-set discipline — every buffer
 * allocated at its exact size when needed and released on last use, the same
 * 1x1-before-upsample commutation — so VRAM peaks at the level-0 concat
 * (~3.25*base*wh floats) instead of holding three worst-case arenas plus
 * every skip to the end. All temporaries live at function scope and are released in
 * cleanup, so a device-OOM mid-sequence (the very case this path exists for:
 * fall back to CPU on a full card) cannot leak. Releases happen at enqueue
 * time and the runtime defers destruction until in-flight commands finish, so
 * on drivers that commit at clCreateBuffer the instantaneous footprint can
 * transiently exceed the ledger by the pending-release set; the factor_cl
 * headroom absorbs that, and the failure mode is the graceful CPU fallback. _unet_peak_floats() is the
 * shared ledger of this sequence: KEEP ALL THREE IN SYNC. */
static int _unet_forward_cl(const dt_nn_model_t *m, const nn_unet_t *u, dt_nn_cl_t *cl, int devid, cl_mem dev_in,
                            cl_mem dev_out, int width, int height)
{
  const int align = 1 << u->depth;
  if(width % align || height % align || width <= 0 || height <= 0) return -1;

  cl_mem weights = _weights_cl(m, devid);
  if(!weights) return -1;

  const size_t wh = (size_t)width * height;
  const size_t base = (size_t)u->base;
  int err = CL_SUCCESS;
  cl_mem skips[NN_MAX_DEPTH] = { NULL };
  cl_mem cur = NULL, tmp = NULL, v = NULL, cat = NULL, us = NULL, d1 = NULL;

#define NN_CL_ALLOC(var, floats)                                                                              \
  do                                                                                                          \
  {                                                                                                           \
    var = dt_opencl_alloc_device_buffer(devid, (floats) * sizeof(float));                                     \
    if(!var)                                                                                                  \
    {                                                                                                         \
      err = -1;                                                                                               \
      goto cleanup;                                                                                           \
    }                                                                                                         \
  } while(0)
#define NN_CL_FREE(var)                                                                                       \
  do                                                                                                          \
  {                                                                                                           \
    if(var) dt_opencl_release_mem_object(var);                                                                \
    var = NULL;                                                                                               \
  } while(0)

  // encoder
  cl_mem src = dev_in;
  int cw = width, chh = height;
  for(int l = 0; l < u->depth && err == CL_SUCCESS; l++)
  {
    const size_t lvl = base * wh >> l;
    cl_mem next = NULL;
    NN_CL_ALLOC(tmp, lvl);
    NN_CL_ALLOC(skips[l], lvl);
    NN_CL_ALLOC(next, lvl >> 2);
    err |= _conv_cl(cl, devid, weights, m->blob, src, tmp, cw, chh, &u->enc1[l], 1, 1, 1);
    err |= _conv_cl(cl, devid, weights, m->blob, tmp, skips[l], cw, chh, &u->enc2[l], 1, 1, 1);
    NN_CL_FREE(tmp);
    err |= _conv_cl(cl, devid, weights, m->blob, skips[l], next, cw, chh, &u->down[l], 2, 0, 0);
    NN_CL_FREE(cur); // level l's input; never dev_in (cur is NULL on l == 0)
    cur = next;
    cw /= 2;
    chh /= 2;
    src = cur;
  }

  // bottleneck (bout reuses the `v` slot so cleanup covers it)
  if(err == CL_SUCCESS)
  {
    const size_t bot = base * wh >> u->depth;
    NN_CL_ALLOC(tmp, bot);
    NN_CL_ALLOC(v, bot);
    err |= _conv_cl(cl, devid, weights, m->blob, src, tmp, cw, chh, &u->bot1, 1, 1, 1);
    err |= _conv_cl(cl, devid, weights, m->blob, tmp, v, cw, chh, &u->bot2, 1, 1, 1);
    NN_CL_FREE(tmp);
    NN_CL_FREE(cur);
    cur = v;
    v = NULL;
  }

  // decoder: up.i / dec.i pair with encoder level l = depth-1-i; the 1x1
  // up-conv runs on the coarse grid, then upsamples (see the doc comment)
  for(int i = 0; i < u->depth && err == CL_SUCCESS; i++)
  {
    const int l = u->depth - 1 - i;
    const size_t w_skip = base << l;
    const size_t half = w_skip * (size_t)(2 * cw) * (size_t)(2 * chh);
    NN_CL_ALLOC(v, half >> 2);
    err |= _conv_cl(cl, devid, weights, m->blob, cur, v, cw, chh, &u->up[i], 1, 0, 0);
    NN_CL_FREE(cur);
    NN_CL_ALLOC(cat, 2 * half);
    err |= dt_opencl_enqueue_copy_buffer_to_buffer(devid, skips[l], cat, 0, 0, half * sizeof(float));
    NN_CL_FREE(skips[l]);
    NN_CL_ALLOC(us, half);
    err |= _upsample_cl(cl, devid, v, us, cw, chh, u->up[i].out_ch);
    NN_CL_FREE(v);
    err |= dt_opencl_enqueue_copy_buffer_to_buffer(devid, us, cat, 0, half * sizeof(float),
                                                   half * sizeof(float));
    NN_CL_FREE(us);
    cw *= 2;
    chh *= 2;
    NN_CL_ALLOC(d1, half);
    err |= _conv_cl(cl, devid, weights, m->blob, cat, d1, cw, chh, &u->dec1[i], 1, 1, 1);
    NN_CL_FREE(cat);
    NN_CL_ALLOC(cur, half);
    err |= _conv_cl(cl, devid, weights, m->blob, d1, cur, cw, chh, &u->dec2[i], 1, 1, 1);
    NN_CL_FREE(d1);
  }

  // head: raw prediction (no activation) into dev_out
  if(err == CL_SUCCESS)
    err |= _conv_cl(cl, devid, weights, m->blob, cur, dev_out, width, height, &u->head, 1, 1, 0);

cleanup:
  for(int l = 0; l < u->depth; l++)
    if(skips[l]) dt_opencl_release_mem_object(skips[l]);
  NN_CL_FREE(cur);
  NN_CL_FREE(tmp);
  NN_CL_FREE(v);
  NN_CL_FREE(cat);
  NN_CL_FREE(us);
  NN_CL_FREE(d1);
  return err;
#undef NN_CL_ALLOC
#undef NN_CL_FREE
}

int dt_nn_unet_apply_stage_cl(const dt_nn_model_t *m, int stage, dt_nn_cl_t *cl, int devid, cl_mem dev_in,
                              cl_mem dev_out, int width, int height)
{
  if(stage == 1)
  {
    if(!m->has_coarse) return -1;
    return _unet_forward_cl(m, &m->coarse, cl, devid, dev_in, dev_out, width, height);
  }
  return _unet_forward_cl(m, &m->fine, cl, devid, dev_in, dev_out, width, height);
}

#endif // HAVE_OPENCL
