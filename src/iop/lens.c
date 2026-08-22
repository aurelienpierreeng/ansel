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
#include "common/global_mutexes.h"
#include "common/utility.h"
#include "system/macros.h"
#include "common/module_versioning.h"
#include "common/logging.h"
#include "system/mem_alloc.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "glib.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "widgets/bauhaus.h"
#include "pixel/interpolation.h"
#include "common/file_location.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"

#include "iop/iop_api.h"
#include <assert.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "widgets/popup.h"
#include "widgets/widget_style.h"
#include "control/signal.h"

#include "develop/geometry/geometry.h"

#include "lensserious.h"    // side-by-side latch against lensfun, see feat/lensserious
#include "lensserious_db.h" // ... and its calibration database, latched the same way


/* The correction axes and the projection numbering.
 *
 * These values are lensfun's, and they must stay lensfun's: `modify_flags` and
 * `target_geom` are SERIALIZED into every user's history and every preset that has ever
 * been saved. Defining them here rather than including them is what lets liblensfun go
 * without rewriting anyone's edits.
 *
 * The projection numbering is also, entry for entry, ls_lens_type_t's -- asserted below
 * rather than assumed, because the two now live in different repositories and nothing else
 * would notice one of them growing a member in the middle. */
typedef enum dt_lens_modify_t
{
  DT_LENS_MODIFY_TCA        = 0x00000001,
  DT_LENS_MODIFY_VIGNETTING = 0x00000002,
  DT_LENS_MODIFY_DISTORTION = 0x00000008,
  DT_LENS_MODIFY_GEOMETRY   = 0x00000010,
  DT_LENS_MODIFY_SCALE      = 0x00000020,
  DT_LENS_MODIFY_ALL        = ~0,
} dt_lens_modify_t;

typedef enum dt_lens_type_t
{
  DT_LENS_UNKNOWN = 0,
  DT_LENS_RECTILINEAR = 1,
  DT_LENS_FISHEYE = 2,
  DT_LENS_PANORAMIC = 3,
  DT_LENS_EQUIRECTANGULAR = 4,
  DT_LENS_FISHEYE_ORTHOGRAPHIC = 5,
  DT_LENS_FISHEYE_STEREOGRAPHIC = 6,
  DT_LENS_FISHEYE_EQUISOLID = 7,
  DT_LENS_FISHEYE_THOBY = 8,
} dt_lens_type_t;

_Static_assert((int)DT_LENS_RECTILINEAR == (int)LS_LENS_RECTILINEAR
                  && (int)DT_LENS_FISHEYE == (int)LS_LENS_FISHEYE
                  && (int)DT_LENS_PANORAMIC == (int)LS_LENS_PANORAMIC
                  && (int)DT_LENS_EQUIRECTANGULAR == (int)LS_LENS_EQUIRECTANGULAR
                  && (int)DT_LENS_FISHEYE_ORTHOGRAPHIC == (int)LS_LENS_FISHEYE_ORTHOGRAPHIC
                  && (int)DT_LENS_FISHEYE_STEREOGRAPHIC == (int)LS_LENS_FISHEYE_STEREOGRAPHIC
                  && (int)DT_LENS_FISHEYE_EQUISOLID == (int)LS_LENS_FISHEYE_EQUISOLID
                  && (int)DT_LENS_FISHEYE_THOBY == (int)LS_LENS_FISHEYE_THOBY,
              "projection numbering must match ls_lens_type_t: stored params depend on it");

DT_MODULE_INTROSPECTION(5, dt_iop_lensfun_params_t)

typedef enum dt_iop_lensfun_modflag_t
{
  LENSFUN_MODFLAG_NONE = 0,
  LENSFUN_MODFLAG_ALL = DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_VIGNETTING,
  LENSFUN_MODFLAG_DIST_TCA = DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_TCA,
  LENSFUN_MODFLAG_DIST_VIGN = DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_VIGNETTING,
  LENSFUN_MODFLAG_TCA_VIGN = DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_VIGNETTING,
  LENSFUN_MODFLAG_DIST = DT_LENS_MODIFY_DISTORTION,
  LENSFUN_MODFLAG_TCA = DT_LENS_MODIFY_TCA,
  LENSFUN_MODFLAG_VIGN = DT_LENS_MODIFY_VIGNETTING,
  LENSFUN_MODFLAG_MASK = DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_VIGNETTING
} dt_iop_lensfun_modflag_t;

typedef struct dt_iop_lensfun_modifier_t
{
  char name[80];
  int pos; // position in combo box
  int modflag;
} dt_iop_lensfun_modifier_t;

typedef struct dt_iop_lensfun_params_t
{
  int modify_flags;
  int inverse; // $MIN: 0 $MAX: 1 $DEFAULT: 0 $DESCRIPTION: "mode"
  float scale; // $MIN: 0.1 $MAX: 2.0 $DEFAULT: 1.0
  float crop;
  float focal;
  float aperture;
  float distance;
  dt_lens_type_t target_geom; // $DEFAULT: DT_LENS_RECTILINEAR $DESCRIPTION: "geometry"
  char camera[128];
  char lens[128];
  gboolean tca_override; // $DEFAULT: FALSE $DESCRIPTION: "TCA overwrite"
  float tca_r; // $MIN: 0.99 $MAX: 1.01 $DEFAULT: 1.0 $DESCRIPTION: "TCA red"
  float tca_b; // $MIN: 0.99 $MAX: 1.01 $DEFAULT: 1.0 $DESCRIPTION: "TCA blue"
  int modified; // $DEFAULT: 0 did user changed anything from automatically detected?
} dt_iop_lensfun_params_t;

typedef struct dt_iop_lensfun_gui_data_t
{
  /** The camera shown in the picker, as a database id; -1 for none. */
  long long camera_id;
  GtkWidget *lens_param_box;
  GtkWidget *cbe[3];
  GtkWidget *camera_model;
  GtkMenu *camera_menu;
  GtkWidget *lens_model;
  GtkMenu *lens_menu;
  GtkWidget *modflags, *target_geom, *reverse, *tca_override, *tca_r, *tca_b, *scale;
  GtkWidget *find_lens_button;
  GtkWidget *find_camera_button;
  GList *modifiers;
  GtkLabel *message;
  int corrections_done;
  gboolean trouble;
} dt_iop_lensfun_gui_data_t;

typedef struct dt_iop_lensfun_global_data_t
{
  gboolean db_tried;
  /** Pre-warm thread, see _lensfun_db_warm(). Joined by cleanup_global(). */
  GThread *db_warm;
  int kernel_lens_distort_bilinear;
  int kernel_lens_distort_bicubic;
  int kernel_lens_distort_mitchell;
  int kernel_lens_vignette;
} dt_iop_lensfun_global_data_t;

/* ---------------------------------------------------------------------------------------
 * The LensSerious calibration database, latched BESIDE liblensfun's rather than replacing
 * it.
 *
 * Every lookup below is answered twice -- once by liblensfun, once by LensSerious -- and
 * both answers are printed. lensfun keeps authoring the pixels; nothing here changes what
 * is rendered. The point is to run the real GUI on real images and read the log, because
 * the disagreements that matter are not the ones a synthetic harness reaches: the harness
 * walks the database, and this walks whatever a user's EXIF actually says, spelled however
 * the camera spelled it.
 *
 * Reading is lock-free by construction -- `mode=ro&immutable=1` with SQLITE_OPEN_NOMUTEX,
 * so SQLite takes no file lock, no shared-memory segment and no mutex. The price is ONE
 * HANDLE PER THREAD, so the handle is thread-local and closed by its destructor when the
 * thread ends. Threads that never touch a lens never open it.
 *
 * The one-entry caches beside it stand in for the two process-wide memo hash tables:
 * commit_params() resolves the camera and the lens on every pipe resync, for every pipe,
 * and asks the SAME question every time -- an image's camera and lens do not change while
 * it is open. A per-thread cache of the last answer serves that exactly, with no lock and
 * no unbounded growth, where a shared table would need a mutex back.
 * ------------------------------------------------------------------------------------ */
typedef struct _ls_tls_t
{
  ls_db_t *db;
  gboolean tried;

  char cam_key[512];
  ls_camera_t cam;
  gboolean cam_found;
  gboolean cam_cached;

  char lens_key[512];
  long long lens_id;
  gboolean lens_cached;
} _ls_tls_t;

/* Closed when the thread that opened it exits, which is the whole reason this is a GPrivate
 * and not a plain __thread pointer: the handle has to be RELEASED, and nothing else in C
 * runs code at thread exit. iop/drawlayer.c holds its per-thread scratch buffers the same
 * way, for the same reason. */
static void _ls_tls_free(gpointer data)
{
  _ls_tls_t *tls = (_ls_tls_t *)data;
  if(IS_NULL_PTR(tls)) return;
  if(!IS_NULL_PTR(tls->db)) ls_db_close(tls->db);
  dt_free(tls);
}

static GPrivate _ls_tls_key = G_PRIVATE_INIT(_ls_tls_free);

/** @brief This thread's cache block, allocated on first use. NULL only if that allocation
 *  failed, in which case every caller below degrades to "no database" rather than crashing. */
static _ls_tls_t *_ls_tls_get(void)
{
  _ls_tls_t *tls = (_ls_tls_t *)g_private_get(&_ls_tls_key);
  if(!IS_NULL_PTR(tls)) return tls;

  tls = (_ls_tls_t *)g_malloc0(sizeof(_ls_tls_t));
  if(IS_NULL_PTR(tls)) return NULL;
  /* The only field whose zeroed value is not the right one: -1 is "no lens", 0 is a
   * perfectly good row id. */
  tls->lens_id = -1;
  g_private_set(&_ls_tls_key, tls);
  return tls;
}

/**
 * @brief This thread's database handle, opened on first use.
 *
 * @details The user's configuration directory is searched before the installed data, so a
 * database regenerated against newer upstream calibrations can be dropped in without
 * rebuilding. A failed open is final for the thread: retrying per lookup would turn a
 * missing file into a slow one.
 */
