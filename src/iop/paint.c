/*
 * drawlayer paint subsystem
 *
 * This file is intentionally included from drawlayer.c instead of compiled as a
 * separate translation unit so it can keep using drawlayer private types, static
 * helpers, and local forward declarations without introducing another private header.
 */

static inline float _brush_profile(const drawlayer_dab_t *dab, const float norm2);
static inline gboolean _ensure_smudge_pixels(drawlayer_rt_path_state_t *state, const int width, const int height);
static inline gboolean _ensure_bristle_map(drawlayer_rt_path_state_t *state, const float amount);
static inline float _sample_alpha_noise(const drawlayer_dab_t *dab, drawlayer_rt_path_state_t *path_state,
                                        const gboolean have_bristle_map, const gboolean have_sprinkles,
                                        const float across, const float along, const float scaled_radius,
                                        const int pixel_x, const int pixel_y);
static inline float _stroke_flow_alpha(const drawlayer_dab_t *dab, const float opacity, const float flow,
                                       const float sample_opacity_scale, const float profile, const float brush_alpha,
                                       const float old_alpha, const float stroke_old_alpha,
                                       const gboolean have_stroke_alpha);
static void _paint_clear_stroke_state(drawlayer_stroke_t *stroke);
static void _paint_reset_worker_paths(dt_iop_drawlayer_gui_data_t *g);
static void _paint_reset_stroke_runtime(dt_iop_drawlayer_gui_data_t *g);
static void _flush_pending_initial_input(dt_iop_module_t *self, drawlayer_rt_path_state_t *state,
                                         void (*consume_sample)(dt_iop_module_t *, const drawlayer_dab_t *));
static void _flush_pending_live_input(dt_iop_module_t *self);
static void _flush_pending_backend_input(dt_iop_module_t *self);
static inline void _advance_smudge_pickup_state(drawlayer_rt_path_state_t *state, const drawlayer_dab_t *current,
                                                const drawlayer_dab_t *previous);
static inline void _sample_smudge_source_float(const float *buffer, const int width, const int height, const float sx,
                                               const float sy, const float motion_dx, const float motion_dy,
                                               const int jitter_x, const int jitter_y, float rgba[4]);
static inline float _smudge_deposit_alpha(const float src_alpha, const float carried_alpha, const float opacity);

static inline void _seed_noise_state(uint32_t state[4], const uint64_t seed)
{
  state[0] = splitmix32(seed ^ 0x9e3779b97f4a7c15ull);
  state[1] = splitmix32(seed ^ 0xbf58476d1ce4e5b9ull);
  state[2] = splitmix32(seed ^ 0x94d049bb133111ebull);
  state[3] = splitmix32(seed ^ 0xda942042e4dd58b5ull);
  if((state[0] | state[1] | state[2] | state[3]) == 0u) state[0] = 1u;
}

static inline float _smoothstep01(const float t)
{
  const float x = _clamp01(t);
  return x * x * (3.0f - 2.0f * x);
}

static inline float _poisson_noise01(const uint64_t seed, const float mu, const float sigma, const int flip)
{
  uint32_t state[4] = { 0 };
  _seed_noise_state(state, seed);
  return _clamp01(dt_noise_generator(DT_NOISE_POISSONIAN, mu, sigma, flip, state));
}

static inline float _value_noise_1d(const uint64_t seed, const float x, const float mu, const float sigma)
{
  const int i0 = (int)floorf(x);
  const int i1 = i0 + 1;
  const float t = _smoothstep01(x - i0);
  const float a = _poisson_noise01(seed ^ ((uint64_t)(uint32_t)i0 * 0x9e3779b185ebca87ull), mu, sigma, 0);
  const float b = _poisson_noise01(seed ^ ((uint64_t)(uint32_t)i1 * 0x9e3779b185ebca87ull), mu, sigma, 1);
  return _lerpf(a, b, t);
}

static inline float _value_noise_2d(const uint64_t seed, const float x, const float y, const float mu, const float sigma)
{
  const int x0 = (int)floorf(x);
  const int y0 = (int)floorf(y);
  const int x1 = x0 + 1;
  const int y1 = y0 + 1;
  const float tx = _smoothstep01(x - x0);
  const float ty = _smoothstep01(y - y0);

  const uint64_t s00 = seed ^ ((uint64_t)(uint32_t)x0 * 0x9e3779b185ebca87ull)
                       ^ ((uint64_t)(uint32_t)y0 * 0xc2b2ae3d27d4eb4full);
  const uint64_t s10 = seed ^ ((uint64_t)(uint32_t)x1 * 0x9e3779b185ebca87ull)
                       ^ ((uint64_t)(uint32_t)y0 * 0xc2b2ae3d27d4eb4full);
  const uint64_t s01 = seed ^ ((uint64_t)(uint32_t)x0 * 0x9e3779b185ebca87ull)
                       ^ ((uint64_t)(uint32_t)y1 * 0xc2b2ae3d27d4eb4full);
  const uint64_t s11 = seed ^ ((uint64_t)(uint32_t)x1 * 0x9e3779b185ebca87ull)
                       ^ ((uint64_t)(uint32_t)y1 * 0xc2b2ae3d27d4eb4full);

  const float n00 = _poisson_noise01(s00, mu, sigma, 0);
  const float n10 = _poisson_noise01(s10, mu, sigma, 1);
  const float n01 = _poisson_noise01(s01, mu, sigma, 1);
  const float n11 = _poisson_noise01(s11, mu, sigma, 0);
  const float nx0 = _lerpf(n00, n10, tx);
  const float nx1 = _lerpf(n01, n11, tx);
  return _lerpf(nx0, nx1, ty);
}

static inline float _sprinkle_noise_at_pixel(const drawlayer_dab_t *dab, const int px, const int py)
{
  if(!dab || dab->sprinkles <= 1e-6f) return 1.0f;
  /* Use continuous multi-scale value noise instead of explicit per-cell blobs.
   * The previous cell/disc model created obvious aliased spots on large
   * brushes. Here each pixel queries a continuous field, so the grain size is
   * controlled by `sprinkle_size` without broadcasting a visible grid. */
  const float scale = 1.0f / fmaxf(dab->sprinkle_size, 1.0f);
  const uint64_t seed = ((uint64_t)dab->stroke_batch << 32) ^ 0x7f4a7c159e3779b9ull;
  const float x = px * scale;
  const float y = py * scale;
  const float n0 = _value_noise_2d(seed, x, y, 0.5f, 0.8f);
  const float n1 = _value_noise_2d(seed ^ 0xbf58476d1ce4e5b9ull, x * 2.13f + 7.1f, y * 2.13f - 3.7f, 0.5f, 0.8f);
  const float n2 = _value_noise_2d(seed ^ 0x94d049bb133111ebull, x * 4.21f - 5.3f, y * 4.21f + 9.4f, 0.5f, 0.8f);
  const float field = 0.55f * n0 + 0.30f * n1 + 0.15f * n2;
  const float centered = 2.0f * field - 1.0f;
  return fmaxf(0.0f, 1.0f + 0.85f * _clamp01(dab->sprinkles) * centered);
}

static inline gboolean _ensure_bristle_map(drawlayer_rt_path_state_t *state, const float amount)
{
  if(!state) return FALSE;
  if(amount <= 1e-6f)
  {
    state->bristle_map_cached_seed = 0u;
    return FALSE;
  }

  if(state->bristle_map_cached_seed != state->bristle_map_seed)
    state->bristle_map_cached_seed = state->bristle_map_seed;

  return TRUE;
}

static inline float _bristle_noise_at_point(const drawlayer_dab_t *dab, const drawlayer_rt_path_state_t *state,
                                            const float across_px, const float along_px)
{
  if(!dab || !state || dab->bristles <= 1e-6f) return 1.0f;
  /* Keep bristles stroke-stable and directionally aligned, but remove the hard
   * stripe lattice. Build them from continuous 1D noise across the stroke,
   * then modulate that slowly along the stroke with a second continuous field.
   * This keeps a "hair" feel without obvious repetitive striping.
   *
   * Also introduce a slow, stroke-stable angular wobble in the local bristle
   * frame. If the across-axis is perfectly rigid, even continuous noise can
   * still read as aliasing because the same strand crossings recur at a fixed
   * projected angle. A small low-frequency random rotation breaks that
   * coherence without turning the texture into isotropic noise. */
  const float size = fmaxf(dab->bristle_size, 1.0f);
  const uint64_t seed = state->bristle_map_seed ^ 0x632be59bd9b4e019ull;
  const float along_phase = along_px / fmaxf(6.0f * size, 1.0f);
  const float angle_field = _value_noise_1d(seed ^ 0xa0761d6478bd642full, along_phase + 3.7f, 0.5f, 0.75f);
  const float dtheta = (2.0f * angle_field - 1.0f) * (12.0f * (float)G_PI / 180.0f) * _clamp01(dab->bristles);
  const float cs = cosf(dtheta);
  const float sn = sinf(dtheta);
  const float warped_across_px = cs * across_px - sn * along_px;
  const float warped_along_px = sn * across_px + cs * along_px;

  const float across = warped_across_px / size;
  const float along = warped_along_px / (3.5f * size);

  const float strand0 = _value_noise_1d(seed, across, 0.5f, 0.75f);
  const float strand1 = _value_noise_1d(seed ^ 0x9e3779b185ebca87ull, across * 2.07f + 1.3f, 0.5f, 0.75f);
  const float along_mod = _value_noise_2d(seed ^ 0xc2b2ae3d27d4eb4full,
                                          across * 0.35f + 4.1f, along + 2.7f, 0.5f, 0.7f);

  const float strand = 0.7f * strand0 + 0.3f * strand1;
  const float centered = 2.0f * strand - 1.0f;
  const float long_centered = 2.0f * along_mod - 1.0f;
  const float modulation = centered + 0.35f * long_centered;
  return fmaxf(0.0f, 1.0f + 0.55f * _clamp01(dab->bristles) * modulation);
}

static inline float _brush_transition_profile(dt_iop_drawlayer_brush_shape_t shape, float t, float inv_t);
static inline float _brush_transition_mass_primitive(dt_iop_drawlayer_brush_shape_t shape, float u,
                                                     float inner, float w, float base);

static inline float _brush_profile(const drawlayer_dab_t *dab, const float norm2)
{
  /* `norm2` is the squared normalized radius. For non-Gaussian modes, hardness defines
   * the inner full-opacity radius, and the selected fall-off shapes only the transition
   * from that inner radius to the outer radius at norm == 1. */
  if(!dab || norm2 >= 1.0f) return 0.0f;

  if(dab->shape == DT_IOP_DRAWLAYER_BRUSH_GAUSSIAN)
  {
    /* Replace the old infinite-support gaussian with a normalized centered
     * cubic cardinal B-spline. It is a fair bell-shaped approximation, but it
     * keeps a strict finite support at the nominal brush radius so it no
     * longer needs any truncation heuristics.
     *
     * Let x = 2r so the standard cubic B-spline support [-2, 2] maps to our
     * radial support [0, 1]. After normalizing B3(0) to 1, the radial profile
     * becomes:
     *
     *   r < 1/2 : 1 - 6 r^2 + 6 r^3
     *   else    : 2 (1 - r)^3
     */
    const float radius = sqrtf(norm2);
    if(radius < 0.5f)
      return 1.0f - 6.0f * norm2 + 6.0f * norm2 * radius;

    const float inv_r = 1.0f - radius;
    return 2.0f * inv_r * inv_r * inv_r;
  }

  const float hardness = _clamp01(dab->softness);
  if(hardness >= 1.0f - 1e-6f) return 1.0f;

  /* A purely mathematical inner radius of 0 would still not give a visible 100% center
   * sample once we rasterize at pixel centers. Clamp the minimum full-opacity core to
   * half a pixel in the dab's current coordinate space, so 0% hardness yields a single
   * fully opaque center pixel and the fall-off starts immediately after it. */
  const float min_inner = 0.5f / fmaxf(dab->radius, 0.5f);
  const float inner = fmaxf(hardness, _clamp01(min_inner));

  const float radius = sqrtf(norm2);
  if(radius <= inner) return 1.0f;

  const float t = _clamp01((radius - inner) / fmaxf(1.0f - inner, 1e-6f));
  const float inv_t = 1.0f - t;

  return _brush_transition_profile(dab->shape, t, inv_t);
}

static inline float _brush_mass_primitive(const drawlayer_dab_t *dab, const float u_in)
{
  if(!dab) return 0.0f;

  const float u = _clamp01(u_in);
  if(u <= 0.0f) return 0.0f;

  if(dab->shape == DT_IOP_DRAWLAYER_BRUSH_GAUSSIAN)
  {
    /* Exact primitive of the normalized cubic cardinal B-spline branch above:
     *
     *   r < 1/2 : f(r) = 1 - 6 r^2 + 6 r^3
     *   else    : f(r) = 2 (1 - r)^3
     *
     * We integrate f(r) * r dr, which is the radial alpha mass density used
     * by the asymptotic stroke normalization. */
    if(u <= 0.5f)
      return 0.5f * u * u - 1.5f * u * u * u * u + 1.2f * u * u * u * u * u;

    const float u2 = u * u;
    const float u3 = u2 * u;
    const float u4 = u2 * u2;
    const float u5 = u4 * u;
    return u2 - 2.0f * u3 + 1.5f * u4 - 0.4f * u5 - 0.0125f;
  }

  const float hardness = _clamp01(dab->softness);
  if(hardness >= 1.0f - 1e-6f) return 0.5f * u * u;

  const float min_inner = 0.5f / fmaxf(dab->radius, 0.5f);
  const float inner = fmaxf(hardness, _clamp01(min_inner));
  if(u <= inner) return 0.5f * u * u;

  const float w = fmaxf(1.0f - inner, 1e-6f);
  const float base = 0.5f * inner * inner;

  return _brush_transition_mass_primitive(dab->shape, u, inner, w, base);
}

