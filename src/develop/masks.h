/*
    This file is part of darktable,
    Copyright (C) 2013-2014, 2016, 2019, 2021 Aldric Renaudin.
    Copyright (C) 2013, 2018, 2020-2021 Pascal Obry.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2014, 2016-2018 Tobias Ellinghaus.
    Copyright (C) 2013-2017, 2019-2020 Ulrich Pegelow.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2021 Hanno Schwalm.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Philipp Lutz.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2025 Alynx Zhou.
    Copyright (C) 2025-2026 Guillaume Stutin.
    
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


/*
Typical forms tree structure :

GList dev->forms
  |
  0) dt_masks_form_t  circle --------------------> ID 1771813676,  "circle #2"
  |    { GList *points
  |        | dt_masks_form_t  --------------------> dt_masks_type_t type; const dt_masks_functions_t *functions;
  |            |                                     float source[2]; char name[128]; int formid; int version;
  |              { GList *points;
  |                  | dt_masks_node_circle_t ----> float center[2]; float radius; float border;
  |
  |
  1) dt_masks_form_t  group ---------------------> ID 1771813678,  "grp retouch"
  |    { GList *points
  |        |-> dt_masks_form_group_t :   ID 1771813676,  parentid: 1771813678,    state: use show union 
  |        |-> dt_masks_form_group_t :   ID 1771942330,  parentid: 1771813678,    state: use show union 
  |
  |
  2) dt_masks_form_t  polygon -------------------> ID 1771815454,  "polygon #1"
  |    { GList *points
  |        | dt_masks_form_t   -------------------> dt_masks_type_t type; const dt_masks_functions_t *functions;
  |              |                                   float source[2]; char name[128]; int formid; int version;
  |              { GList *points;
  |                  | dt_masks_node_polygon_t ---> float node[2]; float ctrl1[2]; float ctrl2[2]; float border[2];
  |                  | dt_masks_node_polygon_t ---> ...
  |                  | ...
  |                  | ...
  |
  |
  3) dt_masks_form_t  group ---------------------> ID 1771942331,  "grp exposure"
  |    { GList *points
  |        |-> dt_masks_form_group_t :  ID 1771815454,   parentid: 1771942331,    state: use show union 
  |        |-> dt_masks_form_group_t :  ID 1771877226,   parentid: 1771942331,    state: use show union 
  |        |-> dt_masks_form_group_t :  ID 1771877232,   parentid: 1771942331,    state: use show union 
  |
  |
  4) dt_masks_form_t  ellipse -------------------> ID 1771877226,  "ellipse #1"
  |    { GList *points
  |        | dt_masks_form_t  --------------------> dt_masks_type_t type; const dt_masks_functions_t *functions;
  |              |                                   float source[2]; char name[128]; int formid; int version;
  |              { GList *points;
  |                  | dt_masks_node_ellipse_t ---> float center[2]; float radius[2]; float rotation;
  |                                                  float border; dt_masks_ellipse_flags_t flags;
  |
  |
  5) dt_masks_form_t  brush ---------------------> ID 1771877232,  "brush #1"
  |    { GList *points
  |        | dt_masks_form_t  --------------------> dt_masks_type_t type; const dt_masks_functions_t *functions;
  |              |                                   float source[2]; char name[128]; int formid; int version;
  |              { GList *points;
  |                  | dt_masks_node_brush_t ----->  float node[2]; float pressure; float fading; float size;
  |                  |                                dt_masks_pressure_sensitivity_t pressure_sensitivity;
  |                  | dt_masks_node_brush_t -----> ...
  |                  |
  |                  | ...
  |                  | ...
  |
  |
  6) dt_masks_form_t  gradient -------------------> ID 1771942330,  "gradient #1"
  |    { GList *points
  |        | dt_masks_form_t  ---------------------> dt_masks_type_t type; const dt_masks_functions_t *functions;
  |              |                                    float source[2]; char name[128]; int formid; int version;
  |              { GList *points;
  |                  | dt_masks_anchor_gradient_t -> float center[2]; float rotation; float extent; float steepness; float curvature;
  |
  7)...
  |
  ...


*/

#ifndef DT_DEVELOP_MASKS_H
#define DT_DEVELOP_MASKS_H

#include "develop/masks_types.h"   // the vocabulary: shape kinds, states, and the value types
#include "system/atomic.h"
#include "common/logging.h"
#include "system/macros.h"
#include "system/simd.h"
#include "common/times.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "develop/develop.h"     // dt_develop_t, and dt_iop_module_t through imageop.h
#include "develop/pixelpipe.h"

