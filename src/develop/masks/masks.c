/*
    This file is part of darktable,
    Copyright (C) 2013-2014, 2016, 2019-2021 Aldric Renaudin.
    Copyright (C) 2013, 2016-2021 Pascal Obry.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013-2014, 2016-2018 Tobias Ellinghaus.
    Copyright (C) 2013-2016, 2019-2020 Ulrich Pegelow.
    Copyright (C) 2016, 2018 Matthieu Moy.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2017, 2019 luzpaz.
    Copyright (C) 2017 Peter Budai.
    Copyright (C) 2018 johannes hanika.
    Copyright (C) 2019-2020 Diederik Ter Rahe.
    Copyright (C) 2019 Jacopo Guderzo.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020 Heiko Bauke.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 darkelectron.
    Copyright (C) 2021 Hanno Schwalm.
    Copyright (C) 2021 Philipp Lutz.
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023 Luca Zulberti.
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
#include "develop/masks.h"
#include "develop/develop.h"
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/math.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "common/undo.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include <stdint.h>

/**
 * @brief Check whether a point lies within a squared radius of a center.
 *
 * Assumptions/caveats:
 * - Uses squared distance to avoid sqrt.
 * - Callers must pass a squared radius (not the radius).
 */
gboolean dt_masks_point_is_within_radius(const float point_x, const float point_y,
                                         const float center_x, const float center_y,
                                         const float squared_radius)
{
  const float delta_x = point_x - center_x;
  const float delta_y = point_y - center_y;
  const float squared_distance = delta_x * delta_x + delta_y * delta_y;
  return squared_distance <= squared_radius;
}

/**
 * @brief Centralized hit-testing for node/handle/segment selection across shapes.
 *
 * This function:
 * - Translates pointer coordinates into GUI space,
 * - Resets selection flags,
 * - Tests border/curve handles and nodes,
 * - Delegates inside/border/segment tests to the shape callback.
 *
 * node_count_override can be used for shapes that don't expose nodes via GList
 * (e.g. gradient/ellipse control points). Pass -1 to use g_list_length().
 *
 * Callers provide shape-specific callbacks for handles and distance tests.
 *
 * The cached cursor in `mask_gui->pos` is authoritative for hit testing.
 */
int dt_masks_find_closest_handle_common(dt_masks_form_t *mask_form,
                                        dt_masks_form_gui_t *mask_gui, int form_index, int node_count_override,
                                        dt_masks_border_handle_fn border_handle_cb,
                                        dt_masks_curve_handle_fn curve_handle_cb,
                                        dt_masks_node_position_fn node_position_cb,
                                        dt_masks_distance_fn distance_cb,
                                        dt_masks_post_select_fn post_select_cb,
                                        void *user_data)
{
  if(!mask_form || !mask_form->points) return 0;
  if(!mask_gui->creation && mask_gui->group_selected != form_index) return 0;

  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(!gui_points) return 0;

  // Handle detection in backbuffer space.
  const float cursor_radius = DT_GUI_MOUSE_EFFECT_RADIUS_SCALED;
  const float cursor_radius2 = cursor_radius * cursor_radius;
  const float cursor_x = mask_gui->pos[0];
  const float cursor_y = mask_gui->pos[1];
  const int selected_node = dt_masks_gui_selected_node_index(mask_gui);

  mask_gui->node_hovered = -1;
  mask_gui->handle_hovered = -1;
  mask_gui->seg_hovered = -1;
  mask_gui->handle_border_hovered = -1;

  if(mask_gui->node_dragging >= 0)
  {
    mask_gui->node_hovered = mask_gui->node_dragging;
    return 1;
  }
  if(mask_gui->handle_dragging >= 0)
  {
    mask_gui->handle_hovered = mask_gui->handle_dragging;
    return 1;
  }
  if(mask_gui->handle_border_dragging >= 0)
  {
    mask_gui->handle_border_hovered = mask_gui->handle_border_dragging;
    return 1;
  }
  if(mask_gui->seg_dragging >= 0)
  {
    mask_gui->seg_hovered = mask_gui->seg_dragging;
    return 1;
  }

  const int node_count = (node_count_override >= 0) ? node_count_override
                                                    : (int)g_list_length(mask_form->points);

  const gboolean has_bezier_layout = mask_form->uses_bezier_points_layout;
  const gboolean can_test_nodes = (node_position_cb != NULL) || has_bezier_layout;
  const gboolean has_selected_node = (node_count > 0) && can_test_nodes
                                     && (mask_gui->group_selected == form_index)
                                     && selected_node >= 0 && selected_node < node_count;

  if(has_selected_node)
  {
    // Current node's border handle (feather handle).
    float handle_x = NAN;
    float handle_y = NAN;
    if(border_handle_cb
       && border_handle_cb(gui_points, node_count, selected_node, &handle_x, &handle_y, user_data)
       && dt_masks_point_is_within_radius(cursor_x, cursor_y, handle_x, handle_y, cursor_radius2))
    {
      mask_gui->handle_border_hovered = selected_node;
      return 1;
    }

    // Current node's curve handle.
    if(!dt_masks_node_is_cusp(gui_points, selected_node) && curve_handle_cb)
    {
      curve_handle_cb(gui_points, selected_node, &handle_x, &handle_y, user_data);
      if(dt_masks_point_is_within_radius(cursor_x, cursor_y, handle_x, handle_y, cursor_radius2))
      {
        mask_gui->handle_hovered = selected_node;
        return 1;
      }
    }

    // Current node itself.
    float node_x = NAN;
    float node_y = NAN;
    if(node_position_cb)
    {
      node_position_cb(gui_points, selected_node, &node_x, &node_y, user_data);
    }
    else if(has_bezier_layout)
    {
      node_x = gui_points->points[selected_node * 6 + 2];
      node_y = gui_points->points[selected_node * 6 + 3];
    }
    if(!isnan(node_x) && !isnan(node_y)
       && dt_masks_point_is_within_radius(cursor_x, cursor_y, node_x, node_y, cursor_radius2))
    {
      mask_gui->node_hovered = selected_node;
      return 1;
    }
  }

  if(can_test_nodes)
  {
    for(int node_index = 0; node_index < node_count; node_index++)
    {
      float node_x = NAN;
      float node_y = NAN;
      if(node_position_cb)
      {
        node_position_cb(gui_points, node_index, &node_x, &node_y, user_data);
      }
      else if(has_bezier_layout)
      {
        node_x = gui_points->points[node_index * 6 + 2];
        node_y = gui_points->points[node_index * 6 + 3];
      }
      if(!isnan(node_x) && !isnan(node_y)
         && dt_masks_point_is_within_radius(cursor_x, cursor_y, node_x, node_y, cursor_radius2))
      {
        mask_gui->node_hovered = node_index;
        return 1;
      }
    }
  }

  if(!distance_cb) return 0;

  // Segment or shape hit tests.
  int inside = 0;
  int inside_border = 0;
  int near_segment = -1;
  int inside_source = 0;
  float nearest_distance = 0.0f;
  distance_cb(cursor_x, cursor_y, cursor_radius, mask_gui, form_index, node_count, &inside, &inside_border,
              &near_segment, &inside_source, &nearest_distance, user_data);

  if(near_segment >= 0)
  {
    if(near_segment < node_count)
      mask_gui->seg_hovered = near_segment;
    return 1;
  }

  if(inside_source)
  {
    mask_gui->form_selected = TRUE;
    mask_gui->source_selected = TRUE;
    if(post_select_cb) post_select_cb(mask_gui, inside, inside_border, inside_source, user_data);
    return 1;
  }
  if(inside_border)
  {
    mask_gui->form_selected = TRUE;
    mask_gui->border_selected = TRUE;
    if(post_select_cb) post_select_cb(mask_gui, inside, inside_border, inside_source, user_data);
    return 1;
  }
  if(inside)
  {
    mask_gui->form_selected = TRUE;
    if(post_select_cb) post_select_cb(mask_gui, inside, inside_border, inside_source, user_data);
    return 1;
  }

  return 0;
}

/**
 * @brief Deep-copy a mask form, including its points list.
 *
 * Caveats:
 * - The caller owns the returned form and must free it.
 * - Point payloads are copied byte-for-byte using the type's point struct size.
 */
dt_masks_form_t *dt_masks_dup_masks_form(const dt_masks_form_t *mask_form)
{
  if (!mask_form) return NULL;

  dt_masks_form_t *duplicate_form = malloc(sizeof(struct dt_masks_form_t));
  memcpy(duplicate_form, mask_form, sizeof(struct dt_masks_form_t));

  // Duplicate the GList *points payloads into a new list.
  GList *duplicated_points = NULL;

  if(mask_form->points)
  {
    const int point_struct_size = (mask_form->functions) ? mask_form->functions->point_struct_size : 0;

    if(point_struct_size != 0)
    {
      for(GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
      {
        void *point_copy = malloc(point_struct_size);
        memcpy(point_copy, point_node->data, point_struct_size);
        duplicated_points = g_list_prepend(duplicated_points, point_copy);
      }
    }
  }

  // The list was built in reverse order, so un-reverse it.
  duplicate_form->points = g_list_reverse(duplicated_points);

  return duplicate_form;
}

static void *_dup_masks_form_cb(const void *formdata, gpointer user_data)
{
  // Duplicate the main form struct, optionally substituting the provided override form.
  dt_masks_form_t *source_form = (dt_masks_form_t *)formdata;
  dt_masks_form_t *override_form = (dt_masks_form_t *)user_data;
  const dt_masks_form_t *form_to_copy
      = (override_form == NULL || source_form->formid != override_form->formid) ? source_form : override_form;
  return (void *)dt_masks_dup_masks_form(form_to_copy);
}

/**
 * @brief Find a form entry inside a group by form id.
 *
 * Assumption: only valid for DT_MASKS_GROUP forms.
 */
static inline dt_masks_form_group_t *_masks_group_find_form(dt_masks_form_t *group_form, const int form_id)
{
  if(!group_form || !(group_form->type & DT_MASKS_GROUP)) return NULL;

  // Iterate group entries to find the matching form id.
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    if(group_entry && group_entry->formid == form_id) return group_entry;
  }
  return NULL;
}

int dt_masks_group_index_from_formid(const dt_masks_form_t *group_form, int form_id)
{
  if(!group_form || !(group_form->type & DT_MASKS_GROUP)) return -1;

  int index = 0;
  for(const GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    const dt_masks_form_group_t *group_entry = (const dt_masks_form_group_t *)group_node->data;
    if(group_entry && group_entry->formid == form_id) return index;
    index++;
  }
  return -1;
}

/**
 * @brief Return the currently visible form used by the masks GUI.
 *
 * This can be a temporary group copy used for editing, not necessarily a form
 * stored in dev->forms.
 */
dt_masks_form_t *dt_masks_get_visible_form(const dt_develop_t *dev)
{
  if(!dev || !dev->form_gui) return NULL;
  return dev->form_gui->form_visible;
}

void dt_masks_set_visible_form(dt_develop_t *dev, dt_masks_form_t *form)
{
  if(!dev || !dev->form_gui) return;
  dev->form_gui->form_visible = form;
}

void dt_masks_gui_init(dt_develop_t *dev)
{
  if(!dev) return;

  if(!dev->form_gui)
  {
    dev->form_gui = (dt_masks_form_gui_t *)calloc(1, sizeof(dt_masks_form_gui_t));
    dt_masks_init_form_gui(dev->form_gui);
  }

  dt_masks_clear_form_gui(dev);
  dt_masks_set_visible_form(dev, NULL);
  dev->form_gui->pipe_hash = 0;
  dev->form_gui->formid = 0;
}

void dt_masks_gui_cleanup(dt_develop_t *dev)
{
  if(!dev || !dev->form_gui) return;

  dt_masks_clear_form_gui(dev);
  free(dev->form_gui);
  dev->form_gui = NULL;
  dt_masks_set_visible_form(dev, NULL);
}

void dt_masks_gui_set_dragging(dt_masks_form_gui_t *gui)
{
  if(!gui) return;

  if(gui->handle_selected && gui->handle_hovered >= 0) gui->handle_dragging = gui->handle_hovered;
  if(gui->handle_border_selected && gui->handle_border_hovered >= 0) gui->handle_border_dragging = gui->handle_border_hovered;
  if(gui->node_selected && gui->node_hovered >= 0) gui->node_dragging = gui->node_hovered;
  if(gui->seg_selected && gui->seg_hovered >= 0) gui->seg_dragging = gui->seg_hovered;
  if(gui->source_selected)
    gui->source_dragging = TRUE;
  else if(gui->form_selected)
    gui->form_dragging = TRUE;
}

void dt_masks_gui_reset_dragging(dt_masks_form_gui_t *gui)
{
  if(!gui) return;

  gui->handle_dragging = -1;
  gui->handle_border_dragging = -1;
  gui->node_dragging = -1;
  gui->seg_dragging = -1;
  gui->form_dragging = FALSE;
  gui->source_dragging = FALSE;
}

gboolean dt_masks_gui_is_dragging(const dt_masks_form_gui_t *gui)
{
  if(!gui) return FALSE;
  return (gui->form_dragging || gui->source_dragging || gui->seg_dragging >= 0 || gui->node_dragging >= 0
          || gui->handle_dragging >= 0 || gui->handle_border_dragging >= 0);
}

/**
 * @brief Return the group entry for a (parent, form) pair.
 *
 * Caveat: returns NULL if parent isn't a group or the entry is missing.
 */
dt_masks_form_group_t *dt_masks_form_group_from_parentid(int parent_id, int form_id)
{
  dt_masks_form_t *group_form = dt_masks_get_from_id(darktable.develop, parent_id);
  if(!group_form || !(group_form->type & DT_MASKS_GROUP)) return NULL;
  return _masks_group_find_form(group_form, form_id);
}

/**
 * @brief Get the selected group entry from the GUI selection index.
 *
 * Selection sequence overview:
 * - The GUI stores a "working" selection index in dt_masks_form_gui_t::group_selected.
 *   This is fast for UI interaction but can become stale when the group list mutates
 *   (insert/remove/reorder or reallocated nodes).
 * - dt_masks_form_get_selected_group() uses that index directly. It assumes the list
 *   is unchanged since the GUI selection was set.
 * - dt_masks_form_get_selected_group_live() resolves the selection more safely by:
 *   1) attempting the GUI index,
 *   2) re-resolving through parentid/formid to refresh the pointer if needed.
 *
 * Use dt_masks_form_get_selected_group() in tight GUI paths where the list is known
 * stable; use dt_masks_form_get_selected_group_live() when correctness matters across
 * potential list mutations.
 * 
 * @todo simplify that.
 */
dt_masks_form_group_t *dt_masks_form_get_selected_group(const dt_masks_form_t *mask_form,
                                                        const dt_masks_form_gui_t *mask_gui)
{
  if(!mask_form || !mask_gui) return NULL;
  if(mask_gui->group_selected < 0) return NULL;
  return (dt_masks_form_group_t *)g_list_nth_data(mask_form->points, mask_gui->group_selected);
}

/**
 * @brief Resolve a "live" selected group entry, even if GUI selection is stale.
 *
 * Selection source:
 * - GUI index (mask_gui->group_selected) for the currently visible group.
 *
 * If the GUI works on a temporary group copy, we re-resolve through parentid
 * to get the live entry from dev->forms.
 */
dt_masks_form_group_t *dt_masks_form_get_selected_group_live(const dt_masks_form_t *mask_form,
                                                             const dt_masks_form_gui_t *mask_gui)
{
  if(!mask_form || !mask_gui) return NULL;

  dt_masks_form_group_t *selected_group_entry = NULL;
  if(mask_gui->group_selected >= 0)
    selected_group_entry = dt_masks_form_get_selected_group(mask_form, mask_gui);

  if(!selected_group_entry) return NULL;

  if(selected_group_entry->parentid > 0)
  {
    // Re-resolve via parentid to ensure the pointer is still current.
    dt_masks_form_group_t *resolved_group_entry
        = dt_masks_form_group_from_parentid(selected_group_entry->parentid, selected_group_entry->formid);
    if(resolved_group_entry) return resolved_group_entry;
  }

  return selected_group_entry;
}

/**
 * @brief Resolve the concrete form that should receive an event.
 *
 * Visible groups expose one selected child to the event path. Non-group forms
 * dispatch to themselves.
 */
static dt_masks_form_t *_dt_masks_events_get_dispatch_form(dt_masks_form_t *visible_form,
                                                           const dt_masks_form_gui_t *mask_gui,
                                                           dt_masks_form_group_t **group_entry,
                                                           int *parent_id, int *form_index)
{
  if(group_entry) *group_entry = NULL;
  if(parent_id) *parent_id = 0;
  if(form_index) *form_index = 0;

  if(!visible_form) return NULL;
  if(!(visible_form->type & DT_MASKS_GROUP)) return visible_form;

  dt_masks_form_group_t *selected_group_entry
      = dt_masks_form_get_selected_group_live(visible_form, mask_gui);
  if(!selected_group_entry) return NULL;

  dt_masks_form_t *selected_form = dt_masks_get_from_id(darktable.develop, selected_group_entry->formid);
  if(!selected_form) return NULL;

  if(group_entry) *group_entry = selected_group_entry;
  if(parent_id) *parent_id = selected_group_entry->parentid;
  if(form_index) *form_index = mask_gui->group_selected;

  return selected_form;
}

/**
 * @brief Update group selection from the current cached cursor before leaf dispatch.
 *
 * If a handle on the currently selected child is already hovered, keep that child selected.
 * Otherwise, fall back to per-shape hit testing to resolve the leaf form under the cursor.
 */
