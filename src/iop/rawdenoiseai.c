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

/*
 * Neural raw (CFA-domain) denoiser.
 *
 * Runs a sigma-map-conditioned U-Net (common/nn_model.{h,c}) on the mosaiced
 * buffer, before demosaicing, where sensor noise is still per-sensel
 * independent. The network was trained on synthetic Poisson-Gaussian noise
 * drawn from the community noise-profile database, conditioned on the
 * per-pixel noise standard deviation — so one set of weights serves every
 * profiled camera, Bayer and X-Trans alike, and a newly profiled camera is
 * supported without retraining. Training pipeline:
 * https://github.com/aurelienpierreeng/ansel-denoise
 *
 * The sigma map is computed from the per-channel variance line
 * Var(x) = a*x + b of the matched noise profile at the image ISO, applied in
 * the post-rawprepare normalized domain — the exact domain the profiles were
 * fitted in and the training used. Do NOT copy denoiseprofile's white-balance
 * adjustments here: those compensate its post-demosaic, post-WB position.
 *
 * User parameters:
 * - "strength": opacity of the correction, an alpha blend of the inferred
 *   noise residual: out = in + strength * (denoised - in).
 * - model version / size (large|half|quarter network width) / variant (single-scale
 *   or multiscale) select the weights file
 *   denoise-<size>-<single|multi>-<version>.anselnn.
 * - "global correction" and the per-channel R/G/B corrections scale the
 *   sigma map (see the calibration note above the params struct).
 *
 * Weights are loaded once per session from the user config dir (override for
 * testing) or <datadir>/. Without a weights file the module stays disabled.
 * The training counterpart of every inference step lives in the
 * ansel-denoise repository: _k_assemble() <-> dataset.py,
 * _k_bin_planes() <-> cfa.bin_mosaic_torch()/bin_sigma_torch(),
 * the coarse->fine guide flow <-> train.ms_forward(), and
 * _apply_low_band_anchor() <-> cfa.fuse_low_bands(). Keep them in sync.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "widgets/bauhaus.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "system/macros.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/module_versioning.h"
#include <json-glib/json-glib.h>
#include "caches/pixelpipe_cache_alloc.h"
#include "common/file_location.h"
#include "common/imagebuf.h"
#include "common/nn_model.h"
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/imageop_math.h"
#include "develop/tiling.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include "widgets/collapsible_section.h"
#include "widgets/label.h"

#define DT_RAWDENOISEAI_MODEL_LEN 128

DT_MODULE_INTROSPECTION(1, dt_iop_rawdenoiseai_params_t)

/* Model version: pins which trained network a history entry uses, so results
 * stay reproducible across app updates. A new release that retrains the net
 * appends a value here (and ships the matching weights file) instead of
 * replacing v1 — old edits keep rendering with the model they were made on. */
typedef enum dt_iop_rawdenoiseai_version_t
{
  DT_RAWDENOISEAI_V1 = 0, // $DESCRIPTION: "v1"
} dt_iop_rawdenoiseai_version_t;

/* Model size: same architecture family, different width of the FINE net
 * (32 / 16 / 8; the coarse chroma net of a multiscale model stays at 32 for
 * every size — it runs on the superpixel-binned image and costs a few
 * percent). large is the reference quality (practical with OpenCL), half is
 * ~4x cheaper for the CPU path, quarter ~4x cheaper again for very weak
 * hardware and near-realtime editing. The outputs are NOT interchangeable,
 * hence a user parameter rather than a silent runtime choice. */
typedef enum dt_iop_rawdenoiseai_size_t
{
  DT_RAWDENOISEAI_LARGE = 0,   // $DESCRIPTION: "large"
  DT_RAWDENOISEAI_HALF = 1,    // $DESCRIPTION: "half"
  DT_RAWDENOISEAI_QUARTER = 2, // $DESCRIPTION: "quarter"
} dt_iop_rawdenoiseai_size_t;

/* Model variant: single-scale (the fine mosaic net alone — fast, no
 * low-frequency chroma handling) vs multiscale (coarse chroma net guiding
 * the fine net, plus the hybrid low-band fusion — high quality). */
typedef enum dt_iop_rawdenoiseai_scale_t
{
  DT_RAWDENOISEAI_SINGLE = 0, // $DESCRIPTION: "single-scale"
  DT_RAWDENOISEAI_MULTI = 1,  // $DESCRIPTION: "multiscale"
} dt_iop_rawdenoiseai_scale_t;

#define DT_RAWDENOISEAI_NUM_VERSIONS 1
#define DT_RAWDENOISEAI_NUM_SIZES 3
#define DT_RAWDENOISEAI_NUM_SCALES 2

// filename components per enum value; the weights file is
// denoise-<size>-<single|multi>-<version>.anselnn
static const char *const _version_tag[DT_RAWDENOISEAI_NUM_VERSIONS] = { "v1" };
static const char *const _size_tag[DT_RAWDENOISEAI_NUM_SIZES] = { "large", "half", "quarter" };
static const char *const _scale_tag[DT_RAWDENOISEAI_NUM_SCALES] = { "single", "multi" };

/* The shipped noise profiles understate the true mosaic-domain sigma by an
 * exact factor 2 before any demosaic effect: tools/noise/noiseprofile.c
 * estimates sigma as MAD/0.6745 of the HH band of a decimated lifting Haar
 * whose normalization is HH = (x00 - x01 - x10 + x11)/4, so std(HH) = sigma/2
 * for iid noise (the orthonormal Haar assumed by the MAD rule divides by 2).
 * The gnuplot fit in ansel-gen-noiseprofile squares that std into (a, b)
 * without correction, so every profile carries 1/4 of the physical variance.
 * Historical consumers (denoiseprofile) were tuned end to end around these
 * units; this module is the first to treat (a, b) as absolute physical
 * variance, so the correction lives here — the shared profile database must
 * stay consistent with a decade of fits and cannot change.
 *
 * The remaining, channel-dependent part of the deviation (the profiles are
 * fitted on demosaiced pixels, and interpolation averages away high-frequency
 * noise — most on the dense green lattice) is exposed as the per-channel
 * corrections below, calibrated by measuring flat-region noise on raw mosaics
 * against the profile prediction: 253 profiled cameras (one raw.pixls.us
 * sample each) plus 64 images across ISO 64-12800 on three local bodies.
 * Cross-camera medians (estimator-bias corrected): R 1.41, G 1.97, B 1.48
 * after the factor 2 above; the deviation is ISO-stable.
 *
 * The whole correction is carried by the three GUI sliders and nothing
 * else: what the user sees is exactly what multiplies the profile sigma
 * (times the global correction). No hidden constants, no model-side
 * multiplication — a model's cfg may document the sigma convention it was
 * trained under, but the module never applies it behind the user's back
 * (a hidden factor stacked with visible sliders is how the calibration got
 * silently applied twice — the yellow-cast field bug). The slider defaults
 * are the calibration itself: 2 x the sweep medians above. */

typedef struct dt_iop_rawdenoiseai_params_t
{
  float strength;                            // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.85 $DESCRIPTION: "strength"
  dt_iop_rawdenoiseai_version_t version;     // $DEFAULT: DT_RAWDENOISEAI_V1 $DESCRIPTION: "model version"
  dt_iop_rawdenoiseai_size_t size;           // $DEFAULT: DT_RAWDENOISEAI_QUARTER $DESCRIPTION: "model size"
  float noise_level;                         // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "global correction"
  float sigma_red;                           // $MIN: 0.5 $MAX: 8.0 $DEFAULT: 2.82 $DESCRIPTION: "red correction"
  float sigma_green;                         // $MIN: 0.5 $MAX: 8.0 $DEFAULT: 3.94 $DESCRIPTION: "green correction"
  float sigma_blue;                          // $MIN: 0.5 $MAX: 8.0 $DEFAULT: 2.96 $DESCRIPTION: "blue correction"
  dt_iop_rawdenoiseai_scale_t scale_variant; // $DEFAULT: DT_RAWDENOISEAI_MULTI $DESCRIPTION: "model variant"
  /* Empty: use the shipped model selected by (version, size, scale) above.
   * Otherwise the basename of a .anselnn in the user config dir, which
   * overrides all three. Stored by NAME, never by list position: the set of
   * files on disk changes between sessions, and an index would silently
   * re-point every history entry that used it (same reason colorin stores
   * its ICC filename). */
  char custom_model[DT_RAWDENOISEAI_MODEL_LEN];
} dt_iop_rawdenoiseai_params_t;

typedef struct dt_iop_rawdenoiseai_gui_data_t
{
  GtkWidget *strength;
  GtkWidget *noise_level;
  GtkWidget *sigma_red;
  GtkWidget *sigma_green;
  GtkWidget *sigma_blue;
  dt_gui_collapsible_section_t cs; // per-channel corrections
  GtkWidget *version;
  GtkWidget *size;
  GtkWidget *scale_variant;
  GtkWidget *custom_model;
  GtkWidget *profile_label;
} dt_iop_rawdenoiseai_gui_data_t;

typedef struct dt_iop_rawdenoiseai_data_t
{
  float strength;       // opacity of the correction: out = in + strength * (denoised - in)
  float noise_level;    // scales the sigma map fed to the network (1.0 = trust the profile)
  float sigma_scale[3]; // per-channel demosaic-bias correction, applied on top of noise_level
  float a[3], b[3];     // noise variance line per RGB channel, normalized domain
  dt_nn_model_t *model; // resolved from (version, variant); NULL disables the piece
} dt_iop_rawdenoiseai_data_t;

typedef struct dt_iop_rawdenoiseai_global_data_t
{
  // lazily loaded, cached for the session; guarded by lock. tried[][] records
  // a load attempt so a missing file is probed only once.
  dt_nn_model_t *models[DT_RAWDENOISEAI_NUM_VERSIONS][DT_RAWDENOISEAI_NUM_SIZES][DT_RAWDENOISEAI_NUM_SCALES];
  gboolean tried[DT_RAWDENOISEAI_NUM_VERSIONS][DT_RAWDENOISEAI_NUM_SIZES][DT_RAWDENOISEAI_NUM_SCALES];
  // user models from the config dir, keyed by basename; a NULL value records
  // a failed load so a broken file is probed once, like tried[][] above
  GHashTable *custom;
  dt_pthread_mutex_t lock;
#ifdef HAVE_OPENCL
  dt_nn_cl_t *nn_cl; // U-Net kernel handles, from rawdenoiseai.cl
  // device-resident glue kernels: the whole tile runs dev_in -> dev_out with
  // no mid-tile host round-trip (command-queue syncs dominate GPU cost)
  int k_assemble, k_bin_planes, k_residual, k_upsample_n, k_bin16_mdv, k_avg2x2;
  int k_floor_fuse, k_fuse_step, k_bilerp_add, k_blend_crop;
#endif
} dt_iop_rawdenoiseai_global_data_t;

/* Thread-safe lazy loader: returns the model for (version, size, scale),
 * loading it on first request from
 * <configdir>/denoise-<size>-<single|multi>-<version>.anselnn (user
 * override) or <datadir>/ (shipped). NULL if the file is absent or invalid.
 * Runs on the pipeline thread via commit_params, hence the mutex. */