static inline float _brush_transition_profile(const dt_iop_drawlayer_brush_shape_t shape, const float t, const float inv_t)
{
  switch(shape)
  {
    case DT_IOP_DRAWLAYER_BRUSH_QUADRATIC:
      return inv_t * inv_t;
    case DT_IOP_DRAWLAYER_BRUSH_SIGMOIDAL:
    {
      const float smooth = t * t * (3.0f - 2.0f * t);
      return 1.0f - smooth;
    }
    case DT_IOP_DRAWLAYER_BRUSH_LINEAR:
    default:
      return inv_t;
  }
}

static inline float _brush_transition_mass_primitive(const dt_iop_drawlayer_brush_shape_t shape, const float u,
                                                     const float inner, const float w, const float base)
{
  switch(shape)
  {
    case DT_IOP_DRAWLAYER_BRUSH_QUADRATIC:
    {
      const float q_u = 0.5f * u * u - (2.0f / 3.0f) * u * u * u + 0.25f * u * u * u * u;
      const float q_i = 0.5f * inner * inner - (2.0f / 3.0f) * inner * inner * inner
                        + 0.25f * inner * inner * inner * inner;
      return base + (q_u - q_i) / (w * w);
    }
    case DT_IOP_DRAWLAYER_BRUSH_SIGMOIDAL:
    {
      const float s = _clamp01((u - inner) / w);
      const float s2 = s * s;
      const float s3 = s2 * s;
      const float s4 = s2 * s2;
      const float s5 = s4 * s;
      const float delta = w * (inner * (s - s3 + 0.5f * s4)
                               + w * (0.5f * s2 - 0.75f * s4 + 0.4f * s5));
      return base + delta;
    }
    case DT_IOP_DRAWLAYER_BRUSH_LINEAR:
    default:
    {
      const float l_u = 0.5f * u * u - (1.0f / 3.0f) * u * u * u;
      const float l_i = 0.5f * inner * inner - (1.0f / 3.0f) * inner * inner * inner;
      return base + (l_u - l_i) / w;
    }
  }
}

static inline float _voronoi_strip_angle_measure(const float rho, const float strip_ratio)
{
  if(strip_ratio <= 0.0f) return 0.0f;
  if(rho <= strip_ratio + 1e-6f) return 2.0f * (float)G_PI;
  return 4.0f * asinf(_clamp01(strip_ratio / fmaxf(rho, 1e-6f)));
}

static inline float _stroke_sample_opacity_scale(const drawlayer_dab_t *dab, const float sample_step)
{
  if(!dab) return 1.0f;

  const float support_radius = fmaxf(dab->radius, 0.5f);
  const float overlap_span = 2.0f * support_radius;
  if(sample_step <= 1e-6f || overlap_span <= 1e-6f || sample_step >= overlap_span - 1e-6f) return 1.0f;

  /* We want the full-flow path to reach, on average, the same apparent
   * opacity as the no-flow "single coat" cap in the dense middle of an
   * infinitely long stroke.
   *
   * The correct asymptotic normalization is therefore not the raw count of
   * overlapping centers on the stroke axis. Visually we average over area, and
   * for soft brushes that average must also be weighted by the brush alpha.
   *
   * For an infinite periodic stroke, the relevant "single-dab share" is the
   * fraction of one dab's total alpha mass that lies inside its Voronoi strip:
   * the region closer to that dab center than to its neighbors, i.e. the
   * vertical strip |x| <= sample_step / 2.
   *
   * `ratio = strip_mass / dab_mass`
   *
   * is the reciprocal of the effective overlap multiplicity. The hot paths then
   * use it as an exponent:
   *
   *   a_step = 1 - (1 - opacity)^ratio
   *
   * so that repeated OVER application over the asymptotic overlap field
   * accumulates back to the user opacity on the well-covered middle of the
   * stroke. Start/end dabs still taper naturally because they have fewer
   * neighbors, which we intentionally keep as a feature. */

  const float half_strip = 0.5f * sample_step;

  /* A fully hard non-Gaussian brush is a plain disc, so the Voronoi-strip
   * share reduces exactly to the area fraction of the disc intersected by the
   * strip. Use the closed-form expression there. */
  if(dab->shape != DT_IOP_DRAWLAYER_BRUSH_GAUSSIAN && _clamp01(dab->softness) >= 1.0f - 1e-6f)
  {
    const float clamped_half_strip = fminf(half_strip, support_radius);
    const float chord_half = sqrtf(fmaxf(support_radius * support_radius
                                         - clamped_half_strip * clamped_half_strip, 0.0f));
    const float strip_area = sample_step * chord_half
                             + 2.0f * support_radius * support_radius
                                   * asinf(_clamp01(clamped_half_strip / support_radius));
    const float full_area = (float)G_PI * support_radius * support_radius;
    return _clamp01(strip_area / fmaxf(full_area, 1e-6f));
  }

  /* Soft brushes use the same idea, but with alpha mass instead of area.
   * Because the brush is radially analytical, the 2D overlap integral reduces
   * to a 1D polar integral:
   *   - the total mass is exact from the radial primitive above
   *   - the Voronoi strip contributes an exact angular measure at each radius
   *
   * We only keep a cheap midpoint quadrature over the normalized radius for
   * that strip term. This avoids the old pseudo-raster "sum alpha over sample
   * pixels" path entirely while preserving the same area/weight-based model. */
  const float strip_ratio = _clamp01(half_strip / support_radius);
  const float full_mass = 2.0f * (float)G_PI * _brush_mass_primitive(dab, 1.0f);
  if(full_mass <= 1e-6f) return 1.0f;

  enum
  {
    weight_samples = 32
  };
  const float dr = 1.0f / (float)weight_samples;
  float strip_mass = 0.0f;

  for(int ir = 0; ir < weight_samples; ir++)
  {
    const float rho = ((float)ir + 0.5f) * dr;
    const float profile = _brush_profile(dab, rho * rho);
    if(profile <= 0.0f) continue;
    const float angle = _voronoi_strip_angle_measure(rho, strip_ratio);
    strip_mass += angle * profile * rho * dr;
  }

  return _clamp01(strip_mass / full_mass);
}

#if DRAWLAYER_COMPARE_ANALYTIC_TIMINGS
static void _log_compare_timings(const char *label, const drawlayer_dab_t *dab, const int rect_w, const int rect_h,
                                 const gint64 main_us)
{
  dt_print(DT_DEBUG_PERF,
           "[drawlayer] %s dab x=%.2f y=%.2f area=%dx%d main=%" G_GINT64_FORMAT "us\n",
           label, dab->x, dab->y, rect_w, rect_h, main_us);
}
#endif

typedef struct drawlayer_stroke_mode_context_t
{
  float blur_rgba[4];
} drawlayer_stroke_mode_context_t;

typedef struct drawlayer_analytic_dab_context_t
{
  int x0;
  int y0;
  int x1;
  int y1;
  int sample_origin_x;
  int sample_origin_y;
  float opacity;
  float flow;
  int mode;
  float scaled_radius;
  float center_x;
  float center_y;
  float inv_radius;
  float tx;
  float ty;
  float paint_r;
  float paint_g;
  float paint_b;
  gboolean use_stroke_mask;
  gboolean is_plain_paint_fast;
  gboolean is_hard_paint_fast;
  gboolean have_bristle_map;
  gboolean have_sprinkles;
} drawlayer_analytic_dab_context_t;

typedef struct drawlayer_analytic_pixel_context_t
{
  float profile;
  float brush_alpha;
  float src_alpha;
  float stroke_old_alpha;
  float *stroke_alpha;
} drawlayer_analytic_pixel_context_t;

static gboolean _prepare_analytic_dab_context(drawlayer_analytic_dab_context_t *ctx, const drawlayer_dab_t *dab,
                                              const int width, const int height, const int origin_x,
                                              const int origin_y, const float scale,
                                              drawlayer_rt_path_state_t *path_state)
{
  /* Backend and preview analytic rasterizers share the same per-dab geometric
   * setup:
   *   - convert the dab support to a destination rectangle,
   *   - normalize the brush direction,
   *   - cache opacity/flow and the noise-source flags.
   *
   * The only differences between the two call sites are:
   *   - backend uses a scaled/origin-shifted layer-space target
   *   - preview uses a 1:1 local widget buffer
   *
   * Put that common work here so the two rasterizers keep only their
   * destination-format-specific pixel writes. */
  if(!ctx || !dab || dab->radius <= 0.0f || dab->opacity <= 0.0f || scale <= 0.0f) return FALSE;

  const float support_radius = dab->radius;
  ctx->opacity = _clamp01(dab->opacity);
  /* UX semantics: high flow means "more fluid", closer to watercolor, so it
   * should build up less with itself. Internally the compositing model uses the
   * opposite convention (0 = union cap, 1 = full self-build-up), so invert the
   * user value at the paint core. */
  ctx->flow = 1.0f - _clamp01(dab->flow);
  ctx->mode = dab->mode;
  ctx->scaled_radius = fmaxf(dab->radius * scale, 0.5f);

  ctx->x0 = MAX(0, (int)floorf((dab->x - support_radius) * scale) - origin_x);
  ctx->y0 = MAX(0, (int)floorf((dab->y - support_radius) * scale) - origin_y);
  ctx->x1 = MIN(width, (int)ceilf((dab->x + support_radius) * scale) - origin_x + 1);
  ctx->y1 = MIN(height, (int)ceilf((dab->y + support_radius) * scale) - origin_y + 1);
  if(ctx->x1 <= ctx->x0 || ctx->y1 <= ctx->y0) return FALSE;

  ctx->sample_origin_x = origin_x;
  ctx->sample_origin_y = origin_y;
  ctx->center_x = dab->x * scale - (float)origin_x;
  ctx->center_y = dab->y * scale - (float)origin_y;
  ctx->inv_radius = 1.0f / ctx->scaled_radius;

  const float dir_len = hypotf(dab->dir_x, dab->dir_y);
  ctx->tx = (dir_len > 1e-6f) ? (dab->dir_x / dir_len) : 0.0f;
  ctx->ty = (dir_len > 1e-6f) ? (dab->dir_y / dir_len) : 1.0f;
  /* Hoist the unchanging paint source color and the cheap mode flags out of
   * the inner loops. The common-case paint path can then avoid repeatedly
   * re-evaluating both the mode and the constant source channels. */
  ctx->paint_r = _clamp01(dab->color[0]);
  ctx->paint_g = _clamp01(dab->color[1]);
  ctx->paint_b = _clamp01(dab->color[2]);
  ctx->use_stroke_mask = (ctx->mode == DT_IOP_DRAWLAYER_MODE_PAINT || ctx->mode == DT_IOP_DRAWLAYER_MODE_ERASE);
  ctx->have_bristle_map = path_state && _ensure_bristle_map(path_state, dab->bristles);
  ctx->have_sprinkles = (dab->sprinkles > 1e-6f);
  ctx->is_plain_paint_fast = (ctx->mode == DT_IOP_DRAWLAYER_MODE_PAINT
                              && !ctx->have_bristle_map && !ctx->have_sprinkles);
  ctx->is_hard_paint_fast = (ctx->is_plain_paint_fast
                             && dab->shape != DT_IOP_DRAWLAYER_BRUSH_GAUSSIAN
                             && dab->softness >= 1.0f - 1e-6f);
  return TRUE;
}

static inline void _lookup_stroke_alpha(const drawlayer_analytic_dab_context_t *dab_ctx, float *stroke_mask,
                                        const int stroke_mask_width, const int stroke_mask_height,
                                        const int x, const int y, float **stroke_alpha, float *stroke_old_alpha)
{
  if(stroke_alpha) *stroke_alpha = NULL;
  if(stroke_old_alpha) *stroke_old_alpha = 0.0f;
  if(!dab_ctx || !dab_ctx->use_stroke_mask || !stroke_mask || !stroke_alpha || !stroke_old_alpha) return;
  if(x < 0 || y < 0 || x >= stroke_mask_width || y >= stroke_mask_height) return;

  *stroke_alpha = stroke_mask + (size_t)y * stroke_mask_width + x;
  *stroke_old_alpha = _clamp01(**stroke_alpha);
}