static ls_db_t *_ls_db(void)
{
  _ls_tls_t *tls = _ls_tls_get();
  if(IS_NULL_PTR(tls)) return NULL;

  if(tls->tried) return tls->db;
  tls->tried = TRUE;

  char dir[PATH_MAX] = { 0 };
  char path[PATH_MAX] = { 0 };

  dt_loc_get_user_config_dir(dir, sizeof(dir));
  snprintf(path, sizeof(path), "%s/lenses.db", dir);
  tls->db = ls_db_open(path);

  if(IS_NULL_PTR(tls->db))
  {
    dt_loc_get_datadir(dir, sizeof(dir));
    snprintf(path, sizeof(path), "%s/lenses.db", dir);
    tls->db = ls_db_open(path);
  }

  if(IS_NULL_PTR(tls->db))
    dt_print(DT_DEBUG_ALWAYS,
             "[lens] no calibration database: looked for lenses.db in the config directory"
             " and in `%s'\n", dir);
  else
    dt_print(DT_DEBUG_PIPE, "[lens] opened `%s' (schema v%d)\n", path,
             ls_db_schema_version(tls->db));

  return tls->db;
}

/** @brief The camera an EXIF maker/model names. @return TRUE when found. */
static gboolean _ls_find_camera(const char *maker, const char *model, ls_camera_t *out)
{
  if(IS_NULL_PTR(model) || !model[0]) return FALSE;
  /* _ls_db() has already established that this thread has a cache block; it cannot have
   * returned a database without one. */
  _ls_tls_t *tls = _ls_tls_get();
  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db) || IS_NULL_PTR(tls)) return FALSE;

  char key[512];
  snprintf(key, sizeof(key), "%s\x1f%s", maker ? maker : "", model);
  if(tls->cam_cached && !strcmp(key, tls->cam_key))
  {
    if(tls->cam_found) *out = tls->cam;
    return tls->cam_found ? TRUE : FALSE;
  }

  ls_camera_t cam;
  /* A miss is cached too: it costs a lookup to establish and it will not change. */
  const gboolean found = (ls_db_find_camera(db, maker, model, &cam) == 1) ? TRUE : FALSE;

  g_strlcpy(tls->cam_key, key, sizeof(tls->cam_key));
  tls->cam = cam;
  tls->cam_found = found;
  tls->cam_cached = TRUE;

  if(found) *out = cam;
  return found;
}

/**
 * @brief The lens a free-text name names, as an id.
 * @param mount_id the camera's mount, to prefer lenses that fit it; 0 for no preference.
 * @param crop the camera's crop factor, which decides between lenses that share a NAME and
 * differ only in the sensor they were calibrated on; 0 to ignore it.
 * @details The name comes from EXIF or from what the user typed, so this is the fuzzy
 * matcher, not a lookup.
 */
static long long _ls_find_lens(long long mount_id, float crop, const char *lens_name)
{
  if(IS_NULL_PTR(lens_name) || !lens_name[0]) return -1;
  _ls_tls_t *tls = _ls_tls_get();
  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db) || IS_NULL_PTR(tls)) return -1;

  char key[512];
  snprintf(key, sizeof(key), "%lld\x1f%.4f\x1f%s", mount_id, (double)crop, lens_name);
  if(tls->lens_cached && !strcmp(key, tls->lens_key)) return tls->lens_id;

  ls_db_match_t m[1];
  const long long id
      = (ls_db_match_lens(db, NULL, lens_name, mount_id, crop, m, 1) > 0) ? m[0].lens_id : -1;

  g_strlcpy(tls->lens_key, key, sizeof(tls->lens_key));
  tls->lens_id = id;
  tls->lens_cached = TRUE;
  return id;
}