static gboolean _dt_masks_events_group_update_selection(dt_masks_form_t *group_form,
                                                        dt_masks_form_gui_t *mask_gui)
{
  if(!group_form || !mask_gui) return FALSE;

  dt_develop_t *const dev = darktable.develop;
  const float radius = DT_GUI_MOUSE_EFFECT_RADIUS_SCALED;
  const float cursor_x = mask_gui->pos[0];
  const float cursor_y = mask_gui->pos[1];

  if(dt_masks_is_anything_hovered(mask_gui))
    return TRUE;

  if(mask_gui->group_selected >= 0)
    dt_masks_soft_reset_form_gui(mask_gui);

  dt_masks_form_t *selected_form = NULL;
  int selected_index = -1;
  float best_dist = FLT_MAX;

  int index = 0;
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node), index++)
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    if(!group_entry) continue;

    dt_masks_form_t *form = dt_masks_get_from_id(dev, group_entry->formid);
    if(!form) continue;

    int inside = 0;
    int inside_border = 0;
    int near = -1;
    int inside_source = 0;
    float dist = FLT_MAX;
    if(form->functions && form->functions->get_distance)
      form->functions->get_distance(cursor_x, cursor_y, radius, mask_gui, index, g_list_length(form->points),
                                    &inside, &inside_border, &near, &inside_source, &dist);

    if(inside || inside_border || near >= 0 || inside_source)
    {
      const float dx = mask_gui->raw_pos[0] - form->gravity_center[0];
      const float dy = mask_gui->raw_pos[1] - form->gravity_center[1];
      const float center_dist2 = dx * dx + dy * dy;
      const float combined_dist2 = dist * center_dist2;
      if(combined_dist2 < best_dist)
      {
        selected_form = form;
        selected_index = index;
        best_dist = combined_dist2;
      }
    }
  }

  if(selected_form)
  {
    mask_gui->group_selected = selected_index;
    return TRUE;
  }

  return mask_gui->group_selected >= 0;
}

static gboolean _dt_masks_events_should_update_hover_on_move(dt_masks_form_gui_t *mask_gui)
{
  if(!mask_gui || mask_gui->creation) return FALSE;
  if(mask_gui->form_rotating || mask_gui->border_toggling || mask_gui->gradient_toggling) return FALSE;
  if(dt_masks_gui_is_dragging(mask_gui)) return FALSE;
  return dt_masks_gui_should_hit_test(mask_gui);
}

static void _dt_masks_events_update_hover(dt_masks_form_t *dispatch_form, dt_masks_form_gui_t *mask_gui,
                                          const int form_index)
{
  if(!dispatch_form || !mask_gui || !dispatch_form->functions || !dispatch_form->functions->update_hover) return;
  dispatch_form->functions->update_hover(dispatch_form, mask_gui, form_index);
}

static gboolean _dt_masks_events_cursor_over_form(const dt_masks_form_t *dispatch_form,
                                                  dt_masks_form_gui_t *mask_gui,
                                                  const int form_index)
{
  if(!dispatch_form || !mask_gui || !dispatch_form->functions || !dispatch_form->functions->get_distance)
    return FALSE;

  int inside = 0;
  int inside_border = 0;
  int near = -1;
  int inside_source = 0;
  float dist = FLT_MAX;
  dispatch_form->functions->get_distance(mask_gui->pos[0], mask_gui->pos[1], DT_GUI_MOUSE_EFFECT_RADIUS_SCALED,
                                         mask_gui, form_index,
                                         g_list_length(dispatch_form->points), &inside, &inside_border, &near,
                                         &inside_source, &dist);
  return inside || inside_border || near >= 0 || inside_source;
}

/**
 * @brief Consume the initial drag motion used to disambiguate scrolling vs dragging in groups.
 */
static gboolean _dt_masks_events_group_blocks_motion(dt_masks_form_gui_t *mask_gui)
{
  if(!mask_gui) return FALSE;

  const float radius = DT_GUI_MOUSE_EFFECT_RADIUS_SCALED;
  if(mask_gui->scrollx == 0.0f || mask_gui->scrolly == 0.0f) return FALSE;

  if((mask_gui->scrollx - mask_gui->pos[0] < radius && mask_gui->scrollx - mask_gui->pos[0] > -radius)
     && (mask_gui->scrolly - mask_gui->pos[1] < radius && mask_gui->scrolly - mask_gui->pos[1] > -radius))
    return TRUE;

  mask_gui->scrollx = 0.0f;
  mask_gui->scrolly = 0.0f;
  return FALSE;
}

/**
 * @brief Flush a deferred throttled rebuild before drag state is reset.
 */
static gboolean _dt_masks_events_flush_rebuild_if_needed(struct dt_iop_module_t *module,
                                                         dt_masks_form_t *dispatch_form,
                                                         dt_masks_form_gui_t *mask_gui,
                                                         const int form_index, const int button)
{
  if(button != 1) return FALSE;
  if(!dt_masks_gui_is_dragging(mask_gui)) return FALSE;

  if(mask_gui->rebuild_pending)
  {
    if(dispatch_form)
    {
      dt_masks_gui_form_create(dispatch_form, mask_gui, form_index, module);

      dt_develop_t *const dev = darktable.develop;
      if(dev)
      {
        mask_gui->last_rebuild_ts = dt_get_wtime();
        mask_gui->last_rebuild_pos[0] = mask_gui->pos[0];
        mask_gui->last_rebuild_pos[1] = mask_gui->pos[1];
      }
    }
    mask_gui->rebuild_pending = FALSE;
  }

  return TRUE;
}

/**
 * @brief Duplicate the list of forms, replacing a single item by formid match.
 */
GList *dt_masks_dup_forms_deep(GList *form_list, dt_masks_form_t *replacement_form)
{
  return (GList *)g_list_copy_deep(form_list, _dup_masks_form_cb, (gpointer)replacement_form);
}

/**
 * @brief Build and display the on-canvas hint message for masks interactions.
 *
 * Pitfall: set_hint_message() may rely on gui->form_selected, so we may need
 *          a temporary override when no hint is produced.
 */
static gboolean _set_hinter_message(dt_masks_form_gui_t *mask_gui, const dt_masks_form_t *mask_form)
{
  char message[256] = "";

  const int form_type = mask_form->type;

  int opacity_percent = 100;

  const dt_masks_form_t *selected_form = mask_form;
  int selected_form_id = 0;
  if(mask_form && (mask_form->type & DT_MASKS_GROUP))
  {
    const dt_masks_form_group_t *selected_group_entry
        = dt_masks_form_get_selected_group_live(mask_form, mask_gui);
    if(selected_group_entry) selected_form_id = selected_group_entry->formid;
  }

  dt_print(DT_DEBUG_INPUT,
           "[masks] hint begin: form=%p type=%d gui=%p group_selected=%d form_selected=%d node_hovered=%d seg_hovered=%d selected_formid=%d\n",
           (void *)mask_form, mask_form ? mask_form->type : -1, (void *)mask_gui,
           mask_gui->group_selected, mask_gui->form_selected,
           mask_gui->node_hovered, mask_gui->seg_hovered,
           selected_form_id);
  if(form_type & DT_MASKS_GROUP)
  {
    // Resolve the selected form inside a group (if any).
    dt_masks_form_group_t *selected_group_entry = dt_masks_form_get_selected_group_live(mask_form, mask_gui);
    if(selected_group_entry)
    {
      opacity_percent
          = dt_masks_form_get_interaction_value(selected_group_entry, DT_MASKS_INTERACTION_OPACITY) * 100.f;
      selected_form = dt_masks_get_from_id(darktable.develop, selected_group_entry->formid);
      if(!selected_form) return FALSE;
    }
  }
  else
  {
    opacity_percent = (int)(dt_conf_get_float("plugins/darkroom/masks/opacity") * 100);
  }

  if(selected_form->functions && selected_form->functions->set_hint_message)
  {
    selected_form->functions->set_hint_message(mask_gui, selected_form, opacity_percent, message, sizeof(message));
  }

  dt_control_hinter_message(darktable.control, message);
  dt_print(DT_DEBUG_INPUT,
           "[masks] hint end: sel=%p has_set_hint=%d opacity=%d msg_len=%zu msg='%s'\n",
           (void *)selected_form,
           (selected_form && selected_form->functions && selected_form->functions->set_hint_message) ? 1 : 0,
           opacity_percent, strlen(message), message);
  return message[0] != '\0';
}

void dt_masks_init_form_gui(dt_masks_form_gui_t *mask_gui)
{
  memset(mask_gui, 0, sizeof(dt_masks_form_gui_t));

  mask_gui->pos[0] = mask_gui->pos[1] = -1.0f;
  mask_gui->rel_pos[0] = mask_gui->rel_pos[1] = -1.0f;
  mask_gui->raw_pos[0] = mask_gui->raw_pos[1] = -1.0f;
  mask_gui->pos_source[0] = mask_gui->pos_source[1] = -1.0f;
  mask_gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
  mask_gui->node_hovered = -1;
  mask_gui->handle_hovered = -1;
  mask_gui->seg_hovered = -1;
  mask_gui->handle_border_hovered = -1;
  mask_gui->node_selected = FALSE;
  mask_gui->handle_selected = FALSE;
  mask_gui->seg_selected = FALSE;
  mask_gui->handle_border_selected = FALSE;
  mask_gui->form_selected = FALSE;
  mask_gui->border_selected = FALSE;
  mask_gui->source_selected = FALSE;
  mask_gui->pivot_selected = FALSE;
  mask_gui->last_rebuild_ts = 0.0;
  mask_gui->last_rebuild_pos[0] = mask_gui->last_rebuild_pos[1] = 0.0f;
  mask_gui->rebuild_pending = FALSE;
  mask_gui->last_hit_test_pos[0] = mask_gui->last_hit_test_pos[1] = -1.0f;
}

void dt_masks_soft_reset_form_gui(dt_masks_form_gui_t *mask_gui)
{
  // Note: we have an hard reset function below that frees all buffers and such
  mask_gui->source_selected = FALSE;
  mask_gui->node_hovered = -1;
  mask_gui->handle_hovered = -1;
  mask_gui->seg_hovered = -1;
  mask_gui->handle_border_hovered = -1;
  mask_gui->node_selected = FALSE;
  mask_gui->handle_selected = FALSE;
  mask_gui->seg_selected = FALSE;
  mask_gui->handle_border_selected = FALSE;
  mask_gui->group_selected = -1;
  mask_gui->delta[0] = mask_gui->delta[1] = 0.0f;
  mask_gui->form_selected = mask_gui->border_selected = mask_gui->form_dragging = mask_gui->form_rotating = FALSE;
  mask_gui->pivot_selected = FALSE;
  mask_gui->handle_border_dragging = mask_gui->seg_dragging = mask_gui->handle_dragging = mask_gui->node_dragging = -1;
  mask_gui->last_rebuild_ts = 0.0;
  mask_gui->last_rebuild_pos[0] = mask_gui->last_rebuild_pos[1] = 0.0f;
  mask_gui->rebuild_pending = FALSE;
  mask_gui->last_hit_test_pos[0] = mask_gui->last_hit_test_pos[1] = -1.0f;
}

void dt_masks_gui_form_create(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui,
                              int form_index, dt_iop_module_t *module)
{
  const int gui_points_count = g_list_length(mask_gui->points);
  if(gui_points_count == form_index)
  {
    dt_masks_form_gui_points_t *gui_points_new
        = (dt_masks_form_gui_points_t *)calloc(1, sizeof(dt_masks_form_gui_points_t));
    mask_gui->points = g_list_append(mask_gui->points, gui_points_new);
  }
  else if(gui_points_count < form_index)
    return;

  dt_masks_gui_form_remove(mask_form, mask_gui, form_index);

  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(dt_masks_get_points_border(darktable.develop, mask_form, &gui_points->points, &gui_points->points_count,
                                &gui_points->border, &gui_points->border_count, 0, NULL) == 0)
  {
    if(mask_form->type & DT_MASKS_CLONE)
    {
      if(dt_masks_get_points_border(darktable.develop, mask_form, &gui_points->source, &gui_points->source_count,
                                    NULL, NULL, TRUE, module)
         != 0)
        return;
    }
    mask_gui->pipe_hash = darktable.develop->preview_pipe->backbuf.hash;
    mask_gui->formid = mask_form->formid;
    mask_gui->type = mask_form->type;

  }

  dt_masks_form_update_gravity_center(mask_form);
}

gboolean dt_masks_gui_form_create_throttled(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui,
                                            int form_index, dt_iop_module_t *module,
                                            float posx, float posy)
{
  if(!mask_form || !mask_gui) return FALSE;

  dt_develop_t *const develop = darktable.develop;
  if(!develop)
  {
    dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
    return TRUE;
  }

  const double now = dt_get_wtime();
  const double min_delta_time = 1.0 / 60.0;
  const float min_dist2 = 4.0f;
  const gboolean force_rebuild
      = (develop->preview_pipe && mask_gui->pipe_hash != develop->preview_pipe->backbuf.hash);

  if(!force_rebuild && mask_gui->last_rebuild_ts > 0.0)
  {
    const double elapsed_time = now - mask_gui->last_rebuild_ts;
    const float delta_x = posx - mask_gui->last_rebuild_pos[0];
    const float delta_y = posy - mask_gui->last_rebuild_pos[1];
    if(elapsed_time < min_delta_time && (delta_x * delta_x + delta_y * delta_y) < min_dist2)
    {
      mask_gui->rebuild_pending = TRUE;
      return FALSE;
    }
  }

  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
  mask_gui->last_rebuild_ts = now;
  mask_gui->last_rebuild_pos[0] = posx;
  mask_gui->last_rebuild_pos[1] = posy;
  mask_gui->rebuild_pending = FALSE;
  return TRUE;
}

void dt_masks_form_gui_points_free(gpointer data)
{
  if(!data) return;

  dt_masks_form_gui_points_t *gui_points = (dt_masks_form_gui_points_t *)data;

  dt_pixelpipe_cache_free_align(gui_points->points);
  dt_pixelpipe_cache_free_align(gui_points->border);
  dt_pixelpipe_cache_free_align(gui_points->source);
  free(gui_points);
}

void dt_masks_remove_node(struct dt_iop_module_t *module, dt_masks_form_t *mask_form, int parent_id,
                          dt_masks_form_gui_t *mask_gui, int form_index, int node_index)
{
  if(!mask_form || !mask_form->points) return;
  dt_masks_node_brush_t *brush_node = (dt_masks_node_brush_t *)g_list_nth_data(mask_form->points, node_index);
  if(!brush_node) return;
  mask_form->points = g_list_remove(mask_form->points, brush_node);
  free(brush_node);
  mask_gui->node_hovered = -1;
  mask_gui->node_selected = FALSE;
  if(mask_form->functions && mask_form->functions->init_ctrl_points)
    mask_form->functions->init_ctrl_points(mask_form);
    
  // we recreate the form points
  dt_masks_gui_form_create(mask_form, mask_gui, form_index, module);
}


/**
 * @brief Remove a shape from the GUI and free its resources.
 * 
 * @param module The module owning the mask
 * @param form The form to remove
 * @param parentid The parent ID of the form
 * @param gui The GUI state
 * @param index The index of the form in the group
 * 
 * @return gboolean TRUE if the form was removed, FALSE otherwise.
 */
static gboolean _masks_remove_shape(struct dt_iop_module_t *module, dt_masks_form_t *mask_form, int parent_id,
                                    dt_masks_form_gui_t *mask_gui, int form_index)
{
  // if the form doesn't below to a group, we don't delete it
  if(parent_id <= 0) return 1;

  // we hide the form
  dt_masks_form_t *visible_form = dt_masks_get_visible_form(darktable.develop);
  if(!visible_form || !(visible_form->type & DT_MASKS_GROUP))
    dt_masks_change_form_gui(NULL);
  else if(g_list_shorter_than(visible_form->points, 2))
    dt_masks_change_form_gui(NULL);
  else
  {
    const int edit_mode = mask_gui->edit_mode;
    dt_masks_clear_form_gui(darktable.develop);
    for(GList *forms = visible_form->points; forms; forms = g_list_next(forms))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)forms->data;
      if(group_entry->formid == mask_form->formid)
      {
        visible_form->points = g_list_remove(visible_form->points, group_entry);
        free(group_entry);
        break;
      }
    }
    mask_gui->edit_mode = edit_mode;
  }

  // we delete or remove the shape
  // Called from node removal, if there was not enough nodes to keep the whole shape,
  // that's how this was called:
  // dt_masks_form_remove(module, NULL, form);
  // Called from shape removal, this is how it was called:
  dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parent_id), mask_form);
  // Not sure what difference it makes.

  return 1;
}

gboolean dt_masks_form_cancel_creation(dt_iop_module_t *module, dt_masks_form_gui_t *mask_gui)
{
  if(mask_gui->creation)
  {
    if(mask_gui->guipoints)
    {
      dt_masks_dynbuf_free(mask_gui->guipoints);
      dt_masks_dynbuf_free(mask_gui->guipoints_payload);
      mask_gui->guipoints = NULL;
      mask_gui->guipoints_payload = NULL;
      mask_gui->guipoints_count = 0;
    }

    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);

    return TRUE;
  }
  return FALSE;
}