static gboolean _prepare_analytic_pixel_context(drawlayer_analytic_pixel_context_t *ctx,
                                                const drawlayer_analytic_dab_context_t *dab_ctx,
                                                const drawlayer_dab_t *dab,
                                                drawlayer_rt_path_state_t *path_state,
                                                const float sample_opacity_scale,
                                                float *stroke_mask, const int stroke_mask_width,
                                                const int stroke_mask_height,
                                                const int x, const int y, const float old_alpha)
{
  /* Shared per-pixel analytic brush math:
   *   1. evaluate the fall-off profile in normalized brush space,
   *   2. modulate it with bristles / sprinkles noise,
   *   3. look up the optional stroke-local alpha mask,
   *   4. compute the final per-dab alpha from the shared flow model.
   *
   * Backend and preview differ only in how they read/write destination pixels.
   * They both need the exact same alpha preparation, so centralize it here. */
  if(!ctx || !dab_ctx || !dab) return FALSE;

  const float dy = ((float)y + 0.5f - dab_ctx->center_y) * dab_ctx->inv_radius;
  const float dx = ((float)x + 0.5f - dab_ctx->center_x) * dab_ctx->inv_radius;
  const float norm2 = dx * dx + dy * dy;
  ctx->profile = _brush_profile(dab, norm2);
  if(ctx->profile <= 0.0f) return FALSE;

  const float across = dx * (-dab_ctx->ty) + dy * dab_ctx->tx;
  const float along = dx * dab_ctx->tx + dy * dab_ctx->ty;
  const float alpha_noise = _sample_alpha_noise(dab, path_state, dab_ctx->have_bristle_map, dab_ctx->have_sprinkles,
                                                across, along, dab_ctx->scaled_radius,
                                                dab_ctx->sample_origin_x + x, dab_ctx->sample_origin_y + y);

  ctx->brush_alpha = _clamp01(dab_ctx->opacity * ctx->profile * alpha_noise);
  if(ctx->brush_alpha <= 0.0f) return FALSE;

  _lookup_stroke_alpha(dab_ctx, stroke_mask, stroke_mask_width, stroke_mask_height, x, y,
                       &ctx->stroke_alpha, &ctx->stroke_old_alpha);

  ctx->src_alpha = _stroke_flow_alpha(dab, dab_ctx->opacity, dab_ctx->flow, sample_opacity_scale, ctx->profile,
                                      ctx->brush_alpha, old_alpha, ctx->stroke_old_alpha, ctx->stroke_alpha != NULL);
  return ctx->src_alpha > 0.0f;
}

static gboolean _prepare_blur_context(drawlayer_stroke_mode_context_t *ctx, const float *buffer, const int width,
                                      const int x0, const int y0, const int x1, const int y1,
                                      const float center_x, const float center_y, const float inv_radius,
                                      const drawlayer_dab_t *dab)
{
  float blur_weight_sum = 0.0f;
  memset(ctx->blur_rgba, 0, sizeof(ctx->blur_rgba));

  for(int y = y0; y < y1; y++)
  {
    const float dy = ((float)y + 0.5f - center_y) * inv_radius;
    const float dy2 = dy * dy;
    for(int x = x0; x < x1; x++)
    {
      const float dx = ((float)x + 0.5f - center_x) * inv_radius;
      const float blur_weight = _brush_profile(dab, dx * dx + dy2);
      if(blur_weight <= 0.0f) continue;

      const float *pixel = buffer + 4 * ((size_t)y * width + x);
      ctx->blur_rgba[0] += pixel[0] * blur_weight;
      ctx->blur_rgba[1] += pixel[1] * blur_weight;
      ctx->blur_rgba[2] += pixel[2] * blur_weight;
      ctx->blur_rgba[3] += pixel[3] * blur_weight;
      blur_weight_sum += blur_weight;
    }
  }

  if(blur_weight_sum <= 1e-8f) return FALSE;

  const float inv_weight = 1.0f / blur_weight_sum;
  ctx->blur_rgba[0] *= inv_weight;
  ctx->blur_rgba[1] *= inv_weight;
  ctx->blur_rgba[2] *= inv_weight;
  ctx->blur_rgba[3] *= inv_weight;
  return TRUE;
}

static gboolean _prepare_stroke_mode_context(drawlayer_stroke_mode_context_t *ctx, float *buffer, const int width,
                                             const int x0, const int y0, const int x1, const int y1,
                                             const float center_x, const float center_y, const float inv_radius,
                                             const drawlayer_dab_t *dab, drawlayer_rt_path_state_t *path_state)
{
  /* Prepare any mode-specific shared context once per dab before we enter the
   * hot per-pixel loops:
   * - blur precomputes the weighted local mean that all pixels in the dab will
   *   blend toward,
   * - smudge ensures the temporary per-pixel "bristle" payload buffer exists,
   * - paint / erase do not need extra setup here.
   *
   * Keeping this in one helper avoids repeating mode branching inside the
   * inner loops and makes the later dispatch read as "prepare once, apply many". */
  switch(dab->mode)
  {
    case DT_IOP_DRAWLAYER_MODE_BLUR:
      _prepare_blur_context(ctx, buffer, width, x0, y0, x1, y1, center_x, center_y, inv_radius, dab);
      return TRUE;
    case DT_IOP_DRAWLAYER_MODE_SMUDGE:
      return _ensure_smudge_pixels(path_state, x1 - x0, y1 - y0);
    case DT_IOP_DRAWLAYER_MODE_PAINT:
    case DT_IOP_DRAWLAYER_MODE_ERASE:
    default:
      return TRUE;
  }
}

static inline float _sample_alpha_noise(const drawlayer_dab_t *dab, drawlayer_rt_path_state_t *path_state,
                                        const gboolean have_bristle_map, const gboolean have_sprinkles,
                                        const float across, const float along, const float scaled_radius,
                                        const int pixel_x, const int pixel_y)
{
  /* Alpha-noise modulation is orthogonal to the painting mode. Compute it in a
   * shared helper so both backend and preview can use the same bristle /
   * sprinkle sampling path without duplicating that branching. */
  float alpha_noise = 1.0f;
  if(have_bristle_map)
    alpha_noise *= _bristle_noise_at_point(dab, path_state, across * scaled_radius, along * scaled_radius);
  if(have_sprinkles) alpha_noise *= _sprinkle_noise_at_pixel(dab, pixel_x, pixel_y);
  return alpha_noise;
}

static inline float _stroke_flow_alpha(const drawlayer_dab_t *dab, const float opacity, const float flow,
                                       const float sample_opacity_scale, const float profile, const float brush_alpha,
                                       const float old_alpha, const float stroke_old_alpha, const gboolean have_stroke_alpha)
{
  /* Paint and erase both need a stroke-local "already applied" reference for
   * the no-self-build-up branch. For paint that reference is the stroke-local
   * paint coverage mask. For erase it is the stroke-local erase coverage mask,
   * not the destination alpha already present on the layer. */
  if(dab->mode == DT_IOP_DRAWLAYER_MODE_SMUDGE)
  {
    /* Smudge is not paint-over. It replaces the current pixel locally by a
     * weighted average between what is already on the canvas and what the
     * brush bristles currently carry. That means it must bypass the stroke-
     * level OVER accumulation model used for paint/erase.
     *
     * However, smudge is still applied once per resampled dab. If we used
     * `brush_alpha` directly here, dense sampling would repeatedly apply that
     * local replacement and quickly converge toward a near-total replacement,
     * even at moderate opacity. Reuse the same asymptotic overlap ratio, but
     * as a pure replacement normalization:
     *
     *   a_step = 1 - (1 - a_target)^ratio
     *
     * where `ratio` is the reciprocal effective overlap multiplicity.
     * Repeating that local mix over the dense middle of the stroke then
     * converges back toward the target `brush_alpha`, which keeps a 50%
     * smudge looking like "leave half of what was there" and suppresses the
     * patterned disc train in the streak. */
    const float normalized = 1.0f - powf(fmaxf(1.0f - brush_alpha, 0.0f), _clamp01(sample_opacity_scale));
    return _clamp01(normalized);
  }

  const float flow_ref_alpha = have_stroke_alpha ? stroke_old_alpha
                                                 : ((dab->mode == DT_IOP_DRAWLAYER_MODE_ERASE) ? 0.0f : old_alpha);
  const float union_alpha = _stroke_union_effective_alpha(flow_ref_alpha, brush_alpha);
  /* On the centerline, roughly `1 / sample_opacity_scale` consecutive dabs
   * overlap when the brush is resampled at a fixed spatial step. We choose the
   * low-user-flow / high-build-up per-dab opacity so that repeated OVER
   * application over that overlap count accumulates back to the user opacity
   * in the dense middle of the stroke.
   *
   * This intentionally makes the first and last few dabs lighter because they
   * have fewer overlaps; we treat that as the expected natural taper of the
   * stroke ends. */
  const float accum_opacity = 1.0f - powf(fmaxf(1.0f - opacity, 0.0f), _clamp01(sample_opacity_scale));
  const float accum_alpha = accum_opacity * profile;
  return _clamp01(_lerpf(union_alpha, accum_alpha, flow));
}

static void _apply_non_smudge_stroke_mode(float *pixel, const drawlayer_dab_t *dab, const float src_alpha,
                                          const float old_r, const float old_g, const float old_b, const float old_alpha,
                                          const drawlayer_stroke_mode_context_t *ctx)
{
  /* Paint, erase and blur can all be expressed as a pure local transform of
   * the current destination pixel plus the already-prepared per-dab context.
   * Keep them together here so the main raster loop only needs one dispatch
   * point after alpha has been computed. Smudge remains separate because it
   * needs the full destination buffer and its per-pixel carried payload. */
  switch(dab->mode)
  {
    case DT_IOP_DRAWLAYER_MODE_ERASE:
    {
      const float inv_alpha = 1.0f - src_alpha;
      pixel[0] = old_r * inv_alpha;
      pixel[1] = old_g * inv_alpha;
      pixel[2] = old_b * inv_alpha;
      pixel[3] = old_alpha * inv_alpha;
      return;
    }
    case DT_IOP_DRAWLAYER_MODE_BLUR:
    {
      const float inv_alpha = 1.0f - src_alpha;
      pixel[0] = ctx->blur_rgba[0] * src_alpha + old_r * inv_alpha;
      pixel[1] = ctx->blur_rgba[1] * src_alpha + old_g * inv_alpha;
      pixel[2] = ctx->blur_rgba[2] * src_alpha + old_b * inv_alpha;
      pixel[3] = ctx->blur_rgba[3] * src_alpha + old_alpha * inv_alpha;
      return;
    }
    case DT_IOP_DRAWLAYER_MODE_PAINT:
    case DT_IOP_DRAWLAYER_MODE_SMUDGE:
    default:
    {
      const float inv_alpha = 1.0f - src_alpha;
      pixel[0] = dab->color[0] * src_alpha + old_r * inv_alpha;
      pixel[1] = dab->color[1] * src_alpha + old_g * inv_alpha;
      pixel[2] = dab->color[2] * src_alpha + old_b * inv_alpha;
      pixel[3] = src_alpha + old_alpha * inv_alpha;
      return;
    }
  }
}

static void _apply_smudge_stroke_mode(float *buffer, const int width, const int height,
                                      drawlayer_rt_path_state_t *path_state,
                                      const drawlayer_dab_t *dab, const float scale,
                                      const int origin_x, const int origin_y,
                                      const int x0, const int y0,
                                      const int x, const int y,
                                      const float center_x, const float center_y,
                                      const float src_alpha,
                                      const float old_r, const float old_g, const float old_b, const float old_alpha)
{
  /* Smudge is the one mode that cannot be reduced to "current pixel in,
   * current pixel out". It needs:
   * - a lagged pickup position,
   * - a source sample gathered from the surrounding buffer,
   * - and a carried per-pixel bristle payload that gets reloaded after each
   *   application.
   *
   * Keep that more involved state machine isolated here so the raster loop only
   * decides "smudge vs not smudge" and delegates the full transport logic. */
  float sampled_rgba[4] = { 0.0f };
  float sample_x = (float)x;
  float sample_y = (float)y;
  float motion_dx = 0.0f;
  float motion_dy = 0.0f;

  if(path_state && path_state->have_smudge_pickup)
  {
    float pickup_center_x = path_state->smudge_pickup_x;
    float pickup_center_y = path_state->smudge_pickup_y;
    pickup_center_x = pickup_center_x * scale - (float)origin_x;
    pickup_center_y = pickup_center_y * scale - (float)origin_y;
    sample_x = (float)x + (pickup_center_x - center_x);
    sample_y = (float)y + (pickup_center_y - center_y);
    motion_dx = center_x - pickup_center_x;
    motion_dy = center_y - pickup_center_y;
  }

  _sample_smudge_source_float(buffer, width, height, sample_x, sample_y, motion_dx, motion_dy,
                              x - x0, y - y0, sampled_rgba);

  float *pixel = buffer + 4 * ((size_t)y * width + x);
  float *bristle = path_state->smudge_pixels + 4 * ((size_t)(y - y0) * path_state->smudge_width + (x - x0));
  const float carried_rgba[4] = { bristle[0], bristle[1], bristle[2], bristle[3] };
  const float pickup_blend = _clamp01(dab->opacity);
  const float carried_alpha = _clamp01(carried_rgba[3]);
  const float deposit_alpha = _smudge_deposit_alpha(src_alpha, carried_alpha, dab->opacity);
  const float inv_alpha = 1.0f - deposit_alpha;

  pixel[0] = carried_rgba[0] * deposit_alpha + old_r * inv_alpha;
  pixel[1] = carried_rgba[1] * deposit_alpha + old_g * inv_alpha;
  pixel[2] = carried_rgba[2] * deposit_alpha + old_b * inv_alpha;
  pixel[3] = carried_rgba[3] * deposit_alpha + old_alpha * inv_alpha;

  for(int c = 0; c < 4; c++) bristle[c] = _lerpf(carried_rgba[c], sampled_rgba[c], pickup_blend);
}