/* The per-shape function table is an implementation detail: every one of its members is
 * dispatched from inside src/develop/masks/ and nowhere else (measured), so its definition
 * lives in develop/masks/masks_functions.h and only this forward declaration is public.
 * That is what lets this header drop widgets/draw.h -- the table was the only thing here
 * naming a drawing type. The interactive-editing state (dt_masks_form_gui_t) and the whole
 * event/drawing/menu API live in develop/masks_gui.h. */
typedef struct dt_masks_functions_t dt_masks_functions_t;

#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVELOP_MASKS_VERSION (6)



/**masts states */

typedef enum dt_masks_event_t
{
  DT_MASKS_EVENT_NONE   = 0,
  DT_MASKS_EVENT_ADD    = 1,
  DT_MASKS_EVENT_REMOVE = 2,
  DT_MASKS_EVENT_UPDATE = 3,
  DT_MASKS_EVENT_DELETE = 4,
  DT_MASKS_EVENT_CHANGE = 5,
  DT_MASKS_EVENT_RESET  = 6
} dt_masks_event_t;


typedef enum dt_masks_points_states_t
{
  DT_MASKS_POINT_STATE_NORMAL = 1,
  DT_MASKS_POINT_STATE_USER = 2
} dt_masks_points_states_t;

typedef enum dt_masks_gradient_states_t
{
  DT_MASKS_GRADIENT_STATE_LINEAR = 1,
  DT_MASKS_GRADIENT_STATE_SIGMOIDAL = 2
} dt_masks_gradient_states_t;





typedef enum dt_masks_pressure_sensitivity_t
{
  DT_MASKS_PRESSURE_OFF = 0,
  DT_MASKS_PRESSURE_FADING_REL = 1,
  DT_MASKS_PRESSURE_FADING_ABS = 2,
  DT_MASKS_PRESSURE_OPACITY_REL = 3,
  DT_MASKS_PRESSURE_OPACITY_ABS = 4,
  DT_MASKS_PRESSURE_BRUSHSIZE_REL = 5
} dt_masks_pressure_sensitivity_t;

typedef enum dt_masks_ellipse_flags_t
{
  DT_MASKS_ELLIPSE_EQUIDISTANT = 0,
  DT_MASKS_ELLIPSE_PROPORTIONAL = 1
} dt_masks_ellipse_flags_t;

typedef enum dt_masks_source_pos_type_t
{
  DT_MASKS_SOURCE_POS_RELATIVE = 0,
  DT_MASKS_SOURCE_POS_RELATIVE_TEMP = 1,
  DT_MASKS_SOURCE_POS_ABSOLUTE = 2
} dt_masks_source_pos_type_t;

/** structure used to store 1 node for a circle */
typedef struct dt_masks_node_circle_t
{
  float center[2]; // point in normalized input space
  float radius;
  float border;
} dt_masks_node_circle_t;

/** structure used to store 1 node for an ellipse */
typedef struct dt_masks_node_ellipse_t
{
  float center[2];
  float radius[2];
  float rotation;
  float border;
  dt_masks_ellipse_flags_t flags;
} dt_masks_node_ellipse_t;

/** structure used to store 1 node for a path form */
typedef struct dt_masks_node_polygon_t
{
  float node[2];
  float ctrl1[2];
  float ctrl2[2];
  float border[2];
  dt_masks_points_states_t state;
} dt_masks_node_polygon_t;

/** structure used to store 1 node for a brush form */
typedef struct dt_masks_node_brush_t
{
  float node[2];
  float ctrl1[2];
  float ctrl2[2];
  float border[2];
  float density;
  float fading;
  dt_masks_points_states_t state;
} dt_masks_node_brush_t;

/** structure used to store anchor for a gradient */
typedef struct dt_masks_anchor_gradient_t
{
  float center[2];
  float rotation;
  float extent;
  float steepness;
  float curvature;
  dt_masks_gradient_states_t state;
} dt_masks_anchor_gradient_t;







