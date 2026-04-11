/*
    This file is part of Ansel,
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

#include "common/history_merge_gui.h"

#include "common/darktable.h"
#include "common/debug.h"
#include "common/iop_order.h"
#include "common/topological_sort.h"
#include "develop/blend.h"
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "gui/gtk.h"

#include <glib.h>
#include <string.h>

typedef struct
{
  // Bitmask of `_hm_id_origin_t` describing where the id was seen.
  guint flags;
  // Non-owning pointer to the module instance from the pasted set (if any).
  const dt_iop_module_t *mod_list;
  // Non-owning pointer to the module instance in the source pipeline (if any).
  const dt_iop_module_t *src_iop;
  // Non-owning pointer to the module instance in the destination pipeline (if any).
  dt_iop_module_t *dst_iop;
} _hm_id_info_t;

static gchar *_hm_clean_module_name(const dt_iop_module_t *mod)
{
  const char *raw = (mod && mod->name) ? mod->name() : (mod ? mod->op : "");
  gchar *clean = delete_underscore(raw ? raw : "");
  dt_capitalize_label(clean);
  return clean;
}

static gchar *_hm_module_label_short(const dt_iop_module_t *mod)
{
  gchar *name = _hm_clean_module_name(mod);
  if(IS_NULL_PTR(name)) return g_strdup("");
  if(mod && mod->multi_name[0] != '\0')
  {
    gchar *out = g_strdup_printf("%s (%s)", name, mod->multi_name);
    dt_free(name);
    return out;
  }
  return name;
}

static gchar *_hm_pretty_id(const char *id)
{
  /* Convert a raw node id ("op|multi_name") to a human-friendly string. */
  if(IS_NULL_PTR(id)) return g_strdup("");

  char op[sizeof(((dt_dev_history_item_t *)0)->op_name)] = { 0 };
  char name[sizeof(((dt_dev_history_item_t *)0)->multi_name)] = { 0 };
  _hm_id_to_op_name(id, op, name);
  if(name[0] == '\0') return g_strdup(op);
  return g_strdup_printf("%s (%s)", op, name);
}

static gchar *_hm_pretty_id_from_id_ht(const char *id, GHashTable *id_ht, const gboolean prefer_dest)
{
  /* Turn a node id into a label suitable for GTK dialogs. */
  if(IS_NULL_PTR(id)) return g_strdup("");

  const _hm_id_info_t *info = id_ht ? (const _hm_id_info_t *)g_hash_table_lookup(id_ht, id) : NULL;
  const dt_iop_module_t *mod = NULL;

  if(info)
  {
    if(prefer_dest && info->dst_iop)
      mod = info->dst_iop;
    else if(!prefer_dest && info->src_iop)
      mod = info->src_iop;

    if(IS_NULL_PTR(mod)) mod = info->dst_iop ? info->dst_iop : (info->src_iop ? info->src_iop : info->mod_list);
  }

  if(mod)
  {
    gchar *name = _hm_clean_module_name(mod);
    if(mod->multi_name[0] == '\0') return name;
    gchar *out = g_strdup_printf("%s (%s)", name ? name : "", mod->multi_name);
    dt_free(name);
    return out;
  }

  return _hm_pretty_id(id);
}

static gchar *_hm_cycle_node_label(const dt_digraph_node_t *n, GHashTable *id_ht)
{
  /* Wrapper around `_hm_pretty_id_from_id_ht()` for cycle nodes. */
  return _hm_pretty_id_from_id_ht(n ? n->id : NULL, id_ht, TRUE);
}

static void _hm_append_cycle_label(GString *out, const char *s, const gboolean bold)
{
  gchar *esc = g_markup_escape_text(s ? s : "", -1);
  if(bold) g_string_append_printf(out, "<b>%s</b>", esc);
  else g_string_append(out, esc);
  dt_free(esc);
}

dt_hm_constraint_choice_t _hm_ask_user_constraints_choice(GHashTable *id_ht, const char *faulty_id,
                                                          const char *src_prev, const char *src_next,
                                                          const char *dst_prev, const char *dst_next)
{
  /* Ask the user how to resolve incompatible adjacency constraints between source and destination. */
  if(IS_NULL_PTR(darktable.gui)) return DT_HM_CONSTRAINTS_PREFER_DEST;
  if(!g_main_context_is_owner(g_main_context_default())) return DT_HM_CONSTRAINTS_PREFER_DEST;

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  if(IS_NULL_PTR(window)) return DT_HM_CONSTRAINTS_PREFER_DEST;

  gchar *faulty = _hm_pretty_id_from_id_ht(faulty_id, id_ht, TRUE);
  gchar *sp = _hm_pretty_id_from_id_ht(src_prev, id_ht, FALSE);
  gchar *sn = _hm_pretty_id_from_id_ht(src_next, id_ht, FALSE);
  gchar *dp = _hm_pretty_id_from_id_ht(dst_prev, id_ht, TRUE);
  gchar *dn = _hm_pretty_id_from_id_ht(dst_next, id_ht, TRUE);

  GtkDialog *dialog = GTK_DIALOG(gtk_dialog_new_with_buttons(
      _("Incompatible module ordering constraints"), GTK_WINDOW(window),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, _("Preserve _destination ordering"), GTK_RESPONSE_REJECT,
      _("Preserve _source ordering"), GTK_RESPONSE_ACCEPT, _("_Cancel"), GTK_RESPONSE_CANCEL, NULL));

  gtk_dialog_set_default_response(dialog, GTK_RESPONSE_REJECT);

  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 80);

  gchar *text = g_strdup_printf(_("Two modules require each other as predecessor, creating a 2-cycle.\n\n"
                                  "Faulty module: %s\n\n"
                                  "Destination wants: %s → %s → %s\n"
                                  "Source wants:      %s → %s → %s\n\n"
                                  "Which ordering constraints should be preserved?"),
                                faulty, dp, faulty, dn, sp, faulty, sn);

  gtk_label_set_text(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(content_area), label, TRUE, TRUE, 0);

  gtk_widget_show_all(GTK_WIDGET(dialog));
  const int res = gtk_dialog_run(dialog);
  gtk_widget_destroy(GTK_WIDGET(dialog));

  dt_free(text);
  dt_free(faulty);
  dt_free(sp);
  dt_free(sn);
  dt_free(dp);
  dt_free(dn);

  if(res == GTK_RESPONSE_ACCEPT) return DT_HM_CONSTRAINTS_PREFER_SRC;
  return DT_HM_CONSTRAINTS_PREFER_DEST;
}

