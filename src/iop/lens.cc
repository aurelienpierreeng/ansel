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

  DT_MODULE_INTROSPECTION(6, dt_iop_lensfun_params_t)

  typedef enum dt_iop_lensfun_modflag_t
  {
    LENSFUN_MODFLAG_NONE = 0,
    LENSFUN_MODFLAG_ALL = LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_VIGNETTING,
    LENSFUN_MODFLAG_DIST_TCA = LF_MODIFY_DISTORTION | LF_MODIFY_TCA,
    LENSFUN_MODFLAG_DIST_VIGN = LF_MODIFY_DISTORTION | LF_MODIFY_VIGNETTING,
    LENSFUN_MODFLAG_TCA_VIGN = LF_MODIFY_TCA | LF_MODIFY_VIGNETTING,
    LENSFUN_MODFLAG_DIST = LF_MODIFY_DISTORTION,
    LENSFUN_MODFLAG_TCA = LF_MODIFY_TCA,
    LENSFUN_MODFLAG_VIGN = LF_MODIFY_VIGNETTING,
    LENSFUN_MODFLAG_MASK = LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_VIGNETTING
  } dt_iop_lensfun_modflag_t;

  typedef struct dt_iop_lensfun_modifier_t
  {
    char name[80];
    int pos; // position in combo box
    int modflag;
  } dt_iop_lensfun_modifier_t;

  // Correction method selector. Exactly two values: the historical Lensfun-DB
  // path, and the embedded-metadata path (camera-provided DNG opcode-list / maker-note
  // correction data). No third value -- there is nothing like an "only vignette" mode.
  enum class dt_iop_lens_method_t
  {
    LENSFUN = 0,          // $DESCRIPTION: "lensfun database"
    EMBEDDED_METADATA = 1 // $DESCRIPTION: "embedded metadata"
  };

  typedef struct dt_iop_lensfun_params_t
  {
    int modify_flags;
    int inverse; // $MIN: 0 $MAX: 1 $DEFAULT: 0 $DESCRIPTION: "mode"
    float scale; // $MIN: 0.1 $MAX: 2.0 $DEFAULT: 1.0
    float crop;
    float focal;
    float aperture;
    float distance;
    lfLensType target_geom; // $DEFAULT: LF_RECTILINEAR $DESCRIPTION: "geometry"
      char camera[128]; // NOSONAR
      char lens[128]; // NOSONAR
    gboolean tca_override; // $DEFAULT: FALSE $DESCRIPTION: "TCA overwrite"
    float tca_r;           // $MIN: 0.99 $MAX: 1.01 $DEFAULT: 1.0 $DESCRIPTION: "TCA red"
    float tca_b;           // $MIN: 0.99 $MAX: 1.01 $DEFAULT: 1.0 $DESCRIPTION: "TCA blue"
    int has_been_set;      // $DEFAULT: 1 1 = auto-detected/defaults, 0 = user explicitly changed
    dt_iop_lens_method_t method; // $DEFAULT: dt_iop_lens_method_t::LENSFUN $DESCRIPTION: "correction method"
    float cor_dist_ft;     // $MIN: 0.5 $MAX: 1.5 $DEFAULT: 1.0 $DESCRIPTION: "distortion strength"
    float cor_vig_ft;      // $MIN: 0.0 $MAX: 2.0 $DEFAULT: 1.0 $DESCRIPTION: "vignetting strength"
    float cor_ca_r_ft;     // $MIN: 0.5 $MAX: 1.5 $DEFAULT: 1.0 $DESCRIPTION: "CA red strength"
    float cor_ca_b_ft;     // $MIN: 0.5 $MAX: 1.5 $DEFAULT: 1.0 $DESCRIPTION: "CA blue strength"
    float scale_md;        // $MIN: 0.5 $MAX: 1.5 $DEFAULT: 1.0 $DESCRIPTION: "image scale"
  } dt_iop_lensfun_params_t;

typedef struct dt_iop_lensfun_gui_data_t
{
  struct
  {
    GtkWidget *modflags, *target_geom, *reverse, *tca_override, *tca_r, *tca_b, *scale;
  } lensfun_controls;
  struct
  {
    GtkWidget *method;
    GtkWidget *cor_dist_ft;
    GtkWidget *cor_vig_ft;
    GtkWidget *cor_ca_r_ft;
    GtkWidget *cor_ca_b_ft;
    GtkWidget *scale_md;
  } embedded_controls;
  struct
  {
    const lfCamera *camera;
    GtkWidget *lens_param_box;
    GtkWidget *camera_model;
    GtkWidget *lens_model;
    GtkMenu *camera_menu, *lens_menu;
    GtkWidget *cbe[3];
    GtkWidget *find_lens_button;
    GtkWidget *find_camera_button;
  } lens_selection;
  struct
  {
    GList *modifiers;
    GtkLabel *message;
    int corrections_done;
    gboolean trouble;
  } status;
} dt_iop_lensfun_gui_data_t;

static_assert(sizeof(dt_iop_lensfun_gui_data_t) == 216,
              "dt_iop_lensfun_gui_data_t size changed -- struct-split integrity failure");

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
    int inverse;
    float scale;
    float crop;
    float focal;
    float aperture;
    float distance;
    lfLensType target_geom;
    gboolean do_nan_checks;
    gboolean tca_override;
    lfLensCalibTCA custom_tca;
  } lensfun;
  struct
  {
    dt_iop_lens_method_t method;
    dt_embedded_lens_finetune_t ft;
    int nc;
    dt_embedded_lens_knots_t knots;
    float scale_md;
  } embedded;
} dt_iop_lensfun_data_t;

