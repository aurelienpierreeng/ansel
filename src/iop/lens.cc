/*
    This file is part of darktable,
    Copyright (C) 2009-2013, 2016 johannes hanika.
    Copyright (C) 2010 Alexandre Prokoudine.
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2011, 2013 Henrik Andersson.
    Copyright (C) 2010 Milan Knížek.
    Copyright (C) 2010, 2013-2014 Pascal de Bruijn.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2010 Thierry Leconte.
    Copyright (C) 2011, 2013 Antony Dovgal.
    Copyright (C) 2011-2012 Jérémy Rosen.
    Copyright (C) 2011 Olivier Tribout.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2014, 2016-2019 Tobias Ellinghaus.
    Copyright (C) 2012 Edouard Gomez.
    Copyright (C) 2012-2013 Gabriel Ebner.
    Copyright (C) 2012, 2015, 2019 parafin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Sergey Pavlov.
    Copyright (C) 2012-2014, 2016-2017 Ulrich Pegelow.
    Copyright (C) 2013, 2020-2021 Aldric Renaudin.
    Copyright (C) 2013 Guilherme Brondani Torri.
    Copyright (C) 2013 Ivan Tarozzi.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2013-2015 Torsten Bronger.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2016, 2018-2022 Pascal Obry.
    Copyright (C) 2017 Heiko Bauke.
    Copyright (C) 2018-2026 Aurélien PIERRE.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2018 Kelvie Wong.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 Peter Budai.
    Copyright (C) 2018, 2021 rawfiner.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019 David-Tillmann Schaefer.
    Copyright (C) 2019 Diederik ter Rahe.
    Copyright (C) 2019 Jakub Filipowicz.
    Copyright (C) 2019 Kevin Daudt.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020-2022 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 fvollmer.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Nicolas Auffray.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2024-2025 Alynx Zhou.
    
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
#include "common/darktable.h"
#include "glib.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/exif.h"
#include "common/file_location.h"
#include "common/imagebuf.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"

#include "gui/draw.h"
#include "gui/gtk.h"
#include "embedded_lens/embedded_lens.h"
#include "lens_predicates.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>
#include <lensfun.h>

extern "C" {

#if LF_VERSION < ((0 << 24) | (2 << 16) | (9 << 8) | 0)
#define LF_SEARCH_SORT_AND_UNIQUIFY 2
#endif

#if LF_VERSION == ((0 << 24) | (3 << 16) | (95 << 8) | 0)
#define LF_0395
#endif

  DT_MODULE_INTROSPECTION(7, dt_iop_lensfun_params_t)

  enum class dt_iop_lens_method_t
  {
    LENSFUN = 0,
    EMBEDDED_METADATA = 1
  };

  typedef struct dt_iop_lensfun_params_t
  {
    int modify_flags;
    float scale;
    float crop;
    float focal;
    float aperture;
    float distance;
    lfLensType target_geom;
    char camera[128];    // NOSONAR
    char lens[128];      // NOSONAR
    float tca_r;
    float tca_b;
    int has_been_set;
    dt_iop_lens_correction_source_t vignetting_method;
    dt_iop_lens_correction_source_t distortion_method;
    dt_iop_lens_tca_source_t tca_method;
  } dt_iop_lensfun_params_t;

static_assert(sizeof(dt_iop_lensfun_params_t) == 308,
              "params_t v7 size changed -- struct-split integrity failure");
static_assert(offsetof(dt_iop_lensfun_params_t, vignetting_method) < sizeof(dt_iop_lensfun_params_t),
              "vignetting_method must be present in v7 params");
static_assert(offsetof(dt_iop_lensfun_params_t, distortion_method) < sizeof(dt_iop_lensfun_params_t),
              "distortion_method must be present in v7 params");
static_assert(offsetof(dt_iop_lensfun_params_t, tca_method) < sizeof(dt_iop_lensfun_params_t),
              "tca_method must be present in v7 params");

typedef struct dt_iop_lensfun_gui_data_t
{
  struct
  {
    GtkWidget *target_geom, *tca_r, *tca_b, *scale;
  } lensfun_controls;
  struct
  {
    GtkWidget *distortion_source;
    GtkWidget *vignetting_source;
    GtkWidget *tca_source;
  } per_correction;
  struct
  {
    const lfCamera *camera;
    GtkWidget *lens_param_box;
    GtkWidget *camera_model;
    GtkWidget *lens_model;
    GtkMenu *camera_menu;
    GtkMenu *lens_menu;
    GtkWidget *cbe[3];
    GtkWidget *find_lens_button;
    GtkWidget *find_camera_button;
  } lens_selection;
  struct
  {
    GtkLabel *message;
    int corrections_done;
    gboolean trouble;
  } status;
} dt_iop_lensfun_gui_data_t;

static_assert(sizeof(dt_iop_lensfun_gui_data_t) == 160,
              "gui_data_t v7 size changed -- struct-split integrity failure");

typedef struct dt_iop_lensfun_global_data_t
{
  lfDatabase *db;
  int kernel_lens_distort_bilinear;
  int kernel_lens_distort_bicubic;
  int kernel_lens_distort_mitchell;
  int kernel_lens_vignette;
  int kernel_md_vignette;
  int kernel_md_lens_correction;
} dt_iop_lensfun_global_data_t;



typedef struct dt_iop_lensfun_data_t
{
  struct
  {
    lfLens *lens;
    int modify_flags;
    float scale;
    float crop;
    float focal;
    float aperture;
    float distance;
    lfLensType target_geom;
    gboolean do_nan_checks;
    lfLensCalibTCA custom_tca;
  } lensfun;
  struct
  {
    int nc;
    dt_embedded_lens_knots_t knots;
  } embedded;
} dt_iop_lensfun_data_t;

static_assert(sizeof(dt_iop_lensfun_data_t) == 464,
              "data_t v7 size changed -- struct-split integrity failure");


const char *name()
{
  return _("_lens correction");
}

const char *aliases()
{
  return _("vignette|chromatic aberrations|distortion");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("correct lenses optical flaws"),
                                      _("corrective"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric and reconstruction, RGB"),
                                      _("linear, RGB, scene-referred"));
}


int default_group()
{
  return IOP_GROUP_REPAIR;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_UNSAFE_COPY;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(!old_params || !new_params) return 1;

  if(old_version == 2 && new_version == 7)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[52];
      char lens[52];
      int tca_override;
      float tca_r;
      float tca_b;
    } dt_iop_lensfun_params_v2_t;

    const dt_iop_lensfun_params_v2_t *o = (dt_iop_lensfun_params_v2_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d;

    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = 0;

    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }
  if(old_version == 3 && new_version == 7)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r;
      float tca_b;
    } dt_iop_lensfun_params_v3_t;

    const dt_iop_lensfun_params_v3_t *o = (dt_iop_lensfun_params_v3_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d;

    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = 0;

    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }

  if(old_version == 4 && new_version == 7)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r;
      float tca_b;
      int modified;
    } dt_iop_lensfun_params_v4_t;

    const dt_iop_lensfun_params_v4_t *o = (dt_iop_lensfun_params_v4_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d;

    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = o->modified ? 0 : 1;

    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }

  if(old_version == 5 && new_version == 7)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[128];
      char lens[128];
      gboolean tca_override;
      float tca_r;
      float tca_b;
      int modified;
    } dt_iop_lensfun_params_v5_t;

    const dt_iop_lensfun_params_v5_t *o = (dt_iop_lensfun_params_v5_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d;

    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = o->modified ? 0 : 1;

    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }

  if(old_version == 6 && new_version == 7)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      lfLensType target_geom;
      char camera[128];
      char lens[128];
      gboolean tca_override;
      float tca_r;
      float tca_b;
      int has_been_set;
      dt_iop_lens_method_t method;
      float cor_dist_ft;
      float cor_vig_ft;
      float cor_ca_r_ft;
      float cor_ca_b_ft;
      float scale_md;
    } dt_iop_lensfun_params_v6_t;

    const dt_iop_lensfun_params_v6_t *o = (dt_iop_lensfun_params_v6_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d;

    n->modify_flags = o->modify_flags;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;
    n->has_been_set = o->has_been_set;

    if(o->method == dt_iop_lens_method_t::EMBEDDED_METADATA)
    {
      n->vignetting_method = dt_iop_lens_correction_source_t::EMBEDDED;
      n->distortion_method  = dt_iop_lens_correction_source_t::EMBEDDED;
      n->tca_method         = dt_iop_lens_tca_source_t::OFF;
    }
    else
    {
      n->vignetting_method = dt_iop_lens_correction_source_t::LENSFUN_DB;
      n->distortion_method  = dt_iop_lens_correction_source_t::LENSFUN_DB;
      n->tca_method         = dt_iop_lens_tca_source_t::LENSFUN_DB;
    }

    return 0;
  }

  return 1;
}

static char *_lens_sanitize(const char *orig_lens)
{
  const char *found_or = strstr(orig_lens, " or ");
  const char *found_parenthesis = strstr(orig_lens, " (");

  if(found_or || found_parenthesis)
  {
    size_t pos_or = (size_t)(found_or - orig_lens);
    size_t pos_parenthesis = (size_t)(found_parenthesis - orig_lens);
    size_t pos = pos_or < pos_parenthesis ? pos_or : pos_parenthesis;

    if(pos > 0)
    {
      char *new_lens = (char *)malloc(pos + 1);

      strncpy(new_lens, orig_lens, pos);
      new_lens[pos] = '\0';

      return new_lens;
    }
    else
    {
      char *new_lens = strdup(orig_lens);
      return new_lens;
    }
  }
  else
  {
    char *new_lens = strdup(orig_lens);
    return new_lens;
  }
}

__DT_CLONE_TARGETS__
static lfModifier * get_modifier(int *mods_done, int w, int h, const dt_iop_lensfun_data_t *d, int mods_filter)
{
  lfModifier *mod;
  int mods_todo = d->lensfun.modify_flags & mods_filter;
  int mods_done_tmp = 0;

#ifdef LF_0395
  mod = new lfModifier(d->lensfun.crop, w, h, LF_PF_F32, FALSE);
  if(mods_todo & LF_MODIFY_DISTORTION)
    mods_done_tmp |= mod->EnableDistortionCorrection(d->lensfun.lens, d->lensfun.focal);
  if((mods_todo & LF_MODIFY_GEOMETRY) && (d->lensfun.lens->Type != d->lensfun.target_geom))
    mods_done_tmp |= mod->EnableProjectionTransform(d->lensfun.lens, d->lensfun.focal, d->lensfun.target_geom);
  if((mods_todo & LF_MODIFY_SCALE) && (d->lensfun.scale != 1.0))
    mods_done_tmp |= mod->EnableScaling(d->lensfun.scale);
  if(mods_todo & LF_MODIFY_TCA)
  {
    if(d->lensfun.custom_tca.Model != LF_TCA_MODEL_NONE)
      mods_done_tmp |= mod->EnableTCACorrection(d->lensfun.custom_tca);
    else
      mods_done_tmp |= mod->EnableTCACorrection(d->lensfun.lens, d->lensfun.focal);
  }
  if(mods_todo & LF_MODIFY_VIGNETTING)
    mods_done_tmp |= mod->EnableVignettingCorrection(d->lensfun.lens, d->lensfun.focal, d->lensfun.aperture, d->lensfun.distance);
#else
  mod = new lfModifier(d->lensfun.lens, d->lensfun.crop, w, h);
  mods_done_tmp = mod->Initialize(d->lensfun.lens, LF_PF_F32, d->lensfun.focal, d->lensfun.aperture, d->lensfun.distance, d->lensfun.scale, d->lensfun.target_geom, mods_todo,
                                  FALSE);
#endif

  if(mods_done) *mods_done = mods_done_tmp;
  return mod;
}

static inline void _lens_fill_vignette_row(float *const buf, const int width, const int ch)
{
  if(ch == DT_PIXEL_SIMD_CHANNELS)
  {
    const dt_aligned_pixel_simd_t half = dt_simd_set1(0.5f);
    for(int x = 0; x < width; x++) dt_store_simd_aligned(buf + (size_t)x * ch, half);
  }
  else
  {
    for(int k = 0; k < ch * width; k++) buf[k] = 0.5f;
  }
}

/* Why do we care about being a monochrome image or not?
 The lensfun library does not have an algorithm for distortion or tca correction specialized for monochrome images,
   the builtin correction works with subtle differences for the color channels leading to some colorizing of the images.
 How is this fixed here:
   Monochrome images (from pure monochrome cameras or cameras with the color filter removed from the sensor) have
   all three rgb colors set to the same value by the demosaicer.
   Looking through lensfun code & docs the ApplySubpixelGeometryDistortion algorithm makes assumptions from given
   coeffs how far data are displaced for the different wavelengths of light.
   As green / Y channel is the most centric i took that as the canonical value instead of taking the mean.
*/

// ---------------------------------------------------------------------------------------
// Embedded-metadata correction math.
//
// Per-correction routing: every geometric entry point reads its three per-correction
// enums from self->params and derives which axes are routed to the embedded knot table
// and which to the Lensfun lfModifier. Mixed-mode (e.g. distortion=EMBEDDED + vignetting=
// LENSFUN_DB) is supported — each axis dispatches to its own source independently.
//
// The vendor union (exif_correction_data.{sony,fuji,dng,olympus}) is read EXACTLY ONCE, in
// commit_params(), through dt_embedded_lens_init_coeffs(). That switch normalizes whichever
// vendor's native format the enum selects into a common LENS_MAXKNOTS-sized linear-spline
// knot table cached in piece->data (d->embedded.nc, d->embedded.knots.knots_dist,
// d->embedded.knots.knots_vig, d->embedded.knots.cor_rgb, d->embedded.knots.vig).
// Every dispatch helper below reads ONLY that knot table -- none of them touch
// self->dev->image_storage.exif_correction_{type,data} again.
//
// Coordinate convention: distortion/TCA/vignetting are all evaluated on a single normalized
// radius r = hypot(dx, dy) / hypot(w/2, h/2) centred on the image centre (dx, dy measured
// from that centre in absolute image-pixel space), where r == 1 at the farthest image
// corner. This is the same convention upstream's own knot-table consumption uses.
//
// cor_rgb[c][] stores the multiplicative radius ratio dr = (input radius)/(output radius)
// for RGB channel c (alpha reuses the canonical/green curve, cor_rgb[1]) -- backtransforming
// an output-space point (cx, cy) relative to the image centre samples
// (dr*cx + w2, dr*cy + h2) in input space. vig[] stores the vignetting map value; a pixel is
// corrected by DIVIDING by vig (matching upstream's own convention), so vig < 1 in a
// periphery that needs brightening.
//
// cor_dist_ft/cor_vig_ft/cor_ca_r_ft/cor_ca_b_ft (per-class fine-tune multipliers, default
// 1.0) blend every class linearly between "no correction" (0.0) and "full correction"
// (1.0), with allowance for over/under-shoot at 1.5/0.5 -- baked directly into the knot
// values at normalize time (once per commit) rather than at every pixel/point; this is
// mathematically equivalent because both the blend and the spline evaluation are affine in
// the underlying correction value.
// ---------------------------------------------------------------------------------------



static void _report_corrections_done(dt_iop_module_t *self, int modify_flags)
{
  if(!self->dev || !self->dev->gui_attached) return;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  if(!g) return;
  dt_iop_gui_enter_critical_section(self);
  g->status.corrections_done = modify_flags;
  dt_iop_gui_leave_critical_section(self);
}

typedef struct {
  gboolean apply_vignette;
  gboolean apply_distortion;
  gboolean apply_tca;
} _emb_axes_t;