gboolean dt_masks_gui_delete(struct dt_iop_module_t *module, dt_masks_form_t *mask_form,
                             dt_masks_form_gui_t *mask_gui, const int parent_id)
{
  // Just clean temp mask if we are in creation mode
  if(dt_masks_form_cancel_creation(module, mask_gui))
    return TRUE;

  // we remove the selected node (and the entire form if there is too few nodes left)
  if(((mask_form->type & DT_MASKS_IS_PATH_SHAPE) != 0) && mask_gui->node_selected)
  {
    if(g_list_shorter_than(mask_form->points, 3))
      return _masks_remove_shape(module, mask_form, parent_id, mask_gui, mask_gui->group_selected);

    dt_masks_remove_node(module, mask_form, parent_id, mask_gui, mask_gui->group_selected,
                         mask_gui->node_hovered);

    return TRUE;
  }
  // we remove the entire shape
  else if(parent_id > 0 && mask_gui->edit_mode == DT_MASKS_EDIT_FULL)
    return _masks_remove_shape(module, mask_form, parent_id, mask_gui, mask_gui->group_selected);
  
  return FALSE;
}

void dt_masks_gui_form_remove(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui, int form_index)
{
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  mask_gui->pipe_hash = mask_gui->formid = 0;

  if(gui_points)
  {
    gui_points->points_count = gui_points->border_count = gui_points->source_count = 0;
    dt_pixelpipe_cache_free_align(gui_points->points);
    gui_points->points = NULL;
    dt_pixelpipe_cache_free_align(gui_points->border);
    gui_points->border = NULL;
    dt_pixelpipe_cache_free_align(gui_points->source);
    gui_points->source = NULL;
  }
}

void dt_masks_gui_form_test_create(dt_masks_form_t *mask_form, dt_masks_form_gui_t *mask_gui,
                                   dt_iop_module_t *module)
{
  // we test if the image has changed
  if(mask_gui->pipe_hash > 0)
  {
    if(mask_gui->pipe_hash != darktable.develop->preview_pipe->backbuf.hash)
    {
      mask_gui->pipe_hash = mask_gui->formid = 0;
      g_list_free_full(mask_gui->points, dt_masks_form_gui_points_free);
      mask_gui->points = NULL;
    }
  }

  // we create the form if needed
  if(mask_gui->pipe_hash == 0)
  {
    if(mask_form->type & DT_MASKS_GROUP)
    {
      int form_index = 0;
      for(GList *group_node = mask_form->points; group_node; group_node = g_list_next(group_node))
      {
        dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
        dt_masks_form_t *child_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);
        if(!child_form) return;
        dt_masks_gui_form_create(child_form, mask_gui, form_index, module);
        form_index++;
      }
    }
    else
    {
      dt_masks_gui_form_create(mask_form, mask_gui, 0, module);
    }
  }
}

static void _check_id(dt_masks_form_t *mask_form)
{
  int new_form_id = 100;
  for(GList *form_node = darktable.develop->forms; form_node; )
  {
    dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
    if(existing_form->formid == mask_form->formid)
    {
      mask_form->formid = new_form_id++;
      form_node = darktable.develop->forms; // jump back to start of list
    }
    else
    {
      form_node = g_list_next(form_node); // advance to next form
    }
  }
}

static void _set_group_name_from_module(dt_iop_module_t *module, dt_masks_form_t *group_form)
{
  gchar *module_label = dt_history_item_get_name(module);
  snprintf(group_form->name, sizeof(group_form->name), "grp %s", module_label);
  g_free(module_label);
}

static dt_masks_form_t *_group_create(dt_develop_t *develop, dt_iop_module_t *module, dt_masks_type_t group_type)
{
  dt_masks_form_t *group_form = dt_masks_create(group_type);
  _set_group_name_from_module(module, group_form);
  _check_id(group_form);
  dt_masks_append_form(develop, group_form);
  module->blend_params->mask_id = group_form->formid;
  return group_form;
}

// get the group form associated to the module, if any
static dt_masks_form_t *_group_from_module(dt_develop_t *develop, dt_iop_module_t *module)
{
  return dt_masks_get_from_id(develop, module->blend_params->mask_id);
}

void dt_masks_append_form(dt_develop_t *develop, dt_masks_form_t *mask_form)
{
  dt_pthread_rwlock_wrlock(&develop->masks_mutex);
  develop->forms = g_list_append(develop->forms, mask_form);
  dt_pthread_rwlock_unlock(&develop->masks_mutex);
}

void dt_masks_remove_form(dt_develop_t *develop, dt_masks_form_t *mask_form)
{
  dt_pthread_rwlock_wrlock(&develop->masks_mutex);
  develop->forms = g_list_remove(develop->forms, mask_form);
  dt_pthread_rwlock_unlock(&develop->masks_mutex);
}

void dt_masks_gui_form_save_creation(dt_develop_t *develop, dt_iop_module_t *module, dt_masks_form_t *mask_form,
                                     dt_masks_form_gui_t *mask_gui)
{
  // we check if the id is already registered
  _check_id(mask_form);

  if(mask_gui) mask_gui->creation = FALSE;

  // mask nb will be at least the length of the list
  guint form_count = 0;

  // count only the same forms to have a clean numbering
  dt_pthread_rwlock_rdlock(&develop->masks_mutex);
  for(GList *form_node = develop->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
    if(existing_form->type == mask_form->type) form_count++;
  }
  dt_pthread_rwlock_unlock(&develop->masks_mutex);

  gboolean name_exists = FALSE;

  // check that we do not have duplicate, in case some masks have been
  // removed we can have hole and so nb could already exists.
  do
  {
    name_exists = FALSE;
    form_count++;

    if(mask_form->functions && mask_form->functions->set_form_name)
      mask_form->functions->set_form_name(mask_form, form_count);

    dt_pthread_rwlock_rdlock(&develop->masks_mutex);
    for(GList *form_node = develop->forms; form_node; form_node = g_list_next(form_node))
    {
      dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
      if(!strcmp(existing_form->name, mask_form->name))
      {
        name_exists = TRUE;
        break;
      }
    }
    dt_pthread_rwlock_unlock(&develop->masks_mutex);

  } while(name_exists);

  dt_masks_form_update_gravity_center(mask_form);
  dt_masks_append_form(develop, mask_form);

  dt_masks_form_group_t *group_entry = malloc(sizeof(dt_masks_form_group_t));
  if(module)
  {
    // is there already a masks group for this module ?
    dt_masks_form_t *group_form = _group_from_module(develop, module);
    if(!group_form)
    {
      // we create a new group
      if(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
        group_form = _group_create(develop, module, DT_MASKS_GROUP | DT_MASKS_CLONE);
      else
        group_form = _group_create(develop, module, DT_MASKS_GROUP);
    }
    // we add the form in this group
    group_entry->formid = mask_form->formid;
    group_entry->parentid = group_form->formid;
    group_entry->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION;
    group_entry->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    group_form->points = g_list_append(group_form->points, group_entry);
    
    // we update module gui
      
    if(mask_gui) dt_masks_iop_update(module);
  }

  if(mask_gui)
  {
    // show the form if needed
    develop->form_gui->formid = mask_form->formid;

    if(module)
    {
      // we save the move
      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(module);
      dt_dev_masks_selection_change(darktable.develop, module, mask_form->formid, TRUE);
      mask_gui->creation_module = NULL;
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_CHANGED, group_entry->formid,
                                    group_entry->parentid, DT_MASKS_EVENT_ADD);
    }
    else
    {
      // we select the new form
      dt_dev_masks_selection_change(darktable.develop, NULL, mask_form->formid, TRUE);
    }
  }

  // Free group_entry if it is unused.
  if(!mask_gui && !module) free(group_entry);
}

int dt_masks_form_duplicate(dt_develop_t *develop, int form_id)
{
  // we create a new empty form
  dt_masks_form_t *base_form = dt_masks_get_from_id(develop, form_id);
  if(!base_form) return -1;
  dt_masks_form_t *dest_form = dt_masks_create(base_form->type);
  _check_id(dest_form);

  // we copy the base values
  dest_form->source[0] = base_form->source[0];
  dest_form->source[1] = base_form->source[1];
  dest_form->version = base_form->version;
  snprintf(dest_form->name, sizeof(dest_form->name), _("copy of %s"), base_form->name);

  dt_masks_append_form(develop, dest_form);

  // we copy all the points
  if(base_form->functions)
    base_form->functions->duplicate_points(develop, base_form, dest_form);

  // and we return its id
  return dest_form->formid;
}

int dt_masks_get_points_border(dt_develop_t *develop, dt_masks_form_t *mask_form,
                               float **point_buffer, int *point_count,
                               float **border_buffer, int *border_count,
                               int source, dt_iop_module_t *module)
{
  if(mask_form->functions && mask_form->functions->get_points_border)
    return mask_form->functions->get_points_border(develop, mask_form, point_buffer, point_count,
                                                   border_buffer, border_count, source, module);
  return 1;
}

int dt_masks_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                      dt_masks_form_t *mask_form,
                      int *area_width, int *area_height, int *area_pos_x, int *area_pos_y)
{
  if(mask_form->functions && mask_form->functions->get_area)
    return mask_form->functions->get_area(module, piece, mask_form, area_width, area_height,
                                          area_pos_x, area_pos_y);
  return 1;
}

int dt_masks_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                             dt_masks_form_t *mask_form,
                             int *area_width, int *area_height,
                             int *area_pos_x, int *area_pos_y)
{
  *area_width = *area_height = *area_pos_x = *area_pos_y = 0;

  // must be a clone form
  if(mask_form->type & DT_MASKS_CLONE)
  {
    if(mask_form->functions && mask_form->functions->get_source_area)
      return mask_form->functions->get_source_area(module, piece, mask_form, area_width, area_height,
                                                   area_pos_x, area_pos_y);
  }
  return 1;
}

int dt_masks_version(void)
{
  return DEVELOP_MASKS_VERSION;
}

static int dt_masks_legacy_params_v1_to_v2(dt_develop_t *develop, void *params)
{
  /*
   * difference: before v2 images were originally rotated on load, and then
   * maybe in flip iop
   * after v2: images are only rotated in flip iop.
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  const dt_image_orientation_t orientation = dt_image_orientation(&develop->image_storage);

  if(orientation == ORIENTATION_NONE)
  {
    // image is not rotated, we're fine!
    mask_form->version = 2;
    return 0;
  }
  else
  {
    if(develop->iop == NULL) return 1;

    const char *opname = "flip";
    dt_iop_module_t *module = NULL;

    for(GList *module_node = develop->iop; module_node; module_node = g_list_next(module_node))
    {
      dt_iop_module_t *iop_module = (dt_iop_module_t *)module_node->data;
      if(!strcmp(iop_module->op, opname))
      {
        module = iop_module;
        break;
      }
    }

    if(module == NULL) return 1;

    dt_dev_pixelpipe_iop_t piece = { 0 };

    module->init_pipe(module, NULL, &piece);
    module->commit_params(module, module->default_params, NULL, &piece);

    piece.buf_in.width = 1;
    piece.buf_in.height = 1;

    GList *point_node = mask_form->points;

    if(!point_node) return 1;

    if(mask_form->type & DT_MASKS_CIRCLE)
    {
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)point_node->data;
      if(!circle) return 1;
      module->distort_backtransform(module, &piece, circle->center, 1);
    }
    else if(mask_form->type & DT_MASKS_POLYGON)
    {
      for(; point_node; point_node = g_list_next(point_node))
      {
        dt_masks_node_polygon_t *polygon_node = (dt_masks_node_polygon_t *)point_node->data;
        if(!polygon_node) return 1;
        module->distort_backtransform(module, &piece, polygon_node->node, 1);
        module->distort_backtransform(module, &piece, polygon_node->ctrl1, 1);
        module->distort_backtransform(module, &piece, polygon_node->ctrl2, 1);
      }
    }
    else if(mask_form->type & DT_MASKS_GRADIENT)
    { // TODO: new ones have wrong rotation.
      dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)point_node->data;
      if(!gradient) return 1;
      module->distort_backtransform(module, &piece, gradient->center, 1);

      if(orientation == ORIENTATION_ROTATE_180_DEG)
        gradient->rotation -= 180.0f;
      else if(orientation == ORIENTATION_ROTATE_CCW_90_DEG)
        gradient->rotation -= 90.0f;
      else if(orientation == ORIENTATION_ROTATE_CW_90_DEG)
        gradient->rotation -= -90.0f;
    }
    else if(mask_form->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)point_node->data;
      module->distort_backtransform(module, &piece, ellipse->center, 1);

      if(orientation & ORIENTATION_SWAP_XY)
      {
        const float y = ellipse->radius[0];
        ellipse->radius[0] = ellipse->radius[1];
        ellipse->radius[1] = y;
      }
    }
    else if(mask_form->type & DT_MASKS_BRUSH)
    {
      for(; point_node; point_node = g_list_next(point_node))
      {
        dt_masks_node_brush_t *brush_node = (dt_masks_node_brush_t *)point_node->data;
        if(!brush_node) return 1;
        module->distort_backtransform(module, &piece, brush_node->node, 1);
        module->distort_backtransform(module, &piece, brush_node->ctrl1, 1);
        module->distort_backtransform(module, &piece, brush_node->ctrl2, 1);
      }
    }

    if(mask_form->type & DT_MASKS_CLONE)
    {
      // NOTE: can be: DT_MASKS_CIRCLE, DT_MASKS_ELLIPSE, DT_MASKS_POLYGON
      module->distort_backtransform(module, &piece, mask_form->source, 1);
    }

    mask_form->version = 2;

    return 0;
  }
}

static void dt_masks_legacy_params_v2_to_v3_transform(const dt_image_t *image, float *coords)
{
  const float image_width = (float)image->width;
  const float image_height = (float)image->height;

  const float crop_x = (float)image->crop_x;
  const float crop_y = (float)image->crop_y;

  const float crop_width = (float)(image->width - image->crop_x - image->crop_width);
  const float crop_height = (float)(image->height - image->crop_y - image->crop_height);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by image original cropped dimensions
   * 2. un-crop them by adding top-left crop coordinates
   * 3. normalize them by the image fully uncropped dimensions
   */
  coords[0] = ((coords[0] * crop_width) + crop_x) / image_width;
  coords[1] = ((coords[1] * crop_height) + crop_y) / image_height;
}

static void dt_masks_legacy_params_v2_to_v3_transform_only_rescale(const dt_image_t *image, float *coords,
                                                                   size_t coords_count)
{
  const float image_width = (float)image->width;
  const float image_height = (float)image->height;

  const float crop_width = (float)(image->width - image->crop_x - image->crop_width);
  const float crop_height = (float)(image->height - image->crop_y - image->crop_height);

  /*
   * masks coordinates are normalized, so we need to:
   * 1. de-normalize them by minimal of image original cropped dimensions
   * 2. normalize them by the minimal of image fully uncropped dimensions
   */
  const float crop_min = MIN(crop_width, crop_height);
  const float image_min = MIN(image_width, image_height);
  for(size_t coord_index = 0; coord_index < coords_count; coord_index++)
    coords[coord_index] = ((coords[coord_index] * crop_min)) / image_min;
}

static int dt_masks_legacy_params_v2_to_v3(dt_develop_t *develop, void *params)
{
  /*
   * difference: before v3 images were originally cropped on load
   * after v3: images are cropped in rawprepare iop.
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  const dt_image_t *image = &(develop->image_storage);

  if(image->crop_x == 0 && image->crop_y == 0 && image->crop_width == 0 && image->crop_height == 0)
  {
    // image has no "raw cropping", we're fine!
    mask_form->version = 3;
    return 0;
  }
  else
  {
    GList *point_node = mask_form->points;

    if(!point_node) return 1;

    if(mask_form->type & DT_MASKS_CIRCLE)
    {
      dt_masks_node_circle_t *circle = (dt_masks_node_circle_t *)point_node->data;
      if(!circle) return 1;
      dt_masks_legacy_params_v2_to_v3_transform(image, circle->center);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, &circle->radius, 1);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, &circle->border, 1);
    }
    else if(mask_form->type & DT_MASKS_POLYGON)
    {
      for(; point_node; point_node = g_list_next(point_node))
      {
        dt_masks_node_polygon_t *polygon_node = (dt_masks_node_polygon_t *)point_node->data;
        if(!polygon_node) return 1;
        dt_masks_legacy_params_v2_to_v3_transform(image, polygon_node->node);
        dt_masks_legacy_params_v2_to_v3_transform(image, polygon_node->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(image, polygon_node->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, polygon_node->border, 2);
      }
    }
    else if(mask_form->type & DT_MASKS_GRADIENT)
    {
      dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)point_node->data;
      dt_masks_legacy_params_v2_to_v3_transform(image, gradient->center);
    }
    else if(mask_form->type & DT_MASKS_ELLIPSE)
    {
      dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)point_node->data;
      dt_masks_legacy_params_v2_to_v3_transform(image, ellipse->center);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, ellipse->radius, 2);
      dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, &ellipse->border, 1);
    }
    else if(mask_form->type & DT_MASKS_BRUSH)
    {
      for(; point_node;  point_node = g_list_next(point_node))
      {
        dt_masks_node_brush_t *brush_node = (dt_masks_node_brush_t *)point_node->data;
        if(!brush_node) return 1;
        dt_masks_legacy_params_v2_to_v3_transform(image, brush_node->node);
        dt_masks_legacy_params_v2_to_v3_transform(image, brush_node->ctrl1);
        dt_masks_legacy_params_v2_to_v3_transform(image, brush_node->ctrl2);
        dt_masks_legacy_params_v2_to_v3_transform_only_rescale(image, brush_node->border, 2);
      }
    }

    if(mask_form->type & DT_MASKS_CLONE)
    {
      // NOTE: can be: DT_MASKS_CIRCLE, DT_MASKS_ELLIPSE, DT_MASKS_POLYGON
      dt_masks_legacy_params_v2_to_v3_transform(image, mask_form->source);
    }

    mask_form->version = 3;

    return 0;
  }
}

static int dt_masks_legacy_params_v3_to_v4(dt_develop_t *develop, void *params)
{
  /*
   * difference affecting ellipse
   * up to v3: only equidistant feathering
   * after v4: choice between equidistant and proportional feathering
   * type of feathering is defined in new flags parameter
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  GList *point_node = mask_form->points;

  if(!point_node) return 1;

  if(mask_form->type & DT_MASKS_ELLIPSE)
  {
    dt_masks_node_ellipse_t *ellipse = (dt_masks_node_ellipse_t *)point_node->data;
    ellipse->flags = DT_MASKS_ELLIPSE_EQUIDISTANT;
  }

  mask_form->version = 4;

  return 0;
}


static int dt_masks_legacy_params_v4_to_v5(dt_develop_t *develop, void *params)
{
  /*
   * difference affecting gradient
   * up to v4: only linear gradient (relative to input image)
   * after v5: curved gradients
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  GList *point_node = mask_form->points;

  if(!point_node) return 1;

  if(mask_form->type & DT_MASKS_GRADIENT)
  {
    dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)point_node->data;
    gradient->curvature = 0.0f;
  }

  mask_form->version = 5;

  return 0;
}

static int dt_masks_legacy_params_v5_to_v6(dt_develop_t *develop, void *params)
{
  /*
   * difference affecting gradient
   * up to v5: linear transition
   * after v5: linear or sigmoidal transition
   */

  dt_masks_form_t *mask_form = (dt_masks_form_t *)params;

  GList *point_node = mask_form->points;

  if(!point_node) return 1;

  if(mask_form->type & DT_MASKS_GRADIENT)
  {
    dt_masks_anchor_gradient_t *gradient = (dt_masks_anchor_gradient_t *)point_node->data;
    gradient->state = DT_MASKS_GRADIENT_STATE_LINEAR;
  }

  mask_form->version = 6;

  return 0;
}