/** structure used to define a form */
typedef struct dt_masks_form_t
{
  GList *points; // list of point structures (nodes)
  dt_masks_type_t type;
  const dt_masks_functions_t *functions;
  // TRUE when gui_points->points uses the Bezier layout (points[k*6+2])
  gboolean uses_bezier_points_layout;

  // position of the origin point of source (used only for clone)
  // in normalized coordinates in raw input space
  float source[2];

  // cached center of gravity
  // in normalized coordinates in raw input space
  float gravity_center[2];
  // FALSE means gravity_center/area are stale and must be recomputed before being read
  // (see dt_masks_form_update_gravity_center() / dt_masks_form_invalidate_gravity_center()).
  // Lets bulk paths (loading history, undo/redo) defer the actual computation to the one
  // GUI hit-testing read site instead of paying it for every form up front.
  gboolean gravity_center_valid;

  // cached shape area, taken as a weight estimator to get
  // the gravity center of multi-shapes by combining
  // weight and gravity centers of all shapes
  float area;
  // name of the form
  char name[128];
  // id used to store the form
  int formid;
  // version of the form
  int version;

  // number of live pointers to this form: dev->forms/allforms, hist->forms
  // snapshots, undo/redo snapshots. See develop/masks/masks_history.h.
  dt_atomic_int refcount;
} dt_masks_form_t;

// dt_masks_form_t must be fully defined above this include.
#include "develop/masks/masks_history.h"

/* Rasterisation entry points: dispatch through the private table. */
/** get points in real space with respect of distortion dx and dy are used to eventually move the center of
 * the circle */
/**
 * @brief What a rasterisation attempt produced.
 *
 * @details The rasterisation family used to return a bare `int` on a "0 means success"
 * convention that could not say the third thing that actually happens: a shape with nothing to
 * draw. Callers therefore had to guess, and the shapes disagreed -- a NULL points list made a
 * circle report failure (which aborted the whole group fold) and an ellipse report success
 * (which rendered it as zeros). Same data, opposite outcomes, decided by which shape carried it.
 *
 * OK    -- the buffer was written; `touched`, where the callee takes one, describes what.
 * EMPTY -- nothing to draw: degenerate geometry, or the shape lies entirely outside this ROI.
 *          A legitimate outcome, NOT an error. The buffer is left as the caller supplied it.
 * ERROR -- the shape could not be computed (allocation failure, a distortion transform that
 *          did not converge). The buffer's contents are undefined and must not be published.
 *
 * OK is 0 so that a caller written against the old convention still reads a success as success;
 * every other value is a non-success, which is the safe direction for anything not yet migrated.
 */
typedef enum dt_masks_raster_result_t
{
  DT_MASKS_RASTER_OK = 0,
  DT_MASKS_RASTER_EMPTY,
  DT_MASKS_RASTER_ERROR
} dt_masks_raster_result_t;

/** Adapt an internal helper still on the plain "0 means success" convention. Such a helper has
 * no way to report EMPTY, so everything non-zero becomes ERROR -- which is the conservative
 * reading, and the one those helpers' callers already applied. */
static inline dt_masks_raster_result_t dt_masks_raster_from_status(const int status)
{
  return (status == 0) ? DT_MASKS_RASTER_OK : DT_MASKS_RASTER_ERROR;
}


/** get the rectangle which include the form and his border */
dt_masks_raster_result_t dt_masks_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                      dt_dev_pixelpipe_iop_t *piece,
                      dt_masks_form_t *form,
                      int *width, int *height, int *posx, int *posy);
dt_masks_raster_result_t dt_masks_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                             dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                             int *width, int *height, int *posx, int *posy);



/** structure for dynamic buffers */
typedef struct dt_masks_dynbuf_t
{
  float *buffer;
  char tag[128];
  size_t pos;
  size_t size;
} dt_masks_dynbuf_t;



// Clone and spot forms share the same default presets, while regular drawn masks use their own.
static inline gboolean dt_masks_form_uses_spot_defaults(const dt_masks_form_t *form)
{
  return (form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) != 0;
}

static inline gboolean dt_masks_form_is_clone(const dt_masks_form_t *form)
{
  return (form->type & DT_MASKS_CLONE) != 0;
}

static inline void dt_masks_reset_source(dt_masks_form_t *form)
{
  form->source[0] = 0.0f;
  form->source[1] = 0.0f;
}

static inline void dt_masks_translate_source(dt_masks_form_t *form, const float delta_x, const float delta_y)
{
  form->source[0] += delta_x;
  form->source[1] += delta_y;
}