static_assert(sizeof(dt_iop_lensfun_data_t) == 496,
              "dt_iop_lensfun_data_t size changed -- struct-split integrity failure");


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
  if(old_version == 2 && new_version == 6)
  {
    // legacy params of version 2; version 1 comes from ancient times and seems to be forgotten by now
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

    *n = *d; // start with a fresh copy of default parameters (v6 defaults: method=LENSFUN, md_*)

    n->modify_flags = o->modify_flags;
    n->inverse = o->inverse;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    n->tca_override = o->tca_override;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->has_been_set = 0;

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    // v2 histories predate embedded-metadata correction: never infer it from decode
    // state, always resolve to the pre-existing Lensfun path.
    n->method = dt_iop_lens_method_t::LENSFUN;

    return 0;
  }
  if(old_version == 3 && new_version == 6)
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

    *n = *d; // start with a fresh copy of default parameters (v6 defaults: method=LENSFUN, md_*)

    // Pinned to the v3 struct size -- NEVER sizeof(dt_iop_lensfun_params_t), which is
    // larger since v6 and would read past this v3-sized input buffer (ASan violation).
    memcpy(n, o, sizeof(dt_iop_lensfun_params_v3_t)); // NOSONAR

    // one more parameter and changed parameters in case we autodetect
    n->has_been_set = 0;

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    // v3 histories predate embedded-metadata correction: never infer it from decode
    // state, always resolve to the pre-existing Lensfun path.
    n->method = dt_iop_lens_method_t::LENSFUN;

    return 0;
  }

  if(old_version == 4 && new_version == 6)
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

    *n = *d; // start with a fresh copy of default parameters (v6 defaults: method=LENSFUN, md_*)

    // Pinned to the v4 struct size -- NEVER sizeof(dt_iop_lensfun_params_t). Before v6
    // this happened to equal sizeof(v4) because v4 and the (then-current) v5 struct were
    // byte-identical; that coincidence breaks now that v6 appends new fields, and would
    // over-read past this v4-sized input buffer (ASan violation) if left as-is.
    memcpy(n, o, sizeof(dt_iop_lensfun_params_v4_t)); // NOSONAR

    // v4 `modified` (0 = auto, 1 = user-changed) shares the same byte offset in the
    // v4/v5 layout as v6 `has_been_set` (1 = auto, 0 = user-changed) -- the memcpy
    // above copied the old value verbatim, so invert it now to match the new semantics.
    n->has_been_set = o->modified ? 0 : 1;

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    // v4 histories predate embedded-metadata correction: never infer it from decode
    // state, always resolve to the pre-existing Lensfun path.
    n->method = dt_iop_lens_method_t::LENSFUN;

    return 0;
  }

  if(old_version == 5 && new_version == 6)
  {
    // the pre-v6 dt_iop_lensfun_params_t layout, through `modified`, with no method/md_*
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

    const auto *o = static_cast<const dt_iop_lensfun_params_v5_t *>(old_params);
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters (v6 defaults: method=LENSFUN, md_*)

    // Pinned to the v5 struct size: copies every pre-v6 field byte-for-byte and cannot
    // reach `method`/`cor_*_ft`, which live past the end of this old-sized input buffer.
    memcpy(n, o, sizeof(dt_iop_lensfun_params_v5_t)); // NOSONAR

    // v5 `modified` (0 = auto, 1 = user-changed) shares the same byte offset in the v5
    // layout as v6 `has_been_set` (1 = auto, 0 = user-changed) -- the memcpy above
    // copied the old value verbatim, so invert it now to match the new semantics.
    n->has_been_set = o->modified ? 0 : 1;

    // Belt-and-suspenders: the memcpy above is sized to the v5 struct so it cannot have
    // touched `method`, but force it explicitly so the invariant is visible in the code
    // itself -- v5 histories always resolve to LENSFUN, never inferred from the image's
    // current embedded-correction decode state. This migration takes no
    // dt_image_t and must stay that way: it never reads any decode-derived field.
    n->method = dt_iop_lens_method_t::LENSFUN;

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
static lfModifier * get_modifier(int *mods_done, int w, int h, const dt_iop_lensfun_data_t *d, int mods_filter, gboolean force_inverse)
{
  lfModifier *mod;
  int mods_todo = d->lensfun.modify_flags & mods_filter;
  int mods_done_tmp = 0;

#ifdef LF_0395
  mod = new lfModifier(d->lensfun.crop, w, h, LF_PF_F32, (force_inverse) ? !d->lensfun.inverse : d->lensfun.inverse);
  if(mods_todo & LF_MODIFY_DISTORTION)
    mods_done_tmp |= mod->EnableDistortionCorrection(d->lensfun.lens, d->lensfun.focal);
  if((mods_todo & LF_MODIFY_GEOMETRY) && (d->lensfun.lens->Type != d->lensfun.target_geom))
    mods_done_tmp |= mod->EnableProjectionTransform(d->lensfun.lens, d->lensfun.focal, d->lensfun.target_geom);
  if((mods_todo & LF_MODIFY_SCALE) && (d->lensfun.scale != 1.0))
    mods_done_tmp |= mod->EnableScaling(d->lensfun.scale);
  if(mods_todo & LF_MODIFY_TCA)
  {
    if(d->lensfun.tca_override) mods_done_tmp |= mod->EnableTCACorrection(d->lensfun.custom_tca);
    else mods_done_tmp |= mod->EnableTCACorrection(d->lensfun.lens, d->lensfun.focal);
  }
  if(mods_todo & LF_MODIFY_VIGNETTING)
    mods_done_tmp |= mod->EnableVignettingCorrection(d->lensfun.lens, d->lensfun.focal, d->lensfun.aperture, d->lensfun.distance);
#else
  mod = new lfModifier(d->lensfun.lens, d->lensfun.crop, w, h);
  mods_done_tmp = mod->Initialize(d->lensfun.lens, LF_PF_F32, d->lensfun.focal, d->lensfun.aperture, d->lensfun.distance, d->lensfun.scale, d->lensfun.target_geom, mods_todo,
                                  (force_inverse) ? !d->lensfun.inverse : d->lensfun.inverse);
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
// method == dt_iop_lens_method_t::EMBEDDED_METADATA routes every geometric entry point here
// instead of into the Lensfun lfModifier machinery below. The vendor union
// (exif_correction_data.{sony,fuji,dng,olympus}) is read EXACTLY ONCE, in
// commit_params(), through the single switch implemented in dt_embedded_lens_init_coeffs()
// below. That switch normalizes whichever vendor's native format the enum selects into a
// common LENS_MAXKNOTS-sized linear-spline knot table cached in piece->data (d->embedded.nc,
// d->embedded.knots.knots_dist, d->embedded.knots.knots_vig, d->embedded.knots.cor_rgb, d->embedded.knots.vig). Every dispatch helper below reads
// ONLY that knot table -- none of them touch
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

static void _compute_geometric_displacement_3ch(const float *const knots_dist,
                                                 const float *const cor_rgb,
                                                 const int nc,
                                                 const float radius,
                                                 const gboolean apply_dist,
                                                 const float cx, const float cy,
                                                 const float w2, const float h2,
                                                 const float roi_x, const float roi_y,
                                                 const float limw, const float limh,
                                                 float *const sx, float *const sy)
{
  const float dr = apply_dist
      ? dt_embedded_lens_linear_spline(knots_dist, cor_rgb, nc, radius)
      : 1.0f;
  *sx = CLAMP(dr * cx + w2 - roi_x, 0.0f, limw);
  *sy = CLAMP(dr * cy + h2 - roi_y, 0.0f, limh);
}

static int _process_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                           const dt_dev_pixelpipe_iop_t *piece, const float *const ivoid,
                                           float *const ovoid)
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

  const gboolean apply_vignette = (d->lensfun.modify_flags & LF_MODIFY_VIGNETTING) != 0;
  const gboolean apply_dist = (d->lensfun.modify_flags & LF_MODIFY_DISTORTION) != 0;
  const gboolean apply_tca = (d->lensfun.modify_flags & LF_MODIFY_TCA) != 0;

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
    // No geometric correction selected: pass the (possibly vignetted) work
    // buffer straight to the output.
    dt_iop_image_copy_by_size(ovoid, work, roi_out->width, roi_out->height, ch);
  }
  else
  {
    __OMP_PARALLEL_FOR_CPP__(firstprivate(work, ovoid, roi_in, roi_out, ch, ch_width, w2, h2, inv_rn, d,
                                          interpolation, limw, limh, raw_monochrome,
                                          apply_dist, apply_tca))
    for(int y = 0; y < roi_out->height; y++)
    {
      float *const out_row = ovoid + (size_t)y * roi_out->width * ch;
      for(int x = 0; x < roi_out->width; x++)
      {
        const float cx = roi_out->x + x - w2;
        const float cy = roi_out->y + y - h2;
        const float radius = hypotf(cx, cy) * inv_rn;
        for(int c = 0; c < ch; c++)
        {
          const int plane = (apply_dist && !apply_tca)
              ? 1
              : ((c < 3 && !raw_monochrome) ? c : 1);
          float sx, sy;
          _compute_geometric_displacement_3ch(d->embedded.knots.knots_dist,
                                               d->embedded.knots.cor_rgb[plane],
                                               d->embedded.nc, radius, apply_dist,
                                               cx, cy, w2, h2,
                                               roi_in->x, roi_in->y,
                                               limw, limh,
                                               &sx, &sy);
          out_row[x * ch + c] = dt_interpolation_compute_sample(interpolation, work + c, sx, sy,
                                                                  roi_in->width, roi_in->height, ch, ch_width);
        }
      }
    }
  }

  dt_free_align(work);
  _report_corrections_done(self, d->lensfun.modify_flags);
  return 0;
}