static void _apply_vignette_gain_3ch(float *const work_pixel, const float *const in_pixel,
                                       const int ch,
                                       const float *const knots_vig, const float *const vig,
                                       const int nc, const float radius)
{
  const float sf = dt_embedded_lens_linear_spline(knots_vig, vig, nc, radius);
  const float gain = 1.0f / fmaxf(sf, 1e-4f);
  for(int c = 0; c < 3 && c < ch; c++) work_pixel[c] = in_pixel[c] * gain;
  for(int c = 3; c < ch; c++) work_pixel[c] = in_pixel[c];
}

typedef struct {
  float cx;
  float cy;
  float w2;
  float h2;
  float roi_x;
  float roi_y;
  float limw;
  float limh;
} _geom_ctx_t;

typedef struct {
  const float *knots_dist;
  const float *cor_rgb;
  int nc;
} _spline_args_t;

static void _compute_geometric_displacement_3ch(const _spline_args_t *spl,
                                                 const float radius,
                                                 const gboolean apply_dist,
                                                 const _geom_ctx_t *geom,
                                                 float *const sx, float *const sy)
{
  const float dr = apply_dist
      ? dt_embedded_lens_linear_spline(spl->knots_dist, spl->cor_rgb, spl->nc, radius)
      : 1.0f;
  *sx = CLAMP(dr * geom->cx + geom->w2 - geom->roi_x, 0.0f, geom->limw);
  *sy = CLAMP(dr * geom->cy + geom->h2 - geom->roi_y, 0.0f, geom->limh);
}

typedef struct {
  int ch;
  int ch_width;
  float w2;
  float h2;
  float inv_rn;
  float limw;
  float limh;
  gboolean raw_monochrome;
  gboolean apply_dist;
  gboolean apply_tca;
} _warp_geom_domain_t;

static inline int _select_warp_plane(const int c, const _warp_geom_domain_t *dom)
{
  if(dom->apply_dist && !dom->apply_tca) return 1;
  if(c < 3 && !dom->raw_monochrome) return c;
  return 1;
}

static void _warp_geom_pass(const float *work, float *ovoid,
                            const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                            const dt_iop_lensfun_data_t *d,
                            const struct dt_interpolation *interpolation,
                            const _warp_geom_domain_t *dom)
{
  __OMP_PARALLEL_FOR_CPP__(firstprivate(work, ovoid, roi_in, roi_out, d, interpolation, dom))
  for(int y = 0; y < roi_out->height; y++)
  {
    float *const out_row = ovoid + (size_t)y * roi_out->width * dom->ch;
    for(int x = 0; x < roi_out->width; x++)
    {
      const float cx = roi_out->x + x - dom->w2;
      const float cy = roi_out->y + y - dom->h2;
      const float radius = hypotf(cx, cy) * dom->inv_rn;
      for(int c = 0; c < dom->ch; c++)
      {
        const int plane = _select_warp_plane(c, dom);
        float sx;
        float sy;
        const _geom_ctx_t geom = { cx, cy, dom->w2, dom->h2,
                                   (float)roi_in->x, (float)roi_in->y,
                                   dom->limw, dom->limh };
        const _spline_args_t spl = { d->embedded.knots.knots_dist,
                                      d->embedded.knots.cor_rgb[plane],
                                      d->embedded.nc };
        _compute_geometric_displacement_3ch(&spl, radius, dom->apply_dist,
                                             &geom, &sx, &sy);
        out_row[x * dom->ch + c] = dt_interpolation_compute_sample(interpolation, work + c, sx, sy,
                                                                    roi_in->width, roi_in->height,
                                                                    dom->ch, dom->ch_width);
      }
    }
  }
}

static int _process_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                            const dt_dev_pixelpipe_iop_t *piece, const float *const ivoid,
                                            float *const ovoid,
                                            const _emb_axes_t *emb_axes)
{
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const int ch = piece->dsc_in.channels;

  if(!d->embedded.nc)
  {
    dt_iop_image_copy_by_size(ovoid, ivoid,
                                roi_out->width, roi_out->height, ch);
    _report_corrections_done(self, d->lensfun.modify_flags);
    return 0;
  }

  const float w2 = 0.5f * roi_in->scale * piece->buf_in.width;
  const float h2 = 0.5f * roi_in->scale * piece->buf_in.height;
  const float rn = hypotf(w2, h2);
  const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;

  const gboolean apply_vignette = emb_axes ? emb_axes->apply_vignette : FALSE;
  const gboolean apply_dist = emb_axes ? emb_axes->apply_distortion : FALSE;
  const gboolean apply_tca = emb_axes ? emb_axes->apply_tca : FALSE;

  const size_t n_pixels = (size_t)roi_in->width * roi_in->height * ch;
  float *const work = dt_alloc_align_float(n_pixels);
  if(IS_NULL_PTR(work)) return 1;

  if(apply_vignette)
  {
    __OMP_PARALLEL_FOR_CPP__(firstprivate(work, ivoid, roi_in, ch, w2, h2, inv_rn, d))
    for(int y = 0; y < roi_in->height; y++)
    {
      const float *const in_row = ivoid + (size_t)y * roi_in->width * ch;
      float *const work_row = work + (size_t)y * roi_in->width * ch;
      for(int x = 0; x < roi_in->width; x++)
      {
        const float cx = roi_in->x + x - w2;
        const float cy = roi_in->y + y - h2;
        const float radius = hypotf(cx, cy) * inv_rn;
        _apply_vignette_gain_3ch(work_row + x * ch, in_row + x * ch, ch,
                                  d->embedded.knots.knots_vig, d->embedded.knots.vig,
                                  d->embedded.nc, radius);
      }
    }
  }
  else
  {
    // Identity vignette: pass pixels through unchanged.
    dt_iop_image_copy_by_size(work, ivoid,
                                roi_in->width, roi_in->height, ch);
  }

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_MITCHELL);
  const int ch_width = ch * roi_in->width;
  const float limw = (float)roi_in->width - 1.0f;
  const float limh = (float)roi_in->height - 1.0f;
  // Monochrome forces every channel (including alpha) onto the canonical (green)
  // curve -- TCA is a no-op regardless of what cor_rgb[0]/cor_rgb[2] carry.
  const gboolean raw_monochrome = self->dev ? dt_image_is_monochrome(&self->dev->image_storage) : FALSE;

  if(!apply_dist && !apply_tca)
  {
    dt_iop_image_copy_by_size(ovoid, work, roi_out->width, roi_out->height, ch);
  }
  else
  {
    const _warp_geom_domain_t dom = {
      ch, ch_width, w2, h2, inv_rn, limw, limh,
      raw_monochrome, apply_dist, apply_tca
    };
    _warp_geom_pass(work, ovoid, roi_in, roi_out, d, interpolation, &dom);
  }

  dt_free_align(work);
  _report_corrections_done(self, d->lensfun.modify_flags);
  return 0;
}

static int _distort_transform_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                                      const dt_dev_pixelpipe_iop_t *piece,
                                                      float *const __restrict points, size_t points_count,
                                                      const _emb_axes_t *emb_axes)
{
  (void)self;
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  const gboolean apply_geom = emb_axes && (emb_axes->apply_distortion || emb_axes->apply_tca);
  if(!d->embedded.nc || !apply_geom) return 1;

  const float w2 = 0.5f * piece->buf_in.width;
  const float h2 = 0.5f * piece->buf_in.height;
  const float rn = hypotf(w2, h2);
  const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;

  // Forward (input -> output) mapping via fixed-point iteration, mirroring upstream's own
  // _distort_transform_md (the backtransform below is direct; this direction has no closed
  // form since the spline is only known in output-radius space).
  __OMP_PARALLEL_FOR_CPP__(firstprivate(points, points_count, d, w2, h2, inv_rn) if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    float p1 = points[i];
    float p2 = points[i + 1];
    for(int iter = 0; iter < 10; iter++)
    {
      const float cx = p1 - w2;
      const float cy = p2 - h2;
      const float dr = dt_embedded_lens_linear_spline(d->embedded.knots.knots_dist, d->embedded.knots.cor_rgb[1], d->embedded.nc, hypotf(cx, cy) * inv_rn);
      const float dist1 = points[i] - (dr * cx + w2);
      const float dist2 = points[i + 1] - (dr * cy + h2);
      if(fabsf(dist1) < 0.5f && fabsf(dist2) < 0.5f) break;
      p1 += dist1;
      p2 += dist2;
    }
    points[i] = p1;
    points[i + 1] = p2;
  }
  return 1;
}

static int _distort_backtransform_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                                          const dt_dev_pixelpipe_iop_t *piece,
                                                          float *const __restrict points, size_t points_count,
                                                          const _emb_axes_t *emb_axes)
{
  (void)self;
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  const gboolean apply_geom = emb_axes && (emb_axes->apply_distortion || emb_axes->apply_tca);
  if(!d->embedded.nc || !apply_geom) return 1;

  const float w2 = 0.5f * piece->buf_in.width;
  const float h2 = 0.5f * piece->buf_in.height;
  const float rn = hypotf(w2, h2);
  const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;

  __OMP_PARALLEL_FOR_CPP__(firstprivate(points, points_count, d, w2, h2, inv_rn) if(points_count > 100))
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    const float cx = points[i] - w2;
    const float cy = points[i + 1] - h2;
    const float dr = dt_embedded_lens_linear_spline(d->embedded.knots.knots_dist, d->embedded.knots.cor_rgb[1], d->embedded.nc, hypotf(cx, cy) * inv_rn);
    points[i] = dr * cx + w2;
    points[i + 1] = dr * cy + h2;
  }
  return 1;
}

static void _distort_mask_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                                  dt_dev_pixelpipe_iop_t *piece, const float *const in,
                                                  float *const out, const dt_iop_roi_t *const roi_in,
                                                  const dt_iop_roi_t *const roi_out,
                                                  const _emb_axes_t *emb_axes)
{
  (void)self;
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  const gboolean apply_geom = emb_axes && (emb_axes->apply_distortion || emb_axes->apply_tca);

  if(!d->embedded.nc || !apply_geom)
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const float w2 = 0.5f * roi_in->scale * piece->buf_in.width;
  const float h2 = 0.5f * roi_in->scale * piece->buf_in.height;
  const float rn = hypotf(w2, h2);
  const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_MITCHELL);
  const float limw = (float)roi_in->width - 1.0f;
  const float limh = (float)roi_in->height - 1.0f;

  __OMP_PARALLEL_FOR_CPP__(firstprivate(in, out, roi_in, roi_out, d, w2, h2, inv_rn, interpolation, limw, limh))
  for(int y = 0; y < roi_out->height; y++)
  {
    float *const out_row = out + (size_t)y * roi_out->width;
    for(int x = 0; x < roi_out->width; x++)
    {
      const float cx = roi_out->x + x - w2;
      const float cy = roi_out->y + y - h2;
      // Masks carry no colour channel of their own: always sample the canonical (green)
      // curve.
      const float dr = dt_embedded_lens_linear_spline(d->embedded.knots.knots_dist, d->embedded.knots.cor_rgb[1], d->embedded.nc, hypotf(cx, cy) * inv_rn);
      const float sx = CLAMP(dr * cx + w2 - roi_in->x, 0.0f, limw);
      const float sy = CLAMP(dr * cy + h2 - roi_in->y, 0.0f, limh);
      out_row[x] = dt_interpolation_compute_sample(interpolation, in, sx, sy, roi_in->width, roi_in->height, 1,
                                                   roi_in->width);
    }
  }
}

typedef struct {
  float px;
  float py;
  float w2;
  float h2;
  float inv_rn;
} _roi_point_t;

typedef struct {
  float xm;
  float xM;
  float ym;
  float yM;
} _roi_bounds_t;

static void _sweep_embedded_roi_point(_roi_point_t p,
                                      const dt_iop_lensfun_data_t *d,
                                      _roi_bounds_t *b)
{
  const float cx = p.px - p.w2;
  const float cy = p.py - p.h2;
  const float radius = hypotf(cx, cy) * p.inv_rn;
  for(int c : {0, 1, 2})
  {
    const float dr = dt_embedded_lens_linear_spline(d->embedded.knots.knots_dist, d->embedded.knots.cor_rgb[c], d->embedded.nc, radius);
    const float sx = dr * cx + p.w2;
    const float sy = dr * cy + p.h2;
    if(isfinite(sx))
    {
      b->xm = fminf(b->xm, sx);
      b->xM = fmaxf(b->xM, sx);
    }
    if(isfinite(sy))
    {
      b->ym = fminf(b->ym, sy);
      b->yM = fmaxf(b->yM, sy);
    }
  }
}

static void _modify_roi_in_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                                   const dt_dev_pixelpipe_iop_t *piece,
                                                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in,
                                                   const _emb_axes_t *emb_axes)
{
  (void)self;
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  const gboolean apply_geom = emb_axes && (emb_axes->apply_distortion || emb_axes->apply_tca);

  if(!d->embedded.nc || !apply_geom) return;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  const float w2 = 0.5f * orig_w;
  const float h2 = 0.5f * orig_h;
  const float rn = hypotf(w2, h2);
  const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;

  const int xoff = roi_out->x;
  const int yoff = roi_out->y;
  const int width = roi_out->width;
  const int height = roi_out->height;

  _roi_bounds_t b = { FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX };

  for(int i = 0; i < width; i++)
  {
    _sweep_embedded_roi_point((_roi_point_t){ (float)(xoff + i), (float)yoff, w2, h2, inv_rn }, d, &b);
    _sweep_embedded_roi_point((_roi_point_t){ (float)(xoff + i), (float)(yoff + height - 1), w2, h2, inv_rn }, d, &b);
  }
  for(int j = 0; j < height; j++)
  {
    _sweep_embedded_roi_point((_roi_point_t){ (float)xoff, (float)(yoff + j), w2, h2, inv_rn }, d, &b);
    _sweep_embedded_roi_point((_roi_point_t){ (float)(xoff + width - 1), (float)(yoff + j), w2, h2, inv_rn }, d, &b);
  }

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_MITCHELL);

  if(!isfinite(b.xm) || b.xm < 0 || b.xm >= orig_w) b.xm = 0;
  if(!isfinite(b.xM) || b.xM < 0 || b.xM >= orig_w) b.xM = orig_w - 1;
  if(!isfinite(b.ym) || b.ym < 0 || b.ym >= orig_h) b.ym = 0;
  if(!isfinite(b.yM) || b.yM < 0 || b.yM >= orig_h) b.yM = orig_h - 1;

  roi_in->x = (int)fmaxf(0.0f, floorf(b.xm - interpolation->width));
  roi_in->y = (int)fmaxf(0.0f, floorf(b.ym - interpolation->width));
  roi_in->width = (int)ceilf(fminf(orig_w - roi_in->x, b.xM - roi_in->x + interpolation->width));
  roi_in->height = (int)ceilf(fminf(orig_h - roi_in->y, b.yM - roi_in->y + interpolation->width));

  roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(orig_w));
  roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(orig_h));
  roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(orig_w) - roi_in->x);
  roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(orig_h) - roi_in->y);
}

static void _modify_roi_out_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                                     const dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                                                    const dt_iop_roi_t *roi_in,
                                                    const _emb_axes_t *emb_axes)
{
  // WarpRectilinear-style corrections are a same-extent remap (no output-size change),
  // unlike Lensfun's target-geometry scale/crop: the corrected image always covers the
  // same canvas as the input, so this stays an identity pass-through. Kept as its own
  // delegate point for symmetry with the other geometric entry points.
  (void)self;
  (void)pipe;
  (void)piece;
  (void)emb_axes;
  *roi_out = *roi_in;
}