int dt_masks_legacy_params(dt_develop_t *develop, void *params, const int old_version, const int new_version)
{
  int result = 1;
#if 0 // we should not need this any longer
  if(old_version == 1 && new_version == 2)
  {
    result = dt_masks_legacy_params_v1_to_v2(develop, params);
  }
#endif

  if(old_version == 1 && new_version == 6)
  {
    result = dt_masks_legacy_params_v1_to_v2(develop, params);
    if(!result) result = dt_masks_legacy_params_v2_to_v3(develop, params);
    if(!result) result = dt_masks_legacy_params_v3_to_v4(develop, params);
    if(!result) result = dt_masks_legacy_params_v4_to_v5(develop, params);
    if(!result) result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }
  else if(old_version == 2 && new_version == 6)
  {
    result = dt_masks_legacy_params_v2_to_v3(develop, params);
    if(!result) result = dt_masks_legacy_params_v3_to_v4(develop, params);
    if(!result) result = dt_masks_legacy_params_v4_to_v5(develop, params);
    if(!result) result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }
  else if(old_version == 3 && new_version == 6)
  {
    result = dt_masks_legacy_params_v3_to_v4(develop, params);
    if(!result) result = dt_masks_legacy_params_v4_to_v5(develop, params);
    if(!result) result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }
  else if(old_version == 4 && new_version == 6)
  {
    result = dt_masks_legacy_params_v4_to_v5(develop, params);
    if(!result) result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }
  else if(old_version == 5 && new_version == 6)
  {
    result = dt_masks_legacy_params_v5_to_v6(develop, params);
  }

  return result;
}

static int form_id_seed = 0;

dt_masks_form_t *dt_masks_create(dt_masks_type_t type)
{
  dt_masks_form_t *mask_form = (dt_masks_form_t *)calloc(1, sizeof(dt_masks_form_t));
  if(!mask_form) return NULL;

  mask_form->type = type;
  mask_form->version = dt_masks_version();
  mask_form->formid = time(NULL) + form_id_seed++;
  mask_form->uses_bezier_points_layout = (type & (DT_MASKS_BRUSH | DT_MASKS_POLYGON)) ? TRUE : FALSE;

  if (type & DT_MASKS_CIRCLE)
    mask_form->functions = &dt_masks_functions_circle;
  else if (type & DT_MASKS_ELLIPSE)
    mask_form->functions = &dt_masks_functions_ellipse;
  else if (type & DT_MASKS_BRUSH)
    mask_form->functions = &dt_masks_functions_brush;
  else if (type & DT_MASKS_POLYGON)
    mask_form->functions = &dt_masks_functions_polygon;
  else if (type & DT_MASKS_GRADIENT)
    mask_form->functions = &dt_masks_functions_gradient;
  else if (type & DT_MASKS_GROUP)
    mask_form->functions = &dt_masks_functions_group;

  if (mask_form->functions && mask_form->functions->sanitize_config)
    mask_form->functions->sanitize_config(type);

  return mask_form;
}

dt_masks_form_t *dt_masks_create_ext(dt_masks_type_t type)
{
  dt_pthread_rwlock_wrlock(&darktable.develop->masks_mutex);
  dt_masks_form_t *mask_form = dt_masks_create(type);

  // all forms created here are registered in darktable.develop->allforms for later cleanup
  if(mask_form)
    darktable.develop->allforms = g_list_append(darktable.develop->allforms, mask_form);

  dt_pthread_rwlock_unlock(&darktable.develop->masks_mutex);

  return mask_form;
}

void dt_masks_replace_current_forms(dt_develop_t *develop, GList *forms)
{
  dt_pthread_rwlock_wrlock(&develop->masks_mutex);
  GList *forms_tmp = dt_masks_dup_forms_deep(forms, NULL);

  while(develop->forms)
  {
    darktable.develop->allforms = g_list_append(darktable.develop->allforms, develop->forms->data);
    develop->forms = g_list_delete_link(develop->forms, develop->forms);
  }

  develop->forms = forms_tmp;
  dt_pthread_rwlock_unlock(&develop->masks_mutex);

  for(GList *form_node = develop->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)form_node->data;
    dt_masks_form_update_gravity_center(mask_form);
  }
}

dt_masks_form_t *dt_masks_get_from_id_ext(GList *form_list, int form_id)
{
  for(; form_list; form_list = g_list_next(form_list))
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)form_list->data;
    if(mask_form->formid == form_id) return mask_form;
  }
  return NULL;
}

dt_masks_form_t *dt_masks_get_from_id(dt_develop_t *develop, int form_id)
{
  dt_pthread_rwlock_rdlock(&develop->masks_mutex);
  dt_masks_form_t *result = dt_masks_get_from_id_ext(develop->forms, form_id);
  dt_pthread_rwlock_unlock(&develop->masks_mutex);
  return result;
}

dt_iop_module_t *dt_masks_get_mask_manager(dt_develop_t *develop)
{
  for(GList *module_node = g_list_first(develop->iop); module_node; module_node = g_list_next(module_node))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)(module_node->data);
    if(strcmp(module->op, "mask_manager") == 0)
      return module;
  }
  return NULL;
}

GList *dt_masks_snapshot_current_forms(dt_develop_t *develop, gboolean reset_changed)
{
  dt_pthread_rwlock_rdlock(&develop->masks_mutex);
  GList *forms_snapshot = dt_masks_dup_forms_deep(develop->forms, NULL);
  dt_pthread_rwlock_unlock(&develop->masks_mutex);
  if(reset_changed) develop->forms_changed = FALSE;
  return forms_snapshot;
}

static void _masks_fill_used_forms(GList *forms_list, const int form_id, int *used_form_ids,
                                   const int used_count)
{
  for(int used_index = 0; used_index < used_count; used_index++)
  {
    if(used_form_ids[used_index] == 0)
    {
      used_form_ids[used_index] = form_id;
      break;
    }
    if(used_form_ids[used_index] == form_id) break;
  }

  dt_masks_form_t *mask_form = dt_masks_get_from_id_ext(forms_list, form_id);
  if(mask_form && (mask_form->type & DT_MASKS_GROUP))
  {
    for(GList *group_node = mask_form->points; group_node; group_node = g_list_next(group_node))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
      _masks_fill_used_forms(forms_list, group_entry->formid, used_form_ids, used_count);
    }
  }
}

int dt_masks_copy_used_forms_for_module(dt_develop_t *develop_dest, dt_develop_t *develop_src,
                                        const dt_iop_module_t *source_module)
{
  if(!(source_module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)) return 0;
  if(source_module->blend_params->mask_id <= 0) return 0;

  const guint form_count = g_list_length(develop_src->forms);
  if(form_count == 0) return 0;

  int *used_form_ids = calloc(form_count, sizeof(int));
  if(!used_form_ids) return 1;

  _masks_fill_used_forms(develop_src->forms, source_module->blend_params->mask_id,
                         used_form_ids, form_count);

  for(int form_index = 0; form_index < (int)form_count && used_form_ids[form_index] > 0; form_index++)
  {
    dt_masks_form_t *mask_form = dt_masks_get_from_id(develop_src, used_form_ids[form_index]);
    if(mask_form)
    {
      dt_masks_form_t *existing_form = dt_masks_get_from_id_ext(develop_dest->forms,
                                                                used_form_ids[form_index]);
      if(existing_form)
      {
        develop_dest->forms = g_list_remove(develop_dest->forms, existing_form);
        develop_dest->allforms = g_list_append(develop_dest->allforms, existing_form);
      }

      dt_masks_form_t *new_form = dt_masks_dup_masks_form(mask_form);
      if(!new_form)
      {
        free(used_form_ids);
        return 1;
      }
      develop_dest->forms = g_list_append(develop_dest->forms, new_form);
    }
    else
    {
      fprintf(stderr, "[dt_masks_copy_used_forms_for_module] form %i not found in source image\n",
              used_form_ids[form_index]);
    }
  }

  free(used_form_ids);
  return 0;
}