static void _accumulate_stroke_dab_plain_paint_hard(float *buffer, const int width, const int height,
                                                    const drawlayer_analytic_dab_context_t *dab_ctx,
                                                    const drawlayer_dab_t *dab, const float sample_opacity_scale,
                                                    float *stroke_mask, const int stroke_mask_width,
                                                    const int stroke_mask_height)
{
  /* Fastest common-case backend path:
   *   - paint mode only
   *   - no bristles / no sprinkles
   *   - hard non-gaussian brush
   *
   * In that configuration the brush profile is a flat disc, so inside the
   * support radius the per-pixel work reduces to:
   *   1. disc membership test,
   *   2. stroke-flow alpha,
   *   3. one paint-over blend.
   *
   * This avoids the generic profile/noise helper and keeps the hottest path
   * branch-free apart from the disc test itself. */
  for(int y = dab_ctx->y0; y < dab_ctx->y1; y++)
  {
    const float dy = ((float)y + 0.5f - dab_ctx->center_y) * dab_ctx->inv_radius;
    const float dy2 = dy * dy;
    for(int x = dab_ctx->x0; x < dab_ctx->x1; x++)
    {
      const float dx = ((float)x + 0.5f - dab_ctx->center_x) * dab_ctx->inv_radius;
      if(dx * dx + dy2 >= 1.0f) continue;

      float *pixel = buffer + 4 * ((size_t)y * width + x);
      const float old_alpha = _clamp01(pixel[3]);
      const float old_r = (old_alpha > 1e-8f) ? pixel[0] : 0.0f;
      const float old_g = (old_alpha > 1e-8f) ? pixel[1] : 0.0f;
      const float old_b = (old_alpha > 1e-8f) ? pixel[2] : 0.0f;

      float stroke_old_alpha = 0.0f;
      float *stroke_alpha = NULL;
      _lookup_stroke_alpha(dab_ctx, stroke_mask, stroke_mask_width, stroke_mask_height, x, y,
                           &stroke_alpha, &stroke_old_alpha);

      const float src_alpha = _stroke_flow_alpha(dab, dab_ctx->opacity, dab_ctx->flow, sample_opacity_scale,
                                                 1.0f, dab_ctx->opacity, old_alpha,
                                                 stroke_old_alpha, stroke_alpha != NULL);
      if(src_alpha <= 0.0f) continue;

      const float inv_alpha = 1.0f - src_alpha;
      pixel[0] = dab_ctx->paint_r * src_alpha + old_r * inv_alpha;
      pixel[1] = dab_ctx->paint_g * src_alpha + old_g * inv_alpha;
      pixel[2] = dab_ctx->paint_b * src_alpha + old_b * inv_alpha;
      pixel[3] = src_alpha + old_alpha * inv_alpha;

      if(stroke_alpha)
        *stroke_alpha = src_alpha + stroke_old_alpha * (1.0f - src_alpha);
    }
  }
}

static void _accumulate_stroke_dab_plain_paint_soft(float *buffer, const int width, const int height,
                                                    const drawlayer_analytic_dab_context_t *dab_ctx,
                                                    const drawlayer_dab_t *dab, const float sample_opacity_scale,
                                                    float *stroke_mask, const int stroke_mask_width,
                                                    const int stroke_mask_height)
{
  /* Second common-case fast path:
   *   - paint mode only
   *   - no bristles / no sprinkles
   *
   * This still needs the analytical fall-off, but it can skip the full generic
   * per-pixel helper because there is no alpha-noise modulation and no mode
   * dispatch inside the loop. */
  for(int y = dab_ctx->y0; y < dab_ctx->y1; y++)
  {
    const float dy = ((float)y + 0.5f - dab_ctx->center_y) * dab_ctx->inv_radius;
    const float dy2 = dy * dy;
    for(int x = dab_ctx->x0; x < dab_ctx->x1; x++)
    {
      const float dx = ((float)x + 0.5f - dab_ctx->center_x) * dab_ctx->inv_radius;
      const float profile = _brush_profile(dab, dx * dx + dy2);
      if(profile <= 0.0f) continue;

      float *pixel = buffer + 4 * ((size_t)y * width + x);
      const float old_alpha = _clamp01(pixel[3]);
      const float old_r = (old_alpha > 1e-8f) ? pixel[0] : 0.0f;
      const float old_g = (old_alpha > 1e-8f) ? pixel[1] : 0.0f;
      const float old_b = (old_alpha > 1e-8f) ? pixel[2] : 0.0f;

      float stroke_old_alpha = 0.0f;
      float *stroke_alpha = NULL;
      _lookup_stroke_alpha(dab_ctx, stroke_mask, stroke_mask_width, stroke_mask_height, x, y,
                           &stroke_alpha, &stroke_old_alpha);

      const float brush_alpha = dab_ctx->opacity * profile;
      const float src_alpha = _stroke_flow_alpha(dab, dab_ctx->opacity, dab_ctx->flow, sample_opacity_scale,
                                                 profile, brush_alpha, old_alpha,
                                                 stroke_old_alpha, stroke_alpha != NULL);
      if(src_alpha <= 0.0f) continue;

      const float inv_alpha = 1.0f - src_alpha;
      pixel[0] = dab_ctx->paint_r * src_alpha + old_r * inv_alpha;
      pixel[1] = dab_ctx->paint_g * src_alpha + old_g * inv_alpha;
      pixel[2] = dab_ctx->paint_b * src_alpha + old_b * inv_alpha;
      pixel[3] = src_alpha + old_alpha * inv_alpha;

      if(stroke_alpha)
        *stroke_alpha = src_alpha + stroke_old_alpha * (1.0f - src_alpha);
    }
  }
}

/* Keep the brush analytic here.
 * A cached kernel looks appealing, but in this path the cache quantization, extra memory traffic,
 * and per-dab setup cost outweighed the saved math while also degrading the stroke shape. */
static void _accumulate_stroke_dab_analytic(float *buffer, const int width, const int height, const int origin_x,
                                            const int origin_y, const float scale, const drawlayer_dab_t *dab,
                                            const float sample_opacity_scale, float *stroke_mask,
                                            const int stroke_mask_width, const int stroke_mask_height,
                                            drawlayer_rt_path_state_t *path_state)
{
  drawlayer_analytic_dab_context_t dab_ctx = { 0 };
  if(!_prepare_analytic_dab_context(&dab_ctx, dab, width, height, origin_x, origin_y, scale, path_state)) return;

  if(dab_ctx.is_hard_paint_fast)
  {
    _accumulate_stroke_dab_plain_paint_hard(buffer, width, height, &dab_ctx, dab, sample_opacity_scale,
                                            stroke_mask, stroke_mask_width, stroke_mask_height);
    return;
  }

  if(dab_ctx.is_plain_paint_fast)
  {
    _accumulate_stroke_dab_plain_paint_soft(buffer, width, height, &dab_ctx, dab, sample_opacity_scale,
                                            stroke_mask, stroke_mask_width, stroke_mask_height);
    return;
  }

  drawlayer_stroke_mode_context_t mode_ctx = { 0 };
  if(!_prepare_stroke_mode_context(&mode_ctx, buffer, width, dab_ctx.x0, dab_ctx.y0, dab_ctx.x1, dab_ctx.y1,
                                   dab_ctx.center_x, dab_ctx.center_y, dab_ctx.inv_radius, dab, path_state))
    return;

  /* Do not use OpenMP work-sharing or SIMD hints here.
   * This kernel is called for every interpolated dab, so thread-team setup and scheduling
   * overhead dominate the useful work, and the explicit SIMD hint also benchmarks slower
   * than the compiler's plain scalar loop here. Keep this as straight C. */
  /* The outer loop walks scanlines in the affected rectangle. The inner loop evaluates
   * one destination pixel, converts it from half to float, applies the premultiplied
   * brush operation, then stores it back to half immediately. */
  /* Same brush math as the layer cache path above, but rendered into the transient
   * widget overlay using display-space color for immediate on-screen feedback. */
  for(int y = dab_ctx.y0; y < dab_ctx.y1; y++)
  {
    for(int x = dab_ctx.x0; x < dab_ctx.x1; x++)
    {
      float *pixel = buffer + 4 * ((size_t)y * width + x);
      const float old_alpha = _clamp01(pixel[3]);
      const float old_r = (old_alpha > 1e-8f) ? pixel[0] : 0.0f;
      const float old_g = (old_alpha > 1e-8f) ? pixel[1] : 0.0f;
      const float old_b = (old_alpha > 1e-8f) ? pixel[2] : 0.0f;
      drawlayer_analytic_pixel_context_t pixel_ctx = { 0 };
      if(!_prepare_analytic_pixel_context(&pixel_ctx, &dab_ctx, dab, path_state, sample_opacity_scale,
                                         stroke_mask, stroke_mask_width, stroke_mask_height, x, y, old_alpha))
        continue;

      /* After the shared alpha preparation, the only remaining mode decision in
       * the backend loop is whether this pixel uses the full smudge transport
       * path or one of the simpler local blend modes. */
      if(dab_ctx.mode == DT_IOP_DRAWLAYER_MODE_SMUDGE)
        _apply_smudge_stroke_mode(buffer, width, height, path_state, dab, scale, origin_x, origin_y,
                                  dab_ctx.x0, dab_ctx.y0, x, y, dab_ctx.center_x, dab_ctx.center_y, pixel_ctx.src_alpha,
                                  old_r, old_g, old_b, old_alpha);
      else
        _apply_non_smudge_stroke_mode(pixel, dab, pixel_ctx.src_alpha, old_r, old_g, old_b, old_alpha, &mode_ctx);

      if(pixel_ctx.stroke_alpha)
        *pixel_ctx.stroke_alpha = pixel_ctx.src_alpha + pixel_ctx.stroke_old_alpha * (1.0f - pixel_ctx.src_alpha);
    }
  }
}

static gboolean _compute_analytic_dab_bounds(const drawlayer_dab_t *dab, const int width, const int height,
                                             const int origin_x, const int origin_y, const float scale,
                                             int *x0, int *y0, int *x1, int *y1)
{
  /* The wrapper functions around the analytic rasterizers all need the same
   * cheap pre-pass:
   *   - convert the dab support to a clipped destination rectangle,
   *   - decide whether the dab touches anything at all,
   *   - expose that rectangle for damage tracking and perf logging.
   *
   * Keep that rectangle setup in one helper so the wrappers no longer duplicate
   * the same support-radius math before calling their analytic body. */
  if(!dab || dab->radius <= 0.0f || scale <= 0.0f || width <= 0 || height <= 0) return FALSE;

  const float support_radius = dab->radius;
  const int bx0 = MAX(0, (int)floorf((dab->x - support_radius) * scale) - origin_x);
  const int by0 = MAX(0, (int)floorf((dab->y - support_radius) * scale) - origin_y);
  const int bx1 = MIN(width, (int)ceilf((dab->x + support_radius) * scale) - origin_x + 1);
  const int by1 = MIN(height, (int)ceilf((dab->y + support_radius) * scale) - origin_y + 1);
  if(bx1 <= bx0 || by1 <= by0) return FALSE;

  if(x0) *x0 = bx0;
  if(y0) *y0 = by0;
  if(x1) *x1 = bx1;
  if(y1) *y1 = by1;
  return TRUE;
}

static void _accumulate_stroke_dab(float *buffer, const int width, const int height, const int origin_x,
                                   const int origin_y, const float scale, const drawlayer_dab_t *dab,
                                   const float sample_opacity_scale, float *stroke_mask,
                                   const int stroke_mask_width, const int stroke_mask_height,
                                   drawlayer_rt_path_state_t *path_state,
                                   gboolean *damage_valid, int *damage_x0, int *damage_y0, int *damage_x1, int *damage_y1)
{
  if(!buffer || !dab || dab->radius <= 0.0f || dab->opacity <= 0.0f || scale <= 0.0f) return;

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if(!_compute_analytic_dab_bounds(dab, width, height, origin_x, origin_y, scale, &x0, &y0, &x1, &y1)) return;
  _union_damage_rect(damage_valid, damage_x0, damage_y0, damage_x1, damage_y1, x0, y0, x1, y1);
#if DRAWLAYER_COMPARE_ANALYTIC_TIMINGS
  const gint64 t0 = g_get_monotonic_time();
#endif

  _accumulate_stroke_dab_analytic(buffer, width, height, origin_x, origin_y, scale, dab, sample_opacity_scale,
                                  stroke_mask, stroke_mask_width, stroke_mask_height, path_state);

#if DRAWLAYER_COMPARE_ANALYTIC_TIMINGS
  const gint64 t1 = g_get_monotonic_time();
  _log_compare_timings("layer", dab, x1 - x0, y1 - y0, t1 - t0);
#endif
}

static float _dab_sample_spacing(const drawlayer_dab_t *dab)
{
  /* Convert the user-facing `distance` percentage into the spatial metronome
   * spacing used for stroke resampling.
   *
   * The UI meaning is:
   *   0%   => emit a sample every pixel
   *   100% => emit a sample every brush diameter
   *
   * This helper applies that rule to one dab, using its current radius. */
  if(!dab) return 1.0f;

  const float percent = _conf_distance() / 100.0f;
  const float radius = fmaxf(0.5f, dab->radius);
  const float diameter = 2.0f * radius;
  return _lerpf(1.0f, diameter, percent);
}

static float _segment_sample_spacing(const drawlayer_dab_t *dabs, const int count)
{
  /* Convert the UI `distance` semantic into a real spatial step:
   *   0%   => one sample per pixel
   *   100% => one sample per brush diameter */
  if(count <= 0) return 1.0f;
  if(count == 1) return _dab_sample_spacing(&dabs[0]);

  const drawlayer_dab_t *p_start = &dabs[count - 2];
  const drawlayer_dab_t *p_end = &dabs[count - 1];
  const float min_radius = fmaxf(0.5f, fminf(p_start->radius, p_end->radius));
  const drawlayer_dab_t tmp = { .radius = min_radius };
  return _dab_sample_spacing(&tmp);
}