typedef struct {
  int ch;
  int ch_width;
  int do_nan_checks;
  int raw_monochrome;
  int mask_display;
} _remap_ctx_t;

static void _remap_pixel_inverse_or_not(float *out, const float *bufptr, const float *src,
                                         const _remap_ctx_t remap,
                                         const dt_iop_roi_t *roi_in,
                                         const struct dt_interpolation *interpolation)
{
  dt_aligned_pixel_simd_t pixel = { 0.f };
  for(int c = 0; c < 3; c++)
  {
    if(remap.do_nan_checks && (!isfinite(bufptr[c * 2]) || !isfinite(bufptr[c * 2 + 1])))
    {
      pixel[c] = 0.0f;
      continue;
    }
    const float *inptr = src + (size_t)c;
    const float pi0 = fmaxf(fminf(bufptr[c * 2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
    const float pi1 = fmaxf(fminf(bufptr[c * 2 + 1] - roi_in->y, roi_in->height - 1.0f), 0.0f);
    pixel[c] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                                roi_in->height, remap.ch, remap.ch_width);
  }
  if(remap.raw_monochrome) { pixel[2] = pixel[1]; pixel[0] = pixel[1]; }
  if(remap.mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
  {
    if(remap.do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
    {
      pixel[3] = 0.0f;
    }
    else
    {
      const float *inptr = src + (size_t)3;
      const float pi0 = fmaxf(fminf(bufptr[2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
      const float pi1 = fmaxf(fminf(bufptr[3] - roi_in->y, roi_in->height - 1.0f), 0.0f);
      pixel[3] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                                  roi_in->height, remap.ch, remap.ch_width);
    }
    if(remap.ch == DT_PIXEL_SIMD_CHANNELS) dt_store_simd_aligned(out, pixel);
    else for(int c = 0; c < remap.ch; c++) out[c] = pixel[c];
  }
  else
  {
    for(int c = 0; c < 3; c++) out[c] = pixel[c];
  }
}

__DT_CLONE_TARGETS__
int process(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  const int ch = piece->dsc_in.channels;
  const int ch_width = ch * piece->roi_in.width;
  const int mask_display = pipe->mask_display;
  const unsigned int pixelformat = ch == 3 ? LF_CR_3(RED, GREEN, BLUE) : LF_CR_4(RED, GREEN, BLUE, UNKNOWN);
  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);

  const gboolean emb_vig  = (p->vignetting_method == dt_iop_lens_correction_source_t::EMBEDDED) && d->embedded.nc > 0;
  const gboolean emb_dist = (p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED) && d->embedded.nc > 0;
  const gboolean emb_tca  = FALSE;
  const gboolean any_emb  = emb_vig || emb_dist;

  const gboolean lf_vig  = (p->vignetting_method == dt_iop_lens_correction_source_t::LENSFUN_DB);
  const gboolean lf_dist = (p->distortion_method == dt_iop_lens_correction_source_t::LENSFUN_DB);
  const gboolean lf_tca  = (p->tca_method == dt_iop_lens_tca_source_t::LENSFUN_DB)
                           || (p->tca_method == dt_iop_lens_tca_source_t::MANUAL);
  const gboolean any_lf  = lf_vig || lf_dist || lf_tca;

  if(any_emb && !any_lf)
  {
    const _emb_axes_t axes = { emb_vig, emb_dist, emb_tca };
    return _process_embedded_metadata_warp(self, pipe, piece,
                                           static_cast<const float *>(ivoid),
                                           static_cast<float *>(ovoid), &axes);
  }

  if(!any_lf)
  {
    dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid,
                              piece->roi_out.width, piece->roi_out.height, ch);
    if(self->dev->gui_attached && g)
    {
      dt_iop_gui_enter_critical_section(self);
      g->status.corrections_done = 0;
      dt_iop_gui_leave_critical_section(self);
    }
    return 0;
  }

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f)
  {
    if(any_emb)
    {
      const _emb_axes_t axes = { emb_vig, emb_dist, emb_tca };
      return _process_embedded_metadata_warp(self, pipe, piece,
                                             static_cast<const float *>(ivoid),
                                             static_cast<float *>(ovoid), &axes);
    }
    dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid,
                              piece->roi_out.width, piece->roi_out.height, ch);
    if(self->dev->gui_attached && g)
    {
      dt_iop_gui_enter_critical_section(self);
      g->status.corrections_done = 0;
      dt_iop_gui_leave_critical_section(self);
    }
    return 0;
  }

  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  const int used_lf_mask = (raw_monochrome) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  int modflags;
  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask);

  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  const float *src_pixels = static_cast<const float *>(ivoid);
  float *work_buf = nullptr;

  if(any_emb)
  {
    const size_t bufsize = (size_t)roi_in->width * roi_in->height * ch * sizeof(float);
    work_buf = (float *)dt_pixelpipe_cache_alloc_align_cache(bufsize, pipe->type);
    if(IS_NULL_PTR(work_buf)) { delete modifier; return 1; }
    memcpy(work_buf, ivoid, bufsize);

    if(emb_vig)
    {
      const float w2 = 0.5f * roi_in->scale * piece->buf_in.width;
      const float h2 = 0.5f * roi_in->scale * piece->buf_in.height;
      const float rn = hypotf(w2, h2);
      const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;
      __OMP_PARALLEL_FOR_CPP__(firstprivate(work_buf, roi_in, ch, w2, h2, inv_rn, d))
      for(int y = 0; y < roi_in->height; y++)
      {
        float *const work_row = work_buf + (size_t)y * roi_in->width * ch;
        for(int x = 0; x < roi_in->width; x++)
        {
          const float cx = roi_in->x + x - w2;
          const float cy = roi_in->y + y - h2;
          const float radius = hypotf(cx, cy) * inv_rn;
          _apply_vignette_gain_3ch(work_row + x * ch, work_row + x * ch, ch,
                                    d->embedded.knots.knots_vig, d->embedded.knots.vig,
                                    d->embedded.nc, radius);
        }
      }
    }

    if(emb_dist || emb_tca)
    {
      const float w2 = 0.5f * roi_in->scale * piece->buf_in.width;
      const float h2 = 0.5f * roi_in->scale * piece->buf_in.height;
      const float rn = hypotf(w2, h2);
      const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;
      const float limw = (float)roi_in->width - 1.0f;
      const float limh = (float)roi_in->height - 1.0f;

      float *const work_ovoid = (float *)dt_pixelpipe_cache_alloc_align_cache(bufsize, pipe->type);
      if(IS_NULL_PTR(work_ovoid))
      {
        dt_pixelpipe_cache_free_align(work_buf);
        delete modifier;
        return 1;
      }

      const _warp_geom_domain_t dom = {
        ch, ch_width, w2, h2, inv_rn, limw, limh,
        raw_monochrome, emb_dist, emb_tca
      };
      _warp_geom_pass(work_buf, work_ovoid, roi_in, roi_out, d, interpolation, &dom);
      dt_pixelpipe_cache_free_align(work_buf);
      work_buf = work_ovoid;
    }

    src_pixels = work_buf;
  }

  {
    const size_t bufsize = (size_t)roi_in->width * roi_in->height * ch * sizeof(float);
    void *buf = dt_pixelpipe_cache_alloc_align_cache(bufsize, pipe->type);
    if(IS_NULL_PTR(buf)) { delete modifier; dt_pixelpipe_cache_free_align(work_buf); return 1; }
    memcpy(buf, src_pixels, bufsize);

    if(modflags & LF_MODIFY_VIGNETTING)
    {
      __OMP_PARALLEL_FOR_CPP__(firstprivate(buf, roi_in, ch, pixelformat, modifier))
      for(int y = 0; y < roi_in->height; y++)
      {
        float *bufptr = ((float *)buf) + (size_t)ch * roi_in->width * y;
        modifier->ApplyColorModification(bufptr, roi_in->x, roi_in->y + y, roi_in->width, 1,
                                         pixelformat, ch * roi_in->width);
      }
    }

    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      const size_t buf2size = (size_t)roi_out->width * 2 * 3;
      size_t padded_buf2size;
      float *const buf2 = dt_pixelpipe_cache_alloc_perthread_float(buf2size, &padded_buf2size);
      if(IS_NULL_PTR(buf2))
      {
        dt_pixelpipe_cache_free_align(buf);
        dt_pixelpipe_cache_free_align(work_buf);
        delete modifier;
        return 1;
      }

#ifdef _OPENMP
#pragma omp parallel for default(none)  \
  firstprivate(roi_out, roi_in, ovoid, ch, padded_buf2size, modifier, mask_display, raw_monochrome, interpolation, ch_width, buf, d, buf2)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *buf2ptr = (float*)dt_get_perthread(buf2, padded_buf2size);
        modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width,
                                                  1, buf2ptr);
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, buf2ptr += 6, out += ch)
          _remap_pixel_inverse_or_not(out, buf2ptr, (const float *)buf,
                                       _remap_ctx_t{ ch, ch_width, d->lensfun.do_nan_checks,
                                                      raw_monochrome, mask_display },
                                       roi_in, interpolation);
      }
      dt_pixelpipe_cache_free_align(buf2);
    }
    else
    {
      memcpy(ovoid, buf, bufsize);
    }
    dt_pixelpipe_cache_free_align(buf);
  }
  dt_pixelpipe_cache_free_align(work_buf);
  delete modifier;

  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    g->status.corrections_done = modflags;
    dt_iop_gui_leave_critical_section(self);
  }
  return 0;
}

#ifdef HAVE_OPENCL
typedef struct {
  cl_mem *dev_knots_vig;
  cl_mem *dev_vig;
  cl_mem *dev_knots_dist;
  cl_mem *dev_cor_rgb0;
  cl_mem *dev_cor_rgb1;
  cl_mem *dev_cor_rgb2;
} _cl_embedded_bufs_t;

typedef struct {
  int iwidth;
  int iheight;
  int owidth;
  int oheight;
  int roi_in_x;
  int roi_in_y;
  int roi_out_x;
  int roi_out_y;
  int nc;
  int monochrome;
  float w2;
  float h2;
  float inv_rn;
  gboolean apply_vignette;
  gboolean apply_dist;
  gboolean apply_tca;
} _cl_domain_t;

static int _run_md_cl_pass(int devid, int kernel,
                           cl_mem dev_src, cl_mem dev_dst,
                           const size_t *sizes)
{
  if(kernel == 0)
  {
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { sizes[0], sizes[1], 1 };
    return dt_opencl_enqueue_copy_image(devid, dev_src, dev_dst, origin, origin, region);
  }
  return dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
}

static cl_int _setup_md_cl_kernels(int devid,
                                    _cl_embedded_bufs_t *bufs,
                                    dt_embedded_lens_knots_t *knots)
{
  const size_t knots_size = (size_t)LENS_MAXKNOTS * sizeof(float);

  *bufs->dev_knots_vig = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*bufs->dev_knots_vig)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *bufs->dev_vig = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*bufs->dev_vig)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *bufs->dev_knots_dist = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*bufs->dev_knots_dist)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *bufs->dev_cor_rgb0 = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*bufs->dev_cor_rgb0)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *bufs->dev_cor_rgb1 = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*bufs->dev_cor_rgb1)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *bufs->dev_cor_rgb2 = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*bufs->dev_cor_rgb2)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;

  cl_int err;
  err = dt_opencl_write_buffer_to_device(devid, knots->knots_vig, *bufs->dev_knots_vig, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->vig, *bufs->dev_vig, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->knots_dist, *bufs->dev_knots_dist, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->cor_rgb[0], *bufs->dev_cor_rgb0, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->cor_rgb[1], *bufs->dev_cor_rgb1, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->cor_rgb[2], *bufs->dev_cor_rgb2, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;

  return CL_SUCCESS;
}

typedef struct {
  int devid;
  dt_iop_lensfun_global_data_t *gd;
  const _cl_domain_t *cldom;
  cl_mem dev_in;
  cl_mem dev_tmp;
  cl_mem dev_out;
  cl_mem dev_knots_vig;
  cl_mem dev_vig;
  cl_mem dev_knots_dist;
  cl_mem dev_cor_rgb0;
  cl_mem dev_cor_rgb1;
  cl_mem dev_cor_rgb2;
  const size_t *isizes;
  const size_t *osizes;
  const size_t *iregion;
  const size_t *oregion;
} _cl_embedded_run_ctx_t;

static cl_int _cl_embedded_run_forward(const _cl_embedded_run_ctx_t *ctx)
{
  cl_int err;
  if(ctx->cldom->apply_vignette)
  {
    const int k = ctx->gd->kernel_md_vignette;
    dt_opencl_set_kernel_arg(ctx->devid, k, 0, sizeof(cl_mem), (void *)&ctx->dev_in);
    dt_opencl_set_kernel_arg(ctx->devid, k, 1, sizeof(cl_mem), (void *)&ctx->dev_tmp);
    dt_opencl_set_kernel_arg(ctx->devid, k, 2, sizeof(int), (void *)&ctx->cldom->iwidth);
    dt_opencl_set_kernel_arg(ctx->devid, k, 3, sizeof(int), (void *)&ctx->cldom->iheight);
    dt_opencl_set_kernel_arg(ctx->devid, k, 4, sizeof(int), (void *)&ctx->cldom->roi_in_x);
    dt_opencl_set_kernel_arg(ctx->devid, k, 5, sizeof(int), (void *)&ctx->cldom->roi_in_y);
    dt_opencl_set_kernel_arg(ctx->devid, k, 6, sizeof(float), (void *)&ctx->cldom->w2);
    dt_opencl_set_kernel_arg(ctx->devid, k, 7, sizeof(float), (void *)&ctx->cldom->h2);
    dt_opencl_set_kernel_arg(ctx->devid, k, 8, sizeof(float), (void *)&ctx->cldom->inv_rn);
    dt_opencl_set_kernel_arg(ctx->devid, k, 9, sizeof(cl_mem), (void *)&ctx->dev_knots_vig);
    dt_opencl_set_kernel_arg(ctx->devid, k, 10, sizeof(cl_mem), (void *)&ctx->dev_vig);
    dt_opencl_set_kernel_arg(ctx->devid, k, 11, sizeof(int), (void *)&ctx->cldom->nc);
    err = _run_md_cl_pass(ctx->devid, k, ctx->dev_in, ctx->dev_tmp, ctx->isizes);
  }
  else
    err = _run_md_cl_pass(ctx->devid, 0, ctx->dev_in, ctx->dev_tmp, ctx->iregion);
  if(err != CL_SUCCESS) return err;

  if(ctx->cldom->apply_dist || ctx->cldom->apply_tca)
  {
    const int k = ctx->gd->kernel_md_lens_correction;
    dt_opencl_set_kernel_arg(ctx->devid, k, 0, sizeof(cl_mem), (void *)&ctx->dev_tmp);
    dt_opencl_set_kernel_arg(ctx->devid, k, 1, sizeof(cl_mem), (void *)&ctx->dev_out);
    dt_opencl_set_kernel_arg(ctx->devid, k, 2, sizeof(int), (void *)&ctx->cldom->owidth);
    dt_opencl_set_kernel_arg(ctx->devid, k, 3, sizeof(int), (void *)&ctx->cldom->oheight);
    dt_opencl_set_kernel_arg(ctx->devid, k, 4, sizeof(int), (void *)&ctx->cldom->roi_in_x);
    dt_opencl_set_kernel_arg(ctx->devid, k, 5, sizeof(int), (void *)&ctx->cldom->roi_in_y);
    dt_opencl_set_kernel_arg(ctx->devid, k, 6, sizeof(int), (void *)&ctx->cldom->roi_out_x);
    dt_opencl_set_kernel_arg(ctx->devid, k, 7, sizeof(int), (void *)&ctx->cldom->roi_out_y);
    dt_opencl_set_kernel_arg(ctx->devid, k, 8, sizeof(int), (void *)&ctx->cldom->iwidth);
    dt_opencl_set_kernel_arg(ctx->devid, k, 9, sizeof(int), (void *)&ctx->cldom->iheight);
    dt_opencl_set_kernel_arg(ctx->devid, k, 10, sizeof(float), (void *)&ctx->cldom->w2);
    dt_opencl_set_kernel_arg(ctx->devid, k, 11, sizeof(float), (void *)&ctx->cldom->h2);
    dt_opencl_set_kernel_arg(ctx->devid, k, 12, sizeof(float), (void *)&ctx->cldom->inv_rn);
    dt_opencl_set_kernel_arg(ctx->devid, k, 13, sizeof(cl_mem), (void *)&ctx->dev_knots_dist);
    dt_opencl_set_kernel_arg(ctx->devid, k, 14, sizeof(cl_mem), (void *)&ctx->dev_cor_rgb0);
    dt_opencl_set_kernel_arg(ctx->devid, k, 15, sizeof(cl_mem), (void *)&ctx->dev_cor_rgb1);
    dt_opencl_set_kernel_arg(ctx->devid, k, 16, sizeof(cl_mem), (void *)&ctx->dev_cor_rgb2);
    dt_opencl_set_kernel_arg(ctx->devid, k, 17, sizeof(int), (void *)&ctx->cldom->nc);
    dt_opencl_set_kernel_arg(ctx->devid, k, 18, sizeof(int), (void *)&ctx->cldom->monochrome);
    err = _run_md_cl_pass(ctx->devid, k, ctx->dev_tmp, ctx->dev_out, ctx->osizes);
  }
  else
    err = _run_md_cl_pass(ctx->devid, 0, ctx->dev_tmp, ctx->dev_out, ctx->oregion);
  return err;
}