void dt_masks_read_masks_history(dt_develop_t *develop, const int32_t image_id)
{
  dt_dev_history_item_t *history_item = NULL;
  dt_dev_history_item_t *last_history_item = NULL;
  int previous_num = -1;

  sqlite3_stmt *statement = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(
      dt_database_get(darktable.db),
      "SELECT imgid, formid, form, name, version, points, points_count, source, num "
      "FROM main.masks_history WHERE imgid = ?1 ORDER BY num",
      -1, &statement, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(statement, 1, image_id);

  while(sqlite3_step(statement) == SQLITE_ROW)
  {
    // db record:
    // 0-img, 1-formid, 2-form_type, 3-name, 4-version, 5-points, 6-points_count, 7-source, 8-num

    // we get the values

    const int form_id = sqlite3_column_int(statement, 1);
    const int history_num = sqlite3_column_int(statement, 8);
    const dt_masks_type_t mask_type = sqlite3_column_int(statement, 2);
    dt_masks_form_t *mask_form = dt_masks_create(mask_type);
    mask_form->formid = form_id;
    const char *form_name = (const char *)sqlite3_column_text(statement, 3);
    g_strlcpy(mask_form->name, form_name, sizeof(mask_form->name));
    mask_form->version = sqlite3_column_int(statement, 4);
    mask_form->points = NULL;
    const int point_count = sqlite3_column_int(statement, 6);
    memcpy(mask_form->source, sqlite3_column_blob(statement, 7), sizeof(float) * 2);

    // and now we "read" the blob
    if(mask_form->functions)
    {
      const char *const point_buffer = (char *)sqlite3_column_blob(statement, 5);
      const size_t point_struct_size = mask_form->functions->point_struct_size;
      for(int point_index = 0; point_index < point_count; point_index++)
      {
        char *point_data = (char *)malloc(point_struct_size);
        memcpy(point_data, point_buffer + point_index * point_struct_size, point_struct_size);
        mask_form->points = g_list_append(mask_form->points, point_data);
      }
    }

    if(mask_form->version != dt_masks_version())
    {
      if(dt_masks_legacy_params(develop, mask_form, mask_form->version, dt_masks_version()))
      {
        const char *fname = develop->image_storage.filename + strlen(develop->image_storage.filename);
        while(fname > develop->image_storage.filename && *fname != '/') fname--;
        if(fname > develop->image_storage.filename) fname++;

        fprintf(stderr,
                "[_dev_read_masks_history] %s (imgid `%i'): mask version mismatch: history is %d, dt %d.\n",
                fname, image_id, mask_form->version, dt_masks_version());
        dt_control_log(_("%s: mask version mismatch: %d != %d"),
                       fname, dt_masks_version(), mask_form->version);

        continue;
      }
    }

    dt_masks_form_update_gravity_center(mask_form);

    // if this is a new history entry let's find it
    if(previous_num != history_num)
    {
      history_item = NULL;
      for(GList *history_node = develop->history; history_node; history_node = g_list_next(history_node))
      {
        dt_dev_history_item_t *history_entry = (dt_dev_history_item_t *)(history_node->data);
        if(history_entry->num == history_num)
        {
          history_item = history_entry;
          break;
        }
      }
      previous_num = history_num;
    }
    // add the form to the history entry
    if(history_item)
    {
      history_item->forms = g_list_append(history_item->forms, mask_form);
    }
    else
      fprintf(stderr,
              "[_dev_read_masks_history] can't find history entry %i while adding mask %s(%i)\n",
              history_num, mask_form->name, form_id);

    if(history_num < dt_dev_get_history_end_ext(develop)) last_history_item = history_item;
  }
  sqlite3_finalize(statement);

  // and we update the current forms snapshot
  dt_masks_replace_current_forms(develop, (last_history_item) ? last_history_item->forms : NULL);
}

void dt_masks_write_masks_history_item(const int32_t image_id, const int history_num,
                                       dt_masks_form_t *mask_form)
{
  sqlite3_stmt *statement = NULL;

  dt_print(DT_DEBUG_HISTORY, "[dt_masks_write_masks_history_item] writing mask %s of type %i for image %i\n",
           mask_form->name, mask_form->type, image_id);

  // write the form into the database
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO main.masks_history (imgid, num, formid, form, name, "
                              "version, points, points_count,source) VALUES "
                              "(?1, ?9, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                              -1, &statement, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(statement, 1, image_id);
  DT_DEBUG_SQLITE3_BIND_INT(statement, 9, history_num);
  DT_DEBUG_SQLITE3_BIND_INT(statement, 2, mask_form->formid);
  DT_DEBUG_SQLITE3_BIND_INT(statement, 3, mask_form->type);
  DT_DEBUG_SQLITE3_BIND_TEXT(statement, 4, mask_form->name, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_BLOB(statement, 8, mask_form->source, 2 * sizeof(float), SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(statement, 5, mask_form->version);
  if(mask_form->functions)
  {
    const size_t point_struct_size = mask_form->functions->point_struct_size;
    const guint point_count = g_list_length(mask_form->points);
    char *const restrict point_buffer = (char *)malloc(point_count * point_struct_size);
    int buffer_offset = 0;
    for(GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
    {
      memcpy(point_buffer + buffer_offset, point_node->data, point_struct_size);
      buffer_offset += point_struct_size;
    }
    DT_DEBUG_SQLITE3_BIND_BLOB(statement, 6, point_buffer,
                               point_count * point_struct_size, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_INT(statement, 7, point_count);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    free(point_buffer);
  }
}

void dt_masks_free_form(dt_masks_form_t *mask_form)
{
  if(!mask_form) return;
  g_list_free_full(mask_form->points, free);
  mask_form->points = NULL;
  free(mask_form);
}

int dt_masks_events_mouse_leave(struct dt_iop_module_t *module)
{
  return 0;
}

int dt_masks_events_mouse_enter(struct dt_iop_module_t *module)
{
  return 0;
}

gboolean dt_masks_is_anything_selected(const dt_masks_form_gui_t *mask_gui)
{
  return mask_gui->form_selected
          || mask_gui->source_selected
          || mask_gui->seg_selected
          || mask_gui->node_selected
          || mask_gui->handle_selected
          || mask_gui->handle_border_selected;
}

gboolean dt_masks_is_anything_hovered(const dt_masks_form_gui_t *mask_gui)
{
  return mask_gui->node_hovered >= 0
          || mask_gui->handle_hovered >= 0
          || mask_gui->handle_border_hovered >= 0
          || mask_gui->seg_hovered >= 0;
}

static void _set_cursor_shape(dt_masks_form_gui_t *mask_gui)
{
  if(!mask_gui) return;

  // circular arrows
  if(mask_gui->pivot_selected)
    dt_control_set_cursor(GDK_EXCHANGE);
  // pointing hand
  else if(mask_gui->creation_closing_form)
    dt_control_set_cursor(GDK_HAND2);

  /*else if(gui->handle_dragging >= 0)
    dt_control_set_cursor(GDK_HAND1);*/

  // crosshair
  else if(!mask_gui->creation
          && (dt_masks_is_anything_selected(mask_gui)
              || dt_masks_is_anything_hovered(mask_gui)))
    dt_control_set_cursor(GDK_FLEUR);
}

static void _apply_gui_button_pressed_state(dt_masks_form_gui_t *mask_gui, const int button,
                                            const uint32_t state,
                                            const gboolean shape_was_selected)
{
  if(!mask_gui || mask_gui->creation || button != 1) return;
  // Drag is only allowed when this click happens on a shape that was already selected.
  // We still rebuild the fine-grained selection from the current hover target first, so the
  // pressed node/handle/segment becomes the active drag target when dragging is allowed.
  const gboolean prev_form_selected = mask_gui->form_selected;
  const gboolean prev_border_selected = mask_gui->border_selected;
  const gboolean prev_source_selected = mask_gui->source_selected;

  mask_gui->node_selected = FALSE;
  mask_gui->handle_selected = FALSE;
  mask_gui->handle_border_selected = FALSE;
  mask_gui->seg_selected = FALSE;
  mask_gui->form_selected = FALSE;
  mask_gui->border_selected = FALSE;
  mask_gui->source_selected = FALSE;

  if(mask_gui->node_hovered >= 0)
  {
    mask_gui->node_selected = TRUE;
  }
  else if(mask_gui->handle_hovered >= 0)
  {
    mask_gui->handle_selected = TRUE;
  }
  else if(mask_gui->handle_border_hovered >= 0)
  {
    mask_gui->handle_border_selected = TRUE;
  }
  else if(mask_gui->seg_hovered >= 0)
  {
    mask_gui->seg_selected = TRUE;
  }
  else
  {
    mask_gui->form_selected = prev_form_selected;
    mask_gui->border_selected = prev_border_selected;
    mask_gui->source_selected = prev_source_selected;
  }

  if(mask_gui->form_rotating || mask_gui->border_toggling || mask_gui->gradient_toggling) return;
  if(dt_modifier_is(state, GDK_CONTROL_MASK)) return;
  if(!shape_was_selected) return;

  dt_masks_gui_set_dragging(mask_gui);
}

/**
 * @brief Convert the GTK/Cairo widget cursor once for the full mask event chain.
 *
 * The event entry points are the only place where widget-space `x, y` are consumed.
 * Downstream handlers reuse the cached positions:
 * - `mask_gui->rel_pos`: normalized output-image coordinates
 * - `mask_gui->pos`: absolute output-image coordinates
 * - `mask_gui->raw_pos`: absolute raw input-image coordinates
 */
static void _dt_masks_events_set_current_pos(const double x, const double y, dt_masks_form_gui_t *mask_gui)
{
  if(!mask_gui) return;

  float point[2] = { (float)x, (float)y };
  dt_dev_coordinates_widget_to_image_norm(darktable.develop, point, 1);
  mask_gui->rel_pos[0] = point[0];
  mask_gui->rel_pos[1] = point[1];

  dt_dev_coordinates_image_norm_to_image_abs(darktable.develop, point, 1);
  mask_gui->pos[0] = point[0];
  mask_gui->pos[1] = point[1];

  mask_gui->raw_pos[0] = point[0];
  mask_gui->raw_pos[1] = point[1];
  dt_dev_coordinates_image_abs_to_raw_abs(darktable.develop, mask_gui->raw_pos, 1);
}

int dt_masks_events_mouse_moved(struct dt_iop_module_t *module, double x, double y, double pressure, int which)
{
  // This assume that if this event is generated, the mouse is over the center window.
  // record mouse position even if there are no masks visible
  dt_masks_form_gui_t *mask_gui = darktable.develop->form_gui;
  dt_masks_form_t *mask_form = dt_masks_get_visible_form(darktable.develop);
  if(!mask_gui) return 0;
  _dt_masks_events_set_current_pos(x, y, mask_gui);

  // do not process if no forms visible
  if(!mask_form) return 0;

  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  int result = 0;
  if((mask_form->type & DT_MASKS_GROUP) && _dt_masks_events_group_blocks_motion(mask_gui))
  {
    result = 1;
  }
  else
  {
    dt_masks_form_group_t *group_entry = NULL;
    int parent_id = 0;
    int form_index = 0;
    dt_masks_form_t *dispatch_form
        = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);

    if(_dt_masks_events_should_update_hover_on_move(mask_gui))
      _dt_masks_events_update_hover(dispatch_form, mask_gui, form_index);

    if(dispatch_form && dispatch_form->functions && dispatch_form->functions->mouse_moved)
      result = dispatch_form->functions->mouse_moved(module, x, y, pressure, which,
                                                     dispatch_form, parent_id, mask_gui, form_index);
  }

  if(mask_gui)
  {
    _set_hinter_message(mask_gui, mask_form);
    _set_cursor_shape(mask_gui);
  }
  return result;
}

int dt_masks_events_button_released(struct dt_iop_module_t *module, double x, double y, int button,
                                    uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *mask_form = dt_masks_get_visible_form(darktable.develop);
  dt_masks_form_gui_t *mask_gui = darktable.develop->form_gui;
  if(!mask_gui) return 0;
  _dt_masks_events_set_current_pos(x, y, mask_gui);
  if(!mask_form) return 0;

  dt_masks_form_group_t *group_entry = NULL;
  int parent_id = 0;
  int form_index = 0;
  dt_masks_form_t *dispatch_form
      = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);

  int result = 0;
  if(dispatch_form && dispatch_form->functions && dispatch_form->functions->button_released)
    result = dispatch_form->functions->button_released(module, x, y, button,
                                                       state, dispatch_form, parent_id, mask_gui, form_index);

  if(_dt_masks_events_flush_rebuild_if_needed(module, dispatch_form, mask_gui, form_index, button))
    result = 1;

  if(mask_form && (mask_form->type & DT_MASKS_GROUP) && mask_gui)
  {
    const dt_masks_form_group_t *selected_group_entry
        = dt_masks_form_get_selected_group_live(mask_form, mask_gui);
    if(selected_group_entry)
      dt_dev_masks_selection_change(darktable.develop, module,
                                    selected_group_entry->formid, FALSE);
  }

  if(mask_gui && !mask_gui->creation && button == 1)
    dt_masks_gui_reset_dragging(mask_gui);

  if(mask_gui)
  {
    _set_hinter_message(mask_gui, mask_form);
    _set_cursor_shape(mask_gui);
  }

  return result;
}

int dt_masks_events_button_pressed(struct dt_iop_module_t *module, double x, double y, double pressure,
                                   int button, int event_type, uint32_t state)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *mask_form = dt_masks_get_visible_form(darktable.develop);
  dt_masks_form_gui_t *mask_gui = darktable.develop->form_gui;
  if(!mask_gui) return 0;

  _dt_masks_events_set_current_pos(x, y, mask_gui);
  if(!mask_form) return 0;
  const gboolean prev_any_selected = dt_masks_is_anything_selected(mask_gui);
  const int prev_group_selected = mask_gui->group_selected;

  /*DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_MASK_SELECTION_CHANGED, NULL, NULL);*/

  if(mask_form->type & DT_MASKS_GROUP)
    _dt_masks_events_group_update_selection(mask_form, mask_gui);

  dt_masks_form_group_t *group_entry = NULL;
  int parent_id = 0;
  int form_index = 0;
  dt_masks_form_t *dispatch_form
      = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);
  _dt_masks_events_update_hover(dispatch_form, mask_gui, form_index);

  gboolean return_val = FALSE;
  if(dispatch_form && dispatch_form->functions && dispatch_form->functions->button_pressed)
    return_val = dispatch_form->functions->button_pressed(module, x, y, pressure,
                                                          button, event_type, state,
                                                          dispatch_form, parent_id, mask_gui, form_index);

  const gboolean shape_was_selected = (mask_form->type & DT_MASKS_GROUP)
                                          ? (prev_group_selected >= 0 && prev_group_selected == form_index)
                                          : prev_any_selected;
  _apply_gui_button_pressed_state(mask_gui, button, state, shape_was_selected);

  if(button == 3 && !return_val)
  {
    // mouse is over a form or one of its handles/nodes
    if(dispatch_form && (mask_gui->creation
                         || dt_masks_is_anything_hovered(mask_gui)
                         || dt_masks_is_anything_selected(mask_gui)))
    {
      GtkWidget *menu = dt_masks_create_menu(mask_gui, dispatch_form, group_entry,
                                             mask_gui->rel_pos[0], mask_gui->rel_pos[1]);
      gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
      return_val = TRUE;
    }
  }

  return return_val;
}

int dt_masks_events_key_pressed(struct dt_iop_module_t *module, GdkEventKey *event)
{
  dt_masks_form_t *mask_form = dt_masks_get_visible_form(darktable.develop);
  if(!mask_form) return 0;
  dt_masks_form_gui_t *mask_gui = darktable.develop->form_gui;
  if(!mask_gui) return 0;

  gboolean return_value = FALSE;
  if(mask_form->type & DT_MASKS_GROUP)
  {
    dt_masks_form_group_t *group_entry = NULL;
    int parent_id = 0;
    int form_index = 0;
    dt_masks_form_t *dispatch_form
        = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);
    if(dispatch_form && dispatch_form->functions && dispatch_form->functions->key_pressed)
      return_value = dispatch_form->functions->key_pressed(module, event, dispatch_form,
                                                           parent_id, mask_gui, form_index);

    if(!return_value && mask_form->functions->key_pressed)
      return_value = mask_form->functions->key_pressed(module, event, mask_form, 0, mask_gui, 0);
  }
  else if(mask_form->functions->key_pressed)
  {
    return_value = mask_form->functions->key_pressed(module, event, mask_form, 0, mask_gui, 0);
  }
  
  if(!return_value)
  {
    switch(event->keyval)
    {
      case GDK_KEY_Escape:
      {
        return_value = dt_masks_form_cancel_creation(module, mask_gui);
        break;
      }
      case GDK_KEY_Delete:
      {
        if(mask_gui->group_selected >= 0)
        {
          // Delete shape from current group
          dt_masks_form_group_t *selected_group = dt_masks_form_get_selected_group(mask_form, mask_gui);
          if(!selected_group) return 0;
          dt_masks_form_t *selected_form = dt_masks_get_from_id(darktable.develop, selected_group->formid);
          if(selected_form)
            return_value = dt_masks_gui_delete(module, selected_form, mask_gui, selected_group->parentid);
          break;
        }
      }
    }
  }

  return return_value;
}

int dt_masks_events_mouse_scrolled(struct dt_iop_module_t *module, double x, double y,
                                   int scroll_up, uint32_t key_state, int scrolling_delta)
{
  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;

  dt_masks_form_t *mask_form = dt_masks_get_visible_form(darktable.develop);
  dt_masks_form_gui_t *mask_gui = darktable.develop->form_gui;
  if(!mask_gui) return 0;

  _dt_masks_events_set_current_pos(x, y, mask_gui);
  if(!mask_form) return 0;

  int result = 0;
  const gboolean scroll_increases = dt_mask_scroll_increases(scroll_up);

  // we want delta_y to be an absolute scrolling speed
  int scroll_flow = (scrolling_delta < 0) ? -scrolling_delta : scrolling_delta;

  dt_masks_form_group_t *group_entry = NULL;
  int parent_id = 0;
  int form_index = 0;
  dt_masks_form_t *dispatch_form
      = _dt_masks_events_get_dispatch_form(mask_form, mask_gui, &group_entry, &parent_id, &form_index);

  if(!mask_gui->creation && !dt_masks_is_anything_selected(mask_gui))
    return 0;

  _dt_masks_events_update_hover(dispatch_form, mask_gui, form_index);

  if(!mask_gui->creation && !_dt_masks_events_cursor_over_form(dispatch_form, mask_gui, form_index))
    return 0;

  if(dispatch_form && dispatch_form->functions && dispatch_form->functions->mouse_scrolled)
    result = dispatch_form->functions->mouse_scrolled(module, x, y,
                                                      scroll_increases ? 1 : 0, scroll_flow,
                                                      key_state, dispatch_form, parent_id, mask_gui, form_index,
                                                      DT_MASKS_INTERACTION_UNDEF);

  if(mask_gui)
  {
    const gboolean hinted = _set_hinter_message(mask_gui, mask_form);
    dt_print(DT_DEBUG_INPUT,
             "[masks] scroll: ret=%d hinted=%d form=%p type=%d gui=%p group_selected=%d flow=%d state=0x%x\n",
             result, hinted, (void *)mask_form, mask_form ? mask_form->type : -1, (void *)mask_gui,
             mask_gui->group_selected, scroll_flow, key_state);
    if(hinted)
      result = 1;
  }
  return result;
}

gboolean dt_masks_node_is_cusp(const dt_masks_form_gui_points_t *gui_points, const int node_index)
{
  if(!gui_points || !gui_points->points) return FALSE;
  if(gui_points->points_count <= 0 || node_index < 0 || node_index >= gui_points->points_count) return FALSE;

  const float *point_values = &gui_points->points[node_index * 6];
  return (point_values[0 + 2] == point_values[2 + 2]
       && point_values[1 + 2] == point_values[3 + 2]);
}

/**
 * @brief Find the best attachment point on the shape contour for a ray crossing the form
 * 
 * The best point is the one with the smallest positive projection along the ray.
 * The result is offset from the contour by a given distance along the ray axis,
 * oriented toward the center of the ray segment [ray_2, ray_1].
 *  
 * @param ray_1 First point of the ray
 * @param ray_2 Second point of the ray
 * @param points Array of points defining the shape contour
 * @param points_count Number of points in the contour
 * @param first_pt Index of the first point to consider
 * @param is_closed_shape Whether the contour is closed
 * @param result Array to store the resulting attachment point
 */
static void _dt_masks_find_best_attachment_point(const float ray_1[2], const float ray_2[2],
                                                 const float *points, const int points_count, const float zoom_scale,
                                                 const int first_pt,
                                                 const gboolean is_closed_shape,
                                                 float result[2])
{
  // Fallback: no intersection found.
  result[0] = ray_1[0];
  result[1] = ray_1[1];

  const int available_points = points_count - first_pt;
  if(available_points < 2) return;

  const float ray2_x = ray_2[0];
  const float ray2_y = ray_2[1];
  const float ray_center_x = 0.5f * (ray_1[0] + ray_2[0]);
  const float ray_center_y = 0.5f * (ray_1[1] + ray_2[1]);
  const float dir_x = ray_1[0] - ray_2[0];
  const float dir_y = ray_1[1] - ray_2[1];
  float min_s = FLT_MAX;
  const float offset = DT_PIXEL_APPLY_DPI(12.0f) / zoom_scale;
  const float inv_dir_len = f_inv_sqrtf(dir_x * dir_x + dir_y * dir_y);
  const float ux = dir_x * inv_dir_len;
  const float uy = dir_y * inv_dir_len;


  const int segment_count = (available_points - 1) + ((is_closed_shape) ? 1 : 0);
  for(int seg = 0; seg < segment_count; seg++)
  {
    // Get the current segment [i, j], with wrap-around if the shape is closed.
    const int i = first_pt + seg;
    const int j = (i + 1 < points_count) ? (i + 1) : first_pt;
    const float x3 = points[i * 2];
    const float y3 = points[i * 2 + 1];
    const float x4 = points[j * 2];
    const float y4 = points[j * 2 + 1];

    // Compute the intersection of the ray with the segment.
    const float dx = x4 - x3;
    const float dy = y4 - y3;
    const float det = dx * (-dir_y) + dy * dir_x;
    if(det > -1e-8f && det < 1e-8f) continue;
    const float inv_det = 1.0f / det;

    const float segment_param = ((ray2_x - x3) * (-dir_y) + (ray2_y - y3) * dir_x) * inv_det;
    const float ray_param = ((x3 - ray2_x) * dy - (y3 - ray2_y) * dx) * inv_det;
    if(segment_param < 0.0f || segment_param > 1.0f || ray_param <= 0.0f || ray_param >= min_s) continue;

    min_s = ray_param;
    const float ix = ray2_x + ray_param * dir_x;
    const float iy = ray2_y + ray_param * dir_y;

    // Offset along the ray axis toward the ray segment center.
    const float to_center = (ray_center_x - ix) * ux + (ray_center_y - iy) * uy;
    const float side_sign = (to_center >= 0.0f) ? 1.0f : -1.0f;
    result[0] = ix + side_sign * offset * ux;
    result[1] = iy + side_sign * offset * uy;
  }
}

void dt_masks_draw_source(cairo_t *cr, dt_masks_form_gui_t *mask_gui, const int form_index,
                          const int node_count, const float zoom_scale,
                          struct dt_masks_gui_center_point_t *center_point,
                          const shape_draw_function_t *draw_shape_func)
{
  if(!mask_gui) return;
  dt_masks_form_gui_points_t *gui_points
      = (dt_masks_form_gui_points_t *)g_list_nth_data(mask_gui->points, form_index);
  if(!gui_points) return;

  if(!mask_gui->creation)
  {
    const float main[2] = { center_point->main.x, center_point->main.y };
    const float source[2] = { center_point->source.x, center_point->source.y };
    const gboolean is_open_shape = (mask_gui->type & DT_MASKS_IS_OPEN_SHAPE) != 0;
    const gboolean is_closed_shape = (mask_gui->type & DT_MASKS_IS_CLOSED_SHAPE) != 0;

    int first_point_index = 2;
    if((mask_gui->type & DT_MASKS_ELLIPSE) != 0)
      first_point_index = 10;
    else if((mask_gui->type & DT_MASKS_IS_PATH_SHAPE) != 0)
      first_point_index = node_count * 3;

    int attach_points_count = gui_points->points_count;
    int attach_source_count = gui_points->source_count;
    if((mask_gui->type & DT_MASKS_BRUSH) != 0)
    {
      attach_points_count /= 2;
      attach_source_count /= 2;
    }

    float head[2] = { 0.0f, 0.0f };
    float tail[2] = { 0.0f, 0.0f };

    // Find attachment point for the arrow's head with the main shape
    _dt_masks_find_best_attachment_point(main, source, gui_points->points, attach_points_count,
                                         zoom_scale, first_point_index, is_closed_shape, head);

    // Find attachment point for the arrow's base with the source shape
    _dt_masks_find_best_attachment_point(source, main, gui_points->source, attach_source_count,
                                         zoom_scale, first_point_index, is_closed_shape, tail);

    const gboolean selected = (mask_gui->group_selected == form_index)
                              && (mask_gui->source_selected || mask_gui->source_dragging);
    gboolean draw_tail = TRUE;

    // Do not draw the arrow tail if the shape overlapes source,
    // Just draw the head pointing to the center of the source shape
    // and displace it at mid-distance between main and source center.
    // Open shapes always draw the tail since they have not really filled area.
    if(is_closed_shape)
    {
      // From more frequent to least frequent, to get out of loop earlier. Tail first, then source.
      const float pts[4] = { tail[0], tail[1], source[0], source[1] };
      gboolean overlap = (dt_masks_point_in_form_exact(pts, 2, gui_points->points, first_point_index,
                                                       gui_points->points_count) >= 0);
      // Skip the second containment test when overlap is already detected.
      if(!overlap)
      {
        const float origin_pt[2] = { main[0], main[1] };
        overlap = (dt_masks_point_in_form_exact(origin_pt, 1, gui_points->source, first_point_index,
                                                gui_points->source_count) >= 0);
      }

      // Update head position to be between main and source center point.
      if(overlap)
      {
        head[0] = 0.5f * (main[0] + source[0]);
        head[1] = 0.5f * (main[1] + source[1]);
      }

      const float arrow_len_sq = sqf(tail[0] - head[0]) + sqf(tail[1] - head[1]);
      draw_tail = arrow_len_sq > 1e-6f && !overlap;
    }

    // Calculate the angle, so the arrow head always points in the direction of the main shape's center.
    const float angle = is_open_shape ? atan2f(tail[1] - head[1], tail[0] - head[0])
                                      : atan2f(head[1] - main[1], head[0] - main[0]);

    dt_draw_arrow(cr, zoom_scale, selected, draw_tail, DT_MASKS_DASH_ROUND, head, tail, angle);



    
    if(darktable.unmuted & DT_DEBUG_MASKS)
    {
      // Debug: show the main and source gravity points, show head and tail points
      cairo_save(cr);
      cairo_arc(cr, main[0], main[1], DT_PIXEL_APPLY_DPI(4.0f) / zoom_scale, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 1);
      cairo_fill(cr);
      cairo_arc(cr, source[0], source[1], DT_PIXEL_APPLY_DPI(4.0f) / zoom_scale, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 0.0, 1.0, 0.0, 1);
      cairo_fill(cr);

      cairo_arc(cr, head[0], head[1], DT_PIXEL_APPLY_DPI(4.0f) / zoom_scale, 0, 2 * M_PI);
      cairo_set_source_rgba(cr, 0.0, 0.0, 1.0, 1);
      cairo_fill(cr);
      cairo_arc(cr, tail[0], tail[1], DT_PIXEL_APPLY_DPI(4.0f) / zoom_scale, 0, 2 * M_PI);
      cairo_fill(cr);
      cairo_restore(cr);
    }
  }

  // draw the source shape
  // Trick to draw the current polygon shape lines while editing, but draw the complete shape in all other cases
  const int rendered_node_count = node_count + !mask_gui->creation;
  const gboolean shape_selected = (mask_gui->group_selected == form_index)
                                  && (mask_gui->form_selected || mask_gui->form_dragging);

  dt_draw_source_shape(cr, zoom_scale, shape_selected, gui_points->source, gui_points->source_count,
                       rendered_node_count, draw_shape_func);
  
}

void dt_masks_draw_path_seg_by_seg(cairo_t *cr, dt_masks_form_gui_t *mask_gui, const int form_index,
                                   const float *points, const int points_count, const int node_count,
                                   const float zoom_scale)
{
  if(!cr || !points || !mask_gui) return;
  if(node_count <= 0 || points_count <= node_count * 3 + 6) return;

  const int total_coords = points_count * 2;
  if(total_coords <= (node_count * 6 + 1)) return;

  const gboolean group_selected = (mask_gui->group_selected == form_index);

  int show_segment_index = 1;
  int current_segment_index = 0;
  cairo_move_to(cr, points[node_count * 6], points[node_count * 6 + 1]);

  for(int point_index = node_count * 3; point_index < points_count; point_index++)
  {
    const int coord_index = point_index * 2;
    if((coord_index + 1) >= total_coords) break;

    const double coord_x = points[coord_index];
    const double coord_y = points[coord_index + 1];
    cairo_line_to(cr, coord_x, coord_y);

    const int segment_coord_index = show_segment_index * 6;
    if((segment_coord_index + 3) >= total_coords) continue;

    const double segment_x = points[segment_coord_index + 2];
    const double segment_y = points[segment_coord_index + 3];
    if(coord_x == segment_x && coord_y == segment_y)
    {
      const gboolean seg_is_selected = group_selected
                                       && (dt_masks_gui_selected_segment_index(mask_gui)
                                           == current_segment_index);
      const gboolean all_selected = group_selected
                                    && dt_masks_is_anything_selected(mask_gui)
                                    && (mask_gui->form_selected || mask_gui->form_dragging);

      if(mask_gui->creation && current_segment_index == node_count - 2)
        dt_draw_stroke_line(DT_MASKS_DASH_ROUND, FALSE, cr, all_selected, zoom_scale, CAIRO_LINE_CAP_ROUND);
      else
        dt_draw_stroke_line(DT_MASKS_NO_DASH, FALSE, cr, (seg_is_selected || all_selected), zoom_scale,
                            CAIRO_LINE_CAP_BUTT);

      show_segment_index = (show_segment_index + 1) % node_count;
      current_segment_index++;
    }

    if(mask_gui->creation && current_segment_index >= node_count - 1) break;
  } 
}

void dt_masks_events_post_expose(struct dt_iop_module_t *module, cairo_t *cr, int32_t width, int32_t height,
                                 int32_t pointerx, int32_t pointery)
{
  dt_develop_t *develop = darktable.develop;
  if(!develop) return;
  dt_masks_form_t *mask_form = dt_masks_get_visible_form(develop);
  dt_masks_form_gui_t *mask_gui = develop->form_gui;
  if(!mask_gui) return;
  if(!mask_form) return;

  int buffer_width = 0;
  int buffer_height = 0;
  dt_dev_get_processed_size(develop, &buffer_width, &buffer_height);

  if(buffer_width < 1.0 || buffer_height < 1.0) return;
  const float zoom_scale = dt_dev_get_zoom_level(develop);

  // Create a surface to draw the mask, so that we can apply
  // operation that does not affect the main context
  cairo_surface_t *overlay = NULL;
  cairo_t *mask_draw = NULL;
  cairo_surface_t *target = cairo_get_target(cr);
  double sx = 1.0, sy = 1.0;
  cairo_surface_get_device_scale(target, &sx, &sy);
  overlay = cairo_surface_create_similar(target, CAIRO_CONTENT_COLOR_ALPHA, (int)ceil(width * sx),
                                         (int)ceil(height * sy));
  cairo_surface_set_device_scale(overlay, sx, sy);
  mask_draw = cairo_create(overlay);

  // Apply the same transformation to the mask drawing context
  /*cairo_matrix_t m;
  cairo_get_matrix(cr, &m);
  cairo_set_matrix(mask_draw, &m);*/
  
  cairo_save(mask_draw);

  // We rescale to input space
  if(dt_dev_rescale_roi_to_input(develop, mask_draw, width, height))
  {
    cairo_restore(mask_draw);
    cairo_destroy(mask_draw);
    cairo_surface_destroy(overlay);
    return;
  }

  // We update the form if needed
  // Add preview when creating a circle, ellipse and gradient
  if(!((mask_form->type & DT_MASKS_IS_PRIMITIVE_SHAPE) && mask_gui->creation))
    dt_masks_gui_form_test_create(mask_form, mask_gui, module);

  // Draw form
  if(mask_form->type & DT_MASKS_GROUP)
    dt_group_events_post_expose(mask_draw, zoom_scale, mask_form, mask_gui);
  else if(mask_form->functions && mask_form->functions->post_expose)
  {
    const guint point_count = g_list_length(mask_form->points);
    mask_gui->type = mask_form->type;
    mask_form->functions->post_expose(mask_draw, zoom_scale, mask_gui, 0, point_count);
  }
  cairo_restore(mask_draw);

  // Draw the overlay with the same transformation as the main context
  cairo_save(cr);
  cairo_identity_matrix(cr);
  cairo_set_source_surface(cr, overlay, 0.0, 0.0);
  cairo_paint(cr);
  cairo_restore(cr);

  cairo_destroy(mask_draw);
  cairo_surface_destroy(overlay);
}

void dt_masks_clear_form_gui(dt_develop_t *develop)
{
  if(!develop->form_gui) return;
  g_list_free_full(develop->form_gui->points, dt_masks_form_gui_points_free);
  develop->form_gui->points = NULL;
  dt_masks_dynbuf_free(develop->form_gui->guipoints);
  develop->form_gui->guipoints = NULL;
  dt_masks_dynbuf_free(develop->form_gui->guipoints_payload);
  develop->form_gui->guipoints_payload = NULL;
  develop->form_gui->guipoints_count = 0;
  develop->form_gui->pipe_hash = develop->form_gui->formid = 0;
  develop->form_gui->delta[0] = develop->form_gui->delta[1] = 0.0f;
  develop->form_gui->scrollx = develop->form_gui->scrolly = 0.0f;
  develop->form_gui->form_selected = develop->form_gui->border_selected = develop->form_gui->form_dragging
      = develop->form_gui->form_rotating = develop->form_gui->border_toggling = develop->form_gui->gradient_toggling = FALSE;
  develop->form_gui->source_selected = develop->form_gui->source_dragging = FALSE;
  develop->form_gui->pivot_selected = FALSE;
  develop->form_gui->node_hovered = -1;
  develop->form_gui->handle_hovered = -1;
  develop->form_gui->seg_hovered = -1;
  develop->form_gui->handle_border_hovered = -1;
  develop->form_gui->node_selected = FALSE;
  develop->form_gui->handle_selected = FALSE;
  develop->form_gui->seg_selected = FALSE;
  develop->form_gui->handle_border_selected = FALSE;
  develop->form_gui->handle_border_dragging = develop->form_gui->seg_dragging = develop->form_gui->handle_dragging
      = develop->form_gui->node_dragging = -1;
  develop->form_gui->creation_closing_form = develop->form_gui->creation = FALSE;
  develop->form_gui->pressure_sensitivity = DT_MASKS_PRESSURE_OFF;
  develop->form_gui->creation_module = NULL;
  develop->form_gui->node_selected = FALSE;

  develop->form_gui->group_selected = -1;
  develop->form_gui->group_selected = -1;
  develop->form_gui->edit_mode = DT_MASKS_EDIT_OFF;
  develop->form_gui->last_rebuild_ts = 0.0;
  develop->form_gui->last_rebuild_pos[0] = develop->form_gui->last_rebuild_pos[1] = 0.0f;
  develop->form_gui->rebuild_pending = FALSE;
  develop->form_gui->last_hit_test_pos[0] = develop->form_gui->last_hit_test_pos[1] = -1.0f;
  // allow to select a shape inside an iop
  dt_masks_select_form(NULL, NULL);
}

void dt_masks_change_form_gui(dt_masks_form_t *new_form)
{
  dt_masks_clear_form_gui(darktable.develop);
  dt_masks_set_visible_form(darktable.develop, new_form);
}

void dt_masks_reset_form_gui(void)
{
  dt_masks_change_form_gui(NULL);
  dt_iop_module_t *module = darktable.develop->gui_module;
  if(module && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(module->flags() & IOP_FLAGS_NO_MASKS)
    && module->blend_data)
  {
    dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->blend_data;
    blend_data->masks_shown = DT_MASKS_EDIT_OFF;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_edit), 0);
    for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_shapes[n]), 0);
  }
}