gboolean _hm_warn_missing_raster_producers(const GList *mod_list)
{
  /* Warn the user when pasted modules rely on raster masks that will be missing. */
  if(IS_NULL_PTR(darktable.gui)) return TRUE;
  if(!g_main_context_is_owner(g_main_context_default())) return TRUE;

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  if(IS_NULL_PTR(window)) return TRUE;

  GHashTable *mods = g_hash_table_new(g_direct_hash, g_direct_equal);
  for(const GList *l = g_list_first((GList *)mod_list); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    if(mod) g_hash_table_add(mods, (gpointer)mod);
  }

  GString *lines = g_string_new("");
  for(const GList *l = g_list_first((GList *)mod_list); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    if(IS_NULL_PTR(mod)) continue;
    const dt_iop_module_t *producer = mod->raster_mask.sink.source;
    if(IS_NULL_PTR(producer)) continue;

    const gboolean missing = !producer || !g_hash_table_contains(mods, producer);
    if(missing)
    {
      gchar *user = _hm_module_label_short(mod);
      gchar *prod = _hm_module_label_short(producer);
      g_string_append_printf(lines, "• %s → %s\n", user, prod);
      dt_free(user);
      dt_free(prod);
    }
  }

  g_hash_table_destroy(mods);

  if(lines->len == 0)
  {
    g_string_free(lines, TRUE);
    return TRUE;
  }

  GtkDialog *dialog = GTK_DIALOG(gtk_dialog_new_with_buttons(
      _("Missing raster mask producers"), GTK_WINDOW(window),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, _("_Cancel merge"), GTK_RESPONSE_CANCEL, _("_Continue"),
      GTK_RESPONSE_ACCEPT, NULL));
  gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);
  gtk_dialog_set_default_response(dialog, GTK_RESPONSE_ACCEPT);

  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_widget_set_valign(label, GTK_ALIGN_START);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 90);

  gchar *text = g_strdup_printf(
      _("Some pasted modules use raster masks produced by modules that were not included.\n"
        "Those masks will not be available after the merge.\n\n"
        "Missing producers:\n\n%s"),
      lines->str);
  gtk_label_set_text(GTK_LABEL(label), text);
  gtk_box_pack_start(GTK_BOX(content_area), label, FALSE, FALSE, 6);

  gtk_widget_show_all(GTK_WIDGET(dialog));
  const int res = gtk_dialog_run(dialog);
  gtk_widget_destroy(GTK_WIDGET(dialog));

  dt_free(text);
  g_string_free(lines, TRUE);

  return res == GTK_RESPONSE_ACCEPT;
}

void _hm_show_toposort_cycle_popup(GList *cycle_nodes, GHashTable *id_ht)
{
  /* Present a detected ordering cycle as a GTK modal popup. */
  if(IS_NULL_PTR(cycle_nodes)) return;
  if(IS_NULL_PTR(darktable.gui)) return;
  if(!g_main_context_is_owner(g_main_context_default())) return;

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  if(IS_NULL_PTR(window)) return;

  GPtrArray *labels = g_ptr_array_new_with_free_func(g_free);
  for(GList *it = g_list_first(cycle_nodes); it; it = g_list_next(it))
  {
    dt_digraph_node_t *n = (dt_digraph_node_t *)it->data;
    g_ptr_array_add(labels, _hm_cycle_node_label(n, id_ht));
  }

  GString *cycle = g_string_new("");
  for(guint i = 0; labels && i < labels->len; i++)
  {
    const char *s = (const char *)g_ptr_array_index(labels, i);
    if(i > 0) g_string_append(cycle, " → ");
    _hm_append_cycle_label(cycle, s, i == 0);
  }
  if(labels && labels->len > 0)
  {
    const char *first = (const char *)g_ptr_array_index(labels, 0);
    g_string_append(cycle, " → ");
    _hm_append_cycle_label(cycle, first, TRUE);
  }

  GtkDialog *dialog = GTK_DIALOG(gtk_dialog_new_with_buttons(
      _("Incompatible module ordering constraints"), GTK_WINDOW(window),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, _("_Close"), GTK_RESPONSE_CLOSE, NULL));
  gtk_window_set_resizable(GTK_WINDOW(dialog), FALSE);

  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  gtk_widget_set_valign(label, GTK_ALIGN_START);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 80);

  GString *text = g_string_new(NULL);
  gchar *prefix = g_markup_escape_text(
      _("Module ordering constraints contain a cycle and cannot be satisfied.\n\nCycle:\n\n"), -1);
  g_string_append(text, prefix);
  dt_free(prefix);
  g_string_append(text, cycle->str);

  gtk_label_set_markup(GTK_LABEL(label), text->str);
  gtk_box_pack_start(GTK_BOX(content_area), label, FALSE, FALSE, 6);

  gtk_widget_show_all(GTK_WIDGET(dialog));
  gtk_dialog_run(dialog);
  gtk_widget_destroy(GTK_WIDGET(dialog));

  if(text) g_string_free(text, TRUE);
  if(cycle) g_string_free(cycle, TRUE);
  if(labels) g_ptr_array_free(labels, TRUE);
}

static gchar *_hm_module_row_label(const dt_iop_module_t *mod)
{
  /* Format a module instance for the report rows: "<order> <name> (multi_name)". */
  gchar *name = _hm_clean_module_name(mod);
  if(mod->multi_name[0] == '\0')
  {
    gchar *out = g_strdup_printf("%4d  %s", mod->iop_order, name ? name : "");
    dt_free(name);
    return out;
  }
  gchar *out = g_strdup_printf("%4d  %s (%s)", mod->iop_order, name ? name : "", mod->multi_name);
  dt_free(name);
  return out;
}

static gboolean _hm_history_masks_match(const dt_dev_history_item_t *a, const dt_dev_history_item_t *b)
{
  if(IS_NULL_PTR(a) || IS_NULL_PTR(b)) return FALSE;

  const gboolean a_has_forms = (!IS_NULL_PTR(a->forms));
  const gboolean b_has_forms = (!IS_NULL_PTR(b->forms));
  if(a_has_forms != b_has_forms) return FALSE;

  const int a_mask_id = a->blend_params ? a->blend_params->mask_id : 0;
  const int b_mask_id = b->blend_params ? b->blend_params->mask_id : 0;
  if(a_mask_id != b_mask_id) return FALSE;

  if(a_has_forms && a_mask_id > 0)
  {
    dt_masks_form_t *a_form = dt_masks_get_from_id_ext(a->forms, a_mask_id);
    dt_masks_form_t *b_form = dt_masks_get_from_id_ext(b->forms, b_mask_id);
    const uint64_t a_hash = dt_masks_group_get_hash(0, a_form);
    const uint64_t b_hash = dt_masks_group_get_hash(0, b_form);
    if(a_hash != b_hash) return FALSE;
  }

  return TRUE;
}

static gboolean _hm_history_items_match(const dt_dev_history_item_t *a, const dt_dev_history_item_t *b)
{
  if(IS_NULL_PTR(a) || IS_NULL_PTR(b)) return FALSE;

  if(strcmp(a->op_name, b->op_name) != 0) return FALSE;
  if(strcmp(a->multi_name, b->multi_name) != 0) return FALSE;
  if(a->multi_priority != b->multi_priority) return FALSE;
  if(a->enabled != b->enabled) return FALSE;
  if(a->iop_order != b->iop_order) return FALSE;

  const int size_a = a->module ? a->module->params_size : 0;
  const int size_b = b->module ? b->module->params_size : 0;
  if(size_a != size_b) return FALSE;
  if(size_a > 0)
  {
    if(IS_NULL_PTR(a->params) || IS_NULL_PTR(b->params)) return FALSE;
    if(memcmp(a->params, b->params, size_a) != 0) return FALSE;
  }

  if((IS_NULL_PTR(a->blend_params)) != (IS_NULL_PTR(b->blend_params))) return FALSE;
  if(a->blend_params && b->blend_params
     && memcmp(a->blend_params, b->blend_params, sizeof(dt_develop_blend_params_t)) != 0)
    return FALSE;

  if(!_hm_history_masks_match(a, b)) return FALSE;

  return TRUE;
}