static inline drawlayer_dab_t _lerp_dab(const drawlayer_dab_t *a, const drawlayer_dab_t *b, const float t)
{
  /* Raw cursor sample after applying GUI-side brush settings. This is not painted as-is:
   * it may be smoothed, then it is resampled into the constant-distance stroke stream. */
  drawlayer_dab_t dab = {
    .x = _lerpf(a->x, b->x, t),
    .y = _lerpf(a->y, b->y, t),
    .wx = _lerpf(a->wx, b->wx, t),
    .wy = _lerpf(a->wy, b->wy, t),
    .radius = fmaxf(0.5f, _lerpf(a->radius, b->radius, t)),
    .dir_x = 0.0f,
    .dir_y = 0.0f,
    .opacity = _clamp01(_lerpf(a->opacity, b->opacity, t)),
    .flow = _clamp01(_lerpf(a->flow, b->flow, t)),
    .bristles = _clamp01(_lerpf(a->bristles, b->bristles, t)),
    .sprinkles = _clamp01(_lerpf(a->sprinkles, b->sprinkles, t)),
    .softness = _clamp01(_lerpf(a->softness, b->softness, t)),
    .color = {
      _lerpf(a->color[0], b->color[0], t),
      _lerpf(a->color[1], b->color[1], t),
      _lerpf(a->color[2], b->color[2], t),
    },
    .display_color = {
      _lerpf(a->display_color[0], b->display_color[0], t),
      _lerpf(a->display_color[1], b->display_color[1], t),
      _lerpf(a->display_color[2], b->display_color[2], t),
    },
    .shape = (t < 0.5f) ? a->shape : b->shape,
    .mode = (t < 0.5f) ? a->mode : b->mode,
  };
  {
    const float dir_x = _lerpf(a->dir_x, b->dir_x, t);
    const float dir_y = _lerpf(a->dir_y, b->dir_y, t);
    const float dir_len = hypotf(dir_x, dir_y);
    if(dir_len > 1e-6f)
    {
      dab.dir_x = dir_x / dir_len;
      dab.dir_y = dir_y / dir_len;
    }
  }
  return dab;
}

static int _segment_sample_layout_with_spacing(const drawlayer_dab_t *dabs, const int count, const float spacing,
                                               float *distance_carry, float *first_distance,
                                               float *step_distance, float *segment_length)
{
  /* For one raw cursor segment, decide how many uniformly spaced samples must be emitted.
   * `distance_carry` stores leftover arc length from the previous raw segment so the
   * whole stroke is sampled in continuous path space instead of "per event". */
  if(first_distance) *first_distance = 0.0f;
  if(step_distance) *step_distance = 0.0f;
  if(segment_length) *segment_length = 0.0f;

  if(count <= 0) return 0;
  if(count == 1)
  {
    if(distance_carry) *distance_carry = 0.0f;
    return 1;
  }

  const drawlayer_dab_t *p_start = &dabs[count - 2];
  const drawlayer_dab_t *p_end = &dabs[count - 1];
  const float length = hypotf(p_end->x - p_start->x, p_end->y - p_start->y);
  if(segment_length) *segment_length = length;
  if(length <= 1e-6f) return 0;

  const float step = fmaxf(spacing, 1e-6f);
  const float carry = distance_carry ? CLAMP(*distance_carry, 0.0f, step) : 0.0f;
  const float start = (carry <= 1e-6f) ? step : (step - carry);
  if(start > length)
  {
    if(distance_carry) *distance_carry = fminf(step, carry + length);
    if(step_distance) *step_distance = step;
    return 0;
  }

  const int steps = 1 + (int)floorf((length - start) / step);
  const float used = start + (steps - 1) * step;
  if(first_distance) *first_distance = start;
  if(step_distance) *step_distance = step;
  if(distance_carry) *distance_carry = fmaxf(0.0f, length - used);
  return MAX(0, steps);
}

static void _reset_input_path_state(drawlayer_rt_path_state_t *state)
{
  /* Reset the worker-local path generator at stroke boundaries.
   *
   * Workers do their own raw-event -> evenly spaced dab conversion, so each
   * worker keeps an independent "last raw point + carry" state. This must be
   * cleared when a new stroke starts, or when batches jump unexpectedly. */
  if(!state) return;
  if(state->history) g_array_set_size(state->history, 0);
  state->have_last_input_dab = FALSE;
  state->have_last_point = FALSE;
  state->last_event_ts = 0;
  state->distance_carry = 0.0f;
  state->stroke_batch = 0u;
  state->smudge_pickup_x = 0.0f;
  state->smudge_pickup_y = 0.0f;
  state->have_smudge_pickup = FALSE;
  if(state->smudge_pixels && state->smudge_width > 0 && state->smudge_height > 0)
    memset(state->smudge_pixels, 0, (size_t)state->smudge_width * state->smudge_height * 4 * sizeof(float));
  state->bristle_map_seed = 0u;
  state->bristle_map_cached_seed = 0u;
}

static void _paint_clear_stroke_state(drawlayer_stroke_t *stroke)
{
  /* Keep the primitive stroke reset in the paint subsystem so GUI/module code
   * does not need to know how stroke runtime state is represented. */
  if(!stroke) return;
  memset(stroke, 0, sizeof(*stroke));
}

static void _paint_reset_worker_paths(dt_iop_drawlayer_gui_data_t *g)
{
  /* Both workers keep independent input-path generators. Most controller
   * actions need both reset together, so expose one narrow helper instead of
   * making drawlayer.c touch the two internals directly. */
  if(!g) return;
  _reset_input_path_state(&g->preview_path);
  _reset_input_path_state(&g->backend_path);
  _reset_input_path_state(&g->process_path);
}

static void _paint_reset_stroke_runtime(dt_iop_drawlayer_gui_data_t *g)
{
  /* Common stroke-side reset used by commits, clears, and initialization. */
  if(!g) return;
  _paint_clear_stroke_state(&g->stroke);
  _paint_reset_worker_paths(g);
  if(g->process_stroke_mask && g->process_stroke_mask_width > 0 && g->process_stroke_mask_height > 0)
    memset(g->process_stroke_mask, 0,
           (size_t)g->process_stroke_mask_width * g->process_stroke_mask_height * sizeof(float));
}

static inline gboolean _ensure_smudge_pixels(drawlayer_rt_path_state_t *state, const int width, const int height)
{
  if(!state || width <= 0 || height <= 0) return FALSE;
  if(state->smudge_width == width && state->smudge_height == height && state->smudge_pixels) return TRUE;

  float *pixels = g_realloc(state->smudge_pixels, (size_t)width * height * 4 * sizeof(float));
  if(!pixels) return FALSE;

  state->smudge_pixels = pixels;
  state->smudge_width = width;
  state->smudge_height = height;
  memset(state->smudge_pixels, 0, (size_t)width * height * 4 * sizeof(float));
  return TRUE;
}

static void _flush_pending_initial_input(dt_iop_module_t *self, drawlayer_rt_path_state_t *state,
                                         void (*consume_sample)(dt_iop_module_t *, const drawlayer_dab_t *))
{
  if(!self || !state || !state->history || !consume_sample || !state->have_last_input_dab || state->history->len > 0) return;

  drawlayer_dab_t dab = state->last_input_dab;
  if(dab.bristles > 1e-6f)
  {
    const float dir_len = hypotf(dab.dir_x, dab.dir_y);
    if(dir_len <= 1e-6f)
    {
      /* A single-dab stroke has no future sample to infer a meaningful
       * tangent from. Falling back to a fixed vertical direction makes the
       * bristle pattern visibly biased. Use a deterministic random angle
       * instead so taps still get a plausible, non-axis-aligned bristle
       * orientation while remaining stable for the stroke. */
      const uint32_t h = splitmix32(state->bristle_map_seed ^ 0x5bf03635u);
      const float angle = 2.0f * (float)G_PI * ((h & 0x00ffffffu) / 16777216.0f);
      dab.dir_x = cosf(angle);
      dab.dir_y = sinf(angle);
    }
  }
  dab.stroke_pos = DRAWLAYER_STROKE_FIRST;
  g_array_append_val(state->history, dab);
  consume_sample(self, &dab);
}

static inline float _smudge_hash_signed(const int x, const int y, const int lane)
{
  guint32 h = (guint32)(x * 73856093u) ^ (guint32)(y * 19349663u) ^ (guint32)(lane * 83492791u);
  h ^= h >> 13;
  h *= 1274126177u;
  h ^= h >> 16;
  return ((h & 0xffffu) / 32767.5f) - 1.0f;
}

static inline void _sample_rgba_float_bilinear(const float *buffer, const int width, const int height, const float x,
                                               const float y, float rgba[4])
{
  memset(rgba, 0, 4 * sizeof(float));
  if(!buffer || width <= 0 || height <= 0) return;

  const float fx = CLAMP(x, 0.0f, (float)(width - 1));
  const float fy = CLAMP(y, 0.0f, (float)(height - 1));
  const int x0 = (int)floorf(fx);
  const int y0 = (int)floorf(fy);
  const int x1 = MIN(width - 1, x0 + 1);
  const int y1 = MIN(height - 1, y0 + 1);
  const float tx = fx - x0;
  const float ty = fy - y0;

  const float *p00 = buffer + 4 * ((size_t)y0 * width + x0);
  const float *p10 = buffer + 4 * ((size_t)y0 * width + x1);
  const float *p01 = buffer + 4 * ((size_t)y1 * width + x0);
  const float *p11 = buffer + 4 * ((size_t)y1 * width + x1);

  for(int c = 0; c < 4; c++)
  {
    const float a = _lerpf(p00[c], p10[c], tx);
    const float b = _lerpf(p01[c], p11[c], tx);
    rgba[c] = _lerpf(a, b, ty);
  }
}

static inline void _sample_smudge_source_float(const float *buffer, const int width, const int height, const float sx,
                                               const float sy, const float motion_dx, const float motion_dy,
                                               const int jitter_x, const int jitter_y, float rgba[4])
{
  memset(rgba, 0, 4 * sizeof(float));
  if(!buffer || width <= 0 || height <= 0) return;

  float dir_x = motion_dx;
  float dir_y = motion_dy;
  const float motion = hypotf(dir_x, dir_y);
  if(motion > 1e-6f)
  {
    dir_x /= motion;
    dir_y /= motion;
  }
  else
  {
    dir_x = 1.0f;
    dir_y = 0.0f;
  }

  const float perp_x = -dir_y;
  const float perp_y = dir_x;
  /* Use a wider anisotropic pickup kernel with stronger lateral randomness so
   * smudge diffuses more and breaks up repeated edge echoes. The kernel is
   * still transport-oriented, but it now samples a broader, noisier blob
   * instead of a tight near-rigid copy of the source. */
  const float jitter = 0.60f * _smudge_hash_signed(jitter_x, jitter_y, 0);
  const float side = 0.90f + 0.30f * _smudge_hash_signed(jitter_x, jitter_y, 1);
  const float trail = 0.80f + 0.25f * _smudge_hash_signed(jitter_x, jitter_y, 2);

  const float taps[7][3] = {
    { 0.00f, jitter, 0.24f },
    { -trail, 0.25f + jitter, 0.18f },
    { -0.45f, -0.35f + jitter, 0.15f },
    { -0.15f, side + jitter, 0.11f },
    { -0.15f, -side + jitter, 0.11f },
    { 0.25f, 0.45f * side + jitter, 0.11f },
    { 0.25f, -0.45f * side + jitter, 0.10f },
  };

  float weight_sum = 0.0f;
  for(int i = 0; i < 7; i++)
  {
    float sample[4] = { 0.0f };
    const float px = sx + dir_x * taps[i][0] + perp_x * taps[i][1];
    const float py = sy + dir_y * taps[i][0] + perp_y * taps[i][1];
    _sample_rgba_float_bilinear(buffer, width, height, px, py, sample);
    const float w = taps[i][2];
    for(int c = 0; c < 4; c++) rgba[c] += sample[c] * w;
    weight_sum += w;
  }

  if(weight_sum > 1e-8f)
  {
    const float inv = 1.0f / weight_sum;
    for(int c = 0; c < 4; c++) rgba[c] *= inv;
  }
}

static inline float _smudge_deposit_alpha(const float src_alpha, const float carried_alpha, const float opacity)
{
  /* An "empty" brush should still be able to transport transparency, otherwise
   * starting a smudge on blank areas can never drag that blankness into painted
   * regions. But using the full local replacement factor there feels like a
   * hard eraser. Use the user-set opacity itself as the floor for empty
   * bristles, then ramp smoothly up to the full replacement strength as the
   * brush picks up actual opacity. This keeps the "transport nothingness"
   * behavior user-driven instead of tied to an arbitrary fixed constant. */
  const float carry = _clamp01(carried_alpha);
  const float base = _clamp01(opacity);
  const float influence = base + (1.0f - base) * carry;
  return _clamp01(src_alpha * influence);
}

static inline float _mapping_profile_value(const drawlayer_mapping_profile_t profile, const float x)
{
  const float v = _clamp01(x);
  switch(profile)
  {
    case DRAWLAYER_PROFILE_QUADRATIC:
      return v * v;
    case DRAWLAYER_PROFILE_SQRT:
      return sqrtf(v);
    case DRAWLAYER_PROFILE_LINEAR:
    default:
      return v;
  }
}

static inline float _mapping_multiplier(const drawlayer_mapping_profile_t profile, const float normalized_input)
{
  return 1.0f + _mapping_profile_value(profile, normalized_input);
}