static cl_int _cl_embedded_try_passthrough(int devid, cl_mem dev_in, cl_mem dev_out,
                                            size_t *origin, size_t *oregion,
                                            const dt_iop_lensfun_data_t *d,
                                            dt_iop_module_t *self,
                                            const _emb_axes_t *emb_axes)
{
  cl_int err;
  if(!d->embedded.nc)
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err == CL_SUCCESS)
      _report_corrections_done(self, d->lensfun.modify_flags);
    return (err == CL_SUCCESS) ? TRUE : FALSE;
  }

  const gboolean apply_vignette = emb_axes ? emb_axes->apply_vignette : FALSE;
  const gboolean apply_dist = emb_axes ? emb_axes->apply_distortion : FALSE;
  const gboolean apply_tca = emb_axes ? emb_axes->apply_tca : FALSE;

  if(apply_dist != apply_tca)
  {
    _report_corrections_done(self, d->lensfun.modify_flags);
    return FALSE;
  }

  if(!apply_vignette && !apply_dist && !apply_tca)
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err == CL_SUCCESS)
      _report_corrections_done(self, d->lensfun.modify_flags);
    return (err == CL_SUCCESS) ? TRUE : FALSE;
  }

  return -1;
}

static int process_embedded_metadata_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                        const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                        dt_iop_lensfun_data_t *d, dt_iop_lensfun_global_data_t *gd,
                                        const _emb_axes_t *emb_axes)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const int devid = pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const int roi_in_x = roi_in->x;
  const int roi_in_y = roi_in->y;
  const int roi_out_x = roi_out->x;
  const int roi_out_y = roi_out->y;
  const int width = MAX(iwidth, owidth);
  const int height = MAX(iheight, oheight);

  cl_int err;
  cl_mem dev_tmp = nullptr;

  size_t origin[] = { 0, 0, 0 };
  size_t oregion[] = { (size_t)owidth, (size_t)oheight, 1 };
  size_t iregion[] = { (size_t)iwidth, (size_t)iheight, 1 };
  size_t isizes[] = { (size_t)ROUNDUPDWD(iwidth, devid), (size_t)ROUNDUPDHT(iheight, devid), 1 };
  size_t osizes[] = { (size_t)ROUNDUPDWD(owidth, devid), (size_t)ROUNDUPDHT(oheight, devid), 1 };

  const int passthrough = _cl_embedded_try_passthrough(devid, dev_in, dev_out, origin, oregion, d, self, emb_axes);
  if(passthrough != -1) return passthrough;

  const gboolean apply_vignette = emb_axes ? emb_axes->apply_vignette : FALSE;
  const gboolean apply_dist = emb_axes ? emb_axes->apply_distortion : FALSE;
  const gboolean apply_tca = emb_axes ? emb_axes->apply_tca : FALSE;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  const float w2 = 0.5f * orig_w;
  const float h2 = 0.5f * orig_h;
  const float rn = hypotf(w2, h2);
  const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;
  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int monochrome = raw_monochrome ? 1 : 0;

  const _cl_domain_t cldom = {
    iwidth, iheight, owidth, oheight,
    roi_in_x, roi_in_y, roi_out_x, roi_out_y,
    d->embedded.nc, monochrome,
    w2, h2, inv_rn,
    apply_vignette, apply_dist, apply_tca
  };

  cl_mem dev_knots_vig = nullptr;
  cl_mem dev_vig = nullptr;
  cl_mem dev_knots_dist = nullptr;
  cl_mem dev_cor_rgb0 = nullptr;
  cl_mem dev_cor_rgb1 = nullptr;
  cl_mem dev_cor_rgb2 = nullptr;

  _cl_embedded_bufs_t embufs = { &dev_knots_vig, &dev_vig,
                                  &dev_knots_dist, &dev_cor_rgb0,
                                  &dev_cor_rgb1, &dev_cor_rgb2 };

  err = _setup_md_cl_kernels(devid, &embufs, &d->embedded.knots);
  if(err != CL_SUCCESS) goto error;

  dev_tmp = (cl_mem)dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(IS_NULL_PTR(dev_tmp)) goto error;

  {
    _cl_embedded_run_ctx_t run_ctx = {
      devid, gd, &cldom,
      dev_in, dev_tmp, dev_out,
      dev_knots_vig, dev_vig, dev_knots_dist,
      dev_cor_rgb0, dev_cor_rgb1, dev_cor_rgb2,
      isizes, osizes, iregion, oregion
    };
    err = _cl_embedded_run_forward(&run_ctx);
  }
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_knots_vig);
  dt_opencl_release_mem_object(dev_vig);
  dt_opencl_release_mem_object(dev_knots_dist);
  dt_opencl_release_mem_object(dev_cor_rgb0);
  dt_opencl_release_mem_object(dev_cor_rgb1);
  dt_opencl_release_mem_object(dev_cor_rgb2);
  dt_print(DT_DEBUG_OPENCL, "[opencl_lens] embedded-metadata complete (md_vignette + md_lens_correction)\n");
  _report_corrections_done(self, d->lensfun.modify_flags);
  return TRUE;

error:
  _report_corrections_done(self, d->lensfun.modify_flags);
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_knots_vig);
  dt_opencl_release_mem_object(dev_vig);
  dt_opencl_release_mem_object(dev_knots_dist);
  dt_opencl_release_mem_object(dev_cor_rgb0);
  dt_opencl_release_mem_object(dev_cor_rgb1);
  dt_opencl_release_mem_object(dev_cor_rgb2);
  dt_print(DT_DEBUG_OPENCL, "[opencl_lens] embedded-metadata kernel failed! %d\n", err);
  return FALSE;
}

int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  auto d = (dt_iop_lensfun_data_t *)piece->data;
  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  const gboolean emb_vig  = (p->vignetting_method == dt_iop_lens_correction_source_t::EMBEDDED) && d->embedded.nc > 0;
  const gboolean emb_dist = (p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED) && d->embedded.nc > 0;
  const gboolean emb_tca  = FALSE;
  const gboolean any_emb  = emb_vig || emb_dist;

  const gboolean lf_vig  = (p->vignetting_method == dt_iop_lens_correction_source_t::LENSFUN_DB);
  const gboolean lf_dist = (p->distortion_method == dt_iop_lens_correction_source_t::LENSFUN_DB);
  const gboolean lf_tca  = (p->tca_method == dt_iop_lens_tca_source_t::LENSFUN_DB)
                           || (p->tca_method == dt_iop_lens_tca_source_t::MANUAL);
  const gboolean any_lf  = lf_vig || lf_dist || lf_tca;

  if(any_emb && !any_lf)
  {
    const _emb_axes_t axes = { emb_vig, emb_dist, emb_tca };
    return process_embedded_metadata_cl(self, pipe, piece, dev_in, dev_out, d, gd, &axes);
  }

  if(any_emb && any_lf) return FALSE;

  if(!any_lf)
  {
    cl_int err = dt_opencl_enqueue_copy_image(pipe->devid, dev_in, dev_out,
                                              (size_t[]){0,0,0}, (size_t[]){0,0,0},
                                              (size_t[]){(size_t)roi_out->width, (size_t)roi_out->height, 1});
    if(self->dev->gui_attached && g)
    {
      dt_iop_gui_enter_critical_section(self);
      g->status.corrections_done = 0;
      dt_iop_gui_leave_critical_section(self);
    }
    return (err == CL_SUCCESS) ? TRUE : FALSE;
  }

  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int used_lf_mask = (raw_monochrome) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  cl_mem dev_tmpbuf = nullptr;
  cl_mem dev_tmp = nullptr;
  cl_int err = -999;

  float *tmpbuf = nullptr;
  lfModifier *modifier = nullptr;

  const int devid = pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const int roi_in_x = roi_in->x;
  const int roi_in_y = roi_in->y;
  const int width = MAX(iwidth, owidth);
  const int height = MAX(iheight, oheight);
  const int ch = piece->dsc_in.channels;
  const int tmpbufwidth = owidth * 2 * 3;
  const size_t tmpbuflen = MAX((size_t)oheight * owidth * 2 * 3, (size_t)iheight * iwidth * ch)
                             * sizeof(float);
  const unsigned int pixelformat = ch == 3 ? LF_CR_3(RED, GREEN, BLUE) : LF_CR_4(RED, GREEN, BLUE, UNKNOWN);

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;

  size_t origin[] = { 0, 0, 0 };
  size_t iregion[] = { (size_t)iwidth, (size_t)iheight, 1 };
  size_t oregion[] = { (size_t)owidth, (size_t)oheight, 1 };
  size_t isizes[] = { (size_t)ROUNDUPDWD(iwidth, devid), (size_t)ROUNDUPDHT(iheight, devid), 1 };
  size_t osizes[] = { (size_t)ROUNDUPDWD(owidth, devid), (size_t)ROUNDUPDHT(oheight, devid), 1 };

  int modflags;
  int ldkernel = -1;
  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f)
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
    if(self->dev->gui_attached && g)
    {
      dt_iop_gui_enter_critical_section(self);
      g->status.corrections_done = 0;
      dt_iop_gui_leave_critical_section(self);
    }
    return TRUE;
  }

  switch(interpolation->id)
  {
    case DT_INTERPOLATION_BILINEAR:
      ldkernel = gd->kernel_lens_distort_bilinear;
      break;
    case DT_INTERPOLATION_BICUBIC:
      ldkernel = gd->kernel_lens_distort_bicubic;
      break;
    case DT_INTERPOLATION_MITCHELL:
      ldkernel = gd->kernel_lens_distort_mitchell;
      break;
    default:
      if(self->dev->gui_attached && g)
      {
        dt_iop_gui_enter_critical_section(self);
        g->status.corrections_done = 0;
        dt_iop_gui_leave_critical_section(self);
      }
      return FALSE;
  }

  tmpbuf = (float *)dt_pixelpipe_cache_alloc_align_cache(tmpbuflen, pipe->type);
  if(IS_NULL_PTR(tmpbuf)) goto error;

  dev_tmp = (cl_mem)dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(IS_NULL_PTR(dev_tmp)) goto error;

  dev_tmpbuf = (cl_mem)dt_opencl_alloc_device_buffer(devid, tmpbuflen);
  if(IS_NULL_PTR(dev_tmpbuf)) goto error;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  modifier = get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(modflags & LF_MODIFY_VIGNETTING)
  {
    __OMP_PARALLEL_FOR_CPP__(firstprivate(tmpbuf, ch, roi_in, pixelformat, modifier))
    for(int y = 0; y < roi_in->height; y++)
    {
      float *buf = tmpbuf + (size_t)y * ch * roi_in->width;
      _lens_fill_vignette_row(buf, roi_in->width, ch);
      modifier->ApplyColorModification(buf, roi_in->x, roi_in->y + y, roi_in->width, 1,
                                       pixelformat, ch * roi_in->width);
    }

    err = dt_opencl_write_buffer_to_device(
        devid, tmpbuf, dev_tmpbuf, 0, (size_t)ch * roi_in->width * roi_in->height * sizeof(float), CL_TRUE);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 1, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 2, sizeof(int), (void *)&iwidth);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 3, sizeof(int), (void *)&iheight);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 4, sizeof(cl_mem), (void *)&dev_tmpbuf);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette, isizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_tmp, origin, origin, iregion);
    if(err != CL_SUCCESS) goto error;
  }

  if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {
    __OMP_PARALLEL_FOR_CPP__(firstprivate(modifier, roi_out, tmpbuf, tmpbufwidth))
    for(int y = 0; y < roi_out->height; y++)
    {
      float *pi = tmpbuf + (size_t)y * tmpbufwidth;
      modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width, 1, pi);
    }

    err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0,
                                           (size_t)owidth * oheight * 2 * 3 * sizeof(float), CL_TRUE);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_tmp);
    dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
    dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
    dt_opencl_set_kernel_arg(devid, ldkernel, 6, sizeof(int), (void *)&roi_in_x);
    dt_opencl_set_kernel_arg(devid, ldkernel, 7, sizeof(int), (void *)&roi_in_y);
    dt_opencl_set_kernel_arg(devid, ldkernel, 8, sizeof(cl_mem), (void *)&dev_tmpbuf);
    dt_opencl_set_kernel_arg(devid, ldkernel, 9, sizeof(int), (void *)&(d->lensfun.do_nan_checks));
    dt_opencl_set_kernel_arg(devid, ldkernel, 10, sizeof(int), (void *)&(raw_monochrome));
    err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
  }

  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    g->status.corrections_done = modflags;
    dt_iop_gui_leave_critical_section(self);
  }

  dt_opencl_release_mem_object(dev_tmpbuf);
  dt_opencl_release_mem_object(dev_tmp);
  dt_pixelpipe_cache_free_align(tmpbuf);
  if(!IS_NULL_PTR(modifier)) delete modifier;
  return TRUE;