typedef enum dt_hm_report_col_t
{
  HM_REPORT_COL_ORIG = 0,
  HM_REPORT_COL_FILET,
  HM_REPORT_COL_SRC,
  HM_REPORT_COL_ARROW,
  HM_REPORT_COL_DST,
  HM_REPORT_COL_SRC_ID,
  HM_REPORT_COL_DST_ID,
  HM_REPORT_COL_SRC_WEIGHT,
  HM_REPORT_COL_DST_WEIGHT,
  HM_REPORT_COL_ORIG_STYLE,
  HM_REPORT_COL_SRC_STYLE,
  HM_REPORT_COL_DST_STYLE,
  HM_REPORT_COL_IS_INPUT,
  HM_REPORT_COL_COUNT
} dt_hm_report_col_t;

typedef struct
{
  dt_develop_t *dev_dest;      // destination develop context to reorder
  dt_develop_t *dev_src;       // source develop context for moved detection
  GtkListStore *store;         // report model to read/update after DnD
  GHashTable *dst_last_by_id;  // last history items by id (mask markers)
  GHashTable *dst_last_before_by_id; // last history items before merge
  GHashTable *override;        // override markers
  const GHashTable *orig_ids;  // original module ids (inserted markers)
  const GHashTable *mod_list_ids; // pasted module ids (show disabled)
  GtkTreePath *drag_path;      // path being dragged
  gboolean in_reorder;         // guard against recursive row-reordered signals
} _hm_report_reorder_ctx_t;

static gboolean _hm_history_item_uses_masks(const dt_dev_history_item_t *hist)
{
  if(IS_NULL_PTR(hist)) return FALSE;
  if(hist->forms) return TRUE;
  if(hist->blend_params && hist->blendop_params_size == sizeof(dt_develop_blend_params_t)
     && hist->blend_params->mask_mode > DEVELOP_MASK_ENABLED)
    return TRUE;
  return FALSE;
}

typedef struct
{
  int iop_order;
  gchar *label;
  int style;
} _hm_label_t;

static gint _hm_label_cmp(gconstpointer a, gconstpointer b)
{
  const _hm_label_t *la = (const _hm_label_t *)a;
  const _hm_label_t *lb = (const _hm_label_t *)b;
  return (la->iop_order > lb->iop_order) - (la->iop_order < lb->iop_order);
}

GPtrArray *_hm_collect_labels_from_history_map(GHashTable *last_by_id, const GHashTable *mod_list_ids,
                                               GPtrArray **out_styles)
{
  GList *labels = NULL;
  GHashTableIter it;
  gpointer key = NULL, value = NULL;
  g_hash_table_iter_init(&it, last_by_id);
  while(g_hash_table_iter_next(&it, &key, &value))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)value;
    if(!hist || !hist->module) continue;
    if(hist->module->flags() & IOP_FLAGS_NO_HISTORY_STACK) continue;
    if(!hist->enabled && (!mod_list_ids || !g_hash_table_contains((GHashTable *)mod_list_ids, key))) continue;

    gchar *label = _hm_module_row_label(hist->module);
    if(_hm_history_item_uses_masks(hist))
    {
      gchar *tmp = g_strdup_printf("%s*", label);
      dt_free(label);
      label = tmp;
    }

    _hm_label_t *item = g_malloc0(sizeof(_hm_label_t));
    item->iop_order = hist->iop_order;
    item->label = label;
    item->style = hist->enabled ? PANGO_STYLE_NORMAL : PANGO_STYLE_ITALIC;
    labels = g_list_insert_sorted(labels, item, (GCompareFunc)_hm_label_cmp);
  }

  GPtrArray *result = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *styles = g_ptr_array_new();
  for(GList *l = g_list_last(labels); l; l = g_list_previous(l))
  {
    _hm_label_t *item = (_hm_label_t *)l->data;
    g_ptr_array_add(result, item->label);
    g_ptr_array_add(styles, GINT_TO_POINTER(item->style));
    dt_free(item);
  }
  g_list_free(labels);
  labels = NULL;

  if(out_styles)
    *out_styles = styles;
  else
    g_ptr_array_free(styles, TRUE);

  return result;
}

static dt_iop_module_t *_hm_module_from_id(dt_develop_t *dev, const char *id)
{
  /* Resolve a node id ("op|multi_name") to a module instance in `dev`. */
  char op[sizeof(((dt_dev_history_item_t *)0)->op_name)];
  char name[sizeof(((dt_dev_history_item_t *)0)->multi_name)];
  _hm_id_to_op_name(id, op, name);

  dt_iop_module_t *mod = dt_iop_get_module_by_instance_name(dev->iop, op, name);
  if(IS_NULL_PTR(mod) && name[0] == '\0') mod = dt_iop_get_module_by_op_priority(dev->iop, op, 0);
  if(IS_NULL_PTR(mod) && name[0] == '\0') mod = dt_iop_get_module_by_op_priority(dev->iop, op, -1);
  return mod;
}

static gboolean _hm_module_visible_in_report(const dt_iop_module_t *mod, const GHashTable *mod_list_ids)
{
  /* Check whether a module appears in the report list and can be reordered. */
  if(IS_NULL_PTR(mod)) return FALSE;
  if(mod->flags() & IOP_FLAGS_NO_HISTORY_STACK) return FALSE;
  if(mod->enabled) return TRUE;
  if(IS_NULL_PTR(mod_list_ids)) return FALSE;
  gchar *id = _hm_make_node_id(mod->op, mod->multi_name);
  const gboolean keep = g_hash_table_contains((GHashTable *)mod_list_ids, id);
  dt_free(id);
  return keep;
}

static gchar *_hm_report_dest_label(const dt_iop_module_t *mod, GHashTable *dst_last_by_id, const GHashTable *orig_ids)
{
  /* Build destination column label with mask/inserted markers and numeric alignment. */
  gchar *dst_txt = _hm_module_row_label(mod);

  gchar *id = _hm_make_node_id(mod->op, mod->multi_name);
  const dt_dev_history_item_t *hist_dst
      = dst_last_by_id ? (const dt_dev_history_item_t *)g_hash_table_lookup(dst_last_by_id, id) : NULL;
  if(_hm_history_item_uses_masks(hist_dst))
  {
    gchar *tmp = g_strdup_printf("%s*", dst_txt);
    dt_free(dst_txt);
    dst_txt = tmp;
  }

  const gboolean inserted = orig_ids && !g_hash_table_contains((GHashTable *)orig_ids, id);
  if(inserted)
  {
    gchar *tmp = g_strdup_printf("[%s ]", dst_txt);
    dt_free(dst_txt);
    dst_txt = tmp;
  }
  else if(dst_txt[0] != '\0')
  {
    gchar *tmp = g_strdup_printf(" %s", dst_txt);
    dt_free(dst_txt);
    dst_txt = tmp;
  }

  dt_free(id);
  return dst_txt;
}

static GPtrArray *_hm_collect_enabled_modules_gui_order(const dt_develop_t *dev, const GHashTable *mod_list_ids)
{
  /* Collect report modules in GUI order (reverse pipeline order). */
  GPtrArray *mods = g_ptr_array_new();
  for(GList *modules = g_list_last(dev->iop); modules; modules = g_list_previous(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(IS_NULL_PTR(mod)) continue;
    if(!_hm_module_visible_in_report(mod, mod_list_ids)) continue;
    g_ptr_array_add(mods, mod);
  }
  return mods;
}

static void _hm_report_resync_history_iop_order(dt_develop_t *dev)
{
  /* Update history item ordering to match current module iop_order values. */
  for(GList *l = g_list_first(dev->history); l; l = g_list_next(l))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)l->data;
    if(!hist || !hist->module) continue;
    hist->iop_order = hist->module->iop_order;
  }
}