static dt_nn_model_t *_get_model(dt_iop_rawdenoiseai_global_data_t *gd, dt_iop_rawdenoiseai_version_t ver,
                                 dt_iop_rawdenoiseai_size_t sz, dt_iop_rawdenoiseai_scale_t sc)
{
  if((int)ver < 0 || (int)ver >= DT_RAWDENOISEAI_NUM_VERSIONS || (int)sz < 0
     || (int)sz >= DT_RAWDENOISEAI_NUM_SIZES || (int)sc < 0 || (int)sc >= DT_RAWDENOISEAI_NUM_SCALES)
    return NULL;

  dt_pthread_mutex_lock(&gd->lock);
  if(!gd->tried[ver][sz][sc])
  {
    gd->tried[ver][sz][sc] = TRUE;
    char name[64];
    snprintf(name, sizeof(name), "denoise-%s-%s-%s.anselnn", _size_tag[sz], _scale_tag[sc], _version_tag[ver]);

    char dir[DT_PATH_MAX] = { 0 };
    char path[DT_PATH_MAX] = { 0 };
    char err[256] = "";
    dt_loc_get_user_config_dir(dir, sizeof(dir));
    dt_concat_path_file(path, dir, name);
    gd->models[ver][sz][sc] = dt_nn_model_load(path, err, sizeof(err));
    if(!gd->models[ver][sz][sc])
    {
      dt_loc_get_datadir(dir, sizeof(dir));
      dt_concat_path_file(path, dir, name);
      gd->models[ver][sz][sc] = dt_nn_model_load(path, err, sizeof(err));
    }
    if(gd->models[ver][sz][sc])
      dt_print(DT_DEBUG_ALWAYS, "[rawdenoiseai] loaded %s\n", path);
    else
      dt_print(DT_DEBUG_ALWAYS, "[rawdenoiseai] %s unavailable (%s)\n", name, err);
  }
  dt_nn_model_t *m = gd->models[ver][sz][sc];
  dt_pthread_mutex_unlock(&gd->lock);
  return m;
}

/* A user model: any .anselnn dropped in the config dir under a name that is
 * not one of the shipped ones. Cached by basename for the session, with a
 * NULL entry recording a failed load so a broken file is probed once. Same
 * mutex as the shipped matrix — this also runs on the pipeline thread. */
static dt_nn_model_t *_get_custom_model(dt_iop_rawdenoiseai_global_data_t *gd, const char *base)
{
  if(!base || !*base || strchr(base, '/') || strchr(base, '\\')) return NULL;

  dt_pthread_mutex_lock(&gd->lock);
  if(!gd->custom) gd->custom = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  dt_nn_model_t *m = NULL;
  if(!g_hash_table_lookup_extended(gd->custom, base, NULL, (gpointer *)&m))
  {
    char dir[DT_PATH_MAX] = { 0 };
    char path[DT_PATH_MAX] = { 0 };
    char err[256] = "";
    dt_loc_get_user_config_dir(dir, sizeof(dir));
    dt_concat_path_file(path, dir, base);
    m = dt_nn_model_load(path, err, sizeof(err));
    g_hash_table_insert(gd->custom, g_strdup(base), m);
    if(m)
      dt_print(DT_DEBUG_ALWAYS, "[rawdenoiseai] loaded user model %s\n", path);
    else
      dt_print(DT_DEBUG_ALWAYS, "[rawdenoiseai] user model %s unusable (%s)\n", base, err);
  }
  dt_pthread_mutex_unlock(&gd->lock);
  return m;
}

/* Basenames of every .anselnn in the config dir, sorted, shipped names
 * excluded (those are overrides of the matrix, already reachable through the
 * size/variant combos). Caller frees with g_list_free_full(l, g_free). */
