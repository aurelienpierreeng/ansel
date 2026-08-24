/*
    This file is part of the Ansel project.
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#include "caches/pixelpipe_cache_alloc.h"
#endif

#include "widgets/bauhaus.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "develop/iop_profile.h"
#include "math/math.h"
#include "system/macros.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "system/mem_alloc.h"
#include "common/module_versioning.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "iop/noise_generator.h"
#include "gui/presets.h"
#include "iop/iop_api.h"

#include <float.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(9, dt_iop_crystgrain_params_t)

#define DT_CRYSTGRAIN_LAYER_KERNELS 16

// Smallest crystal radius, in current grid pixels, the rasterizer is asked to
// resolve. Below that a crystal covers less than a millionth of a pixel and its
// germ intensity would explode without any visible return.
#define DT_CRYSTGRAIN_MIN_RADIUS 1e-3f

typedef enum dt_iop_crystgrain_mode_t
{
  DT_CRYSTGRAIN_MONO = 0, // $DESCRIPTION: "B&W"
  DT_CRYSTGRAIN_COLOR = 1 // $DESCRIPTION: "color"
} dt_iop_crystgrain_mode_t;

typedef struct dt_iop_crystgrain_params_t
{
  dt_iop_crystgrain_mode_t mode; // $DEFAULT: DT_CRYSTGRAIN_MONO $DESCRIPTION: "mode"
  float filling;                 // $MIN: 0.0 $MAX: 95.0 $DEFAULT: 25.0 $DESCRIPTION: "Average layer filling"
  float grain_size;              // $MIN: 1.0 $MAX: 31.0 $DEFAULT: 4.0 $DESCRIPTION: "Crystals average size"
  int layers;                    // $MIN: 1 $MAX: 64 $DEFAULT: 30 $DESCRIPTION: "Crystals layers"
  float size_stddev;             // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 0.25 $DESCRIPTION: "Crystals size variability"
  float layer_capture;           // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "Layer sensitivity"
  float channel_correlation;     // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 67.0 $DESCRIPTION: "Inter-channel grain correlation"
  float colorspace_saturation;   // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 67.0 $DESCRIPTION: "Grain colorfulness"
} dt_iop_crystgrain_params_t;

typedef struct dt_iop_crystgrain_gui_data_t
{
  GtkWidget *mode;
  GtkWidget *filling;
  GtkWidget *grain_size;
  GtkWidget *layers;
  GtkWidget *size_stddev;
  GtkWidget *layer_capture;
  GtkWidget *channel_correlation;
  GtkWidget *colorspace_saturation;
} dt_iop_crystgrain_gui_data_t;

typedef struct dt_iop_crystgrain_data_t
{
  dt_iop_crystgrain_mode_t mode;
  float filling;
  float grain_size;
  int layers;
  float size_stddev;
  float layer_capture;
  float channel_correlation;
  float colorspace_saturation;
} dt_iop_crystgrain_data_t;

typedef struct dt_iop_crystgrain_kernel_t
{
  int count;
  int radius;
  float radius_f;
  float area;
  int *dx;
  int *dy;
  float *alpha;
} dt_iop_crystgrain_kernel_t;

typedef struct dt_iop_crystgrain_layer_kernel_t
{
  dt_iop_crystgrain_kernel_t footprint;
  float intensity; // crystals per grid pixel, see _seed_intensity()
  float vertices;
  float rotation;
} dt_iop_crystgrain_layer_kernel_t;

typedef struct dt_iop_crystgrain_runtime_t
{
  int width;
  int height;
  int roi_x;
  int roi_y;
  int layers;
  float layer_scale;
  float filling;
  float grain_size;
  float size_stddev;
  float kernel_scale;
  float inv_scale;
  float channel_correlation;
  uint64_t base_seed;
} dt_iop_crystgrain_runtime_t;

typedef struct dt_iop_crystgrain_color_state_t
{
  const float *image;
  float *result;
  float *remaining;
} dt_iop_crystgrain_color_state_t;

#ifdef HAVE_OPENCL
typedef struct dt_iop_crystgrain_global_data_t
{
  int kernel_zero_scalar;
  int kernel_zero_rgb;
  int kernel_extract_luminance;
  int kernel_extract_rgb;
  int kernel_simulate_layer;
  int kernel_simulate_layer_color;
  int kernel_apply_mono;
  int kernel_finalize_color;
} dt_iop_crystgrain_global_data_t;
#endif


const char *name()
{
  return _("Photographic grain");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("simulate photographic grain from stacked silver-halide crystal layers"),
                                      _("creative"),
                                      _("non-linear, RGB, scene-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, scene-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_EFFECTS;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_crystgrain_params_t p = { 0 };

  p.mode = DT_CRYSTGRAIN_COLOR;
  p.filling = 85.0f;
  p.grain_size = 4.0f;
  p.layers = 30;
  p.size_stddev = 0.25f;
  p.layer_capture = 0.0f;
  p.channel_correlation = 67.0f;
  p.colorspace_saturation = 67.0f;
  dt_gui_presets_add_generic(_("color grain"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);

  p.mode = DT_CRYSTGRAIN_MONO;
  p.filling = 25.0f;
  p.grain_size = 4.0f;
  p.layers = 30;
  p.size_stddev = 0.25f;
  p.layer_capture = 0.0f;
  p.channel_correlation = 67.0f;
  p.colorspace_saturation = 67.0f;
  dt_gui_presets_add_generic(_("B&W grain"), self->op, self->version(), &p, sizeof(p), 1,
                             DEVELOP_BLEND_CS_RGB_SCENE);
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version, void *new_params,
                  const int new_version)
{
  if((old_version == 1 || old_version == 8) && new_version == 9)
  {
    const dt_iop_crystgrain_params_t *o = old_params;
    dt_iop_crystgrain_params_t *n = new_params;
    *n = *o;
    return 0;
  }

  return 1;
}

/**
 * @brief Hash a string into a stable 32-bit seed.
 */
__DT_CLONE_TARGETS__
static unsigned int _hash_string(const char *s)
{
  unsigned int h = 0;
  while(*s) h = 33 * h ^ (unsigned int)*s++;
  return h;
}

/**
 * @brief Turn a 64-bit seed into a uniform random number in [0; 1).
 */
static inline float _uniform_random(const uint64_t seed)
{
  return splitmix32(seed) * 0x1.0p-32f;
}

/**
 * @brief Turn 2 seeds into one gaussian deviate.
 *
 * @details We only need gaussian draws to pick crystal size and vertex count
 * for one whole layer, so Box-Muller is enough and keeps the implementation
 * local to this module.
 */
static inline float _gaussian_random(const uint64_t seed_a, const uint64_t seed_b)
{
  const float u1 = fmaxf(_uniform_random(seed_a), FLT_MIN);
  const float u2 = _uniform_random(seed_b);
  return sqrtf(-2.0f * logf(u1)) * cosf(2.0f * M_PI_F * u2);
}

/**
 * @brief Mirror indices outside the current buffer like scipy
 * `boundary='symm'`.
 */
static inline int _reflect_index(int i, const int max)
{
  if(max <= 1) return 0;

  while(i < 0 || i >= max)
  {
    if(i < 0)
      i = -i - 1;
    else
      i = 2 * max - i - 1;
  }

  return i;
}

/**
 * @brief Map the requested filling ratio to the germ intensity used to plant
 * seeds, in crystals per grid pixel.
 *
 * @details The crystal population is a boolean (germ-grain) model: germs are
 * a Poisson point process of intensity `lambda` germs per unit surface, and
 * every germ grows one crystal of surface `A`. The probability that a given
 * location is left uncovered is `exp(-lambda * A)`, so matching a requested
 * filling ratio `f` amounts to solving `1 - f = exp(-lambda * A)`, that is
 * `lambda = -ln(1 - f) / A`.
 *
 * We express that intensity per pixel of the *current* grid, so `A` is the
 * crystal surface measured in current grid pixels. Note the invariant that
 * makes the whole model scale-free: the germ mass reaching one pixel is
 * `lambda * A = -ln(1 - f)`, independent of the crystal surface and therefore
 * of the resolution the pipeline happens to run at.
 *
 * Unlike a per-pixel Bernoulli draw, this intensity is *not* capped at one
 * crystal per pixel. Below 100% zoom a crystal eventually becomes smaller
 * than one grid pixel and several of them land in the same pixel; that
 * multiplicity is exactly what averages the grain out at low resolution, so
 * it must be sampled, not truncated.
 */
static inline float _seed_intensity(const float filling, const float crystal_area)
{
  const float clamped_filling = CLAMPS(filling, 0.0f, 0.9999f);
  return -logf(1.0f - clamped_filling) / MAX(crystal_area, FLT_EPSILON);
}

/**
 * @brief Draw one Poisson deviate of mean `mu`.
 *
 * @details Small means use Knuth's product method, which costs `O(mu)` random
 * draws. Large means (deep zoom-outs, where each grid pixel holds many
 * sub-pixel crystals) switch to the gaussian approximation with a continuity
 * correction, so the per-pixel cost stays `O(1)` no matter how far the image
 * is downscaled. The switch sits at `mu = 12`, where the relative error of
 * the gaussian approximation is already below the stochastic spread of the
 * grain itself.
 */