static GPtrArray *_hm_report_collect_dest_ids(GtkTreeModel *model)
{
  /* Collect destination module ids from the report rows, in GUI order (top to bottom). */
  GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
  while(valid)
  {
    gboolean is_input = FALSE;
    gchar *id = NULL;
    gtk_tree_model_get(model, &iter, HM_REPORT_COL_DST_ID, &id, HM_REPORT_COL_IS_INPUT, &is_input, -1);

    if(!is_input && id && id[0] != '\0')
      g_ptr_array_add(ids, id);
    else
      dt_free(id);

    valid = gtk_tree_model_iter_next(model, &iter);
  }

  return ids;
}

static GPtrArray *_hm_report_build_desired_visible_order(dt_develop_t *dev_dest, GtkTreeModel *model)
{
  /* Convert GUI-order rows to pipeline-order module pointers for the destination. */
  GPtrArray *gui_ids = _hm_report_collect_dest_ids(model);
  GPtrArray *mods = g_ptr_array_new();
  int missing = 0;

  for(gint i = (gint)gui_ids->len - 1; i >= 0; i--)
  {
    const char *id = (const char *)g_ptr_array_index(gui_ids, i);
    dt_iop_module_t *mod = _hm_module_from_id(dev_dest, id);
    if(mod)
      g_ptr_array_add(mods, mod);
    else
      missing++;
  }

  g_ptr_array_free(gui_ids, TRUE);

  if(missing > 0)
  {
    dt_print(DT_DEBUG_HISTORY, "[dt_history_merge] report reorder: %d destination modules not found\n", missing);
    g_ptr_array_free(mods, TRUE);
    return NULL;
  }

  return mods;
}

static GList *_hm_report_build_ordered_modules(dt_develop_t *dev_dest, const GPtrArray *visible_order,
                                               const GHashTable *mod_list_ids)
{
  /* Build a full ordered module list by reordering only visible modules. */
  if(IS_NULL_PTR(dev_dest) || !visible_order) return NULL;

  int visible_count = 0;
  for(const GList *l = g_list_first(dev_dest->iop); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    if(mod && _hm_module_visible_in_report(mod, mod_list_ids)) visible_count++;
  }

  if(visible_count != (int)visible_order->len)
  {
    dt_print(DT_DEBUG_HISTORY,
             "[dt_history_merge] report reorder: visible modules mismatch (pipe=%d, gui=%d)\n",
             visible_count, visible_order->len);
  }

  GList *ordered = NULL;
  int visible_idx = 0;
  const int visible_len = (int)visible_order->len;

  for(const GList *l = g_list_first(dev_dest->iop); l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    if(mod && _hm_module_visible_in_report(mod, mod_list_ids) && visible_idx < visible_len)
      mod = (dt_iop_module_t *)g_ptr_array_index((GPtrArray *)visible_order, visible_idx++);

    if(mod) ordered = g_list_append(ordered, mod);
  }

  while(visible_idx < visible_len)
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)g_ptr_array_index((GPtrArray *)visible_order, visible_idx++);
    if(mod) ordered = g_list_append(ordered, mod);
  }

  return ordered;
}

static gboolean _hm_report_apply_visible_order(dt_develop_t *dev_dest, const GPtrArray *visible_order,
                                               const GHashTable *mod_list_ids)
{
  /* Rebuild iop_order_list by reordering only visible modules, keeping others fixed. */
  GList *ordered = _hm_report_build_ordered_modules(dev_dest, visible_order, mod_list_ids);
  if(IS_NULL_PTR(ordered)) return FALSE;

  dt_ioppr_rebuild_iop_order_from_modules(dev_dest, ordered);
  g_list_free(ordered);
  ordered = NULL;
  return TRUE;
}

static GHashTable *_hm_report_build_moved_set(dt_develop_t *dev_src, GtkTreeModel *model,
                                              const GHashTable *mod_list_ids)
{
  /* Build a set of module ids that changed relative order between source and destination. */
  GHashTable *moved = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  if(IS_NULL_PTR(dev_src)) return moved;

  GPtrArray *dest_ids = _hm_report_collect_dest_ids(model);
  if(!dest_ids || dest_ids->len == 0)
  {
    if(dest_ids) g_ptr_array_free(dest_ids, TRUE);
    return moved;
  }

  GHashTable *dest_id_set = g_hash_table_new(g_str_hash, g_str_equal);
  for(guint i = 0; i < dest_ids->len; i++)
    g_hash_table_add(dest_id_set, g_ptr_array_index(dest_ids, i));

  GPtrArray *src_common = g_ptr_array_new_with_free_func(g_free);
  for(const GList *l = g_list_first(dev_src->iop); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    if(IS_NULL_PTR(mod) || !_hm_module_visible_in_report(mod, mod_list_ids)) continue;
    gchar *id = _hm_make_node_id(mod->op, mod->multi_name);
    if(g_hash_table_contains(dest_id_set, id))
      g_ptr_array_add(src_common, id);
    else
      dt_free(id);
  }

  GHashTable *src_id_set = g_hash_table_new(g_str_hash, g_str_equal);
  for(guint i = 0; i < src_common->len; i++)
    g_hash_table_add(src_id_set, g_ptr_array_index(src_common, i));

  GPtrArray *dst_common = g_ptr_array_new();
  for(gint i = (gint)dest_ids->len - 1; i >= 0; i--)
  {
    char *id = (char *)g_ptr_array_index(dest_ids, i);
    if(g_hash_table_contains(src_id_set, id))
      g_ptr_array_add(dst_common, id);
  }

  const gboolean same_len = (src_common->len == dst_common->len);
  gboolean same_order = same_len;
  if(same_order)
  {
    for(guint i = 0; i < src_common->len; i++)
    {
      const char *a = (const char *)g_ptr_array_index(src_common, i);
      const char *b = (const char *)g_ptr_array_index(dst_common, i);
      if(strcmp(a, b))
      {
        same_order = FALSE;
        break;
      }
    }
  }

  if(!same_order)
  {
    GHashTable *src_pos = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *dst_pos = g_hash_table_new(g_str_hash, g_str_equal);
    for(guint i = 0; i < src_common->len; i++)
      g_hash_table_insert(src_pos, g_ptr_array_index(src_common, i), GINT_TO_POINTER((int)i));
    for(guint i = 0; i < dst_common->len; i++)
      g_hash_table_insert(dst_pos, g_ptr_array_index(dst_common, i), GINT_TO_POINTER((int)i));

    for(guint i = 0; i < src_common->len; i++)
    {
      const char *id = (const char *)g_ptr_array_index(src_common, i);
      const gpointer sp = g_hash_table_lookup(src_pos, id);
      const gpointer dp = g_hash_table_lookup(dst_pos, id);
      if(sp && dp && GPOINTER_TO_INT(sp) != GPOINTER_TO_INT(dp))
        g_hash_table_replace(moved, g_strdup(id), GINT_TO_POINTER(1));
    }

    g_hash_table_destroy(src_pos);
    g_hash_table_destroy(dst_pos);
  }

  g_hash_table_destroy(src_id_set);
  g_hash_table_destroy(dest_id_set);
  g_ptr_array_free(src_common, TRUE);
  g_ptr_array_free(dst_common, TRUE);
  g_ptr_array_free(dest_ids, TRUE);

  return moved;
}