static int _distort_transform_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                                     const dt_dev_pixelpipe_iop_t *piece,
                                                     float *const __restrict points, size_t points_count)
{
  (void)self;
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  if(!d->embedded.nc || (d->lensfun.modify_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) == 0) return 1;

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
                                                         float *const __restrict points, size_t points_count)
{
  (void)self;
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
  if(!d->embedded.nc || (d->lensfun.modify_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) == 0) return 1;

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
                                                 const dt_iop_roi_t *const roi_out)
{
  (void)self;
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  if(!d->embedded.nc || (d->lensfun.modify_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) == 0)
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

typedef struct { float px, py, w2, h2, inv_rn; } _roi_point_t;
typedef struct { float xm, xM, ym, yM; }       _roi_bounds_t;

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
                                                  const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  (void)self;
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  // roi_in already equals roi_out (set by the caller before delegating here); grow only if
  // there is an active knot table to sample from and the user requested geometric correction.
  if(!d->embedded.nc || (d->lensfun.modify_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) == 0) return;

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

static void _modify_roi_out_embedded_metadata_warp(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, // NOSONAR
                                                    const dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                                                   const dt_iop_roi_t *roi_in)
{
  // WarpRectilinear-style corrections are a same-extent remap (no output-size change),
  // unlike Lensfun's target-geometry scale/crop: the corrected image always covers the
  // same canvas as the input, so this stays an identity pass-through. Kept as its own
  // delegate point for symmetry with the other geometric entry points.
  (void)self;
  (void)pipe;
  (void)piece;
  *roi_out = *roi_in;
}

static void _remap_pixel_inverse_or_not(float *out, const float *bufptr, const float *src,
                                         int ch, int ch_width, int do_nan_checks,
                                         int raw_monochrome, int mask_display,
                                         const dt_iop_roi_t *roi_in,
                                         const struct dt_interpolation *interpolation)
{
  dt_aligned_pixel_simd_t pixel = { 0.f };
  for(int c = 0; c < 3; c++)
  {
    if(do_nan_checks && (!isfinite(bufptr[c * 2]) || !isfinite(bufptr[c * 2 + 1])))
    {
      pixel[c] = 0.0f;
      continue;
    }
    const float *inptr = src + (size_t)c;
    const float pi0 = fmaxf(fminf(bufptr[c * 2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
    const float pi1 = fmaxf(fminf(bufptr[c * 2 + 1] - roi_in->y, roi_in->height - 1.0f), 0.0f);
    pixel[c] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                               roi_in->height, ch, ch_width);
  }
  if(raw_monochrome) pixel[0] = pixel[2] = pixel[1];
  if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
  {
    if(do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
    {
      pixel[3] = 0.0f;
    }
    else
    {
      const float *inptr = src + (size_t)3;
      const float pi0 = fmaxf(fminf(bufptr[2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
      const float pi1 = fmaxf(fminf(bufptr[3] - roi_in->y, roi_in->height - 1.0f), 0.0f);
      pixel[3] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                                 roi_in->height, ch, ch_width);
    }
    if(ch == DT_PIXEL_SIMD_CHANNELS) dt_store_simd_aligned(out, pixel);
    else for(int c = 0; c < ch; c++) out[c] = pixel[c];
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
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  if(d->embedded.method == dt_iop_lens_method_t::EMBEDDED_METADATA)
    return _process_embedded_metadata_warp(self, pipe, piece, static_cast<const float *>(ivoid), static_cast<float *>(ovoid));

  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  const int ch = piece->dsc_in.channels;
  const int ch_width = ch * roi_in->width;
  const int mask_display = pipe->mask_display;

  const unsigned int pixelformat = ch == 3 ? LF_CR_3(RED, GREEN, BLUE) : LF_CR_4(RED, GREEN, BLUE, UNKNOWN);

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f)
  {
    dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, ch);
    if(self->dev->gui_attached && g)
    {
      dt_iop_gui_enter_critical_section(self);
      g->status.corrections_done = 0;
      dt_iop_gui_leave_critical_section(self);
    }
    return 0;
  }

  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int used_lf_mask = (raw_monochrome) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

  int modflags;
  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE);

  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  if(d->lensfun.inverse)
  {
    // reverse direction (useful for renderings)
    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t bufsize = (size_t)roi_out->width * 2 * 3;

      size_t padded_bufsize;
      float *const buf = dt_pixelpipe_cache_alloc_perthread_float(bufsize, &padded_bufsize);
      if(IS_NULL_PTR(buf)) return 1;

#ifdef _OPENMP
#pragma omp parallel for default(none)  \
  firstprivate(roi_out, roi_in, padded_bufsize, modifier, ch, d, buf, ovoid, ivoid, ch_width, interpolation, raw_monochrome, mask_display)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *bufptr = (float*)dt_get_perthread(buf, padded_bufsize);
        modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width, 1, bufptr);

        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, bufptr += 6, out += ch)
          _remap_pixel_inverse_or_not(out, bufptr, (const float *)ivoid,
                                       ch, ch_width, d->lensfun.do_nan_checks,
                                       raw_monochrome, mask_display,
                                       roi_in, interpolation);
      }
      dt_pixelpipe_cache_free_align(buf);
    }
    else
    {
      dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, ch);
    }

    if(modflags & LF_MODIFY_VIGNETTING)
    {
      __OMP_PARALLEL_FOR_CPP__(firstprivate(modifier, ovoid, roi_out, ch, pixelformat))
      for(int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting */
        // actually this way row stride does not matter.
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        modifier->ApplyColorModification(out, roi_out->x, roi_out->y + y, roi_out->width, 1,
                                         pixelformat, ch * roi_out->width);
      }
      
    }
  }
  else // correct distortions:
  {
    // acquire temp memory for image buffer
    const size_t bufsize = (size_t)roi_in->width * roi_in->height * ch * sizeof(float);
    void *buf = dt_pixelpipe_cache_alloc_align_cache(
        bufsize,
        pipe->type);
    if(IS_NULL_PTR(buf)) return 1;
    memcpy(buf, ivoid, bufsize);

    if(modflags & LF_MODIFY_VIGNETTING)
    {
      __OMP_PARALLEL_FOR_CPP__(firstprivate(buf, roi_in, ch, pixelformat, modifier))
      for(int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting */
        // actually this way row stride does not matter.
        float *bufptr = ((float *)buf) + (size_t)ch * roi_in->width * y;
        modifier->ApplyColorModification(bufptr, roi_in->x, roi_in->y + y, roi_in->width, 1,
                                         pixelformat, ch * roi_in->width);
      }
      
    }

    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t buf2size = (size_t)roi_out->width * 2 * 3;
      size_t padded_buf2size;
      float *const buf2 = dt_pixelpipe_cache_alloc_perthread_float(buf2size, &padded_buf2size);
      if(IS_NULL_PTR(buf2))
      {
        dt_pixelpipe_cache_free_align(buf);
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
        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, buf2ptr += 6, out += ch)
          _remap_pixel_inverse_or_not(out, buf2ptr, (const float *)buf,
                                       ch, ch_width, d->lensfun.do_nan_checks,
                                       raw_monochrome, mask_display,
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
  delete modifier;

  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    g->status.corrections_done = (modflags & LENSFUN_MODFLAG_MASK);
    dt_iop_gui_leave_critical_section(self);
  }
  return 0;
}

#ifdef HAVE_OPENCL
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
                                   cl_mem *dev_knots_vig, cl_mem *dev_vig,
                                   cl_mem *dev_knots_dist, cl_mem *dev_cor_rgb0,
                                   cl_mem *dev_cor_rgb1, cl_mem *dev_cor_rgb2,
                                   dt_embedded_lens_knots_t *knots)
{
  const size_t knots_size = (size_t)LENS_MAXKNOTS * sizeof(float);

  *dev_knots_vig = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*dev_knots_vig)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *dev_vig = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*dev_vig)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *dev_knots_dist = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*dev_knots_dist)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *dev_cor_rgb0 = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*dev_cor_rgb0)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *dev_cor_rgb1 = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*dev_cor_rgb1)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;
  *dev_cor_rgb2 = (cl_mem)dt_opencl_alloc_device_buffer(devid, knots_size);
  if(IS_NULL_PTR(*dev_cor_rgb2)) return CL_MEM_OBJECT_ALLOCATION_FAILURE;

  cl_int err;
  err = dt_opencl_write_buffer_to_device(devid, knots->knots_vig, *dev_knots_vig, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->vig, *dev_vig, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->knots_dist, *dev_knots_dist, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->cor_rgb[0], *dev_cor_rgb0, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->cor_rgb[1], *dev_cor_rgb1, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;
  err = dt_opencl_write_buffer_to_device(devid, knots->cor_rgb[2], *dev_cor_rgb2, 0, knots_size, CL_TRUE);
  if(err != CL_SUCCESS) return err;

  return CL_SUCCESS;
}