void dt_masks_reset_show_masks_icons(void)
{
  for(GList *module_node = darktable.develop->iop; module_node; module_node = g_list_next(module_node))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)module_node->data;
    if(module && (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) && !(module->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->blend_data;
      if(!blend_data) break;  // TODO: this doesn't look right. Why do we break the while look as soon as one module has no blend_data?
      blend_data->masks_shown = DT_MASKS_EDIT_OFF;
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_edit), FALSE);
      gtk_widget_queue_draw(blend_data->masks_edit);
      for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
      {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_shapes[n]), 0);
        gtk_widget_queue_draw(blend_data->masks_shapes[n]);
      }
    }
  }
}

dt_masks_edit_mode_t dt_masks_get_edit_mode(struct dt_iop_module_t *module)
{
  return darktable.develop->form_gui
    ? darktable.develop->form_gui->edit_mode
    : DT_MASKS_EDIT_OFF;
}

void dt_masks_set_edit_mode(struct dt_iop_module_t *module, dt_masks_edit_mode_t value)
{
  if(!module) return;
  dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->blend_data;
  if(!blend_data) return;

  dt_masks_form_t *group_form = NULL;
  dt_masks_form_t *mask_form = dt_masks_get_from_id(module->dev, module->blend_params->mask_id);
  if(value && mask_form)
  {
    group_form = dt_masks_create_ext(DT_MASKS_GROUP);
    group_form->formid = 0;
    dt_masks_group_ungroup(group_form, mask_form);
  }

  if(blend_data) blend_data->masks_shown = value;

  dt_masks_change_form_gui(group_form);
  darktable.develop->form_gui->edit_mode = value;
  if(value && mask_form)
    dt_dev_masks_selection_change(darktable.develop, NULL, mask_form->formid, FALSE);
  else
    dt_dev_masks_selection_change(darktable.develop, NULL, 0, FALSE);

  if(blend_data->masks_support)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(blend_data->masks_edit),
                                 value == DT_MASKS_EDIT_OFF ? FALSE : TRUE);

  dt_control_queue_redraw_center();
}

static void _menu_no_masks(struct dt_iop_module_t *module)
{
  // we drop all the forms in the iop
  dt_masks_form_t *group_form = _group_from_module(darktable.develop, module);
  if(group_form) dt_masks_form_remove(module, NULL, group_form);
  module->blend_params->mask_id = 0;

  // and we update the iop
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_OFF);
  dt_masks_iop_update(module);
}

static void _menu_add_shape(struct dt_iop_module_t *module, dt_masks_type_t type)
{
  dt_masks_creation_mode(module, type);
}

static void _menu_add_exist(dt_iop_module_t *module, int form_id)
{
  if(!module) return;
  dt_masks_form_t *mask_form = dt_masks_get_from_id(darktable.develop, form_id);
  if(!mask_form) return;

  // is there already a masks group for this module ?
  dt_masks_form_t *group_form = _group_from_module(darktable.develop, module);
  if(!group_form)
  {
    group_form = _group_create(darktable.develop, module, DT_MASKS_GROUP);
  }
  // we add the form in this group
  dt_masks_group_add_form(group_form, mask_form);
  // we save the group
  // and we ensure that we are in edit mode

  dt_masks_iop_update(module);
  dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
}

void dt_masks_group_update_name(dt_iop_module_t *module)
{
  dt_masks_form_t *group_form = _group_from_module(darktable.develop, module);
  if (!group_form)
    return;

  _set_group_name_from_module(module, group_form);

  dt_masks_iop_update(module);
}

void dt_masks_iop_use_same_as(dt_iop_module_t *module, dt_iop_module_t *source_module)
{
  if(!module || !source_module) return;

  // we get the source group
  int source_id = source_module->blend_params->mask_id;
  dt_masks_form_t *source_group = dt_masks_get_from_id(darktable.develop, source_id);
  if(!source_group || source_group->type != DT_MASKS_GROUP) return;

  // is there already a masks group for this module ?
  dt_masks_form_t *group_form = _group_from_module(darktable.develop, module);
  if(!group_form)
  {
    group_form = _group_create(darktable.develop, module, DT_MASKS_GROUP);
  }
  // we copy the src group in this group
  for(GList *group_node = source_group->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    dt_masks_form_t *mask_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);
    if(mask_form)
    {
      dt_masks_form_group_t *new_entry = dt_masks_group_add_form(group_form, mask_form);
      if(new_entry)
      {
        new_entry->state = group_entry->state;
        new_entry->opacity = group_entry->opacity;
      }
    }
  }

  // we save the group

}