static void _hm_report_update_move_styles(GtkListStore *store, dt_develop_t *dev_src,
                                          const GHashTable *mod_list_ids)
{
  /* Update bold styles for modules moved between source and destination order. */
  GHashTable *moved = _hm_report_build_moved_set(dev_src, GTK_TREE_MODEL(store), mod_list_ids);

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
  while(valid)
  {
    gboolean is_input = FALSE;
    gchar *src_id = NULL;
    gchar *dst_id = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, HM_REPORT_COL_SRC_ID, &src_id, HM_REPORT_COL_DST_ID, &dst_id,
                       HM_REPORT_COL_IS_INPUT, &is_input, -1);

    const gboolean src_moved = (!is_input && src_id && g_hash_table_contains(moved, src_id));
    const gboolean dst_moved = (!is_input && dst_id && g_hash_table_contains(moved, dst_id));

    gtk_list_store_set(store, &iter, HM_REPORT_COL_SRC_WEIGHT,
                       src_moved ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, HM_REPORT_COL_DST_WEIGHT,
                       dst_moved ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL, -1);

    dt_free(src_id);
    dt_free(dst_id);
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
  }

  g_hash_table_destroy(moved);
}

static void _hm_report_update_arrows(GtkListStore *store, GHashTable *override, GHashTable *dst_last_by_id,
                                     GHashTable *dst_last_before_by_id)
{
  /* Refresh override arrows after destination order changes. */
  GHashTable *dst_row = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
  int row = 0;
  while(valid)
  {
    gboolean is_input = FALSE;
    gchar *src_id = NULL;
    gchar *dst_id = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, HM_REPORT_COL_SRC_ID, &src_id, HM_REPORT_COL_DST_ID, &dst_id,
                       HM_REPORT_COL_IS_INPUT, &is_input, -1);

    if(!is_input)
    {
      dt_free(src_id);

      if(dst_id && dst_id[0] != '\0')
        g_hash_table_replace(dst_row, dst_id, GINT_TO_POINTER(row));
      else
        dt_free(dst_id);
    }
    else
    {
      dt_free(src_id);
      dt_free(dst_id);
    }

    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
    row++;
  }

  valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
  row = 0;
  while(valid)
  {
    gboolean is_input = FALSE;
    gchar *src_id = NULL;
    gchar *dst_id = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, HM_REPORT_COL_SRC_ID, &src_id, HM_REPORT_COL_DST_ID, &dst_id,
                       HM_REPORT_COL_IS_INPUT, &is_input, -1);

    const char *arrow = "";
    if(!is_input && src_id && g_hash_table_contains(override, src_id))
    {
      const dt_dev_history_item_t *hist_after
          = dst_last_by_id ? (const dt_dev_history_item_t *)g_hash_table_lookup(dst_last_by_id, src_id) : NULL;
      const dt_dev_history_item_t *hist_before
          = dst_last_before_by_id ? (const dt_dev_history_item_t *)g_hash_table_lookup(dst_last_before_by_id, src_id)
                                  : NULL;

      gboolean mask_override = FALSE;
      if(hist_before)
        mask_override = !_hm_history_masks_match(hist_after, hist_before);
      else
        mask_override = _hm_history_item_uses_masks(hist_after);

      const gpointer dst_row_ptr = g_hash_table_lookup(dst_row, src_id);
      if(dst_row_ptr)
      {
        const int dst_r = GPOINTER_TO_INT(dst_row_ptr);
        const int delta = dst_r - row;
        if(delta == 0)
          arrow = mask_override ? "→*" : "→";
        else if(delta == 1)
          arrow = mask_override ? "↘*" : "↘";
        else if(delta == -1)
          arrow = mask_override ? "↗*" : "↗";
        else if(delta > 1)
          arrow = mask_override ? "↴*" : "↴";
        else
          arrow = mask_override ? "↴*" : "↴";
      }
    }

    gtk_list_store_set(store, &iter, HM_REPORT_COL_ARROW, arrow, -1);

    dt_free(src_id);
    dt_free(dst_id);
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
    row++;
  }

  g_hash_table_destroy(dst_row);
}

static void _hm_report_keep_input_row_at_bottom(GtkListStore *store)
{
  /* Ensure the "Input image" row stays anchored at the bottom after DnD. */
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
  GtkTreeIter input_iter;
  int input_index = -1;
  int last_index = -1;
  int idx = 0;

  while(valid)
  {
    gboolean is_input = FALSE;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, HM_REPORT_COL_IS_INPUT, &is_input, -1);
    if(is_input)
    {
      input_iter = iter;
      input_index = idx;
    }
    last_index = idx;
    idx++;
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
  }

  if(input_index >= 0 && input_index != last_index)
    gtk_list_store_move_after(store, &input_iter, NULL);
}

static void _hm_report_update_dest_labels(GtkListStore *store, dt_develop_t *dev_dest, GHashTable *dst_last_by_id,
                                          const GHashTable *orig_ids)
{
  /* Refresh destination column labels after iop_order changes. */
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter);
  while(valid)
  {
    gboolean is_input = FALSE;
    gchar *id = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, HM_REPORT_COL_DST_ID, &id, HM_REPORT_COL_IS_INPUT, &is_input, -1);

    if(!is_input && id && id[0] != '\0')
    {
      dt_iop_module_t *mod = _hm_module_from_id(dev_dest, id);
      gchar *dst_txt = mod ? _hm_report_dest_label(mod, dst_last_by_id, orig_ids) : g_strdup("");
      gtk_list_store_set(store, &iter, HM_REPORT_COL_DST, dst_txt, -1);
      dt_free(dst_txt);
    }

    dt_free(id);
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &iter);
  }
}

static void _hm_report_apply_store_order(_hm_report_reorder_ctx_t *ctx)
{
  /* Apply destination column order to the pipeline and refresh labels/styles. */
  if(ctx->in_reorder) return;
  ctx->in_reorder = TRUE;

  _hm_report_keep_input_row_at_bottom(ctx->store);

  GPtrArray *desired = _hm_report_build_desired_visible_order(ctx->dev_dest, GTK_TREE_MODEL(ctx->store));
  if(desired && _hm_report_apply_visible_order(ctx->dev_dest, desired, ctx->mod_list_ids))
  {
    _hm_report_resync_history_iop_order(ctx->dev_dest);
    _hm_report_update_dest_labels(ctx->store, ctx->dev_dest, ctx->dst_last_by_id, ctx->orig_ids);
    _hm_report_update_arrows(ctx->store, ctx->override, ctx->dst_last_by_id, ctx->dst_last_before_by_id);
    _hm_report_update_move_styles(ctx->store, ctx->dev_src, ctx->mod_list_ids);
  }
  if(desired) g_ptr_array_free(desired, TRUE);

  ctx->in_reorder = FALSE;
}

static void _hm_report_drag_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  _hm_report_reorder_ctx_t *ctx = (_hm_report_reorder_ctx_t *)user_data;
  if(ctx->drag_path)
  {
    gtk_tree_path_free(ctx->drag_path);
    ctx->drag_path = NULL;
  }

  GtkTreePath *path = NULL;
  GtkTreeViewColumn *column = NULL;
  gtk_tree_view_get_cursor(GTK_TREE_VIEW(widget), &path, &column);
  if(path) ctx->drag_path = path;
}