typedef struct dt_iop_lensfun_data_t
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  dt_lens_type_t target_geom;
  gboolean do_nan_checks;
  gboolean tca_override;

  /** The lens as DATA: a value owned by this struct, read out of the database at commit
   *  and valid for as long as the struct is -- there is nothing here to free. */
  ls_lens_t ls_lens;
  gboolean ls_have;
} dt_iop_lensfun_data_t;


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
  if(old_version == 2 && new_version == 5)
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
      dt_lens_type_t target_geom;
      char camera[52];
      char lens[52];
      int tca_override;
      float tca_r, tca_b;
    } dt_iop_lensfun_params_v2_t;

    const dt_iop_lensfun_params_v2_t *o = (dt_iop_lensfun_params_v2_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

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
    n->modified = 1;

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }
  if(old_version == 3 && new_version == 5)
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
      dt_lens_type_t target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r, tca_b;
    } dt_iop_lensfun_params_v3_t;

    const dt_iop_lensfun_params_v3_t *o = (dt_iop_lensfun_params_v3_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    memcpy(n, o, sizeof(dt_iop_lensfun_params_t) - sizeof(int));

    // one more parameter and changed parameters in case we autodetect
    n->modified = 1;

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }

  if(old_version == 4 && new_version == 5)
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
      dt_lens_type_t target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r, tca_b;
      int modified;
    } dt_iop_lensfun_params_v4_t;

    const dt_iop_lensfun_params_v4_t *o = (dt_iop_lensfun_params_v4_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    memcpy(n, o, sizeof(dt_iop_lensfun_params_t));

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

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
/**
 * @brief Resolve the lens at one shooting configuration. THE modifier factory.
 *
 * @param mods_done receives the axes actually resolved, as DT_LENS_MODIFY_* -- an axis with
 * no calibration for this focal is absent, which is how every caller decides whether there
 * is anything to do.
 * @param w, h the frame the correction is expressed over, in pixels.
 * @param d the committed correction state.
 * @param mods_filter the axes the caller will accept, intersected with the user's own.
 * @param force_inverse flip the direction, for the callers that undo a correction.
 * @param mod filled in. It owns nothing: an ls_modifier_t is a value, so there is no
 * counterpart to the `delete modifier` this replaces and no way to leak one.
 * @return non-zero when at least one axis resolved.
 */
static int get_modifier(int *mods_done, int w, int h, const dt_iop_lensfun_data_t *d,
                        int mods_filter, gboolean force_inverse, ls_modifier_t *mod)
{
  memset(mod, 0, sizeof(*mod));
  if(mods_done) *mods_done = 0;
  if(!d->ls_have || d->crop <= 0.f) return 0;

  const int mods_todo = d->modify_flags & mods_filter;
  int want = 0;
  if(mods_todo & DT_LENS_MODIFY_DISTORTION) want |= LS_ENABLE_DISTORTION;
  if(mods_todo & DT_LENS_MODIFY_TCA) want |= LS_ENABLE_TCA;
  if(mods_todo & DT_LENS_MODIFY_VIGNETTING) want |= LS_ENABLE_VIGNETTING;
  if(mods_todo & DT_LENS_MODIFY_GEOMETRY) want |= LS_ENABLE_GEOMETRY;
  if(mods_todo & DT_LENS_MODIFY_SCALE) want |= LS_ENABLE_SCALE;

  const int reverse = force_inverse ? !d->inverse : d->inverse;
  const int got = ls_modifier_init(mod, &d->ls_lens, d->crop, w, h, d->focal, d->aperture,
                                   d->distance, d->scale, (int)d->target_geom, want, reverse);

  int done = 0;
  if(got & LS_ENABLE_DISTORTION) done |= DT_LENS_MODIFY_DISTORTION;
  if(got & LS_ENABLE_TCA) done |= DT_LENS_MODIFY_TCA;
  if(got & LS_ENABLE_VIGNETTING) done |= DT_LENS_MODIFY_VIGNETTING;
  if(got & LS_ENABLE_GEOMETRY) done |= DT_LENS_MODIFY_GEOMETRY;
  if(got & LS_ENABLE_SCALE) done |= DT_LENS_MODIFY_SCALE;

  /* A projection change LensSerious will not serve -- panoramic or equirectangular on
   * either side, which map x and y differently and are not radially expressible -- is
   * reported as not done rather than approximated. */
  if(mod->geometry_unsupported) done &= ~DT_LENS_MODIFY_GEOMETRY;

  if(mods_done) *mods_done = done;
  return done != 0;
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

__DT_CLONE_TARGETS__
int process(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  const int ch = piece->dsc_in.channels;
  const int ch_width = ch * roi_in->width;
  const int mask_display = pipe->mask_display;


  if(!d->ls_have || d->crop <= 0.0f)
  {
    dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, ch);
    return 0;
  }

  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int used_lf_mask = (raw_monochrome) ? DT_LENS_MODIFY_ALL & ~DT_LENS_MODIFY_TCA : DT_LENS_MODIFY_ALL;

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;

  dt_pthread_mutex_lock(dt_plugin_threadsafe_mutex());

  int modflags;
  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE, &modifier);
  dt_print(DT_DEBUG_PIPE, "[lens] resolved 0x%x of 0x%x requested (%d dist, %d tca, %d vig"
           " calibrations, crop %.4f, focal %.1f)\n", modflags, d->modify_flags,
           d->ls_lens.n_dist, d->ls_lens.n_tca, d->ls_lens.n_vig, (double)d->crop,
           (double)d->focal);


  dt_pthread_mutex_unlock(dt_plugin_threadsafe_mutex());

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  /* Vignetting is folded into the resampling loops below rather than run as a pass of its
   * own over a whole copy of the frame. ls_eval_vignette_factor() answers 1 when vignetting
   * is not enabled, so the loops need no second branch for it.
   *
   * Which FRAME the falloff lives in depends on the direction. Correcting, it belongs to
   * the source, so each channel takes the factor at ITS OWN source coordinate -- exactly
   * what the two-pass did, which darkened the input and then let each channel sample its
   * own position in it. Reversing, it is being put back onto the frame being produced, so
   * it is evaluated at the destination. */
  ls_eval_t vp;
  const gboolean have_vig = (modflags & DT_LENS_MODIFY_VIGNETTING)
                            && ls_eval_from_modifier(&modifier, &vp);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if(modflags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t bufsize = (size_t)roi_out->width * 2 * 3;

      size_t padded_bufsize;
      float *const buf = dt_pixelpipe_cache_alloc_perthread_float(bufsize, &padded_bufsize);
      if(IS_NULL_PTR(buf)) return 1;

#ifdef _OPENMP
#pragma omp parallel for default(none)  \
  firstprivate(roi_out, roi_in, padded_bufsize, modifier, ch, d, buf, ovoid, ivoid, ch_width, interpolation, raw_monochrome, mask_display, have_vig, vp)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *bufptr = (float*)dt_get_perthread(buf, padded_bufsize);
        ls_modifier_apply_subpixel_geometry(&modifier, roi_out->x, roi_out->y + y, roi_out->width, 1, bufptr);

        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, bufptr += 6, out += ch)
        {
          dt_aligned_pixel_simd_t pixel = { 0.f };
          for(int c = 0; c < 3; c++)
          {
            if(d->do_nan_checks && (!isfinite(bufptr[c * 2]) || !isfinite(bufptr[c * 2 + 1])))
            {
              pixel[c] = 0.0f;
              continue;
            }

            const float *const inptr = (const float *const)ivoid + (size_t)c;
            const float pi0 = fmaxf(fminf(bufptr[c * 2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
            const float pi1 = fmaxf(fminf(bufptr[c * 2 + 1] - roi_in->y, roi_in->height - 1.0f), 0.0f);
            pixel[c] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                                       roi_in->height, ch, ch_width);
          }

          if(have_vig)
          {
            /* Reversing: the falloff belongs to the frame being produced. */
            const float v = ls_eval_vignette_factor(&vp, (float)(roi_out->x + x),
                                                    (float)(roi_out->y + y));
            for(int c = 0; c < 3; c++) pixel[c] *= v;
          }
          if(raw_monochrome) pixel[0] = pixel[2] = pixel[1];

          if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
          {
            if(d->do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
            {
              pixel[3] = 0.0f;
            }
            else
            {
              // take green channel distortion also for alpha channel
              const float *const inptr = (const float *const)ivoid + (size_t)3;
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
      }
      dt_pixelpipe_cache_free_align(buf);
    }
    else
    {
      dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, ch);

      /* Nothing moved, so there was no resampling loop to fold the falloff into. */
      if(have_vig)
      {
        __OMP_PARALLEL_FOR__(firstprivate(modifier, ovoid, roi_out, ch))
        for(int y = 0; y < roi_out->height; y++)
        {
          float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
          ls_modifier_apply_vignetting(&modifier, roi_out->x, roi_out->y + y, roi_out->width, 1,
                                       out, (int)((ch * roi_out->width) * sizeof(float)));
        }
      }
    }
  }
  else // correct distortions:
  {
    /* No copy of the input, and no separate vignetting pass over it. This used to
     * duplicate the whole frame -- 387 MB for a 24 Mpx RGBA buffer -- darken the copy, and
     * resample from it. The falloff is a per-source-pixel gain, so folding it into the
     * resampling loop below gives the same answer while reading the caller's own buffer. */

    if(modflags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t buf2size = (size_t)roi_out->width * 2 * 3;
      size_t padded_buf2size;
      float *const buf2 = dt_pixelpipe_cache_alloc_perthread_float(buf2size, &padded_buf2size);
      if(IS_NULL_PTR(buf2)) return 1;


#ifdef _OPENMP
#pragma omp parallel for default(none)  \
  firstprivate(roi_out, roi_in, ovoid, ivoid, ch, padded_buf2size, modifier, mask_display, raw_monochrome, interpolation, ch_width, d, buf2, have_vig, vp)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *buf2ptr = (float*)dt_get_perthread(buf2, padded_buf2size);
        ls_modifier_apply_subpixel_geometry(&modifier, roi_out->x, roi_out->y + y, roi_out->width, 1, buf2ptr);
        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, buf2ptr += 6, out += ch)
        {
          dt_aligned_pixel_simd_t pixel = { 0.f };
          for(int c = 0; c < 3; c++)
          {
            if(d->do_nan_checks && (!isfinite(buf2ptr[c * 2]) || !isfinite(buf2ptr[c * 2 + 1])))
            {
              pixel[c] = 0.0f;
              continue;
            }

            const float *bufptr = ((const float *)ivoid) + c;
            const float pi0 = fmaxf(fminf(buf2ptr[c * 2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
            const float pi1 = fmaxf(fminf(buf2ptr[c * 2 + 1] - roi_in->y, roi_in->height - 1.0f), 0.0f);
            pixel[c] = dt_interpolation_compute_sample(interpolation, bufptr, pi0, pi1, roi_in->width,
                                                       roi_in->height, ch, ch_width);
            /* Correcting: the falloff belongs to the source, so each channel takes it at
             * its own source coordinate -- which is what sampling an already-darkened input
             * amounted to. */
            if(have_vig)
              pixel[c] *= ls_eval_vignette_factor(&vp, buf2ptr[c * 2], buf2ptr[c * 2 + 1]);
          }
          if(raw_monochrome) pixel[0] = pixel[2] = pixel[1];
          if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
          {
            if(d->do_nan_checks && (!isfinite(buf2ptr[2]) || !isfinite(buf2ptr[3])))
            {
              pixel[3] = 0.0f;
            }
            else
            {
              // take green channel distortion also for alpha channel
              const float *bufptr = ((const float *)ivoid) + 3;
              const float pi0 = fmaxf(fminf(buf2ptr[2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
              const float pi1 = fmaxf(fminf(buf2ptr[3] - roi_in->y, roi_in->height - 1.0f), 0.0f);
              pixel[3] = dt_interpolation_compute_sample(interpolation, bufptr, pi0, pi1, roi_in->width,
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
      }
      dt_pixelpipe_cache_free_align(buf2);
    }
    else
    {
      dt_iop_image_copy_by_size((float *)ovoid, (float *)ivoid, roi_out->width, roi_out->height, ch);

      /* Nothing moved, so there was no resampling loop to fold the falloff into. */
      if(have_vig)
      {
        __OMP_PARALLEL_FOR__(firstprivate(modifier, ovoid, roi_in, ch))
        for(int y = 0; y < roi_in->height; y++)
        {
          float *out = ((float *)ovoid) + (size_t)ch * roi_in->width * y;
          ls_modifier_apply_vignetting(&modifier, roi_in->x, roi_in->y + y, roi_in->width, 1, out,
                                       (int)((ch * roi_in->width) * sizeof(float)));
        }
      }
    }
  }

  /* No GUI state is written here. Which corrections apply is a property of the
   * camera/lens/params combination, not of a rendered frame -- the label is computed on the
   * GUI thread by _lens_corrections_available(). */
  return 0;
}

#ifdef HAVE_OPENCL


int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int used_lf_mask = (raw_monochrome) ? DT_LENS_MODIFY_ALL & ~DT_LENS_MODIFY_TCA : DT_LENS_MODIFY_ALL;

  cl_int err = -999;

  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  ls_modifier_t modifier;
  /* Declared before the first `goto error`: C++ forbids jumping over an initialisation. */
  ls_eval_t p;
  gboolean have_eval = FALSE, do_geom = FALSE, do_vig = FALSE;

  const int devid = pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const int roi_in_x = roi_in->x;
  const int roi_in_y = roi_in->y;
  const int roi_out_x = roi_out->x;
  const int roi_out_y = roi_out->y;

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;

  size_t origin[] = { 0, 0, 0 };
  size_t oregion[] = { (size_t)owidth, (size_t)oheight, 1 };
  size_t isizes[] = { (size_t)ROUNDUPDWD(iwidth, devid), (size_t)ROUNDUPDHT(iheight, devid), 1 };
  size_t osizes[] = { (size_t)ROUNDUPDWD(owidth, devid), (size_t)ROUNDUPDHT(oheight, devid), 1 };

  int modflags;
  int ldkernel = -1;
  /* Declared here, ahead of every `goto error`: C++ will not let one jump over an
   * initialisation. Resolved once below, after get_modifier() has settled modflags. */
  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  if(!d->ls_have || d->crop <= 0.0f)
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
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
      return FALSE;
  }


  get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE, &modifier);

  /* One kernel, in and out, in both directions.
   *
   * The correction crosses as an ls_eval_t -- 632 bytes of coefficients passed by value --
   * and each work-item evaluates its own source coordinates from it, so there is no
   * displacement map, no host buffer and no upload. Vignetting rides along inside the same
   * resampling pass rather than writing a whole intermediate image for the resampler to
   * read back.
   *
   * The direction lives in the block: ls_eval_map() reads p.reverse and composes the chain
   * accordingly, and _lens_devignette() places the falloff in the frame that direction puts
   * it in. So both directions are the same launch, which is why the branch that used to
   * distinguish them is gone. */
  have_eval = ls_eval_from_modifier(&modifier, &p) != 0;
  do_geom = have_eval
      && (modflags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY
                      | DT_LENS_MODIFY_SCALE)) != 0;
  do_vig = have_eval && (modflags & DT_LENS_MODIFY_VIGNETTING) != 0;

  if(do_geom)
  {
    /* Vignetting, if any, is applied inside this pass -- the kernel reads it out of p. */
    dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
    dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
    dt_opencl_set_kernel_arg(devid, ldkernel, 6, sizeof(int), (void *)&roi_in_x);
    dt_opencl_set_kernel_arg(devid, ldkernel, 7, sizeof(int), (void *)&roi_in_y);
    dt_opencl_set_kernel_arg(devid, ldkernel, 8, sizeof(int), (void *)&roi_out_x);
    dt_opencl_set_kernel_arg(devid, ldkernel, 9, sizeof(int), (void *)&roi_out_y);
    dt_opencl_set_kernel_arg(devid, ldkernel, 10, sizeof(ls_eval_t), (void *)&p);
    dt_opencl_set_kernel_arg(devid, ldkernel, 11, sizeof(int), (void *)&(d->do_nan_checks));
    dt_opencl_set_kernel_arg(devid, ldkernel, 12, sizeof(int), (void *)&(raw_monochrome));
    err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(do_vig)
  {
    /* Nothing moves, so there is nothing to resample: a dedicated pass costs one fetch per
     * pixel where the fused one would cost the resampler's full tap count for an identity
     * map. Which frame the falloff belongs to is the same question as above, and with no
     * geometry in play the two coincide. */
    const int vx = d->inverse ? roi_out_x : roi_in_x;
    const int vy = d->inverse ? roi_out_y : roi_in_y;
    const int vw = d->inverse ? owidth : iwidth;
    const int vh = d->inverse ? oheight : iheight;
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 2, sizeof(int), (void *)&vw);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 3, sizeof(int), (void *)&vh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 4, sizeof(int), (void *)&vx);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 5, sizeof(int), (void *)&vy);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 6, sizeof(ls_eval_t), (void *)&p);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette,
                                      d->inverse ? osizes : isizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
  }

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_lens] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  /* CPU: in + out, and nothing else of image size.
   *
   * The whole-frame copy process() used to make -- to darken before resampling from it --
   * is gone with the separate vignetting pass: the falloff is folded into the resampling
   * loop, which now reads the caller's own input buffer. The displacement map is not a
   * whole-image temporary either: it is built a row at a time into a per-thread buffer of
   * width*6 floats, so it grows with the frame's WIDTH and the thread count rather than
   * its area -- ~2 MB for a 6000 px frame on 16 threads, against ~384 MB for one 24 Mpx
   * RGBA buffer. Counting it here would reserve memory nothing allocates.
   *
   * GPU: in + out, and nothing else at all -- one kernel reads the input and writes the
   * output, with vignetting folded into the same pass.
   *
   * Both figures used to be 4.5, meaning in + out + tmp + a six-float-per-pixel map buffer
   * (1.5x an RGBA one) that had to be built on the host and uploaded. The GPU path no
   * longer has that buffer -- each work-item evaluates its own coordinates from ~80 bytes
   * of coefficients passed as a kernel argument -- so reserving 1.5 image buffers for it
   * made the tile solver split frames that would have fitted whole.
   *
   * factor_cl and maxbuf_cl have to be set explicitly: dt_develop_tiling_t defaults them to
   * the CPU figures (develop/tiling.c), so a module that sets only `factor` silently
   * describes its GPU path with its CPU path's appetite. */
  tiling->factor = 2.0f;    // in + out
  tiling->maxbuf = 1.0f;
  tiling->factor_cl = 2.0f; // in + out; no intermediate at all
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

int distort_transform(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                      float *const __restrict points, size_t points_count)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  if(!d->ls_have || d->crop <= 0.0f) return 0;

  const float orig_w = piece->buf_in.width, orig_h = piece->buf_in.height;
  int modflags;

  const int used_lf_mask = (dt_image_is_monochrome(&self->dev->image_storage)) ? DT_LENS_MODIFY_ALL & ~DT_LENS_MODIFY_TCA : DT_LENS_MODIFY_ALL;

  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, TRUE, &modifier);

  if(modflags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE))
  {
    __OMP_PARALLEL_FOR__(firstprivate(points, points_count, modifier) if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      ls_modifier_apply_subpixel_geometry(&modifier, points[i], points[i + 1], 1, 1, buf);
      // take green channel distortion, like distort_mask() does, so x and y come from the
      // same color channel's distortion field instead of mixing red's x with green's y.
      points[i] = buf[2];
      points[i + 1] = buf[3];
    }
  }


  return 1;
}