static GList *_list_custom_models(void)
{
  char dir[DT_PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(dir, sizeof(dir));
  GDir *d = g_dir_open(dir, 0, NULL);
  if(!d) return NULL;
  GList *out = NULL;
  const gchar *fn;
  while((fn = g_dir_read_name(d)))
  {
    if(!g_str_has_suffix(fn, ".anselnn")) continue;
    gboolean shipped = FALSE;
    for(int v = 0; v < DT_RAWDENOISEAI_NUM_VERSIONS && !shipped; v++)
      for(int z = 0; z < DT_RAWDENOISEAI_NUM_SIZES && !shipped; z++)
        for(int c = 0; c < DT_RAWDENOISEAI_NUM_SCALES && !shipped; c++)
        {
          char name[64];
          snprintf(name, sizeof(name), "denoise-%s-%s-%s.anselnn", _size_tag[z], _scale_tag[c], _version_tag[v]);
          shipped = !g_strcmp0(fn, name);
        }
    if(!shipped) out = g_list_prepend(out, g_strdup(fn));
  }
  g_dir_close(d);
  return g_list_sort(out, (GCompareFunc)g_strcmp0);
}

const char *name()
{
  return _("raw denoise (AI)");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
                                _("denoise the raw picture with a neural network conditioned "
                                  "on the camera noise profile"),
                                _("corrective"), _("linear, raw, scene-referred"), _("linear, raw"),
                                _("linear, raw, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

int default_group()
{
  return IOP_GROUP_REPAIR;
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

/* Route the executor's scratch through the pixelpipe cache arena, per the
 * project rule that pixel buffers never come from bare malloc. nn_model
 * cannot include darktable.h (it is deliberately pipeline-free), so the
 * arena is injected here, once, before any pipeline runs. */
/* Per-tile scratch REGION: the whole working set of one process() call —
 * input planes, output plane and every executor tensor — is reserved from the
 * pixelpipe arena as ONE allocation, up front, sized by the executor's exact
 * ledger plus an internal-fragmentation margin. The executor's alloc/free
 * churn then happens inside the region through the sub-allocator below and is
 * invisible to the arena: no interleaving with cache entries, no fragmenting
 * of the arena's free runs, and the tiling engine's largest-free-run cap
 * guarantees the single reservation fits by construction. Memory is planned,
 * reserved, then executed within — never discovered at runtime; if the
 * reservation itself fails, the tile fails BEFORE any compute.
 *
 * The sub-allocator is a trivial first-fit block list: at most a dozen live
 * tensors exist at once, all sized in whole planes. The region pointer is
 * thread-local because the darkroom's full and preview pipes may run
 * process() concurrently. */
#define NN_REGION_MAX_BLOCKS 32
// the two-ended layout (churn bottom-up, skips top-down) achieves the
// ledger's live peak exactly; the slack only covers per-block 64-byte
// alignment crumbs
#define NN_REGION_SLACK 1.02f

typedef struct nn_region_t
{
  char *base;
  size_t size;
  struct
  {
    size_t off, len;
  } blocks[NN_REGION_MAX_BLOCKS];
  int n_blocks;
} nn_region_t;

static __thread nn_region_t *_nn_region = NULL;

static void *_region_alloc(nn_region_t *r, size_t bytes, int long_lived)
{
  bytes = (bytes + 63) & ~(size_t)63; // keep 64-byte alignment inside the region
  if(r->n_blocks >= NN_REGION_MAX_BLOCKS) return NULL;
  size_t off = (size_t)-1;
  int at = 0;
  if(!long_lived)
  {
    // churn packs bottom-up, first fit; blocks are kept sorted by offset
    size_t gap = 0;
    for(int i = 0; i <= r->n_blocks; i++)
    {
      const size_t end = (i < r->n_blocks) ? r->blocks[i].off : r->size;
      if(end - gap >= bytes)
      {
        off = gap;
        at = i;
        break;
      }
      if(i == r->n_blocks) break;
      gap = r->blocks[i].off + r->blocks[i].len;
    }
  }
  else
  {
    // long-lived blocks (skip connections) pack top-down, last fit, so they
    // never split the churn area mid-region — this is what lets the region be
    // sized at exactly planes + the ledger's live peak, no slack
    size_t gap_end = r->size;
    for(int i = r->n_blocks; i >= 0; i--)
    {
      const size_t gap_start = (i > 0) ? r->blocks[i - 1].off + r->blocks[i - 1].len : 0;
      if(gap_end - gap_start >= bytes)
      {
        off = gap_end - bytes;
        at = i;
        break;
      }
      if(i > 0) gap_end = r->blocks[i - 1].off;
    }
  }
  if(off == (size_t)-1)
  {
    size_t live = 0, largest_gap = 0, gap = 0;
    for(int i = 0; i <= r->n_blocks; i++)
    {
      const size_t end = (i < r->n_blocks) ? r->blocks[i].off : r->size;
      if(end - gap > largest_gap) largest_gap = end - gap;
      if(i == r->n_blocks) break;
      live += r->blocks[i].len;
      gap = r->blocks[i].off + r->blocks[i].len;
    }
    dt_print(DT_DEBUG_ALWAYS,
             "[rawdenoiseai] region alloc failed: %" G_GSIZE_FORMAT " bytes (%s), region %" G_GSIZE_FORMAT
             ", live %" G_GSIZE_FORMAT " in %d blocks, largest gap %" G_GSIZE_FORMAT "\n",
             bytes, long_lived ? "long-lived" : "churn", r->size, live, r->n_blocks, largest_gap);
    return NULL;
  }
  for(int i = r->n_blocks; i > at; i--) r->blocks[i] = r->blocks[i - 1];
  r->blocks[at].off = off;
  r->blocks[at].len = bytes;
  r->n_blocks++;
  return r->base + off;
}

static void _region_free(nn_region_t *r, void *p)
{
  const size_t off = (size_t)((char *)p - r->base);
  for(int i = 0; i < r->n_blocks; i++)
    if(r->blocks[i].off == off)
    {
      for(int j = i; j < r->n_blocks - 1; j++) r->blocks[j] = r->blocks[j + 1];
      r->n_blocks--;
      return;
    }
}

static void *_nn_arena_alloc(size_t bytes, int long_lived)
{
  if(_nn_region) return _region_alloc(_nn_region, bytes, long_lived);
  return dt_pixelpipe_cache_alloc_align_cache(bytes, 0);
}

static void _nn_arena_free(void *p)
{
  nn_region_t *r = _nn_region;
  if(r && (char *)p >= r->base && (char *)p < r->base + r->size)
  {
    _region_free(r, p);
    return;
  }
  dt_pixelpipe_cache_free_align(p);
}

void init_global(dt_iop_module_so_t *module)
{
  dt_nn_set_allocator(_nn_arena_alloc, _nn_arena_free);
  dt_iop_rawdenoiseai_global_data_t *gd = calloc(1, sizeof(dt_iop_rawdenoiseai_global_data_t));
  dt_pthread_mutex_init(&gd->lock, NULL);
#ifdef HAVE_OPENCL
  gd->nn_cl = dt_nn_cl_create(39); // rawdenoiseai.cl, from programs.conf
  gd->k_assemble = dt_opencl_create_kernel(39, "nn_assemble");
  gd->k_bin_planes = dt_opencl_create_kernel(39, "nn_bin_planes");
  gd->k_residual = dt_opencl_create_kernel(39, "nn_residual");
  gd->k_upsample_n = dt_opencl_create_kernel(39, "nn_upsample_n");
  gd->k_bin16_mdv = dt_opencl_create_kernel(39, "nn_bin16_mdv");
  gd->k_avg2x2 = dt_opencl_create_kernel(39, "nn_avg2x2");
  gd->k_floor_fuse = dt_opencl_create_kernel(39, "nn_floor_fuse");
  gd->k_fuse_step = dt_opencl_create_kernel(39, "nn_fuse_step");
  gd->k_bilerp_add = dt_opencl_create_kernel(39, "nn_bilerp_add");
  gd->k_blend_crop = dt_opencl_create_kernel(39, "nn_blend_crop");
#endif
  module->data = gd;
  // models are loaded lazily on first use per (version, variant)
}

void cleanup_global(dt_iop_module_so_t *module)
{
  // the hooks point into this module's code: leaving them set after dlclose
  // would leave the core library with dangling function pointers
  dt_nn_set_allocator(NULL, NULL);
  dt_iop_rawdenoiseai_global_data_t *gd = (dt_iop_rawdenoiseai_global_data_t *)module->data;
  if(gd)
  {
    for(int v = 0; v < DT_RAWDENOISEAI_NUM_VERSIONS; v++)
      for(int sz = 0; sz < DT_RAWDENOISEAI_NUM_SIZES; sz++)
        for(int sc = 0; sc < DT_RAWDENOISEAI_NUM_SCALES; sc++) dt_nn_model_free(gd->models[v][sz][sc]);
#ifdef HAVE_OPENCL
    dt_nn_cl_destroy(gd->nn_cl);
    dt_opencl_free_kernel(gd->k_assemble);
    dt_opencl_free_kernel(gd->k_bin_planes);
    dt_opencl_free_kernel(gd->k_residual);
    dt_opencl_free_kernel(gd->k_upsample_n);
    dt_opencl_free_kernel(gd->k_bin16_mdv);
    dt_opencl_free_kernel(gd->k_avg2x2);
    dt_opencl_free_kernel(gd->k_floor_fuse);
    dt_opencl_free_kernel(gd->k_fuse_step);
    dt_opencl_free_kernel(gd->k_bilerp_add);
    dt_opencl_free_kernel(gd->k_blend_crop);
#endif
    if(gd->custom)
    {
      GHashTableIter it;
      gpointer k, v;
      g_hash_table_iter_init(&it, gd->custom);
      while(g_hash_table_iter_next(&it, &k, &v))
        if(v) dt_nn_model_free((dt_nn_model_t *)v);
      g_hash_table_destroy(gd->custom);
      gd->custom = NULL;
    }
    dt_pthread_mutex_destroy(&gd->lock);
  }
  free(module->data);
  module->data = NULL;
}

/* The size a new history entry gets, from what the machine can actually run:
 * half where OpenCL will carry it, quarter on CPU alone — the only size that
 * stays interactive there. The variant is multiscale either way; the coarse
 * chroma pass is worth most exactly where capacity is scarce (it buys quarter
 * ~2.6 dB of low-frequency chroma error and large almost nothing), so the
 * hardware picks the width, not the variant. */
static dt_iop_rawdenoiseai_size_t _default_size(void)
{
  return dt_opencl_is_enabled() ? DT_RAWDENOISEAI_HALF : DT_RAWDENOISEAI_QUARTER;
}

/* Supported when the input is mosaiced and the model a new entry would DEFAULT
 * to can be loaded (i.e. weights are installed). Probing that exact model and
 * not a fixed one is what keeps the enable button honest on an installation
 * carrying only part of the matrix. A specific (version, size, variant) the
 * user selects that turns out to be missing is handled per-piece by
 * copy-through. */
static gboolean _rawdenoiseai_supported(dt_iop_module_t *module)
{
  dt_iop_rawdenoiseai_global_data_t *gd = (dt_iop_rawdenoiseai_global_data_t *)module->global_data;
  return dt_image_needs_demosaic(&module->dev->image_storage) && gd
         && _get_model(gd, DT_RAWDENOISEAI_V1, _default_size(), DT_RAWDENOISEAI_MULTI);
}

void reload_defaults(dt_iop_module_t *module)
{
  /* Pick the default size from what the machine can actually run, because a
   * user who enables the module without knowing what it is must get a result
   * in seconds — not a frozen application. The variant is multiscale in both
   * cases: it is what keeps the smaller networks free of low-frequency chroma
   * blotches, and that matters most exactly where capacity is scarce (see
   * doc/rawdenoiseai.md — the coarse pass buys quarter ~2.6 dB of chroma
   * error and large almost nothing).
   *
   * OpenCL present: half, ~4x the quarter cost but clearly better.
   * CPU only: quarter, the only size that stays interactive on a CPU.
   *
   * dt_opencl_is_enabled() reflects both the build and the user's preference,
   * and is a static stub returning 0 without HAVE_OPENCL. Users who want a
   * different trade-off still pick any size by hand; this only seeds a NEW
   * history entry, so existing edits keep whatever they were created with. */
  dt_iop_rawdenoiseai_params_t *const d = (dt_iop_rawdenoiseai_params_t *)module->default_params;
  d->size = _default_size();
  d->scale_variant = DT_RAWDENOISEAI_MULTI;

  module->hide_enable_button = !_rawdenoiseai_supported(module);
  module->default_enabled = 0;
}

gboolean force_enable(struct dt_iop_module_t *self, const gboolean current_state)
{
  // history sanitization: an entry pasted onto a non-mosaic image, or loaded
  // without a model available, is forced off at history-read time
  return current_state && _rawdenoiseai_supported(self);
}

/* Fill d->a/d->b from the best noise profile for this image: exact ISO match,
 * interpolation between the bracketing profiled ISOs, clamping outside the
 * profiled range, generic Poissonian when the camera has no profiles. Mirrors
 * the training-time ProfileSampler semantics (clamped interpolation). */
static void _fetch_noise_profile(dt_iop_module_t *self, dt_iop_rawdenoiseai_data_t *d)
{
  GList *profiles = dt_noiseprofile_get_matching(&self->dev->image_storage);
  dt_noiseprofile_t interpolated = dt_noiseprofile_generic;
  const float iso = self->dev->image_storage.exif_iso;

  if(profiles)
  {
    dt_noiseprofile_t *first = (dt_noiseprofile_t *)profiles->data;
    dt_noiseprofile_t *last = NULL;
    interpolated = *first; // clamp below the profiled range
    for(GList *iter = profiles; iter; iter = g_list_next(iter))
    {
      dt_noiseprofile_t *current = (dt_noiseprofile_t *)iter->data;
      if(current->iso == iso)
      {
        interpolated = *current;
        break;
      }
      if(last && last->iso < iso && current->iso > iso)
      {
        interpolated.iso = iso;
        dt_noiseprofile_interpolate(last, current, &interpolated);
        break;
      }
      interpolated = *current; // clamp above the profiled range
      last = current;
    }
  }
  for(int k = 0; k < 3; k++)
  {
    d->a[k] = interpolated.a[k];
    d->b[k] = interpolated.b[k];
  }
  g_list_free_full(profiles, dt_noiseprofile_free);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_rawdenoiseai_params_t *p = (dt_iop_rawdenoiseai_params_t *)params;
  dt_iop_rawdenoiseai_data_t *d = (dt_iop_rawdenoiseai_data_t *)piece->data;

  d->strength = p->strength;
  d->noise_level = p->noise_level;
  d->sigma_scale[0] = p->sigma_red;
  d->sigma_scale[1] = p->sigma_green;
  d->sigma_scale[2] = p->sigma_blue;
  // neural inference is among the most expensive nodes of the pipe: always
  // materialize this piece's output in the RAM cache (same policy as
  // diffuse/atrous), so interactive edits downstream never re-run it
  piece->cache_output_on_ram = TRUE;
  _fetch_noise_profile(self, d);

  dt_iop_rawdenoiseai_global_data_t *gd = (dt_iop_rawdenoiseai_global_data_t *)self->global_data;
  /* A named user model wins over the shipped matrix. If it has gone missing
   * since the edit was made, the piece is disabled rather than silently
   * rendered through a DIFFERENT network: the two are not interchangeable,
   * and quietly substituting one would change the picture without telling
   * anyone. The GUI keeps showing the name so the situation is legible. */
  const gboolean want_custom = gd && p->custom_model[0];
  d->model = want_custom      ? _get_custom_model(gd, p->custom_model)
             : gd             ? _get_model(gd, p->version, p->size, p->scale_variant)
                              : NULL;

  dt_print(DT_DEBUG_PARAMS,
           "[rawdenoiseai] commit: camera '%s' iso %.0f model %s-%s-%s%s -> "
           "a=(%.3g %.3g %.3g) b=(%.3g %.3g %.3g) strength %.2f noise level %.2f "
           "channel correction (%.2f %.2f %.2f)\n",
           self->dev->image_storage.camera_makermodel, self->dev->image_storage.exif_iso,
           _size_tag[CLAMP(p->size, 0, DT_RAWDENOISEAI_NUM_SIZES - 1)],
           _scale_tag[CLAMP(p->scale_variant, 0, DT_RAWDENOISEAI_NUM_SCALES - 1)],
           _version_tag[CLAMP(p->version, 0, DT_RAWDENOISEAI_NUM_VERSIONS - 1)],
           d->model ? (want_custom ? " (user model)" : "") : " (missing)",
           d->a[0], d->a[1], d->a[2], d->b[0], d->b[1], d->b[2], p->strength, p->noise_level, p->sigma_red,
           p->sigma_green, p->sigma_blue);

  // plane-layout contract with the training repo: a model that does not
  // match what _assemble_planes builds must be treated as missing, not fed
  if(d->model)
  {
    const int fine_in = dt_nn_model_in_channels(d->model);
    const int c_out = dt_nn_model_coarse_out_channels(d->model);
    const gboolean ok = c_out > 0 ? (dt_nn_model_coarse_in_channels(d->model) == 6 && c_out == 3 && fine_in == 8)
                                  : (fine_in == 5);
    if(!ok)
    {
      dt_print(DT_DEBUG_ALWAYS,
               "[rawdenoiseai] model plane layout unsupported "
               "(fine_in %d coarse_out %d) — module disabled\n",
               fine_in, c_out);
      d->model = NULL;
    }
  }
  if(!d->model || !(p->strength > 0.0f)) piece->enabled = 0;
}

static unsigned _align_lcm(const unsigned a, const unsigned b)
{
  if(a == 0 || b == 0) return a > b ? a : b;
  unsigned x = a, y = b;
  while(y)
  {
    const unsigned t = x % y;
    x = y;
    y = t;
  }
  return a / x * b;
}

void tiling_callback(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                     const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  const dt_iop_rawdenoiseai_data_t *const d = (dt_iop_rawdenoiseai_data_t *)piece->data;
  // per input pixel (4 bytes): 5 input planes + 1 output plane + network
  // scratch, all float32. factor is relative to the unit buffer size.
  float extra = 6.0f;
  float extra_cl = 6.0f;
  unsigned overlap = 0;
  if(d && d->model)
  {
    // input planes + output plane (in_ch is 8 for a multi-scale model), the
    // coarse buffers at 1/bin^2, and the executor scratch (which already
    // includes the coarse net's share for unet-ms)
    const gboolean xtrans_cfa = piece->dsc_in.filters == 9u;
    const int bin = dt_nn_model_bin(d->model, xtrans_cfa);
    const float planes = (float)(dt_nn_model_in_channels(d->model) + 1)
                         + (bin > 1 ? 12.0f / (float)(bin * bin) : 0.0f);
    /* Host: process() reserves the tile's whole working set as ONE arena
     * region — the (in_ch + 1) planes plus the executor's ledger peak times
     * the sub-allocator's fragmentation margin. The factor declares exactly
     * that reservation, so the tiling plan, the reservation and the execution
     * are the same number (the coarse-stage module buffers, 12/bin^2, are the
     * only separate short-lived arena entries). Expressed as a factor of the
     * input image size — floats per input pixel — never absolute bytes. */
    extra = planes + NN_REGION_SLACK * dt_nn_unet_scratch_per_px(d->model);
    /* Device: no region — buffers are device-side. The CL executor's sequence
     * differs (it materializes the decoder concat), and the CL path adds
     * three device planes of its own (dev_den, plus the buffer-format copies
     * dev_in_buf/dev_out_buf) and the fusion grids (~0.13 plane); count all
     * of it explicitly rather than letting it ride inside factor_cl's
     * padding headroom. */
    extra_cl = planes + dt_nn_unet_scratch_per_px_cl(d->model) + 3.2f;

    /* Overlap is set from the MEASURED seam profile, not from the impulse
     * response. An earlier revision sized it at 48 px because the trained
     * model's response to a delta decays to 1.4e-6 of peak by radius 32; that
     * reasoning is wrong for this purpose. A tile boundary does not remove one
     * tap, it removes an entire half-plane of context, and the summed
     * contribution of that half-plane is orders of magnitude above any single
     * tap. Measured on DSCF9668.RAF (X-Trans, 3 tiles across), CPU untiled vs
     * GPU tiled, as a fraction of full scale:
     *
     *   overlap  48 -> seam peaks at 0.17-0.23 %, i.e. 238-317x the interior
     *                  difference, and does not return to it until ~50 px in
     *   overlap 192 -> seam peaks at 0.006-0.016 %, 5.5-14.5x interior
     *
     * The 48 px case is visible; it is the tile grid users report. The decay
     * measured with 48 px of overlap reaches the interior baseline exactly
     * 48 px inside the owned region, so a boundary pixel needs 48 + 48 = 96 px
     * of context: that is the fine net's effective receptive field, and the
     * floor for the single-scale model.
     *
     * The multiscale model adds the coarse stage, whose receptive field is its
     * own (in binned pixels) times `bin`, so it needs substantially more. 192
     * is the value actually validated above. Note X-Trans multiscale reached it
     * only by accident: the engine rounds the requested overlap UP to a
     * multiple of lcm(xalign, yalign), which is 192 there, so a request of 96
     * silently became 192. Bayer multiscale aligns to 64 and would have stayed
     * at 128. Ask for what the measurement supports instead of relying on an
     * alignment that varies with the sensor. */
    overlap = (bin > 1) ? 192 : 96;
  }
  tiling->factor = 2.0f + extra;
  /* The GPU budget must be more conservative than the CPU one, for two
   * reasons the tiling engine cannot see. First, every buffer here is
   * allocated at the alignment-padded tile size, up to (align-1) larger per
   * axis than the tile the engine budgeted — only for the ragged last tile of
   * a row or column now that xalign below equals the alignment, so an interior
   * tile is padded by nothing at all and this term is pure headroom. Second,
   * the engine hands out up to 100 % of the device's reported memory, and a
   * budget that spends all of VRAM never allocates in practice (display,
   * driver and allocator overhead) — CPU RAM overcommits, VRAM does not,
   * and the U-Net scratch dominates this factor so the whole budget is
   * exact where other modules' estimates carry implicit slack. 1.4 covers
   * the worst realistic padding inflation (~1.15) times an allocation
   * headroom (~1.2); the cost of overshooting is a smaller tile, the cost
   * of undershooting is CL_MEM_OBJECT_ALLOCATION_FAILURE and a silent
   * fallback of the whole tile to CPU. */
  tiling->factor_cl = 2.0f + extra_cl * 1.4f;
  tiling->maxbuf = 1.0f;
  // device-resident weight blob (up to ~38 MB) plus fixed slack
  tiling->overhead = 64u * 1024u * 1024u;
  tiling->overlap = overlap;
  /* Tiles must preserve the CFA phase — and, for a multi-scale model, every
   * other LATTICE this module lays over its input: the superpixel binning of
   * the coarse stage (period `bin`) and the low-band fusion pyramid (period
   * DT_NN_FUSION_COARSEST). All of them are anchored to the tile's own origin,
   * because that is the only origin process() is handed. The tiling engine
   * places tile origins on multiples of lcm(xalign, yalign), so anything short
   * of the full lattice period lets two different tile grids bin the same
   * sensels into differently-phased superpixels — which changes the coarse
   * guide, and with it the result, EVERYWHERE inside the tile rather than only
   * near its seams. No amount of overlap fixes that; only aligning the origins
   * does. It is why the same edit rendered differently on CPU and GPU: the two
   * budget their memory differently, so they tile differently, so they binned
   * on different lattices.
   *
   * dt_nn_model_alignment() is the one function that owns those periods (the
   * fine net's stride pyramid, the coarse net's binned one, and the fusion
   * bands) — ask it rather than re-deriving any of them here, or the next
   * lattice added to the model silently reopens this bug. Aligning to the FULL
   * value also means an interior tile needs no reflect padding at all, so no
   * mirrored data is folded into the fusion's per-tile statistics. */
  const gboolean xtrans = piece->dsc_in.filters == 9u;
  unsigned align = xtrans ? 6u : 2u;
  if(d && d->model) align = _align_lcm(align, (unsigned)dt_nn_model_alignment(d->model));
  tiling->xalign = align;
  tiling->yalign = align;
}

// mirror-reflect coordinate into [0, n): same row/column parity is preserved
// for even n-1 steps; the CFA color planes are computed from the SOURCE
// sensel, so the network always sees consistent (value, color) pairs even
// where reflection breaks the periodic layout (X-Trans borders).
static inline __attribute__((always_inline)) int _reflect(int v, int n)
{
  if(n == 1) return 0;
  while(v < 0 || v >= n)
  {
    if(v < 0) v = -v;
    if(v >= n) v = 2 * n - 2 - v;
  }
  return v;
}

/* Total sigma scale per CFA color: global correction x the per-channel
 * slider, nothing else — the GUI values ARE the conditioning (see the
 * calibration comment at the top of this file). */
static void _sigma_scale(const dt_iop_rawdenoiseai_data_t *d, float scale[3])
{
  for(int c = 0; c < 3; c++) scale[c] = d->noise_level * d->sigma_scale[c];
}

/* Build the coarse stage's 6 input planes [R, G, B, sigmaR, sigmaG, sigmaB]
 * from the assembled fine planes 0-3 (count-weighted superpixel means; the
 * binning itself is dt_nn_bin_planes, the bit-exact contract with the
 * training repo). Coarse sigma is the analytic sigma of the mean of n
 * sensels: scale[c] * sqrt((a*x + b) / n). Shared verbatim by the CPU and
 * OpenCL paths. */
__DT_CLONE_TARGETS__
static void _k_bin_planes(const float *const nn_in, float *const coarse_in, float *const cnt,
                                    const int pw, const int ph, const int bin,
                                    const dt_iop_rawdenoiseai_data_t *const d)
{
  const int cw = pw / bin, chh = ph / bin;
  const size_t cplane = (size_t)cw * chh;
  dt_nn_bin_planes(nn_in, pw, ph, bin, coarse_in, cnt);
  float scale[3];
  _sigma_scale(d, scale);
  __OMP_PARALLEL_FOR__()
  for(int c = 0; c < 3; c++)
  {
    const float *const mean = coarse_in + (size_t)c * cplane;
    const float *const n_c = cnt + (size_t)c * cplane;
    float *const sigma = coarse_in + (size_t)(3 + c) * cplane;
    for(size_t i = 0; i < cplane; i++)
    {
      const float n = n_c[i] > 1.0f ? n_c[i] : 1.0f;
      const float var = (d->a[c] * MAX(mean[i], 0.0f) + d->b[c]) / n;
      sigma[i] = scale[c] * sqrtf(MAX(var, 1e-12f));
    }
  }
}

/* Build the network's 5 input planes [mosaic, R, G, B one-hot, sigma] from the
 * mosaic, reflect-padded from (width, height) to (pw, ph). Shared verbatim by
 * the CPU and OpenCL paths so both feed the network identical data. */
__DT_CLONE_TARGETS__
static void _k_assemble(const float *const in, float *const nn_in, const int width, const int height,
                             const int pw, const int ph, const dt_iop_rawdenoiseai_data_t *const d,
                             const uint32_t filters, const uint8_t (*const xtrans)[6],
                             const dt_iop_roi_t *const roi)
{
  const size_t plane = (size_t)pw * ph;
  float scale[3];
  _sigma_scale(d, scale);
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < ph; y++)
  {
    const int sy = _reflect(y, height);
    const float *const irow = in + (size_t)sy * width;
    float *const mosaic = nn_in + (size_t)y * pw;
    float *const onehot_r = nn_in + plane + (size_t)y * pw;
    float *const onehot_g = nn_in + 2 * plane + (size_t)y * pw;
    float *const onehot_b = nn_in + 3 * plane + (size_t)y * pw;
    float *const sigma = nn_in + 4 * plane + (size_t)y * pw;
    for(int x = 0; x < pw; x++)
    {
      const int sx = _reflect(x, width);
      const float v = irow[sx];
      int c = (filters == 9u) ? FCxtrans(sy, sx, roi, xtrans) : (int)FC(sy, sx, filters);
      if(c < 0 || c > 2) c = 1; // both greens share the G statistics; clamp junk CFA data
      mosaic[x] = v;
      onehot_r[x] = (c == 0) ? 1.0f : 0.0f;
      onehot_g[x] = (c == 1) ? 1.0f : 0.0f;
      onehot_b[x] = (c == 2) ? 1.0f : 0.0f;
      const float var = d->a[c] * MAX(v, 0.0f) + d->b[c];
      sigma[x] = scale[c] * sqrtf(MAX(var, 1e-12f));
    }
  }
}


/* ---- CPU kernels ------------------------------------------------------
 * One function per OpenCL kernel in rawdenoiseai.cl, same name, same
 * arguments in the same order, so process() and process_cl() read as the same
 * procedure and a change to one has an obvious counterpart in the other.
 * Every pixel loop in this module lives in one of these. */

/* Per-channel Bayer densities: the fraction of a block's sensels carrying each
 * colour. Shared with the OpenCL twins, which take them as dens0/dens1/dens2.
 * Bayer values are used for both CFA families, matching the torch reference. */
static const float DT_NN_FUSION_DENS[3] = { 0.25f, 0.5f, 0.25f };

/* chi^2-quantile guard: the local mean of a squared noise term over a 3x3 cell
 * neighbourhood has ~9 effective samples, so pure-noise cells fluctuate up to
 * ~2x their expectation. Must equal the literal in nn_floor_fuse/nn_fuse_step. */
#define DT_NN_FUSION_T_CHI2 2.5

/* mirrors nn_residual: the network predicts what to REMOVE, so the denoised
 * signal is input minus head. Applied by the caller on BOTH devices — the
 * executor writes the raw head output either way. */
__DT_CLONE_TARGETS__
static void _k_residual(const float *const in, const float *const head, float *const out, const size_t n)
{
  __OMP_PARALLEL_FOR__()
  for(size_t i = 0; i < n; i++) out[i] = in[i] - head[i];
}

/* mirrors nn_blend_crop: strength is the opacity of the correction, so the
 * output lerps between the original mosaic and the denoised one while dropping
 * the alignment padding. */
__DT_CLONE_TARGETS__
static void _k_blend_crop(const float *const in, const float *const den, float *const out, const int width,
                          const int height, const int pw, const float strength)
{
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
  {
    const float *const src_in = in + (size_t)y * width;
    const float *const src_nn = den + (size_t)y * pw;
    float *const dst = out + (size_t)y * width;
    for(int x = 0; x < width; x++) dst[x] = src_in[x] + strength * (src_nn[x] - src_in[x]);
  }
}

/* mirrors nn_bin16_mdv: count-weighted per-channel mean of the mosaic, the
 * denoised plane and sigma^2 over each 16x16 block. */
__DT_CLONE_TARGETS__
static void _k_bin16_mdv(const float *const nn_in, const float *const denoised, float *const M,
                         float *const D, float *const V, const int pw, const int ph)
{
  const size_t plane = (size_t)pw * ph;
  const float *const sig = nn_in + 4 * plane;
  const int cw = pw / DT_NN_FUSION_FINEST, chh = ph / DT_NN_FUSION_FINEST;
  const size_t p0 = (size_t)cw * chh;
  for(int c = 0; c < 3; c++)
  {
    const float *const oh = nn_in + (size_t)(1 + c) * plane;
    __OMP_PARALLEL_FOR__()
    for(int cy = 0; cy < chh; cy++)
      for(int cx = 0; cx < cw; cx++)
      {
        float sm = 0.f, sd = 0.f, sv = 0.f, cnt = 0.f;
        for(int y = cy * DT_NN_FUSION_FINEST; y < (cy + 1) * DT_NN_FUSION_FINEST; y++)
          for(int x = cx * DT_NN_FUSION_FINEST; x < (cx + 1) * DT_NN_FUSION_FINEST; x++)
          {
            const size_t i = (size_t)y * pw + x;
            sm += nn_in[i] * oh[i];
            sd += denoised[i] * oh[i];
            sv += sig[i] * sig[i] * oh[i];
            cnt += oh[i];
          }
        const float n = cnt > 1.f ? cnt : 1.f;
        const size_t o = (size_t)c * p0 + (size_t)cy * cw + cx;
        M[o] = sm / n;
        D[o] = sd / n;
        V[o] = sv / n;
      }
  }
}

/* mirrors nn_avg2x2: 2x2 average pooling of a 3-plane grid. Per-channel counts
 * are uniform at these scales, so a plain mean of four children equals
 * re-binning from full resolution. */
__DT_CLONE_TARGETS__
static void _k_avg2x2(const float *const in, float *const out, const int sw, const int sh, const size_t p0)
{
  const int w2 = sw / 2, h2 = sh / 2;
  for(int c = 0; c < 3; c++)
  {
    const float *const src = in + (size_t)c * p0;
    float *const dst = out + (size_t)c * p0;
    for(int y = 0; y < h2; y++)
      for(int x = 0; x < w2; x++)
        dst[(size_t)y * w2 + x]
            = 0.25f
              * (src[(size_t)(2 * y) * sw + 2 * x] + src[(size_t)(2 * y) * sw + 2 * x + 1]
                 + src[(size_t)(2 * y + 1) * sw + 2 * x] + src[(size_t)(2 * y + 1) * sw + 2 * x + 1]);
  }
}

/* Hybrid low-band fusion (mirrors cfa.fuse_low_bands in the training repo):
 * per-band self-calibrated Wiener weights at scales 16/32, pure measurement
 * at the coarsest band. All band corrections are upsampled BILINEARLY
 * (align_corners=false, matching torch F.interpolate): nearest upsampling
 * turned the per-block measurement noise (sigma/sqrt(n), non-negligible on
 * very noisy images) into visible checkers of colored squares. The finest
 * fusion band is 16 px for the same reason. The coarsest band is the
 * n-averaged measurement outright, so the hallucination-free guarantee
 * holds. The pyramid is always 16/32/64 — the same bands the training
 * reference fuses — because dt_nn_model_alignment() guarantees a padded tile
 * that divides by the coarsest one. Bayer channel densities are used for both
 * CFA families, matching the torch reference. */
static inline __attribute__((always_inline)) float _bilerp_tap(const float *const p, const int w,
                                                              const int h, const float fx,
                                                              const float fy)
{
  const float cx = fx < 0.f ? 0.f : (fx > w - 1.f ? w - 1.f : fx);
  const float cy = fy < 0.f ? 0.f : (fy > h - 1.f ? h - 1.f : fy);
  const int x0 = (int)cx, y0 = (int)cy;
  const int x1 = x0 + 1 < w ? x0 + 1 : x0;
  const int y1 = y0 + 1 < h ? y0 + 1 : y0;
  const float ax = cx - x0, ay = cy - y0;
  const float top = p[(size_t)y0 * w + x0] * (1.f - ax) + p[(size_t)y0 * w + x1] * ax;
  const float bot = p[(size_t)y1 * w + x0] * (1.f - ax) + p[(size_t)y1 * w + x1] * ax;
  return top * (1.f - ay) + bot * ay;
}

// dst (fw x fh) = bilinear upsample of src (sw x sh) by integer factor f
__DT_CLONE_TARGETS__
static void _upsample_bilinear(const float *const src, const int sw, const int sh, const int f, float *const dst)
{
  const int fw = sw * f, fh = sh * f;
  for(int y = 0; y < fh; y++)
  {
    const float sy = (y + 0.5f) / f - 0.5f;
    for(int x = 0; x < fw; x++) dst[(size_t)y * fw + x] = _bilerp_tap(src, sw, sh, (x + 0.5f) / f - 0.5f, sy);
  }
}

/* Number of pyramid levels between the finest and the coarsest fusion band.
 * Constant by construction — both are fixed by the training reference. */
static inline int _fusion_levels(void)
{
  int n = 1;
  for(int s = DT_NN_FUSION_FINEST; s < DT_NN_FUSION_COARSEST; s *= 2) n++;
  return n;
}

/* Returns 0 on success, non-zero if the fusion could not run — the caller must
 * treat that as a failed tile, exactly as process_cl() does when a device
 * buffer for the same pyramid cannot be allocated. Rendering the tile with the
 * fine network's raw low band instead would be a silent, tile-shaped quality
 * regression, and would differ from whatever the other device did. */
__DT_CLONE_TARGETS__
/* mirrors nn_floor_fuse: structure-gated blend — the measurement owns every
 * cell whose own mean-removed local energy is noise-sized (the dilution
 * guarantee), the model owns structured cells, because a box average across an
 * edge mixes both sides into a saturated outline. The gate reads the
 * MEASUREMENT, not the model discrepancy: D-M cannot tell a real edge from the
 * model drifting on flat content. REQUIRES a model trained with the
 * DC-ownership loss; one from the older fused loss drifts in deep shadows. */
__DT_CLONE_TARGETS__
static void _k_floor_fuse(const float *const M, const float *const D, const float *const V,
                          float *const fused, const int sw, const int sh, const size_t p0, const int S)
{
  for(int c = 0; c < 3; c++)
  {
    const double vscale = 1.0 / (DT_NN_FUSION_DENS[c] * S * S);
    const float *const Mp = M + (size_t)c * p0;
    const float *const Dp = D + (size_t)c * p0;
    const float *const Vp = V + (size_t)c * p0;
    float *const fs = fused + (size_t)c * p0;
    for(int y = 0; y < sh; y++)
      for(int x = 0; x < sw; x++)
      {
        // structure = blur3((M - blur3(M))^2): insensitive to smooth gradients,
        // unlike a plain window variance
        double structure = 0.0;
        for(int ny = -1; ny <= 1; ny++)
          for(int nx = -1; nx <= 1; nx++)
          {
            const int cy2 = CLAMP(y + ny, 0, sh - 1), cx2 = CLAMP(x + nx, 0, sw - 1);
            double mean = 0.0;
            for(int dy = -1; dy <= 1; dy++)
              for(int dx = -1; dx <= 1; dx++)
              {
                const int yy = CLAMP(cy2 + dy, 0, sh - 1), xx = CLAMP(cx2 + dx, 0, sw - 1);
                mean += Mp[(size_t)yy * sw + xx];
              }
            const double mloc = Mp[(size_t)cy2 * sw + cx2] - mean / 9.0;
            structure += mloc * mloc;
          }
        const size_t i = (size_t)y * sw + x;
        // this cell's own mean sigma^2, not the tile's (cfa.fuse_low_bands)
        const double vn = (double)Vp[i] * vscale;
        structure = structure / 9.0 - DT_NN_FUSION_T_CHI2 * vn;
        if(structure < 0.0) structure = 0.0;
        const float w = (float)(structure / (structure + vn + 1e-20));
        fs[i] = w * Dp[i] + (1.f - w) * Mp[i];
      }
  }
}

/* mirrors nn_fuse_step: upsample the running fusion and add the finer band,
 * weighted per cell by a Wiener gain on the band discrepancy. `ups` is scratch
 * for two upsampled planes. */
__DT_CLONE_TARGETS__
static void _k_fuse_step(const float *const fused_c, const float *const Mf, const float *const Df,
                         const float *const Vf, const float *const Mc, const float *const Dc,
                         float *const fused_f, float *const ups, const int sw, const int sh,
                         const size_t p0, const int sc)
{
  const int fw = sw * 2, fh = sh * 2;
  for(int c = 0; c < 3; c++)
  {
    const float *const Dfp = Df + (size_t)c * p0;
    const float *const Mfp = Mf + (size_t)c * p0;
    const float *const Vfp = Vf + (size_t)c * p0;
    float *const upD = ups, *const upM = ups + p0;
    _upsample_bilinear(Dc + (size_t)c * p0, sw, sh, 2, upD);
    _upsample_bilinear(Mc + (size_t)c * p0, sw, sh, 2, upM);
    /* Var(mean_s - up(mean_2s)) = Var(mean_s) * 3/4 once the covariance with the
     * 2x2 parent is folded in, which is what this reciprocal difference is; so
     * the scale-s cell mean is the right sigma^2 for the whole term. */
    const double vscale
        = 1.0 / (DT_NN_FUSION_DENS[c] * sc * sc) - 1.0 / (DT_NN_FUSION_DENS[c] * 4.0 * sc * sc);
    _upsample_bilinear(fused_c + (size_t)c * p0, sw, sh, 2, fused_f + (size_t)c * p0);
    float *const fs = fused_f + (size_t)c * p0;
    for(int y = 0; y < fh; y++)
      for(int x = 0; x < fw; x++)
      {
        // per-cell Wiener weight from the 3x3-smoothed band discrepancy
        double acc = 0.0;
        int n = 0;
        for(int dy = -1; dy <= 1; dy++)
          for(int dx = -1; dx <= 1; dx++)
          {
            const int yy = CLAMP(y + dy, 0, fh - 1), xx = CLAMP(x + dx, 0, fw - 1);
            const size_t j = (size_t)yy * fw + xx;
            const double d = (double)(Dfp[j] - upD[j]) - (double)(Mfp[j] - upM[j]);
            acc += d * d;
            n++;
          }
        const size_t i = (size_t)y * fw + x;
        const double vn = (double)Vfp[i] * vscale;
        double vm = acc / n - DT_NN_FUSION_T_CHI2 * vn;
        if(vm < 0.0) vm = 0.0;
        const float w = (float)(vn / (vn + vm + 1e-20));
        fs[i] += w * (Dfp[i] - upD[i]) + (1.f - w) * (Mfp[i] - upM[i]);
      }
  }
}

/* mirrors nn_bilerp_add: scatter (fused - D16) bilinearly upsampled from the
 * level-0 grid onto the denoised plane, on whichever colour plane owns each
 * sensel. `scratch` is reused as the correction plane. */
__DT_CLONE_TARGETS__
static void _k_bilerp_add(const float *const fused, const float *const D16, float *const scratch,
                          const float *const nn_in, float *const denoised, const int pw, const int ph,
                          const size_t p0, const int cw0, const int ch0)
{
  const size_t plane = (size_t)pw * ph;
  const float inv = 1.f / (float)DT_NN_FUSION_FINEST;
  for(int c = 0; c < 3; c++)
  {
    float *const cs = scratch + (size_t)c * p0;
    const float *const fa = fused + (size_t)c * p0;
    const float *const d0 = D16 + (size_t)c * p0;
    for(size_t i = 0; i < p0; i++) cs[i] = fa[i] - d0[i];
  }
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < ph; y++)
  {
    const float sy = (y + 0.5f) * inv - 0.5f;
    for(int x = 0; x < pw; x++)
    {
      const size_t i = (size_t)y * pw + x;
      for(int c = 0; c < 3; c++)
        if(nn_in[(size_t)(1 + c) * plane + i] > 0.0f)
        {
          denoised[i] += _bilerp_tap(scratch + (size_t)c * p0, cw0, ch0, (x + 0.5f) * inv - 0.5f, sy);
          break;
        }
    }
  }
}

static int _apply_low_band_anchor(const float *const nn_in, float *const denoised, const int pw, const int ph,
                                  const int scale)
{
  if(scale <= 0) return 0;
  // The pyramid is 16/32/64, fixed by the training reference. The padded tile
  // divides by 64 because dt_nn_model_alignment() folds DT_NN_FUSION_COARSEST
  // in for any model that declares an anchor; deriving the number of levels
  // from the tile size instead is what used to make the render depend on how
  // the pipe happened to tile (i.e. on the machine, and on CPU vs GPU).
  const int S = DT_NN_FUSION_COARSEST;
  if(pw % S || ph % S) return 0;
  const int cw0 = pw / 16, ch0 = ph / 16; // level-0 grid (scale 16)
  const size_t p0 = (size_t)cw0 * ch0;
  // slots: M/D/V per level (16, 32, 64) + fused ping-pong + upsample scratch
  float *const buf = dt_pixelpipe_cache_alloc_align_float_cache(p0 * 3 * (3 * 3 + 3), 0);
  if(IS_NULL_PTR(buf)) return 1;
  float *lv[9]; // lv[3 * level + {0: mosaic, 1: denoised, 2: sigma^2}]
  for(int k = 0; k < 9; k++) lv[k] = buf + (size_t)k * p0 * 3;
  float *fusedA = buf + p0 * 3 * 9, *fusedB = fusedA + p0 * 3, *ups = fusedB + p0 * 3;

  _k_bin16_mdv(nn_in, denoised, lv[0], lv[1], lv[2], pw, ph);
  const int nlev = _fusion_levels();
  int w_l = cw0, h_l = ch0;
  for(int k = 1; k < nlev; k++)
  {
    for(int md = 0; md < 3; md++) _k_avg2x2(lv[3 * (k - 1) + md], lv[3 * k + md], w_l, h_l, p0);
    w_l /= 2;
    h_l /= 2;
  }

  // Coarse-to-fine fusion with LOCAL weights (mirrors cfa.fuse_low_bands).
  // Two distinct gates (see the training repo for the full rationale):
  // - floor band: anchor to the measurement wherever the MEASUREMENT itself
  //   is smooth at this scale (local variance vs noise); the model-vs-
  //   measurement discrepancy cannot distinguish a real edge from the model
  //   drifting on flat content;
  // - soft bands: per-cell Wiener on the band discrepancy with a chi^2
  //   guard (T = 2.5, ~9 effective samples per 3x3 cell neighbourhood).
  int sw = w_l, sh = h_l;
  // FLOOR: structure-gated blend (mirrors cfa.fuse_low_bands) — the
  // measurement owns every cell where its own mean-removed local energy is
  // noise-sized (the dilution guarantee), the model owns structured cells
  // (a box average across an edge mixes both sides -> saturated outline).
  // REQUIRES a model trained with the DC-ownership loss: models from the
  // older fused loss drift in deep shadows and must not be used with this
  // code.
  _k_floor_fuse(lv[3 * (nlev - 1)], lv[3 * (nlev - 1) + 1], lv[3 * (nlev - 1) + 2], fusedA, sw, sh, p0,
                DT_NN_FUSION_FINEST << (nlev - 1));
  for(int k = nlev - 2; k >= 0; k--)
  {
    _k_fuse_step(fusedA, lv[3 * k], lv[3 * k + 1], lv[3 * k + 2], lv[3 * (k + 1)], lv[3 * (k + 1) + 1], fusedB,
                 ups, sw, sh, p0, DT_NN_FUSION_FINEST << k);
    float *tmp = fusedA;
    fusedA = fusedB;
    fusedB = tmp;
    sw *= 2;
    sh *= 2;
  }

  _k_bilerp_add(fusedA, lv[1], fusedB, nn_in, denoised, pw, ph, p0, cw0, ch0);
  dt_pixelpipe_cache_free_align(buf);
  return 0;
}

__DT_CLONE_TARGETS__
int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  const dt_iop_roi_t *const roi = &piece->roi_in;
  const dt_iop_rawdenoiseai_data_t *const d = (dt_iop_rawdenoiseai_data_t *)piece->data;
  dt_nn_model_t *const model = d->model;

  const int width = roi->width, height = roi->height;
  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;

  if(!model || !(d->strength > 0.0f))
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, width, height, 1);
    return 0;
  }

  /* The Bayer CFA lookup below is tile-local (FC() has no ROI awareness), so it
   * needs the word already rotated to this ROI's phase — see the CFA-phase rule
   * in CLAUDE.md. The X-Trans branch stays self-correcting: it takes the raw
   * table plus `roi` and adds the offset itself, and dt_dev_get_roi_filters()
   * no-ops on X-Trans, so `filters == 9u` still identifies it. */
  const uint32_t filters = dt_dev_get_roi_filters(piece, roi);
  const uint8_t(*const xtrans)[6] = (const uint8_t(*const)[6])piece->dsc_in.xtrans;

  // reflect-pad to the network alignment
  const int align = dt_nn_model_alignment(model);
  const int pw = (width + align - 1) / align * align;
  const int ph = (height + align - 1) / align * align;
  const size_t plane = (size_t)pw * ph;

  /* The tile's whole working set — input planes, output plane, executor
   * scratch — is ONE arena reservation, made before any compute, sized by the
   * executor's exact ledger plus the sub-allocator's fragmentation margin.
   * This is the same quantity tiling_callback declares, so the plan, the
   * reservation and the execution are one number. */
  const int in_ch = dt_nn_model_in_channels(model);
  nn_region_t region = { 0 };
  region.size = ((plane * (in_ch + 1) * sizeof(float) + 63) & ~(size_t)63)
                + (size_t)(NN_REGION_SLACK * (float)dt_nn_unet_scratch_bytes(model, pw, ph));
  region.size = (region.size + 63) & ~(size_t)63;
  region.base = dt_pixelpipe_cache_alloc_align_cache(region.size, 0);
  if(IS_NULL_PTR(region.base))
  {
    dt_print(DT_DEBUG_ALWAYS, "[rawdenoiseai] region alloc failed for %dx%d tile (%.1f MB)\n", pw, ph,
             region.size / 1048576.0);
    dt_iop_image_copy_by_size(ovoid, ivoid, width, height, 1);
    return 1;
  }
  _nn_region = &region;
  float *const nn_in = _region_alloc(&region, plane * in_ch * sizeof(float), 0);
  float *const nn_out = _region_alloc(&region, plane * sizeof(float), 0);
  // by construction these cannot fail: the region was sized for them

  _k_assemble(in, nn_in, width, height, pw, ph, d, filters, xtrans, roi);

  int rc = 0;
  const int bin = dt_nn_model_bin(model, filters == 9u);
  if(bin > 1)
  {
    // coarse (low-frequency chroma) pass: denoise the superpixel-binned RGB
    // and inject the nearest-upsampled result as guide planes 5-7 of the
    // fine network's input (mirrors ms_forward() in ansel-denoise train.py)
    const int cw = pw / bin, chh = ph / bin;
    const size_t cplane = (size_t)cw * chh;
    float *const coarse_in = dt_pixelpipe_cache_alloc_align_float_cache(cplane * 6, 0);
    float *const cnt = dt_pixelpipe_cache_alloc_align_float_cache(cplane * 3, 0);
    float *const coarse_out = dt_pixelpipe_cache_alloc_align_float_cache(cplane * 3, 0);
    if(IS_NULL_PTR(coarse_in) || IS_NULL_PTR(cnt) || IS_NULL_PTR(coarse_out))
      rc = 1;
    else
    {
      _k_bin_planes(nn_in, coarse_in, cnt, pw, ph, bin, d);
      rc = dt_nn_unet_apply_stage(model, 1, coarse_in, coarse_out, cw, chh, 0);
      // the coarse head predicts the correction to the binned RGB planes
      if(!rc) _k_residual(coarse_in, coarse_out, coarse_out, cplane * 3);
      if(!rc) dt_nn_upsample_nearest(coarse_out, 3, cw, chh, bin, nn_in + plane * 5);
    }
    if(!IS_NULL_PTR(coarse_in)) dt_pixelpipe_cache_free_align(coarse_in);
    if(!IS_NULL_PTR(cnt)) dt_pixelpipe_cache_free_align(cnt);
    if(!IS_NULL_PTR(coarse_out)) dt_pixelpipe_cache_free_align(coarse_out);
  }

  // 3. fine pass writes the RAW noise prediction; the residual is a kernel of
  //    ours, sequenced exactly as process_cl() sequences nn_residual
  if(!rc) rc = dt_nn_unet_apply_stage(model, 0, nn_in, nn_out, pw, ph, 0);
  if(!rc) _k_residual(nn_in, nn_out, nn_out, plane);
  if(!rc) rc = _apply_low_band_anchor(nn_in, nn_out, pw, ph, dt_nn_model_anchor(model));
  if(rc)
  {
    dt_print(DT_DEBUG_ALWAYS, "[rawdenoiseai] inference failed (%d) on %dx%d tile, scratch %.1f MB\n", rc, pw, ph,
             dt_nn_unet_scratch_bytes(model, pw, ph) / 1048576.0);
    dt_iop_image_copy_by_size(ovoid, ivoid, width, height, 1);
  }
  else
  {
    _k_blend_crop(in, nn_out, out, width, height, pw, d->strength);
  }

  _nn_region = NULL;
  dt_pixelpipe_cache_free_align(region.base);
  return rc;
}