static inline int _poisson_random(const uint64_t seed, const float mu)
{
  if(!(mu > 0.0f)) return 0;

  if(mu < 12.0f)
  {
    const float target = expf(-mu);
    float product = 1.0f;
    int count = 0;

    while(product > target && count < 64)
    {
      count++;
      product *= _uniform_random(seed + (uint64_t)count * 0x9e3779b97f4a7c15ull);
    }

    return MAX(count - 1, 0);
  }

  const float deviate = mu + sqrtf(mu) * _gaussian_random(seed ^ 0xbf58476d1ce4e5b9ull,
                                                          seed ^ 0x94d049bb133111ebull);
  return MAX((int)floorf(deviate + 0.5f), 0);
}

/**
 * @brief Analytic surface of one crystal, in current grid pixels squared.
 *
 * @details The polar envelope below draws a regular polygon of `vertices`
 * sides whose circumradius is `radius_f`, so its surface is the textbook
 * `n R^2 sin(2 pi / n) / 2`. We need that surface analytically — and not only
 * as the sum of the rasterized weights — because it is the reference the
 * rasterization gets normalized against, and because it stays exact when the
 * whole crystal is smaller than one pixel.
 */
static inline float _polygon_area(const float radius_f, const float vertices)
{
  return 0.5f * vertices * radius_f * radius_f * sinf(2.0f * M_PI_F / vertices);
}

/**
 * @brief Polar radius of one crystal boundary at angle `theta`.
 */
static inline float _polygon_radius(const float theta, const float radius_f, const float vertices,
                                    const float rotation)
{
  const float envelope = cosf(M_PI_F / vertices)
                          / cosf((2.0f * asinf(cosf(vertices * (theta + rotation))) + M_PI_F)
                                / (2.0f * vertices));
  return radius_f * envelope;
}

#define DT_CRYSTGRAIN_SUBSAMPLES 8

/**
 * @brief Estimate the surface of one pixel covered by one crystal.
 *
 * @details This is a box-filtered (area-exact) rasterization, not a
 * signed-distance ramp: the returned weight is the fraction of the unit
 * square centred on `(dx, dy)` that lies inside the crystal. That distinction
 * is what makes the model survive downscaling. A ramp saturates at 0.5 for a
 * crystal shrinking to a point, so a sub-pixel crystal would keep printing
 * half a pixel worth of density and the grain would never average out; the
 * true covered area goes to zero with the crystal surface, which is the
 * behaviour a physical emulsion has when it is scanned at a lower resolution.
 *
 * Pixels entirely inside the inradius or entirely outside the circumradius
 * are settled by two comparisons against the half-diagonal of the pixel
 * square, so only the boundary band — `O(radius)` pixels out of `O(radius^2)`
 * — pays for the subsampling.
 */
static inline float _crystal_coverage(const int dx, const int dy, const float radius_f, const float vertices,
                                      const float rotation)
{
  const float inradius = radius_f * cosf(M_PI_F / vertices);
  const float distance = hypotf((float)dx, (float)dy);
  const float half_diagonal = 0.70710678f; // half-diagonal of the unit pixel square

  if(distance + half_diagonal <= inradius) return 1.0f;
  if(distance - half_diagonal >= radius_f) return 0.0f;

  int inside = 0;

  for(int sy = 0; sy < DT_CRYSTGRAIN_SUBSAMPLES; sy++)
  {
    const float py = (float)dy - 0.5f + ((float)sy + 0.5f) / (float)DT_CRYSTGRAIN_SUBSAMPLES;
    for(int sx = 0; sx < DT_CRYSTGRAIN_SUBSAMPLES; sx++)
    {
      const float px = (float)dx - 0.5f + ((float)sx + 0.5f) / (float)DT_CRYSTGRAIN_SUBSAMPLES;
      const float sample_radius = hypotf(px, py);
      if(sample_radius <= _polygon_radius(atan2f(py, px), radius_f, vertices, rotation)) inside++;
    }
  }

  return (float)inside / (float)(DT_CRYSTGRAIN_SUBSAMPLES * DT_CRYSTGRAIN_SUBSAMPLES);
}

/**
 * @brief Build one partially-occluding crystal footprint for a layer.
 *
 * @details Each bank entry keeps one crystal size, shape and orientation, then
 * the stochastic look comes from stacking many layers and randomly picking
 * between several bank entries at each seed position. The support window is
 * rasterized to integer pixels, but each tap stores the fraction of that pixel
 * the crystal actually covers, so a crystal that no longer spans a whole pixel
 * degrades into one tap of fractional weight instead of collapsing onto a
 * binary 1-pixel dot.
 *
 * The rasterized weights are then rescaled so their sum matches the analytic
 * crystal surface exactly. The subsampling quantum is `1 / 64` of a pixel, and
 * that residual is not cosmetic: the seed intensity, the per-crystal capture
 * cap and the flat-field exposure prediction are all expressed against this
 * surface, so letting it drift with the rasterization would make the three of
 * them disagree at the scales where the crystal is only a few subsamples wide.
 * The weights are deliberately *not* clamped back to 1 afterwards — they are
 * energy weights, and clamping would silently break that sum.
 *
 * `kernel->radius` is the tight support radius, so a sub-pixel crystal reports
 * radius 0 and a single tap, which is the fast path the simulation keys on.
 */
__DT_CLONE_TARGETS__
static int _create_crystal_kernel(dt_iop_crystgrain_kernel_t *const kernel, const float radius_f,
                                  const float vertices, const float rotation)
{
  memset(kernel, 0, sizeof(*kernel));

  const float target_area = _polygon_area(radius_f, vertices);
  if(!(target_area > 0.0f)) return 1;

  const int window_radius = MAX((int)ceilf(radius_f + 0.5f), 1);
  const int window_width = 2 * window_radius + 1;
  float *const dense = malloc(sizeof(float) * (size_t)window_width * window_width);
  if(IS_NULL_PTR(dense)) return 1;

  float raster_area = 0.0f;
  for(int y = 0; y < window_width; y++)
  {
    for(int x = 0; x < window_width; x++)
    {
      const float alpha = _crystal_coverage(x - window_radius, y - window_radius, radius_f, vertices, rotation);
      dense[(size_t)y * window_width + x] = alpha;
      raster_area += alpha;
    }
  }

  if(raster_area <= FLT_EPSILON)
  {
    // The crystal is smaller than one subsample: keep it as one tap carrying
    // its exact analytic surface rather than dropping it, so the germ
    // statistics stay valid all the way down.
    memset(dense, 0, sizeof(float) * (size_t)window_width * window_width);
    dense[(size_t)window_radius * window_width + window_radius] = target_area;
  }
  else
  {
    const float gain = target_area / raster_area;
    for(size_t k = 0; k < (size_t)window_width * window_width; k++) dense[k] *= gain;
  }

  int count = 0;
  int radius = 0;
  float area = 0.0f;
  for(int y = 0; y < window_width; y++)
  {
    for(int x = 0; x < window_width; x++)
    {
      const float alpha = dense[(size_t)y * window_width + x];
      if(alpha <= FLT_EPSILON) continue;

      count++;
      area += alpha;
      radius = MAX(radius, MAX(abs(x - window_radius), abs(y - window_radius)));
    }
  }

  if(count <= 0 || area <= FLT_EPSILON)
  {
    free(dense);
    return 1;
  }

  kernel->dx = malloc(sizeof(int) * count);
  kernel->dy = malloc(sizeof(int) * count);
  kernel->alpha = malloc(sizeof(float) * count);
  if(IS_NULL_PTR(kernel->dx) || IS_NULL_PTR(kernel->dy) || IS_NULL_PTR(kernel->alpha))
  {
    free(kernel->dx);
    free(kernel->dy);
    free(kernel->alpha);
    memset(kernel, 0, sizeof(*kernel));
    free(dense);
    return 1;
  }

  kernel->count = count;
  kernel->radius = radius;
  kernel->radius_f = radius_f;
  kernel->area = area;

  int k = 0;
  for(int y = 0; y < window_width; y++)
  {
    for(int x = 0; x < window_width; x++)
    {
      const float alpha = dense[(size_t)y * window_width + x];
      if(alpha <= FLT_EPSILON) continue;

      kernel->dx[k] = x - window_radius;
      kernel->dy[k] = y - window_radius;
      kernel->alpha[k] = alpha;
      k++;
    }
  }

  free(dense);
  return 0;
}
/**
 * @brief Release one crystal kernel.
 */
static inline __attribute__((always_inline)) void _free_crystal_kernel(dt_iop_crystgrain_kernel_t *const kernel)
{
  free(kernel->dx);
  free(kernel->dy);
  free(kernel->alpha);
  memset(kernel, 0, sizeof(*kernel));
}

/**
 * @brief Pick one crystal geometry for one bank entry.
 */