int distort_backtransform(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                          float *const __restrict points, size_t points_count)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  if(!d->ls_have || d->crop <= 0.0f) return 0;

  const int used_lf_mask = (dt_image_is_monochrome(&self->dev->image_storage)) ? DT_LENS_MODIFY_ALL & ~DT_LENS_MODIFY_TCA : DT_LENS_MODIFY_ALL;

  const float orig_w = piece->buf_in.width, orig_h = piece->buf_in.height;
  int modflags;
  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE, &modifier);


  if(modflags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE))
  {
    __OMP_PARALLEL_FOR__(firstprivate(points_count, modifier, points) if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      ls_modifier_apply_subpixel_geometry(&modifier, points[i], points[i + 1], 1, 1, buf);
      // take green channel distortion, like distort_mask() does, so x and y come from the
      // same color channel's distortion field instead of mixing red's x with green's y.
      points[i] = buf[2];
      points[i + 1] = buf[3];
    }
  }


  return 1;
}

// TODO: Shall we keep DT_LENS_MODIFY_TCA in the modifiers?
void distort_mask(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece,
                  const float *const in, float *const out, const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  if(!d->ls_have || d->crop <= 0.0f)
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;
  dt_pthread_mutex_lock(dt_plugin_threadsafe_mutex());
  int modflags;
  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d,
               /*DT_LENS_MODIFY_TCA |*/ DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY
                   | DT_LENS_MODIFY_SCALE,
               FALSE, &modifier);

  dt_pthread_mutex_unlock(dt_plugin_threadsafe_mutex());

  if(!(modflags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE)))
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  // acquire temp memory for distorted pixel coords
  const size_t bufsize = (size_t)roi_out->width * 2 * 3;
  size_t padded_bufsize;
  float *const buf = dt_pixelpipe_cache_alloc_perthread_float(bufsize, &padded_bufsize);
  if(IS_NULL_PTR(buf)) return;
  __OMP_PARALLEL_FOR__(firstprivate(buf, padded_bufsize, d, modifier, in, out, interpolation, roi_in, roi_out))
  for(int y = 0; y < roi_out->height; y++)
  {
    float *bufptr = (float*)dt_get_perthread(buf, padded_bufsize);
    ls_modifier_apply_subpixel_geometry(&modifier, roi_out->x, roi_out->y + y, roi_out->width, 1, bufptr);

    // reverse transform the global coords from lf to our buffer
    float *_out = out + (size_t)y * roi_out->width;
    for(int x = 0; x < roi_out->width; x++, bufptr += 6, _out++)
    {
      if(d->do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
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
}

void modify_roi_out(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                    struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

void modify_roi_in(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                   struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  *roi_in = *roi_out;
  // inverse transform with given params

  if(!d->ls_have || d->crop <= 0.0f) return;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  int modflags;
  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d, DT_LENS_MODIFY_ALL, FALSE, &modifier);

  if(modflags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE))
  {
    const int xoff = roi_in->x;
    const int yoff = roi_in->y;
    const int width = roi_in->width;
    const int height = roi_in->height;
    const int awidth = abs(width);
    const int aheight = abs(height);
    const int xstep = (width < 0) ? -1 : 1;
    const int ystep = (height < 0) ? -1 : 1;

    float xm = FLT_MAX, xM = -FLT_MAX, ym = FLT_MAX, yM = -FLT_MAX;
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
        ls_modifier_apply_subpixel_geometry(&modifier, xoff + i * xstep, yoff, 1, 1, buf + 6 * i);
      __OMP_FOR__()
      for(int i = 0; i < awidth; i++)
        ls_modifier_apply_subpixel_geometry(&modifier, xoff + i * xstep, yoff + (height - 1), 1, 1, buf + 6 * (awidth + i));
      __OMP_FOR__()
      for(int j = 0; j < aheight; j++)
        ls_modifier_apply_subpixel_geometry(&modifier, xoff, yoff + j * ystep, 1, 1, buf + 6 * (2 * awidth + j));
      __OMP_FOR__()
      for(int j = 0; j < aheight; j++)
        ls_modifier_apply_subpixel_geometry(&modifier, xoff + (width - 1), yoff + j * ystep, 1, 1, buf + 6 * (2 * awidth + aheight + j));

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
}

/* --- the shared geometry core ----------------------------------------------------------
 *
 * lens resolves its effective parameters and then builds a lensfun state out of them, and both
 * halves are needed twice: once for the pixel pipe, once for the record the geometry service
 * composes GUI coordinates from (develop/geometry/geometry.h). Expressed once here.
 *
 * Note what lens does NOT contribute: modify_roi_out() is the identity, so this module changes
 * no dimensions. It is on the geometry roster purely for its point transforms.
 */

/**
 * @brief Which parameters are actually in force.
 *
 * @details p->modified == 0 means "auto": the user never touched the GUI after autodetection,
 * and the parameters that describe the correction are the module's DEFAULTS, filled in by
 * reload_defaults() from the image's EXIF -- not the ones in history. A record built from
 * history alone would describe a correction the pipe is not applying, on exactly the images
 * where lens correction is automatic, which is most of them.
 */
static const dt_iop_lensfun_params_t *_lens_effective_params(dt_iop_module_t *self,
                                                             const dt_iop_lensfun_params_t *const p)
{
  return (p->modified == 0) ? (const dt_iop_lensfun_params_t *)self->default_params : p;
}

/**
 * @brief Build the lensfun state from resolved parameters. THE constructor.
 *
 * @details @p d is zeroed or already owns a lens; either way it owns a fresh deep copy of the
 * database's calibration on return as a VALUE -- nothing is owned, nothing is freed, and
 * no lock is taken.
 */
static void _lens_build_data(dt_iop_module_t *self, const dt_iop_lensfun_params_t *const p,
                             dt_iop_lensfun_data_t *d)
{
  (void)self;
  memset(&d->ls_lens, 0, sizeof(d->ls_lens));
  d->ls_have = FALSE;

  /* No lock. The reader is lock-free by construction and its handle is thread-local, so a
   * pipeline thread resolving a lens no longer serialises against anything -- least of all
   * against RawSpeed decoding a file, which is what sharing dt_plugin_threadsafe_mutex()
   * used to mean. And nothing is owned on return: an ls_lens_t is a value, valid after the
   * handle that produced it is closed, so there is no deep copy to make and no delete to
   * forget. */
  long long mount_id = 0;
  float camera_crop = 0.f;
  if(p->camera[0])
  {
    ls_camera_t camera;
    /* The stored camera name is a model with no maker -- what the picker writes and what
     * EXIF gives -- so the matcher is asked for one rather than guessing the other. */
    if(_ls_find_camera(NULL, p->camera, &camera))
    {
      d->crop = camera.crop_factor;
      camera_crop = camera.crop_factor;
      mount_id = camera.mount_id;
    }
  }

  if(p->lens[0])
  {
    const long long lens_id = _ls_find_lens(mount_id, camera_crop, p->lens);
    ls_db_t *db = _ls_db();
    if(lens_id >= 0 && !IS_NULL_PTR(db) && ls_db_lens_by_id(db, lens_id, &d->ls_lens) == 1)
    {
      d->ls_have = TRUE;

      if(p->tca_override)
      {
        /* A manual override REPLACES the calibration rather than being added beside it.
         * ls_lens_t is this module's own copy, so overwriting the array is both cheaper and
         * clearer than upstream's remove-every-entry-then-add dance on a shared object --
         * which is what the code here used to do, twice, under two different lensfun APIs.
         * One entry at the shooting focal is exactly what the two sliders describe. */
        d->ls_lens.n_tca = 1;
        d->ls_lens.tca[0].model = LS_TCA_LINEAR;
        d->ls_lens.tca[0].focal = p->focal;
        d->ls_lens.tca[0].terms[0] = p->tca_r;
        d->ls_lens.tca[0].terms[1] = p->tca_b;
        for(int i = 2; i < 6; i++) d->ls_lens.tca[0].terms[i] = 0.f;
      }
    }
  }

  d->modify_flags = p->modify_flags;
  if(dt_image_is_monochrome(&self->dev->image_storage)) d->modify_flags &= ~DT_LENS_MODIFY_TCA;
  d->inverse = p->inverse;
  d->scale = p->scale;
  d->focal = p->focal;
  d->aperture = p->aperture;
  d->distance = p->distance;
  d->target_geom = p->target_geom;
  d->do_nan_checks = TRUE;
  d->tca_override = p->tca_override;

  /*
   * there are certain situations when LensFun can return NAN coordinated.
   * most common case would be when the FOV is increased.
   */
  if(d->target_geom == DT_LENS_RECTILINEAR)
  {
    d->do_nan_checks = FALSE;
  }
  else if((int)d->target_geom == (int)d->ls_lens.type)
  {
    d->do_nan_checks = FALSE;
  }
}

/** @brief The lensfun modify mask this image allows: monochrome sensors get no TCA correction. */
static int _lens_used_mask(dt_iop_module_t *self)
{
  return dt_image_is_monochrome(&self->dev->image_storage) ? (DT_LENS_MODIFY_ALL & ~DT_LENS_MODIFY_TCA)
                                                           : DT_LENS_MODIFY_ALL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_lensfun_params_t *p = _lens_effective_params(self, (dt_iop_lensfun_params_t *)p1);

  // FIXME: this is utter shit and should be made into a GUI "mode".
  // If p->modified == 0, mode = auto and hide all controls
  // if p->modidified == 1, mode = manual and show all controls.
  if(((dt_iop_lensfun_params_t *)p1)->modified == 0)
  {
    // Temporary fix pending GUI unfucking
    dt_iop_compute_module_hash(self, self->dev->forms);
  }

  _lens_build_data(self, p, (dt_iop_lensfun_data_t *)piece->data);

  piece->cache_output_on_ram = TRUE;
}

/* --- the geometry service's view of this module (develop/geometry/geometry.h) ---------
 *
 * The one record in the service whose payload is not plain data: evaluating a lens correction
 * needs the resolved calibration, so the record owns a
 * deep copy and frees it. That is what dt_geometry_record_t::free_data exists for.
 */

typedef struct dt_iop_lens_geometry_t
{
  dt_iop_lensfun_data_t data;   /**< its own copy, exactly like a pipe piece has */
  int used_lf_mask;
} dt_iop_lens_geometry_t;

static void _lens_free_data(void *ptr)
{
  dt_iop_lens_geometry_t *g = (dt_iop_lens_geometry_t *)ptr;
  if(!g) return;
  free(g);
}

/** @brief Apply the correction to points. @p inverse selects the direction, as get_modifier()
 *  means it: distort_transform() passes TRUE, distort_backtransform() passes FALSE. */
static int _lens_geometry_apply(const void *data, const dt_geometry_record_t *const record,
                                float *points, size_t points_count, gboolean inverse)
{
  const dt_iop_lens_geometry_t *const g = (const dt_iop_lens_geometry_t *)data;
  const dt_iop_lensfun_data_t *const d = &g->data;

  if(!d->ls_have || d->crop <= 0.0f) return 0;
  if(record->in.width <= 0 || record->in.height <= 0) return 0;

  int modflags = 0;
  ls_modifier_t modifier;
  if(!get_modifier(&modflags, record->in.width, record->in.height, d, g->used_lf_mask, inverse,
                   &modifier))
    return 0;

  if(modflags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE))
  {
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      ls_modifier_apply_subpixel_geometry(&modifier, points[i], points[i + 1], 1, 1, buf);
      // green channel, like distort_transform() and distort_mask() do, so x and y come from the
      // same colour channel's distortion field instead of mixing red's x with green's y.
      points[i] = buf[2];
      points[i + 1] = buf[3];
    }
  }

  return 1;
}

