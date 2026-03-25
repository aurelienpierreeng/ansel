/*
    This file is part of darktable,
    Copyright (C) 2019-2021 Pascal Obry.
    Copyright (C) 2020-2021 Aldric Renaudin.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Philippe Weyland.
    Copyright (C) 2021, 2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2021 Bill Ferguson.
    Copyright (C) 2021 Chris Elston.
    Copyright (C) 2021 Diederik Ter Rahe.
    Copyright (C) 2022 Martin Bařinka.

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

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/darktable.h"
#include "common/debug.h"
#include "common/history.h"
#include "common/iop_order.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "develop/format.h"
#include "develop/imageop.h"
#include "develop/pixelpipe_hb.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"
#include "gui/presets.h"
#include "libs/lib.h"

#include <gtk/gtk.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE(1)

#define DT_IOPORDER_GRAPH_NODE_WIDTH DT_PIXEL_APPLY_DPI(260)
#define DT_IOPORDER_GRAPH_NODE_STEP DT_PIXEL_APPLY_DPI(300)
#define DT_IOPORDER_GRAPH_LEFT_MARGIN DT_PIXEL_APPLY_DPI(60)
#define DT_IOPORDER_GRAPH_TOP_MARGIN DT_PIXEL_APPLY_DPI(68)
#define DT_IOPORDER_GRAPH_MIN_WIDTH DT_PIXEL_APPLY_DPI(640)
#define DT_IOPORDER_GRAPH_HEIGHT DT_PIXEL_APPLY_DPI(540)
#define DT_IOPORDER_BAND_HEIGHT DT_PIXEL_APPLY_DPI(26)
#define DT_IOPORDER_MASK_ARROW_OFFSET DT_PIXEL_APPLY_DPI(12)
#define DT_IOPORDER_MASK_BOTTOM_MARGIN DT_PIXEL_APPLY_DPI(56)

typedef enum dt_ioporder_dnd_target_t
{
  DT_IOPORDER_DND_TARGET_NODE = 0
} dt_ioporder_dnd_target_t;

typedef enum dt_ioporder_runtime_band_kind_t
{
  DT_IOPORDER_RUNTIME_BAND_NONE = 0,
  DT_IOPORDER_RUNTIME_BAND_UNAVAILABLE,
  DT_IOPORDER_RUNTIME_BAND_RAW,
  DT_IOPORDER_RUNTIME_BAND_SENSOR_RGB,
  DT_IOPORDER_RUNTIME_BAND_PIPELINE_RGB,
  DT_IOPORDER_RUNTIME_BAND_DISPLAY_RGB,
  DT_IOPORDER_RUNTIME_BAND_LAB,
  DT_IOPORDER_RUNTIME_BAND_OTHER
} dt_ioporder_runtime_band_kind_t;

static const GtkTargetEntry _ioporder_target_list[] = {
  { "ioporder-node", GTK_TARGET_SAME_APP, DT_IOPORDER_DND_TARGET_NODE }
};
static const guint _ioporder_target_count = G_N_ELEMENTS(_ioporder_target_list);

typedef struct dt_ioporder_graph_node_t
{
  gboolean is_endpoint;
  dt_iop_module_t *module;
  dt_dev_pixelpipe_iop_t *piece;
  gchar *endpoint_label;

  GtkWidget *event_box;
  GtkWidget *header;
  GtkWidget *title;
  GtkWidget *instance;
  GtkWidget *info_in;
  GtkWidget *info_out;
} dt_ioporder_graph_node_t;

typedef struct dt_lib_ioporder_t
{
  int current_mode;
  gboolean refreshing_toolbar;
  GtkWidget *window;
  GtkWidget *toolbar_label;
  GtkWidget *preset_combo;
  GtkWidget *graph_scroll;
  GtkWidget *graph_overlay;
  GtkWidget *graph_drawing;
  GtkWidget *graph_fixed;
  GList *nodes;
  dt_iop_module_t *drag_source;
  dt_iop_module_t *drag_dest;
} dt_lib_ioporder_t;

static void _ioporder_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                                    GtkSelectionData *selection_data, guint info, guint time,
                                    gpointer user_data);
static void _ioporder_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data);
static void _ioporder_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data);
static gboolean _ioporder_drag_motion(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                      guint time, gpointer user_data);
static gboolean _ioporder_drag_drop(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                    guint time, gpointer user_data);
static void _ioporder_drag_leave(GtkWidget *widget, GdkDragContext *dc, guint time, gpointer user_data);
static void _ioporder_drag_data_received(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                         GtkSelectionData *selection_data, guint info, guint time,
                                         gpointer user_data);
static void _ioporder_free_graph_node(gpointer data);

/**
 * @brief Return TRUE when a module already exists in history.
 *
 * The graph uses the same active/history visibility policy as the active pipe
 * tab, so we scan the history stack and look for the exact module instance.
 *
 * @param module Module to test.
 * @return TRUE when the module is present in history.
 */
static gboolean _ioporder_module_in_history(const dt_iop_module_t *module)
{
  if(!darktable.develop || !module) return FALSE;

  /* Walk the whole history stack and look for the exact module instance. */
  for(GList *history = g_list_last(darktable.develop->history); history; history = g_list_previous(history))
  {
    const dt_dev_history_item_t *hitem = (dt_dev_history_item_t *)history->data;
    if(hitem && hitem->module == module) return TRUE;
  }

  return FALSE;
}

/**
 * @brief Apply the active-pipe visibility policy used by modulegroups.
 *
 * We only show visible GUI modules that are currently enabled or that already
 * appear in history, matching the darkroom pipeline tab.
 *
 * @param module Module to test.
 * @return TRUE when the module should appear in the graph.
 */
static gboolean _ioporder_module_is_graph_visible(const dt_iop_module_t *module)
{
  if(!module) return FALSE;
  if(dt_iop_is_hidden((dt_iop_module_t *)module)) return FALSE;
  return _ioporder_module_in_history(module) || module->enabled;
}

/**
 * @brief Map a module instance to its preview-pipe piece.
 *
 * The runtime descriptors displayed under each node come from the preview pipe,
 * so we scan the instantiated preview nodes and match them by module pointer.
 *
 * @param module Module whose runtime piece is requested.
 * @return Matching preview-pipe piece or NULL when unavailable.
 */
static dt_dev_pixelpipe_iop_t *_ioporder_get_preview_piece(dt_iop_module_t *module)
{
  if(!darktable.develop || !darktable.develop->preview_pipe || !module) return NULL;

  /* Search the preview pipe nodes for the piece instantiated from this module. */
  for(GList *nodes = darktable.develop->preview_pipe->nodes; nodes; nodes = g_list_next(nodes))
  {
    dt_dev_pixelpipe_iop_t *piece = (dt_dev_pixelpipe_iop_t *)nodes->data;
    if(piece && piece->module == module) return piece;
  }

  return NULL;
}

/**
 * @brief Return a human-readable datatype string for a pixel descriptor.
 *
 * @param datatype Descriptor datatype enum.
 * @return Static human-readable datatype string.
 */
static const char *_ioporder_type_to_string(const dt_iop_buffer_type_t datatype)
{
  switch(datatype)
  {
    case TYPE_FLOAT:
      return _("32-bit float");
    case TYPE_UINT16:
      return _("16-bit integer");
    case TYPE_UINT8:
      return _("8-bit integer");
    case TYPE_UNKNOWN:
    default:
      return _("unknown type");
  }
}

/**
 * @brief Return a human-readable colorspace string for a pixel descriptor.
 *
 * @param cst Descriptor colorspace enum.
 * @return Static human-readable colorspace string.
 */
static const char *_ioporder_colorspace_to_string(const dt_iop_colorspace_type_t cst)
{
  switch(cst)
  {
    case IOP_CS_RAW:
      return _("RAW");
    case IOP_CS_RGB:
      return _("RGB");
    case IOP_CS_RGB_DISPLAY:
      return _("Display RGB");
    case IOP_CS_LAB:
      return _("CIE Lab");
    case IOP_CS_LCH:
      return _("LCh");
    case IOP_CS_HSL:
      return _("HSL");
    case IOP_CS_JZCZHZ:
      return _("JzCzHz");
    case IOP_CS_NONE:
      return _("none");
    default:
      return _("unknown colorspace");
  }
}

/**
 * @brief Format the RAW-specific flags carried by a pixel descriptor.
 *
 * @param dsc Descriptor to inspect.
 * @return Newly-allocated string describing RAW layout or passthrough state.
 */
static gchar *_ioporder_raw_flags_to_string(const dt_iop_buffer_dsc_t *dsc)
{
  if(!dsc) return g_strdup(_("no runtime descriptor"));

  if(dsc->cst != IOP_CS_RAW)
  {
    if(dsc->filters == 9u) return g_strdup(_("X-Trans passthrough"));
    if(dsc->filters != 0u) return g_strdup(_("Bayer passthrough"));
    return g_strdup(_("no RAW flags"));
  }

  if(dsc->filters == 9u) return g_strdup(_("RAW X-Trans"));
  if(dsc->filters != 0u) return g_strdup(_("RAW Bayer"));
  return g_strdup(_("RAW monochrome"));
}