static inline void dt_masks_translate_ctrl_node(float node[2], float ctrl1[2], float ctrl2[2],
                                                const float delta_x, const float delta_y)
{
  node[0] += delta_x;
  node[1] += delta_y;
  ctrl1[0] += delta_x;
  ctrl1[1] += delta_y;
  ctrl2[0] += delta_x;
  ctrl2[1] += delta_y;
}

static inline void dt_masks_set_ctrl_points(float ctrl1[2], float ctrl2[2], const float control_points[4])
{
  ctrl1[0] = control_points[0];
  ctrl1[1] = control_points[1];
  ctrl2[0] = control_points[2];
  ctrl2[1] = control_points[3];
}


/** get the transparency mask of the form and his border */
dt_masks_raster_result_t dt_masks_get_mask(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                      const dt_dev_pixelpipe_iop_t *const piece,
                      dt_masks_form_t *const form,
                      float **buffer, int *width, int *height, int *posx, int *posy);

/** Rasterise `form` into the pre-zeroed ROI-sized `buffer`. `touched` (may be NULL) receives
 * the buffer-relative rectangle enclosing every pixel written; empty when nothing was. */



dt_masks_raster_result_t dt_masks_group_render_roi(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe,
                                                   const dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                                                   const dt_iop_roi_t *roi, float *buffer);

// returns current masks version
int dt_masks_version(void);



// update masks from older versions
int dt_masks_legacy_params(dt_develop_t *dev, void *params, const int old_version, const int new_version);
/*
 * TODO:
 *
 * int
 * dt_masks_legacy_params(
 *   dt_develop_t *dev,
 *   const void *const old_params, const int old_version,
 *   void *new_params,             const int new_version);
 */

/** we create a completely new form. */
dt_masks_form_t *dt_masks_create(dt_masks_type_t type);
/** we create a completely new form and add it to dev->allforms. */
dt_masks_form_t *dt_masks_create_ext(dt_develop_t *dev, dt_masks_type_t type);
/** returns a form with formid == id from a list of forms */
dt_masks_form_t *dt_masks_get_from_id_ext(GList *forms, int id);
/** returns a form with formid == id from dev->forms */
dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *dev, int id);
/** copy forms used by a module from dev_src to dev_dest */
int dt_masks_copy_used_forms_for_module(dt_develop_t *dev_dest, dt_develop_t *dev_src,
                                        const struct dt_iop_module_t *mod_src);
/** return the mask manager module instance if present */
struct dt_iop_module_t *dt_masks_get_mask_manager(struct dt_develop_t *dev);

/** read the forms from the db */
void dt_masks_read_masks_history(dt_develop_t *dev, const int32_t imgid);
/** write the forms into the db */
void dt_masks_write_masks_history_item(const int32_t imgid, const int num, dt_masks_form_t *form);
void dt_masks_free_form(dt_masks_form_t *form);
void dt_masks_cleanup_unused(dt_develop_t *dev);


































dt_masks_edit_mode_t dt_masks_get_edit_mode(struct dt_iop_module_t *module);
void dt_masks_set_edit_mode(struct dt_iop_module_t *module, dt_masks_edit_mode_t value);

void dt_masks_iop_use_same_as(struct dt_iop_module_t *module, struct dt_iop_module_t *src);
/** Hash a group's full content (recursing into members). Children are resolved from the given
 * forms list — pass the same list the group came from (live dev->forms or a snapshot), so the
 * hash describes one coherent state instead of mixing the group with foreign children. */
uint64_t dt_masks_group_get_hash_ext(uint64_t hash, GList *masks, dt_masks_form_t *form);
/** Same as dt_masks_group_get_hash_ext(), but hashes only the form's own content: for a group,
 * member references (id/state/opacity) instead of recursing into each member's content.
 * Meant for walking a flat list where every member already gets its own top-level call. */
uint64_t dt_masks_form_get_own_hash(uint64_t hash, GList *masks, const dt_masks_form_t *form);

void dt_masks_form_delete(dt_develop_t *dev, struct dt_iop_module_t *module, dt_masks_form_t *grp, dt_masks_form_t *form);
int dt_masks_form_change_opacity(dt_develop_t *dev, dt_masks_form_t *form, int parentid, int up, const int flow);
void dt_masks_form_move(dt_masks_form_t *grp, int formid, int up);
int dt_masks_form_duplicate(dt_develop_t *dev, int formid);
/**
 * @brief Duplicate a shape (dt_masks_form_duplicate) and, if `group_id` names a valid
 * group, attach the duplicate as a new member of that group right away, inheriting the
 * source form's group-entry state (operation) and opacity. If `group_id` is not a group
 * (e.g. <= 0), the duplicate is left unattached in dev->forms, same as
 * dt_masks_form_duplicate alone. Shared by every "Duplicate shape" UI entry point so the
 * attach-and-inherit behavior does not get re-implemented per caller.
 * @return the new form's id, or <= 0 on failure.
 */