static int _lens_geometry_transform(const void *data, const dt_geometry_record_t *const record,
                                    dt_geometry_chain_t *chain, float *points, size_t points_count)
{
  return _lens_geometry_apply(data, record, points, points_count, TRUE);
}

static int _lens_geometry_backtransform(const void *data, const dt_geometry_record_t *const record,
                                        dt_geometry_chain_t *chain, float *points, size_t points_count)
{
  return _lens_geometry_apply(data, record, points, points_count, FALSE);
}

static const dt_geometry_vtable_t _lens_geometry_vtable = {
  /* .map_size = */ NULL,   // modify_roi_out() is the identity: lens changes no dimensions
  /* .transform = */ _lens_geometry_transform,
  /* .backtransform = */ _lens_geometry_backtransform,
};

gboolean geometry_record(dt_iop_module_t *self, const void *params, dt_geometry_record_t *record)
{
  dt_iop_lens_geometry_t *g = (dt_iop_lens_geometry_t *)calloc(1, sizeof(dt_iop_lens_geometry_t));
  if(!g) return FALSE;

  const dt_iop_lensfun_params_t *p
      = _lens_effective_params(self, (const dt_iop_lensfun_params_t *)params);
  _lens_build_data(self, p, &g->data);
  g->used_lf_mask = _lens_used_mask(self);

  record->data = g;
  record->free_data = _lens_free_data;
  record->vtable = &_lens_geometry_vtable;
  return TRUE;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_lensfun_data_t));
  piece->data_size = sizeof(dt_iop_lensfun_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  /* init_pipe() may have failed to allocate, and cleanup runs regardless. */
  if(IS_NULL_PTR(piece->data)) return;

  /* Nothing to free but the piece itself: ls_lens_t is a value living inside it, where the
   * lfLens it replaces was a heap object this had to remember to delete. */
  dt_free_align(piece->data);
  piece->data = NULL;
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

  /* Nothing to pre-warm any more. Opening the calibration database is one mmap of an
   * already-parsed file, done lazily per thread on first use and measured at 0.18 ms --
   * there is no 100 ms XML parse left to hide behind a startup thread. */
}

static float get_autoscale(dt_iop_module_t *self, dt_iop_lensfun_params_t *p);

void reload_defaults(dt_iop_module_t *module)
{
  char *new_lens;
  const dt_image_t *img = &module->dev->image_storage;

  // reload image specific stuff
  // get all we can from exif:
  dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)module->default_params;

  new_lens = _lens_sanitize(img->exif_lens);
  g_strlcpy(d->lens, new_lens, sizeof(d->lens));
  dt_free(new_lens);
  g_strlcpy(d->camera, img->exif_model, sizeof(d->camera));
  d->crop = img->exif_crop;
  d->aperture = img->exif_aperture;
  d->focal = img->exif_focal_length;
  d->scale = 1.0;
  d->modify_flags = DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_VIGNETTING | DT_LENS_MODIFY_DISTORTION |
                    DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE;
  // if we did not find focus_distance in EXIF, lets default to 1000
  d->distance = img->exif_focus_distance == 0.0f ? 1000.0f : img->exif_focus_distance;
  d->target_geom = DT_LENS_RECTILINEAR;

  if(dt_image_is_monochrome(img))
    d->modify_flags &= ~DT_LENS_MODIFY_TCA;

  // init crop from db:
  char model[100]; // truncate often complex descriptions.
  g_strlcpy(model, img->exif_model, sizeof(model));
  for(char cnt = 0, *c = model; c < model + 100 && *c != '\0'; c++)
    if(*c == ' ')
      if(++cnt == 2) *c = '\0';
  if(img->exif_maker[0] || model[0])
  {
    ls_camera_t cam;
    if(!_ls_find_camera(img->exif_maker, img->exif_model, &cam)) return;

    /* Upstream spells a real fact into the mount NAME: a lower-case initial means a
     * fixed-lens camera. That is how a compact is told from an interchangeable-lens body,
     * and it decides both branches below. */
    char mount[128] = { 0 };
    ls_db_t *db = _ls_db();
    if(IS_NULL_PTR(db)) return;
    ls_db_mount_name(db, cam.mount_id, mount, sizeof(mount));
    const gboolean fixed_lens = (mount[0] != '\0') && islower((unsigned char)mount[0]);

    long long lens_id = _ls_find_lens(cam.mount_id, cam.crop_factor, d->lens);

    if(lens_id < 0 && fixed_lens)
    {
      /* A fixed-lens camera whose EXIF lens string matched nothing -- it is "(65535)", or
       * a name upstream files as "fixed lens". The lens is whatever is built into this
       * mount, so ask the mount directly instead of matching a name. */
      g_strlcpy(d->lens, "", sizeof(d->lens));

      const int n = ls_db_lenses_for_mount(db, cam.mount_id, NULL, 0);
      if(n > 0)
      {
        long long *ids = (long long *)dt_alloc_align(sizeof(long long) * (size_t)n);
        if(!IS_NULL_PTR(ids))
        {
          ls_db_lenses_for_mount(db, cam.mount_id, ids, n);
          /* The shortest model name, as before: a fixed-lens mount can carry several
           * entries for one physical lens and the shortest is the plain one. */
          size_t shortest = SIZE_MAX;
          for(int i = 0; i < n; i++)
          {
            char maker[128] = "", lmodel[256] = "";
            if(ls_db_lens_name(db, ids[i], maker, sizeof(maker), lmodel, sizeof(lmodel)) <= 0)
              continue;
            const size_t len = strlen(lmodel);
            if(len < shortest)
            {
              shortest = len;
              lens_id = ids[i];
              g_strlcpy(d->lens, lmodel, sizeof(d->lens));
            }
          }
          dt_free_align(ids);
        }
      }
    }

    if(lens_id >= 0)
    {
      ls_lens_t lens;
      if(ls_db_lens_by_id(db, lens_id, &lens) == 1)
        d->target_geom = (dt_lens_type_t)lens.type;
    }

    d->crop = cam.crop_factor;
    d->scale = get_autoscale(module, d);
    module->workflow_enabled = dt_image_needs_rawprepare(img);
  }

  // The corrections-done message reset lives in gui_update() now (GUI thread, live widget);
  // reload_defaults() stays params-only and never touches gui_data.
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)module->data;

  /* Before anything is freed: the pre-warm thread may still be building the database. */
  /* No database to tear down and no thread to join. Each thread's handle closes itself
   * when that thread ends, and the one-entry caches beside it die with it. */

  dt_opencl_free_kernel(gd->kernel_lens_distort_bilinear);
  dt_opencl_free_kernel(gd->kernel_lens_distort_bicubic);
  dt_opencl_free_kernel(gd->kernel_lens_distort_mitchell);
  dt_opencl_free_kernel(gd->kernel_lens_vignette);
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

/**
 * @brief Show a camera in the GUI and write it into the params.
 * @param camera_id the database id, or < 0 to clear the widget.
 *
 * @details It takes an ID rather than a pointer because a camera is no longer a durable
 * object owned by a process-wide database -- it is a row, read on demand. The menu items
 * below carry the same id, so nothing holds a pointer whose lifetime it does not control.
 */