static int process_embedded_metadata_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                        const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                        dt_iop_lensfun_data_t *d, dt_iop_lensfun_global_data_t *gd)
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

  cl_int err = -999;
  cl_mem dev_tmp = nullptr;

  size_t origin[] = { 0, 0, 0 };
  size_t iregion[] = { (size_t)iwidth, (size_t)iheight, 1 };
  size_t oregion[] = { (size_t)owidth, (size_t)oheight, 1 };
  size_t isizes[] = { (size_t)ROUNDUPDWD(iwidth, devid), (size_t)ROUNDUPDHT(iheight, devid), 1 };
  size_t osizes[] = { (size_t)ROUNDUPDWD(owidth, devid), (size_t)ROUNDUPDHT(oheight, devid), 1 };

  if(!d->embedded.nc)
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err == CL_SUCCESS)
      _report_corrections_done(self, d->lensfun.modify_flags);
    return (err == CL_SUCCESS) ? TRUE : FALSE;
  }

  // Honour the user's per-class selection from the "Corrections" combobox.
  // commit_params mirrors p->modify_flags into d->lensfun.modify_flags. The kernel pair
  // runs as a unit and can't be told to do "distortion only" or "TCA only"
  // individually, so partial geometric selections (one of distortion/TCA but
  // not both) fall back to the CPU path which has the right per-class logic.
  const gboolean apply_vignette = (d->lensfun.modify_flags & LF_MODIFY_VIGNETTING) != 0;
  const gboolean apply_dist = (d->lensfun.modify_flags & LF_MODIFY_DISTORTION) != 0;
  const gboolean apply_tca = (d->lensfun.modify_flags & LF_MODIFY_TCA) != 0;
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

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  const float w2 = 0.5f * orig_w;
  const float h2 = 0.5f * orig_h;
  const float rn = hypotf(w2, h2);
  const float inv_rn = (rn > 1e-6f) ? 1.0f / rn : 0.0f;
  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int monochrome = raw_monochrome ? 1 : 0;

  cl_mem dev_knots_vig = nullptr;
  cl_mem dev_vig = nullptr;
  cl_mem dev_knots_dist = nullptr;
  cl_mem dev_cor_rgb0 = nullptr;
  cl_mem dev_cor_rgb1 = nullptr;
  cl_mem dev_cor_rgb2 = nullptr;

  err = _setup_md_cl_kernels(devid,
                             &dev_knots_vig, &dev_vig,
                             &dev_knots_dist, &dev_cor_rgb0,
                             &dev_cor_rgb1, &dev_cor_rgb2,
                             &d->embedded.knots);
  if(err != CL_SUCCESS) goto error;

  dev_tmp = (cl_mem)dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(IS_NULL_PTR(dev_tmp)) goto error;

  if(!d->lensfun.inverse)
  {
    /* Forward (darkroom): vignette on input, then geometry on output. */
    if(apply_vignette)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 1, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 2, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 3, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 4, sizeof(int), (void *)&roi_in_x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 5, sizeof(int), (void *)&roi_in_y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 6, sizeof(float), (void *)&w2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 7, sizeof(float), (void *)&h2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 8, sizeof(float), (void *)&inv_rn);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 9, sizeof(cl_mem), (void *)&dev_knots_vig);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 10, sizeof(cl_mem), (void *)&dev_vig);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 11, sizeof(int), (void *)&d->embedded.nc);
      err = _run_md_cl_pass(devid, gd->kernel_md_vignette, dev_in, dev_tmp, isizes);
    }
    else
    {
      err = _run_md_cl_pass(devid, 0, dev_in, dev_tmp, iregion);
    }
    if(err != CL_SUCCESS) goto error;

    if(apply_dist || apply_tca)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 4, sizeof(int), (void *)&roi_in_x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 5, sizeof(int), (void *)&roi_in_y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 6, sizeof(int), (void *)&roi_out_x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 7, sizeof(int), (void *)&roi_out_y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 8, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 9, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 10, sizeof(float), (void *)&w2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 11, sizeof(float), (void *)&h2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 12, sizeof(float), (void *)&inv_rn);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 13, sizeof(cl_mem), (void *)&dev_knots_dist);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 14, sizeof(cl_mem), (void *)&dev_cor_rgb0);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 15, sizeof(cl_mem), (void *)&dev_cor_rgb1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 16, sizeof(cl_mem), (void *)&dev_cor_rgb2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 17, sizeof(int), (void *)&d->embedded.nc);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 18, sizeof(int), (void *)&monochrome);
      err = _run_md_cl_pass(devid, gd->kernel_md_lens_correction, dev_tmp, dev_out, osizes);
    }
    else
    {
      err = _run_md_cl_pass(devid, 0, dev_tmp, dev_out, oregion);
    }
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    /* Inverse (export): geometry on input, then vignette. */
    if(apply_dist || apply_tca)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 1, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 2, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 3, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 4, sizeof(int), (void *)&roi_in_x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 5, sizeof(int), (void *)&roi_in_y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 6, sizeof(int), (void *)&roi_out_x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 7, sizeof(int), (void *)&roi_out_y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 8, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 9, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 10, sizeof(float), (void *)&w2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 11, sizeof(float), (void *)&h2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 12, sizeof(float), (void *)&inv_rn);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 13, sizeof(cl_mem), (void *)&dev_knots_dist);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 14, sizeof(cl_mem), (void *)&dev_cor_rgb0);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 15, sizeof(cl_mem), (void *)&dev_cor_rgb1);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 16, sizeof(cl_mem), (void *)&dev_cor_rgb2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 17, sizeof(int), (void *)&d->embedded.nc);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_lens_correction, 18, sizeof(int), (void *)&monochrome);
      err = _run_md_cl_pass(devid, gd->kernel_md_lens_correction, dev_in, dev_tmp, isizes);
    }
    else
    {
      err = _run_md_cl_pass(devid, 0, dev_in, dev_tmp, iregion);
    }
    if(err != CL_SUCCESS) goto error;

    if(apply_vignette)
    {
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 4, sizeof(int), (void *)&roi_out_x);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 5, sizeof(int), (void *)&roi_out_y);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 6, sizeof(float), (void *)&w2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 7, sizeof(float), (void *)&h2);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 8, sizeof(float), (void *)&inv_rn);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 9, sizeof(cl_mem), (void *)&dev_knots_vig);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 10, sizeof(cl_mem), (void *)&dev_vig);
      dt_opencl_set_kernel_arg(devid, gd->kernel_md_vignette, 11, sizeof(int), (void *)&d->embedded.nc);
      err = _run_md_cl_pass(devid, gd->kernel_md_vignette, dev_tmp, dev_out, osizes);
    }
    else
    {
      err = _run_md_cl_pass(devid, 0, dev_tmp, dev_out, oregion);
    }
    if(err != CL_SUCCESS) goto error;
  }

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
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  auto d = (dt_iop_lensfun_data_t *)piece->data;
  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  if(d->embedded.method == dt_iop_lens_method_t::EMBEDDED_METADATA)
    return process_embedded_metadata_cl(self, pipe, piece, dev_in, dev_out, d, gd);

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
  const size_t tmpbuflen = d->lensfun.inverse ? (size_t)oheight * owidth * 2 * 3 * sizeof(float)
                                      : MAX((size_t)oheight * owidth * 2 * 3, (size_t)iheight * iwidth * ch)
                                        * sizeof(float);
  const unsigned int pixelformat = ch == 3 ? LF_CR_3(RED, GREEN, BLUE) : LF_CR_4(RED, GREEN, BLUE, UNKNOWN);

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;

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

  tmpbuf = (float *)dt_pixelpipe_cache_alloc_align_cache(
      tmpbuflen,
      pipe->type);
  if(IS_NULL_PTR(tmpbuf)) goto error;

  dev_tmp = (cl_mem)dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(IS_NULL_PTR(dev_tmp)) goto error;

  dev_tmpbuf = (cl_mem)dt_opencl_alloc_device_buffer(devid, tmpbuflen);
  if(IS_NULL_PTR(dev_tmpbuf)) goto error;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  modifier = get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(d->lensfun.inverse)
  {
    // reverse direction (useful for renderings)
    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      __OMP_PARALLEL_FOR_CPP__(firstprivate(modifier, tmpbuf, roi_out, tmpbufwidth))
      for(int y = 0; y < roi_out->height; y++)
      {
        float *pi = tmpbuf + (size_t)y * tmpbufwidth;
        modifier->ApplySubpixelGeometryDistortion(roi_out->x, roi_out->y + y, roi_out->width, 1, pi);
      }
      

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0,
                                             (size_t)owidth * oheight * 2 * 3 * sizeof(float), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_tmp);
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
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_tmp, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }

    if(modflags & LF_MODIFY_VIGNETTING)
    {
      __OMP_PARALLEL_FOR_CPP__(firstprivate(modifier, tmpbuf, roi_out, pixelformat, ch))
      for(int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting */
        // actually this way row stride does not matter.
        float *buf = tmpbuf + (size_t)y * ch * roi_out->width;
        _lens_fill_vignette_row(buf, roi_out->width, ch);
        modifier->ApplyColorModification(buf, roi_out->x, roi_out->y + y, roi_out->width, 1,
                                         pixelformat, ch * roi_out->width);
      }
      

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0,
                                             (size_t)ch * roi_out->width * roi_out->height * sizeof(float),
                                             CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 4, sizeof(cl_mem), (void *)&dev_tmpbuf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette, osizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }
  }

  else // correct distortions:
  {

    if(modflags & LF_MODIFY_VIGNETTING)
    {
      __OMP_PARALLEL_FOR_CPP__(firstprivate(tmpbuf, ch, roi_in, pixelformat, modifier))
      for(int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting */
        // actually this way row stride does not matter.
        float *buf = tmpbuf + (size_t)y * ch * roi_in->width;
        _lens_fill_vignette_row(buf, roi_in->width, ch);
        modifier->ApplyColorModification(buf, roi_in->x, roi_in->y + y, roi_in->width, 1,
                                         pixelformat, ch * roi_in->width);
      }
      

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
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
      

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
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
  }

  if(self->dev->gui_attached && g)
  {
    dt_iop_gui_enter_critical_section(self);
    g->status.corrections_done = (modflags & LENSFUN_MODFLAG_MASK);
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

  if(d->embedded.method == dt_iop_lens_method_t::EMBEDDED_METADATA)
  {
    // in + out + one vignette/distortion working copy (see _process_embedded_metadata_warp).
    tiling->factor = 3.0f;
    tiling->maxbuf = 1.0f;
    tiling->overhead = 0;
    tiling->xalign = 1;
    tiling->yalign = 1;

    // No active knot table or no geometric correction requested -> no halo needed.
    if(!d->embedded.nc || (d->lensfun.modify_flags & (LF_MODIFY_DISTORTION | LF_MODIFY_TCA)) == 0)
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
      const double dr = (double)dt_embedded_lens_linear_spline(d->embedded.knots.knots_dist, d->embedded.knots.cor_rgb[c], d->embedded.nc, 1.0f);
      max_abs_dr_minus_1 = fmax(max_abs_dr_minus_1, fabs(dr - 1.0));
    }

    static const int MITCHELL_KERNEL_HALF_WIDTH_PX = 2;
    tiling->overlap = (int)ceil(max_abs_dr_minus_1 * half_diag) + MITCHELL_KERNEL_HALF_WIDTH_PX;
    return;
  }

  tiling->factor = 4.5f; // in + out + tmp + tmpbuf
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
  auto d = (dt_iop_lensfun_data_t *)piece->data;

  if(d->embedded.method == dt_iop_lens_method_t::EMBEDDED_METADATA)
    return _distort_transform_embedded_metadata_warp(self, pipe, piece, points, points_count);

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f) return 0;

  const float orig_w = piece->buf_in.width, orig_h = piece->buf_in.height;
  int modflags;

  const int used_lf_mask = (dt_image_is_monochrome(&self->dev->image_storage)) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, TRUE);
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
  auto d = (dt_iop_lensfun_data_t *)piece->data;

  if(d->embedded.method == dt_iop_lens_method_t::EMBEDDED_METADATA)
    return _distort_backtransform_embedded_metadata_warp(self, pipe, piece, points, points_count);

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f) return 0;

  const int used_lf_mask = (dt_image_is_monochrome(&self->dev->image_storage)) ? LF_MODIFY_ALL & ~LF_MODIFY_TCA : LF_MODIFY_ALL;

  const float orig_w = piece->buf_in.width, orig_h = piece->buf_in.height;
  int modflags;
  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE);

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
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  if(d->embedded.method == dt_iop_lens_method_t::EMBEDDED_METADATA)
  {
    _distort_mask_embedded_metadata_warp(self, pipe, piece, in, out, roi_in, roi_out);
    return;
  }

  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f)
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  int modflags;
  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, /*LF_MODIFY_TCA |*/ LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE, FALSE);

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
  if(const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;
     d->embedded.method == dt_iop_lens_method_t::EMBEDDED_METADATA)
  {
    _modify_roi_out_embedded_metadata_warp(self, pipe, piece, roi_out, roi_in);
    return;
  }

  *roi_out = *roi_in;
}

