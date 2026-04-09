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
#endif

#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "common/iop_profile.h"
#include "common/math.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/noise_generator.h"
#include "gui/presets.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <float.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(9, dt_iop_crystgrain_params_t)

#define DT_CRYSTGRAIN_LAYER_KERNELS 16

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
  float probability;
  float vertices;
  float rotation;
  int width;
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
 * @brief Map the requested filling ratio to the Bernoulli probability used to
 * plant seeds.
 *
 * @details In the simplified Bernoulli model, one binary crystal of area `A`
 * covers a destination pixel if any of the `A` source positions that would
 * hit that pixel spawns a seed. Assuming independent seed events, the
 * uncovered probability is `(1 - p)^A`, so matching a requested filling ratio
 * `f` amounts to solving `1 - f = (1 - p)^A`, that is
 * `p = 1 - (1 - f)^(1 / A)`. This keeps the expected covered surface stable
 * for the actual discrete grain area at every preview scale.
 */
static inline float _seed_probability(const float filling, const float crystal_area)
{
  const float clamped_filling = CLAMPS(filling, 0.0f, 0.9999f);
  if(crystal_area <= 1.0f) return clamped_filling;
  return 1.0f - powf(1.0f - clamped_filling, 1.0f / crystal_area);
}

/**
 * @brief Estimate the partial coverage of one pixel by one crystal boundary.
 *
 * @details The continuous grain radius lives in floating-point, but the raster
 * simulation ultimately writes on whole pixels. We therefore keep the exact
 * radius for the geometry and only quantize the support window. Pixels fully
 * inside the crystal get weight 1, fully outside get 0, and pixels crossed by
 * the boundary get a linear partial occlusion in a 1-pixel transition band.
 */
static inline float _crystal_coverage(const int dx, const int dy, const float radius_f, const float vertices,
                                      const float rotation)
{
  const float local_radius = hypotf((float)dx, (float)dy);
  float signed_distance = 0.0f;
  const float theta = atan2f((float)dy, (float)dx);
  const float envelope = cosf(M_PI_F / vertices)
                          / cosf((2.0f * asinf(cosf(vertices * (theta + rotation))) + M_PI_F)
                                / (2.0f * vertices));
  const float polygon_radius = radius_f * envelope;
  signed_distance = polygon_radius - local_radius;
  return CLAMPS(signed_distance + 0.5f, 0.0f, 1.0f);
}

/**
 * @brief Build one partially-occluding crystal footprint for a layer.
 *
 * @details Each bank entry keeps one crystal size, shape and orientation, then
 * the stochastic look comes from stacking many layers and randomly picking
 * between several bank entries at each seed position. The support window is
 * rasterized to integer pixels, but each tap stores a partial-coverage weight
 * so non-integer radii do not collapse to a binary edge.
 */