static void camera_set(dt_iop_module_t *self, long long camera_id)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  ls_db_t *db = _ls_db();
  char maker[128] = "", model[256] = "", variant[128] = "";
  ls_camera_t cam;
  if(camera_id < 0 || IS_NULL_PTR(db)
     || ls_db_camera_name(db, camera_id, maker, sizeof(maker), model, sizeof(model),
                          variant, sizeof(variant)) != 1
     || ls_db_camera_by_id(db, camera_id, &cam) != 1)
  {
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), "");
    gtk_widget_set_tooltip_text(GTK_WIDGET(g->camera_model), "");
    g->camera_id = -1;
    return;
  }

  g_strlcpy(p->camera, model, sizeof(p->camera));
  p->crop = cam.crop_factor;
  g->camera_id = camera_id;

  gchar *fm = maker[0] ? g_strdup_printf("%s, %s", maker, model) : g_strdup(model);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), fm);
  dt_free(fm);

  char _variant[128];
  if(variant[0])
    snprintf(_variant, sizeof(_variant), " (%s)", variant);
  else
    _variant[0] = 0;

  char mount[128] = "";
  ls_db_mount_name(db, cam.mount_id, mount, sizeof(mount));

  fm = g_strdup_printf(_("maker:\t\t%s\n"
                         "model:\t\t%s%s\n"
                         "mount:\t\t%s\n"
                         "crop factor:\t%.1f"),
                       maker, model, _variant, mount, cam.crop_factor);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->camera_model), fm);
  dt_free(fm);
}

static void camera_menu_select(GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  camera_set(self, (long long)GPOINTER_TO_INT(
                       g_object_get_data(G_OBJECT(menuitem), "lens-camera-id")));
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

/**
 * @brief Build the camera picker from a list of database ids.
 *
 * @param ids the cameras to offer, @p n of them. Grouped by maker into submenus, as before.
 */
static void camera_menu_fill(dt_iop_module_t *self, const long long *ids, int n)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  GPtrArray *makers, *submenus;

  if(g->camera_menu)
  {
    gtk_widget_destroy(GTK_WIDGET(g->camera_menu));
    g->camera_menu = NULL;
  }

  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db)) return;

  /* Count all existing camera makers and create a sorted list */
  makers = g_ptr_array_new_with_free_func(dt_free_gpointer);
  submenus = g_ptr_array_new();
  for(int i = 0; i < n; i++)
  {
    char maker[128] = "", model[256] = "", variant[128] = "";
    if(ls_db_camera_name(db, ids[i], maker, sizeof(maker), model, sizeof(model),
                         variant, sizeof(variant)) != 1)
      continue;

    GtkWidget *submenu, *item;
    int idx = ptr_array_find_sorted(makers, maker, (GCompareFunc)g_utf8_collate);
    if(idx < 0)
    {
      /* No such maker yet, insert it into the array. The strings are OWNED now: they used
       * to point into a database that outlived the menu, and they no longer do. */
      idx = ptr_array_insert_sorted(makers, g_strdup(maker), (GCompareFunc)g_utf8_collate);
      /* Create a submenu for cameras by this maker */
      submenu = gtk_menu_new();
      ptr_array_insert_index(submenus, submenu, idx);
    }

    submenu = (GtkWidget *)g_ptr_array_index(submenus, idx);
    /* Append current camera name to the submenu */
    if(!variant[0])
      item = gtk_menu_item_new_with_label(model);
    else
    {
      gchar *fm = g_strdup_printf("%s (%s)", model, variant);
      item = gtk_menu_item_new_with_label(fm);
      dt_free(fm);
    }
    gtk_widget_show(item);
    g_object_set_data(G_OBJECT(item), "lens-camera-id", GINT_TO_POINTER((gint)ids[i]));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(camera_menu_select), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
  }

  g->camera_menu = GTK_MENU(gtk_menu_new());
  for(unsigned i = 0; i < makers->len; i++)
  {
    GtkWidget *item = (GtkWidget *)gtk_menu_item_new_with_label((const gchar *)g_ptr_array_index(makers, i));
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->camera_menu), item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), (GtkWidget *)g_ptr_array_index(submenus, i));
  }

  g_ptr_array_free(submenus, TRUE);
  g_ptr_array_free(makers, TRUE);
}

/** @brief Every camera in the database, as ids the caller must free with dt_free_align(). */
static long long *_camera_all_ids(int *out_n)
{
  *out_n = 0;
  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db)) return NULL;
  const int n = ls_db_list_cameras(db, NULL, 0);
  if(n <= 0) return NULL;
  long long *ids = (long long *)dt_alloc_align(sizeof(long long) * (size_t)n);
  if(IS_NULL_PTR(ids)) return NULL;
  *out_n = ls_db_list_cameras(db, ids, n);
  return ids;
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
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);

  (void)button;

  int n = 0;
  long long *ids = _camera_all_ids(&n);
  if(IS_NULL_PTR(ids)) return;
  camera_menu_fill(self, ids, n);
  dt_free_align(ids);

  dt_gui_menu_popup(GTK_MENU(g->camera_menu), button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static void camera_autosearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  char model[200];
  const gchar *txt = (const gchar *)((dt_iop_lensfun_params_t *)self->default_params)->camera;

  (void)button;

  if(txt[0] == '\0')
  {
    int n = 0;
    long long *ids = _camera_all_ids(&n);
    if(IS_NULL_PTR(ids)) return;
    camera_menu_fill(self, ids, n);
    dt_free_align(ids);
  }
  else
  {
    parse_model(txt, model, sizeof(model));
    ls_camera_t cam;
    if(!_ls_find_camera(NULL, model, &cam)) return;
    camera_menu_fill(self, &cam.id, 1);
  }

  dt_gui_menu_popup(GTK_MENU(g->camera_menu), button, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

/* -- end camera -- */

static void lens_comboentry_focal_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->focal);
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void lens_comboentry_aperture_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->aperture);
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void lens_comboentry_distance_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->distance);
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void delete_children(GtkWidget *widget, gpointer data)
{
  (void)data;
  gtk_widget_destroy(widget);
}

/** @brief A projection's name, replacing lfLens::GetLensTypeDesc(). */
static const char *_lens_type_name(int type)
{
  switch(type)
  {
    case DT_LENS_RECTILINEAR:           return _("rectilinear");
    case DT_LENS_FISHEYE:               return _("fisheye");
    case DT_LENS_PANORAMIC:             return _("panoramic");
    case DT_LENS_EQUIRECTANGULAR:       return _("equirectangular");
    case DT_LENS_FISHEYE_ORTHOGRAPHIC:  return _("orthographic fisheye");
    case DT_LENS_FISHEYE_STEREOGRAPHIC: return _("stereographic fisheye");
    case DT_LENS_FISHEYE_EQUISOLID:     return _("equisolid fisheye");
    case DT_LENS_FISHEYE_THOBY:         return _("Thoby fisheye");
    default:                            return _("unknown");
  }
}

static void lens_set(dt_iop_module_t *self, long long lens_id)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

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

  ls_db_t *db = _ls_db();
  ls_lens_t lens_v;
  char l_maker[128] = "", l_model[256] = "";
  float min_focal = 0.f, max_focal = 0.f, min_ap = 0.f, max_ap = 0.f;
  const gboolean have = (lens_id >= 0) && !IS_NULL_PTR(db)
                        && (ls_db_lens_by_id(db, lens_id, &lens_v) == 1)
                        && (ls_db_lens_name(db, lens_id, l_maker, sizeof(l_maker),
                                            l_model, sizeof(l_model)) > 0);
  if(have) ls_db_lens_range(db, lens_id, &min_focal, &max_focal, &min_ap, &max_ap);

  if(!have)
  {
    gtk_widget_set_sensitive(GTK_WIDGET(g->modflags), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->target_geom), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->scale), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->reverse), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->tca_r), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->tca_b), FALSE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->message), FALSE);

    g->trouble = TRUE;
    return;
  }
  else
  {
    // no longer in trouble
    gtk_widget_set_sensitive(GTK_WIDGET(g->modflags), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->target_geom), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->scale), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->reverse), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->tca_r), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->tca_b), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(g->message), TRUE);

    g->trouble = FALSE;
  }

  maker = l_maker[0] ? l_maker : NULL;
  model = l_model[0] ? l_model : NULL;

  g_strlcpy(p->lens, l_model, sizeof(p->lens));

  if(model)
  {
    if(maker)
      fm = g_strdup_printf("%s, %s", maker, model);
    else
      fm = g_strdup_printf("%s", model);
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), fm);
    dt_free(fm);
  }

  char focal[100], aperture[100], mounts[200];

  if(min_focal < max_focal)
    snprintf(focal, sizeof(focal), "%g-%gmm", min_focal, max_focal);
  else
    snprintf(focal, sizeof(focal), "%gmm", min_focal);
  if(min_ap < max_ap)
    snprintf(aperture, sizeof(aperture), "%g-%g", min_ap, max_ap);
  else
    snprintf(aperture, sizeof(aperture), "%g", min_ap);

  mounts[0] = 0;
  ls_db_lens_mounts(db, lens_id, mounts, sizeof(mounts));

  fm = g_strdup_printf(_("maker:\t\t%s\n"
                         "model:\t\t%s\n"
                         "focal range:\t%s\n"
                         "aperture:\t%s\n"
                         "crop factor:\t%.1f\n"
                         "type:\t\t%s\n"
                         "mounts:\t%s"),
                       maker ? maker : "?", model ? model : "?", focal, aperture,
                       lens_v.crop_factor, _lens_type_name((int)lens_v.type), mounts);

  gtk_widget_set_tooltip_text(GTK_WIDGET(g->lens_model), fm);
  dt_free(fm);

  /* Create the focal/aperture/distance combo boxes */
  gtk_container_foreach(GTK_CONTAINER(g->lens_param_box), delete_children, NULL);

  int ffi = 1, fli = -1;
  for(i = 1; i < sizeof(focal_values) / sizeof(gdouble) - 1; i++)
  {
    if(focal_values[i] < min_focal) ffi = i + 1;
    if(focal_values[i] > max_focal && fli == -1) fli = i;
  }
  if(focal_values[ffi] > min_focal)
  {
    focal_values[ffi - 1] = min_focal;
    ffi--;
  }
  if(max_focal == 0 || fli < 0) fli = sizeof(focal_values) / sizeof(gdouble) - 2;
  if(focal_values[fli + 1] < max_focal)
  {
    focal_values[fli + 1] = max_focal;
    ffi++;
  }
  if(fli < ffi) fli = ffi + 1;

  GtkWidget *w;
  char txt[30];

  // focal length
  w = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
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
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[0] = w;

  // f-stop
  ffi = 1, fli = sizeof(aperture_values) / sizeof(gdouble) - 1;
  for(i = 1; i < sizeof(aperture_values) / sizeof(gdouble) - 1; i++)
    if(aperture_values[i] < min_ap) ffi = i + 1;
  if(aperture_values[ffi] > min_ap)
  {
    aperture_values[ffi - 1] = min_ap;
    ffi--;
  }

  w = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
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
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[1] = w;

  w = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
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
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[2] = w;

  gtk_widget_show_all(g->lens_param_box);
}