static inline void _advance_smudge_pickup_state(drawlayer_rt_path_state_t *state, const drawlayer_dab_t *current,
                                                const drawlayer_dab_t *previous)
{
  if(!state || !current) return;

  if(!state->have_smudge_pickup)
  {
    state->smudge_pickup_x = current->x;
    state->smudge_pickup_y = current->y;
    state->have_smudge_pickup = TRUE;
    return;
  }

  /* The pickup source is a low-pass follower of the brush center, not a hard
   * "one radius behind" offset. A rigid radius-sized lag imprints a visible
   * wavelength of about one brush radius when smudging across hard edges.
   *
   * Instead, keep a persistent pickup point that moves only part of the way
   * toward the current dab center as the stroke advances. The response is tied
   * to spatial progress, not event cadence: splitting the same traveled
   * distance into more input samples yields the same total pickup motion. */
  const float dx = previous ? (current->x - previous->x) : 0.0f;
  const float dy = previous ? (current->y - previous->y) : 0.0f;
  const float travel = hypotf(dx, dy);
  if(travel <= 1e-6f) return;

  const float radius = fmaxf(current->radius, 0.5f);
  const float response = 1.0f - expf(-0.5f * travel / radius);
  state->smudge_pickup_x = _lerpf(state->smudge_pickup_x, current->x, response);
  state->smudge_pickup_y = _lerpf(state->smudge_pickup_y, current->y, response);
}

static gboolean _build_worker_input_dab(dt_iop_module_t *self, drawlayer_rt_path_state_t *state,
                                        const drawlayer_raw_input_t *input, drawlayer_dab_t *dab)
{
  /* Convert one raw GUI input event into a fully parameterized dab in layer
   * coordinates.
   *
   * This is still not the final painted dab stream: the result will then be
   * smoothed and resampled at constant distance. This helper is responsible
   * only for:
   *   - widget -> layer coordinate conversion
   *   - dynamic brush settings (size/opacity/flow/hardness)
   *   - pressure / tilt / acceleration mapping
   *   - color preparation in both display and pipeline spaces */
  if(!self || !state || !input || !dab) return FALSE;

  float lx = 0.0f;
  float ly = 0.0f;
  if(!_widget_to_layer_coords(self, input->wx, input->wy, &lx, &ly)) return FALSE;

  const float pressure_norm = _clamp01(input->pressure);
  const float tilt_norm = _clamp01(input->tilt);
  const float accel_norm = _clamp01(input->acceleration);
  const gboolean map_pressure_size = _conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_SIZE);
  const gboolean map_pressure_opacity = _conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_OPACITY);
  const gboolean map_pressure_flow = _conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_FLOW);
  const gboolean map_pressure_softness = _conf_bool(DRAWLAYER_CONF_MAP_PRESSURE_SOFTNESS);
  const gboolean map_tilt_size = _conf_bool(DRAWLAYER_CONF_MAP_TILT_SIZE);
  const gboolean map_tilt_opacity = _conf_bool(DRAWLAYER_CONF_MAP_TILT_OPACITY);
  const gboolean map_tilt_flow = _conf_bool(DRAWLAYER_CONF_MAP_TILT_FLOW);
  const gboolean map_tilt_softness = _conf_bool(DRAWLAYER_CONF_MAP_TILT_SOFTNESS);
  const gboolean map_accel_size = _conf_bool(DRAWLAYER_CONF_MAP_ACCEL_SIZE);
  const gboolean map_accel_opacity = _conf_bool(DRAWLAYER_CONF_MAP_ACCEL_OPACITY);
  const gboolean map_accel_flow = _conf_bool(DRAWLAYER_CONF_MAP_ACCEL_FLOW);
  const gboolean map_accel_softness = _conf_bool(DRAWLAYER_CONF_MAP_ACCEL_SOFTNESS);
  const drawlayer_mapping_profile_t pressure_profile = _conf_mapping_profile(DRAWLAYER_CONF_PRESSURE_PROFILE);
  const drawlayer_mapping_profile_t tilt_profile = _conf_mapping_profile(DRAWLAYER_CONF_TILT_PROFILE);
  const drawlayer_mapping_profile_t accel_profile = _conf_mapping_profile(DRAWLAYER_CONF_ACCEL_PROFILE);
  const float pressure_coeff = _mapping_multiplier(pressure_profile, pressure_norm);
  const float tilt_coeff = _mapping_multiplier(tilt_profile, tilt_norm);
  const float accel_coeff = _mapping_multiplier(accel_profile, accel_norm);

  float radius = _conf_size();
  float opacity = _conf_opacity() / 100.0f;
  float flow = _conf_flow() / 100.0f;
  const float bristles = _conf_bristles() / 100.0f;
  const float sprinkles = _conf_sprinkles() / 100.0f;
  float hardness = _conf_hardness();
  const float base_radius = radius;
  const float base_opacity = opacity;
  const float base_flow = flow;
  const float base_hardness = hardness;

  if(map_pressure_size) radius *= pressure_coeff;
  if(map_pressure_opacity) opacity *= pressure_coeff;
  if(map_pressure_flow) flow *= pressure_coeff;
  if(map_pressure_softness) hardness *= pressure_coeff;

  if(map_tilt_size) radius *= tilt_coeff;
  if(map_tilt_opacity) opacity *= tilt_coeff;
  if(map_tilt_flow) flow *= tilt_coeff;
  if(map_tilt_softness) hardness *= tilt_coeff;

  if(map_accel_size) radius *= accel_coeff;
  if(map_accel_opacity) opacity *= accel_coeff;
  if(map_accel_flow) flow *= accel_coeff;
  if(map_accel_softness) hardness *= accel_coeff;

  radius = fmaxf(radius, 0.5f);
  hardness = _clamp01(hardness);
  float display_rgb[3] = { 0.0f };
  float pipeline_rgb[3] = { 0.0f };
  _get_brush_colors(self, display_rgb, pipeline_rgb);

  float dir_x = 0.0f;
  float dir_y = 0.0f;
  if(state->have_last_point)
  {
    const float dx = lx - state->last_layer_x;
    const float dy = ly - state->last_layer_y;
    const float dir_len = hypotf(dx, dy);
    if(dir_len > 1e-6f)
    {
      dir_x = dx / dir_len;
      dir_y = dy / dir_len;
    }
  }

  *dab = (drawlayer_dab_t){
    .x = lx,
    .y = ly,
    .wx = input->wx,
    .wy = input->wy,
    .radius = radius,
    .dir_x = dir_x,
    .dir_y = dir_y,
    .opacity = _clamp01(opacity),
    .flow = _clamp01(flow),
    .bristles = _clamp01(bristles),
    .sprinkles = _clamp01(sprinkles),
    .bristle_size = _conf_bristle_size(),
    .sprinkle_size = _conf_sprinkle_size(),
    .softness = hardness,
    .color = { pipeline_rgb[0], pipeline_rgb[1], pipeline_rgb[2] },
    .display_color = { display_rgb[0], display_rgb[1], display_rgb[2] },
    .shape = _conf_brush_shape(),
    .mode = _conf_brush_mode(),
    .stroke_batch = input->stroke_batch,
    .stroke_pos = input->stroke_pos,
  };

  state->last_layer_x = lx;
  state->last_layer_y = ly;
  state->have_last_point = TRUE;
  state->last_event_ts = input->event_ts;

  if((map_pressure_size || map_pressure_opacity || map_pressure_flow || map_pressure_softness
      || map_tilt_size || map_tilt_opacity || map_tilt_flow || map_tilt_softness
      || map_accel_size || map_accel_opacity || map_accel_flow || map_accel_softness)
     && (input->stroke_pos != DRAWLAYER_STROKE_MIDDLE || ((state->history->len & 15u) == 0u)))
  {
    dt_print(DT_DEBUG_INPUT,
             "[drawlayer] map p=%.4f t=%.4f a=%.4f coeff[p=%.4f t=%.4f a=%.4f] "
             "base[r=%.2f o=%.3f f=%.3f h=%.3f] out[r=%.2f o=%.3f f=%.3f h=%.3f] "
             "flags[p=%d%d%d%d t=%d%d%d%d a=%d%d%d%d]\n",
             pressure_norm, tilt_norm, accel_norm,
             pressure_coeff, tilt_coeff, accel_coeff,
             base_radius, base_opacity, base_flow, base_hardness,
             radius, _clamp01(opacity), _clamp01(flow), _clamp01(hardness),
             map_pressure_size ? 1 : 0, map_pressure_opacity ? 1 : 0, map_pressure_flow ? 1 : 0, map_pressure_softness ? 1 : 0,
             map_tilt_size ? 1 : 0, map_tilt_opacity ? 1 : 0, map_tilt_flow ? 1 : 0, map_tilt_softness ? 1 : 0,
             map_accel_size ? 1 : 0, map_accel_opacity ? 1 : 0, map_accel_flow ? 1 : 0, map_accel_softness ? 1 : 0);
  }

  return TRUE;
}

static gboolean _worker_input_starts_new_stroke(const drawlayer_rt_path_state_t *state,
                                                const drawlayer_raw_input_t *input)
{
  if(!state || !input) return FALSE;
  return input->stroke_pos == DRAWLAYER_STROKE_FIRST
         || (state->stroke_batch != 0u && input->stroke_batch != 0u && input->stroke_batch != state->stroke_batch);
}

static void _seed_worker_stroke_randomness(drawlayer_rt_path_state_t *state, const drawlayer_raw_input_t *input)
{
  if(!state || !input) return;

  const uint64_t qx = (uint64_t)(int64_t)llrintf((double)input->wx * 256.0);
  const uint64_t qy = (uint64_t)(int64_t)llrintf((double)input->wy * 256.0);
  state->bristle_map_seed = ((uint64_t)input->stroke_batch << 32)
                            ^ (uint64_t)input->event_ts
                            ^ (qx * 0x9e3779b185ebca87ull)
                            ^ (qy * 0xc2b2ae3d27d4eb4full);
}

static void _emit_first_worker_sample(dt_iop_module_t *self, drawlayer_rt_path_state_t *state,
                                      const drawlayer_dab_t *dab,
                                      void (*consume_sample)(dt_iop_module_t *, const drawlayer_dab_t *))
{
  if(!state || !dab || !consume_sample) return;

  state->last_input_dab = *dab;
  state->have_last_input_dab = TRUE;
  if(dab->bristles <= 1e-6f)
  {
    drawlayer_dab_t first = *dab;
    first.stroke_pos = DRAWLAYER_STROKE_FIRST;
    g_array_append_val(state->history, first);
    consume_sample(self, &first);
  }
}

static void _flush_pending_initial_input_if_needed(dt_iop_module_t *self, drawlayer_rt_path_state_t *state,
                                                   const drawlayer_dab_t *dab,
                                                   void (*consume_sample)(dt_iop_module_t *, const drawlayer_dab_t *))
{
  if(!state || !dab || state->history->len != 0) return;

  const float dx = dab->x - state->last_input_dab.x;
  const float dy = dab->y - state->last_input_dab.y;
  const float dir_len = hypotf(dx, dy);
  if(dir_len > 1e-6f)
  {
    state->last_input_dab.dir_x = dx / dir_len;
    state->last_input_dab.dir_y = dy / dir_len;
  }
  _flush_pending_initial_input(self, state, consume_sample);
}

static void _emit_resampled_worker_segment(dt_iop_module_t *self, const drawlayer_raw_input_t *input,
                                           drawlayer_rt_path_state_t *state, const drawlayer_dab_t *current_dab,
                                           const float first_distance, const float step_distance,
                                           const float segment_length, const int steps,
                                           void (*consume_sample)(dt_iop_module_t *, const drawlayer_dab_t *))
{
  if(!self || !input || !state || !current_dab || !consume_sample) return;

  for(int step = 0; step < steps; step++)
  {
    const float sample_distance = first_distance + step * step_distance;
    const float t = _clamp01(sample_distance / fmaxf(segment_length, 1e-6f));
    drawlayer_dab_t sample = _lerp_dab(&state->last_input_dab, current_dab, t);
    sample.stroke_batch = input->stroke_batch;
    sample.stroke_pos = DRAWLAYER_STROKE_MIDDLE;
    g_array_append_val(state->history, sample);
    consume_sample(self, &sample);
  }
}

typedef struct drawlayer_segment_window_ctx_t
{
  const drawlayer_dab_t *p_prev;
  const drawlayer_dab_t *p_start;
  const drawlayer_dab_t *p_end;
  float dir_x;
  float dir_y;
  float m1x;
  float m1y;
  float m2x;
  float m2y;
} drawlayer_segment_window_ctx_t;

static void _prepare_segment_window_ctx(const drawlayer_dab_t *dabs, const int count,
                                        drawlayer_segment_window_ctx_t *ctx)
{
  if(!dabs || count < 2 || !ctx) return;

  ctx->p_prev = (count >= 3) ? &dabs[count - 3] : &dabs[count - 2];
  ctx->p_start = &dabs[count - 2];
  ctx->p_end = &dabs[count - 1];

  const float seg_dx = ctx->p_end->x - ctx->p_start->x;
  const float seg_dy = ctx->p_end->y - ctx->p_start->y;
  const float seg_len = hypotf(seg_dx, seg_dy);
  ctx->dir_x = (seg_len > 1e-6f) ? (seg_dx / seg_len) : ctx->p_start->dir_x;
  ctx->dir_y = (seg_len > 1e-6f) ? (seg_dy / seg_len) : ctx->p_start->dir_y;
  ctx->m1x = (count >= 3) ? 0.5f * (ctx->p_end->x - ctx->p_prev->x)
                          : (ctx->p_end->x - ctx->p_start->x);
  ctx->m1y = (count >= 3) ? 0.5f * (ctx->p_end->y - ctx->p_prev->y)
                          : (ctx->p_end->y - ctx->p_start->y);
  ctx->m2x = ctx->p_end->x - ctx->p_start->x;
  ctx->m2y = ctx->p_end->y - ctx->p_start->y;
}

