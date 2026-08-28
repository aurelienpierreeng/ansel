/*
    This file is part of darktable,
    Copyright (C) 2011-2021 the darktable developers.
    Copyright (C) 2026 Aurélien PIERRE.

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

/** @file develop/blend_gui.h
 *
 * @brief The blending panel's GUI state and API, cut out of develop/blend.h.
 *
 * @details dt_iop_gui_blend_data_t (~60 widget fields) and the dt_iop_gui_*_blending()
 * functions were the only reason blend.h fed widgets/gradientslider.h,
 * widgets/collapsible_section.h and gui/color_picker_proxy.h to all of its consumers --
 * most of which only wanted the persisted params vocabulary or the pixel entry points.
 * Implementations live in develop/blend_gui.c.
 */

#ifndef DT_DEVELOP_BLEND_GUI_H
#define DT_DEVELOP_BLEND_GUI_H

#include "develop/blend.h"
#include "develop/masks_types.h"   // DEVELOP_MASKS_NB_SHAPES
#include "develop/imageop.h"
#include "gui/color_picker_proxy.h"
#include "widgets/collapsible_section.h"
#include "widgets/gradientslider.h"

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_iop_gui_blendif_colorstop_t
{
  float stoppoint;
  GdkRGBA color;
} dt_iop_gui_blendif_colorstop_t;

typedef struct dt_iop_gui_blendif_channel_t
{
  char *label;
  char *tooltip;
  float increment;
  int numberstops;
  const dt_iop_gui_blendif_colorstop_t *colorstops;
  gboolean boost_factor_enabled;
  float boost_factor_offset;
  dt_develop_blendif_channels_t param_channels[2];
  dt_dev_pixelpipe_display_mask_t display_channel;
  void (*scale_print)(float value, float boost_factor, char *string, int n);
  int (*altdisplay)(GtkWidget *, dt_iop_module_t *, int);
  char *name;
} dt_iop_gui_blendif_channel_t;

typedef struct dt_iop_gui_blendif_filter_t
{
  GtkDarktableGradientSlider *slider;
  GtkLabel *head;
  GtkLabel *label[4];
  GtkLabel *picker_label;
  GtkWidget *polarity;
  GtkWidget *channel_display;
  GtkWidget *log_scale;
  GtkBox *box;
} dt_iop_gui_blendif_filter_t;