static void lens_menu_select(GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  lens_set(self, (long long)GPOINTER_TO_INT(
                     g_object_get_data(G_OBJECT(menuitem), "lens-id")));
  if(dt_gui_widgets_suppressed()) return;
  p->modified = 1;
  const float scale = get_autoscale(self, p);
  dt_bauhaus_slider_set(g->scale, scale);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void lens_menu_fill(dt_iop_module_t *self, const long long *ids, int n)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  GPtrArray *makers, *submenus;

  if(g->lens_menu)
  {
    gtk_widget_destroy(GTK_WIDGET(g->lens_menu));
    g->lens_menu = NULL;
  }

  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db)) return;

  /* Count all existing lens makers and create a sorted list */
  makers = g_ptr_array_new_with_free_func(dt_free_gpointer);
  submenus = g_ptr_array_new();
  for(int i = 0; i < n; i++)
  {
    char maker[128] = "", model[256] = "";
    if(ls_db_lens_name(db, ids[i], maker, sizeof(maker), model, sizeof(model)) <= 0) continue;

    GtkWidget *submenu, *item;
    int idx = ptr_array_find_sorted(makers, maker, (GCompareFunc)g_utf8_collate);
    if(idx < 0)
    {
      /* No such maker yet, insert it into the array. Owned strings: these no longer point
       * into a database that outlives the menu. */
      idx = ptr_array_insert_sorted(makers, g_strdup(maker), (GCompareFunc)g_utf8_collate);
      /* Create a submenu for lenses by this maker */
      submenu = gtk_menu_new();
      ptr_array_insert_index(submenus, submenu, idx);
    }

    submenu = (GtkWidget *)g_ptr_array_index(submenus, idx);
    /* Append current lens name to the submenu */
    item = gtk_menu_item_new_with_label(model);
    gtk_widget_show(item);
    g_object_set_data(G_OBJECT(item), "lens-id", GINT_TO_POINTER((gint)ids[i]));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(lens_menu_select), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
  }

  g->lens_menu = GTK_MENU(gtk_menu_new());
  for(unsigned i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label((const gchar *)g_ptr_array_index(makers, i));
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->lens_menu), item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), (GtkWidget *)g_ptr_array_index(submenus, i));
  }

  g_ptr_array_free(submenus, TRUE);
  g_ptr_array_free(makers, TRUE);
}

/**
 * @brief The lenses to offer for the camera currently shown, as ids.
 *
 * @param model when non-NULL and non-empty, only lenses whose name matches it -- the fuzzy
 * matcher, so an abbreviated EXIF string finds the full name.
 * @param out_n how many were written. Free the result with dt_free_align().
 *
 * @details Replaces FindLenses(camera, NULL, model, LF_SEARCH_SORT_AND_UNIQUIFY). The
 * SORT half is done by lens_menu_fill(), which groups by maker and inserts sorted; the
 * UNIQUIFY half is not needed, because these are database ids and a row cannot repeat.
 */
static long long *_lens_ids_for_camera(dt_iop_module_t *self, const char *model, int *out_n)
{
  *out_n = 0;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db)) return NULL;

  ls_camera_t cam;
  const gboolean have_cam = (g->camera_id >= 0) && (ls_db_camera_by_id(db, g->camera_id, &cam) == 1);

  if(model && model[0])
  {
    enum { MAX_HITS = 32 };
    ls_db_match_t m[MAX_HITS];
    const int n = ls_db_match_lens(db, NULL, model, have_cam ? cam.mount_id : 0,
                                   have_cam ? cam.crop_factor : 0.f, m, MAX_HITS);
    if(n <= 0) return NULL;
    long long *ids = (long long *)dt_alloc_align(sizeof(long long) * (size_t)n);
    if(IS_NULL_PTR(ids)) return NULL;
    for(int i = 0; i < n; i++) ids[i] = m[i].lens_id;
    *out_n = n;
    return ids;
  }

  /* Everything that fits the camera's mount, or the whole catalogue when no camera is
   * selected -- which is what upstream answered for a NULL camera too. */
  const int total = ls_db_list_lenses(db, NULL, 0);
  if(total <= 0) return NULL;
  long long *all = (long long *)dt_alloc_align(sizeof(long long) * (size_t)total);
  if(IS_NULL_PTR(all)) return NULL;
  const int got = ls_db_list_lenses(db, all, total);

  if(!have_cam)
  {
    *out_n = got;
    return all;
  }

  int keep = 0;
  for(int i = 0; i < got; i++)
    if(ls_db_lens_fits_mount(db, all[i], cam.mount_id) == 1) all[keep++] = all[i];
  *out_n = keep;
  return all;
}

static void lens_menusearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  (void)button;

  int n = 0;
  long long *ids = _lens_ids_for_camera(self, NULL, &n);
  if(IS_NULL_PTR(ids)) return;
  lens_menu_fill(self, ids, n);
  dt_free_align(ids);

  dt_gui_menu_popup(GTK_MENU(g->lens_menu), button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}

static void lens_autosearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  char model[200];
  const gchar *txt = ((dt_iop_lensfun_params_t *)self->default_params)->lens;

  (void)button;

  parse_model(txt, model, sizeof(model));
  int n = 0;
  long long *ids = _lens_ids_for_camera(self, model[0] ? model : NULL, &n);
  if(IS_NULL_PTR(ids)) return;
  lens_menu_fill(self, ids, n);
  dt_free_align(ids);

  dt_gui_menu_popup(GTK_MENU(g->lens_menu), button, GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);
}

/* -- end lens -- */

static void target_geometry_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  int pos = dt_bauhaus_combobox_get(widget);
  p->target_geom = (dt_lens_type_t)(pos + DT_LENS_UNKNOWN + 1);
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void modflags_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  int pos = dt_bauhaus_combobox_get(widget);
  for(GList *modifiers = g->modifiers;  modifiers; modifiers = g_list_next(modifiers))
  {
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->pos == pos)
    {
      p->modify_flags = (p->modify_flags & ~LENSFUN_MODFLAG_MASK) | mm->modflag;
      p->modified = 1;
      dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
      break;
    }
  }
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  gtk_widget_set_visible(g->tca_override, !raw_monochrome);
  // update gui to show/hide tca sliders if tca_override was changed
  if(IS_NULL_PTR(w) || w == g->tca_override)
  {
    // show tca sliders only iff tca_overwrite is set
    gtk_widget_set_visible(g->tca_r, p->tca_override && !raw_monochrome);
    gtk_widget_set_visible(g->tca_b, p->tca_override && !raw_monochrome);
  }

  if(w)
  {
    // user did modify something with some widget
    p->modified = 1;
  }
}


static float get_autoscale(dt_iop_module_t *self, dt_iop_lensfun_params_t *p)
{
  float scale = 1.0f;
  if(p->lens[0] == '\0') return scale;

  ls_camera_t cam;
  const gboolean have_cam = p->camera[0] && _ls_find_camera(NULL, p->camera, &cam);
  const long long lens_id = _ls_find_lens(have_cam ? cam.mount_id : 0,
                                          have_cam ? cam.crop_factor : 0.f, p->lens);
  ls_db_t *db = _ls_db();
  if(lens_id < 0 || IS_NULL_PTR(db)) return scale;

  /* A throwaway correction state, resolved at scale 1 so the search measures the
   * correction itself rather than a scaling already applied to it. */
  dt_iop_lensfun_data_t d;
  memset(&d, 0, sizeof(d));
  if(ls_db_lens_by_id(db, lens_id, &d.ls_lens) != 1) return scale;
  d.ls_have = TRUE;
  d.modify_flags = p->modify_flags;
  d.inverse = p->inverse;
  d.scale = 1.0f;
  d.crop = p->crop;
  d.focal = p->focal;
  d.aperture = p->aperture;
  d.distance = p->distance;
  d.target_geom = p->target_geom;

  const dt_image_t *img = &(self->dev->image_storage);
  // FIXME: get those from rawprepare IOP somehow !!!
  const int iwd = img->width - img->crop_x - img->crop_width,
            iht = img->height - img->crop_y - img->crop_height;

  ls_modifier_t modifier;
  if(get_modifier(NULL, iwd, iht, &d, DT_LENS_MODIFY_ALL, FALSE, &modifier))
    scale = ls_modifier_autoscale(&modifier);
  return scale;
}

static void autoscale_pressed(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const float scale = get_autoscale(self, p);
  p->modified = 1;
  dt_bauhaus_slider_set(g->scale, scale);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

/**
 * @brief Which corrections this configuration will actually apply.
 *
 * @return the DT_LENS_MODIFY_* axes that resolve, masked to what the GUI reports.
 *
 * @details GUI THREAD ONLY, and it needs no pipeline: which corrections a lens can serve is
 * a pure function of the lens, the shooting configuration and the user's own switches. The
 * database answers it in ~0.2 ms.
 *
 * This used to be discovered by RENDERING. process() and process_cl() wrote the resolved
 * flags into gui_data from the pipeline thread under a critical section, and a
 * preview-pipe-finished signal then woke the GUI to read them back. That is a data race
 * whatever the lock does -- gui_data belongs to the GUI thread, which may free it while a
 * worker is mid-write -- and it made a label depend on a frame having been drawn. Neither
 * was necessary: nothing here needs a pixel.
 */
static int _lens_corrections_available(dt_iop_module_t *self,
                                       const dt_iop_lensfun_params_t *const p)
{
  if(IS_NULL_PTR(self->dev)) return 0;

  dt_iop_lensfun_data_t d;
  memset(&d, 0, sizeof(d));
  _lens_build_data(self, p, &d);
  if(!d.ls_have || d.crop <= 0.f) return 0;

  /* The frame the correction is expressed over. Only its aspect matters to which axes
   * resolve, so the full image is a fine stand-in when the pipe has not published one. */
  dt_iop_roi_t roi = { 0, 0, 0, 0, 1.f };
  if(!dt_dev_module_geometry_gui(self->dev, self, &roi, NULL) || roi.width <= 0
     || roi.height <= 0)
  {
    const dt_image_t *img = &self->dev->image_storage;
    roi.width = img->width;
    roi.height = img->height;
  }
  if(roi.width <= 0 || roi.height <= 0) return 0;

  const gboolean mono = dt_image_is_monochrome(&self->dev->image_storage);
  const int mask = mono ? (DT_LENS_MODIFY_ALL & ~DT_LENS_MODIFY_TCA) : DT_LENS_MODIFY_ALL;
  int modflags = 0;
  ls_modifier_t m;
  get_modifier(&modflags, roi.width, roi.height, &d, mask, FALSE, &m);
  return modflags & LENSFUN_MODFLAG_MASK;
}

static void corrections_done(gpointer instance, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  if(dt_gui_widgets_suppressed()) return;

  const int corrections_done
      = _lens_corrections_available(self, (const dt_iop_lensfun_params_t *)self->params);
  g->corrections_done = corrections_done;

  const char empty_message[] = "";
  char *message = (char *)empty_message;
  for(GList *modifiers = g->modifiers; modifiers && self->enabled; modifiers = g_list_next(modifiers))
  {
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->modflag == corrections_done)
    {
      message = mm->name;
      break;
    }
  }

  dt_gui_freeze_begin();
  gtk_label_set_text(g->message, message);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->message), message);
  dt_gui_freeze_end();
}