/**
 * @brief Build the descriptor list shown below each module node.
 *
 * We keep the runtime contract explicit as a short title followed by one item
 * per property so users can scan the in/out descriptors independently.
 *
 * @param prefix User-visible list title such as "In" or "Out".
 * @param dsc Descriptor to format.
 * @param display_colorspace User-visible runtime lifecycle label.
 * @return Newly-allocated user-visible descriptor list.
 */
static gchar *_ioporder_descriptor_to_text(const char *prefix, const dt_iop_buffer_dsc_t *dsc,
                                           const char *display_colorspace)
{
  if(!dsc)
    return g_strdup_printf(_("%s:\n- runtime descriptor unavailable"), prefix);

  gchar *raw_flags = _ioporder_raw_flags_to_string(dsc);
  gchar *text = g_strdup_printf(_("%s:\n\t- %s\n\t- %u channels\n\t- %s\n"
                                  "\t- max RGB:\n\t\t%.3f\n\t\t%.3f\n\t\t%.3f\n\t- %s"),
                                prefix, _ioporder_type_to_string(dsc->datatype), dsc->channels,
                                display_colorspace ? display_colorspace : _("runtime unavailable"),
                                dsc->processed_maximum[0], dsc->processed_maximum[1], dsc->processed_maximum[2],
                                raw_flags);
  dt_free(raw_flags);
  return text;
}

/**
 * @brief Classify one runtime descriptor into a stable band kind.
 *
 * RAW pipelines become sensor RGB before colorin and working-pipeline RGB
 * from colorin onwards. Display RGB is now carried explicitly by the runtime
 * descriptor itself once colorout publishes it.
 *
 * @param raw_input TRUE when the source image starts as RAW.
 * @param colorin_crossed TRUE when the graph already passed colorin.
 * @param module Module owning the runtime piece.
 * @param dsc Runtime descriptor to classify.
 * @param after_module TRUE to classify the module output, FALSE for its input.
 * @return Stable band kind used for merging and coloring segments.
 */
static dt_ioporder_runtime_band_kind_t _ioporder_runtime_band_kind(const gboolean raw_input,
                                                                   const gboolean colorin_crossed,
                                                                   const dt_iop_module_t *module,
                                                                   const dt_iop_buffer_dsc_t *dsc,
                                                                   const gboolean after_module)
{
  if(!dsc || !module) return DT_IOPORDER_RUNTIME_BAND_UNAVAILABLE;

  if(dsc->cst == IOP_CS_RAW) return DT_IOPORDER_RUNTIME_BAND_RAW;
  if(dsc->cst == IOP_CS_LAB) return DT_IOPORDER_RUNTIME_BAND_LAB;
  if(dsc->cst == IOP_CS_RGB_DISPLAY) return DT_IOPORDER_RUNTIME_BAND_DISPLAY_RGB;
  if(dsc->cst == IOP_CS_RGB)
  {
    const gboolean in_pipeline_rgb = colorin_crossed || (after_module && !strcmp(module->op, "colorin"));
    if(raw_input && !in_pipeline_rgb) return DT_IOPORDER_RUNTIME_BAND_SENSOR_RGB;
    return DT_IOPORDER_RUNTIME_BAND_PIPELINE_RGB;
  }
  return DT_IOPORDER_RUNTIME_BAND_OTHER;
}

/**
 * @brief Return the user-visible label for one runtime band kind.
 *
 * We derive labels from the stable enum so translated strings remain a pure UI
 * concern and never drive segment merging or color decisions.
 *
 * @param kind Stable runtime band kind.
 * @param dsc Runtime descriptor, only used for fallback "other" colorspaces.
 * @return Static user-visible label.
 */
static const char *_ioporder_runtime_band_label(const dt_ioporder_runtime_band_kind_t kind,
                                                const dt_iop_buffer_dsc_t *dsc)
{
  switch(kind)
  {
    case DT_IOPORDER_RUNTIME_BAND_RAW:
      return _("RAW");
    case DT_IOPORDER_RUNTIME_BAND_SENSOR_RGB:
      return _("Sensor RGB");
    case DT_IOPORDER_RUNTIME_BAND_PIPELINE_RGB:
      return _("Pipeline RGB");
    case DT_IOPORDER_RUNTIME_BAND_DISPLAY_RGB:
      return _("Display RGB");
    case DT_IOPORDER_RUNTIME_BAND_LAB:
      return _("CIE Lab 1976");
    case DT_IOPORDER_RUNTIME_BAND_UNAVAILABLE:
      return _("runtime unavailable");
    case DT_IOPORDER_RUNTIME_BAND_OTHER:
      return dsc ? _ioporder_colorspace_to_string(dsc->cst) : _("runtime unavailable");
    case DT_IOPORDER_RUNTIME_BAND_NONE:
    default:
      return _("runtime unavailable");
  }
}

/**
 * @brief Pick the color used by the colorspace lifecycle band.
 *
 * @param kind Stable band kind returned by _ioporder_runtime_band_kind().
 * @param color Output color.
 */
static void _ioporder_runtime_band_color(const dt_ioporder_runtime_band_kind_t kind, GdkRGBA *color)
{
  if(!color) return;

  switch(kind)
  {
    case DT_IOPORDER_RUNTIME_BAND_RAW:
      *color = (GdkRGBA){ 0.30, 0.30, 0.30, 0.90 };
      break;
    case DT_IOPORDER_RUNTIME_BAND_SENSOR_RGB:
      *color = (GdkRGBA){ 0.14, 0.56, 0.20, 0.96 };
      break;
    case DT_IOPORDER_RUNTIME_BAND_PIPELINE_RGB:
      *color = (GdkRGBA){ 0.10, 0.38, 0.78, 0.96 };
      break;
    case DT_IOPORDER_RUNTIME_BAND_DISPLAY_RGB:
      *color = (GdkRGBA){ 0.78, 0.38, 0.10, 0.96 };
      break;
    case DT_IOPORDER_RUNTIME_BAND_LAB:
      *color = (GdkRGBA){ 0.72, 0.54, 0.10, 0.96 };
      break;
    case DT_IOPORDER_RUNTIME_BAND_UNAVAILABLE:
    case DT_IOPORDER_RUNTIME_BAND_OTHER:
    case DT_IOPORDER_RUNTIME_BAND_NONE:
    default:
      *color = (GdkRGBA){ 0.38, 0.38, 0.38, 0.90 };
      break;
  }
}

/**
 * @brief Compare two profile descriptors for lifecycle segment merging.
 *
 * RGB segments need to split when the attached profile metadata changes, even
 * if the broad lifecycle kind stays the same. We only compare the fields that
 * affect the label shown to the user.
 *
 * @param profile_a First profile descriptor or NULL.
 * @param profile_b Second profile descriptor or NULL.
 * @return TRUE when both describe the same visible profile.
 */
static gboolean _ioporder_same_runtime_profile(const dt_iop_order_iccprofile_info_t *profile_a,
                                               const dt_iop_order_iccprofile_info_t *profile_b)
{
  if(profile_a == profile_b) return TRUE;
  if(!profile_a || !profile_b) return FALSE;

  return profile_a->type == profile_b->type && !strcmp(profile_a->filename, profile_b->filename);
}

/**
 * @brief Pick the RGB profile metadata that matches one runtime band segment.
 *
 * Runtime RGB segments can refer to the input profile before colorin, the work
 * profile in pipeline RGB, or the output/display profile once colorout has
 * produced display RGB.
 *
 * @param raw_input TRUE when the source image starts as RAW.
 * @param colorin_crossed TRUE when the graph already passed colorin.
 * @param module Module owning the runtime piece.
 * @param dsc Runtime descriptor to classify.
 * @param after_module TRUE to classify the module output, FALSE for its input.
 * @return Matching profile info or NULL when none applies.
 */
static const dt_iop_order_iccprofile_info_t *_ioporder_runtime_band_profile_info(const gboolean raw_input,
                                                                                 const gboolean colorin_crossed,
                                                                                 const dt_iop_module_t *module,
                                                                                 const dt_iop_buffer_dsc_t *dsc,
                                                                                 const gboolean after_module)
{
  if(!darktable.develop || !darktable.develop->preview_pipe || !module || !dsc) return NULL;
  if(!dt_iop_colorspace_is_rgb(dsc->cst)) return NULL;

  if(dsc->cst == IOP_CS_RGB_DISPLAY)
    return dt_ioppr_get_pipe_output_profile_info(darktable.develop->preview_pipe);

  const gboolean in_pipeline_rgb = colorin_crossed || (after_module && !strcmp(module->op, "colorin"));
  if(!in_pipeline_rgb)
    return dt_ioppr_get_pipe_input_profile_info(darktable.develop->preview_pipe);

  (void)raw_input;
  return dt_ioppr_get_pipe_work_profile_info(darktable.develop->preview_pipe);
}