static drawlayer_dab_t _build_segment_window_sample(const drawlayer_segment_window_ctx_t *ctx, const float t)
{
  drawlayer_dab_t dab = {
    .x = _cubic_hermitef(ctx->p_start->x, ctx->p_end->x, ctx->m1x, ctx->m2x, t),
    .y = _cubic_hermitef(ctx->p_start->y, ctx->p_end->y, ctx->m1y, ctx->m2y, t),
    .wx = _lerpf(ctx->p_start->wx, ctx->p_end->wx, t),
    .wy = _lerpf(ctx->p_start->wy, ctx->p_end->wy, t),
    .radius = fmaxf(0.5f, _lerpf(ctx->p_start->radius, ctx->p_end->radius, t)),
    .dir_x = ctx->dir_x,
    .dir_y = ctx->dir_y,
    .opacity = _clamp01(_lerpf(ctx->p_start->opacity, ctx->p_end->opacity, t)),
    .flow = _clamp01(_lerpf(ctx->p_start->flow, ctx->p_end->flow, t)),
    .bristles = _clamp01(_lerpf(ctx->p_start->bristles, ctx->p_end->bristles, t)),
    .sprinkles = _clamp01(_lerpf(ctx->p_start->sprinkles, ctx->p_end->sprinkles, t)),
    .softness = _clamp01(_lerpf(ctx->p_start->softness, ctx->p_end->softness, t)),
    .color = {
      _lerpf(ctx->p_start->color[0], ctx->p_end->color[0], t),
      _lerpf(ctx->p_start->color[1], ctx->p_end->color[1], t),
      _lerpf(ctx->p_start->color[2], ctx->p_end->color[2], t),
    },
    .display_color = {
      _lerpf(ctx->p_start->display_color[0], ctx->p_end->display_color[0], t),
      _lerpf(ctx->p_start->display_color[1], ctx->p_end->display_color[1], t),
      _lerpf(ctx->p_start->display_color[2], ctx->p_end->display_color[2], t),
    },
    .shape = (t < 0.5f) ? ctx->p_start->shape : ctx->p_end->shape,
    .mode = (t < 0.5f) ? ctx->p_start->mode : ctx->p_end->mode,
  };
  return dab;
}

static void _process_worker_input_common(dt_iop_module_t *self, const drawlayer_raw_input_t *input,
                                         drawlayer_rt_path_state_t *state,
                                         void (*consume_sample)(dt_iop_module_t *, const drawlayer_dab_t *))
{
  /* Shared raw-input processing used by both workers.
   *
   * The GUI thread only queues raw pointer events. Each worker independently:
   *   1. converts the raw event to a dab in its own coordinate basis,
   *   2. optionally smooths the newest raw point,
   *   3. emits the evenly spaced "metronome" dabs for that segment,
   *   4. forwards those resampled dabs to its own consumer.
   *
   * This keeps preview and backend logic decoupled while guaranteeing they both
   * derive their stroke from the same raw inputs and the same `distance`
   * semantics. */
  if(!self || !input || !state || !state->history || !consume_sample) return;

  if(_worker_input_starts_new_stroke(state, input))
  {
    _reset_input_path_state(state);
    _seed_worker_stroke_randomness(state, input);
  }

  drawlayer_dab_t dab = { 0 };
  if(!_build_worker_input_dab(self, state, input, &dab)) return;

  state->stroke_batch = input->stroke_batch;

  if(!state->have_last_input_dab)
  {
    _emit_first_worker_sample(self, state, &dab, consume_sample);
    return;
  }

  drawlayer_dab_t segment[2] = { state->last_input_dab, dab };
  const float sample_spacing = _segment_sample_spacing(segment, 2);
  const float sample_radius = fmaxf(0.5f, fminf(segment[0].radius, segment[1].radius));
  _apply_input_smoothing(self, state->history, &dab, sample_spacing, sample_radius);
  segment[1] = dab;

  _flush_pending_initial_input_if_needed(self, state, &dab, consume_sample);

  float first_distance = 0.0f;
  float step_distance = 0.0f;
  float segment_length = 0.0f;
  const int steps = _segment_sample_layout_with_spacing(segment, 2, sample_spacing, &state->distance_carry,
                                                        &first_distance, &step_distance, &segment_length);

  _emit_resampled_worker_segment(self, input, state, &dab, first_distance, step_distance, segment_length,
                                 steps, consume_sample);

  state->last_input_dab = dab;
}

static void _rasterize_segment_window(float *buffer, const int width, const int height, const int origin_x,
                                      const int origin_y, const float scale, const drawlayer_dab_t *dabs,
                                      const int count, const int steps, const float first_distance,
                                      const float step_distance, const float segment_length,
                                      float *stroke_mask, const int stroke_mask_width, const int stroke_mask_height,
                                      drawlayer_rt_path_state_t *path_state,
                                      gboolean *damage_valid, int *damage_x0,
                                      int *damage_y0, int *damage_x1, int *damage_y1)
{
  /* Replay only the newest local segment from the rolling stroke window.
   * - count == 1: one isolated dab
   * - count >= 2: cubic Hermite interpolation between the last two samples
   *   using the previous sample (when available) only to estimate the entering tangent.
  * This same local geometry is used for live preview and for the persistent layer cache. */
  if(count <= 0) return;

  if(count == 1)
  {
    if(path_state)
      path_state->have_smudge_pickup = FALSE;
    const float sample_step = _segment_sample_spacing(dabs, 1);
    const float sample_opacity_scale = _stroke_sample_opacity_scale(&dabs[0], sample_step);
    _accumulate_stroke_dab(buffer, width, height, origin_x, origin_y, scale, &dabs[0], sample_opacity_scale,
                           stroke_mask, stroke_mask_width, stroke_mask_height, path_state,
                           damage_valid, damage_x0, damage_y0, damage_x1, damage_y1);
    return;
  }

  drawlayer_segment_window_ctx_t segment_ctx = { 0 };
  _prepare_segment_window_ctx(dabs, count, &segment_ctx);
  const float sample_step = (steps > 1 && step_distance > 1e-6f) ? step_distance : segment_length;
  drawlayer_dab_t previous_sample = *segment_ctx.p_start;
  /* Emit the spatial metronome samples for this raw cursor segment. These are the only
   * samples queued to the worker, so stroke density is driven by geometry, not by event cadence. */
  for(int step = 0; step < steps; step++)
  {
    const float sample_distance = first_distance + step * step_distance;
    const float t = _clamp01(sample_distance / fmaxf(segment_length, 1e-6f));
    drawlayer_dab_t dab = _build_segment_window_sample(&segment_ctx, t);
    if(path_state)
    {
      if(dab.mode == DT_IOP_DRAWLAYER_MODE_SMUDGE)
        _advance_smudge_pickup_state(path_state, &dab, &previous_sample);
      else
        path_state->have_smudge_pickup = FALSE;
    }
    const float sample_opacity_scale = _stroke_sample_opacity_scale(&dab, sample_step);
    _accumulate_stroke_dab(buffer, width, height, origin_x, origin_y, scale, &dab, sample_opacity_scale,
                           stroke_mask, stroke_mask_width, stroke_mask_height, path_state,
                           damage_valid, damage_x0, damage_y0, damage_x1, damage_y1);
    previous_sample = dab;
  }
}

static void _union_damage_rect(gboolean *valid, int *x0, int *y0, int *x1, int *y1,
                               const int add_x0, const int add_y0, const int add_x1, const int add_y1)
{
  if(add_x1 <= add_x0 || add_y1 <= add_y0) return;

  if(!*valid)
  {
    *x0 = add_x0;
    *y0 = add_y0;
    *x1 = add_x1;
    *y1 = add_y1;
    *valid = TRUE;
    return;
  }

  *x0 = MIN(*x0, add_x0);
  *y0 = MIN(*y0, add_y0);
  *x1 = MAX(*x1, add_x1);
  *y1 = MAX(*y1, add_y1);
}

static void _accumulate_preview_dab_analytic(unsigned char *buffer, const int stride, const int width, const int height,
                                             const drawlayer_dab_t *dab, const float sample_opacity_scale,
                                             float *stroke_mask, const int stroke_mask_width,
                                             const int stroke_mask_height, drawlayer_rt_path_state_t *path_state)
{
  if(!buffer) return;

  /* The 8-bit live preview overlay is intentionally paint-only. It gets
   * cleared after each stroke is committed into the real float layer cache and
   * the pipeline recomputes, so non-additive modes (erase, blur, smudge) do
   * not provide stable or useful feedback there. Avoid maintaining dead preview
   * logic for them and leave those modes to the backend path only. */
  if(dab->mode != DT_IOP_DRAWLAYER_MODE_PAINT) return;

  drawlayer_analytic_dab_context_t dab_ctx = { 0 };
  if(!_prepare_analytic_dab_context(&dab_ctx, dab, width, height, 0, 0, 1.0f, path_state)) return;

  /* Same rationale as the layer buffer path above:
   * explicit OpenMP work-sharing and SIMD hints both benchmark slower here than plain C. */
  for(int y = dab_ctx.y0; y < dab_ctx.y1; y++)
  {
    for(int x = dab_ctx.x0; x < dab_ctx.x1; x++)
    {
      unsigned char *pixel = buffer + (size_t)y * stride + 4 * x;
      const float dst_a = pixel[3] / 255.0f;
      const float dst_b = (dst_a > 1e-8f) ? (pixel[0] / 255.0f) : 0.0f;
      const float dst_g = (dst_a > 1e-8f) ? (pixel[1] / 255.0f) : 0.0f;
      const float dst_r = (dst_a > 1e-8f) ? (pixel[2] / 255.0f) : 0.0f;
      drawlayer_analytic_pixel_context_t pixel_ctx = { 0 };
      if(!_prepare_analytic_pixel_context(&pixel_ctx, &dab_ctx, dab, path_state, sample_opacity_scale,
                                         stroke_mask, stroke_mask_width, stroke_mask_height, x, y, dst_a))
        continue;

      const float src_r = _clamp01(dab->display_color[0]) * pixel_ctx.src_alpha;
      const float src_g = _clamp01(dab->display_color[1]) * pixel_ctx.src_alpha;
      const float src_b = _clamp01(dab->display_color[2]) * pixel_ctx.src_alpha;
      const float inv_alpha = 1.0f - pixel_ctx.src_alpha;
      const float out_r = src_r + dst_r * inv_alpha;
      const float out_g = src_g + dst_g * inv_alpha;
      const float out_b = src_b + dst_b * inv_alpha;
      const float out_a = pixel_ctx.src_alpha + dst_a * inv_alpha;

      pixel[0] = (unsigned char)roundf(255.0f * _clamp01(out_b));
      pixel[1] = (unsigned char)roundf(255.0f * _clamp01(out_g));
      pixel[2] = (unsigned char)roundf(255.0f * _clamp01(out_r));
      pixel[3] = (unsigned char)roundf(255.0f * _clamp01(out_a));
      if(pixel_ctx.stroke_alpha)
        *pixel_ctx.stroke_alpha = pixel_ctx.src_alpha + pixel_ctx.stroke_old_alpha * (1.0f - pixel_ctx.src_alpha);
    }
  }
}

static void _accumulate_preview_dab(unsigned char *buffer, const int stride, const int width, const int height,
                                    const drawlayer_dab_t *dab, const float sample_opacity_scale,
                                    float *stroke_mask, const int stroke_mask_width, const int stroke_mask_height,
                                    drawlayer_rt_path_state_t *path_state,
                                    gboolean *damage_valid, int *damage_x0,
                                    int *damage_y0, int *damage_x1, int *damage_y1)
{
  if(!buffer || !dab || dab->radius <= 0.0f || dab->opacity <= 0.0f) return;

  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  if(!_compute_analytic_dab_bounds(dab, width, height, 0, 0, 1.0f, &x0, &y0, &x1, &y1)) return;
  _union_damage_rect(damage_valid, damage_x0, damage_y0, damage_x1, damage_y1, x0, y0, x1, y1);
#if DRAWLAYER_COMPARE_ANALYTIC_TIMINGS
  const gint64 t0 = g_get_monotonic_time();
#endif

  _accumulate_preview_dab_analytic(buffer, stride, width, height, dab, sample_opacity_scale,
                                   stroke_mask, stroke_mask_width, stroke_mask_height, path_state);

#if DRAWLAYER_COMPARE_ANALYTIC_TIMINGS
  const gint64 t1 = g_get_monotonic_time();
  _log_compare_timings("preview", dab, x1 - x0, y1 - y0, t1 - t0);
#endif
}