int dt_masks_form_duplicate_in_group(dt_develop_t *dev, int group_id, int form_id);
/* returns a duplicate tof form, including the formid */
dt_masks_form_t *dt_masks_dup_masks_form(const dt_masks_form_t *form);

















/**
 * @brief Duplicate a points list for a mask using a fixed node size.
 *
 * The destination list is appended to, mirroring the previous per-mask implementations.
 */
void dt_masks_duplicate_points(const dt_masks_form_t *base, dt_masks_form_t *dest, size_t node_size);








/** code for dynamic handling of intermediate buffers */
static inline gboolean _dt_masks_dynbuf_growto(dt_masks_dynbuf_t *a, size_t size)
{
  const size_t newsize = dt_round_size_sse(sizeof(float) * size) / sizeof(float);
  float *newbuf = dt_pixelpipe_cache_alloc_align_float_cache(newsize, 0);
  if (IS_NULL_PTR(newbuf))
  {
    // not much we can do here except emit an error message
    fprintf(stderr, "critical: out of memory for dynbuf '%s' with size request %" G_GSIZE_FORMAT "!\n", a->tag, size);
    return FALSE;
  }
  if (a->buffer)
  {
    memcpy(newbuf, a->buffer, a->size * sizeof(float));
    dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] grows to size %lu (is %p, was %p)\n", a->tag,
             (unsigned long)a->size, newbuf, a->buffer);
    dt_pixelpipe_cache_free_align(a->buffer);
  }
  a->size = newsize;
  a->buffer = newbuf;
  return TRUE;
}

static inline dt_masks_dynbuf_t *dt_masks_dynbuf_init(size_t size, const char *tag)
{
  assert(size > 0);
  dt_masks_dynbuf_t *a = (dt_masks_dynbuf_t *)calloc(1, sizeof(dt_masks_dynbuf_t));

  if(!IS_NULL_PTR(a))
  {
    g_strlcpy(a->tag, tag, sizeof(a->tag)); //only for debugging purposes
    a->pos = 0;
    if(_dt_masks_dynbuf_growto(a, size))
      dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] with initial size %lu (is %p)\n", a->tag,
               (unsigned long)a->size, a->buffer);
    if(IS_NULL_PTR(a->buffer))
    {
      dt_free(a);
    }
  }
  return a;
}

static inline void dt_masks_dynbuf_add_2(dt_masks_dynbuf_t *a, float value1, float value2)
{
  assert(!IS_NULL_PTR(a));
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + 2 >= a->size, 0))
  {
    if (a->size == 0 || !_dt_masks_dynbuf_growto(a, 2 * (a->size+1)))
      return;
  }
  a->buffer[a->pos++] = value1;
  a->buffer[a->pos++] = value2;
}

// Return a pointer to N floats past the current end of the dynbuf's contents, marking them as already in use.
// The caller should then fill in the reserved elements using the returned pointer.
static inline float *dt_masks_dynbuf_reserve_n(dt_masks_dynbuf_t *a, const int n)
{
  assert(!IS_NULL_PTR(a));
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + n >= a->size, 0))
  {
    if(a->size == 0) return NULL;
    size_t newsize = a->size;
    while(a->pos + n >= newsize) newsize *= 2;
    if (!_dt_masks_dynbuf_growto(a, newsize))
    {
      return NULL;
    }
  }
  // get the current end of the (possibly reallocated) buffer, then mark the next N items as in-use
  float *reserved = a->buffer + a->pos;
  a->pos += n;
  return reserved;
}

static inline void dt_masks_dynbuf_add_zeros(dt_masks_dynbuf_t *a, const int n)
{
  assert(!IS_NULL_PTR(a));
  assert(a->pos <= a->size);
  if(__builtin_expect(a->pos + n >= a->size, 0))
  {
    if(a->size == 0) return;
    size_t newsize = a->size;
    while(a->pos + n >= newsize) newsize *= 2;
    if (!_dt_masks_dynbuf_growto(a, newsize))
    {
      return;
    }
  }
  // now that we've ensured a sufficiently large buffer add N zeros to the end of the existing data
  memset(a->buffer + a->pos, 0, n * sizeof(float));
  a->pos += n;
}