__DT_CLONE_TARGETS__
static int _pick_layer_kernel(dt_iop_crystgrain_layer_kernel_t *const entry,
                              const dt_iop_crystgrain_runtime_t *const rt, const uint64_t seed)
{
  memset(entry, 0, sizeof(*entry));

  // The size distribution is drawn in full-resolution pixels — the unit the
  // slider is authored in — and only converted to the current grid at the very
  // end, by one linear factor. Drawing it directly in grid pixels instead is
  // what used to make the crystal population itself resolution-dependent: the
  // old form subtracted one whole pixel from the size before halving it, and
  // an affine map cannot commute with a scale change, so halving the preview
  // scale shrank the crystals by more than half and eventually pinned them at
  // the 1-pixel floor.
  const float mean_size = MAX(rt->grain_size, 1.0f);
  const float max_size = 3.0f * mean_size;

  for(int attempt = 0; attempt < 8; attempt++)
  {
    const float vertices = CLAMPS(6.0f + 1.5f * _gaussian_random(seed + 17u + attempt * 31u,
                                                                  seed + 23u + attempt * 37u),
                                  3.0f, 10.0f);
    const float rotation = 2.0f * M_PI_F * _uniform_random(seed + 101u + attempt * 43u);
    const float log_size = logf(mean_size) + rt->size_stddev * _gaussian_random(seed + 151u + attempt * 47u,
                                                                                 seed + 181u + attempt * 53u);
    const float random_size = CLAMPS(expf(log_size), 0.25f, max_size);
    const float radius_f = MAX(0.5f * random_size * rt->kernel_scale, DT_CRYSTGRAIN_MIN_RADIUS);

    if(_create_crystal_kernel(&entry->footprint, radius_f, vertices, rotation) == 0)
    {
      entry->intensity = _seed_intensity(rt->filling, entry->footprint.area);
      entry->vertices = vertices;
      entry->rotation = rotation;
      return 0;
    }
  }

  if(_create_crystal_kernel(&entry->footprint, MAX(0.5f * mean_size * rt->kernel_scale, DT_CRYSTGRAIN_MIN_RADIUS),
                            4.0f, 0.0f) != 0)
    return 1;

  entry->intensity = _seed_intensity(rt->filling, entry->footprint.area);
  entry->vertices = 4.0f;
  entry->rotation = 0.0f;
  return 0;
}

/**
 * @brief Estimate the reference grain surface used to normalize layer capture.
 *
 * @details The user-facing layer capture is expressed against the average
 * grain size control, not against the exact randomized footprint drawn for
 * each seed. We therefore normalize it by the area of a disc built from the
 * average grain radius at the current preview scale. This is only the fallback
 * used when no bank could be sampled.
 */
static inline float _average_grain_surface(const dt_iop_crystgrain_runtime_t *const rt)
{
  const float mean_radius = MAX(0.5f * MAX(rt->grain_size, 1.0f) * rt->kernel_scale, DT_CRYSTGRAIN_MIN_RADIUS);
  return M_PI_F * mean_radius * mean_radius;
}

static int _build_layer_kernel_bank(dt_iop_crystgrain_layer_kernel_t *const bank,
                                    const dt_iop_crystgrain_runtime_t *const rt, const uint64_t layer_seed);
static void _free_layer_kernel_bank(dt_iop_crystgrain_layer_kernel_t *const bank);

/**
 * @brief Estimate the actual rasterized grain surface at the current scale.
 *
 * @details The grain size slider lives in continuous preview pixels, but the
 * simulation ultimately grows integer odd-width kernels that get discretized
 * on the raster grid. That quantization is exactly what changes the look at
 * small preview scales, so we normalize layer capture against the average
 * discrete footprint area sampled from a few layer banks instead of against a
 * noisier variance proxy.
 */
__DT_CLONE_TARGETS__
static float _average_discrete_grain_surface(const dt_iop_crystgrain_runtime_t *const rt)
{
  const int sampled_layers = MIN(rt->layers, 4);
  if(sampled_layers <= 0) return _average_grain_surface(rt);

  float total_area = 0.0f;
  int total_kernels = 0;

  for(int layer = 0; layer < sampled_layers; layer++)
  {
    dt_iop_crystgrain_layer_kernel_t bank[DT_CRYSTGRAIN_LAYER_KERNELS];
    const uint64_t layer_seed = rt->base_seed + layer * 4099u;

    if(_build_layer_kernel_bank(bank, rt, layer_seed) != 0)
      return _average_grain_surface(rt);

    for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++)
      total_area += bank[i].footprint.area;

    total_kernels += DT_CRYSTGRAIN_LAYER_KERNELS;
    _free_layer_kernel_bank(bank);
  }

  return (total_area > FLT_EPSILON && total_kernels > 0)
    ? total_area / total_kernels
    : _average_grain_surface(rt);
}

/**
 * @brief Build the crystal bank for one layer.
 *
 * @details We precompute several crystal footprints for the current layer so
 * each accepted seed can randomly pick one geometry without paying the kernel
 * construction cost inside the hot pixel loop.
 */
__DT_CLONE_TARGETS__
static int _build_layer_kernel_bank(dt_iop_crystgrain_layer_kernel_t *const bank,
                                    const dt_iop_crystgrain_runtime_t *const rt, const uint64_t layer_seed)
{
  memset(bank, 0, sizeof(dt_iop_crystgrain_layer_kernel_t) * DT_CRYSTGRAIN_LAYER_KERNELS);

  for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++)
  {
    const uint64_t kernel_seed = layer_seed ^ ((uint64_t)(i + 1) * 0xd1342543de82ef95ull);
    if(_pick_layer_kernel(&bank[i], rt, kernel_seed) != 0)
    {
      for(int k = 0; k < i; k++) _free_crystal_kernel(&bank[k].footprint);
      return 1;
    }
  }

  return 0;
}

/**
 * @brief Release all crystal footprints from one layer bank.
 */
__DT_CLONE_TARGETS__
static void _free_layer_kernel_bank(dt_iop_crystgrain_layer_kernel_t *const bank)
{
  for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++) _free_crystal_kernel(&bank[i].footprint);
}

/**
 * @brief Deplete a flat light field with a continuous germ mass.
 *
 * @details Mean-field integration of the crystal stack over one layer. One
 * pixel receives, from every germ whose footprint overlaps it, a share `alpha`
 * of a flat tone `min(r, cap)`, where `cap` is the per-crystal capture ceiling
 * and `r` the light still available. Summing those shares over the germ
 * process, the total weight reaching a pixel is `intensity * area`, so the
 * depletion obeys `dr/dt = -min(r, cap)` integrated over `t` in `[0, mass]`,
 * `t` counting accumulated coverage weight rather than crystals.
 *
 * That ODE is piecewise-closed: `r` first falls linearly at rate `cap` while
 * it is above the ceiling, then decays exponentially once the ceiling stops
 * binding. Integrating instead of taking the first-order product is what keeps
 * the prediction usable when a single pixel is swept by many sub-pixel
 * crystals — the first-order form silently over-predicts there, and it is the
 * exposure normalization that would drift with resolution.
 */
static inline float _flat_field_capture(const float remaining, const float cap, const float mass)
{
  if(!(cap > 0.0f) || !(mass > 0.0f) || !(remaining > 0.0f)) return 0.0f;

  float light = remaining;
  float weight = mass;

  if(light > cap)
  {
    const float linear_weight = (light - cap) / cap;
    if(weight <= linear_weight) return cap * weight;
    light = cap;
    weight -= linear_weight;
  }

  return remaining - light * expf(-weight);
}

/**
 * @brief Deplete one pixel with `count` coincident crystals.
 *
 * @details Exact closed form of the recurrence `r <- r - alpha * min(r, cap)`
 * iterated `count` times, which is what the simulation would do if it looped
 * over each of the crystals a sub-pixel germ process dropped on that single
 * pixel. Same two regimes as the continuous version above, but counted in
 * whole crystals: `steps` iterations while the ceiling binds, then a geometric
 * decay by `(1 - alpha)` per crystal.
 *
 * Evaluating it in closed form rather than looping is what bounds the cost:
 * the crystal count per pixel grows as the square of the downscaling factor,
 * so a thumbnail would otherwise pay the full-resolution grain count on a
 * fraction of the pixels.
 */
static inline float _coincident_capture(const float remaining, const float cap, const float alpha, const int count)
{
  if(count <= 0 || !(cap > 0.0f) || !(alpha > 0.0f) || !(remaining > 0.0f)) return 0.0f;

  float light = remaining;
  int left = count;

  if(light > cap)
  {
    const float exact_steps = (light - cap) / (alpha * cap);
    const int steps = (exact_steps >= (float)left) ? left : (int)ceilf(exact_steps);
    light -= (float)steps * alpha * cap;
    left -= steps;
  }

  if(left > 0) light *= powf(1.0f - fminf(alpha, 1.0f), (float)left);

  return fmaxf(remaining - light, 0.0f);
}

/**
 * @brief Predict the mean captured energy of one flat-field layer.
 *
 * @details The output normalization only needs the average exposure loss of
 * the stochastic crystal stack, so we run the mean-field depletion above on a
 * unit flat field, once per bank entry, and average the results.
 *
 * The germ mass one pixel receives from an entry is `intensity * area`, which
 * by construction equals `-ln(1 - filling)` for every entry — the invariant
 * that makes the model scale-free. The per-crystal ceiling `area * layer_scale`
 * is what still differentiates the entries, since the layer sensitivity is
 * expressed per unit of grain surface.
 */
__DT_CLONE_TARGETS__
static float _predict_layer_capture(const dt_iop_crystgrain_layer_kernel_t *const bank, const float layer_scale,
                                    const float remaining_fraction)
{
  double capture = 0.0;

  for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++)
  {
    const float area = bank[i].footprint.area;
    capture += _flat_field_capture(remaining_fraction, area * layer_scale, bank[i].intensity * area);
  }

  return MAX((float)(capture / DT_CRYSTGRAIN_LAYER_KERNELS), 0.0f);
}