void dt_masks_iop_combo_populate(GtkWidget *widget, void *data)
{
  // we ensure that the module has focus
  dt_iop_module_t *module = (dt_iop_module_t *)data;
  dt_iop_request_focus(module);
  dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->blend_data;

  // we determine a higher approx of the entry number
  const guint forms_count = g_list_length(module->dev->forms);
  const guint iop_count = g_list_length(module->dev->iop);
  guint combo_capacity = 5 + forms_count + iop_count;
  free(blend_data->masks_combo_ids);
  blend_data->masks_combo_ids = malloc(sizeof(int) * combo_capacity);

  int *combo_ids = blend_data->masks_combo_ids;
  GtkWidget *combo = blend_data->masks_combo;

  // we remove all the combo entries except the first one
  while(dt_bauhaus_combobox_length(combo) > 1)
  {
    dt_bauhaus_combobox_remove_at(combo, 1);
  }

  int combo_index = 0;
  combo_ids[combo_index] = 0; // nothing to do for the first entry (already here)
  combo_index++;

  // add existing shapes
  dt_pthread_rwlock_rdlock(&module->dev->masks_mutex);
  for(GList *form_node = module->dev->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)form_node->data;
    if((mask_form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
       || mask_form->formid == module->blend_params->mask_id)
    {
      continue;
    }

    // we search were this form is used in the current module
    int is_used = 0;
    dt_masks_form_t *group_form = _group_from_module(module->dev, module);
    if(group_form && (group_form->type & DT_MASKS_GROUP))
    {
      for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
      {
        dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
        if(group_entry->formid == mask_form->formid)
        {
          is_used = 1;
          break;
        }
      }
    }
    if(!is_used)
    {
      dt_bauhaus_combobox_add(combo, mask_form->name);
      combo_ids[combo_index] = mask_form->formid;
      combo_index++;
    }
  }
  dt_pthread_rwlock_unlock(&module->dev->masks_mutex);

  // masks from other iops
  int iop_index = 1;
  for(GList *module_node = module->dev->iop; module_node; module_node = g_list_next(module_node))
  {
    dt_iop_module_t *other_module = (dt_iop_module_t *)module_node->data;
    if((other_module != module) && (other_module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
       && !(other_module->flags() & IOP_FLAGS_NO_MASKS))
    {
      dt_masks_form_t *group_form = _group_from_module(darktable.develop, other_module);
      if(group_form)
      {
        gchar *module_label = dt_history_item_get_name(other_module);
        dt_bauhaus_combobox_add(combo, g_strdup_printf(_("reuse shapes from %s"), module_label));
        g_free(module_label);
        combo_ids[combo_index] = -1 * iop_index;
        combo_index++;
      }
    }
    iop_index++;
  }
}

void dt_masks_iop_value_changed_callback(GtkWidget *widget, struct dt_iop_module_t *module)
{
  // we get the corresponding value
  dt_iop_gui_blend_data_t *blend_data = (dt_iop_gui_blend_data_t *)module->blend_data;

  int selection_index = dt_bauhaus_combobox_get(blend_data->masks_combo);
  if(selection_index == 0) return;
  if(selection_index > 0)
  {
    int selection_value = blend_data->masks_combo_ids[selection_index];
    const guint iop_count = g_list_length(module->dev->iop);
    // FIXME : these values should use binary enums
    if(selection_value == -1000000)
    {
      // delete all masks
      _menu_no_masks(module);
    }
    else if(selection_value == -2000001)
    {
      // add a circle shape
      _menu_add_shape(module, DT_MASKS_CIRCLE);
    }
    else if(selection_value == -2000002)
    {
      // add a path shape
      _menu_add_shape(module, DT_MASKS_POLYGON);
    }
    else if(selection_value == -2000016)
    {
      // add a gradient shape
      _menu_add_shape(module, DT_MASKS_GRADIENT);
    }
    else if(selection_value == -2000032)
    {
      // add a gradient shape
      _menu_add_shape(module, DT_MASKS_ELLIPSE);
    }
    else if(selection_value == -2000064)
    {
      // add a brush shape
      _menu_add_shape(module, DT_MASKS_BRUSH);
    }
    else if(selection_value < 0)
    {
      // use same shapes as another iop
      selection_value = -1 * selection_value - 1;
      if(selection_value < (int)iop_count)
      {
        dt_iop_module_t *source_module
            = (dt_iop_module_t *)g_list_nth_data(module->dev->iop, selection_value);
        dt_masks_iop_use_same_as(module, source_module);
        // and we ensure that we are in edit mode
        //

        dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      }
    }
    else if(selection_value > 0)
    {
      // add an existing shape
      _menu_add_exist(module, selection_value);
    }
    else
      return;
  }
  // we update the combo line
  dt_masks_iop_update(module);
  dt_dev_add_history_item(module->dev, module, TRUE, TRUE);
}

void dt_masks_form_remove(struct dt_iop_module_t *module, dt_masks_form_t *group_form,
                          dt_masks_form_t *mask_form)
{
  if(!mask_form) return;
  int form_id = mask_form->formid;
  if(group_form && !(group_form->type & DT_MASKS_GROUP)) return;

  if(!(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE)) && group_form)
  {
    // we try to remove the form from the masks group
    int removed = 0;
    for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
      if(group_entry->formid == form_id)
      {
        removed = 1;
        group_form->points = g_list_remove(group_form->points, group_entry);
        free(group_entry);
        break;
      }
    }
    if(removed)
    if(removed && module)
    {
      dt_masks_iop_update(module);

    }
    if(removed) dt_masks_form_update_gravity_center(group_form);
    if(removed && group_form->points == NULL) dt_masks_form_remove(module, NULL, group_form);
    return;
  }

  if(mask_form->type & DT_MASKS_GROUP && mask_form->type & DT_MASKS_CLONE)
  {
    // when removing a cloning group the children have to be removed, too, as they won't be shown in the mask manager
    // and are thus not accessible afterwards.
    while(mask_form->points)
    {
      dt_masks_form_group_t *group_child = (dt_masks_form_group_t *)mask_form->points->data;
      dt_masks_form_t *child = dt_masks_get_from_id(darktable.develop, group_child->formid);
      dt_masks_form_remove(module, mask_form, child);
      // no need to do anything to mask_form->points, the recursive call will have removed child from the list
    }
  }

  // if we are here that mean we have to permanently delete this form
  // we drop the form from all modules
  for(GList *iop_node = darktable.develop->iop; iop_node; iop_node = g_list_next(iop_node))
  {
    dt_iop_module_t *iop_module = (dt_iop_module_t *)iop_node->data;
    if(iop_module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
    {
      // is the form the base group of the iop ?
      if(form_id == iop_module->blend_params->mask_id)
      {
        iop_module->blend_params->mask_id = 0;
        dt_masks_iop_update(iop_module);
      }
      else
      {
        dt_masks_form_t *iop_group = _group_from_module(darktable.develop, iop_module);
        if(iop_group && (iop_group->type & DT_MASKS_GROUP))
        {
          int removed = 0;
          GList *group_node = iop_group->points;
          while(group_node)
          {
            dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
            if(group_entry->formid == form_id)
            {
              removed = 1;
              iop_group->points = g_list_remove(iop_group->points, group_entry);
              free(group_entry);
              group_node = iop_group->points; // jump back to start of list
              continue;
            }
            group_node = g_list_next(group_node); // advance to next form
          }
          if(removed)
          {
            dt_masks_iop_update(iop_module);

            if(iop_group->points == NULL) dt_masks_form_remove(iop_module, NULL, iop_group);
          }
        }
      }
    }
  }
  // we drop the form from the general list
  for(GList *form_node = darktable.develop->forms; form_node; form_node = g_list_next(form_node))
  {
    dt_masks_form_t *existing_form = (dt_masks_form_t *)form_node->data;
    if(existing_form->formid == form_id)
    {
      dt_masks_remove_form(darktable.develop, existing_form);
      break;
    }
  }
}

float dt_masks_form_get_interaction_value(dt_masks_form_group_t *form_group,
                                          dt_masks_interaction_t interaction)
{
  if(!form_group) return NAN;

  if(interaction == DT_MASKS_INTERACTION_OPACITY)
  {
    return form_group->opacity;
  }

  dt_masks_form_t *target_form = dt_masks_get_from_id(darktable.develop, form_group->formid);
  if(!target_form || !target_form->functions || !target_form->functions->get_interaction_value) return NAN;

  return target_form->functions->get_interaction_value(target_form, interaction);
}

gboolean dt_masks_form_get_gravity_center(const dt_masks_form_t *mask_form, float center[2], float *area)
{
  center[0] = 0.0f;
  center[1] = 0.0f;
  if(area) *area = 0.0f;

  if(!mask_form || !mask_form->functions || !mask_form->functions->get_gravity_center || !center) return FALSE;
  return mask_form->functions->get_gravity_center(mask_form, center, area);
}

void dt_masks_form_update_gravity_center(dt_masks_form_t *mask_form)
{
  if(!mask_form) return;

  float center_point[2];
  float area = 0.0f;
  const gboolean ok = dt_masks_form_get_gravity_center(mask_form, center_point, &area);
  mask_form->gravity_center[0] = center_point[0];
  mask_form->gravity_center[1] = center_point[1];
  mask_form->area = area;

  dt_print(DT_DEBUG_MASKS,
           "[masks] gravity center updated: form=%p id=%d type=0x%x ok=%d center=(%f,%f), area=%f\n",
           (void *)mask_form, mask_form->formid, mask_form->type, ok,
           mask_form->gravity_center[0], mask_form->gravity_center[1], mask_form->area);
}

static float _change_opacity(dt_masks_form_group_t *form_group, float value,
                             const dt_masks_increment_t increment, const int flow)
{
  if(!form_group) return 0;

  form_group->opacity = CLAMPF(dt_masks_apply_increment(form_group->opacity, value, increment, flow), 0.0f, 1.0f);
  dt_toast_log(_("Opacity: %3.2f%%"), form_group->opacity * 100.f);
  return form_group->opacity;
}

float dt_masks_form_set_interaction_value(dt_masks_form_group_t *form_group,
                                          dt_masks_interaction_t interaction,
                                          float value, dt_masks_increment_t increment, int flow,
                                          dt_masks_form_gui_t *mask_gui, dt_iop_module_t *module)
{
  if(!form_group) return NAN;

  if(interaction == DT_MASKS_INTERACTION_OPACITY)
  {
    const float result = _change_opacity(form_group, value, increment, flow);
    dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
    return result;
  }

  dt_masks_form_t *target_form = dt_masks_get_from_id(darktable.develop, form_group->formid);
  if(!target_form || !target_form->functions
     || !target_form->functions->set_interaction_value) return NAN;

  const float result = target_form->functions->set_interaction_value(target_form, interaction, value, increment,
                                                                     flow, mask_gui, module);
  if(isnan(result)) return NAN;
  dt_masks_form_update_gravity_center(target_form);
  dt_dev_add_history_item(darktable.develop, module, TRUE, TRUE);
  return result;
}

const char * _get_mask_plugin(dt_masks_form_t *mask_form)
{
  // Internal masks are used by spots removal and retouch modules
  if(mask_form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
    return "spots";
  // Regular all-purpose masks
  else
    return "masks";
}

const char * _get_mask_type(dt_masks_form_t *mask_form)
{
  // warning: mask types or not int enum but bit flags ?!?
  // that's a shitty design that prevents us from doing a clean switch case over the enum.
  // why would we overlap mask types ?!?
  if(mask_form->type & DT_MASKS_CIRCLE)
    return "circle";
  else if(mask_form->type & DT_MASKS_POLYGON)
    return "polygon";
  else if(mask_form->type & DT_MASKS_ELLIPSE)
    return "ellipse";
  else if(mask_form->type & DT_MASKS_GRADIENT)
    return "gradient";
  else if(mask_form->type & DT_MASKS_BRUSH)
    return "brush";
  else
    return "unknown";
}

float dt_masks_apply_increment(float current, float amount, dt_masks_increment_t increment, int flow)
{
  switch(increment)
  {
    case DT_MASKS_INCREMENT_SCALE:
      return current * powf(amount, (float)flow);
    case DT_MASKS_INCREMENT_OFFSET:
      return current + amount * (float)flow;
    case DT_MASKS_INCREMENT_ABSOLUTE:
    default:
      return amount;
  }
}

float dt_masks_apply_increment_precomputed(float current, float amount, float scale_amount, float offset_amount,
                                           dt_masks_increment_t increment)
{
  switch(increment)
  {
    case DT_MASKS_INCREMENT_SCALE:
      return current * scale_amount;
    case DT_MASKS_INCREMENT_OFFSET:
      return current + offset_amount;
    case DT_MASKS_INCREMENT_ABSOLUTE:
    default:
      return amount;
  }
}

float dt_masks_get_set_conf_value(dt_masks_form_t *mask_form, char *feature, float new_value,
                                  float value_min, float value_max,
                                  dt_masks_increment_t increment, int flow)
{
  gchar *config_key = NULL;
  if(!strcmp(feature, "opacity"))
    config_key = g_strdup_printf("plugins/darkroom/%s_opacity", _get_mask_plugin(mask_form));
  else
    config_key = g_strdup_printf("plugins/darkroom/%s/%s/%s",
                                 _get_mask_plugin(mask_form), _get_mask_type(mask_form), feature);

  if(!g_strcmp0(feature, "rotation")) flow = (flow > 1) ? (flow - 1) * 5 : flow;

  const float current_value = dt_conf_get_float(config_key);
  float updated_value = dt_masks_apply_increment(current_value, new_value, increment, flow);
  if(!g_strcmp0(feature, "rotation"))
  {
    // Ensure the rotation value stays within the interval [min, max)
    if(updated_value > value_max) updated_value = fmodf(updated_value, value_max);
    else if(updated_value < value_min)
      updated_value = value_max - fmodf(value_min - updated_value, value_max);
  }
  else updated_value = MAX(value_min, MIN(updated_value, value_max));

  dt_conf_set_float(config_key, updated_value);

  g_free(config_key);
  return updated_value;
}

float dt_masks_get_set_conf_value_with_toast(dt_masks_form_t *mask_form, const char *feature, float amount,
                                             float value_min, float value_max,
                                             dt_masks_increment_t increment, int flow,
                                             const char *toast_fmt, float toast_scale)
{
  float value = dt_masks_get_set_conf_value(mask_form, (char *)feature, amount,
                                            value_min, value_max, increment, flow);
  if(toast_fmt && toast_fmt[0] != '\0')
    dt_toast_log(toast_fmt, value * toast_scale);
  return value;
}

void dt_masks_duplicate_points(const dt_masks_form_t *base_form, dt_masks_form_t *dest_form,
                               size_t node_size)
{
  if(!base_form || !dest_form || !base_form->points || node_size == 0) return;

  for(const GList *point_node = base_form->points; point_node; point_node = g_list_next(point_node))
  {
    const void *point_data = point_node->data;
    if(!point_data) continue;
    void *point_copy = malloc(node_size);
    if(!point_copy) continue;
    memcpy(point_copy, point_data, node_size);
    dest_form->points = g_list_append(dest_form->points, point_copy);
  }
}

int dt_masks_form_change_opacity(dt_masks_form_t *mask_form, int parent_id, int scroll_up,
                                 const int flow)
{
  if(!mask_form) return 0;
  dt_masks_form_group_t *form_group = dt_masks_form_group_from_parentid(parent_id, mask_form->formid);
  if(!form_group) return 0;

  float amount = scroll_up ? 0.02f : -0.02f;
  const float changed = dt_masks_form_set_interaction_value(form_group, DT_MASKS_INTERACTION_OPACITY,
                                                            amount, DT_MASKS_INCREMENT_OFFSET, flow, NULL, NULL);
  return !isnan(changed);
}

void dt_masks_form_move(dt_masks_form_t *group_form, int form_id, int move_up)
{
  if(!group_form || !(group_form->type & DT_MASKS_GROUP)) return;

  // we search the form in the group
  dt_masks_form_group_t *group_entry = NULL;
  guint group_index = 0;
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *entry = (dt_masks_form_group_t *)group_node->data;
    if(entry->formid == form_id)
    {
      group_entry = entry;
      break;
    }
    group_index++;
  }

  // we remove the form and read it
  if(group_entry)
  {
    const guint group_length = g_list_length(group_form->points);
    if(!move_up && group_index == 0) return;
    if(move_up && group_index == group_length - 1) return;

    group_form->points = g_list_remove(group_form->points, group_entry);
    if(!move_up)
      group_index -= 1;
    else
      group_index += 1;
    group_form->points = g_list_insert(group_form->points, group_entry, group_index);

  }
}

static int _find_in_group(dt_masks_form_t *group_form, int form_id)
{
  if(!(group_form->type & DT_MASKS_GROUP)) return 0;
  if(group_form->formid == form_id) return 1;
  int nested_count = 0;
  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    const dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    dt_masks_form_t *mask_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);
    if(mask_form)
    {
      if(mask_form->type & DT_MASKS_GROUP) nested_count += _find_in_group(mask_form, form_id);
    }
  }
  return nested_count;
}

dt_masks_form_group_t *dt_masks_group_add_form(dt_masks_form_t *group_form, dt_masks_form_t *mask_form)
{
  // add a form to group and check for self inclusion

  if(!(group_form->type & DT_MASKS_GROUP)) return NULL;
  // either the form to add is not a group, so no risk
  // or we go through all points of form to see if we find a ref to grp->formid
  if(!(mask_form->type & DT_MASKS_GROUP) || _find_in_group(mask_form, group_form->formid) == 0)
  {
    dt_masks_form_group_t *group_entry = malloc(sizeof(dt_masks_form_group_t));
    group_entry->formid = mask_form->formid;
    group_entry->parentid = group_form->formid;
    group_entry->state = DT_MASKS_STATE_SHOW | DT_MASKS_STATE_USE | DT_MASKS_STATE_UNION;
    group_entry->opacity = dt_conf_get_float("plugins/darkroom/masks/opacity");
    group_form->points = g_list_append(group_form->points, group_entry);
    dt_masks_form_update_gravity_center(group_form);
    return group_entry;
  }

  dt_control_log(_("masks can not contain themselves"));
  return NULL;
}

void dt_masks_group_ungroup(dt_masks_form_t *dest_group, dt_masks_form_t *group_form)
{
  if(!group_form || !dest_group) return;
  if(!(group_form->type & DT_MASKS_GROUP) || !(dest_group->type & DT_MASKS_GROUP)) return;

  for(GList *group_node = group_form->points; group_node; group_node = g_list_next(group_node))
  {
    dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
    dt_masks_form_t *mask_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);
    if(mask_form)
    {
      if(mask_form->type & DT_MASKS_GROUP)
      {
        dt_masks_group_ungroup(dest_group, mask_form);
      }
      else
      {
        dt_masks_form_group_t *new_entry = (dt_masks_form_group_t *)malloc(sizeof(dt_masks_form_group_t));
        new_entry->formid = group_entry->formid;
        new_entry->parentid = group_entry->parentid;
        new_entry->state = group_entry->state;
        new_entry->opacity = group_entry->opacity;
        dest_group->points = g_list_append(dest_group->points, new_entry);
      }
    }
  }

  dt_masks_form_update_gravity_center(dest_group);
}