static void _hm_report_drag_data_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data,
                                     guint info, guint time, gpointer user_data)
{
  _hm_report_reorder_ctx_t *ctx = (_hm_report_reorder_ctx_t *)user_data;
  GtkTreePath *path = ctx->drag_path;

  if(IS_NULL_PTR(path))
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(widget), &path, NULL);

  if(IS_NULL_PTR(path)) return;

  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store), &iter, path))
  {
    if(path != ctx->drag_path) gtk_tree_path_free(path);
    return;
  }

  gboolean is_input = FALSE;
  gchar *dst_id = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &iter, HM_REPORT_COL_DST_ID, &dst_id, HM_REPORT_COL_IS_INPUT,
                     &is_input, -1);
  if(is_input || !dst_id || dst_id[0] == '\0')
  {
    dt_free(dst_id);
    if(path != ctx->drag_path) gtk_tree_path_free(path);
    return;
  }
  dt_free(dst_id);

  gchar *path_str = gtk_tree_path_to_string(path);
  gtk_selection_data_set(selection_data, gdk_atom_intern_static_string("DT_HISTORY_MERGE_DST_ROW"), 8,
                         (const guchar *)path_str, strlen(path_str));
  dt_free(path_str);

  if(path != ctx->drag_path) gtk_tree_path_free(path);
}

static void _hm_report_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                          GtkSelectionData *selection_data, guint info, guint time,
                                          gpointer user_data)
{
  _hm_report_reorder_ctx_t *ctx = (_hm_report_reorder_ctx_t *)user_data;

  if(IS_NULL_PTR(selection_data)) return;
  const guchar *data = gtk_selection_data_get_data(selection_data);
  if(IS_NULL_PTR(data)) return;
  gchar *src_path_str = g_strdup((const gchar *)data);
  if(IS_NULL_PTR(src_path_str)) return;

  GtkTreePath *src_path = gtk_tree_path_new_from_string(src_path_str);
  dt_free(src_path_str);
  if(IS_NULL_PTR(src_path)) return;

  GtkTreePath *dst_path = NULL;
  GtkTreeViewDropPosition pos;
  if(!gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(widget), x, y, &dst_path, &pos))
  {
    gtk_tree_path_free(src_path);
    return;
  }

  if(gtk_tree_path_compare(src_path, dst_path) == 0)
  {
    gtk_tree_path_free(src_path);
    gtk_tree_path_free(dst_path);
    return;
  }

  GtkTreeIter src_iter;
  GtkTreeIter dst_iter;
  if(!gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store), &src_iter, src_path)
     || !gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store), &dst_iter, dst_path))
  {
    gtk_tree_path_free(src_path);
    gtk_tree_path_free(dst_path);
    return;
  }

  gboolean src_input = FALSE;
  gboolean dst_input = FALSE;
  gchar *src_dst_id = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &src_iter, HM_REPORT_COL_IS_INPUT, &src_input, HM_REPORT_COL_DST_ID,
                     &src_dst_id, -1);
  gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &dst_iter, HM_REPORT_COL_IS_INPUT, &dst_input, -1);

  if(src_input || !src_dst_id || src_dst_id[0] == '\0')
  {
    dt_free(src_dst_id);
    gtk_tree_path_free(src_path);
    gtk_tree_path_free(dst_path);
    return;
  }

  GPtrArray *dest_rows = g_ptr_array_new();
  GPtrArray *dest_ids = g_ptr_array_new();
  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(ctx->store), &iter);
  int row_index = 0;
  while(valid)
  {
    gboolean is_input = FALSE;
    gchar *id = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(ctx->store), &iter, HM_REPORT_COL_DST_ID, &id, HM_REPORT_COL_IS_INPUT,
                       &is_input, -1);
    if(!is_input && id && id[0] != '\0')
    {
      g_ptr_array_add(dest_rows, GINT_TO_POINTER(row_index));
      g_ptr_array_add(dest_ids, id);
    }
    else
    {
      dt_free(id);
    }
    valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(ctx->store), &iter);
    row_index++;
  }

  int src_row_index = gtk_tree_path_get_indices(src_path)[0];
  int dst_row_index = gtk_tree_path_get_indices(dst_path)[0];

  int src_pos = -1;
  for(guint i = 0; i < dest_rows->len; i++)
  {
    if(GPOINTER_TO_INT(g_ptr_array_index(dest_rows, i)) == src_row_index)
    {
      src_pos = (int)i;
      break;
    }
  }

  if(src_pos < 0)
  {
    dt_free(src_dst_id);
    for(guint i = 0; i < dest_ids->len; i++) dt_free(g_ptr_array_index(dest_ids, i));
    g_ptr_array_free(dest_ids, TRUE);
    g_ptr_array_free(dest_rows, TRUE);
    gtk_tree_path_free(src_path);
    gtk_tree_path_free(dst_path);
    return;
  }

  int target_pos = 0;
  if(dst_input)
  {
    target_pos = (int)dest_ids->len;
  }
  else
  {
    for(guint i = 0; i < dest_rows->len; i++)
    {
      const int row = GPOINTER_TO_INT(g_ptr_array_index(dest_rows, i));
      if(row < dst_row_index) target_pos++;
    }
    if(pos == GTK_TREE_VIEW_DROP_AFTER || pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER)
      target_pos++;
  }

  if(target_pos > (int)dest_ids->len) target_pos = (int)dest_ids->len;

  if(target_pos == src_pos || target_pos == src_pos + 1)
  {
    dt_free(src_dst_id);
    for(guint i = 0; i < dest_ids->len; i++) dt_free(g_ptr_array_index(dest_ids, i));
    g_ptr_array_free(dest_ids, TRUE);
    g_ptr_array_free(dest_rows, TRUE);
    gtk_tree_path_free(src_path);
    gtk_tree_path_free(dst_path);
    return;
  }

  gchar *moved_id = (gchar *)g_ptr_array_index(dest_ids, src_pos);
  g_ptr_array_remove_index(dest_ids, src_pos);
  if(target_pos > src_pos) target_pos--;
  g_ptr_array_insert(dest_ids, target_pos, moved_id);

  for(guint i = 0; i < dest_rows->len && i < dest_ids->len; i++)
  {
    const int row = GPOINTER_TO_INT(g_ptr_array_index(dest_rows, i));
    GtkTreeIter row_iter;
    if(gtk_tree_model_iter_nth_child(GTK_TREE_MODEL(ctx->store), &row_iter, NULL, row))
      gtk_list_store_set(ctx->store, &row_iter, HM_REPORT_COL_DST_ID,
                         (const char *)g_ptr_array_index(dest_ids, i), -1);
  }

  _hm_report_apply_store_order(ctx);

  for(guint i = 0; i < dest_ids->len; i++) dt_free(g_ptr_array_index(dest_ids, i));
  g_ptr_array_free(dest_ids, TRUE);
  g_ptr_array_free(dest_rows, TRUE);
  dt_free(src_dst_id);

  gtk_tree_path_free(src_path);
  gtk_tree_path_free(dst_path);
}