/**
 * @brief Build the text shown in one colorspace-band segment.
 *
 * When a runtime band is RGB-based and the pipe exposes ICC/profile metadata,
 * we append the profile display name so the graph states both the lifecycle
 * stage and the actual RGB basis.
 *
 * @param label Runtime lifecycle label such as "pipeline RGB".
 * @param profile_info Matching profile metadata.
 * @return Newly-allocated user-visible segment text.
 */
static gchar *_ioporder_runtime_band_text(const char *label,
                                          const dt_iop_order_iccprofile_info_t *profile_info)
{
  if(!label) return g_strdup(_("runtime unavailable"));
  if(!profile_info || profile_info->type == DT_COLORSPACE_NONE) return g_strdup(label);

  const char *profile_name = dt_colorspaces_get_name(profile_info->type, profile_info->filename);
  if(!profile_name || profile_name[0] == '\0') return g_strdup(label);

  return g_strdup_printf("%s - %s", label, profile_name);
}

/**
 * @brief Retrieve the display name of the current pipeline order.
 *
 * Built-in orders use their public name while custom orders are matched
 * against saved presets to surface a preset name when possible.
 *
 * @param self The ioporder lib module.
 * @return Newly-allocated current order name.
 */
static gchar *_ioporder_get_current_order_name(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  if(!darktable.develop || !darktable.develop->iop_order_list)
  {
    d->current_mode = DT_IOP_ORDER_V30;
    return g_strdup(_(dt_iop_order_string(DT_IOP_ORDER_V30)));
  }

  const dt_iop_order_t kind = dt_ioppr_get_iop_order_list_kind(darktable.develop->iop_order_list);

  if(kind != DT_IOP_ORDER_CUSTOM)
  {
    d->current_mode = kind;
    return g_strdup(_(dt_iop_order_string(kind)));
  }

  gchar *iop_order_list = dt_ioppr_serialize_text_iop_order_list(darktable.develop->iop_order_list);
  gchar *name = NULL;
  int index = 0;
  sqlite3_stmt *stmt;

  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT op_params, name"
                              " FROM data.presets"
                              " WHERE operation='ioporder'"
                              " ORDER BY writeprotect DESC, LOWER(name), rowid",
                              -1, &stmt, NULL);
  // clang-format on

  /* Compare the current order text against every saved preset serialization. */
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *params = (const char *)sqlite3_column_blob(stmt, 0);
    const int32_t params_len = sqlite3_column_bytes(stmt, 0);
    const char *preset_name = (const char *)sqlite3_column_text(stmt, 1);
    GList *preset_list = dt_ioppr_deserialize_iop_order_list(params, params_len);
    gchar *preset_text = dt_ioppr_serialize_text_iop_order_list(preset_list);
    g_list_free_full(preset_list, dt_free_gpointer);
    preset_list = NULL;
    index++;

    if(!g_strcmp0(iop_order_list, preset_text))
    {
      d->current_mode = index;
      name = g_strdup(preset_name);
      dt_free(preset_text);
      break;
    }

    dt_free(preset_text);
  }

  sqlite3_finalize(stmt);
  dt_free(iop_order_list);

  if(name) return name;

  d->current_mode = DT_IOP_ORDER_CUSTOM;
  return g_strdup(_(dt_iop_order_string(DT_IOP_ORDER_CUSTOM)));
}

/**
 * @brief Refresh the preset combo and status label in the popup toolbar.
 *
 * The combo is rebuilt from the preset table so it always exposes the latest
 * saved pipeline-order presets and keeps the current order selected when it
 * matches one of them.
 *
 * @param self The ioporder lib module.
 */
static void _ioporder_refresh_toolbar(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  if(!d->preset_combo || !d->toolbar_label) return;

  gchar *current_name = _ioporder_get_current_order_name(self);

  gtk_label_set_text(GTK_LABEL(d->toolbar_label), current_name);

  d->refreshing_toolbar = TRUE;
  gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(d->preset_combo));
  gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->preset_combo), "__custom__", _("custom order"));

  gchar *active_id = g_strdup("__custom__");
  sqlite3_stmt *stmt;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name"
                              " FROM data.presets"
                              " WHERE operation='ioporder' AND op_version=?1"
                              " ORDER BY writeprotect DESC, LOWER(name), rowid",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, self->version());

  /* Rebuild the preset list in DB order and keep the current one selected. */
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *preset_name = (const char *)sqlite3_column_text(stmt, 0);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(d->preset_combo), preset_name, preset_name);
    if(!g_strcmp0(current_name, preset_name))
    {
      dt_free(active_id);
      active_id = g_strdup(preset_name);
    }
  }
  sqlite3_finalize(stmt);

  gtk_combo_box_set_active_id(GTK_COMBO_BOX(d->preset_combo), active_id);
  d->refreshing_toolbar = FALSE;

  dt_free(active_id);
  dt_free(current_name);
}

/**
 * @brief Destroy every node widget and free the node descriptors.
 *
 * @param d Popup runtime state.
 */
static void _ioporder_clear_graph(dt_lib_ioporder_t *d)
{
  if(!d || !d->graph_fixed) return;

  GList *children = gtk_container_get_children(GTK_CONTAINER(d->graph_fixed));
  for(GList *iter = children; iter; iter = g_list_next(iter))
  {
    gtk_widget_destroy(GTK_WIDGET(iter->data));
  }
  g_list_free(children);
  children = NULL;

  g_list_free_full(d->nodes, _ioporder_free_graph_node);
  d->nodes = NULL;
}

/**
 * @brief Release one graph node descriptor.
 *
 * Widgets are destroyed separately when clearing the fixed container, so this
 * helper only releases the bookkeeping fields owned by the node.
 *
 * @param data Node descriptor to free.
 */
static void _ioporder_free_graph_node(gpointer data)
{
  dt_ioporder_graph_node_t *node = (dt_ioporder_graph_node_t *)data;
  if(!node) return;

  dt_free(node->endpoint_label);
  dt_free(node);
}

/**
 * @brief Apply the same enable-button icon policy used by module headers.
 *
 * The real helper is private to imageop.c, so the graph node mirrors the same
 * paint selection locally.
 *
 * @param widget Proxy enable button.
 * @param module Module represented by the proxy.
 */
static void _ioporder_set_enable_button_icon(GtkWidget *widget, dt_iop_module_t *module)
{
  if(!DTGTK_IS_TOGGLEBUTTON(widget) || !module) return;

  if(module->hide_enable_button)
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_module_switch_on, 0, module);
  }
  else
  {
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(widget), dtgtk_cairo_paint_module_switch, 0, module);
  }
}

/**
 * @brief Proxy the module enable state through the real darkroom header widget.
 *
 * The proxy button keeps the graph UI consistent while delegating the actual
 * state change to the existing module header callback chain.
 *
 * @param togglebutton Proxy enable button.
 * @param user_data Node descriptor.
 */
static void _ioporder_node_toggle_enable(GtkToggleButton *togglebutton, gpointer user_data)
{
  const dt_ioporder_graph_node_t *node = (const dt_ioporder_graph_node_t *)user_data;
  if(!node || !GTK_IS_TOGGLE_BUTTON(node->module->off)) return;

  const gboolean active = gtk_toggle_button_get_active(togglebutton);
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(node->module->off)) != active)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(node->module->off), active);
}

/**
 * @brief Proxy raster/drawn mask preview toggling through the real module header.
 *
 * @param togglebutton Proxy mask-display button.
 * @param user_data Node descriptor.
 */
static void _ioporder_node_toggle_mask(GtkToggleButton *togglebutton, gpointer user_data)
{
  const dt_ioporder_graph_node_t *node = (const dt_ioporder_graph_node_t *)user_data;
  if(!node || !GTK_IS_TOGGLE_BUTTON(node->module->mask_indicator)) return;

  const gboolean active = gtk_toggle_button_get_active(togglebutton);
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(node->module->mask_indicator)) != active)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(node->module->mask_indicator), active);
}

/**
 * @brief Open the standard module preset popup from a graph node.
 *
 * @param button Proxy presets button clicked in the graph node.
 * @param user_data Node descriptor.
 */
static void _ioporder_node_show_presets(GtkButton *button, gpointer user_data)
{
  const dt_ioporder_graph_node_t *node = (const dt_ioporder_graph_node_t *)user_data;
  if(!node || !node->module) return;

  dt_gui_presets_popup_menu_show_for_module(node->module);
  dt_gui_menu_popup(darktable.gui->presets_popup_menu, GTK_WIDGET(button),
                    GDK_GRAVITY_SOUTH_EAST, GDK_GRAVITY_NORTH_EAST);

  if(DTGTK_IS_BUTTON(button)) dtgtk_button_set_active(DTGTK_BUTTON(button), FALSE);
}