/**
 * @brief Predict the exposure compensation of one monochrome grain stack.
 *
 * @details We reuse the exact layer bank sampled for the synthesis and update
 * a flat-field remaining-light fraction alongside the real image simulation.
 * If `r_l` is the remaining light fraction before layer `l`, the recurrence is
 *
 * `r_(l+1) = max(r_l - mean_i(E_i(r_l)), 0)`
 *
 * with `r_0 = 1`. The synthesized stack therefore transmits on average
 * `1 - r_L`, so the final global exposure correction is simply
 *
 * `exposure = 1 / (1 - r_L)`.
 *
 * This keeps the output normalization tied to the current grain size, filling
 * ratio and layer sensitivity without measuring any image averages.
 */
static inline float _predict_stack_exposure(const float remaining_fraction)
{
  const float transmitted = 1.0f - remaining_fraction;
  return (transmitted > FLT_EPSILON) ? 1.0f / transmitted : 1.0f;
}

static inline size_t _rgb_index(const size_t pixel, const int channel)
{
  return 4 * pixel + channel;
}

/**
 * @brief Simulate one monochrome grain field from one scalar image.
 *
 * @details We loop over seed candidates, looking for pixels that still have
 * photons left to capture on the current layer. Each seed first picks one
 * crystal footprint from the precomputed layer bank, then averages the local
 * layer energy over that footprint so one whole crystal prints one uniform
 * tone. The crystal is finally grown over that footprint while capping the
 * accumulated capture by the local layer capacity so the growth stays
 * energy-conserving. Most pixels live away from image borders, so we keep a
 * fast path there with direct indexing and only fall back to reflected
 * coordinates near the edges.
 */
__DT_CLONE_TARGETS__
static int _simulate_channel(const dt_iop_crystgrain_runtime_t *const rt, const float *const image, float *const result,
                             float *const remaining, float *const exposure)
{
  const int width = rt->width;
  const int height = rt->height;
  const size_t npixels = (size_t)width * height;
  float predicted_remaining = 1.0f;
  memset(result, 0, sizeof(float) * npixels);
  memcpy(remaining, image, sizeof(float) * npixels);

  for(int layer = 0; layer < rt->layers; layer++)
  {
    dt_iop_crystgrain_layer_kernel_t kernel_bank[DT_CRYSTGRAIN_LAYER_KERNELS];
    const uint64_t layer_seed = rt->base_seed + layer * 4099u;
    if(_build_layer_kernel_bank(kernel_bank, rt, layer_seed) != 0) return 1;
    predicted_remaining = fmaxf(predicted_remaining
                                - _predict_layer_capture(kernel_bank, rt->layer_scale, predicted_remaining),
                                0.0f);

    for(int y = 0; y < rt->height; y++)
    {
      const int world_y = (int)((rt->roi_y + y) * rt->inv_scale);
      for(int x = 0; x < rt->width; x++)
      {
        const size_t index = (size_t)y * rt->width + x;
        if(remaining[index] <= 0.0f) continue;

        const int world_x = (int)((rt->roi_x + x) * rt->inv_scale);
        const uint64_t pixel_seed = rt->base_seed
                                    ^ ((uint64_t)(uint32_t)world_x << 32)
                                    ^ (uint32_t)world_y
                                    ^ (uint64_t)(layer + 1) * 0x9e3779b97f4a7c15ull;
        const int kernel_index = splitmix32(pixel_seed ^ 0x94d049bb133111ebull) & (DT_CRYSTGRAIN_LAYER_KERNELS - 1);
        const dt_iop_crystgrain_layer_kernel_t *const entry = &kernel_bank[kernel_index];
        const dt_iop_crystgrain_kernel_t *const kernel = &entry->footprint;
        const int radius = kernel->radius;
        const int interior = (y >= radius && y < rt->height - radius && x >= radius && x < rt->width - radius);

        // How many crystals the germ process actually dropped on this pixel.
        // At and near 100% a crystal is wider than a pixel, the intensity is
        // well below one and this behaves like the old Bernoulli draw. Zoomed
        // out, several sub-pixel crystals share the same pixel, and sampling
        // that count — instead of truncating it at one — is precisely what
        // makes the grain average out with resolution the way a real emulsion
        // does when it is scanned smaller.
        const int crystals = _poisson_random(pixel_seed ^ 0xda942042e4dd58b5ull, entry->intensity);
        if(crystals <= 0) continue;

        if(kernel->count == 1 && kernel->dx[0] == 0 && kernel->dy[0] == 0)
        {
          // Sub-pixel regime: the whole crystal fits in its own pixel, so the
          // coincident crystals only ever deplete that one pixel and the
          // recurrence has a closed form.
          const float alpha = kernel->alpha[0];
          const float cap = image[index] * alpha * rt->layer_scale;
          const float deposited = _coincident_capture(remaining[index], cap, alpha, crystals);
          if(deposited <= 0.0f) continue;

          result[index] += deposited;
          remaining[index] = fmaxf(remaining[index] - deposited, 0.0f);
          continue;
        }

        // Each pixel sweeps only its own crystal footprint to print one flat
        // tone into the reconstruction while depleting the remaining light
        // field in place. Coincident crystals are printed one after the other,
        // each seeing what the previous ones left behind.
        for(int crystal = 0; crystal < crystals; crystal++)
        {
          float seed_energy = 0.0f;
          float original_energy = 0.0f;

          for(int tap = 0; tap < kernel->count; tap++)
          {
            int xx = x + kernel->dx[tap];
            int yy = y + kernel->dy[tap];
            if(!interior)
            {
              xx = _reflect_index(xx, width);
              yy = _reflect_index(yy, height);
            }

            const size_t dst = (size_t)yy * width + xx;
            // A crystal prints one flat tone from the average of the current
            // light field and of the immutable input over the whole grain
            // surface, so no detail finer than the grain survives inside it.
            seed_energy += remaining[dst] * kernel->alpha[tap];
            original_energy += image[dst] * kernel->alpha[tap];
          }
          seed_energy /= kernel->area;
          // The user layer scale applies to the whole grain surface, so the
          // per-pixel flat tone cap must scale with the grain area too.
          original_energy *= rt->layer_scale;
          seed_energy = fminf(seed_energy, original_energy);
          if(seed_energy <= 0.0f) break;

          for(int tap = 0; tap < kernel->count; tap++)
          {
            int xx = x + kernel->dx[tap];
            int yy = y + kernel->dy[tap];
            if(!interior)
            {
              xx = _reflect_index(xx, width);
              yy = _reflect_index(yy, height);
            }

            const size_t dst = (size_t)yy * width + xx;
            // Write the flat crystal tone back to the output and subtract the
            // same quantity from the light field that will feed deeper layers.
            const float deposited = seed_energy * kernel->alpha[tap];
            result[dst] += deposited;
            remaining[dst] = fmaxf(remaining[dst] - deposited, 0.0f);
          }
        }
      }
    }

    _free_layer_kernel_bank(kernel_bank);
  }

  *exposure = _predict_stack_exposure(predicted_remaining);
  return 0;
}

/**
 * @brief Simulate one color grain stack with shared crystal geometry.
 *
 * @details Real color film is not achromatic either: it stacks blue-, green-
 * and red-sensitive monochrome emulsions in depth, each with its own crystal
 * population. This routine therefore keeps one sequential remaining-light
 * model, but assigns each layer to one spectral sub-stack in blue/green/red
 * order. That keeps the physical "light goes through upper layers first"
 * behavior while avoiding the over-correlated all-channels-at-once look.
 */