/** blend gui data */
typedef struct dt_iop_gui_blend_data_t
{
  int blendif_support;
  int blendif_inited;
  int masks_support;
  int masks_inited;
  int raster_inited;

  dt_develop_blend_colorspace_t csp;
  dt_iop_module_t *module;

  GtkWidget *blending_box;
  GtkWidget *blending_notebook;
  GtkWidget *top_enable;
  GtkWidget *masks_enable;
  GtkWidget *raster_enable;
  GtkWidget *blendif_enable;
  GtkWidget *masks_content;
  GtkWidget *raster_content;
  GtkWidget *blendif_content;
  GtkWidget *contours_content;
  GtkBox *blendif_box;
  GtkBox *masks_box;
  GtkBox *raster_box;

  GtkWidget *colorpicker;
  GtkWidget *colorpicker_set_values;
  dt_iop_gui_blendif_filter_t filter[2];
  GtkWidget *showmask;
  GtkWidget *masks_combine_combo;
  GtkWidget *blend_modes_combo;
  GtkWidget *blend_modes_blend_order;
  GtkWidget *blend_mode_parameter_slider;
  GtkWidget *masks_invert_combo;
  GtkWidget *opacity_slider;
  GtkWidget *masks_feathering_guide_combo;
  GtkWidget *feathering_radius_slider;
  GtkWidget *blur_radius_slider;
  GtkWidget *contrast_slider;
  GtkWidget *brightness_slider;

  dt_develop_blend_colorspace_t blend_modes_csp;
  dt_develop_blend_colorspace_t channel_tabs_csp;

  const dt_iop_gui_blendif_channel_t *channel;
  int tab;
  int altmode[8][2];
  dt_dev_pixelpipe_display_mask_t save_for_leave;
  int timeout_handle;
  GtkNotebook *channel_tabs;
  gboolean output_channels_shown;

  GtkWidget *channel_boost_factor_slider;
  GtkWidget *details_slider;

  GtkWidget *masks_combo;
  GtkWidget *masks_shapes[DEVELOP_MASKS_NB_SHAPES];
  int masks_type[DEVELOP_MASKS_NB_SHAPES];
  GtkWidget *masks_edit;
  GtkWidget *group_shapes_label;
  GtkWidget *masks_polarity;
  GtkWidget *wire_shape_toggle;
  int *masks_combo_ids;
  int masks_shown;
  GtkWidget *masks_treeview;
  GtkWidget *masks_group_treeview;
  GtkTreeStore *group_shapes_store;
  GtkTreeViewColumn *group_shapes_col;
  GtkTreeViewColumn *group_unlink_col;
  GtkTreeViewColumn *group_delete_col;
  GtkListStore *all_shapes_store;
  GtkWidget *group_shapes_sw;
  GtkTreeViewColumn *all_shapes_col;
  GtkTreeViewColumn *all_shapes_delete_col;
  GtkWidget *all_shapes_sw;
  GtkWidget *lists_stack;
  GdkPixbuf *masks_ic_inverse;
  GdkPixbuf *masks_ic_union;
  GdkPixbuf *masks_ic_intersection;
  GdkPixbuf *masks_ic_difference;
  GdkPixbuf *masks_ic_exclusion;
  GtkWidget *all_shapes_buttons;
  GtkWidget *lists_box;
  dt_gui_collapsible_section_t masks_cs;
  // Mouse-wheel mapping grid: application-wide state (masks_gui.h), so this holds no value of
  // its own -- it reads conf when shown and writes it when toggled.
  dt_gui_collapsible_section_t scroll_cs;


  GtkWidget *raster_combo;
  GtkWidget *raster_polarity;

  gboolean picker_set_values_box_valid;
  dt_boundingbox_t picker_set_values_box;
  gboolean picker_set_values_manual_boost_lock;

  int control_button_pressed;
  dt_pthread_mutex_t lock;
} dt_iop_gui_blend_data_t;

/** gui related stuff */
void dt_iop_gui_init_blendif(GtkBox *blendw, dt_iop_module_t *module, GtkWidget *header);
void dt_iop_gui_init_blending(dt_iop_module_t *module);
void dt_iop_gui_init_blending_body(GtkWidget *container, dt_iop_module_t *module);
void dt_iop_gui_update_blending(dt_iop_module_t *module);
void dt_iop_gui_update_blendif(dt_iop_module_t *module);
void dt_iop_gui_cleanup_blending_body(dt_iop_module_t *module);
void dt_iop_gui_cleanup_blending(dt_iop_module_t *module);
void dt_iop_gui_blending_lose_focus(dt_iop_module_t *module);
void dt_iop_gui_blending_reload_defaults(dt_iop_module_t *module);

/** Refresh the blend GUI's mask widgets (shape combo, edit/polarity toggles, shape buttons and
 * the mask list) from the module's current mask group.
 *
 * Declared here because this is where it lives and what it touches: dt_iop_gui_blend_data_t,
 * which is the blend GUI's own. It spent years declared in develop/masks.h under a dt_masks_
 * name while its body sat in blend_gui.c -- a masks symbol that the masks module did not
 * implement and could not have, since every widget it drives is private to this one. */
void dt_iop_gui_blend_masks_update(dt_iop_module_t *module);

gboolean blend_color_picker_apply(dt_iop_module_t *module, GtkWidget *picker, dt_dev_pixelpipe_t *pipe,
                                  dt_dev_pixelpipe_iop_t *piece);

#ifdef __cplusplus
}
#endif

#endif // DT_DEVELOP_BLEND_GUI_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