void modify_roi_in(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                   struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  auto d = (dt_iop_lensfun_data_t *)piece->data;
  *roi_in = *roi_out;

  if(d->embedded.method == dt_iop_lens_method_t::EMBEDDED_METADATA)
  {
    _modify_roi_in_embedded_metadata_warp(self, pipe, piece, roi_out, roi_in);
    return;
  }

  // inverse transform with given params
  if(!d->lensfun.lens || !d->lensfun.lens->Maker || d->lensfun.crop <= 0.0f) return;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  int modflags;
  const lfModifier *modifier = get_modifier(&modflags, orig_w, orig_h, d, LF_MODIFY_ALL, FALSE);

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

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  auto p = (dt_iop_lensfun_params_t *)p1;

  // Capture the correction method (and its fine-tune fields) from the
  // HISTORY-SNAPSHOT pointer p1, BEFORE the p->has_been_set==1 branch below possibly
  // substitutes p with self->default_params. Dispatch must always follow the
  // history-recorded method, never the substituted defaults -- otherwise an
  // auto-detected embedded-method edit applied via preset/mass-export to an image
  // whose own default_params->method is LENSFUN would silently render with Lensfun
  // instead. Read this from p1 unconditionally: for the
  // ordinary same-image case p1->method and default_params->method agree anyway,
  // because reload_defaults() set default_params->method for that same image.
  const dt_iop_lens_method_t method = ((dt_iop_lensfun_params_t *)p1)->method;
  const float cor_dist_ft = ((dt_iop_lensfun_params_t *)p1)->cor_dist_ft;
  const float cor_vig_ft = ((dt_iop_lensfun_params_t *)p1)->cor_vig_ft;
  const float cor_ca_r_ft = ((dt_iop_lensfun_params_t *)p1)->cor_ca_r_ft;
  const float cor_ca_b_ft = ((dt_iop_lensfun_params_t *)p1)->cor_ca_b_ft;

  // has_been_set == 1 means "auto-detected/defaults, no user modification".
  // In that case, use default_params for the edit (presets, mass-export).
  if(p->has_been_set == 1)
  {
    /*
     * user did not modify anything in gui after autodetection - let's
     * use current default_params as params - for presets and mass-export
     */
    p = (dt_iop_lensfun_params_t *)self->default_params;

    dt_iop_compute_module_hash(self, self->dev->forms);
  }

  auto d = (dt_iop_lensfun_data_t *)piece->data;

  d->embedded.method = method;
  d->embedded.ft.distortion = cor_dist_ft;
  d->embedded.ft.vignette = cor_vig_ft;
  d->embedded.ft.ca_red = cor_ca_r_ft;
  d->embedded.ft.ca_blue = cor_ca_b_ft;

  // The vendor union is read EXACTLY ONCE, here, through the single switch inside
  // dt_embedded_lens_init_coeffs() -- normalizing whichever vendor member
  // self->dev->image_storage.exif_correction_type selects into the vendor-agnostic
  // LENS_MAXKNOTS knot table cached in piece->data. No dispatch helper reads the union
  // again. Toggle values are read from the captured p1 snapshot (`method`/
  // `md_*` above), not the possibly-substituted `p`, for the same reason the
  // capture above documents.
  //
  // Early-return before the Lensfun camera/lens DB lookup below -- the embedded
  // path never depends on Lensfun DB state, so skip acquiring darktable.plugin_threadsafe
  // and querying gd->db entirely.
  if(method == dt_iop_lens_method_t::EMBEDDED_METADATA)
  {
    d->lensfun.modify_flags = p->modify_flags & LENSFUN_MODFLAG_MASK;
    if(dt_image_is_monochrome(&self->dev->image_storage)) d->lensfun.modify_flags &= ~LF_MODIFY_TCA;
    d->embedded.nc = dt_embedded_lens_init_coeffs(&self->dev->image_storage,
                                          &d->embedded.ft,
                                          &d->embedded.knots, &d->embedded.scale_md);
    piece->cache_output_on_ram = TRUE;
    return;
  }

  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
  const lfCamera *camera = nullptr;
  const lfCamera **cam = nullptr;
  delete d->lensfun.lens; // NOSONAR
  d->lensfun.lens = new lfLens;

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
      if(p->tca_override)
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
        // add manual d->lensfun.lens stuff:
        lfLensCalibTCA tca = { LF_TCA_MODEL_NONE };
        tca.Focal = 0;
        tca.Model = LF_TCA_MODEL_LINEAR;
        tca.Terms[0] = p->tca_r;
        tca.Terms[1] = p->tca_b;
        if(d->lensfun.lens->CalibTCA)
          while(d->lensfun.lens->CalibTCA[0]) d->lensfun.lens->RemoveCalibTCA(0);
        d->lensfun.lens->AddCalibTCA(&tca);
#endif
      }
      lf_free(lens);
    }
  }
  lf_free(cam);
  d->lensfun.modify_flags = p->modify_flags;
  if(dt_image_is_monochrome(&self->dev->image_storage)) d->lensfun.modify_flags &= ~LF_MODIFY_TCA;
  d->lensfun.inverse = p->inverse;
  d->lensfun.scale = p->scale;
  d->lensfun.focal = p->focal;
  d->lensfun.aperture = p->aperture;
  d->lensfun.distance = p->distance;
  d->lensfun.target_geom = p->target_geom;
  d->lensfun.do_nan_checks = TRUE;
  d->lensfun.tca_override = p->tca_override;

  /*
   * there are certain situations when LensFun can return NAN coordinated.
   * most common case would be when the FOV is increased.
   */
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

  // Correction-method default. Params-only: must not touch any GUI widget --
  // the method-selector rebuild belongs in gui_update(), never here.
  // Placed before the Lensfun camera/lens DB lookup below, which can return
  // early (missing/unloaded lensfun db), so the method default is always
  // resolved regardless of Lensfun database availability.
  // This only ever writes default_params (this function's own `d`), never
  // module->params -- an already-edited image's explicit method choice is
  // therefore never overridden by this recomputation.
  d->method = dt_iop_lens_method_t::LENSFUN;
  if(dt_exif_lens_correction_available() && dt_embedded_lens_has_data(img))
    d->method = dt_iop_lens_method_t::EMBEDDED_METADATA;

  // init crop from db:
  char model[100]; // truncate often complex descriptions.
  g_strlcpy(model, img->exif_model, sizeof(model));
  for(char cnt = 0, *c = model; c < model + 100 && *c != '\0'; c++)
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

  int m = 0, l = 0, r = length - 1;

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

  int l = 0, r = length - 1;
  int m = 0, cmp = 0;

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
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.modflags), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.target_geom), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.scale), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.reverse), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.tca_r), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.tca_b), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->status.message), FALSE);

    g->status.trouble = TRUE;
    return;
  }
  else
  {
    // no longer in trouble
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.modflags), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.target_geom), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.scale), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->lensfun_controls.reverse), TRUE);
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

  int ffi = 1, fli = -1;
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
  ffi = 1, fli = sizeof(aperture_values) / sizeof(gdouble) - 1;
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