static void _rasterize_preview_segment_window(unsigned char *buffer, const int stride, const int width, const int height,
                                              const drawlayer_dab_t *dabs, const int count, const int steps,
                                              const float first_distance, const float step_distance,
                                              const float segment_length,
                                              float *stroke_mask, const int stroke_mask_width,
                                              const int stroke_mask_height, drawlayer_rt_path_state_t *path_state,
                                              gboolean *damage_valid, int *damage_x0, int *damage_y0, int *damage_x1,
                                              int *damage_y1)
{
  if(count <= 0) return;

  if(count == 1)
  {
    if(path_state) path_state->have_smudge_pickup = FALSE;
    const float sample_step = _segment_sample_spacing(dabs, 1);
    const float sample_opacity_scale = _stroke_sample_opacity_scale(&dabs[0], sample_step);
    _accumulate_preview_dab(buffer, stride, width, height, &dabs[0], sample_opacity_scale,
                            stroke_mask, stroke_mask_width, stroke_mask_height, path_state,
                            damage_valid, damage_x0, damage_y0, damage_x1, damage_y1);
    return;
  }

  /* Same "prepare once, emit many" structure as the backend segment path:
   * derive the local interpolation geometry once, then only evaluate `t` and
   * stamp samples inside the loop. */
  drawlayer_segment_window_ctx_t segment_ctx = { 0 };
  _prepare_segment_window_ctx(dabs, count, &segment_ctx);
  const float preview_segment_length = hypotf(segment_ctx.p_end->x - segment_ctx.p_start->x,
                                              segment_ctx.p_end->y - segment_ctx.p_start->y);
  const float sample_step = (steps > 1 && step_distance > 1e-6f && segment_length > 1e-6f)
                                ? preview_segment_length * (step_distance / segment_length)
                                : preview_segment_length;
  drawlayer_dab_t previous_sample = *segment_ctx.p_start;
  for(int step = 0; step < steps; step++)
  {
    const float sample_distance = first_distance + step * step_distance;
    const float t = _clamp01(sample_distance / fmaxf(segment_length, 1e-6f));
    drawlayer_dab_t dab = _build_segment_window_sample(&segment_ctx, t);
    if(path_state)
    {
      if(dab.mode == DT_IOP_DRAWLAYER_MODE_SMUDGE)
        _advance_smudge_pickup_state(path_state, &dab, &previous_sample);
      else
        path_state->have_smudge_pickup = FALSE;
    }
    const float sample_opacity_scale = _stroke_sample_opacity_scale(&dab, sample_step);
    _accumulate_preview_dab(buffer, stride, width, height, &dab, sample_opacity_scale,
                            stroke_mask, stroke_mask_width, stroke_mask_height, path_state,
                            damage_valid, damage_x0, damage_y0, damage_x1, damage_y1);
    previous_sample = dab;
  }
}

static void _process_live_input(dt_iop_module_t *self, const drawlayer_raw_input_t *input)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;
  _process_worker_input_common(self, input, &g->preview_path, _process_live_dab);
}

static void _flush_pending_live_input(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;
  _flush_pending_initial_input(self, &g->preview_path, _process_live_dab);
}

static void _process_backend_input(dt_iop_module_t *self, const drawlayer_raw_input_t *input)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;
  _process_worker_input_common(self, input, &g->backend_path, _process_backend_dab);
}

static void _flush_pending_backend_input(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = self ? (dt_iop_drawlayer_gui_data_t *)self->gui_data : NULL;
  if(!g) return;
  _flush_pending_initial_input(self, &g->backend_path, _process_backend_dab);
}

static gboolean _prepare_worker_dab_window(GArray *history, const drawlayer_dab_t *dab,
                                           drawlayer_dab_t window[3], int *count, float *segment_length)
{
  if(!history || !dab || !window || !count || !segment_length) return FALSE;

  if(dab->stroke_pos == DRAWLAYER_STROKE_FIRST) g_array_set_size(history, 0);
  g_array_append_val(history, *dab);
  if(history->len == 0) return FALSE;

  const int total = (int)history->len;
  *count = MIN(total, 3);
  memcpy(window, ((drawlayer_dab_t *)history->data) + (total - *count), (size_t)(*count) * sizeof(drawlayer_dab_t));

  *segment_length = 0.0f;
  if(*count > 1)
  {
    const drawlayer_dab_t *p_start = &window[*count - 2];
    const drawlayer_dab_t *p_end = &window[*count - 1];
    *segment_length = hypotf(p_end->x - p_start->x, p_end->y - p_start->y);
  }

  return TRUE;
}

static void _trim_worker_dab_window(GArray *history)
{
  if(!history) return;
  if(history->len > 3) g_array_remove_range(history, 0, history->len - 3);
}

static void _process_live_dab(dt_iop_module_t *self, const drawlayer_dab_t *dab)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !g->preview_dabs || !dab || !g->live_argb || !g->live_surface) return;

  /* Preview-worker hot path for one already-resampled stroke sample:
   * 1. extend the short rolling interpolation window,
   * 2. update only the transient widget overlay,
   * 3. keep only the last 3 samples needed for the next local segment.
   *
   * The full-resolution cached layer is updated independently by the backend worker from
   * the same resampled dab stream, so live feedback stays decoupled from UI repaint work. */
  int count = 0;
  drawlayer_dab_t window[3] = { 0 };
  float segment_length = 0.0f;
  if(!_prepare_worker_dab_window(g->preview_dabs, dab, window, &count, &segment_length)) return;

  g->live_mode = window[count - 1].mode;
  drawlayer_dab_t preview_window[3] = { 0 };
  const float preview_w = fmaxf(g->preview_x1 - g->preview_x0, 1e-6f);
  const float preview_h = fmaxf(g->preview_y1 - g->preview_y0, 1e-6f);
  const int surface_w = cairo_image_surface_get_width(g->live_surface);
  const int surface_h = cairo_image_surface_get_height(g->live_surface);
  const float zoom = self->dev ? (self->dev->roi.natural_scale * self->dev->roi.scaling) : 1.0f;
  for(int i = 0; i < count; i++)
  {
    preview_window[i] = window[i];
    preview_window[i].x = ((window[i].wx - g->preview_x0) / preview_w) * surface_w;
    preview_window[i].y = ((window[i].wy - g->preview_y0) / preview_h) * surface_h;
    const float fallback_radius = fmaxf(0.5f, window[i].radius * zoom * ((float)surface_w / preview_w));
    preview_window[i].radius = _widget_brush_radius(self, &window[i], fallback_radius);
  }

  gboolean widget_damage_valid = FALSE;
  int widget_x0 = 0, widget_y0 = 0, widget_x1 = 0, widget_y1 = 0;
  _rasterize_preview_segment_window(g->live_argb, g->live_stride, surface_w, surface_h, preview_window, count,
                                    (count > 1) ? 1 : 0,
                                    fmaxf(segment_length, 1e-6f), 0.0f, fmaxf(segment_length, 1e-6f),
                                    g->preview_stroke_mask, g->preview_stroke_mask_width, g->preview_stroke_mask_height,
                                    &g->preview_path,
                                    &widget_damage_valid, &widget_x0, &widget_y0, &widget_x1, &widget_y1);

  if(widget_damage_valid)
    cairo_surface_mark_dirty_rectangle(g->live_surface, widget_x0, widget_y0, widget_x1 - widget_x0, widget_y1 - widget_y0);

  if(widget_damage_valid)
    _union_damage_rect(&g->stroke.widget_damage_valid, &g->stroke.widget_x0, &g->stroke.widget_y0,
                       &g->stroke.widget_x1, &g->stroke.widget_y1, widget_x0, widget_y0, widget_x1, widget_y1);

  g->stroke.active = g->stroke.widget_damage_valid;
  g->live_dirty = g->stroke.widget_damage_valid;
  if(g->preview_dabs->len > 3) g_array_remove_range(g->preview_dabs, 0, g->preview_dabs->len - 3);
  if(widget_damage_valid)
  {
    _rt_schedule_async_redraw(self);
  }
  _trim_worker_dab_window(g->preview_dabs);
}

static void _process_backend_dab(dt_iop_module_t *self, const drawlayer_dab_t *dab)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !g->backend_dabs || !dab) return;

  /* Backend-worker hot path for one already-resampled stroke sample:
   * 1. extend the short rolling interpolation window in layer space,
   * 2. stamp the current sample directly into the display-sized process tile,
   * 3. keep only the last 3 samples needed for the next local segment.
   *
   * The authoritative full-resolution cache is updated later by
   * `_flush_process_patch_to_base()` (rescale back to layer/raw coordinates).
   * This keeps the realtime path cheap while preserving final accuracy. */
  int count = 0;
  drawlayer_dab_t window[3] = { 0 };
  float segment_length = 0.0f;
  if(!_prepare_worker_dab_window(g->backend_dabs, dab, window, &count, &segment_length)) return;

  gboolean layer_damage_valid = FALSE;
  int layer_x0 = 0, layer_y0 = 0, layer_x1 = 0, layer_y1 = 0;
  const float safe_segment = fmaxf(segment_length, 1e-6f);
  gboolean wrote_process_patch = FALSE;
  if(g->process_patch_valid && g->process_patch.pixels && g->process_patch.width > 0 && g->process_patch.height > 0
     && g->process_combined_roi.scale > 1e-6f)
  {
    wrote_process_patch = TRUE;
    _rasterize_segment_window(g->process_patch.pixels, g->process_patch.width, g->process_patch.height,
                              g->process_combined_roi.x, g->process_combined_roi.y, g->process_combined_roi.scale,
                              window, count, (count > 1) ? 1 : 0, safe_segment, 0.0f, safe_segment,
                              g->process_stroke_mask, g->process_stroke_mask_width, g->process_stroke_mask_height,
                              &g->process_path,
                              &layer_damage_valid, &layer_x0, &layer_y0, &layer_x1, &layer_y1);
    if(layer_damage_valid)
      dt_dev_pixelpipe_cache_flush_host_pinned_image(darktable.pixelpipe_cache, g->process_patch.pixels, NULL, -1);
  }
  else if(g->base_patch.pixels)
  {
    /* Fallback for rare cases where the process tile has not been initialized
     * yet (for example a backend event arriving before the first process pass). */
    if(g->base_patch.cache_entry)
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, TRUE,
                                          g->base_patch.cache_entry);
    _rasterize_segment_window(g->base_patch.pixels, g->base_patch.width, g->base_patch.height, 0, 0, 1.0f,
                              window, count, (count > 1) ? 1 : 0, safe_segment, 0.0f, safe_segment,
                              g->stroke_mask, g->stroke_mask_width, g->stroke_mask_height,
                              &g->backend_path,
                              &layer_damage_valid, &layer_x0, &layer_y0, &layer_x1, &layer_y1);
    if(g->base_patch.cache_entry)
      dt_dev_pixelpipe_cache_wrlock_entry(darktable.pixelpipe_cache, g->base_patch.cache_hash, FALSE,
                                          g->base_patch.cache_entry);
  }

  if(layer_damage_valid)
  {
    g->cache_dirty = TRUE;
    int base_x0 = layer_x0;
    int base_y0 = layer_y0;
    int base_x1 = layer_x1;
    int base_y1 = layer_y1;
    if(wrote_process_patch && g->process_combined_roi.scale > 1e-6f)
    {
      const float inv_scale = 1.0f / g->process_combined_roi.scale;
      base_x0 = MAX((int)floorf((layer_x0 + g->process_combined_roi.x) * inv_scale), 0);
      base_y0 = MAX((int)floorf((layer_y0 + g->process_combined_roi.y) * inv_scale), 0);
      base_x1 = MIN((int)ceilf((layer_x1 + g->process_combined_roi.x) * inv_scale), g->base_patch.width);
      base_y1 = MIN((int)ceilf((layer_y1 + g->process_combined_roi.y) * inv_scale), g->base_patch.height);
      g->process_patch_dirty = TRUE;
      _union_damage_rect(&g->process_dirty_bounds_valid, &g->process_dirty_x0, &g->process_dirty_y0,
                         &g->process_dirty_x1, &g->process_dirty_y1, base_x0, base_y0, base_x1, base_y1);
    }
    _union_damage_rect(&g->stroke.layer_damage_valid, &g->stroke.layer_x0, &g->stroke.layer_y0,
                       &g->stroke.layer_x1, &g->stroke.layer_y1, base_x0, base_y0, base_x1, base_y1);
  }

  _trim_worker_dab_window(g->backend_dabs);
}

static gboolean _layer_bounds_to_widget_bounds(dt_iop_module_t *self, const float x0, const float y0,
                                               const float x1, const float y1,
                                               float *left, float *top, float *right, float *bottom)
{
  if(!self || !self->dev || !self->dev->virtual_pipe) return FALSE;

  float pts[8] = {
    x0, y0,
    x1, y0,
    x0, y1,
    x1, y1,
  };

  if(!dt_dev_distort_transform_plus(self->dev, self->dev->virtual_pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 4))
    return FALSE;

  dt_dev_coordinates_image_abs_to_image_norm(self->dev, pts, 4);
  dt_dev_coordinates_image_norm_to_widget(self->dev, pts, 4);

  float min_x = pts[0];
  float max_x = pts[0];
  float min_y = pts[1];
  float max_y = pts[1];
  for(int i = 1; i < 4; i++)
  {
    min_x = fminf(min_x, pts[2 * i]);
    max_x = fmaxf(max_x, pts[2 * i]);
    min_y = fminf(min_y, pts[2 * i + 1]);
    max_y = fmaxf(max_y, pts[2 * i + 1]);
  }

  if(left) *left = min_x;
  if(top) *top = min_y;
  if(right) *right = max_x;
  if(bottom) *bottom = max_y;
  return TRUE;
}

static void _paint_temp_buffer(dt_iop_module_t *self, cairo_t *cr, const int width, const int height)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  if(!g || !self->dev || !g->stroke.widget_damage_valid || !g->live_surface) return;

  const float draw_w = fmaxf(g->preview_x1 - g->preview_x0, 1e-6f);
  const float draw_h = fmaxf(g->preview_y1 - g->preview_y0, 1e-6f);
  const int surface_w = cairo_image_surface_get_width(g->live_surface);
  const int surface_h = cairo_image_surface_get_height(g->live_surface);
  if(surface_w <= 0 || surface_h <= 0) return;

  (void)width;
  (void)height;

  cairo_save(cr);
  cairo_translate(cr, g->preview_x0, g->preview_y0);
  cairo_scale(cr, draw_w / (float)surface_w, draw_h / (float)surface_h);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  cairo_set_source_surface(cr, g->live_surface, 0.0, 0.0);
  cairo_paint(cr);
  cairo_restore(cr);
}