/**
 * @brief Create one interactive graph node mirroring a module header.
 *
 * @param module Module shown by the node.
 * @param piece Matching preview-pipe runtime piece, if any.
 * @return Newly-allocated node descriptor.
 */
static dt_ioporder_graph_node_t *_ioporder_create_graph_node(dt_iop_module_t *module,
                                                             dt_dev_pixelpipe_iop_t *piece,
                                                             const char *display_in,
                                                             const char *display_out)
{
  dt_ioporder_graph_node_t *node = (dt_ioporder_graph_node_t *)calloc(1, sizeof(dt_ioporder_graph_node_t));
  node->module = module;
  node->piece = piece;

  GtkWidget *event_box = gtk_event_box_new();
  GtkWidget *frame = gtk_frame_new(NULL);
  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(6));
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(4));
  gchar *clean_name = delete_underscore(module->name());
  gchar **split_name = g_strsplit(clean_name, "-", -1);
  gchar *module_name = g_strjoinv(" ", split_name);
  GtkWidget *label = gtk_label_new(module_name);
  gchar *instance_name = dt_dev_get_multi_name(module);
  GtkWidget *instance = gtk_label_new(instance_name);
  GtkWidget *enable = dtgtk_togglebutton_new(dtgtk_cairo_paint_module_switch, 0, module);
  GtkWidget *mask = dtgtk_togglebutton_new(dtgtk_cairo_paint_showmask, 0, NULL);
  GtkWidget *presets = dtgtk_button_new(dtgtk_cairo_paint_presets, 0, NULL);

  dt_gui_add_class(frame, "dt_module_frame");
  gtk_widget_set_name(body, "module-header");
  dt_gui_add_class(enable, "dt_transparent_background");
  dt_gui_add_class(enable, "dt_iop_enable_button");
  dt_gui_add_class(mask, "dt_transparent_background");
  gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);

  gtk_widget_set_size_request(event_box, DT_IOPORDER_GRAPH_NODE_WIDTH, -1);
  gtk_widget_set_hexpand(event_box, FALSE);
  gtk_widget_set_halign(event_box, GTK_ALIGN_START);

  dt_capitalize_label(module_name);
  gtk_label_set_text(GTK_LABEL(label), module_name);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_name(label, "iop-panel-label");

  gtk_widget_set_halign(instance, GTK_ALIGN_START);
  gtk_widget_set_visible(instance, instance_name[0] != '\0');
  gtk_label_set_xalign(GTK_LABEL(instance), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(instance), PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_line_wrap(GTK_LABEL(instance), FALSE);

  _ioporder_set_enable_button_icon(enable, module);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enable), module->enabled);
  gtk_widget_set_sensitive(enable, !module->hide_enable_button);
  gtk_widget_set_valign(enable, GTK_ALIGN_CENTER);

  const gboolean show_masks = (module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) == IOP_FLAGS_SUPPORTS_BLENDING
                              && module->blend_params
                              && module->blend_params->mask_mode > DEVELOP_MASK_ENABLED;
  const gboolean show_mask_preview = module->request_mask_display
                                     & (DT_DEV_PIXELPIPE_DISPLAY_MASK | DT_DEV_PIXELPIPE_DISPLAY_CHANNEL);
  gtk_widget_set_visible(mask, show_masks);
  gtk_widget_set_sensitive(mask, show_masks && module->enabled);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mask), show_mask_preview);
  gtk_widget_set_valign(mask, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(presets, GTK_ALIGN_CENTER);

  gtk_widget_set_tooltip_text(enable, _("toggle module"));
  gtk_widget_set_tooltip_text(mask, _("display mask"));
  gtk_widget_set_tooltip_text(presets, _("module presets"));

  gtk_box_pack_start(GTK_BOX(header), enable, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(header), label, TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(header), presets, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(header), mask, FALSE, FALSE, 0);

  gchar *text_in = _ioporder_descriptor_to_text(_("In"), piece ? &piece->dsc_in : NULL, display_in);
  gchar *text_out = _ioporder_descriptor_to_text(_("Out"), piece ? &piece->dsc_out : NULL, display_out);
  GtkWidget *info_in = gtk_label_new(text_in);
  GtkWidget *info_out = gtk_label_new(text_out);
  dt_free(text_in);
  dt_free(text_out);
  dt_free(instance_name);
  g_strfreev(split_name);
  dt_free(module_name);
  dt_free(clean_name);

  gtk_label_set_xalign(GTK_LABEL(info_in), 0.0f);
  gtk_label_set_xalign(GTK_LABEL(info_out), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(info_in), TRUE);
  gtk_label_set_line_wrap(GTK_LABEL(info_out), TRUE);
  gtk_label_set_justify(GTK_LABEL(info_in), GTK_JUSTIFY_LEFT);
  gtk_label_set_justify(GTK_LABEL(info_out), GTK_JUSTIFY_LEFT);

  gtk_box_pack_start(GTK_BOX(body), header, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(body), instance, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(body), info_in, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(body), info_out, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(frame), body);
  gtk_container_add(GTK_CONTAINER(event_box), frame);

  g_signal_connect(enable, "toggled", G_CALLBACK(_ioporder_node_toggle_enable), node);
  g_signal_connect(mask, "toggled", G_CALLBACK(_ioporder_node_toggle_mask), node);
  g_signal_connect(presets, "clicked", G_CALLBACK(_ioporder_node_show_presets), node);

  node->event_box = event_box;
  node->header = header;
  node->title = label;
  node->instance = instance;
  node->info_in = info_in;
  node->info_out = info_out;

  g_object_set_data(G_OBJECT(event_box), "dt-ioporder-module", module);

  return node;
}

/**
 * @brief Create a compact endpoint node used for the graph boundaries.
 *
 * Endpoint nodes represent the pipeline source and sink, so they stay smaller,
 * non-interactive, and visually distinct from module header replicas.
 *
 * @param label User-visible endpoint label.
 * @return Newly-allocated endpoint node descriptor.
 */