__DT_CLONE_TARGETS__
static int _create_crystal_kernel(dt_iop_crystgrain_kernel_t *const kernel, const float radius_f,
                                  const float vertices, const float rotation)
{
  memset(kernel, 0, sizeof(*kernel));

  const int radius = MAX((int)ceilf(radius_f + 0.5f), 1);
  const int width = 2 * radius + 1;
  int count = 0;
  float area = 0.0f;

  for(int y = 0; y < width; y++)
  {
    for(int x = 0; x < width; x++)
    {
      const float alpha = _crystal_coverage(x - radius, y - radius, radius_f, vertices, rotation);
      if(alpha > FLT_EPSILON)
      {
        count++;
        area += alpha;
      }
    }
  }

  if(count <= 0 || area <= FLT_EPSILON) return 1;

  kernel->dx = malloc(sizeof(int) * count);
  kernel->dy = malloc(sizeof(int) * count);
  kernel->alpha = malloc(sizeof(float) * count);
  if(kernel->dx == NULL || kernel->dy == NULL || kernel->alpha == NULL)
  {
    free(kernel->dx);
    free(kernel->dy);
    free(kernel->alpha);
    memset(kernel, 0, sizeof(*kernel));
    return 1;
  }

  kernel->count = count;
  kernel->radius = radius;
  kernel->radius_f = radius_f;
  kernel->area = area;

  int k = 0;
  for(int y = 0; y < width; y++)
  {
    for(int x = 0; x < width; x++)
    {
      const float alpha = _crystal_coverage(x - radius, y - radius, radius_f, vertices, rotation);
      if(alpha > FLT_EPSILON)
      {
        kernel->dx[k] = x - radius;
        kernel->dy[k] = y - radius;
        kernel->alpha[k] = alpha;
        k++;
      }
    }
  }

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

  // Let the grain follow the preview scaling below 100% so zoomed-out views
  // stay visually coherent, but clamp at 100% to avoid inventing larger
  // crystals when the user zooms in past the native image scale.
  const float mean_size = MAX(rt->grain_size * rt->kernel_scale, 1.0f);
  const float max_size = MAX(3.0f * mean_size, 1.0f);

  for(int attempt = 0; attempt < 8; attempt++)
  {
    const float vertices = CLAMPS(6.0f + 1.5f * _gaussian_random(seed + 17u + attempt * 31u,
                                                                  seed + 23u + attempt * 37u),
                                  3.0f, 10.0f);
    const float rotation = 2.0f * M_PI_F * _uniform_random(seed + 101u + attempt * 43u);
    const float log_size = logf(mean_size) + rt->size_stddev * _gaussian_random(seed + 151u + attempt * 47u,
                                                                                 seed + 181u + attempt * 53u);
    const float random_size = CLAMPS(expf(log_size), 1.0f, max_size);
    const float radius_f = MAX(0.5f * (random_size - 1.0f), 0.5f);

    if(_create_crystal_kernel(&entry->footprint, radius_f, vertices, rotation) == 0)
    {
      entry->probability = _seed_probability(rt->filling, entry->footprint.area);
      entry->vertices = vertices;
      entry->rotation = rotation;
      entry->width = 2 * entry->footprint.radius + 1;
      return 0;
    }
  }

  if(_create_crystal_kernel(&entry->footprint, 0.5f, 4.0f, 0.0f) != 0) return 1;

  entry->probability = _seed_probability(rt->filling, entry->footprint.area);
  entry->vertices = 4.0f;
  entry->rotation = 0.0f;
  entry->width = 1;
  return 0;
}

/**
 * @brief Estimate the reference grain surface used to normalize layer capture.
 *
 * @details The user-facing layer capture is expressed against the average
 * grain size control, not against the exact randomized footprint drawn for
 * each seed. We therefore normalize it by the area of a circle built from the
 * average grain radius at the current preview scale.
 */
static inline float _average_grain_surface(const dt_iop_crystgrain_runtime_t *const rt)
{
  const float mean_size = MAX(rt->grain_size * rt->kernel_scale, 1.0f);
  const float mean_radius = MAX(0.5f * (mean_size - 1.0f), 0.5f);
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
 * @brief Predict the mean captured energy of one flat-field layer.
 *
 * @details The output normalization only needs the average exposure loss of
 * the stochastic crystal stack. For a unit flat field with remaining energy
 * `r`, one seed of kernel area `A` prints a flat tone
 * `c = min(r, A * layer_scale)`, because the unit input averages to `1` over
 * the whole crystal support and the layer sensitivity is expressed per grain
 * surface. One translated crystal contributes `c * alpha` to a destination
 * pixel, and the sum of all translated weights over the lattice equals `A`,
 * so the expected per-pixel capture of one bank entry is:
 *
 * `E_i = p_i * A_i * min(r, A_i * layer_scale)`
 *
 * where `p_i` is the Bernoulli seed probability of that bank entry. Averaging
 * `E_i` over the precomputed kernel bank gives a mean-field prediction of the
 * layer capture that depends only on the grain statistics, not on the image
 * content.
 */
__DT_CLONE_TARGETS__
static float _predict_layer_capture(const dt_iop_crystgrain_layer_kernel_t *const bank, const float layer_scale,
                                    const float remaining_fraction)
{
  double capture = 0.0;

  for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++)
  {
    const float area = bank[i].footprint.area;
    const float captured = fminf(remaining_fraction, area * layer_scale);
    capture += bank[i].probability * area * captured;
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
        float seed_energy = 0.0f;
        float original_energy = 0.0f;

        // The seed tests the light field that is still available after all
        // previous grains and layers have already depleted their share.
        if(_uniform_random(pixel_seed ^ 0xda942042e4dd58b5ull) >= entry->probability) continue;

        // Like the OpenCL path, each pixel either exits immediately or sweeps
        // only its own crystal footprint to print one flat tone into the
        // reconstruction while depleting the remaining light field in place.
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
        // The user layer scale now applies to the whole grain surface, so the
        // per-pixel flat tone cap must scale with the grain area too.
        original_energy *= rt->layer_scale;
        seed_energy = fminf(seed_energy, original_energy);
        if(seed_energy <= 0.0f) continue;

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
        float seed_energy = 0.0f;
        float original_energy = 0.0f;

        if(_uniform_random(pixel_seed ^ 0xda942042e4dd58b5ull) >= entry->probability) continue;

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
        if(captured <= 0.0f) continue;

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
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, in, work_profile, image) \
  schedule(static)
#endif
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
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, in, image) \
  schedule(static)