error:
  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    g->status.corrections_done = 0;
    dt_iop_gui_leave_critical_section(self);
  }
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_tmpbuf);
  dt_pixelpipe_cache_free_align(tmpbuf);
  if(!IS_NULL_PTR(modifier)) delete modifier;
  dt_print(DT_DEBUG_OPENCL, "[opencl_lens] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  auto p = (dt_iop_lensfun_params_t *)self->params;
  const gboolean emb_dist = (p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED)
                            && d->embedded.nc > 0;
  const gboolean emb_tca = FALSE;
  const gboolean any_emb = emb_dist || emb_tca;

  if(any_emb && p->vignetting_method != dt_iop_lens_correction_source_t::LENSFUN_DB
     && p->distortion_method != dt_iop_lens_correction_source_t::LENSFUN_DB
     && (p->tca_method != dt_iop_lens_tca_source_t::LENSFUN_DB
         && p->tca_method != dt_iop_lens_tca_source_t::MANUAL))
  {
    tiling->factor = 3.0f;
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 1;
    tiling->yalign = 1;

    if(!emb_dist)
    {
      tiling->overlap = 0;
      return;
    }

    // Vendor-agnostic conservative worst-case displacement: evaluate the
    // normalized-radius spline at the farthest image corner (radius == 1.0) for every RGB
    // channel curve and scale by the half-diagonal, plus a fixed margin matching the
    // Mitchell-Netravali kernel's half width (DT_INTERPOLATION_USERPREF_WARP's compiled-in
    // default, see CLAUDE.md) -- avoids depending on dt_conf/dt_interpolation_new from this
    // declare-time callback.
    const double orig_w = piece->buf_in.width;
    const double orig_h = piece->buf_in.height;
    const auto half_diag = hypot(0.5 * orig_w, 0.5 * orig_h);

    double max_abs_dr_minus_1 = 0.0;
    for(int c : {0, 1, 2})
    {
      const auto dr = (double)dt_embedded_lens_linear_spline(d->embedded.knots.knots_dist, d->embedded.knots.cor_rgb[c], d->embedded.nc, 1.0f);
      max_abs_dr_minus_1 = fmax(max_abs_dr_minus_1, fabs(dr - 1.0));
    }

    static const int MITCHELL_KERNEL_HALF_WIDTH_PX = 2;
    tiling->overlap = (int)ceil(max_abs_dr_minus_1 * half_diag) + MITCHELL_KERNEL_HALF_WIDTH_PX;
    return;
  }

  tiling->factor = 4.5f;
  tiling->maxbuf = 1.5f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

int distort_transform(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                      float *const __restrict points, size_t points_count)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  auto d = (dt_iop_lensfun_data_t *)piece->data;

  const gboolean emb_dist = (p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED)
                            && d->embedded.nc > 0;
  const gboolean emb_tca = FALSE;
  const gboolean any_emb_geom = emb_dist || emb_tca;

  if(any_emb_geom)
  {
    _emb_axes_t axes = { FALSE, emb_dist, emb_tca };
    return _distort_transform_embedded_metadata_warp(self, pipe, piece, points, points_count, &axes);
  }

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f) return 0;

  const float orig_w = piece->buf_in.width;
  const float orig_h = piece->buf_in.height;
  int modflags;

  const int used_lf_mask = (dt_image_is_monochrome(&self->dev->image_storage)) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask);
  if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {
    __OMP_PARALLEL_FOR_CPP__(firstprivate(points, points_count, modifier) if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      modifier->ApplySubpixelGeometryDistortion(points[i], points[i + 1], 1, 1, buf);
      // take green channel distortion, like distort_mask() does, so x and y come from the
      // same color channel's distortion field instead of mixing red's x with green's y.
      points[i] = buf[2];
      points[i + 1] = buf[3];
    }
  }

  delete modifier;
  return 1;
}

int distort_backtransform(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                          float *const __restrict points, size_t points_count)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  auto d = (dt_iop_lensfun_data_t *)piece->data;

  const gboolean emb_dist = (p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED)
                            && d->embedded.nc > 0;
  const gboolean emb_tca = FALSE;
  const gboolean any_emb_geom = emb_dist || emb_tca;

  if(any_emb_geom)
  {
    _emb_axes_t axes = { FALSE, emb_dist, emb_tca };
    return _distort_backtransform_embedded_metadata_warp(self, pipe, piece, points, points_count, &axes);
  }

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f) return 0;

  const int used_lf_mask = (dt_image_is_monochrome(&self->dev->image_storage)) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  const float orig_w = piece->buf_in.width;
  const float orig_h = piece->buf_in.height;
  int modflags;
  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask);

  if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {
    __OMP_PARALLEL_FOR_CPP__(firstprivate(points_count, modifier, points) if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      modifier->ApplySubpixelGeometryDistortion(points[i], points[i + 1], 1, 1, buf);
      // take green channel distortion, like distort_mask() does, so x and y come from the
      // same color channel's distortion field instead of mixing red's x with green's y.
      points[i] = buf[2];
      points[i + 1] = buf[3];
    }
  }

  delete modifier;
  return 1;
}

// TODO: Shall we keep LF_MODIFY_TCA in the modifiers?
void distort_mask(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece,
                  const float *const in, float *const out, const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  (void)pipe;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  const gboolean emb_dist = (p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED)
                            && d->embedded.nc > 0;
  const gboolean emb_tca = FALSE;
  const gboolean any_emb_geom = emb_dist || emb_tca;

  if(any_emb_geom)
  {
    _emb_axes_t axes = { FALSE, emb_dist, emb_tca };
    _distort_mask_embedded_metadata_warp(self, pipe, piece, in, out, roi_in, roi_out, &axes);
    return;
  }

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f)
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  int modflags;
  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE);

  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(!(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE)))
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    delete modifier;
    return;
  }

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  // acquire temp memory for distorted pixel coords
  const size_t bufsize = (size_t)roi_out->width * 2 * 3;
  size_t padded_bufsize;
  float *const buf = dt_pixelpipe_cache_alloc_perthread_float(bufsize, &padded_bufsize);
  if(IS_NULL_PTR(buf)) return;
  __OMP_PARALLEL_FOR_CPP__(firstprivate(buf, padded_bufsize, d, modifier, in, out, interpolation, roi_in, roi_out))
  for(int y = 0; y < roi_out->height; y++)
  {
    float *bufptr = (float*)dt_get_perthread(buf, padded_bufsize);
    modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width, 1, bufptr);

    // reverse transform the global coords from lf to our buffer
    float *_out = out + (size_t)y * roi_out->width;
    for(int x = 0; x < roi_out->width; x++, bufptr += 6, _out++)
    {
      if(d->lensfun.do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
      {
        *_out = 0.0f;
        continue;
      }

      // take green channel distortion also for alpha channel
      const float pi0 = bufptr[2] - roi_in->x;
      const float pi1 = bufptr[3] - roi_in->y;
      *_out = dt_interpolation_compute_sample(interpolation, in, pi0, pi1, roi_in->width, roi_in->height, 1,
                                              roi_in->width);
    }
  }
  
  
  dt_pixelpipe_cache_free_align(buf);
  delete modifier;
}

void modify_roi_out(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                    struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  const gboolean emb_dist = (p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED)
                            && d->embedded.nc > 0;
  const gboolean emb_tca = FALSE;
  const gboolean any_emb = emb_dist || emb_tca;

  if(any_emb)
  {
    _emb_axes_t axes = { FALSE, emb_dist, emb_tca };
    _modify_roi_out_embedded_metadata_warp(self, pipe, piece, roi_out, roi_in, &axes);
    return;
  }

  *roi_out = *roi_in;
}

void modify_roi_in(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                   struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  auto d = (dt_iop_lensfun_data_t *)piece->data;
  *roi_in = *roi_out;

  const gboolean emb_dist = (p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED)
                            && d->embedded.nc > 0;
  const gboolean emb_tca = FALSE;
  const gboolean any_emb_geom = emb_dist || emb_tca;

  if(any_emb_geom)
  {
    _emb_axes_t axes = { FALSE, emb_dist, emb_tca };
    _modify_roi_in_embedded_metadata_warp(self, pipe, piece, roi_out, roi_in, &axes);
    return;
  }

  // inverse transform with given params
  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f) return;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  int modflags;
  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, LF_MODIFY_ALL);

  if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {
    const int xoff = roi_in->x;
    const int yoff = roi_in->y;
    const int width = roi_in->width;
    const int height = roi_in->height;
    const int awidth = abs(width);
    const int aheight = abs(height);
    const int xstep = (width < 0) ? -1 : 1;
    const int ystep = (height < 0) ? -1 : 1;

    float xm = FLT_MAX;
  float xM = -FLT_MAX;
  float ym = FLT_MAX;
  float yM = -FLT_MAX;
    const size_t nbpoints = 2 * awidth + 2 * aheight;

  // ROI planning passes the active pipe now, but this temporary edge buffer only needs an
  // allocator bucket id, so use a stable generic bucket.
    float *const buf = (float *)dt_pixelpipe_cache_alloc_align_cache(sizeof(float) * nbpoints * 2 * 3,
                                                                     DT_DEV_PIXELPIPE_FULL);
    if(IS_NULL_PTR(buf)) return;

#ifdef _OPENMP
#pragma omp parallel default(none) reduction(min : xm, ym) reduction(max : xM, yM) \
  firstprivate(modifier, xoff, yoff, awidth, aheight, width, height, nbpoints, ystep, xstep, buf)
#endif
    {
      __OMP_FOR__()
      for(int i = 0; i < awidth; i++)
        modifier->ApplySubpixelGeometryDistortion(xoff + i * xstep, yoff, 1, 1, buf + 6 * i);
      __OMP_FOR__()
      for(int i = 0; i < awidth; i++)
        modifier->ApplySubpixelGeometryDistortion(xoff + i * xstep, yoff + (height - 1), 1, 1, buf + 6 * (awidth + i));
      __OMP_FOR__()
      for(int j = 0; j < aheight; j++)
        modifier->ApplySubpixelGeometryDistortion(xoff, yoff + j * ystep, 1, 1, buf + 6 * (2 * awidth + j));
      __OMP_FOR__()
      for(int j = 0; j < aheight; j++)
        modifier->ApplySubpixelGeometryDistortion(xoff + (width - 1), yoff + j * ystep, 1, 1, buf + 6 * (2 * awidth + aheight + j));

#ifdef _OPENMP
#pragma omp barrier
#endif
      __OMP_FOR__()
      for(size_t k = 0; k < nbpoints; k++)
      {
        // iterate over RGB channels x and y coordinates
        for(size_t c = 0; c < 6; c+=2)
        {
          const float x = buf[6 * k + c];
          const float y = buf[6 * k + c + 1];
          xm = isnan(x) ? xm : MIN(xm, x);
          xM = isnan(x) ? xM : MAX(xM, x);
          ym = isnan(y) ? ym : MIN(ym, y);
          yM = isnan(y) ? yM : MAX(yM, y);
        }
      }
    }

  dt_pixelpipe_cache_free_align(buf);

    // LensFun can return NAN coords, so we need to handle them carefully.
    if(!isfinite(xm) || !(0 <= xm && xm < orig_w)) xm = 0;
    if(!isfinite(xM) || !(1 <= xM && xM < orig_w)) xM = orig_w;
    if(!isfinite(ym) || !(0 <= ym && ym < orig_h)) ym = 0;
    if(!isfinite(yM) || !(1 <= yM && yM < orig_h)) yM = orig_h;

    const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    roi_in->x = fmaxf(0.0f, roundf(xm - interpolation->width));
    roi_in->y = fmaxf(0.0f, roundf(ym - interpolation->width));
    roi_in->width = roundf(fminf(orig_w - roi_in->x, xM - roi_in->x + interpolation->width));
    roi_in->height = roundf(fminf(orig_h - roi_in->y, yM - roi_in->y + interpolation->width));

    // sanity check.
    roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(orig_w));
    roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(orig_h));
    roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(orig_w) - roi_in->x);
    roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(orig_h) - roi_in->y);
  }
  delete modifier;
}

static void _apply_tca_override_lf_legacy(lfLens *lens, float tca_r, float tca_b)
{
  lfLensCalibTCA tca = { LF_TCA_MODEL_NONE };
  tca.Focal = 0;
  tca.Model = LF_TCA_MODEL_LINEAR;
  tca.Terms[0] = tca_r;
  tca.Terms[1] = tca_b;
  if(lens->CalibTCA)
    while(lens->CalibTCA[0]) lens->RemoveCalibTCA(0);
  lens->AddCalibTCA(&tca);
}