uint64_t dt_masks_group_get_hash(uint64_t hash, dt_masks_form_t *mask_form)
{
  if(!mask_form) return hash;

  // basic infos
  hash = dt_hash(hash, (char *)&mask_form->type, sizeof(dt_masks_type_t));
  hash = dt_hash(hash, (char *)&mask_form->formid, sizeof(int));
  hash = dt_hash(hash, (char *)&mask_form->version, sizeof(int));
  hash = dt_hash(hash, (char *)&mask_form->source, sizeof(float) * 2);

  for(const GList *point_node = mask_form->points; point_node; point_node = g_list_next(point_node))
  {
    if(mask_form->type & DT_MASKS_GROUP)
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)point_node->data;
      dt_masks_form_t *child_form = dt_masks_get_from_id(darktable.develop, group_entry->formid);
      if(child_form)
      {
        // state & opacity
        hash = dt_hash(hash, (char *)&group_entry->state, sizeof(int));
        hash = dt_hash(hash, (char *)&group_entry->opacity, sizeof(float));

        // the form itself
        hash = dt_masks_group_get_hash(hash, child_form);
      }
    }
    else if(mask_form->functions)
    {
      hash = dt_hash(hash, (char *)point_node->data, mask_form->functions->point_struct_size);
    }
  }
  return hash;
}

// adds formid to used array
// if formid is a group it adds all the forms that belongs to that group
static void _cleanup_unused_recurs(GList *form_list, int form_id, int *used_form_ids, int used_count)
{
  // first, we search for the formid in used table
  for(int used_index = 0; used_index < used_count; used_index++)
  {
    if(used_form_ids[used_index] == 0)
    {
      // we store the formid
      used_form_ids[used_index] = form_id;
      break;
    }
    if(used_form_ids[used_index] == form_id) break;
  }

  // if the form is a group, we iterate through the sub-forms
  dt_masks_form_t *mask_form = dt_masks_get_from_id_ext(form_list, form_id);
  if(mask_form && (mask_form->type & DT_MASKS_GROUP))
  {
    for(GList *group_node = mask_form->points; group_node; group_node = g_list_next(group_node))
    {
      dt_masks_form_group_t *group_entry = (dt_masks_form_group_t *)group_node->data;
      _cleanup_unused_recurs(form_list, group_entry->formid, used_form_ids, used_count);
    }
  }
}

// removes from _forms all forms that are not used in history_list up to history_end
static int _masks_cleanup_unused(GList **forms_list, GList *history_list, const int history_end)
{
  int masks_removed = 0;
  GList *forms = *forms_list;

  // we create a table to store the ids of used forms
  guint form_count = g_list_length(forms);
  int *used_form_ids = calloc(form_count, sizeof(int));

  // check in history if the module has drawn masks and add it to used array
  int history_index = 0;
  for(GList *history_node = history_list; history_node && history_index < history_end;
      history_node = g_list_next(history_node))
  {
    dt_dev_history_item_t *history_item = (dt_dev_history_item_t *)history_node->data;
    dt_develop_blend_params_t *blend_params = history_item->blend_params;
    if(blend_params)
    {
      if(blend_params->mask_id > 0)
        _cleanup_unused_recurs(forms, blend_params->mask_id, used_form_ids, form_count);
    }
    history_index++;
  }

  // and we delete all unused forms
  GList *shape_node = forms;
  while(shape_node)
  {
    dt_masks_form_t *mask_form = (dt_masks_form_t *)shape_node->data;
    int is_used = 0;
    for(int used_index = 0; used_index < form_count; used_index++)
    {
      if(used_form_ids[used_index] == mask_form->formid)
      {
        is_used = 1;
        break;
      }
      if(used_form_ids[used_index] == 0) break;
    }

    shape_node = g_list_next(shape_node); // need to get 'next' now, because we may be removing the current node

    if(is_used == 0)
    {
      forms = g_list_remove(forms, mask_form);
      // and add it to allforms for cleanup
      darktable.develop->allforms = g_list_append(darktable.develop->allforms, mask_form);
      masks_removed = 1;
    }
  }

  free(used_form_ids);

  *forms_list = forms;

  return masks_removed;
}

// removes all unused form from history
/**
 * @brief Remove unused mask forms from a history list, preserving undo safety.
 *
 * Caveat: if multiple history entries reference masks, some unused masks may remain.
 *         This is intentional so users can still jump back in history.
 */
void dt_masks_cleanup_unused_from_list(GList *history_list)
{
  // a mask is used in a given hist->forms entry if it is used up to the next hist->forms
  // so we are going to remove for each hist->forms from the top
  int history_count = g_list_length(history_list);
  int history_end = history_count;
  for(const GList *history_node = g_list_last(history_list); history_node;
      history_node = g_list_previous(history_node))
  {
    dt_dev_history_item_t *history_item = (dt_dev_history_item_t *)history_node->data;
    if(history_item->forms && strcmp(history_item->op_name, "mask_manager") == 0)
    {
      _masks_cleanup_unused(&history_item->forms, history_list, history_end);
      history_end = history_count - 1;
    }
    history_count--;
  }
}

/**
 * @brief Cleanup unused masks and refresh the current forms snapshot.
 *
 * Assumption: caller already decided to drop unused forms (non-reversible).
 */
void dt_masks_cleanup_unused(dt_develop_t *develop)
{
  dt_masks_change_form_gui(NULL);

  // we remove the forms from history
  dt_masks_cleanup_unused_from_list(develop->history);

  // and we save all that
  GList *forms = NULL;
  int history_index = 0;
  for(const GList *history_node = g_list_first(develop->history);
      history_node && history_index < dt_dev_get_history_end_ext(develop);
      history_node = g_list_next(history_node))
  {
    dt_dev_history_item_t *history_item = (dt_dev_history_item_t *)history_node->data;

    if(history_item->forms) forms = history_item->forms;
    history_index++;
  }

  dt_masks_replace_current_forms(develop, forms);
}

/**
 * @brief Check whether any 2D point in pts[] lies inside the form points[].
 *
 * We use the ray casting algorithm for each tested point.
 *
 * @param pts Flat array of tested points [x0, y0, x1, y1, ...].
 * @param num_pts Number of tested points in pts.
 * @param points The array of form vertices.
 * @param points_start The starting index of the form vertices in the array.
 * @param points_count The total number of vertices in the form.
 * @return int Index of the first tested point found inside the form, -1 otherwise.
 */
int dt_masks_point_in_form_exact(const float *test_points, int test_point_count,
                                 const float *form_points, int form_points_start, int form_points_count)
{
  if(!test_points || test_point_count <= 0 || !form_points) return -1;
  if(form_points_count <= 2 + form_points_start) return -1;

  const int start_index = form_points_start;
  for(int test_index = 0; test_index < test_point_count; test_index++)
  {
    int intersection_count = 0;
    const float point_x = test_points[test_index * 2];
    const float point_y = test_points[test_index * 2 + 1];

    for(int i = form_points_start, next = start_index + 1; i < form_points_count;)
    {
      const float y1 = form_points[i * 2 + 1];
      const float y2 = form_points[next * 2 + 1];

      // if we need to skip points (in case of deleted point, because of self-intersection)
      if(isnan(form_points[next * 2]))
      {
        next = isnan(y2) ? start_index : (int)y2;
        continue;
      }

      if(((point_y <= y2 && point_y > y1) || (point_y >= y2 && point_y < y1))
         && (form_points[i * 2] > point_x))
        intersection_count++;

      if(next == start_index) break;
      i = next++;
      // loop
      if(next >= form_points_count) next = start_index;
    }

    if(intersection_count & 1) return test_index;
  }

  return -1;
}

/**
 * @brief Select or clear the current mask form, notifying the owning module if needed.
 *
 * Passing NULL clears the selection.
 */
void dt_masks_select_form(struct dt_iop_module_t *module, dt_masks_form_t *selected_form)
{
  const int selected_formid = selected_form ? selected_form->formid : 0;

  if(!module && selected_formid == 0)
    module = darktable.develop->gui_module;

  if(module && module->masks_selection_changed)
    module->masks_selection_changed(module, selected_formid);
}

/**
 * @brief Decide initial source positioning mode for clone masks.
 *
 * Uses key modifiers to choose absolute vs. relative positioning, and stores
 * the reference position in preview coordinates.
 * The current implementation caches that reference in absolute output-image coordinates.
 */
void dt_masks_set_source_pos_initial_state(dt_masks_form_gui_t *mask_gui, const uint32_t key_state)
{
  if(dt_modifier_is(key_state, GDK_SHIFT_MASK | GDK_CONTROL_MASK))
    mask_gui->source_pos_type = DT_MASKS_SOURCE_POS_ABSOLUTE;
  else if(dt_modifier_is(key_state, GDK_SHIFT_MASK))
    mask_gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE_TEMP;
  else
    fprintf(stderr, "[dt_masks_set_source_pos_initial_state] unknown state for setting masks position type\n");

  // both source types record an absolute position,
  // for the relative type, the first time is used the position is recorded,
  // the second time a relative position is calculated based on that one
  mask_gui->pos_source[0] = mask_gui->pos[0];
  mask_gui->pos_source[1] = mask_gui->pos[1];
}

/**
 * @brief Initialize the clone source position based on current GUI state.
 *
 * Handles first-time relative positioning, existing relative offsets, and
 * absolute coordinates. Updates mask_form->source accordingly.
 * `mask_gui->rel_pos` is the normalized output-image
 * cursor, while `mask_gui->pos_source` stores either an absolute output-image
 * position or an absolute output-image delta depending on the current source mode.
 */
void dt_masks_set_source_pos_initial_value(dt_masks_form_gui_t *mask_gui, dt_masks_form_t *mask_form)
{
  const float raw_width = darktable.develop->roi.raw_width;
  const float raw_height = darktable.develop->roi.raw_height;

  const float xx = mask_gui->pos[0];
  const float yy = mask_gui->pos[1];

  // if this is the first time the relative pos is used
  if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    // if it has not been defined by the user, set some default
    if(mask_gui->pos_source[0] == -1.0f && mask_gui->pos_source[1] == -1.0f)
    {
      if(mask_form->functions && mask_form->functions->initial_source_pos)
      {
        mask_form->functions->initial_source_pos(raw_width, raw_height,
                                                 &mask_gui->pos_source[0], &mask_gui->pos_source[1]);
      }
      else
        fprintf(stderr, "[dt_masks_set_source_pos_initial_value] unsupported masks type when calculating source position initial value\n");

      // set offset to form->source
      mask_form->source[0] = mask_gui->pos[0] + mask_gui->pos_source[0];
      mask_form->source[1] = mask_gui->pos[1] + mask_gui->pos_source[1];
      dt_dev_coordinates_image_abs_to_raw_abs(darktable.develop, mask_form->source, 1);
      // normalize backbuf points
      dt_dev_coordinates_raw_abs_to_raw_norm(darktable.develop, mask_form->source, 1);

    }
    else
    {
      // if a position was defined by the user, use the absolute value the first time
      float source_points[2] = { mask_gui->pos_source[0], mask_gui->pos_source[1] };
      dt_dev_coordinates_image_abs_to_raw_norm(darktable.develop, source_points, 1);

      mask_form->source[0] = source_points[0];
      mask_form->source[1] = source_points[1];

      mask_gui->pos_source[0] = mask_gui->pos_source[0] - xx;
      mask_gui->pos_source[1] = mask_gui->pos_source[1] - yy;
    }

    mask_gui->source_pos_type = DT_MASKS_SOURCE_POS_RELATIVE;
  }
  else if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    // original pos was already defined and relative value calculated, just use it
    mask_form->source[0] = mask_gui->pos[0] + mask_gui->pos_source[0];
    mask_form->source[1] = mask_gui->pos[1] + mask_gui->pos_source[1];
    dt_dev_coordinates_image_abs_to_raw_norm(darktable.develop, mask_form->source, 1);
  }
  else if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // an absolute position was defined by the user
    float source_points[2] = { mask_gui->pos_source[0], mask_gui->pos_source[1] };
    dt_dev_coordinates_image_abs_to_raw_norm(darktable.develop, source_points, 1);

    mask_form->source[0] = source_points[0];
    mask_form->source[1] = source_points[1];
  }
  else
    fprintf(stderr, "[dt_masks_set_source_pos_initial_value] unknown source position type\n");
}

/**
 * @brief Compute preview-space source position for drawing the clone indicator.
 *
 * This uses the stored source positioning mode and can follow the cursor while adding.
 */
void dt_masks_calculate_source_pos_value(dt_masks_form_gui_t *mask_gui, const float initial_xpos,
                                         const float initial_ypos, const float xpos, const float ypos,
                                         float *pos_x, float *pos_y, const int adding)
{
  float source_x = 0.0f;
  float source_y = 0.0f;
  const float raw_width = darktable.develop->roi.raw_width;
  const float raw_height = darktable.develop->roi.raw_height;
  if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE)
  {
    source_x = xpos + mask_gui->pos_source[0];
    source_y = ypos + mask_gui->pos_source[1];
  }
  else if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_RELATIVE_TEMP)
  {
    if(mask_gui->pos_source[0] == -1.0f && mask_gui->pos_source[1] == -1.0f)
    {
      const dt_masks_form_t *visible_form = dt_masks_get_visible_form(darktable.develop);
      if(visible_form && visible_form->functions && visible_form->functions->initial_source_pos)
      {
        visible_form->functions->initial_source_pos(raw_width, raw_height, &source_x, &source_y);
        source_x += xpos;
        source_y += ypos;
      }
      else
        fprintf(stderr, "[dt_masks_calculate_source_pos_value] unsupported masks type when calculating source position value\n");
    }
    else
    {
      source_x = mask_gui->pos_source[0];
      source_y = mask_gui->pos_source[1];
    }
  }
  else if(mask_gui->source_pos_type == DT_MASKS_SOURCE_POS_ABSOLUTE)
  {
    // if the user is actually adding, the mask follow the cursor
    if(adding)
    {
      source_x = xpos + mask_gui->pos_source[0] - initial_xpos;
      source_y = ypos + mask_gui->pos_source[1] - initial_ypos;
    }
    else
    {
      // if not added yet set the start position
      source_x = mask_gui->pos_source[0];
      source_y = mask_gui->pos_source[1];
    }
  }
  else
    fprintf(stderr, "[dt_masks_calculate_source_pos_value] unknown source position type for setting source position value\n");

  *pos_x = source_x;
  *pos_y = source_y;
}

/**
 * @brief Compute rotation angle (degrees) around a center using an anchor point.
 *
 * `anchor`, `center`, and `mask_gui->delta` are absolute output-image
 * coordinates. The angle accounts for possible axis inversion due to
 * distortion transforms.
 * Updates mask_gui->delta to store the last anchor position.
 */
float dt_masks_rotate_with_anchor(dt_develop_t *develop, const float anchor[2], const float center[2],
                                  dt_masks_form_gui_t *mask_gui)
{
  const float center_x = center[0];
  const float center_y = center[1];

  // get the current angle
  const float anchor_x = anchor[0];
  const float anchor_y = anchor[1];
  const float angle_current = atan2f(anchor_y - center_y, anchor_x - center_x);

  // get the previous angle
  const float delta_x = mask_gui->delta[0];
  const float delta_y = mask_gui->delta[1];
  const float angle_prev = atan2f(delta_y - center_y, delta_x - center_x);

  // calculate the angle difference an normalize to -180 to 180 degrees
  float delta_angle = angle_current - angle_prev;
  float angle = atan2f(sinf(delta_angle), cosf(delta_angle));

  // check if distortion inverts the axes
  float test_points[8] = { center_x, center_y, anchor_x , anchor_y,
                           center_x + 10.0f, center_y, center_x, center_y + 10.0f };
  dt_dev_coordinates_image_abs_to_raw_abs(develop, test_points, 4);
  float check_angle = atan2f(test_points[7] - test_points[1], test_points[6] - test_points[0])
                      - atan2f(test_points[5] - test_points[1], test_points[4] - test_points[0]);
  // Normalize to the range -180 to 180 degrees
  check_angle = atan2f(sinf(check_angle), cosf(check_angle));

  // Adjust the sign if the axes are inverted by distortion
  if(check_angle < 0.0f) angle = -angle;

  // Update the delta for the next frame (old position becomes the current one)
  mask_gui->delta[0] = anchor_x;
  mask_gui->delta[1] = anchor_y;

  return angle / M_PI * 180.0f;
}

/**
 * @brief Enter mask creation mode for a given shape type.
 *
 * NOTE: this does quite the same as _menu_add_shape.
 */
gboolean dt_masks_creation_mode(dt_iop_module_t *module, const dt_masks_type_t type)
{
  if(!module || (type & DT_MASKS_ALL) == 0) return FALSE;
  // we want to be sure that the iop has focus
  dt_iop_request_focus(module);

  dt_masks_form_t *mask_form = dt_masks_create(type);
  dt_masks_change_form_gui(mask_form);
  darktable.develop->form_gui->creation = TRUE;
  darktable.develop->form_gui->creation_module = module;

  // Give focus to central view to allow using shortcuts for mask creation right after selecting a mask type in the manager
  gtk_widget_grab_focus(dt_ui_center(darktable.gui->ui));
  return TRUE;
}

/**
 * @brief Apply a mask state operation on a group entry.
 *
 * Inverse toggles its flag, combine operations replace the combine bits.
 */
void apply_operation(struct dt_masks_form_group_t *group_entry, const dt_masks_state_t apply_state)
{
  if(!group_entry) return;

  // Apply Inverse
  if(apply_state == DT_MASKS_STATE_INVERSE)
    group_entry->state ^= DT_MASKS_STATE_INVERSE;
  
  else if((apply_state & DT_MASKS_STATE_IS_COMBINE_OP) != 0)
  {
    // Reset all and apply state
    group_entry->state = (group_entry->state & ~DT_MASKS_STATE_IS_COMBINE_OP) | apply_state;
  }
}

#include "detail.c"

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