#endif
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
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, exposure, in, image, out, result) \
  schedule(static)
#endif
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
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, exposure_r, exposure_g, exposure_b, colorfulness, in, out, image, result) \
  schedule(static)
#endif
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
    float kernel_bank_cl[DT_CRYSTGRAIN_LAYER_KERNELS][4];
    cl_mem dev_kernel_bank = NULL;
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

    for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++)
    {
      kernel_bank_cl[i][0] = kernel_bank[i].vertices;
      kernel_bank_cl[i][1] = kernel_bank[i].rotation;
      kernel_bank_cl[i][2] = kernel_bank[i].probability;
      kernel_bank_cl[i][3] = kernel_bank[i].footprint.radius_f;
    }

    dev_kernel_bank = dt_opencl_copy_host_to_device_constant(devid, sizeof(kernel_bank_cl), kernel_bank_cl);
    _free_layer_kernel_bank(kernel_bank);
    if(dev_kernel_bank == NULL) return CL_MEM_OBJECT_ALLOCATION_FAILURE;

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
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_simulate_layer, sizes);
    dt_opencl_release_mem_object(dev_kernel_bank);
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
  const float kernel_scale = fminf(MAX(1.0f / dt_dev_get_module_scale(pipe, roi_in), 1e-6f), 1.0f);
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
  if(dev_image == NULL || dev_result == NULL || dev_remaining == NULL)
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
    .base_seed = ((uint64_t)_hash_string(pipe->image.filename) << 32)
                 ^ ((uint64_t)width << 16) ^ (uint64_t)height
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

    const int use_work_profile = (work_profile != NULL) ? 1 : 0;
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
  if(dev_image_rgb == NULL || dev_result_rgb == NULL || dev_remaining_rgb == NULL)
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
    float kernel_bank_cl[DT_CRYSTGRAIN_LAYER_KERNELS][4];
    cl_mem dev_kernel_bank = NULL;
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

    for(int i = 0; i < DT_CRYSTGRAIN_LAYER_KERNELS; i++)
    {
      kernel_bank_cl[i][0] = kernel_bank[i].vertices;
      kernel_bank_cl[i][1] = kernel_bank[i].rotation;
      kernel_bank_cl[i][2] = kernel_bank[i].probability;
      kernel_bank_cl[i][3] = kernel_bank[i].footprint.radius_f;
    }

    dev_kernel_bank = dt_opencl_copy_host_to_device_constant(devid, sizeof(kernel_bank_cl), kernel_bank_cl);
    _free_layer_kernel_bank(kernel_bank);
    if(dev_kernel_bank == NULL)
    {
      err = CL_MEM_OBJECT_ALLOCATION_FAILURE;
      goto error;
    }

    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 9, sizeof(cl_mem), &dev_kernel_bank);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 11, sizeof(int), &sublayer);
    dt_opencl_set_kernel_arg(devid, gd->kernel_simulate_layer_color, 12, sizeof(int), &active_channel);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_simulate_layer_color, sizes);
    dt_opencl_release_mem_object(dev_kernel_bank);
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
  // Grain size is authored in full-resolution output pixels at 100% zoom.
  const float kernel_scale = fminf(MAX(1.0f / dt_dev_get_module_scale(pipe, roi_in), 1e-6f), 1.0f);

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
    .base_seed = ((uint64_t)_hash_string(pipe->image.filename) << 32)
                 ^ ((uint64_t)width << 16) ^ (uint64_t)height
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
  dt_iop_crystgrain_gui_data_t *g = (dt_iop_crystgrain_gui_data_t *)self->gui_data;
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
  gtk_widget_set_tooltip_text(g->grain_size, _("average crystal footprint at 100% zoom, clamped so zooming in does not enlarge it further"));

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