__DT_CLONE_TARGETS__
static int _simulate_color(const dt_iop_crystgrain_runtime_t *const rt,
                           const dt_iop_crystgrain_color_state_t *const state,
                           float *const exposure)
{
  const int width = rt->width;
  const int height = rt->height;
  const size_t npixels = (size_t)width * height;
  const int blue_layers = (rt->layers + 2) / 3;
  const int green_layers = (rt->layers + 1) / 3;
  float predicted_remaining[3] = { 1.0f, 1.0f, 1.0f };
  const uint64_t channel_salt[3] = {
    0xa24baed4963ee407ull,
    0x9fb21c651e98df25ull,
    0xc13fa9a902a6328full
  };

  memset(state->result, 0, sizeof(float) * npixels * 4);
  memcpy(state->remaining, state->image, sizeof(float) * npixels * 4);

  for(int layer = 0; layer < rt->layers; layer++)
  {
    dt_iop_crystgrain_layer_kernel_t kernel_bank[DT_CRYSTGRAIN_LAYER_KERNELS];
    const int c = (layer < blue_layers) ? 2 : ((layer < blue_layers + green_layers) ? 1 : 0);
    const int sublayer = (c == 2) ? layer : ((c == 1) ? layer - blue_layers : layer - blue_layers - green_layers);
    const uint64_t layer_seed = rt->base_seed + (uint64_t)(sublayer + 1) * 4099u;
    if(_build_layer_kernel_bank(kernel_bank, rt, layer_seed) != 0) return 1;
    predicted_remaining[c] = fmaxf(predicted_remaining[c]
                                   - _predict_layer_capture(kernel_bank, rt->layer_scale, predicted_remaining[c]),
                                   0.0f);

    for(int y = 0; y < height; y++)
    {
      const int world_y = (int)((rt->roi_y + y) * rt->inv_scale);
      for(int x = 0; x < width; x++)
      {
        const size_t index = (size_t)y * width + x;
        const float remaining_total = state->remaining[_rgb_index(index, 0)]
                                      + state->remaining[_rgb_index(index, 1)]
                                      + state->remaining[_rgb_index(index, 2)];
        if(remaining_total <= 0.0f) continue;

        const int world_x = (int)((rt->roi_x + x) * rt->inv_scale);
        const uint64_t shared_seed = rt->base_seed
                                     ^ ((uint64_t)(uint32_t)world_x << 32)
                                     ^ (uint32_t)world_y
                                     ^ (uint64_t)(sublayer + 1) * 0x9e3779b97f4a7c15ull;
        const uint64_t channel_seed = shared_seed ^ channel_salt[c];
        const int use_shared = _uniform_random(channel_seed ^ 0x4f1bbcdc6762f96bull) < rt->channel_correlation;
        const uint64_t pixel_seed = use_shared ? shared_seed : channel_seed;
        const int kernel_index = splitmix32(pixel_seed ^ 0x94d049bb133111ebull) & (DT_CRYSTGRAIN_LAYER_KERNELS - 1);
        const dt_iop_crystgrain_layer_kernel_t *const entry = &kernel_bank[kernel_index];
        const dt_iop_crystgrain_kernel_t *const kernel = &entry->footprint;
        const int radius = kernel->radius;
        const int interior = (y >= radius && y < height - radius && x >= radius && x < width - radius);

        // Same germ count as the monochrome path, drawn per spectral sub-stack.
        const int crystals = _poisson_random(pixel_seed ^ 0xda942042e4dd58b5ull, entry->intensity);
        if(crystals <= 0) continue;

        if(kernel->count == 1 && kernel->dx[0] == 0 && kernel->dy[0] == 0)
        {
          const float alpha = kernel->alpha[0];
          const float cap = state->image[_rgb_index(index, c)] * alpha * rt->layer_scale;
          const float deposited = _coincident_capture(state->remaining[_rgb_index(index, c)], cap, alpha, crystals);
          if(deposited <= 0.0f) continue;

          state->result[_rgb_index(index, c)] += deposited;
          state->remaining[_rgb_index(index, c)]
            = fmaxf(state->remaining[_rgb_index(index, c)] - deposited, 0.0f);
          continue;
        }

        for(int crystal = 0; crystal < crystals; crystal++)
        {
          float seed_energy = 0.0f;
          float original_energy = 0.0f;

          for(int tap = 0; tap < kernel->count; tap++)
          {
            int xx = x + kernel->dx[tap];
            int yy = y + kernel->dy[tap];
            if(!interior)
            {
              xx = _reflect_index(xx, width);
              yy = _reflect_index(yy, height);
            }

            const size_t dst = (size_t)yy * width + xx;
            // Each depth layer belongs to one spectral emulsion only, so it
            // prints one flat tone from that channel and leaves the others to
            // deeper layers.
            seed_energy += state->remaining[_rgb_index(dst, c)] * kernel->alpha[tap];
            original_energy += state->image[_rgb_index(dst, c)] * kernel->alpha[tap];
          }

          seed_energy /= kernel->area;
          original_energy *= rt->layer_scale;
          const float captured = fminf(seed_energy, original_energy);
          if(captured <= 0.0f) break;

          for(int tap = 0; tap < kernel->count; tap++)
          {
            int xx = x + kernel->dx[tap];
            int yy = y + kernel->dy[tap];
            if(!interior)
            {
              xx = _reflect_index(xx, width);
              yy = _reflect_index(yy, height);
            }

            const size_t dst = (size_t)yy * width + xx;
            const float deposited = captured * kernel->alpha[tap];
            state->result[_rgb_index(dst, c)] += deposited;
            state->remaining[_rgb_index(dst, c)]
              = fmaxf(state->remaining[_rgb_index(dst, c)] - deposited, 0.0f);
          }
        }
      }
    }

    _free_layer_kernel_bank(kernel_bank);
  }

  for(int c = 0; c < 3; c++) exposure[c] = _predict_stack_exposure(predicted_remaining[c]);
  return 0;
}

/**
 * @brief Extract a luminance image from the RGB input buffer.
 *
 * @details We loop over rows so each OpenMP worker owns whole scanlines and
 * writes to disjoint cache lines in the destination buffer.
 */
__DT_CLONE_TARGETS__
static void _extract_luminance_kernel(const float *const restrict in, float *const restrict image,
                                      const int width, const int height,
                                      const dt_iop_order_iccprofile_info_t *const work_profile)
{
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
  {
    const size_t row = (size_t)y * width;
    for(int x = 0; x < width; x++)
    {
      const size_t k = row + x;
      const float luminance = (work_profile)
        ? dt_ioppr_get_rgb_matrix_luminance(in + 4 * k, work_profile->matrix_in, work_profile->lut_in,
                                            work_profile->unbounded_coeffs_in, work_profile->lutsize,
                                            work_profile->nonlinearlut)
        : dt_camera_rgb_luminance(in + 4 * k);

      image[k] = fmaxf(luminance, 0.0f);
    }
  }
}

/**
 * @brief Extract the three RGB light channels as scalar images.
 *
 * @details The grain model works on per-channel light fields, so we extract
 * them together in one pass to keep the input image hot in cache and avoid
 * three independent full-frame reads before color synthesis starts.
 */
__DT_CLONE_TARGETS__
static void _extract_rgb_kernels(const float *const restrict in, float *const restrict image,
                                 const int width, const int height)
{
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
  {
    const size_t row = (size_t)y * width;
    for(int x = 0; x < width; x++)
    {
      const size_t k = row + x;
      const float red = fmaxf(in[4 * k + 0], 0.0f);
      const float green = fmaxf(in[4 * k + 1], 0.0f);
      const float blue = fmaxf(in[4 * k + 2], 0.0f);

      image[_rgb_index(k, 0)] = red;
      image[_rgb_index(k, 1)] = green;
      image[_rgb_index(k, 2)] = blue;
      image[_rgb_index(k, 3)] = 0.0f;
    }
  }
}

/**
 * @brief Apply one monochrome grain field back onto the RGB image.
 *
 * @details We loop over rows, looking for the ratio between the original and
 * grainy luminance, then rescale RGB together so hue stays unchanged. The
 * fully synthesized grain field is applied directly after exposure
 * normalization, with no extra transparency stage. Since the module now lives
 * before filmic RGB, we preserve scene-referred highlights and only clamp
 * negative values away.
 */
__DT_CLONE_TARGETS__
static void _apply_mono_grain_kernel(const float *const restrict in, float *const restrict out,
                                     const float *const restrict image, const float *const restrict result,
                                     const int width, const int height, const float exposure)
{
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
  {
    const size_t row = (size_t)y * width;
    for(int x = 0; x < width; x++)
    {
      const size_t k = row + x;
      const float grainy = fmaxf(result[k] * exposure, 0.0f);
      const float ratio = (image[k] > 1e-6f) ? grainy / image[k] : 0.0f;

      out[4 * k + 0] = fmaxf(in[4 * k + 0] * ratio, 0.0f);
      out[4 * k + 1] = fmaxf(in[4 * k + 1] * ratio, 0.0f);
      out[4 * k + 2] = fmaxf(in[4 * k + 2] * ratio, 0.0f);
    }
  }
}

/**
 * @brief Finalize the three color grain channels in one pass.
 *
 * @details The color path only needs one final RGB pass once synthesis is
 * done. We therefore restore each channel exposure, extract the RGB grain
 * residual around the original image, mute only its chromatic excursion, and
 * write the final RGBA output without staging intermediate normalized buffers.
 */
__DT_CLONE_TARGETS__
static void _finalize_color_grain_kernel(const float *const restrict in, float *const restrict out,
                                         const float *const restrict image, const float *const restrict result,
                                         const int width, const int height, const float exposure_r,
                                         const float exposure_g, const float exposure_b, const float colorfulness)
{
  __OMP_PARALLEL_FOR__()
  for(int y = 0; y < height; y++)
  {
    const size_t row = (size_t)y * width;
    for(int x = 0; x < width; x++)
    {
      const size_t k = row + x;
      const float image_r = image[_rgb_index(k, 0)];
      const float image_g = image[_rgb_index(k, 1)];
      const float image_b = image[_rgb_index(k, 2)];
      const float grain_r = (exposure_r > 0.0f) ? fmaxf(result[_rgb_index(k, 0)] * exposure_r, 0.0f) : image_r;
      const float grain_g = (exposure_g > 0.0f) ? fmaxf(result[_rgb_index(k, 1)] * exposure_g, 0.0f) : image_g;
      const float grain_b = (exposure_b > 0.0f) ? fmaxf(result[_rgb_index(k, 2)] * exposure_b, 0.0f) : image_b;
      const float residual_r = grain_r - image_r;
      const float residual_g = grain_g - image_g;
      const float residual_b = grain_b - image_b;
      const float mean = (residual_r + residual_g + residual_b) / 3.0f;

      out[4 * k + 0] = in[4 * k + 0] + mean + (residual_r - mean) * colorfulness;
      out[4 * k + 1] = in[4 * k + 1] + mean + (residual_g - mean) * colorfulness;
      out[4 * k + 2] = in[4 * k + 2] + mean + (residual_b - mean) * colorfulness;
    }
  }
}