#ifdef HAVE_OPENCL
/* GPU path. Step for step the same procedure as process(), each _k_* function
 * there having the kernel of the same name here; the whole tile runs
 * dev_in -> dev_out with no mid-tile host round-trip, since command-queue syncs
 * dominate GPU cost. Returns FALSE on any failure so the pipeline falls back
 * to CPU. */
int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi = &piece->roi_in;
  const dt_iop_rawdenoiseai_data_t *const d = (dt_iop_rawdenoiseai_data_t *)piece->data;
  dt_iop_rawdenoiseai_global_data_t *gd = (dt_iop_rawdenoiseai_global_data_t *)self->global_data;
  dt_nn_model_t *const model = d->model;
  const int devid = pipe->devid;

  if(!model || !gd || !gd->nn_cl || !(d->strength > 0.0f)) return FALSE;

  const int width = roi->width, height = roi->height;
  // pre-shifted for the kernel's tile-local Bayer branch, exactly as in process()
  const uint32_t filters = dt_dev_get_roi_filters(piece, roi);
  const int is_xtrans = filters == 9u;

  const int align = dt_nn_model_alignment(model);
  const int pw = (width + align - 1) / align * align;
  const int ph = (height + align - 1) / align * align;
  const size_t plane = (size_t)pw * ph;
  const int in_ch = dt_nn_model_in_channels(model);
  const int bin = dt_nn_model_bin(model, is_xtrans);
  const int cw = pw / bin, chh = ph / bin;
  const size_t cplane = (size_t)cw * chh;
  const int anchor = dt_nn_model_anchor(model);
  const int gw = pw / 16, gh = ph / 16;
  const size_t p16 = (size_t)gw * gh;

  float scale[3];
  _sigma_scale(d, scale);

  /* The whole tile runs on the device: assembly, binning, both network
   * stages, residuals, low-band fusion and the final blend are enqueued as
   * one chain with no mid-tile readback — command-queue syncs dominate GPU
   * cost, not arithmetic. The CPU path (process()) is the bit-parity
   * reference for every step. */
  gboolean success = FALSE;
  int err = CL_SUCCESS;
  cl_mem dev_planes = dt_opencl_alloc_device_buffer(devid, plane * in_ch * sizeof(float));
  cl_mem dev_noise = dt_opencl_alloc_device_buffer(devid, plane * sizeof(float));
  cl_mem dev_den = dt_opencl_alloc_device_buffer(devid, plane * sizeof(float));
  cl_mem dev_xtrans = dt_opencl_alloc_device_buffer(devid, 36);
  /* The pipeline hands modules their I/O as image2d objects (CL_R float here),
   * but every kernel in this chain addresses plain float buffers — passing an
   * image where a buffer is declared makes clSetKernelArg fail with
   * CL_INVALID_MEM_OBJECT and the enqueue with CL_INVALID_KERNEL_ARGS. Convert
   * at the endpoints only: one device-side image->buffer copy of the input, one
   * buffer->image copy of the blended result. The interior chain is untouched
   * and stays device-resident. */
  cl_mem dev_in_buf = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * sizeof(float));
  cl_mem dev_out_buf = dt_opencl_alloc_device_buffer(devid, (size_t)width * height * sizeof(float));
  cl_mem dev_cin = NULL, dev_chead = NULL, dev_cden = NULL;
  // M/D/V per level (16, 32, 64) then the fused ping-pong pair
  cl_mem grids[11] = { NULL };
  if(!dev_planes || !dev_noise || !dev_den || !dev_xtrans || !dev_in_buf || !dev_out_buf) goto cleanup;
  {
    size_t origin[3] = { 0, 0, 0 };
    size_t region[3] = { (size_t)width, (size_t)height, 1 };
    err = dt_opencl_enqueue_copy_image_to_buffer(devid, dev_in, dev_in_buf, origin, region, 0);
    if(err != CL_SUCCESS) goto cleanup;
  }
  if(bin > 1)
  {
    dev_cin = dt_opencl_alloc_device_buffer(devid, cplane * 6 * sizeof(float));
    dev_chead = dt_opencl_alloc_device_buffer(devid, cplane * 3 * sizeof(float));
    dev_cden = dt_opencl_alloc_device_buffer(devid, cplane * 3 * sizeof(float));
    if(!dev_cin || !dev_chead || !dev_cden) goto cleanup;
  }
  // same gate as the CPU path's _apply_low_band_anchor(), so both devices fuse
  // or skip together — the alignment guarantees the modulo holds
  const int do_anchor = anchor > 0 && pw % DT_NN_FUSION_COARSEST == 0 && ph % DT_NN_FUSION_COARSEST == 0;
  if(do_anchor)
  {
    for(int k = 0; k < 11; k++)
    {
      grids[k] = dt_opencl_alloc_device_buffer(devid, p16 * 3 * sizeof(float));
      if(!grids[k]) goto cleanup;
    }
  }

  {
    unsigned char xtrans_host[36]; // staging copy: the CL API takes a non-const pointer
    memcpy(xtrans_host, piece->dsc_in.xtrans, sizeof(xtrans_host));
    if(dt_opencl_write_buffer_to_device(devid, xtrans_host, dev_xtrans, 0, 36, CL_TRUE) != CL_SUCCESS)
      goto cleanup;
  }

  // 1. assemble the base planes (reflect pad + one-hot + sigma)
  {
    const int K = gd->k_assemble;
    const unsigned int f = filters;
    dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &dev_in_buf);
    dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &dev_planes);
    dt_opencl_set_kernel_arg(devid, K, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, K, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, K, 4, sizeof(int), &pw);
    dt_opencl_set_kernel_arg(devid, K, 5, sizeof(int), &ph);
    dt_opencl_set_kernel_arg(devid, K, 6, sizeof(unsigned int), &f);
    dt_opencl_set_kernel_arg(devid, K, 7, sizeof(cl_mem), &dev_xtrans);
    dt_opencl_set_kernel_arg(devid, K, 8, sizeof(int), &is_xtrans);
    dt_opencl_set_kernel_arg(devid, K, 9, sizeof(int), &roi->x);
    dt_opencl_set_kernel_arg(devid, K, 10, sizeof(int), &roi->y);
    for(int c = 0; c < 3; c++) dt_opencl_set_kernel_arg(devid, K, 11 + c, sizeof(float), &d->a[c]);
    for(int c = 0; c < 3; c++) dt_opencl_set_kernel_arg(devid, K, 14 + c, sizeof(float), &d->b[c]);
    for(int c = 0; c < 3; c++) dt_opencl_set_kernel_arg(devid, K, 17 + c, sizeof(float), &scale[c]);
    size_t sizes[3] = { ROUNDUPDWD(pw, devid), ROUNDUPDHT(ph, devid), 1 };
    err = dt_opencl_enqueue_kernel_2d(devid, K, sizes);
    if(err != CL_SUCCESS) goto cleanup;
  }

  // 2. coarse chroma pass (multiscale models)
  if(bin > 1)
  {
    const int K = gd->k_bin_planes;
    dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &dev_planes);
    dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &dev_cin);
    dt_opencl_set_kernel_arg(devid, K, 2, sizeof(int), &pw);
    dt_opencl_set_kernel_arg(devid, K, 3, sizeof(int), &ph);
    dt_opencl_set_kernel_arg(devid, K, 4, sizeof(int), &bin);
    for(int c = 0; c < 3; c++) dt_opencl_set_kernel_arg(devid, K, 5 + c, sizeof(float), &d->a[c]);
    for(int c = 0; c < 3; c++) dt_opencl_set_kernel_arg(devid, K, 8 + c, sizeof(float), &d->b[c]);
    for(int c = 0; c < 3; c++) dt_opencl_set_kernel_arg(devid, K, 11 + c, sizeof(float), &scale[c]);
    size_t sizes[3] = { ROUNDUPDWD(cw, devid), ROUNDUPDHT(chh * 3, devid), 1 };
    err = dt_opencl_enqueue_kernel_2d(devid, K, sizes);
    if(err != CL_SUCCESS) goto cleanup;

    err = dt_nn_unet_apply_stage_cl(model, 1, gd->nn_cl, devid, dev_cin, dev_chead, cw, chh);
    if(err != CL_SUCCESS) goto cleanup;
    const int KR = gd->k_residual;
    const int n3 = (int)(cplane * 3);
    dt_opencl_set_kernel_arg(devid, KR, 0, sizeof(cl_mem), &dev_cin);
    dt_opencl_set_kernel_arg(devid, KR, 1, sizeof(cl_mem), &dev_chead);
    dt_opencl_set_kernel_arg(devid, KR, 2, sizeof(cl_mem), &dev_cden);
    dt_opencl_set_kernel_arg(devid, KR, 3, sizeof(int), &n3);
    size_t sz[3] = { ROUNDUPDWD(n3, devid), 1, 1 };
    err = dt_opencl_enqueue_kernel_2d(devid, KR, sz);
    if(err != CL_SUCCESS) goto cleanup;
    cl_mem guide_src = dev_cden;
    const int KU = gd->k_upsample_n;
    const int three = 3;
    const cl_long dst_off = (cl_long)plane * 5;
    dt_opencl_set_kernel_arg(devid, KU, 0, sizeof(cl_mem), &guide_src);
    dt_opencl_set_kernel_arg(devid, KU, 1, sizeof(cl_mem), &dev_planes);
    dt_opencl_set_kernel_arg(devid, KU, 2, sizeof(int), &cw);
    dt_opencl_set_kernel_arg(devid, KU, 3, sizeof(int), &chh);
    dt_opencl_set_kernel_arg(devid, KU, 4, sizeof(int), &bin);
    dt_opencl_set_kernel_arg(devid, KU, 5, sizeof(int), &three);
    dt_opencl_set_kernel_arg(devid, KU, 6, sizeof(cl_long), &dst_off);
    size_t sizes_u[3] = { ROUNDUPDWD(pw, devid), ROUNDUPDHT(ph * 3, devid), 1 };
    err = dt_opencl_enqueue_kernel_2d(devid, KU, sizes_u);
    if(err != CL_SUCCESS) goto cleanup;
  }

  // 3. fine pass (raw noise) + residual -> denoised plane
  err = dt_nn_unet_apply_stage_cl(model, 0, gd->nn_cl, devid, dev_planes, dev_noise, pw, ph);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[rawdenoiseai] GPU inference failed on %dx%d tile, falling back to CPU\n", pw, ph);
    goto cleanup;
  }
  {
    const int KR = gd->k_residual;
    const int n1 = (int)plane;
    dt_opencl_set_kernel_arg(devid, KR, 0, sizeof(cl_mem), &dev_planes);
    dt_opencl_set_kernel_arg(devid, KR, 1, sizeof(cl_mem), &dev_noise);
    dt_opencl_set_kernel_arg(devid, KR, 2, sizeof(cl_mem), &dev_den);
    dt_opencl_set_kernel_arg(devid, KR, 3, sizeof(int), &n1);
    size_t sz[3] = { ROUNDUPDWD(n1, devid), 1, 1 };
    err = dt_opencl_enqueue_kernel_2d(devid, KR, sz);
    if(err != CL_SUCCESS) goto cleanup;
  }

  // 4. hybrid low-band fusion, entirely on device
  if(do_anchor)
  {
    {
      const int K = gd->k_bin16_mdv;
      dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &dev_planes);
      dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &dev_den);
      dt_opencl_set_kernel_arg(devid, K, 2, sizeof(cl_mem), &grids[0]);
      dt_opencl_set_kernel_arg(devid, K, 3, sizeof(cl_mem), &grids[1]);
      dt_opencl_set_kernel_arg(devid, K, 4, sizeof(cl_mem), &grids[2]);
      dt_opencl_set_kernel_arg(devid, K, 5, sizeof(int), &pw);
      dt_opencl_set_kernel_arg(devid, K, 6, sizeof(int), &ph);
      size_t sizes[3] = { ROUNDUPDWD(gw, devid), ROUNDUPDHT(gh * 3, devid), 1 };
      err = dt_opencl_enqueue_kernel_2d(devid, K, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }
    const int nlev = _fusion_levels();
    int lw = gw, lh = gh;
    for(int k = 1; k < nlev; k++)
    {
      for(int md = 0; md < 3; md++)
      {
        const int K = gd->k_avg2x2;
        dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &grids[3 * (k - 1) + md]);
        dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &grids[3 * k + md]);
        dt_opencl_set_kernel_arg(devid, K, 2, sizeof(int), &lw);
        dt_opencl_set_kernel_arg(devid, K, 3, sizeof(int), &lh);
        size_t sizes[3] = { ROUNDUPDWD(lw / 2, devid), ROUNDUPDHT((lh / 2) * 3, devid), 1 };
        err = dt_opencl_enqueue_kernel_2d(devid, K, sizes);
        if(err != CL_SUCCESS) goto cleanup;
      }
      lw /= 2;
      lh /= 2;
    }
    // floor band: structure-gated blend (see the CPU comment)
    int fA = 9, fB = 10; // fused ping-pong, past the 3 x 3 level grids
    {
      const int K = gd->k_floor_fuse;
      const int S = 16 << (nlev - 1);
      dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &grids[3 * (nlev - 1)]);
      dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &grids[3 * (nlev - 1) + 1]);
      dt_opencl_set_kernel_arg(devid, K, 2, sizeof(cl_mem), &grids[fA]);
      dt_opencl_set_kernel_arg(devid, K, 3, sizeof(cl_mem), &grids[3 * (nlev - 1) + 2]);
      dt_opencl_set_kernel_arg(devid, K, 4, sizeof(int), &lw);
      dt_opencl_set_kernel_arg(devid, K, 5, sizeof(int), &lh);
      dt_opencl_set_kernel_arg(devid, K, 6, sizeof(int), &S);
      for(int c = 0; c < 3; c++) dt_opencl_set_kernel_arg(devid, K, 7 + c, sizeof(float), &DT_NN_FUSION_DENS[c]);
      size_t sizes[3] = { ROUNDUPDWD(lw, devid), ROUNDUPDHT(lh * 3, devid), 1 };
      err = dt_opencl_enqueue_kernel_2d(devid, K, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }
    for(int k = nlev - 2; k >= 0; k--)
    {
      const int fw = lw * 2, fh = lh * 2, sc = 16 << k;
      {
        const int K = gd->k_fuse_step;
        dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &grids[fA]);
        dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &grids[3 * k]);
        dt_opencl_set_kernel_arg(devid, K, 2, sizeof(cl_mem), &grids[3 * k + 1]);
        dt_opencl_set_kernel_arg(devid, K, 3, sizeof(cl_mem), &grids[3 * (k + 1)]);
        dt_opencl_set_kernel_arg(devid, K, 4, sizeof(cl_mem), &grids[3 * (k + 1) + 1]);
        dt_opencl_set_kernel_arg(devid, K, 5, sizeof(cl_mem), &grids[fB]);
        dt_opencl_set_kernel_arg(devid, K, 6, sizeof(cl_mem), &grids[3 * k + 2]);
        dt_opencl_set_kernel_arg(devid, K, 7, sizeof(int), &fw);
        dt_opencl_set_kernel_arg(devid, K, 8, sizeof(int), &fh);
        dt_opencl_set_kernel_arg(devid, K, 9, sizeof(int), &sc);
        for(int c = 0; c < 3; c++) dt_opencl_set_kernel_arg(devid, K, 10 + c, sizeof(float), &DT_NN_FUSION_DENS[c]);
        size_t sizes[3] = { ROUNDUPDWD(fw, devid), ROUNDUPDHT(fh * 3, devid), 1 };
        err = dt_opencl_enqueue_kernel_2d(devid, K, sizes);
        if(err != CL_SUCCESS) goto cleanup;
      }
      const int t = fA;
      fA = fB;
      fB = t;
      lw = fw;
      lh = fh;
    }
    {
      const int K = gd->k_bilerp_add;
      dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &grids[fA]);
      dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &grids[1]);
      dt_opencl_set_kernel_arg(devid, K, 2, sizeof(cl_mem), &dev_planes);
      dt_opencl_set_kernel_arg(devid, K, 3, sizeof(cl_mem), &dev_den);
      dt_opencl_set_kernel_arg(devid, K, 4, sizeof(int), &pw);
      dt_opencl_set_kernel_arg(devid, K, 5, sizeof(int), &ph);
      size_t sizes[3] = { ROUNDUPDWD(pw, devid), ROUNDUPDHT(ph, devid), 1 };
      err = dt_opencl_enqueue_kernel_2d(devid, K, sizes);
      if(err != CL_SUCCESS) goto cleanup;
    }
  }

  // 5. strength blend + crop straight into the pipeline output
  {
    const int K = gd->k_blend_crop;
    dt_opencl_set_kernel_arg(devid, K, 0, sizeof(cl_mem), &dev_in_buf);
    dt_opencl_set_kernel_arg(devid, K, 1, sizeof(cl_mem), &dev_den);
    dt_opencl_set_kernel_arg(devid, K, 2, sizeof(cl_mem), &dev_out_buf);
    dt_opencl_set_kernel_arg(devid, K, 3, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, K, 4, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, K, 5, sizeof(int), &pw);
    dt_opencl_set_kernel_arg(devid, K, 6, sizeof(float), &d->strength);
    size_t sizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    err = dt_opencl_enqueue_kernel_2d(devid, K, sizes);
    if(err != CL_SUCCESS) goto cleanup;
  }
  {
    // hand the result back in the pipeline's format (see the entry copy above)
    size_t origin[3] = { 0, 0, 0 };
    size_t region[3] = { (size_t)width, (size_t)height, 1 };
    err = dt_opencl_enqueue_copy_buffer_to_image(devid, dev_out_buf, dev_out, 0, origin, region);
    if(err != CL_SUCCESS) goto cleanup;
  }
  success = TRUE;