// Pure entry-list builder for the correction-method selector, deliberately separated
// from the GTK widget construction in gui_init() so the exactly-1-vs-exactly-2 entry
// count and the identity of the entries are unit-testable without a live GTK/bauhaus
// context. Writes up to 2 translatable (N_()-marked, translate at the
// dt_bauhaus_combobox_add() call site) labels into `out_labels` and returns how many
// were written. Position in the array is the position the entry will get in the
// combobox: 0 is always "lensfun database", 1 (if present) is always "embedded
// metadata" -- lens_method_changed() above and gui_update()'s selector sync below
// both rely on this exact, exhaustive mapping.
static int lens_method_selector_entries(gboolean available, const char *out_labels[2])
{
  int n = 0;
  out_labels[n++] = N_("lensfun database");
  if(available) out_labels[n++] = N_("embedded metadata");
  return n;
}

// Pure visibility predicate for the embedded-metadata fine-tune panel, separated
// from gui_changed() so it is unit-testable without live GTK widgets.
static gboolean lens_show_embedded_panel(dt_iop_lens_method_t method)
{
  return method == dt_iop_lens_method_t::EMBEDDED_METADATA;
}

// Correction-method selector callback. Built manually rather than via
// dt_bauhaus_combobox_from_params() -- like g->lensfun_controls.target_geom just below, whose entry list
// is also conditional (LF_VERSION-gated) -- because the introspection-driven from_params()
// combobox always emits every enum value, and the embedded-metadata entry must be entirely
// absent from the widget's model (not merely insensitive) when
// dt_exif_lens_correction_available() == FALSE. The widget's model therefore has
// either 1 entry (pos 0 = lensfun) or 2 (pos 0 = lensfun, pos 1 = embedded metadata); no
// other position is ever added, so the mapping below is exhaustive.
//
// Unlike modflags_changed()/target_geometry_changed() above, this routes through
// dt_iop_gui_changed() instead of writing p->modified/dt_dev_add_history_item() directly:
// selecting a method must immediately toggle the embedded fine-tune panel's visibility
// (gui_changed()'s job, mirroring its own tca_override handling), so this reuses the same
// framework hook already invoked automatically for the from_params() widgets in this file.
static void lens_method_changed(GtkWidget *widget, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  const int pos = dt_bauhaus_combobox_get(widget);
  p->method = (pos == 1) ? dt_iop_lens_method_t::EMBEDDED_METADATA : dt_iop_lens_method_t::LENSFUN;
  p->has_been_set = 0;
  dt_iop_gui_changed(self, widget, nullptr);
}

// Per-class fine-tune slider callback. One handler serves all 5 embedded-fine-tune
// sliders (cor_dist_ft, cor_vig_ft, cor_ca_r_ft, cor_ca_b_ft, scale_md) -- the actual field
// write is done by the bauhaus default callback (dt_bauhaus_value_changed_default_callback,
// fired synchronously before this signal -- see src/bauhaus/bauhaus.c:_value_changed_timer
// and src/develop/imageop.c:dt_bauhaus_value_changed_default_callback), which is enabled
// automatically by dt_bauhaus_slider_from_params(). All this callback has to do is the two
// side effects the introspection layer does not handle: mark the params as user-modified
// (so commit_params keeps p1 over default_params on the next pipe run) and push a history
// item so undo/redo sees the change.
static void _embedded_fine_tune_slider_changed(GtkWidget *, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  p->has_been_set = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
}

static void autoscale_pressed_md(GtkWidget *widget, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  if(dt_gui_widgets_suppressed()) return;
  dt_bauhaus_slider_set(g->embedded_controls.scale_md, 1.0f);
}