#ifdef HAVE_OPENCL
#define DT_CRYSTGRAIN_CL_PROGRAM 36
#define DT_CRYSTGRAIN_REDUCESIZE 64

/**
 * @brief Upload one layer bank to the device.
 *
 * @details The device used to re-derive every tap's coverage analytically from
 * `(radius, vertices, rotation)`, twice per crystal and once per tap. It now
 * reads the very same weights the CPU path rasterizes, uploaded as one dense
 * `(2 * radius + 1)^2` map per bank entry: cheaper in the inner loop, and it
 * removes a standing CPU/GPU divergence — the two coverage functions had
 * already drifted apart, the device one short-circuiting any crystal with 5
 * vertices or more to a disc.
 *
 * `params` carries what the kernel still needs per entry: the germ intensity,
 * the crystal surface, the support radius (0 for a sub-pixel crystal, which is
 * the fast path) and the offset of its weight map.
 */
static cl_int _upload_layer_bank(const int devid, const dt_iop_crystgrain_layer_kernel_t *const bank,
                                 cl_mem *const dev_params, cl_mem *const dev_alpha)
{
  float params[DT_CRYSTGRAIN_LAYER_KERNELS][4];
  size_t total_taps = 0;

  *dev_params = NULL;
  *dev_alpha = NULL;

  for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++)
  {
    const int radius = bank[i].footprint.radius;
    const size_t stride = (size_t)(2 * radius + 1);
    params[i][0] = bank[i].intensity;
    params[i][1] = bank[i].footprint.area;
    params[i][2] = (float)radius;
    params[i][3] = (float)total_taps;
    total_taps += stride * stride;
  }

  float *const alpha = calloc(total_taps, sizeof(float));
  if(IS_NULL_PTR(alpha)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;

  for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++)
  {
    const dt_iop_crystgrain_kernel_t *const footprint = &bank[i].footprint;
    const int radius = footprint->radius;
    const size_t stride = (size_t)(2 * radius + 1);
    const size_t offset = (size_t)params[i][3];

    for(int tap = 0; tap < footprint->count; tap++)
      alpha[offset + (size_t)(footprint->dy[tap] + radius) * stride + (size_t)(footprint->dx[tap] + radius)]
        = footprint->alpha[tap];
  }

  *dev_params = dt_opencl_copy_host_to_device_constant(devid, sizeof(params), params);
  *dev_alpha = dt_opencl_alloc_device_buffer(devid, sizeof(float) * total_taps);
  cl_int err = (IS_NULL_PTR(*dev_params) || IS_NULL_PTR(*dev_alpha)) ? CL_MEM_OBJECT_ALLOCATION_FAILURE : CL_SUCCESS;

  if(err == CL_SUCCESS)
    err = dt_opencl_write_buffer_to_device(devid, alpha, *dev_alpha, 0, sizeof(float) * total_taps, TRUE);

  free(alpha);

  if(err != CL_SUCCESS)
  {
    dt_opencl_release_mem_object(*dev_params);
    dt_opencl_release_mem_object(*dev_alpha);
    *dev_params = NULL;
    *dev_alpha = NULL;
  }

  return err;
}

/**
 * @brief Simulate one grain field entirely on the OpenCL device.
 *
 * @details The host precomputes a small bank of crystal geometries for the
 * current layer and uploads only their analytic parameters. Each layer is then
 * dispatched once on the GPU: every pixel decides whether it spawns a crystal,
 * averages the input and current light field over that footprint, then updates
 * the output and remaining light in place through atomic writes.
 */
static int _simulate_channel_cl(const int devid, dt_iop_crystgrain_global_data_t *const gd,
                                const dt_iop_crystgrain_runtime_t *const rt, cl_mem dev_image, cl_mem dev_result,
                                cl_mem dev_remaining, float *const exposure)
{
  cl_int err = CL_SUCCESS;
  const int width = rt->width;
  const int height = rt->height;
  size_t sizes[3] = { ROUNDUP((size_t)width, (size_t)16), ROUNDUP((size_t)height, (size_t)16), 1 };
  const size_t buffer_size = sizeof(float) * (size_t)width * height;
  float predicted_remaining = 1.0f;

  dt_opencl_set_kernel_arg(devid, gd->kernel_zero_scalar, 0, sizeof(cl_mem), &dev_result);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zero_scalar, 1, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zero_scalar, 2, sizeof(int), &height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zero_scalar, sizes);
  if(err != CL_SUCCESS) return err;

  err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, dev_image, dev_remaining, 0, 0, buffer_size);
  if(err != CL_SUCCESS) return err;

  for(int layer = 0; layer < rt->layers; layer++)
  {
    dt_iop_crystgrain_layer_kernel_t kernel_bank[DT_CRYSTGRAIN_LAYER_KERNELS];
    cl_mem dev_kernel_bank = NULL;
    cl_mem dev_kernel_alpha = NULL;
    const float layer_scale = rt->layer_scale;
    const int roi_x = rt->roi_x;
    const int roi_y = rt->roi_y;
    const float inv_scale = rt->inv_scale;
    const cl_ulong base_seed = (cl_ulong)rt->base_seed;

    if(_build_layer_kernel_bank(kernel_bank, rt, rt->base_seed + layer * 4099u) != 0)
    {
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      return err;
    }
    predicted_remaining = fmaxf(predicted_remaining
                                - _predict_layer_capture(kernel_bank, rt->layer_scale, predicted_remaining),
                                0.0f);

    err = _upload_layer_bank(devid, kernel_bank, &dev_kernel_bank, &dev_kernel_alpha);
    _free_layer_kernel_bank(kernel_bank);
    if(err != CL_SUCCESS) return err;

    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 0, sizeof(cl_mem), &dev_image);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 1, sizeof(cl_mem), &dev_remaining);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 2, sizeof(cl_mem), &dev_result);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 3, sizeof(cl_mem), &dev_kernel_bank);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 4, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 5, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 6, sizeof(int), &roi_x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 7, sizeof(int), &roi_y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 8, sizeof(float), &inv_scale);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 9, sizeof(cl_ulong), &base_seed);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 10, sizeof(int), &layer);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 11, sizeof(float), &layer_scale);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer, 12, sizeof(cl_mem), &dev_kernel_alpha);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_simulate_layer, sizes);
    dt_opencl_release_mem_object(dev_kernel_bank);
    dt_opencl_release_mem_object(dev_kernel_alpha);
    if(err != CL_SUCCESS) return err;
  }

  *exposure = _predict_stack_exposure(predicted_remaining);
  return err;
}

void init_global(dt_iop_module_so_t *module)
{
  dt_iop_crystgrain_global_data_t *gd = (dt_iop_crystgrain_global_data_t *)malloc(sizeof(dt_iop_crystgrain_global_data_t));
  module->data = gd;
  const int program = DT_CRYSTGRAIN_CL_PROGRAM;
  gd->kernel_zero_scalar = dt_opencl_create_kernel(program, "crystgrain_zero_scalar");
  gd->kernel_zero_rgb = dt_opencl_create_kernel(program, "crystgrain_zero_rgb");
  gd->kernel_extract_luminance = dt_opencl_create_kernel(program, "crystgrain_extract_luminance");
  gd->kernel_extract_rgb = dt_opencl_create_kernel(program, "crystgrain_extract_rgb");
  gd->kernel_simulate_layer = dt_opencl_create_kernel(program, "crystgrain_simulate_layer");
  gd->kernel_simulate_layer_color = dt_opencl_create_kernel(program, "crystgrain_simulate_layer_color");
  gd->kernel_apply_mono = dt_opencl_create_kernel(program, "crystgrain_apply_mono");
  gd->kernel_finalize_color = dt_opencl_create_kernel(program, "crystgrain_finalize_color");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_crystgrain_global_data_t *gd = (dt_iop_crystgrain_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_zero_scalar);
  dt_opencl_free_kernel(gd->kernel_zero_rgb);
  dt_opencl_free_kernel(gd->kernel_extract_luminance);
  dt_opencl_free_kernel(gd->kernel_extract_rgb);
  dt_opencl_free_kernel(gd->kernel_simulate_layer);
  dt_opencl_free_kernel(gd->kernel_simulate_layer_color);
  dt_opencl_free_kernel(gd->kernel_apply_mono);
  dt_opencl_free_kernel(gd->kernel_finalize_color);
  free(module->data);
  module->data = NULL;
}