cleanup:
  if(dev_planes) dt_opencl_release_mem_object(dev_planes);
  if(dev_noise) dt_opencl_release_mem_object(dev_noise);
  if(dev_den) dt_opencl_release_mem_object(dev_den);
  if(dev_xtrans) dt_opencl_release_mem_object(dev_xtrans);
  if(dev_in_buf) dt_opencl_release_mem_object(dev_in_buf);
  if(dev_out_buf) dt_opencl_release_mem_object(dev_out_buf);
  if(dev_cin) dt_opencl_release_mem_object(dev_cin);
  if(dev_chead) dt_opencl_release_mem_object(dev_chead);
  if(dev_cden) dt_opencl_release_mem_object(dev_cden);
  for(int k = 0; k < 11; k++)
    if(grids[k]) dt_opencl_release_mem_object(grids[k]);
  return success;
}
#endif // HAVE_OPENCL

void init(dt_iop_module_t *module)
{
  dt_iop_default_init(module);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_rawdenoiseai_data_t));
  piece->data_size = sizeof(dt_iop_rawdenoiseai_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

/* The combo lists "(shipped model)" then whatever .anselnn files the config
 * dir holds. The VALUE written to params is the basename, never the index —
 * see the params comment. gui_update re-derives the index from the name and
 * appends an entry for a name that is no longer on disk, so an edit made with
 * a since-removed model still shows what it wants instead of silently
 * displaying the wrong one. */
static void _custom_model_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_request_focus(self);
  dt_iop_rawdenoiseai_params_t *p = (dt_iop_rawdenoiseai_params_t *)self->params;
  const int idx = dt_bauhaus_combobox_get(w);
  const char *txt = idx > 0 ? dt_bauhaus_combobox_get_text(w) : NULL;
  if(txt)
    g_strlcpy(p->custom_model, txt, sizeof(p->custom_model));
  else
    p->custom_model[0] = '\0';
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void _custom_model_populate(dt_iop_module_t *self)
{
  dt_iop_rawdenoiseai_gui_data_t *g = (dt_iop_rawdenoiseai_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_rawdenoiseai_params_t *const p = (dt_iop_rawdenoiseai_params_t *)self->params;
  dt_bauhaus_combobox_clear(g->custom_model);
  dt_bauhaus_combobox_add(g->custom_model, _("(shipped model)"));
  GList *files = _list_custom_models();
  int sel = 0, i = 0;
  for(GList *l = files; l; l = g_list_next(l))
  {
    dt_bauhaus_combobox_add(g->custom_model, (const char *)l->data);
    i++;
    if(!g_strcmp0((const char *)l->data, p->custom_model)) sel = i;
  }
  // the edit names a file that is not there any more: keep it visible
  if(p->custom_model[0] && !sel)
  {
    dt_bauhaus_combobox_add(g->custom_model, p->custom_model);
    sel = i + 1;
  }
  g_list_free_full(files, g_free);
  dt_bauhaus_combobox_set(g->custom_model, sel);
  // the shipped-matrix combos are meaningless while a user model is selected
  const gboolean shipped = !p->custom_model[0];
  gtk_widget_set_sensitive(g->version, shipped);
  gtk_widget_set_sensitive(g->size, shipped);
  gtk_widget_set_sensitive(g->scale_variant, shipped);
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rawdenoiseai_gui_data_t *g = (dt_iop_rawdenoiseai_gui_data_t *)dt_iop_gui_data(self);
  const dt_iop_rawdenoiseai_params_t *p = (dt_iop_rawdenoiseai_params_t *)self->params;
  dt_iop_rawdenoiseai_global_data_t *gd = (dt_iop_rawdenoiseai_global_data_t *)self->global_data;
  gtk_stack_set_visible_child_name(GTK_STACK(self->gui->widget), self->hide_enable_button ? "unsupported" : "raw");

  // rescan on every panel update: the user may have dropped a file in since
  _custom_model_populate(self);

  // two-line status: the selected model (warn if its weights are missing) and
  // the noise profile the sigma map will use
  const gboolean have_model = gd && (p->custom_model[0] ? _get_custom_model(gd, p->custom_model)
                                                        : _get_model(gd, p->version, p->size, p->scale_variant));
  GList *profiles = dt_noiseprofile_get_matching(&self->dev->image_storage);
  gchar *prof = profiles
                    ? g_strdup_printf(_("noise profile: %s at ISO %d"), self->dev->image_storage.camera_makermodel,
                                      (int)self->dev->image_storage.exif_iso)
                    : g_strdup(_("no noise profile for this camera — using the generic profile"));
  gchar *label = have_model
                     ? g_strdup(prof)
                     : g_strdup_printf(_("selected model (%s %s, %s) is not installed — module inactive\n%s"),
                                       _size_tag[CLAMP(p->size, 0, DT_RAWDENOISEAI_NUM_SIZES - 1)],
                                       _scale_tag[CLAMP(p->scale_variant, 0, DT_RAWDENOISEAI_NUM_SCALES - 1)],
                                       _version_tag[CLAMP(p->version, 0, DT_RAWDENOISEAI_NUM_VERSIONS - 1)], prof);
  gtk_label_set_text(GTK_LABEL(g->profile_label), label);
  dt_gui_update_collapsible_section(&g->cs);
  g_free(label);
  g_free(prof);
  g_list_free_full(profiles, dt_noiseprofile_free);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_rawdenoiseai_gui_data_t *g = IOP_GUI_ALLOC(rawdenoiseai);

  GtkWidget *box_raw = self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  dt_bauhaus_slider_set_digits(g->strength, 3);
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("opacity of the noise removal: blends between the original\n"
                                             "image (0%) and the fully denoised result (100%).\n"
                                             "lower it to keep some residual grain"));

  g->version = dt_bauhaus_combobox_from_params(self, "version");
  gtk_widget_set_tooltip_text(g->version, _("neural model version. Older edits keep their original\n"
                                            "version so their result never changes across updates."));

  g->size = dt_bauhaus_combobox_from_params(self, "size");
  gtk_widget_set_tooltip_text(g->size, _("network width. large: reference quality, practical on GPU\n"
                                         "(OpenCL) and the default there. half: ~4x faster.\n"
                                         "quarter: ~4x faster again, the default without OpenCL\n"
                                         "and the choice for weak hardware or near-realtime editing."));

  g->scale_variant = dt_bauhaus_combobox_from_params(self, "scale_variant");
  gtk_widget_set_tooltip_text(g->scale_variant, _("single-scale: the fine full-resolution pass only — fast,\n"
                                                  "no low-frequency chroma handling. multiscale: adds the\n"
                                                  "coarse chroma pass and the low-band fusion — high quality,\n"
                                                  "recommended for high ISO."));

  g->custom_model = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->custom_model, N_("custom model"));
  gtk_box_pack_start(GTK_BOX(box_raw), g->custom_model, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->custom_model,
                              _("use a neural model of your own instead of the shipped ones.\n"
                                "drop a .anselnn file into your Ansel config directory and it\n"
                                "appears here; the edit records the file NAME, so it keeps\n"
                                "pointing at the same model as the folder changes."));
  g_signal_connect(G_OBJECT(g->custom_model), "value-changed", G_CALLBACK(_custom_model_callback), self);

  gtk_box_pack_start(GTK_BOX(box_raw), dt_ui_section_label_new(_("noise profile correction")), FALSE, FALSE, 0);

  g->profile_label = dt_ui_label_new("");
  gtk_label_set_line_wrap(GTK_LABEL(g->profile_label), TRUE);
  gtk_box_pack_start(GTK_BOX(box_raw), g->profile_label, FALSE, FALSE, 0);

  g->noise_level = dt_bauhaus_slider_from_params(self, "noise_level");
  dt_bauhaus_slider_set_digits(g->noise_level, 3);
  dt_bauhaus_slider_set_format(g->noise_level, "%");
  gtk_widget_set_tooltip_text(g->noise_level, _("global scale on the assumed noise amplitude, relative to the\n"
                                                "calibrated noise for this camera at this ISO (100% trusts the\n"
                                                "calibration). raise it if noise remains, lower it if fine\n"
                                                "detail is eaten"));

  dt_gui_new_collapsible_section(&g->cs, "plugins/darkroom/rawdenoiseai/expand_channel",
                                 _("per-channel corrections"), GTK_BOX(box_raw), GTK_PACK_START);
  self->gui->widget = GTK_WIDGET(g->cs.container); // sliders below pack into the section

  const char *sigma_tooltip = _("per-channel correction of the camera noise profile, applied on top\n"
                                "of the global correction. Profiles are measured after demosaicing,\n"
                                "which averages away part of the noise — most on the dense green\n"
                                "lattice — while this module sees the raw sensor noise at full\n"
                                "strength. The defaults are calibrated against raw-mosaic\n"
                                "measurements over 253 cameras.");
  g->sigma_red = dt_bauhaus_slider_from_params(self, "sigma_red");
  dt_bauhaus_slider_set_digits(g->sigma_red, 3);
  dt_bauhaus_slider_set_format(g->sigma_red, "%");
  gtk_widget_set_tooltip_text(g->sigma_red, sigma_tooltip);

  g->sigma_green = dt_bauhaus_slider_from_params(self, "sigma_green");
  dt_bauhaus_slider_set_digits(g->sigma_green, 3);
  dt_bauhaus_slider_set_format(g->sigma_green, "%");
  gtk_widget_set_tooltip_text(g->sigma_green, sigma_tooltip);

  g->sigma_blue = dt_bauhaus_slider_from_params(self, "sigma_blue");
  dt_bauhaus_slider_set_digits(g->sigma_blue, 3);
  dt_bauhaus_slider_set_format(g->sigma_blue, "%");
  gtk_widget_set_tooltip_text(g->sigma_blue, sigma_tooltip);

  self->gui->widget = box_raw; // done packing into the collapsible section

  GtkWidget *label_unsupported = dt_ui_label_new(_("AI raw denoising needs a mosaiced raw image\n"
                                                   "and an installed rawdenoiseai model file."));

  self->gui->widget = gtk_stack_new();
  gtk_stack_set_homogeneous(GTK_STACK(self->gui->widget), FALSE);
  gtk_stack_add_named(GTK_STACK(self->gui->widget), label_unsupported, "unsupported");
  gtk_stack_add_named(GTK_STACK(self->gui->widget), box_raw, "raw");
}

void gui_cleanup(dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