static dt_ioporder_graph_node_t *_ioporder_create_endpoint_node(const char *label)
{
  dt_ioporder_graph_node_t *node = (dt_ioporder_graph_node_t *)calloc(1, sizeof(dt_ioporder_graph_node_t));
  node->is_endpoint = TRUE;
  node->endpoint_label = g_strdup(label);

  GtkWidget *event_box = gtk_event_box_new();
  GtkWidget *frame = gtk_frame_new(NULL);
  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *title = gtk_label_new(label);

  dt_gui_add_class(frame, "dt_module_frame");
  gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
  gtk_widget_set_size_request(event_box, DT_PIXEL_APPLY_DPI(180), DT_PIXEL_APPLY_DPI(64));
  gtk_widget_set_halign(event_box, GTK_ALIGN_START);
  gtk_widget_set_valign(event_box, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(body, DT_PIXEL_APPLY_DPI(12));
  gtk_widget_set_margin_bottom(body, DT_PIXEL_APPLY_DPI(12));
  gtk_widget_set_margin_start(body, DT_PIXEL_APPLY_DPI(12));
  gtk_widget_set_margin_end(body, DT_PIXEL_APPLY_DPI(12));
  gtk_widget_set_name(body, "ioporder-endpoint");

  dt_capitalize_label(node->endpoint_label);
  gtk_label_set_text(GTK_LABEL(title), node->endpoint_label);
  gtk_widget_set_halign(title, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(title, GTK_ALIGN_CENTER);
  gtk_label_set_xalign(GTK_LABEL(title), 0.5f);
  gtk_widget_set_name(title, "iop-panel-label");

  gtk_box_pack_start(GTK_BOX(body), title, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(frame), body);
  gtk_container_add(GTK_CONTAINER(event_box), frame);

  node->event_box = event_box;
  node->header = body;
  node->title = title;

  return node;
}

/**
 * @brief Refresh the full graph contents from the current darkroom pipeline.
 *
 * We rebuild the graph from scratch because the visible subset, runtime
 * descriptors, mask links, and reorder boundaries all change together whenever
 * the pipe changes.
 *
 * @param self The ioporder lib module.
 */
static void _ioporder_rebuild_graph(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  if(!d || !d->graph_fixed || !d->graph_overlay) return;

  _ioporder_refresh_toolbar(self);
  _ioporder_clear_graph(d);

  int x = DT_IOPORDER_GRAPH_LEFT_MARGIN;
  int visible_count = 0;
  const gboolean raw_input = darktable.develop && darktable.develop->image_storage.dsc.cst == IOP_CS_RAW;
  gboolean colorin_crossed = FALSE;
  const int endpoint_height = DT_PIXEL_APPLY_DPI(64);
  const int base_x = x;
  int screen_x = x;
  int endpoint_y = (DT_IOPORDER_GRAPH_HEIGHT) / 2;
  int module_header_center_y = 0;

  x += DT_IOPORDER_GRAPH_NODE_STEP;

  /* Walk the sorted module list and instantiate one graph node per visible module. */
  for(GList *modules = darktable.develop ? darktable.develop->iop : NULL; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *module = (dt_iop_module_t *)modules->data;
    if(!_ioporder_module_is_graph_visible(module)) continue;

    dt_dev_pixelpipe_iop_t *piece = _ioporder_get_preview_piece(module);
    const dt_iop_buffer_dsc_t *const dsc_in = piece ? &piece->dsc_in : NULL;
    const dt_iop_buffer_dsc_t *const dsc_out = piece ? &piece->dsc_out : NULL;
    const char *display_in
        = _ioporder_runtime_band_label(_ioporder_runtime_band_kind(raw_input, colorin_crossed, module, dsc_in, FALSE),
                                       dsc_in);
    const char *display_out
        = _ioporder_runtime_band_label(_ioporder_runtime_band_kind(raw_input, colorin_crossed, module, dsc_out, TRUE),
                                       dsc_out);
    dt_ioporder_graph_node_t *node = _ioporder_create_graph_node(module, piece, display_in, display_out);
    d->nodes = g_list_append(d->nodes, node);

    gtk_drag_source_set(node->event_box, GDK_BUTTON1_MASK, _ioporder_target_list, _ioporder_target_count,
                        GDK_ACTION_COPY);
    gtk_drag_dest_set(node->event_box, GTK_DEST_DEFAULT_MOTION | GTK_DEST_DEFAULT_DROP,
                      _ioporder_target_list, _ioporder_target_count, GDK_ACTION_COPY);
    g_signal_connect(node->event_box, "drag-begin", G_CALLBACK(_ioporder_drag_begin), self);
    g_signal_connect(node->event_box, "drag-data-get", G_CALLBACK(_ioporder_drag_data_get), self);
    g_signal_connect(node->event_box, "drag-end", G_CALLBACK(_ioporder_drag_end), self);
    g_signal_connect(node->event_box, "drag-motion", G_CALLBACK(_ioporder_drag_motion), self);
    g_signal_connect(node->event_box, "drag-drop", G_CALLBACK(_ioporder_drag_drop), self);
    g_signal_connect(node->event_box, "drag-data-received", G_CALLBACK(_ioporder_drag_data_received), self);
    g_signal_connect(node->event_box, "drag-leave", G_CALLBACK(_ioporder_drag_leave), self);

    gtk_fixed_put(GTK_FIXED(d->graph_fixed), node->event_box, x, DT_IOPORDER_GRAPH_TOP_MARGIN);
    gtk_widget_show_all(node->event_box);
    if(module_header_center_y == 0)
    {
      GtkRequisition minimum = { 0 }, natural = { 0 };
      gtk_widget_get_preferred_size(node->header, &minimum, &natural);
      module_header_center_y = DT_IOPORDER_GRAPH_TOP_MARGIN + natural.height / 2;
    }

    x += DT_IOPORDER_GRAPH_NODE_STEP;
    visible_count++;
    if(!strcmp(module->op, "colorin")) colorin_crossed = TRUE;
  }

  if(module_header_center_y > 0)
    endpoint_y = module_header_center_y - endpoint_height / 2;

  dt_ioporder_graph_node_t *base_node = _ioporder_create_endpoint_node(_("base image"));
  d->nodes = g_list_prepend(d->nodes, base_node);
  gtk_fixed_put(GTK_FIXED(d->graph_fixed), base_node->event_box, base_x, endpoint_y);
  gtk_widget_show_all(base_node->event_box);
  visible_count++;

  screen_x = x;
  dt_ioporder_graph_node_t *screen_node = _ioporder_create_endpoint_node(_("screen"));
  d->nodes = g_list_append(d->nodes, screen_node);
  gtk_fixed_put(GTK_FIXED(d->graph_fixed), screen_node->event_box, screen_x, endpoint_y);
  gtk_widget_show_all(screen_node->event_box);
  visible_count++;

  const int total_width = MAX(DT_IOPORDER_GRAPH_MIN_WIDTH,
                              DT_IOPORDER_GRAPH_LEFT_MARGIN + MAX(0, visible_count - 1) * DT_IOPORDER_GRAPH_NODE_STEP
                              + DT_IOPORDER_GRAPH_NODE_WIDTH + DT_IOPORDER_GRAPH_LEFT_MARGIN);

  gtk_widget_set_size_request(d->graph_overlay, total_width, DT_IOPORDER_GRAPH_HEIGHT);
  gtk_widget_set_size_request(d->graph_drawing, total_width, DT_IOPORDER_GRAPH_HEIGHT);
  gtk_widget_set_size_request(d->graph_fixed, total_width, DT_IOPORDER_GRAPH_HEIGHT);
  if(d->window && d->graph_scroll)
  {
    GdkDisplay *display = gtk_widget_get_display(d->window);
    GdkWindow *window = gtk_widget_get_window(d->window);
    if(!window)
      window = gtk_widget_get_window(dt_ui_main_window(darktable.gui->ui));

    GdkMonitor *monitor = (display && window) ? gdk_display_get_monitor_at_window(display, window) : NULL;
    if(!monitor && display)
    {
      monitor = gdk_display_get_primary_monitor(display);
      if(!monitor && gdk_display_get_n_monitors(display) > 0)
        monitor = gdk_display_get_monitor(display, 0);
    }

    GdkRectangle geometry = { 0 };
    if(monitor)
      gdk_monitor_get_workarea(monitor, &geometry);

    const int viewport_width = geometry.width > 0 ? (int)(geometry.width * 0.88) : DT_PIXEL_APPLY_DPI(1120);
    const int viewport_height = geometry.height > 0 ? (int)(geometry.height * 0.70) : DT_PIXEL_APPLY_DPI(440);
    const int graph_width = MIN(total_width, viewport_width);
    const int graph_height = MIN(DT_IOPORDER_GRAPH_HEIGHT, viewport_height);
    const int wanted_width = graph_width + DT_PIXEL_APPLY_DPI(40);
    const int wanted_height = graph_height + DT_PIXEL_APPLY_DPI(110);

    gtk_widget_set_size_request(d->graph_scroll, graph_width, graph_height);
    gtk_window_resize(GTK_WINDOW(d->window), wanted_width, wanted_height);
  }
  gtk_widget_queue_draw(d->graph_drawing);
}

/**
 * @brief Draw a filled rounded rectangle.
 *
 * @param cr Cairo context.
 * @param x Left coordinate.
 * @param y Top coordinate.
 * @param width Rectangle width.
 * @param height Rectangle height.
 * @param radius Corner radius.
 */
static void _ioporder_draw_rounded_rect(cairo_t *cr, const double x, const double y,
                                        const double width, const double height, const double radius)
{
  const double r = MIN(radius, MIN(width, height) * 0.5);

  cairo_new_sub_path(cr);
  cairo_arc(cr, x + width - r, y + r, r, -M_PI_2, 0.0);
  cairo_arc(cr, x + width - r, y + height - r, r, 0.0, M_PI_2);
  cairo_arc(cr, x + r, y + height - r, r, M_PI_2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI_2);
  cairo_close_path(cr);
}

/**
 * @brief Draw a short label with the widget font on the graph background.
 *
 * @param widget Reference widget used to create the layout.
 * @param cr Cairo context.
 * @param x Left coordinate.
 * @param y Top coordinate.
 * @param text Text to draw.
 */
static void _ioporder_draw_label(GtkWidget *widget, cairo_t *cr, const double x, const double y, const char *text)
{
  if(!text || !widget) return;

  PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
  cairo_move_to(cr, x, y);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
}

/**
 * @brief Draw a straight sequence arrow between two consecutive module nodes.
 *
 * @param cr Cairo context.
 * @param x1 Arrow start X.
 * @param y1 Arrow start Y.
 * @param x2 Arrow end X.
 * @param y2 Arrow end Y.
 */
static void _ioporder_draw_sequence_arrow(cairo_t *cr, const double x1, const double y1,
                                          const double x2, const double y2)
{
  cairo_move_to(cr, x1, y1);
  cairo_line_to(cr, x2, y2);
  cairo_stroke(cr);

  const double angle = atan2(y2 - y1, x2 - x1);
  const double arrow = DT_PIXEL_APPLY_DPI(8);

  cairo_move_to(cr, x2, y2);
  cairo_line_to(cr, x2 - arrow * cos(angle - M_PI / 6.0), y2 - arrow * sin(angle - M_PI / 6.0));
  cairo_move_to(cr, x2, y2);
  cairo_line_to(cr, x2 - arrow * cos(angle + M_PI / 6.0), y2 - arrow * sin(angle + M_PI / 6.0));
  cairo_stroke(cr);
}

/**
 * @brief Draw a curved raster-mask dependency arrow between two nodes.
 *
 * @param widget Reference widget used to render the arrow annotation.
 * @param cr Cairo context.
 * @param sx Source X.
 * @param sy Source Y.
 * @param dx Destination X.
 * @param dy Destination Y.
 */
static void _ioporder_draw_mask_arrow(GtkWidget *widget, cairo_t *cr, const double sx, const double sy,
                                      const double dx, const double dy)
{
  GtkAllocation area = { 0 };
  gtk_widget_get_allocation(widget, &area);

  const double span = fabs(dx - sx);
  const double lift = DT_PIXEL_APPLY_DPI(36) + span * 0.20;
  const double cy = MIN(area.height - DT_IOPORDER_MASK_BOTTOM_MARGIN, MAX(sy, dy) + lift);
  const double c1x = sx + (dx - sx) * 0.25;
  const double c2x = sx + (dx - sx) * 0.75;

  cairo_move_to(cr, sx, sy);
  cairo_curve_to(cr, c1x, cy, c2x, cy, dx, dy);
  cairo_stroke(cr);

  const double arrow = DT_PIXEL_APPLY_DPI(8);
  const double tx = dx - c2x;
  const double ty = dy - cy;
  const double angle = atan2(ty, tx);

  cairo_move_to(cr, dx, dy);
  cairo_line_to(cr, dx - arrow * cos(angle - M_PI / 6.0), dy - arrow * sin(angle - M_PI / 6.0));
  cairo_move_to(cr, dx, dy);
  cairo_line_to(cr, dx - arrow * cos(angle + M_PI / 6.0), dy - arrow * sin(angle + M_PI / 6.0));
  cairo_stroke(cr);

  const double mx = (sx + dx) * 0.5;
  const double my = MIN(area.height - DT_PIXEL_APPLY_DPI(18), cy + DT_PIXEL_APPLY_DPI(8));
  cairo_save(cr);
  cairo_set_source_rgba(cr, 0.12, 0.12, 0.12, 0.85);
  _ioporder_draw_rounded_rect(cr, mx - DT_PIXEL_APPLY_DPI(12), my - DT_PIXEL_APPLY_DPI(12),
                              DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(6));
  cairo_fill(cr);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  dtgtk_cairo_paint_showmask(cr, (int)(mx - DT_PIXEL_APPLY_DPI(7)), (int)(my - DT_PIXEL_APPLY_DPI(7)),
                             DT_PIXEL_APPLY_DPI(14), DT_PIXEL_APPLY_DPI(14), 0, NULL);
  cairo_restore(cr);

  (void)widget;
}

/**
 * @brief Paint the graph background, runtime bands, fences, and arrows.
 *
 * @param widget Drawing area used as graph background.
 * @param cr Cairo context.
 * @param user_data The ioporder lib module.
 * @return FALSE to continue default GTK drawing.
 */
static gboolean _ioporder_graph_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  if(!d) return FALSE;

  GtkAllocation area = { 0 };
  gtk_widget_get_allocation(widget, &area);

  gtk_render_background(gtk_widget_get_style_context(widget), cr, 0.0, 0.0, area.width, area.height);

  if(!d->nodes)
  {
    cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
    _ioporder_draw_label(widget, cr, DT_PIXEL_APPLY_DPI(20), DT_PIXEL_APPLY_DPI(20),
                         _("No active or history modules are currently available."));
    return FALSE;
  }

  const gboolean raw_input = darktable.develop && darktable.develop->image_storage.dsc.cst == IOP_CS_RAW;

  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2));

  gboolean colorin_crossed = FALSE;
  dt_ioporder_runtime_band_kind_t segment_kind = DT_IOPORDER_RUNTIME_BAND_NONE;
  dt_iop_colorspace_type_t segment_cst = IOP_CS_NONE;
  const dt_iop_order_iccprofile_info_t *segment_profile = NULL;
  gchar *segment_label = NULL;
  GdkRGBA segment_color = { 0 };
  double segment_x = 0.0;
  double segment_width = 0.0;

  /* Draw the runtime colorspace band by merging consecutive nodes with the same runtime state. */
  for(GList *iter = d->nodes; iter; iter = g_list_next(iter))
  {
    const dt_ioporder_graph_node_t *node = (const dt_ioporder_graph_node_t *)iter->data;
    if(node->is_endpoint || !node->module) continue;
    GtkAllocation alloc = { 0 };
    gtk_widget_get_allocation(node->event_box, &alloc);

    const dt_iop_buffer_dsc_t *const dsc_out = node->piece ? &node->piece->dsc_out : NULL;
    const dt_ioporder_runtime_band_kind_t band_kind
        = _ioporder_runtime_band_kind(raw_input, colorin_crossed, node->module, dsc_out, TRUE);
    const dt_iop_order_iccprofile_info_t *profile_info
        = _ioporder_runtime_band_profile_info(raw_input, colorin_crossed, node->module, dsc_out, TRUE);
    const char *band_label = _ioporder_runtime_band_label(band_kind, dsc_out);
    gchar *band_text = _ioporder_runtime_band_text(band_label, profile_info);
    GdkRGBA band_color = { 0 };
    _ioporder_runtime_band_color(band_kind, &band_color);

    if(!segment_label || segment_kind != band_kind || segment_cst != (dsc_out ? dsc_out->cst : IOP_CS_NONE)
       || !_ioporder_same_runtime_profile(segment_profile, profile_info))
    {
      if(segment_label)
      {
        cairo_set_source_rgba(cr, segment_color.red, segment_color.green, segment_color.blue, segment_color.alpha);
        _ioporder_draw_rounded_rect(cr, segment_x, DT_PIXEL_APPLY_DPI(18), segment_width, DT_IOPORDER_BAND_HEIGHT,
                                    DT_PIXEL_APPLY_DPI(8));
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
        _ioporder_draw_label(widget, cr, segment_x + DT_PIXEL_APPLY_DPI(10), DT_PIXEL_APPLY_DPI(22), segment_label);
        dt_free(segment_label);
      }

      segment_kind = band_kind;
      segment_cst = dsc_out ? dsc_out->cst : IOP_CS_NONE;
      segment_profile = profile_info;
      segment_label = band_text;
      segment_color = band_color;
      segment_x = alloc.x;
      segment_width = alloc.width;
    }
    else
    {
      dt_free(band_text);
      segment_width = alloc.x + alloc.width - segment_x;
    }
    if(!strcmp(node->module->op, "colorin")) colorin_crossed = TRUE;
  }

  if(segment_label)
  {
    cairo_set_source_rgba(cr, segment_color.red, segment_color.green, segment_color.blue, segment_color.alpha);
    _ioporder_draw_rounded_rect(cr, segment_x, DT_PIXEL_APPLY_DPI(16), segment_width, DT_IOPORDER_BAND_HEIGHT,
                                DT_PIXEL_APPLY_DPI(8));
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    _ioporder_draw_label(widget, cr, segment_x + DT_PIXEL_APPLY_DPI(10), DT_PIXEL_APPLY_DPI(22), segment_label);
    dt_free(segment_label);
  }

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2));

  /* Draw every fence as a background boundary that modules must not cross. */
  for(GList *iter = d->nodes; iter; iter = g_list_next(iter))
  {
    const dt_ioporder_graph_node_t *node = (const dt_ioporder_graph_node_t *)iter->data;
    if(node->is_endpoint || !node->module) continue;
    if(!(node->module->flags() & IOP_FLAGS_FENCE)) continue;

    GtkAllocation alloc = { 0 };
    gtk_widget_get_allocation(node->event_box, &alloc);
    const double x = alloc.x - DT_PIXEL_APPLY_DPI(12);
    cairo_move_to(cr, x, DT_PIXEL_APPLY_DPI(12));
    cairo_line_to(cr, x, area.height - DT_PIXEL_APPLY_DPI(12));
  }
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.90);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(2));

  /* Consecutive nodes are laid out left-to-right in processing order, so we
   * keep the arrow head on the right-hand successor. */
  for(GList *iter = d->nodes; iter && g_list_next(iter); iter = g_list_next(iter))
  {
    const dt_ioporder_graph_node_t *node_a = (const dt_ioporder_graph_node_t *)iter->data;
    const dt_ioporder_graph_node_t *node_b = (const dt_ioporder_graph_node_t *)g_list_next(iter)->data;
    GtkAllocation alloc_a = { 0 }, alloc_b = { 0 }, head_a = { 0 }, head_b = { 0 };
    gtk_widget_get_allocation(node_a->event_box, &alloc_a);
    gtk_widget_get_allocation(node_b->event_box, &alloc_b);
    gtk_widget_get_allocation(node_a->header, &head_a);
    gtk_widget_get_allocation(node_b->header, &head_b);
    const double arrow_y_a = node_a->is_endpoint ? alloc_a.y + alloc_a.height * 0.5
                                                 : alloc_a.y + head_a.y + head_a.height * 0.5;
    const double arrow_y_b = node_b->is_endpoint ? alloc_b.y + alloc_b.height * 0.5
                                                 : alloc_b.y + head_b.y + head_b.height * 0.5;

    _ioporder_draw_sequence_arrow(cr, alloc_a.x + alloc_a.width + DT_PIXEL_APPLY_DPI(6),
                                  arrow_y_a,
                                  alloc_b.x - DT_PIXEL_APPLY_DPI(6),
                                  arrow_y_b);
  }

  cairo_set_source_rgba(cr, 0.95, 0.54, 0.13, 0.92);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3));

  /* Walk every visible module and draw one orange dependency arrow per raster-mask consumer. */
  for(GList *iter = d->nodes; iter; iter = g_list_next(iter))
  {
    const dt_ioporder_graph_node_t *source = (const dt_ioporder_graph_node_t *)iter->data;
    if(source->is_endpoint || !source->module) continue;
    GtkAllocation source_alloc = { 0 };
    gtk_widget_get_allocation(source->event_box, &source_alloc);

    for(GList *consumers = d->nodes; consumers; consumers = g_list_next(consumers))
    {
      const dt_ioporder_graph_node_t *sink = (const dt_ioporder_graph_node_t *)consumers->data;
      if(sink->is_endpoint || !sink->module) continue;
      if(sink->module->raster_mask.sink.source != source->module) continue;

      GtkAllocation sink_alloc = { 0 };
      gtk_widget_get_allocation(sink->event_box, &sink_alloc);

      _ioporder_draw_mask_arrow(widget, cr,
                                source_alloc.x + source_alloc.width * 0.5,
                                source_alloc.y + source_alloc.height + DT_IOPORDER_MASK_ARROW_OFFSET,
                                sink_alloc.x + sink_alloc.width * 0.5,
                                sink_alloc.y + sink_alloc.height + DT_IOPORDER_MASK_ARROW_OFFSET);
    }
  }

  if(d->drag_source && d->drag_dest && d->drag_source != d->drag_dest)
  {
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.95);
    cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(4));

    /* Materialize the drop position with a bright insertion line. */
    for(GList *iter = d->nodes; iter; iter = g_list_next(iter))
    {
      const dt_ioporder_graph_node_t *node = (const dt_ioporder_graph_node_t *)iter->data;
      if(node->module != d->drag_dest) continue;

      GtkAllocation alloc = { 0 };
      gtk_widget_get_allocation(node->event_box, &alloc);
      const double x = d->drag_source->iop_order < d->drag_dest->iop_order
                           ? alloc.x + alloc.width + DT_PIXEL_APPLY_DPI(10)
                           : alloc.x - DT_PIXEL_APPLY_DPI(10);
      cairo_move_to(cr, x, DT_PIXEL_APPLY_DPI(12));
      cairo_line_to(cr, x, area.height - DT_PIXEL_APPLY_DPI(12));
      cairo_stroke(cr);
      break;
    }
  }

  return FALSE;
}