static float commit_unified_scale(const dt_image_t *img, float user_scale,
                                   dt_embedded_lens_knots_t *knots, int *out_nc)
{
  const dt_embedded_lens_finetune_t ft = { 1.0f, 1.0f, 1.0f, 1.0f };
  float out_scale = 1.0f;
  *out_nc = dt_embedded_lens_init_coeffs(img, &ft, knots, &out_scale);
  return user_scale * out_scale;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  auto p = (dt_iop_lensfun_params_t *)p1;

  // Capture per-correction enums and scale from the HISTORY-SNAPSHOT p1
  // BEFORE the has_been_set substitution, so the dispatch always follows
  // the history-recorded per-axis choice, never the substituted defaults.
  const dt_iop_lens_correction_source_t dist_method = ((dt_iop_lensfun_params_t *)p1)->distortion_method;
  const dt_iop_lens_correction_source_t vig_method  = ((dt_iop_lensfun_params_t *)p1)->vignetting_method;
  const dt_iop_lens_tca_source_t        tca_method  = ((dt_iop_lensfun_params_t *)p1)->tca_method;
  const float user_scale = ((dt_iop_lensfun_params_t *)p1)->scale;

  if(p->has_been_set == 1)
  {
    p = (dt_iop_lensfun_params_t *)self->default_params;
    dt_iop_compute_module_hash(self, self->dev->forms);
  }

  auto d = (dt_iop_lensfun_data_t *)piece->data;

  const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);

  const gboolean needs_lensfun =
    (dist_method == dt_iop_lens_correction_source_t::LENSFUN_DB)
    || (vig_method == dt_iop_lens_correction_source_t::LENSFUN_DB)
    || (tca_method == dt_iop_lens_tca_source_t::LENSFUN_DB);

  const gboolean needs_embedded =
    (dist_method == dt_iop_lens_correction_source_t::EMBEDDED)
    || (vig_method == dt_iop_lens_correction_source_t::EMBEDDED);

  d->lensfun.modify_flags = per_axis_modify_flags(dist_method, vig_method, tca_method, monochrome);

  if(needs_embedded)
  {
    d->lensfun.scale = commit_unified_scale(&self->dev->image_storage, user_scale,
                                             &d->embedded.knots, &d->embedded.nc);
  }
  else
  {
    d->embedded.nc = 0;
    d->lensfun.scale = user_scale;
  }

  delete d->lensfun.lens; // NOSONAR
  d->lensfun.lens = new lfLens;

  if(needs_lensfun)
  {
    auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
    auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
    const lfCamera *camera = nullptr;
    const lfCamera **cam = nullptr;

    if(p->camera[0])
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      cam = dt_iop_lensfun_db->FindCamerasExt(NULL, p->camera, 0);
      if(cam)
      {
        camera = cam[0];
        d->lensfun.crop = cam[0]->CropFactor;
      }
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    }

    if(p->lens[0])
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      const lfLens **lens
          = dt_iop_lensfun_db->FindLenses(camera, NULL, p->lens, 0);
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
      if(lens)
      {
        *d->lensfun.lens = *lens[0];
        if(tca_method == dt_iop_lens_tca_source_t::MANUAL)
        {
#ifdef LF_0395
          const dt_image_t *img = &(self->dev->image_storage);

          d->lensfun.custom_tca =
            {
             .Model     = LF_TCA_MODEL_LINEAR,
             .Focal     = p->focal,
             .Terms     = { p->tca_r, p->tca_b },
             .CalibAttr = {
                           .CenterX = 0.0f,
                           .CenterY = 0.0f,
                           .CropFactor = d->lensfun.crop,
                           .AspectRatio = (float)img->width / (float)img->height
                           }
            };
#else
          _apply_tca_override_lf_legacy(d->lensfun.lens, p->tca_r, p->tca_b);
#endif
        }
        lf_free(lens);
      }
    }
    lf_free(cam);
  }

  d->lensfun.focal = p->focal;
  d->lensfun.aperture = p->aperture;
  d->lensfun.distance = p->distance;
  d->lensfun.target_geom = p->target_geom;

  d->lensfun.do_nan_checks = TRUE;
  if(d->lensfun.target_geom == LF_RECTILINEAR)
  {
    d->lensfun.do_nan_checks = FALSE;
  }
  else if(d->lensfun.target_geom == d->lensfun.lens->Type)
  {
    d->lensfun.do_nan_checks = FALSE;
  }

  piece->cache_output_on_ram = TRUE;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_lensfun_data_t));
  piece->data_size = sizeof(dt_iop_lensfun_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  auto d = (dt_iop_lensfun_data_t *)piece->data;

  if(d->lensfun.lens)
  {
    delete d->lensfun.lens; // NOSONAR
    d->lensfun.lens = nullptr;
  }
  dt_free_align(piece->data);
  piece->data = nullptr;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_lensfun_global_data_t *gd
      = (dt_iop_lensfun_global_data_t *)calloc(1, sizeof(dt_iop_lensfun_global_data_t));
  module->data = gd;
  gd->kernel_lens_distort_bilinear = dt_opencl_create_kernel(program, "lens_distort_bilinear");
  gd->kernel_lens_distort_bicubic = dt_opencl_create_kernel(program, "lens_distort_bicubic");
  gd->kernel_lens_distort_mitchell = dt_opencl_create_kernel(program, "lens_distort_mitchell");
  gd->kernel_lens_vignette = dt_opencl_create_kernel(program, "lens_vignette");
  gd->kernel_md_vignette = dt_opencl_create_kernel(program, "md_vignette");
  gd->kernel_md_lens_correction = dt_opencl_create_kernel(program, "md_lens_correction");

  lfDatabase *dt_iop_lensfun_db = new lfDatabase;
  gd->db = (lfDatabase *)dt_iop_lensfun_db;

#if defined(__MACH__) || defined(__APPLE__)
#else
  if(dt_iop_lensfun_db->Load() != LF_NO_ERROR)
#endif
  {
    char datadir[PATH_MAX] = { 0 };
    dt_loc_get_datadir(datadir, sizeof(datadir));

    // get parent directory
    GFile *file = g_file_parse_name(datadir);
    GFile *parent = g_file_get_parent(file);
    gchar *path = g_file_get_path(parent);
    g_object_unref(parent);
    g_object_unref(file);
#ifdef LF_MAX_DATABASE_VERSION
    gchar *sysdbpath = g_build_filename(path, "lensfun", "version_" STR(LF_MAX_DATABASE_VERSION), (char *)NULL);
#endif

#ifdef LF_0395
    const long userdbts = dt_iop_lensfun_db->ReadTimestamp(dt_iop_lensfun_db->UserUpdatesLocation);
    const long sysdbts = dt_iop_lensfun_db->ReadTimestamp(sysdbpath);
    const char *dbpath = userdbts > sysdbts ? dt_iop_lensfun_db->UserUpdatesLocation : sysdbpath;
    if(dt_iop_lensfun_db->Load(dbpath) != LF_NO_ERROR)
      fprintf(stderr, "[iop_lens]: could not load lensfun database in `%s'!\n", dbpath);
    else
      dt_iop_lensfun_db->Load(dt_iop_lensfun_db->UserLocation);
#else
    // code for older lensfun preserved as-is
#ifdef LF_MAX_DATABASE_VERSION
    dt_free(dt_iop_lensfun_db->HomeDataDir);
    dt_iop_lensfun_db->HomeDataDir = g_strdup(sysdbpath);
    if(dt_iop_lensfun_db->Load() != LF_NO_ERROR)
    {
      fprintf(stderr, "[iop_lens]: could not load lensfun database in `%s'!\n", sysdbpath);
#endif
      dt_free(dt_iop_lensfun_db->HomeDataDir);
      dt_iop_lensfun_db->HomeDataDir = g_build_filename(path, "lensfun", (char *)NULL);
      if(dt_iop_lensfun_db->Load() != LF_NO_ERROR)
        fprintf(stderr, "[iop_lens]: could not load lensfun database in `%s'!\n", dt_iop_lensfun_db->HomeDataDir);
#ifdef LF_MAX_DATABASE_VERSION
    }
#endif
#endif

#ifdef LF_MAX_DATABASE_VERSION
    dt_free(sysdbpath);
#endif
    dt_free(path);
  }
}

static float get_autoscale(dt_iop_module_t *self, dt_iop_lensfun_params_t *p, const lfCamera *camera);

void reload_defaults(dt_iop_module_t *module)
{
  char *new_lens;
  const dt_image_t *img = &module->dev->image_storage;

  // reload image specific stuff
  // get all we can from exif:
  auto d = (dt_iop_lensfun_params_t *)module->default_params;

  new_lens = _lens_sanitize(img->exif_lens);
  g_strlcpy(d->lens, new_lens, sizeof(d->lens));
  dt_free(new_lens);
  g_strlcpy(d->camera, img->exif_model, sizeof(d->camera));
  d->crop = img->exif_crop;
  d->aperture = img->exif_aperture;
  d->focal = img->exif_focal_length;
  d->scale = 1.0;
  d->modify_flags = LF_MODIFY_TCA | LF_MODIFY_VIGNETTING | LF_MODIFY_DISTORTION |
                    LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE;
  // if we did not find focus_distance in EXIF, lets default to 1000
  d->distance = img->exif_focus_distance == 0.0f ? 1000.0f : img->exif_focus_distance;
  d->target_geom = LF_RECTILINEAR;

  if(dt_image_is_monochrome(img))
    d->modify_flags &= ~LF_MODIFY_TCA;

  d->distortion_method = (!IS_NULL_PTR(img) && dt_embedded_lens_has_distortion(img))
    ? dt_iop_lens_correction_source_t::EMBEDDED
    : dt_iop_lens_correction_source_t::LENSFUN_DB;
  d->vignetting_method = (!IS_NULL_PTR(img) && dt_embedded_lens_has_vignetting(img))
    ? dt_iop_lens_correction_source_t::EMBEDDED
    : dt_iop_lens_correction_source_t::LENSFUN_DB;
  d->tca_method = dt_iop_lens_tca_source_t::LENSFUN_DB;

  // init crop from db:
  char model[100]; // truncate often complex descriptions.
  g_strlcpy(model, img->exif_model, sizeof(model));
  int cnt = 0;
  for(char *c = model; c < model + 100 && *c != '\0'; c++)
    if(*c == ' ')
      if(++cnt == 2) *c = '\0';
  if(img->exif_maker[0] || model[0])
  {
    auto gd = (dt_iop_lensfun_global_data_t *)module->global_data;

    // just to be sure
    if(IS_NULL_PTR(gd) || IS_NULL_PTR(gd->db)) return;

    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **cam = gd->db->FindCamerasExt(img->exif_maker, img->exif_model, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam)
    {
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      const lfLens **lens = gd->db->FindLenses(cam[0], NULL, d->lens, 0);
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

      if(!lens && islower(cam[0]->Mount[0]))
      {
        /*
         * This is a fixed-lens camera, and LF returned no lens.
         * (reasons: lens is "(65535)" or lens is correct lens name,
         *  but LF have it as "fixed lens")
         *
         * Let's unset lens name and re-run lens query
         */
        g_strlcpy(d->lens, "", sizeof(d->lens));

        dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
        lens = gd->db->FindLenses(cam[0], NULL, d->lens, 0);
        dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
      }

      if(lens)
      {
        int lens_i = 0;

        /*
         * Current SVN lensfun lets you test for a fixed-lens camera by looking
         * at the zeroth character in the mount's name:
         * If it is a lower case letter, it is a fixed-lens camera.
         */
        if(!d->lens[0] && islower(cam[0]->Mount[0]))
        {
          /*
           * no lens info in EXIF, and this is fixed-lens camera,
           * let's find shortest lens model in the list of possible lenses
           */
          size_t min_model_len = SIZE_MAX;
          for(int i = 0; lens[i]; i++)
          {
            if(strlen(lens[i]->Model) < min_model_len)
            {
              min_model_len = strlen(lens[i]->Model);
              lens_i = i;
            }
          }

          // and set lens to it
          g_strlcpy(d->lens, lens[lens_i]->Model, sizeof(d->lens));
        }

        d->target_geom = lens[lens_i]->Type;
        lf_free(lens);
      }

      d->crop = cam[0]->CropFactor;
      d->scale = get_autoscale(module, d, cam[0]);
      module->workflow_enabled = dt_image_needs_rawprepare(img);
      lf_free(cam);
    }
  }

  // The corrections-done message reset lives in gui_update() now (GUI thread, live widget);
  // reload_defaults() stays params-only and never touches gui_data.
}

void cleanup_global(dt_iop_module_so_t *module)
{
  auto gd = (dt_iop_lensfun_global_data_t *)module->data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
  delete dt_iop_lensfun_db;

  dt_opencl_free_kernel(gd->kernel_lens_distort_bilinear);
  dt_opencl_free_kernel(gd->kernel_lens_distort_bicubic);
  dt_opencl_free_kernel(gd->kernel_lens_distort_mitchell);
  dt_opencl_free_kernel(gd->kernel_lens_vignette);
  dt_opencl_free_kernel(gd->kernel_md_vignette);
  dt_opencl_free_kernel(gd->kernel_md_lens_correction);
  dt_free(module->data);
}

/// ############################################################
/// gui stuff: inspired by ufraws lensfun tab:

/* simple function to compute the floating-point precision
   which is enough for "normal use". The criteria is to have
   about 3 leading digits after the initial zeros.  */
static int precision(double x, double adj)
{
  x *= adj;

  if(x == 0) return 1;
  if(x < 1.0)
    if(x < 0.1)
      if(x < 0.01)
        return 5;
      else
        return 4;
    else
      return 3;
  else if(x < 100.0)
    if(x < 10.0)
      return 2;
    else
      return 1;
  else
    return 0;
}

/* -- ufraw ptr array functions -- */

static int ptr_array_insert_sorted(GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  g_ptr_array_set_size(array, length + 1);
  const void **root = (const void **)array->pdata;

  int m = 0;
  int l = 0;
  int r = length - 1;

  // Skip trailing NULL, if any
  if(l <= r && !root[r]) r--;

  while(l <= r)
  {
    m = (l + r) / 2;
    int cmp = compare(root[m], item);

    if(cmp == 0)
    {
      ++m;
      goto done;
    }
    else if(cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }
  if(r == m) m++;

done:
  memmove(root + m + 1, root + m, sizeof(void *) * (length - m));
  root[m] = item;
  return m;
}

static int ptr_array_find_sorted(const GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  void **root = array->pdata;

  int l = 0;
  int r = length - 1;
  int m = 0;
  int cmp = 0;

  if(!length) return -1;

  // Skip trailing NULL, if any
  if(!root[r]) r--;

  while(l <= r)
  {
    m = (l + r) / 2;
    cmp = compare(root[m], item);

    if(cmp == 0)
      return m;
    else if(cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }

  return -1;
}

static void ptr_array_insert_index(GPtrArray *array, const void *item, int index)
{
  const void **root;
  int length = array->len;
  g_ptr_array_set_size(array, length + 1);
  root = (const void **)array->pdata;
  memmove(root + index + 1, root + index, sizeof(void *) * (length - index));
  root[index] = item;
}

/* -- end ufraw ptr array functions -- */

/* -- camera -- */

static void camera_set(dt_iop_module_t *self, const lfCamera *cam)
{
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  gchar *fm;
  const char *maker, *model, *variant;
  char _variant[100];

  if(IS_NULL_PTR(cam))
  {
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_selection.camera_model))), "");
    gtk_widget_set_tooltip_text(GTK_WIDGET(g->lens_selection.camera_model), "");
    return;
  }

  g_strlcpy(p->camera, cam->Model, sizeof(p->camera));
  p->crop = cam->CropFactor;
  g->lens_selection.camera = cam;

  maker = lf_mlstr_get(cam->Maker);
  model = lf_mlstr_get(cam->Model);
  variant = lf_mlstr_get(cam->Variant);

  if(model)
  {
    if(maker)
      fm = g_strdup_printf("%s, %s", maker, model);
    else
      fm = g_strdup_printf("%s", model);
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_selection.camera_model))), fm);
    dt_free(fm);
  }

  if(variant)
    snprintf(_variant, sizeof(_variant), " (%s)", variant);
  else
    _variant[0] = 0;

  fm = g_strdup_printf(_("maker:\t\t%s\n"
                         "model:\t\t%s%s\n"
                         "mount:\t\t%s\n"
                         "crop factor:\t%.1f"),
                       maker, model, _variant, cam->Mount, cam->CropFactor);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->lens_selection.camera_model), fm);
  dt_free(fm);
}

static void camera_menu_select(GtkMenuItem *menuitem, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  camera_set(self, (lfCamera *)g_object_get_data(G_OBJECT(menuitem), "lfCamera"));
  if(dt_gui_widgets_suppressed()) return;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  p->has_been_set = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void camera_menu_fill(dt_iop_module_t *self, const lfCamera *const *camlist)
{
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if(g->lens_selection.camera_menu)
  {
    gtk_widget_destroy(GTK_WIDGET(g->lens_selection.camera_menu));
    g->lens_selection.camera_menu = nullptr;
  }

  /* Count all existing camera makers and create a sorted list */
  makers = g_ptr_array_new();
  submenus = g_ptr_array_new();
  for(i = 0; camlist[i]; i++)
  {
    GtkWidget *submenu;
    GtkWidget *item;
    const char *m = lf_mlstr_get(camlist[i]->Maker);
    int idx = ptr_array_find_sorted(makers, m, (GCompareFunc)g_utf8_collate);
    if(idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = ptr_array_insert_sorted(makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for cameras by this maker */
      submenu = gtk_menu_new();
      ptr_array_insert_index(submenus, submenu, idx);
    }

    submenu = (GtkWidget *)g_ptr_array_index(submenus, idx);
    /* Append current camera name to the submenu */
    m = lf_mlstr_get(camlist[i]->Model);
    if(!camlist[i]->Variant)
      item = gtk_menu_item_new_with_label(m);
    else
    {
      gchar *fm = g_strdup_printf("%s (%s)", m, camlist[i]->Variant);
      item = gtk_menu_item_new_with_label(fm);
      dt_free(fm);
    }
    gtk_widget_show(item);
    g_object_set_data(G_OBJECT(item), "lfCamera", (void *)camlist[i]);
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(camera_menu_select), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
  }

  g->lens_selection.camera_menu = GTK_MENU(gtk_menu_new());
  for(i = 0; i < makers->len; i++)
  {
    GtkWidget *item = (GtkWidget *)gtk_menu_item_new_with_label((const gchar *)g_ptr_array_index(makers, i));
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->lens_selection.camera_menu), item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), (GtkWidget *)g_ptr_array_index(submenus, i));
  }

  g_ptr_array_free(submenus, TRUE);
  g_ptr_array_free(makers, TRUE);
}

static void parse_model(const char *txt, char *model, size_t sz_model)
{
  while(txt[0] && isspace(txt[0])) txt++;
  size_t len = strlen(txt);
  if(len > sz_model - 1) len = sz_model - 1;
  memcpy(model, txt, len);
  model[len] = 0;
}