int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_crystgrain_data_t *const d = (const dt_iop_crystgrain_data_t *)piece->data;
  dt_iop_crystgrain_global_data_t *gd = (dt_iop_crystgrain_global_data_t *)self->global_data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  const int devid = pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;
  // Grain size is authored in full-resolution output pixels at 100% zoom.
  // The current processing grid may already be downsampled twice:
  // 1. by the ROI zoom factor used for the current preview/export,
  // 2. by the mipmap level chosen before the pipe even starts.
  // Clamped at 1 so zooming past the native scale never invents larger
  // crystals than the ones the slider describes.
  const float kernel_scale = CLAMPS(1.0f / dt_dev_get_module_scale(pipe, roi_in), 1e-6f, 1.0f);
  cl_int err = CL_SUCCESS;
  float exposure[3] = { 1.0f, 1.0f, 1.0f };

  if(width <= 0 || height <= 0 || d->layers <= 0 || d->filling <= 0.0f)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { (size_t)width, (size_t)height, 1 };
    return dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, region);
  }

  cl_mem dev_image = NULL;
  cl_mem dev_result = NULL;
  cl_mem dev_remaining = NULL;
  cl_mem dev_image_rgb = NULL;
  cl_mem dev_result_rgb = NULL;
  cl_mem dev_remaining_rgb = NULL;
  dt_colorspaces_iccprofile_info_cl_t *profile_info_cl = NULL;
  cl_float *profile_lut_cl = NULL;
  cl_mem dev_profile_info = NULL;
  cl_mem dev_profile_lut = NULL;

  dev_image = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)width * height);
  dev_result = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)width * height);
  dev_remaining = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)width * height);
  if(IS_NULL_PTR(dev_image) || IS_NULL_PTR(dev_result) || IS_NULL_PTR(dev_remaining))
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  dt_iop_crystgrain_runtime_t rt = {
    .width = width,
    .height = height,
    .roi_x = roi_out->x,
    .roi_y = roi_out->y,
    .layers = d->layers,
    .layer_scale = 0.0f,
    .filling = d->filling,
    .grain_size = d->grain_size,
    .size_stddev = d->size_stddev,
    .kernel_scale = kernel_scale,
    .inv_scale = 1.0f / kernel_scale,
    .channel_correlation = d->channel_correlation,
    // The seed is keyed on the image alone, deliberately not on the buffer
    // size: crystal sizes are drawn in full-resolution pixels and seeds are
    // addressed by full-resolution position, so keeping the seed free of the
    // grid dimensions is what lets the same crystals land in the same places
    // whatever resolution the pipeline runs at.
    .base_seed = ((uint64_t)_hash_string(pipe->dev->image_storage.filename) << 32)
                 ^ 0x9e3779b97f4a7c15ull
  };
  const float current_surface = _average_discrete_grain_surface(&rt);
  // Neutral layer capture is defined as 1/layers of the input energy for a
  // grain of average rasterized surface. Since each sampled bank entry can
  // have a different discrete area A_i, the flat-field recurrence uses
  // min(r_l, A_i * layer_scale) per crystal, with the current_surface term
  // keeping the user-facing EV control centered on that neutral 1/layers
  // behaviour across preview scales.
  rt.layer_scale = d->layer_capture / MAX((float)d->layers, 1.0f) / MAX(current_surface, FLT_EPSILON);
  const int blue_layers = (rt.layers + 2) / 3;
  const int green_layers = (rt.layers + 1) / 3;

  size_t sizes[3] = { ROUNDUP((size_t)width, (size_t)16), ROUNDUP((size_t)height, (size_t)16), 1 };

  if(d->mode == DT_CRYSTGRAIN_MONO)
  {
    err = dt_ioppr_build_iccprofile_params_cl(work_profile, devid, &profile_info_cl, &profile_lut_cl,
                                              &dev_profile_info, &dev_profile_lut);
    if(err != CL_SUCCESS) goto error;

    const int use_work_profile = (!IS_NULL_PTR(work_profile)) ? 1 : 0;
    dt_opencl_set_kernel_arg(devid, gd->kernel_extract_luminance, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_extract_luminance, 1, sizeof(cl_mem), &dev_image);
    dt_opencl_set_kernel_arg(devid, gd->kernel_extract_luminance, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_extract_luminance, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_extract_luminance, 4, sizeof(cl_mem), &dev_profile_info);
    dt_opencl_set_kernel_arg(devid, gd->kernel_extract_luminance, 5, sizeof(cl_mem), &dev_profile_lut);
    dt_opencl_set_kernel_arg(devid, gd->kernel_extract_luminance, 6, sizeof(int), &use_work_profile);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_extract_luminance, sizes);
    if(err != CL_SUCCESS) goto error;

    err = _simulate_channel_cl(devid, gd, &rt, dev_image, dev_result, dev_remaining, &exposure[0]);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_set_kernel_arg(devid, gd->kernel_apply_mono, 0, sizeof(cl_mem), &dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_apply_mono, 1, sizeof(cl_mem), &dev_image);
    dt_opencl_set_kernel_arg(devid, gd->kernel_apply_mono, 2, sizeof(cl_mem), &dev_result);
    dt_opencl_set_kernel_arg(devid, gd->kernel_apply_mono, 3, sizeof(cl_mem), &dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_apply_mono, 4, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_apply_mono, 5, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_apply_mono, 6, sizeof(float), &exposure[0]);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_apply_mono, sizes);
    goto error;
  }

  dev_image_rgb = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)width * height * 4);
  dev_result_rgb = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)width * height * 4);
  dev_remaining_rgb = dt_opencl_alloc_device_buffer(devid, sizeof(float) * (size_t)width * height * 4);
  if(IS_NULL_PTR(dev_image_rgb) || IS_NULL_PTR(dev_result_rgb) || IS_NULL_PTR(dev_remaining_rgb))
  {
    err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
    goto error;
  }

  dt_opencl_set_kernel_arg(devid, gd->kernel_extract_rgb, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_extract_rgb, 1, sizeof(cl_mem), &dev_image_rgb);
  dt_opencl_set_kernel_arg(devid, gd->kernel_extract_rgb, 2, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_extract_rgb, 3, sizeof(int), &height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_extract_rgb, sizes);
  if(err != CL_SUCCESS) goto error;

  const size_t color_buffer_size = sizeof(float) * (size_t)width * height * 4;
  dt_opencl_set_kernel_arg(devid, gd->kernel_zero_rgb, 0, sizeof(cl_mem), &dev_result_rgb);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zero_rgb, 1, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_zero_rgb, 2, sizeof(int), &height);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_zero_rgb, sizes);
  if(err != CL_SUCCESS) goto error;

  err = dt_opencl_enqueue_copy_buffer_to_buffer(devid, dev_image_rgb, dev_remaining_rgb, 0, 0, color_buffer_size);
  if(err != CL_SUCCESS) goto error;

  float predicted_remaining[3] = { 1.0f, 1.0f, 1.0f };
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 0, sizeof(cl_mem), &dev_image_rgb);
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 1, sizeof(cl_mem), &dev_remaining_rgb);
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 2, sizeof(cl_mem), &dev_result_rgb);
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 3, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 4, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 5, sizeof(int), &rt.roi_x);
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 6, sizeof(int), &rt.roi_y);
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 7, sizeof(float), &rt.inv_scale);
  {
    const cl_ulong base_seed = (cl_ulong)rt.base_seed;
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 8, sizeof(cl_ulong), &base_seed);
  }
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 10, sizeof(float), &rt.layer_scale);
  dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 13, sizeof(float), &rt.channel_correlation);

  for(int layer = 0; layer < rt.layers; layer++)
  {
    dt_iop_crystgrain_layer_kernel_t kernel_bank[DT_CRYSTGRAIN_LAYER_KERNELS];
    cl_mem dev_kernel_bank = NULL;
    cl_mem dev_kernel_alpha = NULL;
    const int active_channel = (layer < blue_layers) ? 2 : ((layer < blue_layers + green_layers) ? 1 : 0);
    const int sublayer = (active_channel == 2)
      ? layer
      : ((active_channel == 1) ? layer - blue_layers : layer - blue_layers - green_layers);
    if(_build_layer_kernel_bank(kernel_bank, &rt, rt.base_seed + (uint64_t)(sublayer + 1) * 4099u) != 0)
    {
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto error;
    }
    predicted_remaining[active_channel]
      = fmaxf(predicted_remaining[active_channel]
              - _predict_layer_capture(kernel_bank, rt.layer_scale, predicted_remaining[active_channel]),
              0.0f);

    err = _upload_layer_bank(devid, kernel_bank, &dev_kernel_bank, &dev_kernel_alpha);
    _free_layer_kernel_bank(kernel_bank);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 9, sizeof(cl_mem), &dev_kernel_bank);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 11, sizeof(int), &sublayer);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 12, sizeof(int), &active_channel);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 14, sizeof(cl_mem), &dev_kernel_alpha);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_simulate_layer_color, sizes);
    dt_opencl_release_mem_object(dev_kernel_bank);
    dt_opencl_release_mem_object(dev_kernel_alpha);
    if(err != CL_SUCCESS) goto error;
  }

  for(int c = 0; c < 3; c++) exposure[c] = _predict_stack_exposure(predicted_remaining[c]);

  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 0, sizeof(cl_mem), &dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 1, sizeof(cl_mem), &dev_image_rgb);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 2, sizeof(cl_mem), &dev_result_rgb);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 3, sizeof(cl_mem), &dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 4, sizeof(int), &width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 5, sizeof(int), &height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 6, sizeof(float), &exposure[0]);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 7, sizeof(float), &exposure[1]);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 8, sizeof(float), &exposure[2]);
  dt_opencl_set_kernel_arg(devid, gd->kernel_finalize_color, 9, sizeof(float), &d->colorspace_saturation);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_finalize_color, sizes);

error:
  dt_opencl_release_mem_object(dev_image);
  dt_opencl_release_mem_object(dev_result);
  dt_opencl_release_mem_object(dev_remaining);
  dt_opencl_release_mem_object(dev_image_rgb);
  dt_opencl_release_mem_object(dev_result_rgb);
  dt_opencl_release_mem_object(dev_remaining_rgb);
  dt_ioppr_free_iccprofile_params_cl(&profile_info_cl, &profile_lut_cl, &dev_profile_info, &dev_profile_lut);
  return (err == CL_SUCCESS) ? TRUE : FALSE;
}
#endif