/**
 * @brief Handle drag-data export for the graph node drag and drop.
 *
 * @param widget Source node widget.
 * @param context Drag context.
 * @param selection_data Drag payload.
 * @param info Target info id.
 * @param time Event timestamp.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_drag_data_get(GtkWidget *widget, GdkDragContext *context,
                                    GtkSelectionData *selection_data, guint info, guint time,
                                    gpointer user_data)
{
  guint value = 1;
  gtk_selection_data_set(selection_data, gdk_atom_intern("ioporder-node", TRUE), 32,
                         (const guchar *)&value, 1);
}

/**
 * @brief Track the module being dragged from the graph.
 *
 * @param widget Source node widget.
 * @param context Drag context.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  d->drag_source = (dt_iop_module_t *)g_object_get_data(G_OBJECT(widget), "dt-ioporder-module");
  d->drag_dest = NULL;
  gtk_widget_queue_draw(d->graph_drawing);
}

/**
 * @brief Reset drag feedback when the source drag ends.
 *
 * @param widget Source node widget.
 * @param context Drag context.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_drag_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  d->drag_source = NULL;
  d->drag_dest = NULL;
  gtk_widget_queue_draw(d->graph_drawing);
}

/**
 * @brief Validate the potential drop target while dragging across graph nodes.
 *
 * @param widget Destination node widget under the pointer.
 * @param dc Drag context.
 * @param x Local X coordinate.
 * @param y Local Y coordinate.
 * @param time Event timestamp.
 * @param user_data The ioporder lib module.
 * @return TRUE when the drop target is valid.
 */