static inline float dt_masks_dynbuf_get(dt_masks_dynbuf_t *a, int offset)
{
  assert(!IS_NULL_PTR(a));
  // offset: must be negative distance relative to end of buffer
  assert(offset < 0);
  assert((long)a->pos + offset >= 0);
  return (a->buffer[a->pos + offset]);
}

static inline void dt_masks_dynbuf_set(dt_masks_dynbuf_t *a, int offset, float value)
{
  assert(!IS_NULL_PTR(a));
  // offset: must be negative distance relative to end of buffer
  assert(offset < 0);
  assert((long)a->pos + offset >= 0);
  a->buffer[a->pos + offset] = value;
}

static inline float *dt_masks_dynbuf_buffer(dt_masks_dynbuf_t *a)
{
  assert(!IS_NULL_PTR(a));
  return a->buffer;
}

static inline gboolean dt_masks_center_of_gravity_from_points(const float *points, const int points_count,
                                                              float center[2], float *area)
{
  if(IS_NULL_PTR(points) || IS_NULL_PTR(center) || IS_NULL_PTR(area) || points_count <= 0)
  {
    if(center)
    {
      center[0] = 0.0f;
      center[1] = 0.0f;
    }
    if(!IS_NULL_PTR(area)) *area = 0.0f;
    return FALSE;
  }

  double start = 0.;
  if(dt_get_debug_flags() & DT_DEBUG_PERF) start = dt_get_wtime();

  // Points must be ordered sequentially along the polygon boundary.
  // Use the shoelace formula to compute area and centroid.
  if(points_count >= 3)
  {
    double area2 = 0.0;
    double cx = 0.0;
    double cy = 0.0;

    for(int i = 0; i < points_count; i++)
    {
      const int j = (i + 1 < points_count) ? (i + 1) : 0;
      const double x0 = points[i * 2];
      const double y0 = points[i * 2 + 1];
      const double x1 = points[j * 2];
      const double y1 = points[j * 2 + 1];

      const double cross = x0 * y1 - x1 * y0;
      area2 += cross;
      cx += (x0 + x1) * cross;
      cy += (y0 + y1) * cross;
    }

    if(fabs(area2) > 1e-12)
    {
      const double inv = 1.0 / (3.0 * area2);
      center[0] = (float)(cx * inv);
      center[1] = (float)(cy * inv);

      *area = (float)(0.5 * fabs(area2));
      return TRUE;
    }
  }

  // Fallback to arithmetic mean for degenerate polygons or short lists.
  float sum_x = 0.0f;
  float sum_y = 0.0f;
  const float inv_count = 1.0f / (float)points_count;
  for(int i = 0; i < points_count; i++)
  {
    sum_x += points[i * 2] * inv_count;
    sum_y += points[i * 2 + 1] * inv_count;
  }

  if(dt_get_debug_flags() & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks] computing centroid took %0.04f sec\n",
             dt_get_wtime() - start);


  center[0] = sum_x;
  center[1] = sum_y;
  *area = 0.0f;
  return TRUE;
}

static inline size_t dt_masks_dynbuf_position(dt_masks_dynbuf_t *a)
{
  assert(!IS_NULL_PTR(a));
  return a->pos;
}

static inline void dt_masks_dynbuf_reset(dt_masks_dynbuf_t *a)
{
  assert(!IS_NULL_PTR(a));
  a->pos = 0;
}

static inline float *dt_masks_dynbuf_harvest(dt_masks_dynbuf_t *a)
{
  // take out data buffer and make dynamic buffer obsolete
  if(IS_NULL_PTR(a)) return NULL;
  float *r = a->buffer;
  a->buffer = NULL;
  a->pos = a->size = 0;
  return r;
}

static inline void dt_masks_dynbuf_free(dt_masks_dynbuf_t *a)
{
  if(IS_NULL_PTR(a)) return;
  dt_print(DT_DEBUG_MASKS, "[masks dynbuf '%s'] freed (was %p)\n", a->tag,
          a->buffer);
  dt_pixelpipe_cache_free_align(a->buffer);
  dt_free(a);
}

static inline int dt_masks_roundup(int num, int mult)
{
  const int rem = num % mult;

  return (rem == 0) ? num : num + mult - rem;
}























/** Dialogs */





#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