void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = IOP_GUI_ALLOC(lensfun);

  g->camera_id = -1;
  g->camera_menu = NULL;
  g->lens_menu = NULL;
  g->modifiers = NULL;

  g->corrections_done = -1;

  // initialize modflags options
  int pos = -1;
  dt_iop_lensfun_modifier_t *modifier;
  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("none"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_NONE;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("all"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_ALL;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("distortion & TCA"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST_TCA;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("distortion & vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST_VIGN;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("TCA & vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_TCA_VIGN;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only distortion"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only TCA"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_TCA;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_VIGN;
  modifier->pos = ++pos;

  self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
    gtk_widget_set_name(self->gui->widget, "lens-module");

  // camera selector
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  g->camera_model = dt_iop_button_new(self, N_("camera model"),
                                      G_CALLBACK(camera_menusearch_clicked), FALSE, 0, (GdkModifierType)0,
                                      NULL, 0, hbox);
  g->find_camera_button = dt_iop_button_new(self, N_("find camera"),
                                            G_CALLBACK(camera_autosearch_clicked), FALSE, 0, (GdkModifierType)0,
                                            dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
  dt_gui_add_class(g->find_camera_button, "dt_big_btn_canvas");
  gtk_box_pack_start(GTK_BOX(hbox), g->find_camera_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), hbox, TRUE, TRUE, 0);

  // lens selector
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  g->lens_model = dt_iop_button_new(self, N_("lens model"),
                                    G_CALLBACK(lens_menusearch_clicked), FALSE, 0, (GdkModifierType)0,
                                    NULL, 0, hbox);
  g->find_lens_button = dt_iop_button_new(self, N_("find lens"),
                                          G_CALLBACK(lens_autosearch_clicked), FALSE, 0, (GdkModifierType)0,
                                          dtgtk_cairo_paint_solid_arrow, CPF_DIRECTION_DOWN, NULL);
  dt_gui_add_class(g->find_lens_button, "dt_big_btn_canvas");
  gtk_box_pack_start(GTK_BOX(hbox), g->find_lens_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), hbox, TRUE, TRUE, 0);

  // lens properties
  g->lens_param_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), g->lens_param_box, TRUE, TRUE, 0);


  // selector for correction type (modflags): one or more out of distortion, TCA, vignetting
  g->modflags = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->modflags, N_("corrections"));
  gtk_box_pack_start(GTK_BOX(self->gui->widget), g->modflags, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->modflags, _("which corrections to apply"));
  GList *l = g->modifiers;
  while(l)
  {
    modifier = (dt_iop_lensfun_modifier_t *)l->data;
    dt_bauhaus_combobox_add(g->modflags, modifier->name);
    l = g_list_next(l);
  }
  dt_bauhaus_combobox_set(g->modflags, 0);
  g_signal_connect(G_OBJECT(g->modflags), "value-changed", G_CALLBACK(modflags_changed), (gpointer)self);

  // target geometry
  g->target_geom = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->target_geom, N_("geometry"));
  gtk_box_pack_start(GTK_BOX(self->gui->widget), g->target_geom, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->target_geom, _("target geometry"));
  dt_bauhaus_combobox_add(g->target_geom, _("rectilinear"));
  dt_bauhaus_combobox_add(g->target_geom, _("fish-eye"));
  dt_bauhaus_combobox_add(g->target_geom, _("panoramic"));
  dt_bauhaus_combobox_add(g->target_geom, _("equirectangular"));
  dt_bauhaus_combobox_add(g->target_geom, _("orthographic"));
  dt_bauhaus_combobox_add(g->target_geom, _("stereographic"));
  dt_bauhaus_combobox_add(g->target_geom, _("equisolid angle"));
  dt_bauhaus_combobox_add(g->target_geom, _("thoby fish-eye"));
  g_signal_connect(G_OBJECT(g->target_geom), "value-changed", G_CALLBACK(target_geometry_changed),
                   (gpointer)self);

  // scale
  g->scale = dt_bauhaus_slider_from_params(self, N_("scale"));
  dt_bauhaus_slider_set_digits(g->scale, 3);
  dt_bauhaus_widget_set_quad_paint(g->scale, dtgtk_cairo_paint_refresh, 0, NULL);
  g_signal_connect(G_OBJECT(g->scale), "quad-pressed", G_CALLBACK(autoscale_pressed), self);
  gtk_widget_set_tooltip_text(g->scale, _("auto scale"));

  // reverse direction
  g->reverse = dt_bauhaus_combobox_from_params(self, "inverse");
  dt_bauhaus_combobox_add(g->reverse, _("correct"));
  dt_bauhaus_combobox_add(g->reverse, _("distort"));
  gtk_widget_set_tooltip_text(g->reverse, _("correct distortions or apply them"));

  g->tca_override = dt_bauhaus_toggle_from_params(self, "tca_override");

  // override linear tca (if not 1.0):
  g->tca_r = dt_bauhaus_slider_from_params(self, "tca_r");
  dt_bauhaus_slider_set_digits(g->tca_r, 5);
  gtk_widget_set_tooltip_text(g->tca_r, _("Transversal Chromatic Aberration red"));

  g->tca_b = dt_bauhaus_slider_from_params(self, "tca_b");
  dt_bauhaus_slider_set_digits(g->tca_b, 5);
  gtk_widget_set_tooltip_text(g->tca_b, _("Transversal Chromatic Aberration blue"));

  // message box to inform user what corrections have been done. this is useful as depending on lensfuns
  // profile only some of the lens flaws can be corrected
  GtkBox *hbox1 = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));
  GtkWidget *label = gtk_label_new(_("corrections done: "));
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_tooltip_text(label, _("which corrections have actually been done"));
  gtk_box_pack_start(GTK_BOX(hbox1), label, FALSE, FALSE, 0);
  g->message = GTK_LABEL(gtk_label_new("")); // This gets filled in by process
  gtk_label_set_ellipsize(GTK_LABEL(g->message), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->message), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), GTK_WIDGET(hbox1), TRUE, TRUE, 0);

  /* add signal handler for preview pipe finish to update message on corrections done */
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(corrections_done), self);
}

void gui_update(struct dt_iop_module_t *self)
{
  // let gui elements reflect params
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  if(p->modified == 0)
  {
    /*
     * user did not modify anything in gui after autodetection - let's
     * use current default_params as params - for presets and mass-export
     */
    memcpy(self->params, self->default_params, sizeof(dt_iop_lensfun_params_t));
  }

  // these are the wrong (untranslated) strings in general but that's ok, they will be overwritten further
  // down
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), p->camera);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), p->lens);
  gtk_widget_set_tooltip_text(g->camera_model, "");
  gtk_widget_set_tooltip_text(g->lens_model, "");

  int modflag = p->modify_flags & LENSFUN_MODFLAG_MASK;
  for(GList *modifiers = g->modifiers; modifiers; modifiers = g_list_next(modifiers))
  {
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->modflag == modflag)
    {
      dt_bauhaus_combobox_set(g->modflags, mm->pos);
      break;
    }
  }

  dt_bauhaus_combobox_set(g->target_geom, p->target_geom - DT_LENS_UNKNOWN - 1);
  dt_bauhaus_combobox_set(g->reverse, p->inverse);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->tca_override), p->tca_override);
  g->camera_id = -1;
  if(p->camera[0])
  {
    /* Resolved the same way the pipeline resolves it. Recovering the id by comparing the
     * params string against stored model names cannot work: matching is on the normalised
     * form, so "NIKON D5300" finds a row whose model column reads "D5300" -- which is why
     * this label was blank on every image while the correction itself was applied. */
    ls_camera_t cam;
    camera_set(self, _ls_find_camera(NULL, p->camera, &cam) ? cam.id : -1);
  }
  if(g->camera_id >= 0 && p->lens[0])
  {
    char model[200];
    parse_model(p->lens, model, sizeof(model));
    int n = 0;
    long long *ids = _lens_ids_for_camera(self, model[0] ? model : NULL, &n);
    lens_set(self, (n > 0 && !IS_NULL_PTR(ids)) ? ids[0] : -1);
    if(!IS_NULL_PTR(ids)) dt_free_align(ids);
  }
  else
  {
    lens_set(self, -1);
  }

  // Default to blank: safe fallback if the piece isn't ready yet (e.g. very first call for this
  // image, before any pipe sync happened).
  dt_iop_gui_enter_critical_section(self);
  g->corrections_done = -1;
  dt_iop_gui_leave_critical_section(self);
  gtk_label_set_text(g->message, "");

  /* Which corrections are available depends on the camera, the lens and the params, and on
   * nothing else -- not on process() having run, and not on the geometry chain having been
   * published. Both of those were consulted here, and both could be absent at exactly the
   * moment the label is first shown, leaving it blank until some unrelated event forced
   * another pipe run. */
  g->corrections_done = _lens_corrections_available(self, p);

  for(GList *modifiers = g->modifiers; !IS_NULL_PTR(modifiers); modifiers = g_list_next(modifiers))
  {
    const dt_iop_lensfun_modifier_t *mm = (const dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->modflag == g->corrections_done)
    {
      gtk_label_set_text(g->message, mm->name);
      gtk_widget_set_tooltip_text(GTK_WIDGET(g->message), mm->name);
      break;
    }
  }

  gui_changed(self, NULL, NULL);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(corrections_done), self);

  while(g->modifiers)
  {
    dt_free(g->modifiers->data);
    g->modifiers = g_list_delete_link(g->modifiers, g->modifiers);
  }

  IOP_GUI_FREE;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