static gboolean _ioporder_drag_motion(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                      guint time, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  dt_iop_module_t *module_src = d->drag_source;
  dt_iop_module_t *module_dest = (dt_iop_module_t *)g_object_get_data(G_OBJECT(widget), "dt-ioporder-module");

  gboolean can_move = FALSE;
  if(module_src && module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
      can_move = dt_ioppr_check_can_move_after_iop(darktable.develop->iop, module_src, module_dest);
    else
      can_move = dt_ioppr_check_can_move_before_iop(darktable.develop->iop, module_src, module_dest);
  }

  d->drag_dest = can_move ? module_dest : NULL;
  gtk_widget_queue_draw(d->graph_drawing);
  gdk_drag_status(dc, can_move ? GDK_ACTION_COPY : 0, time);
  return can_move;
}

/**
 * @brief Request the drag payload when dropping on a graph node.
 *
 * @param widget Destination node widget.
 * @param dc Drag context.
 * @param x Local X coordinate.
 * @param y Local Y coordinate.
 * @param time Event timestamp.
 * @param user_data The ioporder lib module.
 * @return Always TRUE because data retrieval continues asynchronously.
 */
static gboolean _ioporder_drag_drop(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                    guint time, gpointer user_data)
{
  gtk_drag_get_data(widget, dc, gdk_atom_intern("ioporder-node", TRUE), time);
  return TRUE;
}

/**
 * @brief Clear drop feedback when leaving a graph node during drag.
 *
 * @param widget Destination node widget.
 * @param dc Drag context.
 * @param time Event timestamp.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_drag_leave(GtkWidget *widget, GdkDragContext *dc, guint time, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  d->drag_dest = NULL;
  gtk_widget_queue_draw(d->graph_drawing);
}

/**
 * @brief Commit a module reorder after dropping on another graph node.
 *
 * This mirrors darkroom's reorder flow: update the order list, reorder the
 * visible expander widgets, rebuild the pipe, append history, and raise the
 * module-moved signal.
 *
 * @param widget Drop target node widget.
 * @param dc Drag context.
 * @param x Local X coordinate.
 * @param y Local Y coordinate.
 * @param selection_data Drag payload.
 * @param info Target info id.
 * @param time Event timestamp.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_drag_data_received(GtkWidget *widget, GdkDragContext *dc, gint x, gint y,
                                         GtkSelectionData *selection_data, guint info, guint time,
                                         gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  dt_iop_module_t *module_src = d->drag_source;
  dt_iop_module_t *module_dest = (dt_iop_module_t *)g_object_get_data(G_OBJECT(widget), "dt-ioporder-module");

  if(module_src && module_dest && module_src != module_dest)
  {
    if(module_src->iop_order < module_dest->iop_order)
      dt_iop_gui_move_module_after(module_src, module_dest, "_ioporder_drag_data_received");
    else
      dt_iop_gui_move_module_before(module_src, module_dest, "_ioporder_drag_data_received");
  }

  gtk_drag_finish(dc, TRUE, FALSE, time);
  d->drag_source = NULL;
  d->drag_dest = NULL;

  _ioporder_rebuild_graph(self);
}

/**
 * @brief Save the current pipeline order as a named preset.
 *
 * @param button Toolbar add-preset button.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_add_preset(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  GtkWindow *parent = GTK_WINDOW(d->window ? d->window : dt_ui_main_window(darktable.gui->ui));
  GtkWidget *dialog = gtk_dialog_new_with_buttons(_("save module order preset"), parent,
                                                  GTK_DIALOG_DESTROY_WITH_PARENT,
                                                  _("_cancel"), GTK_RESPONSE_CANCEL,
                                                  _("_save"), GTK_RESPONSE_ACCEPT, NULL);
  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *entry = gtk_entry_new();

  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_widget_set_tooltip_text(entry, _("preset name"));
  gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, DT_PIXEL_APPLY_DPI(8));
  gtk_widget_show_all(dialog);

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  g_signal_connect(entry, "key-press-event", G_CALLBACK(dt_handle_dialog_enter), dialog);

  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    const char *preset_name = gtk_entry_get_text(GTK_ENTRY(entry));
    if(preset_name && preset_name[0] != '\0')
    {
      int size = 0;
      void *params = self->get_params(self, &size);
      dt_lib_presets_add(preset_name, self->plugin_name, self->version(), params, size, FALSE);
      dt_free(params);
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_PRESETS_CHANGED, g_strdup(self->plugin_name));
    }
  }

  gtk_widget_destroy(dialog);
  _ioporder_refresh_toolbar(self);
}

/**
 * @brief Reset the current order to the default v3.0 order.
 *
 * @param button Toolbar reset button.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_reset_order(GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  self->gui_reset(self);
  _ioporder_rebuild_graph(self);
}

/**
 * @brief Apply the preset selected in the popup toolbar combo box.
 *
 * @param combo Preset selection combo box.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_apply_preset(GtkComboBox *combo, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  if(d->refreshing_toolbar) return;

  const gchar *preset_name = gtk_combo_box_get_active_id(combo);
  if(!preset_name || !strcmp(preset_name, "__custom__"))
    return;

  dt_lib_presets_apply(preset_name, self->plugin_name, self->version());
  dt_iop_gui_commit_iop_order_change(darktable.develop, NULL, FALSE, TRUE, "_ioporder_apply_preset");
  _ioporder_rebuild_graph(self);
}

/**
 * @brief Build the popup window lazily on first use.
 *
 * The popup owns the toolbar and the scrollable graph surface and is kept alive
 * for the whole darkroom session so updates only need to refresh its contents.
 *
 * @param self The ioporder lib module.
 */