static void camera_menusearch_clicked(GtkWidget *button, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  (void)button;

  const lfCamera *const *camlist;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  camlist = dt_iop_lensfun_db->GetCameras();
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(IS_NULL_PTR(camlist)) return;
  camera_menu_fill(self, camlist);

  dt_gui_menu_popup(GTK_MENU(g->lens_selection.camera_menu), button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static void camera_autosearch_clicked(GtkWidget *button, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  char make[200];
  char model[200];
  const gchar *txt = (const gchar *)((dt_iop_lensfun_params_t *)self->default_params)->camera;

  (void)button;

  if(txt[0] == '\0')
  {
    const lfCamera *const *camlist;
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    camlist = dt_iop_lensfun_db->GetCameras();
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(IS_NULL_PTR(camlist)) return;
    camera_menu_fill(self, camlist);
  }
  else
  {
    make[0] = '\0';
    parse_model(txt, model, sizeof(model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **camlist = dt_iop_lensfun_db->FindCamerasExt(make, model, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(IS_NULL_PTR(camlist)) return;
    camera_menu_fill(self, camlist);
    lf_free(camlist);
  }

  dt_gui_menu_popup(GTK_MENU(g->lens_selection.camera_menu), button, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

/* -- end camera -- */

static void lens_comboentry_focal_update(GtkWidget *widget, dt_iop_module_t *self)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->focal);
  p->has_been_set = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void lens_comboentry_aperture_update(GtkWidget *widget, dt_iop_module_t *self)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->aperture);
  p->has_been_set = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void lens_comboentry_distance_update(GtkWidget *widget, dt_iop_module_t *self)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->distance);
  p->has_been_set = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void delete_children(GtkWidget *widget, gpointer data)
{
  (void)data;
  gtk_widget_destroy(widget);
}

static void lens_set(dt_iop_module_t *self, const lfLens *lens)
{
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  auto p = (dt_iop_lensfun_params_t *)self->params;

  gchar *fm;
  const char *maker, *model;
  unsigned i;
  gdouble focal_values[]
      = { -INFINITY, 4.5, 8,   10,  12,  14,  15,  16,  17,  18,  20,  24,  28,   30,      31,  35,
          38,        40,  43,  45,  50,  55,  60,  70,  75,  77,  80,  85,  90,   100,     105, 110,
          120,       135, 150, 200, 210, 240, 250, 300, 400, 500, 600, 800, 1000, INFINITY };
  gdouble aperture_values[]
      = { -INFINITY, 0.7, 0.8, 0.9, 1, 1.1, 1.2, 1.4, 1.8, 2,  2.2, 2.5, 2.8, 3.2, 3.4, 4,  4.5, 5.0,
          5.6,       6.3, 7.1, 8,   9, 10,  11,  13,  14,  16, 18,  20,  22,  25,  29,  32, 38,  INFINITY };

  if(!lens)
  {
    gtk_widget_set_sensitive(g->per_correction.distortion_source, FALSE);
    gtk_widget_set_sensitive(g->per_correction.vignetting_source, FALSE);
    gtk_widget_set_sensitive(g->per_correction.tca_source, FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.target_geom), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.scale), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.tca_r), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.tca_b), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->status.message), FALSE);

    g->status.trouble = TRUE;
    return;
  }
  else
  {
    gtk_widget_set_sensitive(g->per_correction.distortion_source, TRUE);
    gtk_widget_set_sensitive(g->per_correction.vignetting_source, TRUE);
    gtk_widget_set_sensitive(g->per_correction.tca_source, TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.target_geom), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.scale), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.tca_r), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.tca_b), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->status.message), TRUE);

    g->status.trouble = FALSE;
  }

  maker = lf_mlstr_get(lens->Maker);
  model = lf_mlstr_get(lens->Model);

  g_strlcpy(p->lens, lens->Model, sizeof(p->lens));

  if(model)
  {
    if(maker)
      fm = g_strdup_printf("%s, %s", maker, model);
    else
      fm = g_strdup_printf("%s", model);
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_selection.lens_model))), fm);
    dt_free(fm);
  }

  char focal[100], aperture[100], mounts[200];

  if(lens->MinFocal < lens->MaxFocal)
    snprintf(focal, sizeof(focal), "%g-%gmm", lens->MinFocal, lens->MaxFocal);
  else
    snprintf(focal, sizeof(focal), "%gmm", lens->MinFocal);
  if(lens->MinAperture < lens->MaxAperture)
    snprintf(aperture, sizeof(aperture), "%g-%g", lens->MinAperture, lens->MaxAperture);
  else
    snprintf(aperture, sizeof(aperture), "%g", lens->MinAperture);

  mounts[0] = 0;
#ifdef LF_0395
  const char* const* mount_names = lens->GetMountNames();
  i = 0;
  while (mount_names && *mount_names) {
    if(i > 0) g_strlcat(mounts, ", ", sizeof(mounts));
    g_strlcat(mounts, *mount_names, sizeof(mounts));
    i++;
    mount_names++;
  }
#else
  if(lens->Mounts)
    for(i = 0; lens->Mounts[i]; i++)
    {
      if(i > 0) g_strlcat(mounts, ", ", sizeof(mounts));
      g_strlcat(mounts, lens->Mounts[i], sizeof(mounts));
    }