static void modflags_changed(GtkWidget *widget, gpointer user_data)
{
  auto *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  auto p = (dt_iop_lensfun_params_t *)self->params;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  int pos = dt_bauhaus_combobox_get(widget);
  for(GList *modifiers = g->status.modifiers;  modifiers; modifiers = g_list_next(modifiers))
  {
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->pos == pos)
    {
      p->modify_flags = (p->modify_flags & ~LENSFUN_MODFLAG_MASK) | mm->modflag;
      p->has_been_set = 0;
      dt_dev_add_history_item(darktable.develop, self, TRUE, TRUE);
      _report_corrections_done(self, mm->modflag);
      break;
    }
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  auto p = (dt_iop_lensfun_params_t *)self->params;
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  gtk_widget_set_visible(g->lensfun_controls.tca_override, !raw_monochrome);
  // update gui to show/hide tca sliders if tca_override was changed
  if(IS_NULL_PTR(w) || w == g->lensfun_controls.tca_override)
  {
    // show tca sliders only iff tca_overwrite is set
    gtk_widget_set_visible(g->lensfun_controls.tca_r, p->tca_override && !raw_monochrome);
    gtk_widget_set_visible(g->lensfun_controls.tca_b, p->tca_override && !raw_monochrome);
  }

  // The embedded-metadata fine-tune panel (4 sliders + scale slider) is only
  // part of the active widget tree under method == EMBEDDED_METADATA; hidden,
  // not destroyed, under LENSFUN -- same show/hide idiom as tca_r/tca_b just above.
  const gboolean show_embedded_panel = lens_show_embedded_panel(p->method);

  // Detect which correction classes the current image actually carries, so we don't
  // expose sliders that would do nothing (e.g. no CA on a Sony without maker-note CA,
  // no vignetting on a DNG warp-only, etc.).
  gboolean has_dist = FALSE;
  gboolean has_vign = FALSE;
  gboolean has_ca = FALSE;
  if(show_embedded_panel)
  {
    const dt_image_t *img = &self->dev->image_storage;
    has_dist = dt_embedded_lens_has_distortion(img);
    has_vign = dt_embedded_lens_has_vignetting(img);
    has_ca   = dt_embedded_lens_has_ca(img);
  }

  gtk_widget_set_visible(g->embedded_controls.cor_dist_ft, show_embedded_panel && has_dist);
  gtk_widget_set_visible(g->embedded_controls.cor_vig_ft, show_embedded_panel && has_vign);
  gtk_widget_set_visible(g->embedded_controls.cor_ca_r_ft, show_embedded_panel && has_ca);
  gtk_widget_set_visible(g->embedded_controls.cor_ca_b_ft, show_embedded_panel && has_ca);
  gtk_widget_set_visible(g->embedded_controls.scale_md, show_embedded_panel);

  if(show_embedded_panel)
  {
    gtk_box_reorder_child(GTK_BOX(self->widget), g->lensfun_controls.modflags, 1);
    gtk_widget_set_visible(g->lens_selection.camera_model, FALSE);
    gtk_widget_set_visible(g->lens_selection.lens_model, FALSE);
    gtk_widget_set_visible(g->lens_selection.find_camera_button, FALSE);
    gtk_widget_set_visible(g->lens_selection.find_lens_button, FALSE);
    gtk_widget_set_visible(g->lens_selection.lens_param_box, FALSE);
    gtk_widget_set_visible(g->lensfun_controls.target_geom, FALSE);
    gtk_widget_set_visible(g->lensfun_controls.scale, FALSE);
    gtk_widget_set_visible(g->lensfun_controls.reverse, FALSE);
    gtk_widget_set_visible(g->lensfun_controls.tca_override, FALSE);
    gtk_widget_set_visible(g->lensfun_controls.tca_r, FALSE);
    gtk_widget_set_visible(g->lensfun_controls.tca_b, FALSE);
  }
  else
  {
    gtk_box_reorder_child(GTK_BOX(self->widget), g->lensfun_controls.modflags, 9);
    gtk_widget_set_visible(g->lens_selection.camera_model, TRUE);
    gtk_widget_set_visible(g->lens_selection.lens_model, TRUE);
    gtk_widget_set_visible(g->lens_selection.find_camera_button, TRUE);
    gtk_widget_set_visible(g->lens_selection.find_lens_button, TRUE);
    gtk_widget_set_visible(g->lens_selection.lens_param_box, TRUE);
    gtk_widget_set_visible(g->lensfun_controls.target_geom, TRUE);
    gtk_widget_set_visible(g->lensfun_controls.scale, TRUE);
    gtk_widget_set_visible(g->lensfun_controls.reverse, TRUE);
    // tca_override / tca_r / tca_b re-routed through the tca_override branch above.
  }

  if(w)
  {
    // user did modify something with some widget
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

      // FIXME: get those from rawprepare IOP somehow !!!
      const int iwd = img->width - img->crop_x - img->crop_width,
                iht = img->height - img->crop_y - img->crop_height;

      // create dummy modifier
#if defined(__GNUC__) && (__GNUC__ > 7)
      const dt_iop_lensfun_data_t d =
        {
         .lensfun =
           {
            .lens         = (lfLens *)lenslist[0],
            .modify_flags = p->modify_flags,
            .inverse      = p->inverse,
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
      // prior to GCC 8.x the / .custom_tca   = { .Model = ??? } / was not supported:
      //    sorry, unimplemented: non-trivial designated initializers not supported
      // ?? This code can be removed when GCC-7 is not used anymore.

      dt_iop_lensfun_data_t d;
      d.lensfun.lens             = (lfLens *)lenslist[0];
      d.lensfun.modify_flags     = p->modify_flags;
      d.lensfun.inverse          = p->inverse;
      d.lensfun.scale            = 1.0f;
      d.lensfun.crop             = p->crop;
      d.lensfun.focal            = p->focal;
      d.lensfun.aperture         = p->aperture;
      d.lensfun.distance         = p->distance;
      d.lensfun.target_geom      = p->target_geom;
      d.lensfun.custom_tca.Model = LF_TCA_MODEL_NONE;
#endif

      lfModifier *modifier = get_modifier(NULL, iwd, iht, &d, LF_MODIFY_ALL, FALSE);

      scale = modifier->GetAutoScale(p->inverse);
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
  if(dt_gui_widgets_suppressed()) return;

  dt_iop_gui_enter_critical_section(self);
  const int corrections_done = g->status.corrections_done;
  dt_iop_gui_leave_critical_section(self);

  const char empty_message[] = "";
  char *message = (char *)empty_message;
  for(GList *modifiers = g->status.modifiers; modifiers && self->enabled; modifiers = g_list_next(modifiers))
  {
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->modflag == corrections_done)
    {
      message = mm->name;
      break;
    }
  }

  dt_gui_freeze_begin();
  gtk_label_set_text(g->status.message, message);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->status.message), message);
  dt_gui_freeze_end();
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = IOP_GUI_ALLOC(lensfun);

  g->lens_selection.camera = nullptr;
  g->lens_selection.camera_menu = nullptr;
  g->lens_selection.lens_menu = nullptr;
  g->status.modifiers = nullptr;

  dt_iop_gui_enter_critical_section(self); // not actually needed, we're the only one with a ref to this instance
  g->status.corrections_done = -1;
  dt_iop_gui_leave_critical_section(self);

  // initialize modflags options
  int pos = -1;
  dt_iop_lensfun_modifier_t *modifier;
  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("none"), sizeof(modifier->name));
  g->status.modifiers = g_list_append(g->status.modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_NONE;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("all"), sizeof(modifier->name));
  g->status.modifiers = g_list_append(g->status.modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_ALL;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("distortion & TCA"), sizeof(modifier->name));
  g->status.modifiers = g_list_append(g->status.modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST_TCA;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("distortion & vignetting"), sizeof(modifier->name));
  g->status.modifiers = g_list_append(g->status.modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST_VIGN;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("TCA & vignetting"), sizeof(modifier->name));
  g->status.modifiers = g_list_append(g->status.modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_TCA_VIGN;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only distortion"), sizeof(modifier->name));
  g->status.modifiers = g_list_append(g->status.modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only TCA"), sizeof(modifier->name));
  g->status.modifiers = g_list_append(g->status.modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_TCA;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only vignetting"), sizeof(modifier->name));
  g->status.modifiers = g_list_append(g->status.modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_VIGN;
  modifier->pos = ++pos;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
    gtk_widget_set_name(self->widget, "lens-module");

    // Correction-method selector. Exactly 2 entries when the loaded image carries
    // embedded correction data (and Exiv2 >= 0.27.4), exactly 1 (lensfun only)
    // otherwise. The embedded-metadata entry is never added to the model at all when
    // unavailable -- not merely disabled. See lens_method_changed() above for why this
    // is built manually instead of via dt_bauhaus_combobox_from_params(); the entry list
    // itself comes from lens_method_selector_entries() so the exactly-1-vs-exactly-2
    // contract is unit-tested independently of this GTK construction.
    g->embedded_controls.method = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
    dt_bauhaus_widget_set_label(g->embedded_controls.method, N_("correction method"));
    gtk_box_pack_start(GTK_BOX(self->widget), g->embedded_controls.method, TRUE, TRUE, 0);
    gtk_widget_set_tooltip_text(g->embedded_controls.method, _("source of the lens correction data"));
    const char *method_labels[2];
    const gboolean has_embedded = dt_exif_lens_correction_available()
        && !IS_NULL_PTR(self->dev)
        && dt_embedded_lens_has_data(&self->dev->image_storage);
    const int n_method_entries = lens_method_selector_entries(has_embedded, method_labels);
    for(int i = 0; i < n_method_entries; i++) dt_bauhaus_combobox_add(g->embedded_controls.method, _(method_labels[i]));
    g_signal_connect(G_OBJECT(g->embedded_controls.method), "value-changed", G_CALLBACK(lens_method_changed), (gpointer)self);

    // Embedded-metadata fine-tune sliders. Always constructed -- like
    // g->lensfun_controls.tca_r/g->lensfun_controls.tca_b above -- visibility is toggled in gui_changed() based on
    // p->method, so it is part of the widget tree but not shown/active under LENSFUN.
    g->embedded_controls.cor_dist_ft = dt_bauhaus_slider_from_params(self, "cor_dist_ft");
    gtk_widget_set_visible(g->embedded_controls.cor_dist_ft, FALSE);
    g_signal_connect(G_OBJECT(g->embedded_controls.cor_dist_ft), "value-changed", G_CALLBACK(_embedded_fine_tune_slider_changed), self);

    g->embedded_controls.cor_vig_ft = dt_bauhaus_slider_from_params(self, "cor_vig_ft");
    gtk_widget_set_visible(g->embedded_controls.cor_vig_ft, FALSE);
    g_signal_connect(G_OBJECT(g->embedded_controls.cor_vig_ft), "value-changed", G_CALLBACK(_embedded_fine_tune_slider_changed), self);

    g->embedded_controls.cor_ca_r_ft = dt_bauhaus_slider_from_params(self, "cor_ca_r_ft");
    gtk_widget_set_visible(g->embedded_controls.cor_ca_r_ft, FALSE);
    g_signal_connect(G_OBJECT(g->embedded_controls.cor_ca_r_ft), "value-changed", G_CALLBACK(_embedded_fine_tune_slider_changed), self);

    g->embedded_controls.cor_ca_b_ft = dt_bauhaus_slider_from_params(self, "cor_ca_b_ft");
    gtk_widget_set_visible(g->embedded_controls.cor_ca_b_ft, FALSE);
    g_signal_connect(G_OBJECT(g->embedded_controls.cor_ca_b_ft), "value-changed", G_CALLBACK(_embedded_fine_tune_slider_changed), self);

    g->embedded_controls.scale_md = dt_bauhaus_slider_from_params(self, "scale_md");
    dt_bauhaus_slider_set_digits(g->embedded_controls.scale_md, 4);
    dt_bauhaus_widget_set_quad_paint(g->embedded_controls.scale_md, dtgtk_cairo_paint_refresh, 0, NULL);
    g_signal_connect(G_OBJECT(g->embedded_controls.scale_md), "value-changed", G_CALLBACK(_embedded_fine_tune_slider_changed), self);
    g_signal_connect(G_OBJECT(g->embedded_controls.scale_md), "quad-pressed", G_CALLBACK(autoscale_pressed_md), self);
    gtk_widget_set_tooltip_text(g->embedded_controls.scale_md, _("image scale"));
    gtk_widget_set_visible(g->embedded_controls.scale_md, FALSE);

    // camera selector
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
    g->lens_selection.camera_model = dt_iop_button_new(self, N_("camera model"), G_CALLBACK(camera_menusearch_clicked), FALSE, 0,
                                        (GdkModifierType)0, NULL, 0, hbox);
    g->lens_selection.find_camera_button
        = dt_iop_button_new(self, N_("find camera"), G_CALLBACK(camera_autosearch_clicked), FALSE, 0,
                            (GdkModifierType)0, dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
    dt_gui_add_class(g->lens_selection.find_camera_button, "dt_big_btn_canvas");
    gtk_box_pack_start(GTK_BOX(hbox), g->lens_selection.find_camera_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

    // lens selector
    hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
    g->lens_selection.lens_model = dt_iop_button_new(self, N_("lens model"), G_CALLBACK(lens_menusearch_clicked), FALSE, 0,
                                      (GdkModifierType)0, NULL, 0, hbox);
    g->lens_selection.find_lens_button
        = dt_iop_button_new(self, N_("find lens"), G_CALLBACK(lens_autosearch_clicked), FALSE, 0,
                            (GdkModifierType)0, dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
    dt_gui_add_class(g->lens_selection.find_lens_button, "dt_big_btn_canvas");
    gtk_box_pack_start(GTK_BOX(hbox), g->lens_selection.find_lens_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

    // lens properties
    g->lens_selection.lens_param_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
    gtk_box_pack_start(GTK_BOX(self->widget), g->lens_selection.lens_param_box, TRUE, TRUE, 0);

  // selector for correction type (modflags): one or more out of distortion, TCA, vignetting
  g->lensfun_controls.modflags = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->lensfun_controls.modflags, N_("corrections"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->lensfun_controls.modflags, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->lensfun_controls.modflags, _("which corrections to apply"));
  GList *l = g->status.modifiers;
  while(l)
  {
    modifier = (dt_iop_lensfun_modifier_t *)l->data;
    dt_bauhaus_combobox_add(g->lensfun_controls.modflags, modifier->name);
    l = g_list_next(l);
  }
  dt_bauhaus_combobox_set(g->lensfun_controls.modflags, 0);
  g_signal_connect(G_OBJECT(g->lensfun_controls.modflags), "value-changed", G_CALLBACK(modflags_changed), (gpointer)self);

  // target geometry
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

  // scale
  g->lensfun_controls.scale = dt_bauhaus_slider_from_params(self, N_("scale"));
  dt_bauhaus_slider_set_digits(g->lensfun_controls.scale, 3);
  dt_bauhaus_widget_set_quad_paint(g->lensfun_controls.scale, dtgtk_cairo_paint_refresh, 0, NULL);
  g_signal_connect(G_OBJECT(g->lensfun_controls.scale), "quad-pressed", G_CALLBACK(autoscale_pressed), self);
  gtk_widget_set_tooltip_text(g->lensfun_controls.scale, _("auto scale"));

  // reverse direction
  g->lensfun_controls.reverse = dt_bauhaus_combobox_from_params(self, "inverse");
  dt_bauhaus_combobox_add(g->lensfun_controls.reverse, _("correct"));
  dt_bauhaus_combobox_add(g->lensfun_controls.reverse, _("distort"));
  gtk_widget_set_tooltip_text(g->lensfun_controls.reverse, _("correct distortions or apply them"));

  g->lensfun_controls.tca_override = dt_bauhaus_toggle_from_params(self, "tca_override");

  // override linear tca (if not 1.0):
  g->lensfun_controls.tca_r = dt_bauhaus_slider_from_params(self, "tca_r");
  dt_bauhaus_slider_set_digits(g->lensfun_controls.tca_r, 5);
  gtk_widget_set_tooltip_text(g->lensfun_controls.tca_r, _("Transversal Chromatic Aberration red"));

  g->lensfun_controls.tca_b = dt_bauhaus_slider_from_params(self, "tca_b");
  dt_bauhaus_slider_set_digits(g->lensfun_controls.tca_b, 5);
  gtk_widget_set_tooltip_text(g->lensfun_controls.tca_b, _("Transversal Chromatic Aberration blue"));

  // message box to inform user what corrections have been done. this is useful as depending on lensfuns
  // profile only some of the lens flaws can be corrected
  GtkBox *hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));
  GtkWidget *label = gtk_label_new(_("corrections done: "));
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_tooltip_text(label, _("which corrections have actually been done"));
  gtk_box_pack_start(GTK_BOX(hbox1), label, FALSE, FALSE, 0);
  g->status.message = GTK_LABEL(gtk_label_new("")); // This gets filled in by process
  gtk_label_set_ellipsize(GTK_LABEL(g->status.message), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->status.message), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox1), TRUE, TRUE, 0);

  /* add signal handler for preview pipe finish to update message on corrections done */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(corrections_done), self);
}

void gui_update(struct dt_iop_module_t *self)
{
  // let gui elements reflect params
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  auto p = (dt_iop_lensfun_params_t *)self->params;

  if(p->has_been_set == 1)
  {
    /*
     * user did not modify anything in gui after autodetection - let's
     * use current default_params as params - for presets and mass-export.
     * Preserve the recorded method, since reload_defaults() may have set a
     * different method in default_params than the one currently in p.
     */
    const dt_iop_lens_method_t saved_method = p->method;
    memcpy(self->params, self->default_params, sizeof(dt_iop_lensfun_params_t));
    p->method = saved_method;
  }

  auto gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  auto dt_iop_lensfun_db = (lfDatabase *)gd->db;
  // these are the wrong (untranslated) strings in general but that's ok, they will be overwritten further
  // down
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_selection.camera_model))), p->camera);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_selection.lens_model))), p->lens);
  gtk_widget_set_tooltip_text(g->lens_selection.camera_model, "");
  gtk_widget_set_tooltip_text(g->lens_selection.lens_model, "");

  int modflag = p->modify_flags & LENSFUN_MODFLAG_MASK;
  for(GList *modifiers = g->status.modifiers; modifiers; modifiers = g_list_next(modifiers))
  {
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->modflag == modflag)
    {
      dt_bauhaus_combobox_set(g->lensfun_controls.modflags, mm->pos);
      break;
    }
  }

  // Rebuild the method selector entries if image capability changed since
  // gui_init() or the last gui_update(). gui_init() gates the "embedded metadata"
  // entry on the initial image's data; when the user switches to an image whose
  // correction type differs, the entry list must be rebuilt. Clear/rebuild is
  // only performed when the entry count is wrong -- the common case (same image,
  // same entries) is a no-op.
  const gboolean has_embedded
      = dt_exif_lens_correction_available() && dt_embedded_lens_has_data(&self->dev->image_storage);
  const int method_len = dt_bauhaus_combobox_length(g->embedded_controls.method);
  const int desired_len = has_embedded ? 2 : 1;
  if(method_len != desired_len)
  {
    const char *method_labels[2];
    dt_bauhaus_combobox_clear(g->embedded_controls.method);
    const int n = lens_method_selector_entries(has_embedded, method_labels);
    for(int i = 0; i < n; i++) dt_bauhaus_combobox_add(g->embedded_controls.method, _(method_labels[i]));
  }
  const int method_pos
      = (p->method == dt_iop_lens_method_t::EMBEDDED_METADATA && has_embedded) ? 1 : 0;
  dt_bauhaus_combobox_set(g->embedded_controls.method, method_pos);

  dt_bauhaus_combobox_set(g->lensfun_controls.target_geom, p->target_geom - LF_UNKNOWN - 1);
  dt_bauhaus_combobox_set(g->lensfun_controls.reverse, p->inverse);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->lensfun_controls.tca_override), p->tca_override);
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

  const dt_dev_pixelpipe_iop_t *lens_piece = dt_dev_distort_get_iop_pipe(self->dev->virtual_pipe, self);
  const dt_iop_lensfun_data_t *lens_d
      = (!IS_NULL_PTR(lens_piece)) ? (const dt_iop_lensfun_data_t *)lens_piece->data : NULL;

  int modflags = p->modify_flags & LENSFUN_MODFLAG_MASK;

  if(!IS_NULL_PTR(lens_d))
  {
    if(p->method == dt_iop_lens_method_t::LENSFUN)
    {
      if(!IS_NULL_PTR(lens_d->lensfun.lens) && !IS_NULL_PTR(lens_d->lensfun.lens->Maker)
         && lens_d->lensfun.crop > 0.0f && lens_piece->buf_in.width > 0 && lens_piece->buf_in.height > 0)
      {
        const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
        const int used_lf_mask = raw_monochrome ? (LF_MODIFY_ALL & ~LF_MODIFY_TCA) : LF_MODIFY_ALL;
        dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
        lfModifier *modifier = get_modifier(&modflags, lens_piece->buf_in.width, lens_piece->buf_in.height,
                                            lens_d, used_lf_mask, FALSE);
        delete modifier;
        dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
        modflags &= LENSFUN_MODFLAG_MASK;
      }
    }
    else
    {
      modflags = lens_d->lensfun.modify_flags & LENSFUN_MODFLAG_MASK;
    }
  }

  dt_iop_gui_enter_critical_section(self);
  g->status.corrections_done = modflags;
  dt_iop_gui_leave_critical_section(self);

  for(GList *modifiers = g->status.modifiers; !IS_NULL_PTR(modifiers); modifiers = g_list_next(modifiers))
  {
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->modflag == modflags)
    {
      gtk_label_set_text(g->status.message, mm->name);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->status.message), mm->name);
      break;
    }
  }

  gui_changed(self, NULL, NULL);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  auto g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(corrections_done), self);

  while(g->status.modifiers)
  {
    dt_free(g->status.modifiers->data);
    g->status.modifiers = g_list_delete_link(g->status.modifiers, g->status.modifiers);
  }

  IOP_GUI_FREE;
}

}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
