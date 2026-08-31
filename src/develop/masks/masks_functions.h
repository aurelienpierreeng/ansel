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

/** @file develop/masks/masks_functions.h
 *
 * @brief The per-shape function table, private to the masks implementation.
 *
 * @details Every member is dispatched from inside src/develop/masks/ and nowhere else
 * (measured before the move), so nothing outside the shape files and the masks core has
 * any business seeing the table's layout. Public code holds it only as the opaque
 * `dt_masks_form_t.functions` pointer, forward-declared in develop/masks.h. Keeping the
 * definition here is also what keeps drawing and event types out of the public header:
 * several members take dt_masks_form_gui_t* or a shape_draw_function_t.
 */

#ifndef DT_DEVELOP_MASKS_MASKS_FUNCTIONS_H
#define DT_DEVELOP_MASKS_MASKS_FUNCTIONS_H

#include "develop/masks.h"
#include "develop/masks_gui.h"

#ifdef __cplusplus
extern "C" {
#endif


/** structure used to store pointers to the functions implementing operations on a mask shape */
/** plus a few per-class descriptive data items */
typedef struct dt_masks_functions_t
{
  int point_struct_size;   // sizeof(struct dt_masks_point_*_t)
  void (*sanitize_config)(dt_masks_type_t type_flags);
  void (*set_form_name)(struct dt_masks_form_t *const form, const size_t nb);
  void (*set_hint_message)(const struct dt_masks_form_gui_t *const gui, const struct dt_masks_form_t *const form,
                           char *const __restrict__ msgbuf, const size_t msgbuf_len);
  void (*duplicate_points)(struct dt_develop_t *const dev, struct dt_masks_form_t *base, struct dt_masks_form_t *dest);
  void (*initial_source_pos)(struct dt_develop_t *dev, const float iwd, const float iht, float *x, float *y);
  // input coordinates are in absolute output-image space, dist is squared in the same space
  void (*get_distance)(float x, float y, float as, struct dt_masks_form_gui_t *gui, int index, int num_points,
                       int *inside, int *inside_border, int *near_handle, int *inside_source, float *dist);
  int (*get_points)(struct dt_develop_t *dev, float x, float y, float radius_a, float radius_b, float rotation,
                    float **points, int *points_count);
  /** Build the shape's outline (and, when `border' is given, its border outline).
   *
   * Returns OK when the outline was built, EMPTY when the shape has no geometry to build one
   * from -- an outline that is legitimately empty, which the caller may cache like any other
   * result -- and ERROR when the build itself failed and produced nothing cacheable. The
   * distinction is load-bearing: the outline cache key covers the whole group, so caching a
   * FAILURE hides the shape until the geometry next moves, while refusing to cache an
   * legitimately EMPTY one rebuilds every shape of the group on every expose, forever.
   */
  dt_masks_raster_result_t (*get_points_border)(struct dt_develop_t *dev, struct dt_masks_form_t *form,
                           float **points, int *points_count,
                           float **border, int *border_count,
                           dt_masks_skip_range_t **border_skips, int *border_skip_count,
                           int source, const dt_iop_module_t *const module);
  /** Rasterise into a freshly allocated buffer covering the shape's own bounding box.
   * Same three outcomes as get_mask_roi. On anything but OK the out-parameters are still
   * written (NULL buffer, zero geometry): callers read them unconditionally. */
  dt_masks_raster_result_t (*get_mask)(const dt_iop_module_t *const module, struct dt_dev_pixelpipe_t *pipe,
                  const dt_dev_pixelpipe_iop_t *const piece,
                  struct dt_masks_form_t *const form,
                  float **buffer, int *width, int *height, int *posx, int *posy);
  /** Rasterise into a pre-zeroed ROI-sized buffer.
   *
   * `touched` (may be NULL) receives the buffer-relative rectangle enclosing every pixel
   * written -- see masks_touched.h.
   *
   * Returns OK when the buffer was written, EMPTY when the shape has nothing to draw here
   * (degenerate geometry, or wholly outside `roi`) and the buffer was left untouched, ERROR
   * when the shape could not be computed and the buffer's contents are undefined. EMPTY is
   * NOT a failure: the group fold skips such a shape and keeps folding. Every implementation
   * must agree on which is which -- see dt_masks_raster_result_t.
   */
  dt_masks_raster_result_t (*get_mask_roi)(const dt_iop_module_t *const fmodule, struct dt_dev_pixelpipe_t *pipe,
                      const dt_dev_pixelpipe_iop_t *const piece,
                      struct dt_masks_form_t *const form,
                      const dt_iop_roi_t *roi, float *buffer, dt_iop_roi_t *touched);
  /** The shape's bounding box. Same three outcomes; the out-parameters are always written. */
  dt_masks_raster_result_t (*get_area)(const dt_iop_module_t *const module, struct dt_dev_pixelpipe_t *pipe,
                  const dt_dev_pixelpipe_iop_t *const piece,
                  struct dt_masks_form_t *const form,
                  int *width, int *height, int *posx, int *posy);
  /** The clone source's bounding box. Same three outcomes; out-parameters always written. */
  dt_masks_raster_result_t (*get_source_area)(dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                         dt_dev_pixelpipe_iop_t *piece, struct dt_masks_form_t *form,
                         int *width, int *height, int *posx, int *posy);
  gboolean (*get_gravity_center)(struct dt_develop_t *dev, const struct dt_masks_form_t *form, float center[2], float *area);
  float (*get_interaction_value)(const struct dt_masks_form_t *form, dt_masks_interaction_t interaction);
  float (*set_interaction_value)(struct dt_masks_form_t *form, dt_masks_interaction_t interaction, float value,
                                 dt_masks_increment_t increment, int flow,
                                 struct dt_masks_form_gui_t *gui, struct dt_iop_module_t *module);
  /* Recompute hovered handles/nodes from the cached cursor state in gui. */
  int (*update_hover)(struct dt_masks_form_t *form, struct dt_masks_form_gui_t *gui, int index);
  /* Mouse x and y are widget-space coordinates from GTK/Cairo */
  int (*mouse_moved)(struct dt_iop_module_t *module, double x, double y, double pressure, int which,
                     struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Mouse x and y are widget-space coordinates from GTK/Cairo */
  int (*mouse_scrolled)(struct dt_iop_module_t *module, double x, double y, int up, const int delta_y, uint32_t state,
                        struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index,
                        dt_masks_interaction_t interaction);
  /* Mouse x and y are widget-space coordinates from GTK/Cairo */
  int (*button_pressed)(struct dt_iop_module_t *module, double x, double y,
                        double pressure, int which, int type, uint32_t state,
                        struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Mouse x and y are widget-space coordinates from GTK/Cairo */
  int (*button_released)(struct dt_iop_module_t *module, double x, double y, int which, uint32_t state,
                         struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  /* Key event */
  int (*key_pressed)(struct dt_iop_module_t *module, GdkEventKey *event, struct dt_masks_form_t *form, int parentid, struct dt_masks_form_gui_t *gui, int index);
  void (*post_expose)(cairo_t *cr, float zoom_scale, struct dt_masks_form_gui_t *gui, int index, int num_points);
  // The function to draw the shape in question. Signature must match shape_draw_function_t.
  shape_draw_function_t draw_shape;
  /** initialise all control points to eventually match a catmull-rom like spline */
  void (*init_ctrl_points)(struct dt_masks_form_t *form);
  int (*populate_context_menu)(GtkWidget *menu, struct dt_masks_form_t *form, struct dt_masks_form_gui_t *gui, const float pzx, const float pzy);
} dt_masks_functions_t;

/* Rasterisation entry points, dispatched only from inside the masks module: the group fold
 * calls get_mask_roi on its children, and the GUI outline builder calls get_points_border.
 * They were declared in the public header with no caller outside this directory. */
dt_masks_raster_result_t dt_masks_get_mask_roi(const dt_iop_module_t *const module, dt_dev_pixelpipe_t *pipe,
                                               const dt_dev_pixelpipe_iop_t *const piece,
                                               dt_masks_form_t *const form, const dt_iop_roi_t *roi,
                                               float *buffer, dt_iop_roi_t *touched);

dt_masks_raster_result_t dt_masks_get_points_border(struct dt_develop_t *dev, dt_masks_form_t *form,
                               float **points, int *points_count,
                               float **border, int *border_count,
                               dt_masks_skip_range_t **border_skips, int *border_skip_count,
                               int source, dt_iop_module_t *module);

/** Find every place a shape's closed border contour crosses itself, writing one (i, j) sample-
 * index pair per crossing into @p crossing_pairs (2 floats each, at most @p max_pairs pairs) and
 * returning how many were written. @p header is where the border samples start, past the shape's
 * per-node header triplets. Feed the result to dt_masks_skip_ranges_build().
 *
 * Exact (segment intersection over a spatial hash), unlike polygon.c's own pixel-grid detector,
 * and the reported indices sit AT the crossing so a cut made between them closes. */
int dt_masks_border_find_self_intersections(const float *const border, const int border_count,
                                            const int header, float *const crossing_pairs,
                                            const int max_pairs);

/** Is @p index inside one of the excluded spans? For a consumer that SEARCHES the outline rather
 * than walking it; a forward walk should use dt_masks_draw_outline_runs() instead. */
gboolean dt_masks_skip_contains(const dt_masks_skip_range_t *skips, const int skip_count, const int index);


/** One rectangular lattice of sample points over a rasterisation ROI.
 *
 * Every shape whose mask is evaluated on a coarse grid and then interpolated back up describes
 * that grid the same way: a cell count, a spacing in output pixels, the ROI origin, and the
 * inverse of the ROI scale. `x0`/`y0` are the index of the first cell -- non-zero for the shapes
 * that grid only their own bounding box (circle, ellipse), zero for the ones that grid the whole
 * ROI (gradient). */
typedef struct dt_masks_sample_grid_t
{
  int x0, y0;         // index of the first cell, in cells
  int width, height;  // number of cells
  int step;           // cell spacing, in output pixels
  int px, py;         // ROI origin, in output pixels
  float iscale;       // 1.0f / roi->scale
} dt_masks_sample_grid_t;

/** Axis-aligned bounding box of an outline, in the outline's own coordinates.
 *
 * `points` is the interleaved x/y array a shape's `get_points_border` produced, and point 0 is
 * SKIPPED: every shape that uses this stores its centre there, not a point on the outline. */
void dt_masks_points_bounding_box(const float *const points, const int num_points,
                                  int *width, int *height, int *posx, int *posy);

float *dt_masks_sample_grid_backtransform(struct dt_dev_pixelpipe_t *pipe, const double iop_order,
                                          const dt_masks_sample_grid_t *const grid,
                                          const char *const shape, const char *const form_name);


/** Give a creation preview its clone-source outline: the target outline, offset by the drag.
 *
 * Allocates `preview->source_points` from `preview->points`, which must already be populated.
 * Returns 0 on success, 1 on allocation failure -- the caller's own error convention. */
int dt_masks_preview_add_clone_source(struct dt_masks_form_gui_t *gui,
                                      struct dt_masks_preview_buffers_t *preview);

/** Turn an outline of a shape into the outline of its clone source, in final image reference.
 *
 * `*points` arrives as the TARGET outline in RAW reference and leaves as the SOURCE outline with
 * every distortion applied. `first_shifted` is the index the outline proper begins at: shapes
 * that keep handle points in their header pass the index past them and get their centre written
 * directly, shapes whose point 0 is the centre and nothing else pass 0 and have it shifted with
 * the rest. On failure the buffer is freed and `*points`/`*points_count` are cleared.
 *
 * Returns 0 on success, 1 on failure. */
int dt_masks_points_shift_to_source(struct dt_develop_t *dev, const struct dt_iop_module_t *module,
                                    float **points, int *points_count,
                                    const float xs, const float ys, const int first_shifted);

/** Largest cell spacing any caller of the sample-grid helpers may ask for.
 *
 * The ROI rasterisers clamp their step to 4; `_gradient_get_mask()` uses a fixed 8. The
 * interpolator sizes its per-cell weight tables from this, so a caller wanting a coarser grid
 * must raise it here rather than locally. */
#define DT_MASKS_GRID_MAX_STEP 8

/** Bilinear expansion of a coarse grid of mask values back to full ROI resolution.
 *
 * `points` is the buffer `dt_masks_sample_grid_backtransform()` returned, with each cell's mask
 * value written over its x coordinate (that is, at `points[index * 2]`) -- every shape evaluates
 * in place like that, so the interpolator reads the same stride whatever the shape.
 *
 * Writes into `buffer` over the rectangle the grid covers, clipped to the buffer, and reports the
 * exclusive end of that rectangle through `endx`/`endy` for callers that go on to mark the
 * touched box. */
void dt_masks_sample_grid_interpolate(const float *const points, const dt_masks_sample_grid_t *const grid,
                                      float *const buffer, const int buf_width, const int buf_height,
                                      int *const endx, int *const endy);
/**
 * @brief Turn the self-intersection detector's raw crossing pairs into the disjoint,
 * forward-only skip ranges every border walk consumes. Pure, allocation-free.
 *
 * @details Each pair (v, w) names two raw border indices where the offset curve crosses
 * itself, in whatever order the detector's discovery walk met them. This normalizes each to
 * forward order, DROPS a pair whose forward span is the longer arc of the closed contour (a
 * fold straddling the buffer seam -- issue #1313: encoding it as [min,max] named its
 * complement and swallowed the shape; the sentinels, and now the ranges, can only express a
 * forward skip, so that fold is left in and the damage stays bounded by the fold itself),
 * then sorts and merges overlaps so the result is disjoint (unmerged overlapping ranges are
 * how the walk got trapped in a cycle once already).
 *
 * @param crossing_pairs 2*pair_count floats, as _polygon_find_self_intersection() emits them.
 * @param point_count    Number of points in the border buffer (bounds the indices).
 * @param out            Capacity >= pair_count entries. Receives the merged ranges.
 * @param dropped_wrapping (may be NULL) how many seam-straddling pairs were dropped.
 * @return the number of ranges written to @p out.
 */
int dt_masks_skip_ranges_build(const float *crossing_pairs, int pair_count, int point_count,
                               dt_masks_skip_range_t *out, int *dropped_wrapping);

/**
 * @brief Ray-cast point-in-polygon over a form point stream, honouring skip ranges.
 *
 * Walks [points_start, points_count) forward, wrapping to points_start exactly once. A skip
 * range closes the contour with a chord from the point before `jump_from` to `resume_at`.
 * A range that does not move the walk strictly forward is IGNORED AND REPORTED rather than
 * followed: a backward jump would re-walk the span just left and spin until the visit cap --
 * the silent-garbage mode both historical bugs of the in-band encoding shipped as.
 *
 * @param skips may be NULL (with skip_count 0): no cuts, the common case for every shape
 *              but polygon and for path (non-border) walks.
 * @return Index of the first tested point found inside the form, -1 otherwise.
 */
int dt_masks_point_in_form_exact(const float *pts, int num_pts, const float *points,
                                 int points_start, int points_count,
                                 const dt_masks_skip_range_t *skips, int skip_count);

/* The raw membership-state mutator. Module-private on purpose: it takes a row by pointer and
 * cannot touch the group, so every external caller had to remember the copy-on-write dance and
 * most did not. Outside code goes through dt_masks_group_set_member_operation(), which owns it. */
void dt_masks_group_entry_apply_operation(struct dt_masks_form_group_t *pt,
                                          const dt_masks_state_t apply_state);

/** the shape-specific function tables */
extern const dt_masks_functions_t dt_masks_functions_circle;
extern const dt_masks_functions_t dt_masks_functions_ellipse;
extern const dt_masks_functions_t dt_masks_functions_brush;
extern const dt_masks_functions_t dt_masks_functions_polygon;
extern const dt_masks_functions_t dt_masks_functions_gradient;
extern const dt_masks_functions_t dt_masks_functions_group;

/** init dt_masks_form_gui_t struct with default values */


/* Shared between masks.c (data/persistence) and masks_gui.c (interaction) since the
 * GUI half moved out; internal to the masks subsystem like everything here. */
void dt_masks_form_gui_points_free(gpointer data);
void _check_id(dt_develop_t *dev, dt_masks_form_t *mask_form);
void _set_group_name_from_module(dt_iop_module_t *module, dt_masks_form_t *group_form);
dt_masks_form_t *_group_create(dt_develop_t *develop, dt_iop_module_t *module, dt_masks_type_t group_type);
dt_masks_form_t *_group_from_module(dt_develop_t *develop, dt_iop_module_t *module);
int _find_in_group(dt_develop_t *dev, dt_masks_form_t *group_form, int form_id);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_MASKS_MASKS_FUNCTIONS_H