static GHashTable *_hm_build_override_map(const dt_develop_t *dev_dest, GHashTable *src_last_by_id,
                                          GHashTable *dst_last_before_by_id)
{
  /* Build a set of module ids whose final history item matches the source but not the destination.
   *
   * We only report overrides when source and destination history items differ.
   */
  GHashTable *override = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  const int history_end = dt_dev_get_history_end_ext((dt_develop_t *)dev_dest);

  for(GList *modules = g_list_first(dev_dest->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    dt_dev_history_item_t *hist_after
        = dt_dev_history_get_last_item_by_module(dev_dest->history, mod, history_end);

    gchar *id = _hm_make_node_id(mod->op, mod->multi_name);
    const dt_dev_history_item_t *hist_src
        = src_last_by_id ? (const dt_dev_history_item_t *)g_hash_table_lookup(src_last_by_id, id) : NULL;
    const dt_dev_history_item_t *hist_dst
        = dst_last_before_by_id ? (const dt_dev_history_item_t *)g_hash_table_lookup(dst_last_before_by_id, id) : NULL;

    const gboolean match_src = hist_src && _hm_history_items_match(hist_after, hist_src);
    const gboolean match_dst = hist_dst && _hm_history_items_match(hist_after, hist_dst);
    if(match_src && !match_dst)
      g_hash_table_replace(override, id, GINT_TO_POINTER(1));
    else
      dt_free(id);
  }

  return override;
}

gboolean _hm_show_merge_report_popup(dt_develop_t *dev_dest, dt_develop_t *dev_src,
                                     const gboolean merge_iop_order, const gboolean used_source_order,
                                     const dt_history_merge_strategy_t strategy, GHashTable *src_last_by_id,
                                     GHashTable *dst_last_before_by_id, const GPtrArray *orig_labels,
                                     const GPtrArray *orig_styles, const GHashTable *orig_ids,
                                     const GHashTable *mod_list_ids)
{
  /* Present a merge report with source/destination pipelines and override markers. */
  if(IS_NULL_PTR(darktable.gui)) return FALSE;
  if(!g_main_context_is_owner(g_main_context_default())) return FALSE;

  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);
  if(IS_NULL_PTR(window)) return FALSE;

  const char *merge_mode = merge_iop_order ? _("merge") : _("destination");
  const char *strategy_name
      = (strategy == DT_HISTORY_MERGE_APPEND) ? _("append")
        : (strategy == DT_HISTORY_MERGE_APPSTART) ? _("appstart")
        : _("replace");

  gchar *title_text
      = g_strdup_printf(_("Copy, merging pipeline in %s and history in %s mode"), merge_mode, strategy_name);

  GtkDialog *dialog = GTK_DIALOG(gtk_dialog_new_with_buttons(
      _("History merge report"), GTK_WINDOW(window),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, _("_Revert"), GTK_RESPONSE_ACCEPT, _("_Accept"),
      GTK_RESPONSE_CLOSE, NULL));

  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *label = gtk_label_new(title_text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 100);
  gtk_box_pack_start(GTK_BOX(content_area), label, FALSE, FALSE, 6);

  const char *order_text = used_source_order ? _("Source pipeline order was used")
                                             : _("Destination pipeline order was used");
  const char *fallback_text = (used_source_order != merge_iop_order)
                                  ? _(" as a fallback because we could not resolve positionning constraints with source order.")
                                  : ".";
  gchar *order_label_text = g_strdup_printf("%s%s", order_text, fallback_text);
  GtkWidget *order_label = gtk_label_new(order_label_text);
  gtk_label_set_xalign(GTK_LABEL(order_label), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(order_label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(order_label), 100);
  gtk_box_pack_start(GTK_BOX(content_area), order_label, FALSE, FALSE, 6);
  dt_free(order_label_text);

  GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scrolled, 740, 420);
  gtk_box_pack_start(GTK_BOX(content_area), scrolled, TRUE, TRUE, 0);

  GtkListStore *store = gtk_list_store_new(HM_REPORT_COL_COUNT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                           G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT,
                                           G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_BOOLEAN);
  GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  g_object_unref(store);
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);

  gchar *src_base = dev_src ? g_path_get_basename(dev_src->image_storage.filename) : g_strdup("");
  gchar *dst_base = g_path_get_basename(dev_dest->image_storage.filename);

  gchar *orig_title = g_strdup_printf(_("Original: %d %s"), dev_dest->image_storage.id, dst_base);
  gchar *src_title = dev_src ? g_strdup_printf(_("Source: %d %s"), dev_src->image_storage.id, src_base)
                             : g_strdup(_("Source"));
  gchar *dst_title = g_strdup_printf(_("Destination: %d %s"), dev_dest->image_storage.id, dst_base);

  GtkCellRenderer *r_orig = gtk_cell_renderer_text_new();
  g_object_set(r_orig, "fixed-height-from-font", 1, "ypad", 0, NULL);
  GtkTreeViewColumn *c_orig = gtk_tree_view_column_new_with_attributes(orig_title, r_orig, "text",
                                                                       HM_REPORT_COL_ORIG, "style",
                                                                       HM_REPORT_COL_ORIG_STYLE, NULL);
  gtk_tree_view_column_set_expand(c_orig, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_orig);

  GtkCellRenderer *r_filet = gtk_cell_renderer_text_new();
  g_object_set(r_filet, "xalign", 0.5f, "fixed-height-from-font", 1, "ypad", 0, NULL);
  GtkTreeViewColumn *c_filet = gtk_tree_view_column_new_with_attributes("", r_filet, "text",
                                                                        HM_REPORT_COL_FILET, NULL);
  gtk_tree_view_column_set_alignment(c_filet, 0.5f);
  gtk_tree_view_column_set_sizing(c_filet, GTK_TREE_VIEW_COLUMN_FIXED);
  gtk_tree_view_column_set_fixed_width(c_filet, 16);
  gtk_tree_view_column_set_expand(c_filet, FALSE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_filet);

  GtkCellRenderer *r_src = gtk_cell_renderer_text_new();
  g_object_set(r_src, "fixed-height-from-font", 1, "ypad", 0, NULL);
  GtkTreeViewColumn *c_src = gtk_tree_view_column_new_with_attributes(src_title, r_src, "text",
                                                                      HM_REPORT_COL_SRC, "weight",
                                                                      HM_REPORT_COL_SRC_WEIGHT, "style",
                                                                      HM_REPORT_COL_SRC_STYLE, NULL);
  gtk_tree_view_column_set_expand(c_src, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_src);

  GtkCellRenderer *r_arrow = gtk_cell_renderer_text_new();
  g_object_set(r_arrow, "xalign", 0.5f, "fixed-height-from-font", 1, "ypad", 0, NULL);
  GtkTreeViewColumn *c_arrow = gtk_tree_view_column_new_with_attributes(_("Override"), r_arrow, "markup",
                                                                        HM_REPORT_COL_ARROW, NULL);
  gtk_tree_view_column_set_alignment(c_arrow, 0.5f);
  gtk_tree_view_column_set_expand(c_arrow, FALSE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_arrow);

  GtkCellRenderer *r_dst = gtk_cell_renderer_text_new();
  g_object_set(r_dst, "fixed-height-from-font", 1, "ypad", 0, NULL);
  GtkTreeViewColumn *c_dst = gtk_tree_view_column_new_with_attributes(dst_title, r_dst, "text",
                                                                      HM_REPORT_COL_DST, "weight",
                                                                      HM_REPORT_COL_DST_WEIGHT, "style",
                                                                      HM_REPORT_COL_DST_STYLE, NULL);
  gtk_tree_view_column_set_expand(c_dst, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), c_dst);

  gtk_container_add(GTK_CONTAINER(scrolled), tree);

  GtkWidget *legend = gtk_label_new(_("[name] inserted module, * uses masks, bold = moved module, italic = disabled module (shown only if copied).\n"
    "Arrows indicate parameters overriden (→ same row, ↗/↘ adjacent, ↴/↴ farther), * on arrow means masks overridden.\n"
                                      "Drag and drop modules in the `Destination` column to reorder the pipeline."));
  gtk_label_set_xalign(GTK_LABEL(legend), 0.0f);
  gtk_label_set_line_wrap(GTK_LABEL(legend), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(legend), 100);
  gtk_box_pack_start(GTK_BOX(content_area), legend, FALSE, FALSE, 6);

  const int orig_len = orig_labels ? orig_labels->len : 0;
  GPtrArray *src_mods = dev_src ? _hm_collect_enabled_modules_gui_order(dev_src, mod_list_ids) : g_ptr_array_new();
  GPtrArray *dst_mods = _hm_collect_enabled_modules_gui_order(dev_dest, mod_list_ids);
  GHashTable *dst_last_by_id = NULL;
  if(_hm_build_last_history_by_id(dev_dest, &dst_last_by_id)) return FALSE;

  const int src_len = src_mods->len;
  const int dst_len = dst_mods->len;
  const int rows = MAX(orig_len, MAX(src_len, dst_len));
  const int orig_offset = rows - orig_len;
  const int src_offset = rows - src_len;
  const int dst_offset = rows - dst_len;

  GHashTable *override = _hm_build_override_map(dev_dest, src_last_by_id, dst_last_before_by_id);
  _hm_report_reorder_ctx_t *reorder_ctx = g_new0(_hm_report_reorder_ctx_t, 1);
  reorder_ctx->dev_dest = dev_dest;
  reorder_ctx->dev_src = dev_src;
  reorder_ctx->store = store;
  reorder_ctx->dst_last_by_id = dst_last_by_id;
  reorder_ctx->dst_last_before_by_id = dst_last_before_by_id;
  reorder_ctx->override = override;
  reorder_ctx->orig_ids = orig_ids;
  reorder_ctx->mod_list_ids = mod_list_ids;

  for(int r = 0; r < rows; r++)
  {
    const int orig_idx = r - orig_offset;
    const int src_idx = r - src_offset;
    const int dst_idx = r - dst_offset;

    const char *orig_txt = (orig_idx >= 0 && orig_labels)
                               ? (const char *)g_ptr_array_index((GPtrArray *)orig_labels, orig_idx)
                               : "";
    const int orig_style = (orig_idx >= 0 && orig_styles)
                               ? GPOINTER_TO_INT(g_ptr_array_index((GPtrArray *)orig_styles, orig_idx))
                               : PANGO_STYLE_NORMAL;
    const dt_iop_module_t *src_mod = (src_idx >= 0) ? (const dt_iop_module_t *)g_ptr_array_index(src_mods, src_idx) : NULL;
    const dt_iop_module_t *dst_mod = (dst_idx >= 0) ? (const dt_iop_module_t *)g_ptr_array_index(dst_mods, dst_idx) : NULL;

    gchar *src_txt = src_mod ? _hm_module_row_label(src_mod) : g_strdup("");
    gchar *dst_txt = dst_mod ? _hm_report_dest_label(dst_mod, dst_last_by_id, orig_ids) : g_strdup("");
    gchar *src_id = src_mod ? _hm_make_node_id(src_mod->op, src_mod->multi_name) : NULL;
    const int src_style = (src_mod && !src_mod->enabled) ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL;
    const int dst_style = (dst_mod && !dst_mod->enabled) ? PANGO_STYLE_ITALIC : PANGO_STYLE_NORMAL;

    if(src_mod && src_last_by_id)
    {
      const dt_dev_history_item_t *hist_src = (const dt_dev_history_item_t *)g_hash_table_lookup(src_last_by_id, src_id);
      if(_hm_history_item_uses_masks(hist_src))
      {
        gchar *tmp = g_strdup_printf("%s*", src_txt);
        dt_free(src_txt);
        src_txt = tmp;
      }
    }

    const char *arrow = "";

    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gchar *dst_id = dst_mod ? _hm_make_node_id(dst_mod->op, dst_mod->multi_name) : NULL;
    gtk_list_store_set(store, &iter, HM_REPORT_COL_ORIG, orig_txt, HM_REPORT_COL_FILET, "│", HM_REPORT_COL_SRC,
                       src_txt, HM_REPORT_COL_ARROW, arrow, HM_REPORT_COL_DST, dst_txt, HM_REPORT_COL_SRC_ID,
                       src_id, HM_REPORT_COL_DST_ID, dst_id, HM_REPORT_COL_SRC_WEIGHT, PANGO_WEIGHT_NORMAL,
                       HM_REPORT_COL_DST_WEIGHT, PANGO_WEIGHT_NORMAL, HM_REPORT_COL_ORIG_STYLE, orig_style,
                       HM_REPORT_COL_SRC_STYLE, src_style, HM_REPORT_COL_DST_STYLE, dst_style,
                       HM_REPORT_COL_IS_INPUT, FALSE, -1);
    dt_free(dst_id);
    dt_free(src_id);

    dt_free(src_txt);
    dt_free(dst_txt);
  }

  {
    gchar *input_label = g_strdup_printf("%4s  %s", "0", _("Input image"));
    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter, HM_REPORT_COL_ORIG, input_label, HM_REPORT_COL_FILET, "│", HM_REPORT_COL_SRC,
                       input_label, HM_REPORT_COL_ARROW, "", HM_REPORT_COL_DST, input_label, HM_REPORT_COL_SRC_ID,
                       NULL, HM_REPORT_COL_DST_ID, NULL, HM_REPORT_COL_SRC_WEIGHT, PANGO_WEIGHT_NORMAL,
                       HM_REPORT_COL_DST_WEIGHT, PANGO_WEIGHT_NORMAL, HM_REPORT_COL_ORIG_STYLE, PANGO_STYLE_NORMAL,
                       HM_REPORT_COL_SRC_STYLE, PANGO_STYLE_NORMAL, HM_REPORT_COL_DST_STYLE, PANGO_STYLE_NORMAL,
                       HM_REPORT_COL_IS_INPUT, TRUE, -1);
    dt_free(input_label);
  }

  _hm_report_update_move_styles(store, dev_src, mod_list_ids);
  _hm_report_update_arrows(store, override, dst_last_by_id, dst_last_before_by_id);

  GtkTargetEntry targets[] = { { "DT_HISTORY_MERGE_DST_ROW", GTK_TARGET_SAME_WIDGET, 0 } };
  gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(tree), GDK_BUTTON1_MASK, targets, 1, GDK_ACTION_MOVE);
  gtk_tree_view_enable_model_drag_dest(GTK_TREE_VIEW(tree), targets, 1, GDK_ACTION_MOVE);

  gulong drag_begin_handler =
      g_signal_connect(G_OBJECT(tree), "drag-begin", G_CALLBACK(_hm_report_drag_begin), reorder_ctx);
  gulong drag_get_handler =
      g_signal_connect(G_OBJECT(tree), "drag-data-get", G_CALLBACK(_hm_report_drag_data_get), reorder_ctx);
  gulong drag_recv_handler =
      g_signal_connect(G_OBJECT(tree), "drag-data-received", G_CALLBACK(_hm_report_drag_data_received), reorder_ctx);

  gtk_widget_show_all(GTK_WIDGET(dialog));
  const int res = gtk_dialog_run(dialog);

  g_signal_handler_disconnect(tree, drag_begin_handler);
  g_signal_handler_disconnect(tree, drag_get_handler);
  g_signal_handler_disconnect(tree, drag_recv_handler);
  if(reorder_ctx->drag_path) gtk_tree_path_free(reorder_ctx->drag_path);
  dt_free(reorder_ctx);

  gtk_widget_destroy(GTK_WIDGET(dialog));

  g_hash_table_destroy(override);
  g_ptr_array_free(src_mods, TRUE);
  g_ptr_array_free(dst_mods, TRUE);
  if(dst_last_by_id) g_hash_table_destroy(dst_last_by_id);

  dt_free(src_base);
  dt_free(dst_base);
  dt_free(orig_title);
  dt_free(src_title);
  dt_free(dst_title);
  dt_free(title_text);

  return (res == GTK_RESPONSE_ACCEPT);
}