static void _ioporder_init_popup(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  if(d->window) return;

  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(8));
  GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(8));
  GtkWidget *label = gtk_label_new("");
  GtkWidget *add_preset = gtk_button_new_with_label(_("add preset"));
  GtkWidget *preset_combo = gtk_combo_box_text_new();
  GtkWidget *reset = gtk_button_new_with_label(_("reset"));
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  GtkWidget *overlay = gtk_overlay_new();
  GtkWidget *drawing = gtk_drawing_area_new();
  GtkWidget *fixed = gtk_fixed_new();

  gtk_window_set_title(GTK_WINDOW(window), _("module order"));
  gtk_window_set_default_size(GTK_WINDOW(window), DT_PIXEL_APPLY_DPI(1120), DT_PIXEL_APPLY_DPI(440));
  gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
  gtk_container_set_border_width(GTK_CONTAINER(root), DT_PIXEL_APPLY_DPI(8));

  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);

  gtk_widget_set_hexpand(preset_combo, TRUE);
  gtk_widget_set_hexpand(scroll, TRUE);
  gtk_widget_set_vexpand(scroll, TRUE);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_IN);

  gtk_widget_set_size_request(overlay, DT_IOPORDER_GRAPH_MIN_WIDTH, DT_IOPORDER_GRAPH_HEIGHT);
  gtk_widget_set_size_request(drawing, DT_IOPORDER_GRAPH_MIN_WIDTH, DT_IOPORDER_GRAPH_HEIGHT);
  gtk_widget_set_size_request(fixed, DT_IOPORDER_GRAPH_MIN_WIDTH, DT_IOPORDER_GRAPH_HEIGHT);

  gtk_box_pack_start(GTK_BOX(toolbar), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(toolbar), add_preset, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(toolbar), preset_combo, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(toolbar), reset, FALSE, FALSE, 0);

  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), fixed);
  gtk_container_add(GTK_CONTAINER(overlay), drawing);
  gtk_container_add(GTK_CONTAINER(scroll), overlay);

  gtk_box_pack_start(GTK_BOX(root), toolbar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(window), root);

  g_signal_connect(window, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
  g_signal_connect(add_preset, "clicked", G_CALLBACK(_ioporder_add_preset), self);
  g_signal_connect(reset, "clicked", G_CALLBACK(_ioporder_reset_order), self);
  g_signal_connect(preset_combo, "changed", G_CALLBACK(_ioporder_apply_preset), self);
  g_signal_connect(drawing, "draw", G_CALLBACK(_ioporder_graph_draw), self);

  d->window = window;
  d->toolbar_label = label;
  d->preset_combo = preset_combo;
  d->graph_scroll = scroll;
  d->graph_overlay = overlay;
  d->graph_drawing = drawing;
  d->graph_fixed = fixed;

  _ioporder_refresh_toolbar(self);
  _ioporder_rebuild_graph(self);
}

/**
 * @brief Open the popup window owned by the ioporder lib.
 *
 * The module is now headless in the side panels, so darkroom launches the
 * popup explicitly through the lib API instead of proxying a hidden widget.
 *
 * @param self The ioporder lib module.
 */
void show_popup(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;

  _ioporder_init_popup(self);
  _ioporder_rebuild_graph(self);

  gtk_widget_show_all(d->window);
  gtk_window_present(GTK_WINDOW(d->window));
}

/**
 * @brief Central refresh callback for develop-side state changes.
 *
 * @param instance Signal instance.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_refresh_callback(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  if(d->window && gtk_widget_get_visible(d->window)) _ioporder_rebuild_graph(self);
}

/**
 * @brief Refresh the preset toolbar when ioporder presets change.
 *
 * @param instance Signal instance.
 * @param module_name Plugin name carried by the preset-changed signal.
 * @param user_data The ioporder lib module.
 */
static void _ioporder_presets_changed_callback(gpointer instance, gpointer module_name, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  const char *changed_module = (const char *)module_name;
  if(changed_module && strcmp(changed_module, self->plugin_name) != 0) return;

  _ioporder_refresh_toolbar(self);
  if(d->window && gtk_widget_get_visible(d->window)) _ioporder_rebuild_graph(self);
}

const char *name(struct dt_lib_module_t *self)
{
  return _("module order");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "special", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 0;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)calloc(1, sizeof(dt_lib_ioporder_t));
  self->data = (void *)d;

  d->current_mode = -1;

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED,
                                  G_CALLBACK(_ioporder_refresh_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE,
                                  G_CALLBACK(_ioporder_refresh_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_HISTORY_CHANGE,
                                  G_CALLBACK(_ioporder_refresh_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_MODULE_MOVED,
                                  G_CALLBACK(_ioporder_refresh_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                                  G_CALLBACK(_ioporder_refresh_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PRESETS_CHANGED,
                                  G_CALLBACK(_ioporder_presets_changed_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  if(!d) return;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_ioporder_refresh_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_ioporder_presets_changed_callback), self);

  if(d->window) gtk_widget_destroy(d->window);
  _ioporder_clear_graph(d);
  dt_free(self->data);
  self->data = NULL;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_ioporder_t *d = (dt_lib_ioporder_t *)self->data;
  GList *iop_order_list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30);
  if(!iop_order_list) return;

  const int32_t imgid = darktable.develop->image_storage.id;
  dt_ioppr_change_iop_order(darktable.develop, imgid, iop_order_list);
  dt_iop_gui_commit_iop_order_change(darktable.develop, NULL, FALSE, FALSE, "dt_lib_ioporder_gui_reset");

  d->current_mode = DT_IOP_ORDER_V30;
  g_list_free_full(iop_order_list, dt_free_gpointer);
}

void init_presets(dt_lib_module_t *self)
{
  size_t size = 0;
  char *params = NULL;
  GList *list = NULL;

  list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_LEGACY);
  params = dt_ioppr_serialize_iop_order_list(list, &size);
  dt_lib_presets_add(_("legacy"), self->plugin_name, self->version(), params, (int32_t)size, TRUE);
  dt_free(params);
  g_list_free_full(list, dt_free_gpointer);

  list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30);
  params = dt_ioppr_serialize_iop_order_list(list, &size);
  dt_lib_presets_add(_("v3.0 for RAW input (default)"), self->plugin_name, self->version(), params,
                     (int32_t)size, TRUE);
  dt_free(params);
  g_list_free_full(list, dt_free_gpointer);

  list = dt_ioppr_get_iop_order_list_version(DT_IOP_ORDER_V30_JPG);
  params = dt_ioppr_serialize_iop_order_list(list, &size);
  dt_lib_presets_add(_("v3.0 for JPEG/non-RAW input"), self->plugin_name, self->version(), params,
                     (int32_t)size, TRUE);
  dt_free(params);
  g_list_free_full(list, dt_free_gpointer);
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  if(!params) return 1;

  GList *iop_order_list = dt_ioppr_deserialize_iop_order_list(params, (size_t)size);
  if(!iop_order_list) return 1;

  const int32_t imgid = darktable.develop->image_storage.id;
  dt_ioppr_change_iop_order(darktable.develop, imgid, iop_order_list);
  dt_iop_gui_commit_iop_order_change(darktable.develop, NULL, FALSE, FALSE, "dt_lib_ioporder_set_params");
  g_list_free_full(iop_order_list, dt_free_gpointer);
  return 0;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  size_t p_size = 0;
  void *params = dt_ioppr_serialize_iop_order_list(darktable.develop->iop_order_list, &p_size);
  *size = (int)p_size;
  return params;
}

gboolean preset_autoapply(dt_lib_module_t *self)
{
  return TRUE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