#endif
  fm = g_strdup_printf(_("maker:\t\t%s\n"
                         "model:\t\t%s\n"
                         "focal range:\t%s\n"
                         "aperture:\t%s\n"
                         "crop factor:\t%.1f\n"
                         "type:\t\t%s\n"
                         "mounts:\t%s"),
                       maker ? maker : "?", model ? model : "?", focal, aperture,
#ifdef LF_0395
                       g->lens_selection.camera->CropFactor,
#else
                       lens->CropFactor,
#endif
                       lfLens::GetLensTypeDesc(lens->Type, NULL), mounts);

  gtk_widget_set_tooltip_text(GTK_WIDGET(g->lens_selection.lens_model), fm);
  dt_free(fm);

  /* Create the focal/aperture/distance combo boxes */
  gtk_container_foreach(GTK_CONTAINER(g->lens_selection.lens_param_box), delete_children, nullptr);

  int ffi = 1;
  int fli = -1;
  for(i = 1; i < sizeof(focal_values) / sizeof(gdouble) - 1; i++)
  {
    if(focal_values[i] < lens->MinFocal) ffi = i + 1;
    if(focal_values[i] > lens->MaxFocal && fli == -1) fli = i;
  }
  if(focal_values[ffi] > lens->MinFocal)
  {
    focal_values[ffi - 1] = lens->MinFocal;
    ffi--;
  }
  if(lens->MaxFocal == 0 || fli < 0) fli = sizeof(focal_values) / sizeof(gdouble) - 2;
  if(focal_values[fli + 1] < lens->MaxFocal)
  {
    focal_values[fli + 1] = lens->MaxFocal;
    ffi++;
  }
  if(fli < ffi) fli = ffi + 1;

  GtkWidget *w;
  char txt[30];

  // focal length
  w = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(w, N_("mm"));
  gtk_widget_set_tooltip_text(w, _("focal length (mm)"));
  snprintf(txt, sizeof(txt), "%.*f", precision(p->focal, 10.0), p->focal);
  dt_bauhaus_combobox_add(w, txt);
  for(int k = 0; k < fli - ffi; k++)
  {
    snprintf(txt, sizeof(txt), "%.*f", precision(focal_values[ffi + k], 10.0), focal_values[ffi + k]);
    dt_bauhaus_combobox_add(w, txt);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(lens_comboentry_focal_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_selection.lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->lens_selection.cbe[0] = w;

  // f-stop
  ffi = 1;
  fli = sizeof(aperture_values) / sizeof(gdouble) - 1;
  for(i = 1; i < sizeof(aperture_values) / sizeof(gdouble) - 1; i++)
    if(aperture_values[i] < lens->MinAperture) ffi = i + 1;
  if(aperture_values[ffi] > lens->MinAperture)
  {
    aperture_values[ffi - 1] = lens->MinAperture;
    ffi--;
  }

  w = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(w, N_("f"));
  gtk_widget_set_tooltip_text(w, _("f-number (aperture)"));
  snprintf(txt, sizeof(txt), "%.*f", precision(p->aperture, 10.0), p->aperture);
  dt_bauhaus_combobox_add(w, txt);
  for(int k = 0; k < fli - ffi; k++)
  {
    snprintf(txt, sizeof(txt), "%.*f", precision(aperture_values[ffi + k], 10.0), aperture_values[ffi + k]);
    dt_bauhaus_combobox_add(w, txt);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(lens_comboentry_aperture_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_selection.lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->lens_selection.cbe[1] = w;

  w = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(w, N_("d"));
  gtk_widget_set_tooltip_text(w, _("distance to subject"));
  snprintf(txt, sizeof(txt), "%.*f", precision(p->distance, 10.0), p->distance);
  dt_bauhaus_combobox_add(w, txt);
  float val = 0.25f;
  for(int k = 0; k < 25; k++)
  {
    if(val > 1000.0f) val = 1000.0f;
    snprintf(txt, sizeof(txt), "%.*f", precision(val, 10.0), val);
    dt_bauhaus_combobox_add(w, txt);
    if(val >= 1000.0f) break;
    val *= sqrtf(2.0f);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(lens_comboentry_distance_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_selection.lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->lens_selection.cbe[2] = w;

  gtk_widget_show_all(g->lens_selection.lens_param_box);
}

static void lens_menu_select(GtkMenuItem *menuitem, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  lens_set(self, (lfLens *)g_object_get_data(G_OBJECT(menuitem), "lfLens"));
  if(dt_gui_widgets_suppressed()) return;
  p->has_been_set = 0;
  const float scale = get_autoscale(self, p, g->lens_selection.camera);
  dt_bauhaus_slider_set(g->lensfun_controls.scale, scale);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void lens_menu_fill(dt_iop_module_t *self, const lfLens *const *lenslist)
{
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if(g->lens_selection.lens_menu)
  {
    gtk_widget_destroy(GTK_WIDGET(g->lens_selection.lens_menu));
    g->lens_selection.lens_menu = nullptr;
  }

  /* Count all existing lens makers and create a sorted list */
  makers = g_ptr_array_new();
  submenus = g_ptr_array_new();
  for(i = 0; lenslist[i]; i++)
  {
    GtkWidget *submenu;
    GtkWidget *item;
    const char *m = lf_mlstr_get(lenslist[i]->Maker);
    int idx = ptr_array_find_sorted(makers, m, (GCompareFunc)g_utf8_collate);
    if(idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = ptr_array_insert_sorted(makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for lenses by this maker */
      submenu = gtk_menu_new();
      ptr_array_insert_index(submenus, submenu, idx);
    }

    submenu = (GtkWidget *)g_ptr_array_index(submenus, idx);
    /* Append current lens name to the submenu */
    item = gtk_menu_item_new_with_label(lf_mlstr_get(lenslist[i]->Model));
    gtk_widget_show(item);
    g_object_set_data(G_OBJECT(item), "lfLens", (void *)lenslist[i]);
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(lens_menu_select), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
  }

  g->lens_selection.lens_menu = GTK_MENU(gtk_menu_new());
  for(i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label((const gchar *)g_ptr_array_index(makers, i));
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->lens_selection.lens_menu), item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), (GtkWidget *)g_ptr_array_index(submenus, i));
  }

  g_ptr_array_free(submenus, TRUE);
  g_ptr_array_free(makers, TRUE);
}

static void lens_menusearch_clicked(GtkWidget *button, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfLens **lenslist;

  (void)button;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lenslist = dt_iop_lensfun_db->FindLenses(g->lens_selection.camera, NULL, NULL, LF_SEARCH_SORT_AND_UNIQUIFY);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(IS_NULL_PTR(lenslist)) return;
  lens_menu_fill(self, lenslist);
  lf_free(lenslist);

  dt_gui_menu_popup(GTK_MENU(g->lens_selection.lens_menu), button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static void lens_autosearch_clicked(GtkWidget *button, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfLens **lenslist;
  char model[200];
  const gchar *txt = ((dt_iop_lensfun_params_t *)self->default_params)->lens;

  (void)button;

  parse_model(txt, model, sizeof(model));
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lenslist = dt_iop_lensfun_db->FindLenses(g->lens_selection.camera, NULL,
                                           model[0] ? model : NULL, LF_SEARCH_SORT_AND_UNIQUIFY);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if(IS_NULL_PTR(lenslist)) return;
  lens_menu_fill(self, lenslist);
  lf_free(lenslist);

  dt_gui_menu_popup(GTK_MENU(g->lens_selection.lens_menu), button, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

/* -- end lens -- */

static void target_geometry_changed(GtkWidget *widget, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto p = (dt_iop_lensfun_params_t *)self->params;

  int pos = dt_bauhaus_combobox_get(widget);
  p->target_geom = (lfLensType)(pos + LF_UNKNOWN + 1);
  p->has_been_set = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void vignetting_method_changed(GtkWidget *widget, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const int val = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget));
  p->vignetting_method = (dt_iop_lens_correction_source_t)val;
  p->has_been_set = 0;
  dt_iop_gui_changed(self, widget, nullptr);
}

static void distortion_method_changed(GtkWidget *widget, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const int val = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget));
  p->distortion_method = (dt_iop_lens_correction_source_t)val;
  p->has_been_set = 0;
  dt_iop_gui_changed(self, widget, nullptr);
}

static void tca_method_changed(GtkWidget *widget, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const int val = GPOINTER_TO_INT(dt_bauhaus_combobox_get_data(widget));
  p->tca_method = (dt_iop_lens_tca_source_t)val;
  p->has_been_set = 0;
  dt_iop_gui_changed(self, widget, nullptr);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);

  p->distortion_method  = (dt_iop_lens_correction_source_t)CLAMP((int)p->distortion_method, 0, 2);
  p->vignetting_method  = (dt_iop_lens_correction_source_t)CLAMP((int)p->vignetting_method, 0, 2);
  p->tca_method         = (dt_iop_lens_tca_source_t)CLAMP((int)p->tca_method, 0, 2);
  dt_bauhaus_combobox_set_from_value(g->per_correction.distortion_source, (int)p->distortion_method);
  dt_bauhaus_combobox_set_from_value(g->per_correction.vignetting_source, (int)p->vignetting_method);
  dt_bauhaus_combobox_set_from_value(g->per_correction.tca_source,        (int)p->tca_method);

  const gboolean tca_manual = tca_show_manual_sliders(p->tca_method) && !monochrome;
  gtk_widget_set_visible(g->lensfun_controls.tca_r, tca_manual);
  gtk_widget_set_visible(g->lensfun_controls.tca_b, tca_manual);

  const gboolean geom_active = (p->distortion_method == dt_iop_lens_correction_source_t::LENSFUN_DB);
  gtk_widget_set_visible(g->lensfun_controls.target_geom, geom_active);

  if(w)
  {
    p->has_been_set = 0;
  }
}


static float get_autoscale(dt_iop_module_t *self, dt_iop_lensfun_params_t *p, const lfCamera *camera)
{
  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
  float scale = 1.0;
  if(p->lens[0] != '\0')
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist
        = dt_iop_lensfun_db->FindLenses(camera, NULL, p->lens, 0);
    if(lenslist)
    {
      const dt_image_t *img = &(self->dev->image_storage);

      const int iwd = img->width - img->crop_x - img->crop_width,
                iht = img->height - img->crop_y - img->crop_height;

      const int modify_flags = per_axis_modify_flags(p->distortion_method,
                                                      p->vignetting_method,
                                                      p->tca_method,
                                                      dt_image_is_monochrome(img));

#if defined(__GNUC__) && (__GNUC__ > 7)
      const dt_iop_lensfun_data_t d =
        {
         .lensfun =
           {
            .lens         = (lfLens *)lenslist[0],
            .modify_flags = modify_flags,
            .scale        = 1.0f,
            .crop         = p->crop,
            .focal        = p->focal,
            .aperture     = p->aperture,
            .distance     = p->distance,
            .target_geom  = p->target_geom,
            .custom_tca   = { .Model = LF_TCA_MODEL_NONE }
           }
        };
#else
      dt_iop_lensfun_data_t d;
      d.lensfun.lens             = (lfLens *)lenslist[0];
      d.lensfun.modify_flags     = modify_flags;
      d.lensfun.scale            = 1.0f;
      d.lensfun.crop             = p->crop;
      d.lensfun.focal            = p->focal;
      d.lensfun.aperture         = p->aperture;
      d.lensfun.distance         = p->distance;
      d.lensfun.target_geom      = p->target_geom;
      d.lensfun.custom_tca.Model = LF_TCA_MODEL_NONE;
#endif

      lfModifier *modifier = get_modifier(NULL, iwd, iht, &d, modify_flags);

      scale = modifier->GetAutoScale(FALSE);
      delete modifier;
    }
    lf_free(lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  return scale;
}

static void autoscale_pressed(GtkWidget *button, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const float scale = get_autoscale(self, p, g->lens_selection.camera);
  p->has_been_set = 0;
  dt_bauhaus_slider_set(g->lensfun_controls.scale, scale);
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void corrections_done(gpointer instance, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  if(IS_NULL_PTR(g)) return;
  if(!IS_NULL_PTR(self->dev) && self->dev->gui_attached)
  {
    auto p = (dt_iop_lensfun_params_t *)self->params;
    const gboolean monochrome = dt_image_is_monochrome(&self->dev->image_storage);
    const char *status = corrections_status_string(
        p->distortion_method, p->vignetting_method, p->tca_method, monochrome);
    dt_iop_gui_enter_critical_section(self);
    gtk_label_set_text(g->status.message, status);
    dt_iop_gui_leave_critical_section(self);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = IOP_GUI_ALLOC(lensfun);

  g->lens_selection.camera = nullptr;
  g->lens_selection.camera_menu = nullptr;
  g->lens_selection.lens_menu = nullptr;

  dt_iop_gui_enter_critical_section(self);
  g->status.corrections_done = -1;
  dt_iop_gui_leave_critical_section(self);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
    gtk_widget_set_name(self->widget, "lens-module");

    // Position 1: Camera and lens selection widgets — always visible
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
    g->lens_selection.camera_model = dt_iop_button_new(self, N_("camera model"), G_CALLBACK(camera_menusearch_clicked), FALSE, 0,
                                        (GdkModifierType)0, NULL, 0, hbox);
    g->lens_selection.find_camera_button
        = dt_iop_button_new(self, N_("find camera"), G_CALLBACK(camera_autosearch_clicked), FALSE, 0,
                            (GdkModifierType)0, dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
    dt_gui_add_class(g->lens_selection.find_camera_button, "dt_big_btn_canvas");
    gtk_box_pack_start(GTK_BOX(hbox), g->lens_selection.find_camera_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
    g->lens_selection.lens_model = dt_iop_button_new(self, N_("lens model"), G_CALLBACK(lens_menusearch_clicked), FALSE, 0,
                                       (GdkModifierType)0, NULL, 0, hbox);
    g->lens_selection.find_lens_button
        = dt_iop_button_new(self, N_("find lens"), G_CALLBACK(lens_autosearch_clicked), FALSE, 0,
                            (GdkModifierType)0, dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
    dt_gui_add_class(g->lens_selection.find_lens_button, "dt_big_btn_canvas");
    gtk_box_pack_start(GTK_BOX(hbox), g->lens_selection.find_lens_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

    g->lens_selection.lens_param_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
    gtk_box_pack_start(GTK_BOX(self->widget), g->lens_selection.lens_param_box, TRUE, TRUE, 0);

    // Position 2: Unified scale slider — always visible
    g->lensfun_controls.scale = dt_bauhaus_slider_from_params(self, N_("scale"));
    dt_bauhaus_slider_set_digits(g->lensfun_controls.scale, 3);
    dt_bauhaus_widget_set_quad_paint(g->lensfun_controls.scale, dtgtk_cairo_paint_refresh, 0, NULL);
    g_signal_connect(G_OBJECT(g->lensfun_controls.scale), "quad-pressed", G_CALLBACK(autoscale_pressed), self);
    gtk_widget_set_tooltip_text(g->lensfun_controls.scale, _("auto scale"));

    // Position 3: Vignetting source combobox
    {
      const dt_image_t *img = !IS_NULL_PTR(self->dev) ? &self->dev->image_storage : nullptr;
      const gboolean has_vign = !IS_NULL_PTR(img) && dt_embedded_lens_has_vignetting(img);

      g->per_correction.vignetting_source = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
      dt_bauhaus_widget_set_label(g->per_correction.vignetting_source, N_("vignetting"));
      gtk_box_pack_start(GTK_BOX(self->widget), g->per_correction.vignetting_source, TRUE, TRUE, 0);
      gtk_widget_set_tooltip_text(g->per_correction.vignetting_source, _("source of vignetting correction"));

      const char *labels[3];
      int values[3];
      const int n = vignetting_selector_entries(has_vign, labels, values);
      for(int i = 0; i < n; i++) dt_bauhaus_combobox_add_full(g->per_correction.vignetting_source,
          _(labels[i]), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(values[i]), NULL, TRUE);
      g_signal_connect(G_OBJECT(g->per_correction.vignetting_source), "value-changed",
                       G_CALLBACK(vignetting_method_changed), (gpointer)self);
    }

    // Position 4: Distortion source combobox
    {
      const dt_image_t *img = !IS_NULL_PTR(self->dev) ? &self->dev->image_storage : nullptr;
      const gboolean has_dist = !IS_NULL_PTR(img) && dt_embedded_lens_has_distortion(img);

      g->per_correction.distortion_source = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
      dt_bauhaus_widget_set_label(g->per_correction.distortion_source, N_("distortion"));
      gtk_box_pack_start(GTK_BOX(self->widget), g->per_correction.distortion_source, TRUE, TRUE, 0);
      gtk_widget_set_tooltip_text(g->per_correction.distortion_source, _("source of distortion correction"));

      const char *labels[3];
      int values[3];
      const int n = distortion_selector_entries(has_dist, labels, values);
      for(int i = 0; i < n; i++) dt_bauhaus_combobox_add_full(g->per_correction.distortion_source,
          _(labels[i]), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(values[i]), NULL, TRUE);
      g_signal_connect(G_OBJECT(g->per_correction.distortion_source), "value-changed",
                       G_CALLBACK(distortion_method_changed), (gpointer)self);
    }

    // Position 5: Geometry combobox (target_geom) — visible when distortion is LENSFUN_DB
    g->lensfun_controls.target_geom = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
    dt_bauhaus_widget_set_label(g->lensfun_controls.target_geom, N_("geometry"));
    gtk_box_pack_start(GTK_BOX(self->widget), g->lensfun_controls.target_geom, TRUE, TRUE, 0);
    gtk_widget_set_tooltip_text(g->lensfun_controls.target_geom, _("target geometry"));
    dt_bauhaus_combobox_add(g->lensfun_controls.target_geom, _("rectilinear"));
    dt_bauhaus_combobox_add(g->lensfun_controls.target_geom, _("fish-eye"));
    dt_bauhaus_combobox_add(g->lensfun_controls.target_geom, _("panoramic"));
    dt_bauhaus_combobox_add(g->lensfun_controls.target_geom, _("equirectangular"));
#if LF_VERSION >= ((0 << 24) | (2 << 16) | (6 << 8) | 0)
    dt_bauhaus_combobox_add(g->lensfun_controls.target_geom, _("orthographic"));
    dt_bauhaus_combobox_add(g->lensfun_controls.target_geom, _("stereographic"));
    dt_bauhaus_combobox_add(g->lensfun_controls.target_geom, _("equisolid angle"));
    dt_bauhaus_combobox_add(g->lensfun_controls.target_geom, _("thoby fish-eye"));
#endif
    g_signal_connect(G_OBJECT(g->lensfun_controls.target_geom), "value-changed", G_CALLBACK(target_geometry_changed),
                     (gpointer)self);

    // Position 6: TCA source combobox
    {
      g->per_correction.tca_source = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
      dt_bauhaus_widget_set_label(g->per_correction.tca_source, N_("TCA"));
      gtk_box_pack_start(GTK_BOX(self->widget), g->per_correction.tca_source, TRUE, TRUE, 0);
      gtk_widget_set_tooltip_text(g->per_correction.tca_source, _("source of TCA correction"));

      const char *labels[3];
      int values[3];
      const int n = tca_selector_entries(FALSE, labels, values);
      for(int i = 0; i < n; i++) dt_bauhaus_combobox_add_full(g->per_correction.tca_source,
          _(labels[i]), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(values[i]), NULL, TRUE);
      g_signal_connect(G_OBJECT(g->per_correction.tca_source), "value-changed",
                       G_CALLBACK(tca_method_changed), (gpointer)self);
    }

    // Position 7-8: TCA manual sliders
    g->lensfun_controls.tca_r = dt_bauhaus_slider_from_params(self, "tca_r");
    dt_bauhaus_slider_set_digits(g->lensfun_controls.tca_r, 5);
    gtk_widget_set_tooltip_text(g->lensfun_controls.tca_r, _("Transversal Chromatic Aberration red"));

    g->lensfun_controls.tca_b = dt_bauhaus_slider_from_params(self, "tca_b");
    dt_bauhaus_slider_set_digits(g->lensfun_controls.tca_b, 5);
    gtk_widget_set_tooltip_text(g->lensfun_controls.tca_b, _("Transversal Chromatic Aberration blue"));

    // Position 9: Status label
    GtkBox *hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));
    GtkWidget *label = gtk_label_new(_("corrections done: "));
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_tooltip_text(label, _("which corrections have actually been done"));
    gtk_box_pack_start(GTK_BOX(hbox1), label, FALSE, FALSE, 0);
    g->status.message = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(GTK_LABEL(g->status.message), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->status.message), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox1), TRUE, TRUE, 0);

    DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                              G_CALLBACK(corrections_done), self);
}

void gui_update(struct dt_iop_module_t *self)
{
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  auto p = (dt_iop_lensfun_params_t *)self->params;

  if(p->has_been_set == 1)
  {
    if((int)p->vignetting_method < 0 || (int)p->vignetting_method > 2)
      p->vignetting_method = dt_iop_lens_correction_source_t::LENSFUN_DB;
    if((int)p->distortion_method < 0 || (int)p->distortion_method > 2)
      p->distortion_method = dt_iop_lens_correction_source_t::LENSFUN_DB;
    if((int)p->tca_method < 0 || (int)p->tca_method > 2)
      p->tca_method = dt_iop_lens_tca_source_t::LENSFUN_DB;

    const dt_iop_lens_correction_source_t saved_vignetting = p->vignetting_method;
    const dt_iop_lens_correction_source_t saved_distortion = p->distortion_method;
    const dt_iop_lens_tca_source_t        saved_tca        = p->tca_method;
    memcpy(self->params, self->default_params, sizeof(dt_iop_lensfun_params_t));
    p->vignetting_method = saved_vignetting;
    p->distortion_method = saved_distortion;
    p->tca_method        = saved_tca;
  }

  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;

  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_selection.camera_model))), p->camera);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_selection.lens_model))), p->lens);
  gtk_widget_set_tooltip_text(g->lens_selection.camera_model, "");
  gtk_widget_set_tooltip_text(g->lens_selection.lens_model, "");

  dt_bauhaus_combobox_set(g->lensfun_controls.target_geom, p->target_geom - LF_UNKNOWN - 1);

  const lfCamera **cam = nullptr;
  g->lens_selection.camera = nullptr;
  if(p->camera[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = dt_iop_lensfun_db->FindCamerasExt(NULL, p->camera, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam)
      camera_set(self, cam[0]);
    else
      camera_set(self, NULL);
    lf_free(cam);
  }
  if(g->lens_selection.camera && p->lens[0])
  {
    char model[200];
    parse_model(p->lens, model, sizeof(model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = dt_iop_lensfun_db->FindLenses(g->lens_selection.camera, NULL,
                                                            model[0] ? model : NULL, 0);
    if(lenslist)
      lens_set(self, lenslist[0]);
    else
      lens_set(self, NULL);
    lf_free(lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  else
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    lens_set(self, NULL);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }

  const dt_image_t *img = &self->dev->image_storage;
  const gboolean has_vign = dt_embedded_lens_has_vignetting(img);
  const gboolean has_dist = dt_embedded_lens_has_distortion(img);

  // Downgrade EMBEDDED to LENSFUN_DB when the current image lacks embedded
  // data for that axis. Runs unconditionally: both auto-applied (has_been_set)
  // and user-explicit params (from v6→v7 migration) carry EMBEDDED values that
  // may not be valid for this image.
  if(p->vignetting_method == dt_iop_lens_correction_source_t::EMBEDDED && !has_vign)
    p->vignetting_method = dt_iop_lens_correction_source_t::LENSFUN_DB;
  if(p->distortion_method == dt_iop_lens_correction_source_t::EMBEDDED && !has_dist)
    p->distortion_method = dt_iop_lens_correction_source_t::LENSFUN_DB;

  auto rebuild_combobox = [&](GtkWidget *combobox, int desired_len,
                               const char *labels[3], const int values[3], int n_labels) {
    const int current_len = dt_bauhaus_combobox_length(combobox);
    if(current_len != desired_len)
    {
      dt_bauhaus_combobox_clear(combobox);
      for(int i = 0; i < n_labels; i++)
        dt_bauhaus_combobox_add_full(combobox, _(labels[i]),
            DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT, GINT_TO_POINTER(values[i]), NULL, TRUE);
    }
  };

  {
    const char *labels[3];
    int values[3];
    const int n = vignetting_selector_entries(has_vign, labels, values);
    rebuild_combobox(g->per_correction.vignetting_source, n, labels, values, n);
  }
  {
    const char *labels[3];
    int values[3];
    const int n = distortion_selector_entries(has_dist, labels, values);
    rebuild_combobox(g->per_correction.distortion_source, n, labels, values, n);
  }
  {
    const char *labels[3];
    int values[3];
    const int n = tca_selector_entries(FALSE, labels, values);
    rebuild_combobox(g->per_correction.tca_source, n, labels, values, n);
  }

  const dt_dev_pixelpipe_iop_t *lens_piece = dt_dev_distort_get_iop_pipe(self->dev->virtual_pipe, self);
  const dt_iop_lensfun_data_t *lens_d
      = (!IS_NULL_PTR(lens_piece)) ? (const dt_iop_lensfun_data_t *)lens_piece->data : NULL;

  int modflags = per_axis_modify_flags(p->distortion_method,
                                        p->vignetting_method,
                                        p->tca_method,
                                        dt_image_is_monochrome(img));

  if(!IS_NULL_PTR(lens_d))
  {
    if(!IS_NULL_PTR(lens_d->lensfun.lens) && !IS_NULL_PTR(lens_d->lensfun.lens->Maker)
       && lens_d->lensfun.crop > 0.0f && lens_piece->buf_in.width > 0 && lens_piece->buf_in.height > 0)
    {
      const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
      const int used_lf_mask = raw_monochrome ? (LF_MODIFY_ALL & ~LF_MODIFY_TCA) : LF_MODIFY_ALL;
      dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
      lfModifier *modifier = get_modifier(&modflags, lens_piece->buf_in.width, lens_piece->buf_in.height,
                                          lens_d, used_lf_mask);
      delete modifier;
      modflags &= LENSFUN_MODFLAG_MASK;
    }
  }

  dt_iop_gui_enter_critical_section(self);
  g->status.corrections_done = modflags;
  dt_iop_gui_leave_critical_section(self);

  gui_changed(self, NULL, NULL);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(corrections_done), self);
  IOP_GUI_FREE;
}

}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