void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_crystgrain_params_t *p = (dt_iop_crystgrain_params_t *)p1;
  dt_iop_crystgrain_data_t *d = (dt_iop_crystgrain_data_t *)piece->data;

  d->mode = p->mode;
  d->filling = p->filling * 0.01f;
  d->grain_size = p->grain_size;
  d->layers = p->layers;
  d->size_stddev = p->size_stddev;
  d->layer_capture = exp2f(p->layer_capture);
  d->channel_correlation = p->channel_correlation * 0.01f;
  d->colorspace_saturation = p->colorspace_saturation * 0.01f;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_crystgrain_data_t));
  piece->data_size = sizeof(dt_iop_crystgrain_data_t);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

int process(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_crystgrain_data_t *const d = (const dt_iop_crystgrain_data_t *)piece->data;
  const dt_iop_order_iccprofile_info_t *const work_profile = dt_ioppr_get_pipe_work_profile_info(pipe);
  const float *const restrict in = (const float *const)ivoid;
  float *const restrict out = (float *const)ovoid;
  const int width = roi_out->width;
  const int height = roi_out->height;
  // Grain size is authored in full-resolution output pixels at 100% zoom, and
  // clamped there — see the OpenCL path for the details.
  const float kernel_scale = CLAMPS(1.0f / dt_dev_get_module_scale(pipe, roi_in), 1e-6f, 1.0f);

  if(width <= 0 || height <= 0 || d->layers <= 0 || d->filling <= 0.0f)
  {
    dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out, TRUE);
    return 0;
  }

  float *image = NULL;
  float *result = NULL;
  float *remaining = NULL;
  float *image_rgb = NULL;
  float *result_rgb = NULL;
  float *remaining_rgb = NULL;
  if(dt_iop_alloc_image_buffers(self, roi_in, roi_out,
                                1, &image,
                                1 | DT_IMGSZ_CLEARBUF, &result,
                                1, &remaining,
                                4, &image_rgb,
                                4 | DT_IMGSZ_CLEARBUF, &result_rgb,
                                4, &remaining_rgb,
                                0))
  {
    dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out, TRUE);
    return 1;
  }

  dt_iop_image_copy_by_size(out, in, width, height, 4);

  dt_iop_crystgrain_runtime_t rt = {
    .width = width,
    .height = height,
    .roi_x = roi_out->x,
    .roi_y = roi_out->y,
    .layers = d->layers,
    .layer_scale = 0.0f,
    .filling = d->filling,
    .grain_size = d->grain_size,
    .size_stddev = d->size_stddev,
    .kernel_scale = kernel_scale,
    .inv_scale = 1.0f / kernel_scale,
    .channel_correlation = d->channel_correlation,
    // The seed is keyed on the image alone, deliberately not on the buffer
    // size: crystal sizes are drawn in full-resolution pixels and seeds are
    // addressed by full-resolution position, so keeping the seed free of the
    // grid dimensions is what lets the same crystals land in the same places
    // whatever resolution the pipeline runs at.
    .base_seed = ((uint64_t)_hash_string(pipe->dev->image_storage.filename) << 32)
                 ^ 0x9e3779b97f4a7c15ull
  };
  const float current_surface = _average_discrete_grain_surface(&rt);
  // Neutral layer capture is defined as 1/layers of the input energy for a
  // grain of average rasterized surface. Since each sampled bank entry can
  // have a different discrete area A_i, the flat-field recurrence uses
  // min(r_l, A_i * layer_scale) per crystal, with the current_surface term
  // keeping the user-facing EV control centered on that neutral 1/layers
  // behaviour across preview scales.
  rt.layer_scale = d->layer_capture / MAX((float)d->layers, 1.0f) / MAX(current_surface, FLT_EPSILON);

  if(d->mode == DT_CRYSTGRAIN_MONO)
  {
    _extract_luminance_kernel(in, image, width, height, work_profile);
    float mono_exposure = 1.0f;

    if(_simulate_channel(&rt, image, result, remaining, &mono_exposure) != 0)
    {
      dt_pixelpipe_cache_free_align(image);
      dt_pixelpipe_cache_free_align(result);
      dt_pixelpipe_cache_free_align(remaining);
      dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out, TRUE);
      return 1;
    }

    _apply_mono_grain_kernel(in, out, image, result, width, height, mono_exposure);
  }
  else
  {
    // Color film layers share one crystal geometry stack. Keep the working
    // light fields interleaved as RGB tuples so extraction, simulation and
    // final write-back all walk one contiguous color buffer instead of three
    // independent scalar plates.
    _extract_rgb_kernels(in, image_rgb, width, height);
    float color_exposure[3] = { 1.0f, 1.0f, 1.0f };
    const dt_iop_crystgrain_color_state_t color_state = {
      .image = image_rgb,
      .result = result_rgb,
      .remaining = remaining_rgb
    };

    if(_simulate_color(&rt, &color_state, color_exposure) != 0)
    {
      dt_pixelpipe_cache_free_align(image);
      dt_pixelpipe_cache_free_align(result);
      dt_pixelpipe_cache_free_align(remaining);
      dt_pixelpipe_cache_free_align(image_rgb);
      dt_pixelpipe_cache_free_align(result_rgb);
      dt_pixelpipe_cache_free_align(remaining_rgb);
      dt_iop_copy_image_roi(out, in, 4, roi_in, roi_out, TRUE);
      return 1;
    }

    _finalize_color_grain_kernel(in, out, image_rgb, result_rgb, width, height,
                                 color_exposure[0], color_exposure[1], color_exposure[2],
                                 d->colorspace_saturation);
  }

  dt_pixelpipe_cache_free_align(image);
  dt_pixelpipe_cache_free_align(result);
  dt_pixelpipe_cache_free_align(remaining);
  dt_pixelpipe_cache_free_align(image_rgb);
  dt_pixelpipe_cache_free_align(result_rgb);
  dt_pixelpipe_cache_free_align(remaining_rgb);
  return 0;
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_crystgrain_gui_data_t *g = (dt_iop_crystgrain_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_crystgrain_params_t *p = (dt_iop_crystgrain_params_t *)self->params;
  const gboolean is_color = (p->mode == DT_CRYSTGRAIN_COLOR);

  gtk_widget_set_visible(g->channel_correlation, is_color);
  gtk_widget_set_visible(g->colorspace_saturation, is_color);
}

static void _mode_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  gui_update(self);
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_crystgrain_gui_data_t *g = IOP_GUI_ALLOC(crystgrain);

  g->mode = dt_bauhaus_combobox_from_params(self, "mode");
  gtk_widget_set_tooltip_text(g->mode, _("simulate one shared B&W grain field or one shared blue/green/red-sensitive color grain stack"));
  g_signal_connect(G_OBJECT(g->mode), "value-changed", G_CALLBACK(_mode_changed), self);

  g->filling = dt_bauhaus_slider_from_params(self, "filling");
  dt_bauhaus_slider_set_format(g->filling, "%");
  gtk_widget_set_tooltip_text(g->filling, _("surface ratio occupied by silver-halide crystals in each layer"));

  g->grain_size = dt_bauhaus_slider_from_params(self, "grain_size");
  dt_bauhaus_slider_set_digits(g->grain_size, 0);
  dt_bauhaus_slider_set_format(g->grain_size, " px");
  gtk_widget_set_tooltip_text(g->grain_size, _("average crystal diameter, in full-resolution pixels. The same crystals are simulated at every preview and export resolution, so the rendered grain stays consistent across sizes"));

  g->layers = dt_bauhaus_slider_from_params(self, "layers");
  dt_bauhaus_slider_set_digits(g->layers, 0);
  gtk_widget_set_tooltip_text(g->layers, _("number of crystal layers stacked through the emulsion"));

  g->layer_capture = dt_bauhaus_slider_from_params(self, "layer_capture");
  dt_bauhaus_slider_set_soft_range(g->layer_capture, -2.0f, 2.0f);
  dt_bauhaus_slider_set_format(g->layer_capture, _(" EV"));
  gtk_widget_set_tooltip_text(g->layer_capture, _("0 EV means one layer captures its neutral 1/layers share after normalization by the rasterized grain surface; positive values increase that capture and negative values decrease it"));

  g->channel_correlation = dt_bauhaus_slider_from_params(self, "channel_correlation");
  dt_bauhaus_slider_set_format(g->channel_correlation, "%");
  gtk_widget_set_tooltip_text(g->channel_correlation, _("probability that blue-, green- and red-sensitive sub-layers reuse the same crystal births and shapes at matching depths"));

  g->colorspace_saturation = dt_bauhaus_slider_from_params(self, "colorspace_saturation");
  dt_bauhaus_slider_set_format(g->colorspace_saturation, "%");
  gtk_widget_set_tooltip_text(g->colorspace_saturation, _("scale only the chromatic amplitude of the RGB grain residual while keeping its achromatic strength unchanged"));

  g->size_stddev = dt_bauhaus_slider_from_params(self, "size_stddev");
  gtk_widget_set_tooltip_text(g->size_stddev, _("log-normal standard deviation of crystal sizes"));

  gui_update(self);
}
