/*
    This file is part of darktable,
    Copyright (C) 2010, 2015 Bruce Guenter.
    Copyright (C) 2010-2013 Henrik Andersson.
    Copyright (C) 2010-2013, 2016 johannes hanika.
    Copyright (C) 2010 Josep Puigdemont.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2010-2018 Tobias Ellinghaus.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Brian Teague.
    Copyright (C) 2011 Moritz Lipp.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2012 calca.
    Copyright (C) 2012 José Carlos García Sogo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2013 Simon Spannagel.
    Copyright (C) 2013, 2016, 2019-2022 Aldric Renaudin.
    Copyright (C) 2013 Benjamin Cahill.
    Copyright (C) 2013 Gaspard Jankowiak.
    Copyright (C) 2013-2016 Jérémy Rosen.
    Copyright (C) 2013-2015, 2018-2022 Pascal Obry.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2013-2014 Ulrich Pegelow.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2016 Erik Duisters.
    Copyright (C) 2017 Dan Torop.
    Copyright (C) 2017 parafin.
    Copyright (C) 2017 pgkos.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 Peter Budai.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2018 Rick Yorgason.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2018, 2020 Sam Smith.
    Copyright (C) 2019, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019-2022 Diederik Ter Rahe.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019-2022 Philippe Weyland.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2020 codingdave@gmail.com.
    Copyright (C) 2020 EdgarLux.
    Copyright (C) 2020 GrahamByrnes.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2020 jpverrue.
    Copyright (C) 2020 Marco.
    Copyright (C) 2020 Matt Maguire.
    Copyright (C) 2020, 2022 Nicolas Auffray.
    Copyright (C) 2020 Reinout Nonhebel.
    Copyright (C) 2020 Vincent THOMAS.
    Copyright (C) 2021 Arnaud TANGUY.
    Copyright (C) 2021 Bill Ferguson.
    Copyright (C) 2021 David-Tillmann Schaefer.
    Copyright (C) 2021 Harald.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2021 quovadit.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2026 Miguel Moquillon.

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
  Library module — browse and manage collections shown in the lighttable.

  This is *only* the GUI. The SQL engine lives in src/common/collection.c: it reads the conf
  keys plugins/lighttable/collect/{num_rules,item<N>,mode<N>,string<N>} and turns them into
  the query (get_query_string / dt_collection_update_query). So this module's whole contract
  is: write those conf keys through the helpers in "Section 2 — conf layer", then call
  _commit_colllection(). Everything else here is presentation and management.

  Three tabs (a notebook tab-bar drives one shared value view):
    - Folders      : film-rolls/folders, as a flat List or a hierarchical Tree. The place to
                     relocate and remove film-rolls (in batches). item0 = FILMROLL | FOLDERS.
    - Collections  : tags, as a hierarchical Tree. Browse, rename, delete (batches). item0 = TAG.
    - Queries      : an arbitrary multi-rule builder (property/value/AND-OR-NOT), plus a raw
                     SQL escape hatch (item0 = DT_COLLECTION_PROP_QUERY).

  Right-click on a Folders/Collections row opens a context menu built from a small ACTIONS
  table (Section 6) — adding a bulk operation (e.g. pre-render thumbnails) is one table row,
  fed by _rows_to_imgids() / dt_collection_get_images_for_rule(), which map the selected rows
  to the matching image ids so any whole-set operation can be bolted on without touching the
  view code.

  TODO:
    - when querying on numeric/datetime fields and using a range like `[2021;2022]`, limit the
      treeview content to items actually fitting within that range (same behaviour as when typing
      text for folders/filenames: the list is reduced to matching elements),
    - range selection from queries, using `[;]` syntax is weird, find a better way to handle ranges
      through regular GUI/API using `>` on the lower bound `AND` `<` on the higher bound. 
      This may have to dynamically add a new rule and spawn comboboxes based on user selections
      in treeview. Though the current text-based listing makes for a simple GUI and expressive
      queries writing, it conflicts with current GUI and the syntax is too advanced. 
      I don't know what the best course of action is here.
    - implement drag & drap from thumbtable thumbnails to:
      1. folders/filmrolls: move dragged images to the target folder (disable/forbid it if more 
         than one treeview row is selected)
      2. tags (collections): attach the target tag to the dragged images,
      3. in both cases, refresh treeview and lighttable/thumbtable view to update images that moved
         elsewhere,
    - de-implement the preferences (hidden) popup and add every view configuration parameters to the
      front widget, into the relevant tab if needed to not pollute the overall view,
    - sort by ID has no effect on folders treeview, it is only sorted alphabetically. Hide the 
      "sort by" combobox entirely in that case, for consistency.
    - add an entry in context menu (on right click in treeview/list) to pre-render all thumbnails
      from the target collection (see gui/actions/run.c menu for example). An API already exists
      to produce a GList of imgids from a collection extracted from treeview row.
*/

#include "libs/collect.h"
#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/datetime.h"
#include "common/debug.h"
#include "common/film.h"
#include "common/image.h"
#include "common/map_locations.h"
#include "common/metadata.h"
#include "common/mipmap_cache.h"
#include "common/selection.h"
#include "common/tags.h"
#include "common/utility.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/jobs.h"
#include "control/jobs/control_jobs.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "dtgtk/togglebutton.h"
#include "gui/drag_and_drop.h"
#include "gui/gtk.h"
#include "gui/preferences_dialogs.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#include "views/view.h"
#ifndef _WIN32
#include <gio/gunixmounts.h>
#endif
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(3)

#define MAX_RULES 10
#define PARAM_STRING_SIZE 256

typedef enum dt_collect_tab_t
{
  TAB_FOLDERS = 0,
  TAB_COLLECTIONS = 1,
  TAB_QUERIES = 2
} dt_collect_tab_t;

typedef enum dt_lib_collect_cols_t
{
  DT_LIB_COLLECT_COL_TEXT = 0,
  DT_LIB_COLLECT_COL_ID,
  DT_LIB_COLLECT_COL_TOOLTIP,
  DT_LIB_COLLECT_COL_PATH,
  DT_LIB_COLLECT_COL_VISIBLE,
  DT_LIB_COLLECT_COL_UNREACHABLE,
  DT_LIB_COLLECT_COL_COUNT,
  DT_LIB_COLLECT_COL_INDEX,
  DT_LIB_COLLECT_COL_FONT,
  DT_LIB_COLLECT_NUM_COLS
} dt_lib_collect_cols_t;

typedef struct dt_lib_collect_rule_t
{
  int num;
  GtkWidget *hbox;
  GtkWidget *combo;
  GtkWidget *op_combo; // comparison operator selector (numeric/date/rating properties)
  GtkWidget *text;
  GtkWidget *button;
  gboolean typing;
  gboolean reveal;   // one-shot: after a view/tab switch, unfold the tree to the preserved query
  gchar *searchstring;
  void *lib_collect; // backref to dt_lib_collect_t
} dt_lib_collect_rule_t;

typedef struct dt_lib_collect_t
{
  dt_lib_collect_rule_t rule[MAX_RULES];
  GtkWidget *notebook;

  int active_rule;
  int nb_rules;

  GtkTreeView *view;
  int view_rule;

  GtkTreeModel *treefilter;
  GtkTreeModel *listfilter;

  // Folders-tab inline controls
  GtkWidget *folders_controls; // hbox holding the widgets below
  GtkWidget *recursive_check;  // "include sub-folders" -> '*' suffix
  GtkWidget *sort_dir;         // ascending/descending toggle
  GtkWidget *sort_by;          // film-roll sort key (id / folder name)
  GtkWidget *folder_levels;    // show_folder_levels: levels shown in film-roll names (List only)

  // Collections-tab inline controls
  GtkWidget *collections_controls; // hbox holding the widget below
  GtkWidget *no_uncategorized;     // "no 'uncategorized' group" for childless tags

  // Queries-tab raw SQL escape
  GtkWidget *raw_box;
  GtkWidget *raw_check;
  GtkWidget *raw_entry;

  struct dt_lib_collect_params_t *params;
#ifdef _WIN32
  GVolumeMonitor *vmonitor;
#else
  GUnixMountMonitor *vmonitor;
#endif
} dt_lib_collect_t;

typedef struct dt_lib_collect_params_rule_t
{
  uint32_t item : 16;
  uint32_t mode : 16;
  char string[PARAM_STRING_SIZE];
} dt_lib_collect_params_rule_t;

typedef struct dt_lib_collect_params_t
{
  uint32_t rules;
  dt_lib_collect_params_rule_t rule[MAX_RULES];
} dt_lib_collect_params_t;

typedef struct _range_t
{
  gchar *start;
  gchar *stop;
  GtkTreePath *path1;
  GtkTreePath *path2;
} _range_t;

// ---- forward declarations ----
static void _lib_collect_gui_update(dt_lib_module_t *self);
static void entry_changed(GtkEntry *entry, dt_lib_collect_rule_t *dr);
static void combo_changed(GtkWidget *combo, dt_lib_collect_rule_t *dr);
static void collection_updated(gpointer instance, dt_collection_change_t query_change,
                               dt_collection_properties_t changed_property, gpointer imgs, int next,
                               gpointer self);
static void row_activated(GtkTreeView *view, GtkTreePath *path, GdkEventButton *event, dt_lib_collect_t *d);
static void update_view(dt_lib_collect_rule_t *dr);
static void _populate_collect_combo(GtkWidget *w);
static int _combo_get_active_collection(GtkWidget *combo);
static gboolean _combo_set_active_collection(GtkWidget *combo, const int property);
static void _op_changed(GtkWidget *w, dt_lib_collect_rule_t *dr);

// =====================================================================================
// Section 0 — property predicates
// =====================================================================================

static int is_time_property(int property)
{
  return property == DT_COLLECTION_PROP_TIME || property == DT_COLLECTION_PROP_IMPORT_TIMESTAMP
         || property == DT_COLLECTION_PROP_CHANGE_TIMESTAMP || property == DT_COLLECTION_PROP_EXPORT_TIMESTAMP
         || property == DT_COLLECTION_PROP_PRINT_TIMESTAMP;
}

static gboolean item_is_folder(int item)
{
  return item == DT_COLLECTION_PROP_FILMROLL || item == DT_COLLECTION_PROP_FOLDERS;
}

static gboolean item_is_tag(int item)
{
  return item == DT_COLLECTION_PROP_TAG;
}

static gboolean item_is_numeric(int item)
{
  return item == DT_COLLECTION_PROP_DAY || is_time_property(item) || item == DT_COLLECTION_PROP_APERTURE
         || item == DT_COLLECTION_PROP_FOCAL_LENGTH || item == DT_COLLECTION_PROP_ISO
         || item == DT_COLLECTION_PROP_EXPOSURE || item == DT_COLLECTION_PROP_RATING;
}

// A property displayed as a hierarchical tree (vs a flat list).
static gboolean item_is_tree(int item)
{
  return item == DT_COLLECTION_PROP_FOLDERS || item == DT_COLLECTION_PROP_TAG
         || item == DT_COLLECTION_PROP_GEOTAGGING || item == DT_COLLECTION_PROP_DAY || is_time_property(item);
}

// Comparison operators offered by the operator combo for numeric/date/rating properties.
// OP_TOKENS is the prefix written into the rule string; OP_LABELS is what the user sees.
static const char *const OP_TOKENS[] = { "", "<", "<=", ">", ">=", "<>" };
static const char *const OP_LABELS[] = { "=", "<", "≤", ">", "≥", "≠" };
#define COLLECT_N_OPS ((int)G_N_ELEMENTS(OP_TOKENS))

// Split a rule string into a leading operator index (into OP_TOKENS) and the remaining value.
static void _split_operator(const char *text, int *op_idx, const char **value)
{
  *op_idx = 0;
  *value = text ? text : "";
  if(IS_NULL_PTR(text) || text[0] == '[') return; // a [a;b] range carries no leading operator
  if(g_str_has_prefix(text, "<="))
  {
    *op_idx = 2;
    *value = text + 2;
  }
  else if(g_str_has_prefix(text, ">="))
  {
    *op_idx = 4;
    *value = text + 2;
  }
  else if(g_str_has_prefix(text, "<>"))
  {
    *op_idx = 5;
    *value = text + 2;
  }
  else if(g_str_has_prefix(text, "<"))
  {
    *op_idx = 1;
    *value = text + 1;
  }
  else if(g_str_has_prefix(text, ">"))
  {
    *op_idx = 3;
    *value = text + 1;
  }
  else if(g_str_has_prefix(text, "="))
  {
    *op_idx = 0;
    *value = text + 1;
  }
  while(**value == ' ') (*value)++;
}

// =====================================================================================
// Section 1 — module identity & presets (conf-only; unchanged contract)
// =====================================================================================

const char *name(struct dt_lib_module_t *self)
{
  return _("Library");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = { "lighttable", "map", "print", NULL };
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 400;
}

void *legacy_params(struct dt_lib_module_t *self, const void *const old_params, const size_t old_params_size,
                    const int old_version, int *new_version, size_t *new_size)
{
  if(old_version == 1 || old_version == 2)
  {
    // v1->v2 and v2->v3 only reordered/extended the property enum; presets store the property
    // index. Rather than carry the historical remap tables, drop incompatible presets.
    return NULL;
  }
  return NULL;
}

void init_presets(dt_lib_module_t *self)
{
  dt_lib_collect_params_t params;

#define CLEAR_PARAMS(r)                                                                                           \
  {                                                                                                               \
    memset(&params, 0, sizeof(params));                                                                           \
    params.rules = 1;                                                                                             \
    params.rule[0].mode = 0;                                                                                      \
    params.rule[0].item = r;                                                                                      \
  }

  GDateTime *now = g_date_time_new_now_local();
  char *datetime_today = g_date_time_format(now, "%Y:%m:%d");
  GDateTime *gdt = g_date_time_add_days(now, -1);
  char *datetime_24hrs = g_date_time_format(gdt, "> %Y:%m:%d %H:%M");
  g_date_time_unref(gdt);
  gdt = g_date_time_add_days(now, -30);
  char *datetime_30d = g_date_time_format(gdt, "> %Y:%m:%d");
  g_date_time_unref(gdt);
  g_date_time_unref(now);

  CLEAR_PARAMS(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  g_strlcpy(params.rule[0].string, datetime_today, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("imported: today"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  g_strlcpy(params.rule[0].string, datetime_24hrs, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("imported: last 24h"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  g_strlcpy(params.rule[0].string, datetime_30d, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("imported: last 30 days"), self->plugin_name, self->version(), &params, sizeof(params),
                     TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_TIME);
  g_strlcpy(params.rule[0].string, datetime_today, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("taken: today"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_TIME);
  g_strlcpy(params.rule[0].string, datetime_24hrs, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("taken: last 24h"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  CLEAR_PARAMS(DT_COLLECTION_PROP_TIME);
  g_strlcpy(params.rule[0].string, datetime_30d, PARAM_STRING_SIZE);
  dt_lib_presets_add(_("taken: last 30 days"), self->plugin_name, self->version(), &params, sizeof(params), TRUE);

  dt_free(datetime_today);
  dt_free(datetime_24hrs);
  dt_free(datetime_30d);
#undef CLEAR_PARAMS
}

// =====================================================================================
// Section 2 — conf layer (the single source of truth read by the SQL engine)
// =====================================================================================

static int _rules_count()
{
  return CLAMP(dt_conf_get_int("plugins/lighttable/collect/num_rules"), 1, MAX_RULES);
}

static void _rules_set_count(int n)
{
  dt_conf_set_int("plugins/lighttable/collect/num_rules", CLAMP(n, 1, MAX_RULES));
}

static int _rule_get_item(int n)
{
  char k[64];
  snprintf(k, sizeof(k), "plugins/lighttable/collect/item%1d", n);
  return dt_conf_get_int(k);
}

static void _rule_set_item(int n, int item)
{
  char k[64];
  snprintf(k, sizeof(k), "plugins/lighttable/collect/item%1d", n);
  dt_conf_set_int(k, item);
}

static int _rule_get_mode(int n)
{
  char k[64];
  snprintf(k, sizeof(k), "plugins/lighttable/collect/mode%1d", n);
  return dt_conf_get_int(k);
}

static void _rule_set_mode(int n, int mode)
{
  char k[64];
  snprintf(k, sizeof(k), "plugins/lighttable/collect/mode%1d", n);
  dt_conf_set_int(k, mode);
}

static gchar *_rule_get_string(int n)
{
  char k[64];
  snprintf(k, sizeof(k), "plugins/lighttable/collect/string%1d", n);
  return dt_conf_get_string(k);
}

static void _rule_set_string(int n, const char *s)
{
  char k[64];
  snprintf(k, sizeof(k), "plugins/lighttable/collect/string%1d", n);
  dt_conf_set_string(k, s ? s : "");
}

// Push the GUI state of one rule (combo property + operator + entry text) into conf. For
// numeric/date/rating properties the chosen operator is prepended to the value.
static void set_properties(dt_lib_collect_rule_t *dr)
{
  const int property = _combo_get_active_collection(dr->combo);
  const char *val = gtk_entry_get_text(GTK_ENTRY(dr->text));
  gchar *s;
  if(item_is_numeric(property) && val[0] != '[') // a [a;b] range carries its own operator
  {
    const int idx = CLAMP(gtk_combo_box_get_active(GTK_COMBO_BOX(dr->op_combo)), 0, COLLECT_N_OPS - 1);
    s = g_strconcat(OP_TOKENS[idx], val, NULL);
  }
  else
    s = g_strdup(val);
  _rule_set_string(dr->num, s);
  dt_free(s);
  _rule_set_item(dr->num, property);
}

// Pull the conf state of one rule back into the GUI (without firing the change handlers).
static void get_properties(dt_lib_collect_rule_t *dr)
{
  _combo_set_active_collection(dr->combo, _rule_get_item(dr->num));
  const int property = _combo_get_active_collection(dr->combo);
  gchar *text = _rule_get_string(dr->num);
  if(text)
  {
    g_signal_handlers_block_matched(dr->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
    g_signal_handlers_block_matched(dr->op_combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _op_changed, NULL);

    if(item_is_numeric(property))
    {
      int idx;
      const char *val;
      _split_operator(text, &idx, &val);
      gtk_combo_box_set_active(GTK_COMBO_BOX(dr->op_combo), idx);
      gtk_entry_set_text(GTK_ENTRY(dr->text), val);
    }
    else
    {
      gtk_entry_set_text(GTK_ENTRY(dr->text), text);
    }

    gtk_editable_set_position(GTK_EDITABLE(dr->text), -1);
    dr->typing = FALSE;

    g_signal_handlers_unblock_matched(dr->op_combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _op_changed, NULL);
    g_signal_handlers_unblock_matched(dr->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
    dt_free(text);
  }
}

// Rebuild the collection query from the conf rules and refresh the lighttable.
// Note: _commit() would conflict with msys64/ucrt64/include/io.h namespace on Windows.
static void _commit_colllection()
{
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

// Like _commit_colllection() but without bouncing back into our own collection_updated() handler.
static void _commit_quiet()
{
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                  darktable.view_manager->proxy.module_collect.module);
  _commit_colllection();
  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
}

static dt_lib_collect_t *get_collect(dt_lib_collect_rule_t *r)
{
  return (dt_lib_collect_t *)r->lib_collect;
}

static dt_lib_collect_rule_t *get_active_rule(dt_lib_collect_t *d)
{
  return d->rule + d->active_rule;
}

static void get_number_of_rules(dt_lib_collect_t *d)
{
  d->nb_rules = _rules_count();
  d->active_rule = CLAMP(d->active_rule, 0, d->nb_rules - 1);
}

// get_params/set_params/gui_reset rely on the conf layer above, so they stay tiny.
static void _lib_collect_update_params(dt_lib_collect_t *d)
{
  dt_lib_collect_params_t *p = d->params;
  memset(p, 0, sizeof(dt_lib_collect_params_t));
  const int n = _rules_count();
  for(int i = 0; i < n; i++)
  {
    p->rule[i].item = _rule_get_item(i);
    p->rule[i].mode = _rule_get_mode(i);
    gchar *s = _rule_get_string(i);
    if(s) g_strlcpy(p->rule[i].string, s, PARAM_STRING_SIZE);
    dt_free(s);
  }
  p->rules = n;
}

void *get_params(dt_lib_module_t *self, int *size)
{
  _lib_collect_update_params(self->data);
  *size = sizeof(dt_lib_collect_params_t);
  void *p = malloc(*size);
  memcpy(p, ((dt_lib_collect_t *)self->data)->params, *size);
  return p;
}

int set_params(dt_lib_module_t *self, const void *params, int size)
{
  dt_lib_collect_params_t *p = (dt_lib_collect_params_t *)params;
  for(uint32_t i = 0; i < p->rules; i++)
  {
    _rule_set_item(i, p->rule[i].item);
    _rule_set_mode(i, p->rule[i].mode);
    _rule_set_string(i, p->rule[i].string);
  }
  _rules_set_count(p->rules);
  _lib_collect_update_params(self->data);
  _lib_collect_gui_update(self);
  _commit_colllection();
  return 0;
}

void gui_reset(dt_lib_module_t *self)
{
  _rules_set_count(1);
  _rule_set_item(0, DT_COLLECTION_PROP_FILMROLL);
  _rule_set_mode(0, DT_LIB_COLLECT_MODE_AND);
  _rule_set_string(0, "");
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  d->active_rule = 0;
  d->view_rule = -1;
  dt_collection_set_query_flags(darktable.collection, COLLECTION_QUERY_FULL);
  _commit_colllection();
}

// =====================================================================================
// Section 3 — property combo
// =====================================================================================

static int _combo_get_active_collection(GtkWidget *combo)
{
  return GPOINTER_TO_UINT(dt_bauhaus_combobox_get_data(combo)) - 1;
}

static gboolean _combo_set_active_collection(GtkWidget *combo, const int property)
{
  const gboolean found = dt_bauhaus_combobox_set_from_value(combo, property + 1);
  if(!found) dt_bauhaus_combobox_set_from_value(combo, DT_COLLECTION_PROP_FILMROLL + 1);
  return found;
}

static void _populate_collect_combo(GtkWidget *w)
{
#define ADD_COLLECT_ENTRY(value)                                                                                  \
  dt_bauhaus_combobox_add_full(w, dt_collection_name(value), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,                     \
                               GUINT_TO_POINTER(value + 1), NULL, TRUE)

  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILMROLL);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOLDERS);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FILENAME);

  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TAG);
  for(unsigned int i = 0; i < DT_METADATA_NUMBER; i++)
  {
    const uint32_t keyid = dt_metadata_get_keyid_by_display_order(i);
    const gchar *name_ = dt_metadata_get_name(keyid);
    gchar *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name_);
    const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    dt_free(setting);
    const int meta_type = dt_metadata_get_type(keyid);
    if(meta_type != DT_METADATA_TYPE_INTERNAL && !hidden) ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_METADATA + i);
  }
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_RATING);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_COLORLABEL);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GEOTAGGING);

  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_DAY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_TIME);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_IMPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CHANGE_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPORT_TIMESTAMP);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_PRINT_TIMESTAMP);

  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_CAMERA);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LENS);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_APERTURE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_EXPOSURE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_FOCAL_LENGTH);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ISO);

  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_GROUPING);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_LOCAL_COPY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_HISTORY);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_MODULE);
  ADD_COLLECT_ENTRY(DT_COLLECTION_PROP_ORDER);
#undef ADD_COLLECT_ENTRY
}

// =====================================================================================
// Section 4 — model population (flat list + hierarchical tree) and filtering
// =====================================================================================

static int string_array_length(char **list)
{
  int length = 0;
  for(; *list; list++) length++;
  return length;
}

// NULL-terminated array of path components (drops the leading empty root component on POSIX).
static char **split_path(const char *path)
{
  if(IS_NULL_PTR(path) || !*path) return NULL;

  char **result;
  char **tokens = g_strsplit(path, G_DIR_SEPARATOR_S, -1);

#ifdef _WIN32
  if(!(g_ascii_isalpha(tokens[0][0]) && tokens[0][strlen(tokens[0]) - 1] == ':'))
  {
    g_strfreev(tokens);
    tokens = NULL;
  }
  result = tokens;
#else
  const unsigned int size = g_strv_length(tokens);
  result = malloc(sizeof(char *) * size);
  for(unsigned int i = 0; i < size; i++) result[i] = tokens[i + 1];
  dt_free(tokens[0]);
  dt_free(tokens);
#endif
  return result;
}

typedef struct name_key_tuple_t
{
  char *name, *collate_key;
  int count, status;
} name_key_tuple_t;

static void free_tuple(gpointer data)
{
  name_key_tuple_t *tuple = (name_key_tuple_t *)data;
  dt_free(tuple->name);
  dt_free(tuple->collate_key);
  dt_free(tuple);
}

static gint sort_folder_tag(gconstpointer a, gconstpointer b)
{
  const name_key_tuple_t *ta = (const name_key_tuple_t *)a;
  const name_key_tuple_t *tb = (const name_key_tuple_t *)b;
  return g_strcmp0(ta->collate_key, tb->collate_key);
}

// Sort key so that "not tagged" & "darktable|" come first, sub-tags directly behind their parent.
static char *tag_collate_key(char *tag)
{
  const size_t len = strlen(tag);
  char *result = g_malloc(len + 2);
  if(!g_strcmp0(tag, _("not tagged")))
    *result = '\1';
  else if(g_str_has_prefix(tag, "darktable|"))
    *result = '\2';
  else
    *result = '\3';
  memcpy(result + 1, tag, len + 1);
  for(char *iter = result + 1; *iter; iter++)
    if(*iter == '|') *iter = '\1';
  return result;
}

static void tree_count_show(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                            GtkTreeIter *iter, gpointer data)
{
  gchar *name;
  guint count;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_TEXT, &name, DT_LIB_COLLECT_COL_COUNT, &count, -1);
  if(!count)
    g_object_set(renderer, "text", name, NULL);
  else
  {
    gchar *coltext = g_strdup_printf("%s (%d)", name, count);
    g_object_set(renderer, "text", coltext, NULL);
    dt_free(coltext);
  }
  dt_free(name);
}

// ---- search filtering (list) ----
static gboolean list_match_string(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *)data;
  gchar *str = NULL;
  gboolean visible = FALSE;
  gboolean was_visible;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_VISIBLE, &was_visible, -1);

  gchar *haystack = g_utf8_strdown(str, -1);
  const gchar *needle = dr->searchstring;
  const int property = _combo_get_active_collection(dr->combo);

  if(property == DT_COLLECTION_PROP_APERTURE || property == DT_COLLECTION_PROP_FOCAL_LENGTH
     || property == DT_COLLECTION_PROP_ISO || property == DT_COLLECTION_PROP_RATING)
  {
    visible = TRUE;
    gchar *operator, * number, *number2;
    dt_collection_split_operator_number(needle, &number, &number2, &operator);
    if(number)
    {
      const float nb1 = g_strtod(number, NULL);
      const float nb2 = g_strtod(haystack, NULL);
      if(operator&& strcmp(operator, ">") == 0)
        visible = (nb2 > nb1);
      else if(operator&& strcmp(operator, ">=") == 0)
        visible = (nb2 >= nb1);
      else if(operator&& strcmp(operator, "<") == 0)
        visible = (nb2 < nb1);
      else if(operator&& strcmp(operator, "<=") == 0)
        visible = (nb2 <= nb1);
      else if(operator&& strcmp(operator, "<>") == 0)
        visible = (nb1 != nb2);
      else if(operator&& number2 && strcmp(operator, "[]") == 0)
      {
        const float nb3 = g_strtod(number2, NULL);
        visible = (nb2 >= nb1 && nb2 <= nb3);
      }
      else
        visible = (nb1 == nb2);
    }
    dt_free(operator);
    dt_free(number);
    dt_free(number2);
  }
  else if(property == DT_COLLECTION_PROP_FILENAME && strchr(needle, ',') != NULL)
  {
    GList *list = dt_util_str_to_glist(",", needle);
    for(const GList *l = list; l; l = g_list_next(l))
    {
      const char *name = (char *)l->data;
      if((visible = (g_strrstr(haystack, name + (name[0] == '%')) != NULL))) break;
    }
    g_list_free_full(list, dt_free_gpointer);
  }
  else
  {
    if(needle[0] == '%') needle++;
    if(!needle[0])
      visible = TRUE;
    else if(!needle[1])
      visible = (strchr(haystack, needle[0]) != NULL);
    else
      visible = (g_strrstr(haystack, needle) != NULL);
  }

  dt_free(haystack);
  dt_free(str);
  if(visible != was_visible)
    gtk_list_store_set(GTK_LIST_STORE(model), iter, DT_LIB_COLLECT_COL_VISIBLE, visible, -1);
  return FALSE;
}

// ---- search filtering (tree) ----
static gboolean tree_match_string(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *)data;
  gchar *str = NULL;
  gboolean cur_state, visible;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_VISIBLE, &cur_state, -1);

  if(dr->typing == FALSE && !cur_state)
    visible = TRUE;
  else
  {
    gchar *haystack = g_utf8_strdown(str, -1),
          *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(dr->text)), -1);
    visible = (g_strrstr(haystack, needle) != NULL);
    dt_free(haystack);
    dt_free(needle);
  }
  dt_free(str);
  gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_COLLECT_COL_VISIBLE, visible, -1);
  return FALSE;
}

static gboolean tree_reveal_func(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  gboolean state;
  GtkTreeIter parent, child = *iter;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_VISIBLE, &state, -1);
  if(!state) return FALSE;
  while(gtk_tree_model_iter_parent(model, &parent, &child))
  {
    gtk_tree_store_set(GTK_TREE_STORE(model), &parent, DT_LIB_COLLECT_COL_VISIBLE, TRUE, -1);
    child = parent;
  }
  return FALSE;
}

static void tree_set_visibility(GtkTreeModel *model, gpointer data)
{
  gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)tree_match_string, data);
  gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)tree_reveal_func, NULL);
}

// Turn a partial date string ("2021", "2021:06:15", "2021:06:15 13:00", ...) into a comparable
// 14-digit YYYYMMDDHHMMSS number, keeping only digits (separator-agnostic) and padding the
// unspecified low-order part with `pad`. Pad '0' yields the earliest instant of the prefix,
// pad '9' an upper bound past its latest instant.
static guint64 _date_key(const char *s, char pad)
{
  char digits[15];
  int n = 0;
  for(const char *p = s; p && *p && n < 14; p++)
    if(g_ascii_isdigit(*p)) digits[n++] = *p;
  while(n < 14) digits[n++] = pad;
  digits[14] = '\0';
  return g_ascii_strtoull(digits, NULL, 10);
}

typedef struct _date_range_t
{
  guint64 lo, hi; // inclusive bounds as _date_key() numbers
} _date_range_t;

// Reduce a date tree to the nodes whose date prefix overlaps [lo;hi]. A node's prefix spans
// [node_lo;node_hi]; it overlaps the range iff node_hi >= lo AND node_lo <= hi. Pair this with
// tree_reveal_func() so the visible leaves' ancestors stay visible too.
static gboolean tree_range_visible(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  const _date_range_t *r = (const _date_range_t *)data;
  gchar *str = NULL;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, -1);
  const guint64 node_lo = _date_key(str, '0');
  const guint64 node_hi = _date_key(str, '9');
  dt_free(str);
  const gboolean visible = (node_hi >= r->lo) && (node_lo <= r->hi);
  gtk_tree_store_set(GTK_TREE_STORE(model), iter, DT_LIB_COLLECT_COL_VISIBLE, visible, -1);
  return FALSE;
}

static gboolean list_select(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *)data;
  dt_lib_collect_t *d = get_collect(dr);
  gchar *str = NULL;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, -1);

  gchar *haystack = g_utf8_strdown(str, -1);
  gchar *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(dr->text)), -1);
  if(strcmp(haystack, needle) == 0)
  {
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(d->view), path);
    gtk_tree_view_scroll_to_cell(d->view, path, NULL, FALSE, 0.2, 0);
  }
  dt_free(haystack);
  dt_free(needle);
  dt_free(str);
  return FALSE;
}

static gboolean range_select(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  _range_t *range = (_range_t *)data;
  gchar *str = NULL;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, -1);

  gchar *haystack = g_utf8_strdown(str, -1);
  gchar *needle = range->path1 ? g_utf8_strdown(range->stop, -1) : g_utf8_strdown(range->start, -1);
  if(strcmp(haystack, needle) == 0)
  {
    if(range->path1)
    {
      range->path2 = gtk_tree_path_copy(path);
      dt_free(haystack);
      dt_free(needle);
      dt_free(str);
      return TRUE;
    }
    else
      range->path1 = gtk_tree_path_copy(path);
  }
  dt_free(haystack);
  dt_free(needle);
  dt_free(str);
  return FALSE;
}

static gboolean tree_expand(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  dt_lib_collect_rule_t *dr = (dt_lib_collect_rule_t *)data;
  dt_lib_collect_t *d = get_collect(dr);
  gchar *str = NULL, *txt = NULL;
  gboolean startwildcard = FALSE, expanded = FALSE;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &str, DT_LIB_COLLECT_COL_TEXT, &txt, -1);

  gchar *haystack = g_utf8_strdown(str, -1);
  gchar *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(dr->text)), -1);
  gchar *txt2 = g_utf8_strdown(txt, -1);
  const int property = _combo_get_active_collection(dr->combo);

  // While typing, or right after a view/tab switch that preserved a query, we want to actively
  // reveal the matching node(s); on a plain refresh we leave the tree where the user left it.
  const gboolean reveal = dr->typing || dr->reveal;

  if(g_str_has_prefix(needle, "%")) startwildcard = TRUE;
  if(g_str_has_suffix(needle, "%")) needle[strlen(needle) - 1] = '\0';
  if(g_str_has_suffix(haystack, "%")) haystack[strlen(haystack) - 1] = '\0';
  if(property == DT_COLLECTION_PROP_TAG || property == DT_COLLECTION_PROP_GEOTAGGING)
  {
    if(g_str_has_suffix(needle, "*")) needle[strlen(needle) - 1] = '\0'; // hierarchy + sub
    if(g_str_has_suffix(needle, "|")) needle[strlen(needle) - 1] = '\0';
    if(g_str_has_suffix(haystack, "|")) haystack[strlen(haystack) - 1] = '\0';
  }
  else if(property == DT_COLLECTION_PROP_FOLDERS)
  {
    if(g_str_has_suffix(needle, "*")) needle[strlen(needle) - 1] = '\0';
    if(g_str_has_suffix(needle, "/")) needle[strlen(needle) - 1] = '\0';
    if(g_str_has_suffix(haystack, "/")) haystack[strlen(haystack) - 1] = '\0';
  }
  else if(DT_COLLECTION_PROP_DAY == property || is_time_property(property))
  {
    if(g_str_has_suffix(needle, ":")) needle[strlen(needle) - 1] = '\0';
    if(g_str_has_suffix(haystack, ":")) haystack[strlen(haystack) - 1] = '\0';
  }

  if(reveal && g_strrstr(txt2, needle) != NULL)
  {
    gtk_tree_view_expand_to_path(d->view, path);
    expanded = TRUE;
  }

  if(strlen(needle) == 0)
  {
    // keep collapsed
  }
  else if(strcmp(haystack, needle) == 0)
  {
    gtk_tree_view_expand_to_path(d->view, path);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(d->view), path);
    gtk_tree_view_scroll_to_cell(d->view, path, NULL, FALSE, 0.2, 0);
    expanded = TRUE;
  }
  else if(startwildcard && g_strrstr(haystack, needle + 1) != NULL)
  {
    gtk_tree_view_expand_to_path(d->view, path);
    expanded = TRUE;
  }
  else if((reveal || property != DT_COLLECTION_PROP_FOLDERS) && g_str_has_prefix(haystack, needle))
  {
    gtk_tree_view_expand_to_path(d->view, path);
    expanded = TRUE;
  }

  dt_free(haystack);
  dt_free(needle);
  dt_free(txt2);
  dt_free(str);
  dt_free(txt);
  return expanded;
}

// Walk down a folder tree through single-child nodes and return the path of the deepest such node
// (the unique common root), so the filtered model can hide the redundant leading folders. Returns
// NULL when there is nothing to collapse. Stops descending at a node that is itself a film-roll.
static GtkTreePath *_folders_root_collapse_path(GtkTreeModel *model)
{
  GtkTreeIter child, iter;
  int level = 0;
  while(gtk_tree_model_iter_n_children(model, level > 0 ? &iter : NULL) > 0)
  {
    if(level > 0)
    {
      gchar *pth = NULL;
      gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &pth, -1);
      const int id = dt_film_get_id(pth); // is this folder a known film-roll?
      dt_free(pth);
      if(id != -1)
      {
        if(!gtk_tree_model_iter_parent(model, &child, &iter)) level = 0;
        iter = child;
        break;
      }
    }
    if(gtk_tree_model_iter_n_children(model, level > 0 ? &iter : NULL) != 1) break;
    gtk_tree_model_iter_children(model, &child, level > 0 ? &iter : NULL);
    iter = child;
    level++;
  }
  if(level <= 0) return NULL;

  if(gtk_tree_model_iter_n_children(model, &iter) == 0 && gtk_tree_model_iter_parent(model, &child, &iter))
    return gtk_tree_model_get_path(model, &child);
  return gtk_tree_model_get_path(model, &iter);
}

// Build the filtered model; for folders, collapse a unique common root into the virtual root.
static GtkTreeModel *_create_filtered_model(GtkTreeModel *model, dt_lib_collect_rule_t *dr)
{
  GtkTreePath *path = (_combo_get_active_collection(dr->combo) == DT_COLLECTION_PROP_FOLDERS)
                          ? _folders_root_collapse_path(model)
                          : NULL;
  GtkTreeModel *filter = gtk_tree_model_filter_new(model, path);
  gtk_tree_path_free(path);
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(filter), DT_LIB_COLLECT_COL_VISIBLE);
  return filter;
}

static const char *UNCATEGORIZED_TAG = N_("uncategorized");

// --- preserve tree expansion across a rebuild so the user keeps their place ---
static void _collect_expanded_cb(GtkTreeView *view, GtkTreePath *path, gpointer data)
{
  GHashTable *set = (GHashTable *)data;
  GtkTreeModel *model = gtk_tree_view_get_model(view);
  GtkTreeIter it;
  if(gtk_tree_model_get_iter(model, &it, path))
  {
    gchar *p = NULL;
    gtk_tree_model_get(model, &it, DT_LIB_COLLECT_COL_PATH, &p, -1);
    if(p) g_hash_table_add(set, p); // hash table takes ownership of p
  }
}

typedef struct _expand_ctx_t
{
  dt_lib_collect_t *d;
  GHashTable *set;
} _expand_ctx_t;

static gboolean _restore_expanded_cb(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer data)
{
  _expand_ctx_t *c = (_expand_ctx_t *)data;
  gchar *p = NULL;
  gtk_tree_model_get(model, iter, DT_LIB_COLLECT_COL_PATH, &p, -1);
  if(p && g_hash_table_contains(c->set, p)) gtk_tree_view_expand_to_path(c->d->view, path);
  dt_free(p);
  return FALSE;
}

// Split a tree value into its path components for the active property.
static char **_split_tree_name(int property, const char *name)
{
  if(property == DT_COLLECTION_PROP_FOLDERS) return split_path(name);
  if(property == DT_COLLECTION_PROP_DAY) return g_strsplit(name, ":", -1);
  if(is_time_property(property)) return g_strsplit_set(name, ": ", 4);
  return g_strsplit(name, "|", -1);
}

// Add `count` to every ancestor of `leaf` so parent folders/dates show the total beneath them
// (fixes #537 for the displayed count).
static void _propagate_count_to_ancestors(GtkTreeStore *store, GtkTreeIter *leaf, int count)
{
  GtkTreeModel *model = GTK_TREE_MODEL(store);
  GtkTreeIter parent, child = *leaf;
  while(gtk_tree_model_iter_parent(model, &parent, &child))
  {
    guint parentcount;
    gtk_tree_model_get(model, &parent, DT_LIB_COLLECT_COL_COUNT, &parentcount, -1);
    gtk_tree_store_set(store, &parent, DT_LIB_COLLECT_COL_COUNT, count + parentcount, -1);
    child = parent;
  }
}

// A top-level tag with no children of its own is filed under a synthetic "uncategorized" node
// (created lazily). Returns TRUE when `name` was filed this way, so the caller skips it.
static gboolean _maybe_file_uncategorized(GtkTreeStore *store, const char *name, const char *next_name_raw,
                                          GtkTreeIter *uncategorized, guint *index, int count)
{
  if(strchr(name, '|') != NULL) return FALSE; // has a hierarchy of its own

  char *next_name = g_strdup(next_name_raw ? next_name_raw : "");
  if(strlen(next_name) >= strlen(name) + 1 && next_name[strlen(name)] == '|') next_name[strlen(name)] = '\0';
  const gboolean leaf_toplevel = g_strcmp0(next_name, name) && g_strcmp0(name, _("not tagged"));
  dt_free(next_name);
  if(!leaf_toplevel) return FALSE;

  if(!uncategorized->stamp)
  {
    gtk_tree_store_insert_with_values(store, uncategorized, NULL, -1, DT_LIB_COLLECT_COL_TEXT,
                                      _(UNCATEGORIZED_TAG), DT_LIB_COLLECT_COL_PATH, "",
                                      DT_LIB_COLLECT_COL_VISIBLE, TRUE, DT_LIB_COLLECT_COL_INDEX, *index,
                                      DT_LIB_COLLECT_COL_FONT, PANGO_WEIGHT_NORMAL, -1);
    (*index)++;
  }
  GtkTreeIter temp;
  gtk_tree_store_insert_with_values(store, &temp, uncategorized, 0, DT_LIB_COLLECT_COL_TEXT, name,
                                    DT_LIB_COLLECT_COL_PATH, name, DT_LIB_COLLECT_COL_VISIBLE, TRUE,
                                    DT_LIB_COLLECT_COL_COUNT, count, DT_LIB_COLLECT_COL_INDEX, *index,
                                    DT_LIB_COLLECT_COL_FONT, PANGO_WEIGHT_NORMAL, -1);
  (*index)++;
  return TRUE;
}

// Pull the raw values from the SQL engine and sort them ourselves: sqlite knows nothing about path
// separators, so we order by a path-aware collate key and build the tree by hand. Caller frees with
// g_list_free_full(list, free_tuple).
static GList *_collect_sorted_tree_names(int property, int rule)
{
  GList *sorted_names = NULL;
  GList *values = dt_collection_get_property_values(property, rule);
  for(GList *v = values; v; v = g_list_next(v))
  {
    dt_collection_name_value_t *nv = (dt_collection_name_value_t *)v->data;
    char *name = g_strdup(nv->name ? nv->name : "");
    gchar *collate_key;
    if(property == DT_COLLECTION_PROP_FOLDERS)
    {
      char *name_folded = g_utf8_casefold(name, -1);
      char *name_folded_slash = g_strconcat(name_folded, G_DIR_SEPARATOR_S, NULL);
      collate_key = g_utf8_collate_key_for_filename(name_folded_slash, -1);
      dt_free(name_folded_slash);
      dt_free(name_folded);
    }
    else
      collate_key = tag_collate_key(name);

    name_key_tuple_t *tuple = (name_key_tuple_t *)malloc(sizeof(name_key_tuple_t));
    tuple->name = name;
    tuple->collate_key = collate_key;
    tuple->count = nv->count;
    tuple->status = (property == DT_COLLECTION_PROP_FOLDERS) ? nv->status : -1;
    sorted_names = g_list_prepend(sorted_names, tuple);
  }
  g_list_free_full(values, dt_collection_name_value_free);

  sorted_names = g_list_sort(sorted_names, sort_folder_tag);
  if(!dt_conf_get_bool("plugins/collect/descending")) sorted_names = g_list_reverse(sorted_names);
  return sorted_names;
}

// Build the hierarchical tree store from the pre-sorted names. Each name is split into path
// components; consecutive names share their common prefix, so we only insert the new tail under
// the right parent (which is why the input must be path-sorted).
static void _build_tree_store(GtkTreeStore *store, int property, GList *sorted_names,
                              gboolean no_uncategorized, const char *format_separator)
{
  GtkTreeModel *model = GTK_TREE_MODEL(store);
  GtkTreeIter uncategorized = { 0 };
  char **last_tokens = NULL;
  int last_tokens_length = 0;
  GtkTreeIter last_parent = { 0 };
  guint index = 0;

  for(GList *names = sorted_names; names; names = g_list_next(names))
  {
    name_key_tuple_t *tuple = (name_key_tuple_t *)names->data;
    char *name = tuple->name;
    const int count = tuple->count;
    const int status = tuple->status;
    if(IS_NULL_PTR(name)) continue;

    const char *next_name = names->next ? ((name_key_tuple_t *)names->next->data)->name : NULL;
    if(!no_uncategorized && _maybe_file_uncategorized(store, name, next_name, &uncategorized, &index, count))
      continue;

    char **tokens = _split_tree_name(property, name);
    if(IS_NULL_PTR(tokens)) continue;

    GtkTreeIter parent = last_parent;
    const int tokens_length = string_array_length(tokens);
    int common_length = 0;
    if(last_tokens)
    {
      while(tokens[common_length] && last_tokens[common_length]
            && !g_strcmp0(tokens[common_length], last_tokens[common_length]))
        common_length++;
      for(int i = common_length; i < last_tokens_length; i++)
      {
        gtk_tree_model_iter_parent(model, &parent, &last_parent);
        last_parent = parent;
      }
    }

    char *pth = NULL;
#ifndef _WIN32
    if(property == DT_COLLECTION_PROP_FOLDERS) pth = g_strdup("/");
#endif
    for(int i = 0; i < common_length; i++) pth = dt_util_dstrcat(pth, format_separator, tokens[i]);

    for(char **token = &tokens[common_length]; *token; token++)
    {
      GtkTreeIter iter;
      pth = dt_util_dstrcat(pth, format_separator, *token);
      if(is_time_property(property) && !*(token + 1)) pth[10] = ' ';

      gchar *pth2 = g_strdup(pth);
      pth2[strlen(pth2) - 1] = '\0';
      const gboolean leaf = !*(token + 1);
      gtk_tree_store_insert_with_values(store, &iter, common_length > 0 ? &parent : NULL, 0,
                                        DT_LIB_COLLECT_COL_TEXT, *token, DT_LIB_COLLECT_COL_PATH, pth2,
                                        DT_LIB_COLLECT_COL_VISIBLE, TRUE, DT_LIB_COLLECT_COL_COUNT,
                                        (leaf ? count : 0), DT_LIB_COLLECT_COL_INDEX, index,
                                        DT_LIB_COLLECT_COL_UNREACHABLE, (leaf ? !status : 0),
                                        DT_LIB_COLLECT_COL_FONT, PANGO_WEIGHT_NORMAL, -1);
      index++;
      const gboolean recursive_count = property == DT_COLLECTION_PROP_DAY || is_time_property(property)
                                       || property == DT_COLLECTION_PROP_FOLDERS;
      if(recursive_count && leaf) _propagate_count_to_ancestors(store, &iter, count);
      common_length++;
      parent = iter;
      dt_free(pth2);
    }
    dt_free(pth);

    if(last_tokens) g_strfreev(last_tokens);
    last_tokens = tokens;
    last_parent = parent;
    last_tokens_length = tokens_length;
  }
  g_strfreev(last_tokens);
}

// Hierarchical (tree) properties: folders, tags, geotags, day, date-times.
static void _populate_tree(dt_lib_collect_rule_t *dr)
{
  dt_lib_collect_t *d = get_collect(dr);
  const int property = _combo_get_active_collection(dr->combo);
  char *format_separator = "";

  switch(property)
  {
    case DT_COLLECTION_PROP_FOLDERS:
      format_separator = "%s" G_DIR_SEPARATOR_S;
      break;
    case DT_COLLECTION_PROP_TAG:
    case DT_COLLECTION_PROP_GEOTAGGING:
      format_separator = "%s|";
      break;
    case DT_COLLECTION_PROP_DAY:
    case DT_COLLECTION_PROP_TIME:
    case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
    case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
      format_separator = "%s:";
      break;
  }

  set_properties(dr);

  GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(d->treefilter));
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model), GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);

  GHashTable *saved_expanded = NULL; // expanded node paths, captured across a rebuild

  if(d->view_rule != property)
  {
    // remember which nodes were expanded so the rebuild doesn't lose the user's place
    saved_expanded = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gtk_tree_view_map_expanded_rows(d->view, _collect_expanded_cb, saved_expanded);

    g_object_ref(model);
    g_object_unref(d->treefilter);
    gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), NULL);
    gtk_tree_store_clear(GTK_TREE_STORE(model));
    gtk_widget_hide(GTK_WIDGET(d->view));

    const gboolean no_uncategorized = (property == DT_COLLECTION_PROP_TAG)
                                          ? dt_conf_get_bool("plugins/lighttable/tagging/no_uncategorized")
                                          : TRUE;
    GList *sorted_names = _collect_sorted_tree_names(property, dr->num);
    _build_tree_store(GTK_TREE_STORE(model), property, sorted_names, no_uncategorized, format_separator);
    g_list_free_full(sorted_names, free_tuple);

    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->view), DT_LIB_COLLECT_COL_TOOLTIP);
    d->treefilter = _create_filtered_model(model, dr);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);

    gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), d->treefilter);
    gtk_widget_set_no_show_all(GTK_WIDGET(d->view), FALSE);
    gtk_widget_show_all(GTK_WIDGET(d->view));

    g_object_unref(model);
    d->view_rule = property;
  }

  // A [a;b] range on a numeric/date tree property reduces the tree to the in-range nodes (like
  // a text search reduces folder/filename lists), instead of a substring filter that — since no
  // node literally contains "[a;b]" — would hide everything.
  _range_t *range = NULL;
  if(item_is_numeric(property))
  {
    GRegex *regex = g_regex_new("^\\s*\\[\\s*(.*)\\s*;\\s*(.*)\\s*\\]\\s*$", 0, 0, NULL);
    GMatchInfo *match_info;
    g_regex_match_full(regex, gtk_entry_get_text(GTK_ENTRY(dr->text)), -1, 0, 0, &match_info, NULL);
    if(g_match_info_get_match_count(match_info) == 3)
    {
      range = (_range_t *)calloc(1, sizeof(_range_t));
      range->start = g_match_info_fetch(match_info, 2); // inverted: dates are reverse-ordered
      range->stop = g_match_info_fetch(match_info, 1);
    }
    g_match_info_free(match_info);
    g_regex_unref(regex);
  }

  if(range)
  {
    // restrict the visible nodes to the dates within [start;stop] (bounds taken order-agnostic)
    const guint64 a_lo = _date_key(range->start, '0'), a_hi = _date_key(range->start, '9');
    const guint64 b_lo = _date_key(range->stop, '0'), b_hi = _date_key(range->stop, '9');
    _date_range_t dvr = { MIN(a_lo, b_lo), MAX(a_hi, b_hi) };
    gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)tree_range_visible, &dvr);
    gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)tree_reveal_func, NULL);
  }
  else if(dr->typing)
    tree_set_visibility(model, dr);

  gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(d->view));

  // Active search (text or range) collapses, then we expand just the matches. When merely
  // refreshing/browsing, restore the previous expansion so the view stays where the user was.
  if(range)
    gtk_tree_view_expand_all(d->view); // reveal the reduced in-range set
  else if(dr->typing)
    gtk_tree_view_collapse_all(d->view);
  else if(saved_expanded)
  {
    _expand_ctx_t ctx = { d, saved_expanded };
    gtk_tree_model_foreach(d->treefilter, _restore_expanded_cb, &ctx);
  }
  if(saved_expanded) g_hash_table_destroy(saved_expanded);

  if(range)
  {
    // also select the boundary rows when the typed bounds match actual leaves
    gtk_tree_model_foreach(d->treefilter, (GtkTreeModelForeachFunc)range_select, range);
    if(range->path1 && range->path2)
      gtk_tree_selection_select_range(gtk_tree_view_get_selection(d->view), range->path1, range->path2);
    dt_free(range->start);
    dt_free(range->stop);
    gtk_tree_path_free(range->path1);
    gtk_tree_path_free(range->path2);
    dt_free(range);
  }
  else
    gtk_tree_model_foreach(d->treefilter, (GtkTreeModelForeachFunc)tree_expand, dr);
}

// Flat (list) properties: film-rolls, camera, lens, filename, rating, metadata, ...
static void _populate_list(dt_lib_collect_rule_t *dr)
{
  dt_lib_collect_t *d = get_collect(dr);
  const int property = _combo_get_active_collection(dr->combo);

  set_properties(dr);

  GtkTreeModel *model = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(d->listfilter));
  if(d->view_rule != property)
  {
    GtkTreeIter iter;
    g_object_unref(d->listfilter);
    g_object_ref(model);
    gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), NULL);
    gtk_list_store_clear(GTK_LIST_STORE(model));
    gtk_widget_hide(GTK_WIDGET(d->view));

    GList *values = dt_collection_get_property_values(property, dr->num);
    for(GList *v = values; v; v = g_list_next(v))
    {
      dt_collection_name_value_t *nv = (dt_collection_name_value_t *)v->data;
      if(IS_NULL_PTR(nv->name)) continue;
      const char *value = nv->name;
      // film-rolls show a shortened folder name but keep the full path for queries/management
      const char *display = (property == DT_COLLECTION_PROP_FILMROLL) ? dt_image_film_roll_name(value) : value;
      const int unreachable = (property == DT_COLLECTION_PROP_FILMROLL) ? (nv->status == 0) : 0;

      gchar *text = g_strdup(value);
      gchar *ptr = text;
      while(!g_utf8_validate(ptr, -1, (const gchar **)&ptr)) ptr[0] = '?';
      gchar *escaped_text = g_markup_escape_text(text, -1);

      gtk_list_store_append(GTK_LIST_STORE(model), &iter);
      gtk_list_store_set(GTK_LIST_STORE(model), &iter, DT_LIB_COLLECT_COL_TEXT, display, DT_LIB_COLLECT_COL_ID,
                         nv->id, DT_LIB_COLLECT_COL_TOOLTIP, escaped_text, DT_LIB_COLLECT_COL_PATH, value,
                         DT_LIB_COLLECT_COL_VISIBLE, TRUE, DT_LIB_COLLECT_COL_COUNT, nv->count,
                         DT_LIB_COLLECT_COL_UNREACHABLE, unreachable, DT_LIB_COLLECT_COL_FONT, PANGO_WEIGHT_NORMAL,
                         -1);
      dt_free(text);
      dt_free(escaped_text);
    }
    g_list_free_full(values, dt_collection_name_value_free);

    gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(d->view), DT_LIB_COLLECT_COL_TOOLTIP);
    d->listfilter = _create_filtered_model(model, dr);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(d->view));
    const gboolean multi = item_is_numeric(property) || property == DT_COLLECTION_PROP_FILMROLL;
    gtk_tree_selection_set_mode(selection, multi ? GTK_SELECTION_MULTIPLE : GTK_SELECTION_SINGLE);

    gtk_tree_view_set_model(GTK_TREE_VIEW(d->view), d->listfilter);
    gtk_widget_set_no_show_all(GTK_WIDGET(d->view), FALSE);
    gtk_widget_show_all(GTK_WIDGET(d->view));
    g_object_unref(model);
    d->view_rule = property;
  }

  // restrict to matching entries while typing
  if(dr->typing
     && (property == DT_COLLECTION_PROP_CAMERA || property == DT_COLLECTION_PROP_FILENAME
         || property == DT_COLLECTION_PROP_FILMROLL || property == DT_COLLECTION_PROP_LENS
         || property == DT_COLLECTION_PROP_APERTURE || property == DT_COLLECTION_PROP_FOCAL_LENGTH
         || property == DT_COLLECTION_PROP_ISO || property == DT_COLLECTION_PROP_MODULE
         || property == DT_COLLECTION_PROP_ORDER || property == DT_COLLECTION_PROP_RATING
         || (property >= DT_COLLECTION_PROP_METADATA
             && property < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)))
  {
    gchar *needle = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(dr->text)), -1);
    if(g_str_has_suffix(needle, "%")) needle[strlen(needle) - 1] = '\0';
    dr->searchstring = needle;
    gtk_tree_model_foreach(model, (GtkTreeModelForeachFunc)list_match_string, dr);
    dr->searchstring = NULL;
    dt_free(needle);
  }
  gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(d->view));

  if(item_is_numeric(property))
  {
    GRegex *regex = g_regex_new("^\\s*\\[\\s*(.*)\\s*;\\s*(.*)\\s*\\]\\s*$", 0, 0, NULL);
    GMatchInfo *match_info;
    g_regex_match_full(regex, gtk_entry_get_text(GTK_ENTRY(dr->text)), -1, 0, 0, &match_info, NULL);
    const int match_count = g_match_info_get_match_count(match_info);
    if(match_count == 3)
    {
      _range_t *range = (_range_t *)calloc(1, sizeof(_range_t));
      range->start = g_match_info_fetch(match_info, 1);
      range->stop = g_match_info_fetch(match_info, 2);
      gtk_tree_model_foreach(d->listfilter, (GtkTreeModelForeachFunc)range_select, range);
      if(range->path1 && range->path2)
        gtk_tree_selection_select_range(gtk_tree_view_get_selection(d->view), range->path1, range->path2);
      dt_free(range->start);
      dt_free(range->stop);
      gtk_tree_path_free(range->path1);
      gtk_tree_path_free(range->path2);
      dt_free(range);
    }
    else
      gtk_tree_model_foreach(d->listfilter, (GtkTreeModelForeachFunc)list_select, dr);
    g_match_info_free(match_info);
    g_regex_unref(regex);
  }
  else
    gtk_tree_model_foreach(d->listfilter, (GtkTreeModelForeachFunc)list_select, dr);
}

static void update_view(dt_lib_collect_rule_t *dr)
{
  const int property = _combo_get_active_collection(dr->combo);
  if(item_is_tree(property))
    _populate_tree(dr);
  else
    _populate_list(dr);
  dr->reveal = FALSE; // one-shot: consumed by this rebuild
}

// =====================================================================================
// Section 5 — clicking a value row -> write the rule text and refresh the collection
// =====================================================================================

// Append the tag/geotag hierarchy suffix chosen by the modifier keys. Consumes and replaces
// `text`: ctrl -> sub-hierarchies only ("|%"), plain -> this hierarchy + sub ("*"), shift -> the
// exact node (no suffix).
static gchar *_decorate_hierarchy(gchar *text, GdkEventButton *event)
{
  if(event && dt_modifier_is(event->state, GDK_CONTROL_MASK))
  {
    gchar *n = g_strconcat(text, "|%", NULL);
    dt_free(text);
    return n;
  }
  if(!event || !dt_modifier_is(event->state, GDK_SHIFT_MASK))
  {
    gchar *n = g_strconcat(text, "*", NULL);
    dt_free(text);
    return n;
  }
  return text;
}

// Clicking a leaf tag in the first rule adopts that tag's saved sort order. Returns TRUE (with
// *order filled) when an order change should be signalled.
static gboolean _adopt_tag_order(const char *text, int *order)
{
  const uint32_t tagid = dt_tag_get_tag_id_by_name(text);
  if(!tagid)
  {
    dt_collection_set_tag_id((dt_collection_t *)darktable.collection, 0);
    return FALSE;
  }
  uint32_t sort = DT_COLLECTION_SORT_NONE;
  gboolean descending = FALSE;
  if(dt_tag_get_tag_order_by_id(tagid, &sort, &descending))
    *order = sort | (descending ? DT_COLLECTION_ORDER_FLAG : 0);
  else
  {
    *order = DT_COLLECTION_SORT_FILENAME;
    dt_tag_set_tag_order_by_id(tagid, *order & ~DT_COLLECTION_ORDER_FLAG, *order & DT_COLLECTION_ORDER_FLAG);
  }
  dt_collection_set_tag_id((dt_collection_t *)darktable.collection, tagid);
  return TRUE;
}

static void row_activated(GtkTreeView *view, GtkTreePath *path, GdkEventButton *event, dt_lib_collect_t *d)
{
  GtkTreeIter iter;
  GtkTreeModel *model = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(view);
  const int n_selected = gtk_tree_selection_count_selected_rows(selection);
  if(n_selected < 1) return;

  GList *sels = gtk_tree_selection_get_selected_rows(selection, &model);
  GtkTreePath *path1 = (GtkTreePath *)sels->data;
  if(!gtk_tree_model_get_iter(model, &iter, path1))
  {
    g_list_free_full(sels, (GDestroyNotify)gtk_tree_path_free);
    return;
  }

  get_number_of_rules(d);
  dt_lib_collect_rule_t *active_rule = get_active_rule(d);
  active_rule->typing = FALSE;
  const int item = d->view_rule;

  gchar *text;
  gboolean order_request = FALSE;
  int order = 0;
  gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);

  if(text && strlen(text) > 0)
  {
    if(n_selected > 1 && item_is_numeric(item))
    {
      // range selection [a;b]
      GtkTreeIter iter2;
      GtkTreePath *path2 = (GtkTreePath *)g_list_last(sels)->data;
      if(gtk_tree_model_get_iter(model, &iter2, path2))
      {
        gchar *text2;
        gtk_tree_model_get(model, &iter2, DT_LIB_COLLECT_COL_PATH, &text2, -1);
        gchar *n_text = (item == DT_COLLECTION_PROP_DAY || is_time_property(item))
                            ? g_strdup_printf("[%s;%s]", text2, text) // dates are reverse-ordered
                            : g_strdup_printf("[%s;%s]", text, text2);
        dt_free(text);
        dt_free(text2);
        text = n_text;
      }
    }
    else if(item == DT_COLLECTION_PROP_FOLDERS)
    {
      // recursion is driven by the explicit "include sub-folders" checkbox (#537)
      if(d->recursive_check && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(d->recursive_check)))
      {
        gchar *n_text = g_strconcat(text, "*", NULL);
        dt_free(text);
        text = n_text;
      }
    }
    else if(item == DT_COLLECTION_PROP_TAG || item == DT_COLLECTION_PROP_GEOTAGGING)
    {
      if(gtk_tree_model_iter_has_child(model, &iter))
        text = _decorate_hierarchy(text, event);
      else if(item == DT_COLLECTION_PROP_TAG && active_rule == d->rule && g_strcmp0(text, _("not tagged")))
        order_request = _adopt_tag_order(text, &order);
    }
    else
      _combo_set_active_collection(active_rule->combo, item);
  }
  g_list_free_full(sels, (GDestroyNotify)gtk_tree_path_free);

  g_signal_handlers_block_matched(active_rule->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  gtk_entry_set_text(GTK_ENTRY(active_rule->text), text);
  gtk_editable_set_position(GTK_EDITABLE(active_rule->text), -1);
  g_signal_handlers_unblock_matched(active_rule->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  dt_free(text);

  // properties whose value list is unaffected by the new selection only need the conf written;
  // the others need the value list refreshed to reflect the new search string.
  if(item == DT_COLLECTION_PROP_TAG || item == DT_COLLECTION_PROP_FILMROLL || item == DT_COLLECTION_PROP_DAY
     || is_time_property(item) || item == DT_COLLECTION_PROP_COLORLABEL || item == DT_COLLECTION_PROP_GEOTAGGING
     || item == DT_COLLECTION_PROP_HISTORY || item == DT_COLLECTION_PROP_LOCAL_COPY
     || item == DT_COLLECTION_PROP_GROUPING)
    set_properties(active_rule);
  else
    update_view(active_rule);

  if(order_request) DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_IMAGES_ORDER_CHANGE, order);
  _commit_quiet();
  dt_gui_refocus_center();
  dt_control_queue_redraw_center();
}

// =====================================================================================
// Section 6 — management & admin actions (extensible right-click framework)
// =====================================================================================

static dt_lib_module_t *_self()
{
  return darktable.view_manager->proxy.module_collect.module;
}

static void _force_refresh(dt_lib_collect_t *d)
{
  d->view_rule = -1;
  _lib_collect_gui_update(_self());
}

// one selected value row
typedef struct collect_row_t
{
  gchar *path; // DT_LIB_COLLECT_COL_PATH (folder path / tag path)
  gint id;     // DT_LIB_COLLECT_COL_ID   (film_roll id / tag id)
} collect_row_t;

static void _free_row(gpointer p)
{
  collect_row_t *r = (collect_row_t *)p;
  dt_free(r->path);
  dt_free(r);
}

static GList *_selected_rows(dt_lib_collect_t *d)
{
  GtkTreeModel *model = NULL;
  GList *paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(d->view), &model);
  GList *out = NULL;
  for(GList *l = paths; l; l = g_list_next(l))
  {
    GtkTreeIter it;
    if(gtk_tree_model_get_iter(model, &it, (GtkTreePath *)l->data))
    {
      collect_row_t *r = g_malloc0(sizeof(collect_row_t));
      gtk_tree_model_get(model, &it, DT_LIB_COLLECT_COL_PATH, &r->path, DT_LIB_COLLECT_COL_ID, &r->id, -1);
      out = g_list_prepend(out, r);
    }
  }
  g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free);
  return g_list_reverse(out);
}

// Map selected rows to the de-duplicated set of matching image ids, reusing the SQL engine.
// Foundation for any bulk operation (export, pre-render thumbnails, ...). Caller g_list_free.
static GList *_rows_to_imgids(int property, GList *rows, gboolean recursive)
{
  GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
  GList *out = NULL;
  for(GList *l = rows; l; l = g_list_next(l))
  {
    collect_row_t *r = (collect_row_t *)l->data;
    gchar *text;
    if(item_is_folder(property))
      text = recursive ? g_strconcat(r->path, "*", NULL) : g_strdup(r->path);
    else
      text = g_strdup(r->path);
    // folder rows always query through FOLDERS (supports the recursive '*' suffix)
    const int prop = item_is_folder(property) ? DT_COLLECTION_PROP_FOLDERS : property;
    GList *ids = dt_collection_get_images_for_rule(prop, text);
    dt_free(text);
    for(GList *i = ids; i; i = g_list_next(i))
      if(!g_hash_table_contains(seen, i->data))
      {
        g_hash_table_add(seen, i->data);
        out = g_list_prepend(out, i->data);
      }
    g_list_free(ids);
  }
  g_hash_table_destroy(seen);
  return g_list_reverse(out);
}

// ---- small dialog helpers ----
static gboolean _confirm(const char *title, const char *message)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
                                             GTK_BUTTONS_YES_NO, "%s", message);
  gtk_window_set_title(GTK_WINDOW(dialog), title);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  const gint res = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  return res == GTK_RESPONSE_YES;
}

static gchar *_ask_text(const char *title, const char *initial)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkWidget *dialog
      = gtk_dialog_new_with_buttons(title, GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT, _("_cancel"),
                                    GTK_RESPONSE_CANCEL, _("_ok"), GTK_RESPONSE_ACCEPT, NULL);
  GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
  if(initial) gtk_entry_set_text(GTK_ENTRY(entry), initial);
  gtk_box_pack_start(GTK_BOX(area), entry, TRUE, TRUE, 0);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
  g_signal_connect(dialog, "key-press-event", G_CALLBACK(dt_handle_dialog_enter), NULL);
  gtk_widget_show_all(dialog);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gchar *result = NULL;
  if(gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
  {
    const gchar *t = gtk_entry_get_text(GTK_ENTRY(entry));
    if(t && *t) result = g_strdup(t);
  }
  gtk_widget_destroy(dialog);
  return result;
}

// ---- actions ----
static void _act_folders_remove(dt_lib_collect_t *d, GList *rows)
{
  // Select the images of exactly the chosen folders (non-recursive: we must not silently pull
  // in a whole parent subtree), then hand them to dt_control_remove_images(), which already
  // prompts "remove from library vs trash files".
  GList *imgids = _rows_to_imgids(DT_COLLECTION_PROP_FOLDERS, rows, FALSE);
  if(!imgids) return;
  dt_selection_clear(darktable.selection);
  dt_selection_select_list(darktable.selection, imgids);
  g_list_free(imgids);
  if(dt_control_remove_images()) _force_refresh(d);
}

static void _act_folders_relocate(dt_lib_collect_t *d, GList *rows)
{
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  const int n = g_list_length(rows);
  const gboolean single = (n == 1);
  collect_row_t *first = (collect_row_t *)rows->data;

  GtkFileChooserNative *fc = gtk_file_chooser_native_new(
      single ? _("select the new location of this folder") : _("select the new parent folder"), GTK_WINDOW(win),
      GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, _("_open"), _("_cancel"));
  if(single && first->path) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(fc), first->path);

  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(fc)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(fc));
    gchar *chosen = g_filename_from_uri(uri, NULL, NULL);
    dt_free(uri);
    if(chosen)
    {
      for(GList *l = rows; l; l = g_list_next(l))
      {
        collect_row_t *r = (collect_row_t *)l->data;
        if(IS_NULL_PTR(r->path)) continue;
        if(single)
          dt_film_relocate(r->path, chosen);
        else
        {
          gchar *base = g_path_get_basename(r->path);
          gchar *dest = g_build_filename(chosen, base, NULL);
          dt_film_relocate(r->path, dest);
          dt_free(base);
          dt_free(dest);
        }
      }
      dt_film_set_folder_status();
      dt_collection_memory_update();
      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
      _force_refresh(d);
      dt_free(chosen);
    }
    else
      dt_control_log(_("problem selecting new path for the folder"));
  }
  g_object_unref(fc);
}

static void _act_tags_remove(dt_lib_collect_t *d, GList *rows)
{
  const int n = g_list_length(rows);
  gchar *msg = g_strdup_printf(ngettext("Delete %d tag and detach it from all images?",
                                        "Delete %d tags and detach them from all images?", n),
                               n);
  const gboolean ok = _confirm(_("delete tags"), msg);
  dt_free(msg);
  if(!ok) return;

  for(GList *l = rows; l; l = g_list_next(l))
  {
    collect_row_t *r = (collect_row_t *)l->data;
    // tree rows don't carry the real tag id (the enumeration uses a placeholder), so resolve
    // it from the full tag path
    const guint tagid = IS_NULL_PTR(r->path) ? 0 : dt_tag_get_tag_id_by_name(r->path);
    if(tagid) dt_tag_remove(tagid, TRUE);
  }
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  _force_refresh(d);
}

static void _act_tag_rename(dt_lib_collect_t *d, GList *rows)
{
  collect_row_t *r = (collect_row_t *)rows->data;
  if(IS_NULL_PTR(r->path)) return;
  const guint tagid = dt_tag_get_tag_id_by_name(r->path);
  if(!tagid) return;
  gchar *newname = _ask_text(_("rename tag"), r->path);
  if(newname)
  {
    dt_tag_rename(tagid, newname);
    dt_free(newname);
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
    _force_refresh(d);
  }
}

// ---- pre-render thumbnails of the matching image set (background job) ----
typedef struct collect_prerender_t
{
  GList *imgids;            // owned: imgids to render
  dt_mipmap_size_t max_size;
} collect_prerender_t;

static void _prerender_free(void *p)
{
  collect_prerender_t *pr = (collect_prerender_t *)p;
  g_list_free(pr->imgids);
  g_free(pr);
}

// Fill the on-disk mipmap cache for every imgid, largest size first (smaller sizes are then
// downscaled from it rather than recomputed). Mirrors the "preload" job in gui/actions/run.c,
// but works on an explicit imgid list so it never touches the user's selection.
static int32_t _prerender_job(dt_job_t *job)
{
  collect_prerender_t *p = (collect_prerender_t *)dt_control_job_get_params(job);
  const dt_mipmap_size_t max = p->max_size;
  const int n = g_list_length(p->imgids);
  const float total = (n > 0) ? (float)(n * (max + 1)) : 1.0f;
  int done = 0;

  for(GList *l = p->imgids; l && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    for(int k = max; k >= DT_MIPMAP_0 && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED; k--)
    {
      char filename[PATH_MAX] = { 0 };
      dt_mipmap_get_cache_filename(filename, darktable.mipmap_cache, k, imgid);
      if(!dt_util_test_image_file(filename)) // skip thumbnails already on disc
      {
        dt_mipmap_buffer_t buf;
        dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, k, DT_MIPMAP_BLOCKING, 'r');
        dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      }
      dt_control_job_set_progress(job, (float)(++done) / total);
    }
    dt_mimap_cache_evict(darktable.mipmap_cache, imgid); // flush to disc, free RAM
  }
  return 0;
}

static void _act_prerender(dt_lib_collect_t *d, GList *rows)
{
  // recursive for folders so a parent folder renders its whole subtree
  GList *imgids = _rows_to_imgids(d->view_rule, rows, TRUE);
  if(!imgids) return;
  collect_prerender_t *p = g_malloc0(sizeof(collect_prerender_t));
  p->imgids = imgids; // takes ownership
  p->max_size = DT_MIPMAP_2;
  dt_job_t *job = dt_control_job_create(&_prerender_job, "prerender collection thumbnails");
  dt_control_job_set_params(job, p, _prerender_free);
  dt_control_job_add_progress(job, _("pre-rendering thumbnails"), TRUE);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, job);
}

// ---- action table: add a bulk operation by adding a row here ----
static gboolean _en_folders(int property, int n)
{
  return item_is_folder(property);
}
static gboolean _en_tags(int property, int n)
{
  return item_is_tag(property);
}
static gboolean _en_tag_single(int property, int n)
{
  return item_is_tag(property) && n == 1;
}
static gboolean _en_any(int property, int n)
{
  return item_is_folder(property) || item_is_tag(property);
}

typedef struct collect_action_t
{
  const char *label;
  gboolean multi; // allow more than one selected row
  gboolean (*enabled)(int property, int n);
  void (*run)(dt_lib_collect_t *d, GList *rows);
} collect_action_t;

static const collect_action_t ACTIONS[] = {
  { N_("remove from library..."), TRUE, _en_folders, _act_folders_remove },
  { N_("relocate..."), TRUE, _en_folders, _act_folders_relocate },
  { N_("delete tag(s)..."), TRUE, _en_tags, _act_tags_remove },
  { N_("rename tag..."), FALSE, _en_tag_single, _act_tag_rename },
  { N_("pre-render thumbnails"), TRUE, _en_any, _act_prerender },
};

static void _action_activate(GtkMenuItem *mi, dt_lib_collect_t *d)
{
  const collect_action_t *act = g_object_get_data(G_OBJECT(mi), "collect-action");
  GList *rows = _selected_rows(d);
  if(rows) act->run(d, rows);
  g_list_free_full(rows, _free_row);
}

static void _show_context_menu(dt_lib_collect_t *d, GdkEventButton *event)
{
  const int property = d->view_rule;
  const int n = gtk_tree_selection_count_selected_rows(gtk_tree_view_get_selection(d->view));
  if(n < 1) return;

  GtkWidget *menu = gtk_menu_new();
  int shown = 0;
  for(size_t i = 0; i < G_N_ELEMENTS(ACTIONS); i++)
  {
    const collect_action_t *act = &ACTIONS[i];
    if(!act->enabled(property, n)) continue;
    if(!act->multi && n > 1) continue;
    GtkWidget *mi = gtk_menu_item_new_with_label(_(act->label));
    g_object_set_data(G_OBJECT(mi), "collect-action", (gpointer)act);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(_action_activate), d);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    shown++;
  }
  if(shown)
  {
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
  }
  else
    gtk_widget_destroy(menu);
}

// =====================================================================================
// Section 7 — view & widget events
// =====================================================================================

// Drag & drop target: images dragged from the lighttable thumbtable (DND_TARGET_IMGID carries an
// array of uint32_t imgids). Dropping on a folder row physically moves the files into it; dropping
// on a tag row attaches the tag.
static gboolean _drop_move_to_folder(dt_lib_collect_t *d, const char *folder, GList *imgs)
{
  if(IS_NULL_PTR(folder) || !*folder || IS_NULL_PTR(imgs)) return FALSE;
  const int n = g_list_length(imgs);
  gchar *msg = g_strdup_printf(ngettext("Physically move %d image to\n%s ?\n\nFiles are moved on disk.",
                                        "Physically move %d images to\n%s ?\n\nFiles are moved on disk.", n),
                               n, folder);
  const gboolean ok = _confirm(_("move images"), msg);
  g_free(msg);
  if(!ok) return FALSE;

  dt_film_t film;
  dt_film_init(&film);
  dt_film_new(&film, folder); // create-or-fetch the film roll for that folder
  const int32_t filmid = film.id;
  dt_film_cleanup(&film);
  if(filmid <= 0)
  {
    dt_control_log(_("could not access the destination folder"));
    return FALSE;
  }

  int moved = 0;
  for(GList *l = imgs; l; l = g_list_next(l))
    if(dt_image_move(GPOINTER_TO_INT(l->data), filmid) != -1) moved++;

  if(moved)
  {
    dt_collection_memory_update();
    DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
    _force_refresh(d);
    dt_control_queue_redraw_center();
  }
  return moved > 0;
}

static gboolean _drop_attach_tag(dt_lib_collect_t *d, const char *tagpath, GList *imgs)
{
  // tree rows carry a placeholder id, so resolve the real tag id from the full path
  const guint tagid = (IS_NULL_PTR(tagpath) || !*tagpath) ? 0 : dt_tag_get_tag_id_by_name(tagpath);
  if(!tagid) return FALSE;
  dt_tag_attach_images(tagid, imgs, TRUE);
  dt_image_synch_xmp(-1);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  _force_refresh(d);
  return TRUE;
}

// Perform the drop of `sel` (a uint32_t imgid array) onto the row under (x, y). Returns success.
static gboolean _do_drop(dt_lib_collect_t *d, GtkTreeView *tree, gint x, gint y, GtkSelectionData *sel)
{
  const gboolean to_tag = item_is_tag(d->view_rule);
  if(!(to_tag || item_is_folder(d->view_rule))) return FALSE;

  const int imgs_nb = gtk_selection_data_get_length(sel) / (int)sizeof(uint32_t);
  if(imgs_nb <= 0) return FALSE;

  GtkTreePath *path = NULL;
  if(!gtk_tree_view_get_path_at_pos(tree, x, y, &path, NULL, NULL, NULL)) return FALSE;

  GtkTreeModel *model = gtk_tree_view_get_model(tree);
  GtkTreeIter iter;
  gboolean ok = FALSE;
  if(gtk_tree_model_get_iter(model, &iter, path))
  {
    const uint32_t *imgt = (const uint32_t *)gtk_selection_data_get_data(sel);
    GList *imgs = NULL;
    for(int i = 0; i < imgs_nb; i++) imgs = g_list_prepend(imgs, GINT_TO_POINTER((int)imgt[i]));

    gchar *rowpath = NULL;
    gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &rowpath, -1);
    ok = to_tag ? _drop_attach_tag(d, rowpath, imgs) : _drop_move_to_folder(d, rowpath, imgs);
    dt_free(rowpath);
    g_list_free(imgs);
  }
  gtk_tree_path_free(path);
  return ok;
}

static void _view_drag_data_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                     GtkSelectionData *selection_data, guint target_type, guint time,
                                     dt_lib_collect_t *d)
{
  GtkTreeView *tree = GTK_TREE_VIEW(widget);
  g_signal_stop_emission_by_name(tree, "drag-data-received"); // bypass GtkTreeView's own DnD
  const gboolean success = (target_type == DND_TARGET_IMGID && !IS_NULL_PTR(selection_data))
                           && _do_drop(d, tree, x, y, selection_data);
  gtk_drag_finish(context, success, FALSE, time);
}

static gboolean _view_button_pressed(GtkWidget *treeview, GdkEventButton *event, dt_lib_collect_t *d)
{
  // We only special-case right-click to raise the management menu; left clicks / expander
  // toggles use GtkTreeView's default handling, and activation goes through "row-activated".
  if(event->button != 3 || event->type != GDK_BUTTON_PRESS) return FALSE;
  if(!(item_is_folder(d->view_rule) || item_is_tag(d->view_rule))) return FALSE;

  GtkTreeView *view = GTK_TREE_VIEW(treeview);
  GtkTreeSelection *sel = gtk_tree_view_get_selection(view);

  // Right-clicking a row outside the current selection re-selects just that row; right-clicking
  // within an existing multi-selection keeps it so the batch actions apply to all of it.
  GtkTreePath *path = NULL;
  if(gtk_tree_view_get_path_at_pos(view, (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL) && path)
  {
    if(!gtk_tree_selection_path_is_selected(sel, path))
    {
      gtk_tree_selection_unselect_all(sel);
      gtk_tree_selection_select_path(sel, path);
    }
    gtk_tree_path_free(path);
  }

  // Show the menu whenever something is selected, even if the click missed a precise cell.
  if(gtk_tree_selection_count_selected_rows(sel) < 1) return FALSE;
  _show_context_menu(d, event);
  return TRUE;
}

static gboolean _view_popup_menu(GtkWidget *treeview, dt_lib_collect_t *d)
{
  if(!(item_is_folder(d->view_rule) || item_is_tag(d->view_rule))) return FALSE;
  _show_context_menu(d, NULL);
  return TRUE;
}

static void _view_row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, dt_lib_collect_t *d)
{
  GdkEvent *ev = gtk_get_current_event(); // carries ctrl/shift state for tag hierarchy clicks
  // only a button event has ->state at the layout row_activated() expects; for key activation
  // (Enter) pass NULL so we fall back to the default (plain-click) behaviour.
  GdkEventButton *be
      = (ev && (ev->type == GDK_BUTTON_PRESS || ev->type == GDK_2BUTTON_PRESS || ev->type == GDK_BUTTON_RELEASE))
            ? (GdkEventButton *)ev
            : NULL;
  row_activated(view, path, be, d);
  if(ev) gdk_event_free(ev);
}

static void _view_row_expanded(GtkTreeView *view, GtkTreeIter *iter, GtkTreePath *path, dt_lib_collect_t *d)
{
  if(d->view_rule != DT_COLLECTION_PROP_FOLDERS) return;
  gtk_tree_view_scroll_to_cell(view, path, NULL, TRUE, 0.0, 0.0);
}

// Document the search syntax of the active property, on both the entry and its combo. Imported
// from upstream so the wildcards / operators / ranges stay discoverable.
static void _set_tooltip(dt_lib_collect_rule_t *dr)
{
  const int property = _combo_get_active_collection(dr->combo);

  if(property == DT_COLLECTION_PROP_APERTURE || property == DT_COLLECTION_PROP_FOCAL_LENGTH
     || property == DT_COLLECTION_PROP_ISO || property == DT_COLLECTION_PROP_EXPOSURE)
    gtk_widget_set_tooltip_text(dr->text, _("use <, <=, >, >=, <>, =, [;] as operators"));
  else if(property == DT_COLLECTION_PROP_RATING)
    gtk_widget_set_tooltip_text(dr->text, _("use <, <=, >, >=, <>, =, [;] as operators\n"
                                            "star rating: 0-5\n"
                                            "rejected images: -1"));
  else if(property == DT_COLLECTION_PROP_DAY || is_time_property(property))
    gtk_widget_set_tooltip_text(dr->text,
                                _("use <, <=, >, >=, <>, =, [;] as operators\n"
                                  "type dates in the form: YYYY:MM:DD hh:mm:ss.sss (only the year is mandatory)"));
  else if(property == DT_COLLECTION_PROP_FILENAME)
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(dr->text, _("use `%' as wildcard and `,' to separate values"));
  else if(property == DT_COLLECTION_PROP_TAG)
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(dr->text, _("use `%' as wildcard\n"
                                            "click to include hierarchy + sub-hierarchies (suffix `*')\n"
                                            "shift+click to include only the current hierarchy (no suffix)\n"
                                            "ctrl+click to include only sub-hierarchies (suffix `|%')"));
  else if(property == DT_COLLECTION_PROP_GEOTAGGING)
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(dr->text, _("use `%' as wildcard\n"
                                            "click to include location + sub-locations (suffix `*')\n"
                                            "shift+click to include only the current location (no suffix)\n"
                                            "ctrl+click to include only sub-locations (suffix `|%')"));
  else if(property == DT_COLLECTION_PROP_FOLDERS)
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(dr->text,
                                _("use `%' as wildcard and append `*' to match sub-folders"));
  else
    /* xgettext:no-c-format */
    gtk_widget_set_tooltip_text(dr->text, _("use `%' as wildcard"));

  gchar *tip = gtk_widget_get_tooltip_text(dr->text);
  gtk_widget_set_tooltip_text(GTK_WIDGET(dr->combo), tip);
  dt_free(tip);
}

// Show the operator combo only for properties that support comparison operators.
static void _update_op_combo(dt_lib_collect_rule_t *dr)
{
  gtk_widget_set_visible(dr->op_combo, item_is_numeric(_combo_get_active_collection(dr->combo)));
}

static void _op_changed(GtkWidget *w, dt_lib_collect_rule_t *dr)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_lib_collect_t *c = get_collect(dr);
  c->active_rule = dr->num;
  set_properties(dr);
  c->view_rule = -1;
  _commit_colllection(); // signal-driven refresh, like combo_changed
}

static void combo_changed(GtkWidget *combo, dt_lib_collect_rule_t *dr)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_lib_collect_t *c = get_collect(dr);
  const int previous = _rule_get_item(dr->num); // conf still holds the old property
  const int property = _combo_get_active_collection(dr->combo);

  c->active_rule = dr->num;
  dr->typing = FALSE;

  // Clear the search text when switching to an unrelated property; folder<->folder (List/Tree)
  // and tag<->tag keep their text since the search string is transferable.
  const gboolean transferable
      = (item_is_folder(previous) && item_is_folder(property)) || (item_is_tag(previous) && item_is_tag(property));
  if(!transferable)
  {
    g_signal_handlers_block_matched(dr->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
    gtk_entry_set_text(GTK_ENTRY(dr->text), "");
    g_signal_handlers_unblock_matched(dr->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);

    // start the new property from the default "=" operator instead of silently carrying over the
    // previous property's operator (which would prefix the now-empty value with e.g. ">")
    g_signal_handlers_block_matched(dr->op_combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _op_changed, NULL);
    gtk_combo_box_set_active(GTK_COMBO_BOX(dr->op_combo), 0);
    g_signal_handlers_unblock_matched(dr->op_combo, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _op_changed, NULL);
  }

  // On the Folders tab the property combo is the List/Tree toggle: keep the sub-folders
  // checkbox visible only for the hierarchical (Tree = FOLDERS) view, and the "sort by"
  // selector only for the flat film-roll List (the folder Tree is always path-sorted).
  if(c->folders_controls && gtk_widget_get_visible(c->folders_controls))
  {
    gtk_widget_set_visible(c->recursive_check, property == DT_COLLECTION_PROP_FOLDERS);
    gtk_widget_set_visible(c->sort_by, property == DT_COLLECTION_PROP_FILMROLL);
    gtk_widget_set_visible(c->folder_levels, property == DT_COLLECTION_PROP_FILMROLL);
  }

  _set_tooltip(dr);
  _update_op_combo(dr);
  set_properties(dr);

  // when the query carried over (e.g. List <-> Tree, or folder <-> film-roll), unfold the rebuilt
  // tree to it instead of leaving the user on a collapsed view
  dr->reveal = transferable;

  // Signal-driven refresh: committing rebuilds where_ext first, then collection_updated ->
  // _lib_collect_gui_update rebuilds the value list against the fresh constraints. (Doing the
  // rebuild ourselves before committing would use a stale where_ext and not refresh.)
  c->view_rule = -1;
  _commit_colllection();
}

static void entry_changed(GtkEntry *entry, dt_lib_collect_rule_t *dr)
{
  dr->typing = TRUE;
  dt_lib_collect_t *d = get_collect(dr);

  // keep the Folders "include sub-folders" checkbox in sync with a *, % or |% typed by hand
  if(d->recursive_check && _combo_get_active_collection(dr->combo) == DT_COLLECTION_PROP_FOLDERS)
  {
    const gchar *t = gtk_entry_get_text(GTK_ENTRY(dr->text));
    const gboolean recursive = g_str_has_suffix(t, "*") || g_str_has_suffix(t, "%");
    g_signal_handlers_block_matched(d->recursive_check, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, d);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->recursive_check), recursive);
    g_signal_handlers_unblock_matched(d->recursive_check, G_SIGNAL_MATCH_DATA, 0, 0, NULL, NULL, d);
  }
  update_view(dr);
}

static void entry_activated(GtkWidget *entry, dt_lib_collect_rule_t *dr)
{
  update_view(dr);
  dt_lib_collect_t *c = get_collect(dr);
  const int property = _combo_get_active_collection(dr->combo);

  // for flat lists, pressing enter with a single remaining match selects it
  if(!item_is_tree(property))
  {
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(c->view));
    if(gtk_tree_model_iter_n_children(model, NULL) == 1)
    {
      GtkTreeIter iter;
      if(gtk_tree_model_get_iter_first(model, &iter))
      {
        gchar *text;
        gtk_tree_model_get(model, &iter, DT_LIB_COLLECT_COL_PATH, &text, -1);
        g_signal_handlers_block_matched(dr->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
        gtk_entry_set_text(GTK_ENTRY(dr->text), text);
        gtk_editable_set_position(GTK_EDITABLE(dr->text), -1);
        g_signal_handlers_unblock_matched(dr->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
        dt_free(text);
        update_view(dr);
      }
    }
  }
  _commit_quiet();
  dr->typing = FALSE;
  dt_control_queue_redraw_center();
}

static gboolean _entry_focus_in(GtkWidget *w, GdkEventFocus *event, dt_lib_collect_rule_t *dr)
{
  dt_lib_collect_t *c = get_collect(dr);
  update_view(get_active_rule(c));
  return FALSE;
}

// ---- Folders inline controls ----
static void _recursive_toggled(GtkToggleButton *b, dt_lib_collect_t *d)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_lib_collect_rule_t *dr = get_active_rule(d);
  if(_combo_get_active_collection(dr->combo) != DT_COLLECTION_PROP_FOLDERS) return;

  gchar *t = g_strdup(gtk_entry_get_text(GTK_ENTRY(dr->text)));
  // strip any trailing recursion markers, then re-append a single '*' if recursion is wanted
  while(g_str_has_suffix(t, "*") || g_str_has_suffix(t, "%") || g_str_has_suffix(t, "|")) t[strlen(t) - 1] = '\0';
  gchar *n = gtk_toggle_button_get_active(b) ? g_strconcat(t, "*", NULL) : g_strdup(t);
  dt_free(t);

  g_signal_handlers_block_matched(dr->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  gtk_entry_set_text(GTK_ENTRY(dr->text), n);
  gtk_editable_set_position(GTK_EDITABLE(dr->text), -1);
  g_signal_handlers_unblock_matched(dr->text, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, entry_changed, NULL);
  dt_free(n);

  set_properties(dr);
  _commit_quiet();
}

static void _sort_dir_toggled(GtkToggleButton *b, dt_lib_collect_t *d)
{
  const gboolean desc = gtk_toggle_button_get_active(b);
  dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(b), dtgtk_cairo_paint_sortby,
                               desc ? CPF_DIRECTION_DOWN : CPF_DIRECTION_UP, NULL);
  gtk_widget_queue_draw(GTK_WIDGET(b));
  if(dt_gui_widgets_suppressed()) return;
  dt_conf_set_bool("plugins/collect/descending", desc);
  _force_refresh(d);
}

static void _sort_by_changed(GtkWidget *combo, dt_lib_collect_t *d)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_conf_set_string("plugins/collect/filmroll_sort", dt_bauhaus_combobox_get(combo) == 0 ? "folder" : "id");
  _force_refresh(d);
}

// Surfaced settings that used to live in the hidden preferences popup (TODO).
static void _folder_levels_changed(GtkWidget *spin, dt_lib_collect_t *d)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_conf_set_int("show_folder_levels", (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(spin)));
  _force_refresh(d);
}

static void _no_uncategorized_toggled(GtkToggleButton *b, dt_lib_collect_t *d)
{
  if(dt_gui_widgets_suppressed()) return;
  dt_conf_set_bool("plugins/lighttable/tagging/no_uncategorized", gtk_toggle_button_get_active(b));
  _force_refresh(d);
}

// ---- Queries raw-SQL escape ----
static void _raw_toggled(GtkToggleButton *b, dt_lib_collect_t *d);
static void _raw_entry_activated(GtkWidget *entry, dt_lib_collect_t *d)
{
  _rules_set_count(1);
  _rule_set_item(0, DT_COLLECTION_PROP_QUERY);
  _rule_set_mode(0, DT_LIB_COLLECT_MODE_AND);
  _rule_set_string(0, gtk_entry_get_text(GTK_ENTRY(entry)));
  d->active_rule = 0;
  _commit_colllection();
}

// ---- Queries rule +/- management ----
static void menuitem_mode(GtkMenuItem *menuitem, dt_lib_collect_rule_t *dr)
{
  const int active = _rules_count();
  if(active < MAX_RULES)
  {
    const dt_lib_collect_mode_t mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuitem), "menuitem_mode"));
    _rule_set_mode(active, mode);
    _rule_set_string(active, "");
    _rule_set_item(active, DT_COLLECTION_PROP_FILMROLL);
    _rules_set_count(active + 1);
    dt_lib_collect_t *c = get_collect(dr);
    c->active_rule = active;
    c->view_rule = -1;
  }
  _commit_colllection();
}

static void menuitem_mode_change(GtkMenuItem *menuitem, dt_lib_collect_rule_t *dr)
{
  const int num = dr->num + 1;
  if(num < MAX_RULES && num > 0)
    _rule_set_mode(num, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuitem), "menuitem_mode")));
  dt_lib_collect_t *c = get_collect(dr);
  c->view_rule = -1;
  _commit_colllection();
}

static void menuitem_clear(GtkMenuItem *menuitem, dt_lib_collect_rule_t *dr)
{
  const int active = _rules_count();
  dt_lib_collect_t *c = get_collect(dr);
  if(active > 1)
  {
    _rules_set_count(active - 1);
    if(c->active_rule >= active - 1) c->active_rule = active - 2;
  }
  else
  {
    _rule_set_mode(0, DT_LIB_COLLECT_MODE_AND);
    _rule_set_item(0, DT_COLLECTION_PROP_FILMROLL);
    _rule_set_string(0, "");
    dr->typing = FALSE;
  }
  // shift the rules below the removed one up by one
  for(int i = dr->num; i < MAX_RULES - 1; i++)
  {
    gchar *string = _rule_get_string(i + 1);
    if(string)
    {
      _rule_set_mode(i, _rule_get_mode(i + 1));
      _rule_set_item(i, _rule_get_item(i + 1));
      _rule_set_string(i, string);
      dt_free(string);
    }
  }
  c->view_rule = -1;
  _commit_colllection();
}

static gboolean popup_button_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_collect_rule_t *dr)
{
  if(event->button != 1) return FALSE;

  GtkWidget *menu = gtk_menu_new();
  GtkWidget *mi;
  const int active = _rules_count();

  mi = gtk_menu_item_new_with_label(_("clear this rule"));
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_clear), dr);

  if(dr->num == active - 1)
  {
    mi = gtk_menu_item_new_with_label(_("narrow down search"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_AND));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode), dr);

    mi = gtk_menu_item_new_with_label(_("add more images"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_OR));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode), dr);

    mi = gtk_menu_item_new_with_label(_("exclude images"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_AND_NOT));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode), dr);
  }
  else if(dr->num < active - 1)
  {
    mi = gtk_menu_item_new_with_label(_("change to: and"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_AND));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode_change), dr);

    mi = gtk_menu_item_new_with_label(_("change to: or"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_OR));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode_change), dr);

    mi = gtk_menu_item_new_with_label(_("change to: except"));
    g_object_set_data(G_OBJECT(mi), "menuitem_mode", GINT_TO_POINTER(DT_LIB_COLLECT_MODE_AND_NOT));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    g_signal_connect(G_OBJECT(mi), "activate", G_CALLBACK(menuitem_mode_change), dr);
  }

  gtk_widget_show_all(GTK_WIDGET(menu));
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
  return TRUE;
}

// =====================================================================================
// Section 9 — tab configuration (one shared value view, reconfigured per tab)
// =====================================================================================

static void _on_tab_switch(GtkNotebook *nb, GtkWidget *page, guint page_num, dt_lib_module_t *self);

static void _combo_as_view_toggle(GtkWidget *combo) // Folders: List / Tree
{
  dt_bauhaus_combobox_clear(combo);
  dt_bauhaus_widget_set_label(combo, _("View"));
  dt_bauhaus_combobox_set_selected_text_align(combo, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT);
  dt_bauhaus_combobox_add_full(combo, _("List"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                               GUINT_TO_POINTER(DT_COLLECTION_PROP_FILMROLL + 1), NULL, TRUE);
  dt_bauhaus_combobox_add_full(combo, _("Tree"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                               GUINT_TO_POINTER(DT_COLLECTION_PROP_FOLDERS + 1), NULL, TRUE);
}

static void _combo_as_collections(GtkWidget *combo) // Collections: tags only
{
  dt_bauhaus_combobox_clear(combo);
  dt_bauhaus_widget_set_label(combo, _("View"));
  dt_bauhaus_combobox_set_selected_text_align(combo, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT);
  dt_bauhaus_combobox_add_full(combo, _("Collections"), DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT,
                               GUINT_TO_POINTER(DT_COLLECTION_PROP_TAG + 1), NULL, TRUE);
}

static void _combo_as_full(GtkWidget *combo) // Queries: every property
{
  dt_bauhaus_combobox_clear(combo);
  dt_bauhaus_widget_set_label(combo, NULL);
  dt_bauhaus_combobox_set_selected_text_align(combo, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT);
  _populate_collect_combo(combo);
}

static void _set_rule_button(dt_lib_collect_rule_t *dr, gboolean last, gboolean active)
{
  if(last)
  {
    gtk_button_set_label(GTK_BUTTON(dr->button), "-");
    gtk_widget_set_tooltip_text(GTK_WIDGET(dr->button), _("clear this rule"));
  }
  else if(active)
  {
    gtk_button_set_label(GTK_BUTTON(dr->button), "+");
    gtk_widget_set_tooltip_text(GTK_WIDGET(dr->button), _("clear this rule or add new rules"));
  }
  else
  {
    const int mode = _rule_get_mode(dr->num + 1);
    gtk_button_set_label(GTK_BUTTON(dr->button), mode == DT_LIB_COLLECT_MODE_AND  ? _("AND")
                                                 : mode == DT_LIB_COLLECT_MODE_OR ? _("OR")
                                                                                  : _("AND NOT"));
    gtk_widget_set_tooltip_text(GTK_WIDGET(dr->button), _("clear this rule"));
  }
}

static void _hide_all_widgets(dt_lib_collect_t *d)
{
  for(int i = 0; i < MAX_RULES; i++)
  {
    gtk_widget_set_no_show_all(d->rule[i].hbox, TRUE);
    gtk_widget_hide(d->rule[i].hbox);
  }
  gtk_widget_set_no_show_all(d->folders_controls, TRUE);
  gtk_widget_hide(d->folders_controls);
  gtk_widget_set_no_show_all(d->collections_controls, TRUE);
  gtk_widget_hide(d->collections_controls);
  gtk_widget_set_no_show_all(d->raw_box, TRUE);
  gtk_widget_hide(d->raw_box);
  gtk_widget_set_no_show_all(GTK_WIDGET(d->view), FALSE);
  gtk_widget_show(GTK_WIDGET(d->view));
}

static void _configure_tab(dt_lib_collect_t *d, dt_collect_tab_t tab)
{
  dt_gui_freeze_begin();
  _hide_all_widgets(d);

  if(tab == TAB_FOLDERS)
  {
    _rules_set_count(1);
    int item = _rule_get_item(0);
    if(!item_is_folder(item))
    {
      _rule_set_string(0, ""); // coming from a different property family
      item = DT_COLLECTION_PROP_FOLDERS;
      _rule_set_item(0, item);
    }
    _combo_as_view_toggle(d->rule[0].combo);
    _combo_set_active_collection(d->rule[0].combo, item);
    get_properties(&d->rule[0]);
    gtk_widget_set_no_show_all(d->rule[0].hbox, FALSE);
    gtk_widget_show_all(d->rule[0].hbox);
    gtk_widget_show(d->rule[0].combo);
    gtk_widget_hide(d->rule[0].button); // adding rules only makes sense on the Queries tab
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->rule[0].text), _("Search a folder..."));
    _set_tooltip(&d->rule[0]);
    _update_op_combo(&d->rule[0]);

    gtk_widget_set_no_show_all(d->folders_controls, FALSE);
    gtk_widget_show_all(d->folders_controls);
    gtk_widget_set_visible(d->recursive_check, item == DT_COLLECTION_PROP_FOLDERS);
    const gboolean desc = dt_conf_get_bool("plugins/collect/descending");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->sort_dir), desc);
    dtgtk_togglebutton_set_paint(DTGTK_TOGGLEBUTTON(d->sort_dir), dtgtk_cairo_paint_sortby,
                                 desc ? CPF_DIRECTION_DOWN : CPF_DIRECTION_UP, NULL);
    dt_bauhaus_combobox_set(
        d->sort_by, g_strcmp0(dt_conf_get_string_const("plugins/collect/filmroll_sort"), "id") == 0 ? 1 : 0);
    // "sort by name/id" and "folder levels" only affect the flat film-roll List; the folder
    // Tree is always path-sorted and shows full paths, so hide them there for consistency (TODO).
    gtk_widget_set_visible(d->sort_by, item == DT_COLLECTION_PROP_FILMROLL);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(d->folder_levels),
                              CLAMP(dt_conf_get_int("show_folder_levels"), 1, 5));
    gtk_widget_set_visible(d->folder_levels, item == DT_COLLECTION_PROP_FILMROLL);
    const gchar *t = gtk_entry_get_text(GTK_ENTRY(d->rule[0].text));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->recursive_check),
                                 g_str_has_suffix(t, "*") || g_str_has_suffix(t, "%"));
    d->active_rule = 0;
  }
  else if(tab == TAB_COLLECTIONS)
  {
    _rules_set_count(1);
    if(!item_is_tag(_rule_get_item(0))) _rule_set_string(0, "");
    _rule_set_item(0, DT_COLLECTION_PROP_TAG);
    _combo_as_collections(d->rule[0].combo);
    _combo_set_active_collection(d->rule[0].combo, DT_COLLECTION_PROP_TAG);
    get_properties(&d->rule[0]);
    gtk_widget_set_no_show_all(d->rule[0].hbox, FALSE);
    gtk_widget_show_all(d->rule[0].hbox);
    gtk_widget_hide(d->rule[0].combo);  // single option, no need to show it
    gtk_widget_hide(d->rule[0].button); // adding rules only makes sense on the Queries tab
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->rule[0].text), _("Search a collection..."));
    _set_tooltip(&d->rule[0]);
    _update_op_combo(&d->rule[0]);

    gtk_widget_set_no_show_all(d->collections_controls, FALSE);
    gtk_widget_show_all(d->collections_controls);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->no_uncategorized),
                                 dt_conf_get_bool("plugins/lighttable/tagging/no_uncategorized"));
    d->active_rule = 0;
  }
  else // TAB_QUERIES
  {
    get_number_of_rules(d);
    const gboolean raw = (_rule_get_item(0) == DT_COLLECTION_PROP_QUERY);
    gtk_widget_set_no_show_all(d->raw_box, FALSE);
    gtk_widget_show_all(d->raw_box);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->raw_check), raw);

    if(raw)
    {
      gchar *s = _rule_get_string(0);
      gtk_entry_set_text(GTK_ENTRY(d->raw_entry), s ? s : "");
      dt_free(s);
      gtk_widget_show(d->raw_entry);
      gtk_widget_set_no_show_all(GTK_WIDGET(d->view), TRUE);
      gtk_widget_hide(GTK_WIDGET(d->view));
      d->active_rule = 0;
    }
    else
    {
      gtk_widget_hide(d->raw_entry);
      for(int i = 0; i <= d->active_rule; i++)
      {
        _combo_as_full(d->rule[i].combo);
        get_properties(&d->rule[i]);
        gtk_widget_set_no_show_all(d->rule[i].hbox, FALSE);
        gtk_widget_show_all(d->rule[i].hbox);
        gtk_widget_show(d->rule[i].combo);
        gtk_widget_show(d->rule[i].button); // rule +/- management, Queries tab only
        _set_rule_button(&d->rule[i], i == MAX_RULES - 1, i == d->active_rule);
        gtk_entry_set_placeholder_text(GTK_ENTRY(d->rule[i].text), _("Search..."));
        _set_tooltip(&d->rule[i]);
        _update_op_combo(&d->rule[i]);
      }
    }
  }
  dt_gui_freeze_end();
}

static void _raw_toggled(GtkToggleButton *b, dt_lib_collect_t *d)
{
  if(dt_gui_widgets_suppressed()) return;
  if(gtk_toggle_button_get_active(b))
  {
    _rules_set_count(1);
    _rule_set_item(0, DT_COLLECTION_PROP_QUERY);
    _rule_set_string(0, gtk_entry_get_text(GTK_ENTRY(d->raw_entry)));
    d->active_rule = 0;
  }
  else if(_rule_get_item(0) == DT_COLLECTION_PROP_QUERY)
  {
    _rule_set_item(0, DT_COLLECTION_PROP_FILMROLL);
    _rule_set_string(0, "");
  }
  d->view_rule = -1;
  _configure_tab(d, TAB_QUERIES);
  if(!gtk_toggle_button_get_active(b)) update_view(get_active_rule(d)); // raw mode has no value list
  _commit_quiet();
}

static void _on_tab_switch(GtkNotebook *nb, GtkWidget *page, guint page_num, dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  dt_conf_set_int("plugins/lighttable/collect/tab", page_num);
  if(page_num == TAB_FOLDERS || page_num == TAB_COLLECTIONS) _rules_set_count(1);

  const gboolean raw = (page_num == TAB_QUERIES && _rule_get_item(0) == DT_COLLECTION_PROP_QUERY);
  _configure_tab(d, page_num);
  d->view_rule = -1;
  if(!raw)
  {
    // _configure_tab kept the query whenever the destination field is compatible; unfold the tree
    // to it (a no-op when the field was incompatible and the entry was cleared)
    get_active_rule(d)->reveal = TRUE;
    update_view(get_active_rule(d));
  }
  // raw SQL mode has no value list
  // NB: switching tabs only reconfigures the GUI and rebuilds the value list; it must NOT
  // re-run the collection query (that rebuilds the whole lighttable and was the slow path).
  // The collection updates when the user actually clicks a value or edits a rule.
}

static void _lib_collect_gui_update(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  if(d->view_rule != -1) return; // nothing changed since the last build

  dt_gui_freeze_begin();
  get_number_of_rules(d);

  dt_collect_tab_t tab;
  if(item_is_folder(_rule_get_item(0)) && d->nb_rules == 1)
    tab = TAB_FOLDERS;
  else if(item_is_tag(_rule_get_item(0)) && d->nb_rules == 1)
    tab = TAB_COLLECTIONS;
  else
    tab = TAB_QUERIES;
  dt_conf_set_int("plugins/lighttable/collect/tab", tab);

  g_signal_handlers_block_matched(d->notebook, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _on_tab_switch, NULL);
  gtk_notebook_set_current_page(GTK_NOTEBOOK(d->notebook), tab);
  g_signal_handlers_unblock_matched(d->notebook, G_SIGNAL_MATCH_FUNC, 0, 0, NULL, _on_tab_switch, NULL);

  const gboolean raw = (tab == TAB_QUERIES && _rule_get_item(0) == DT_COLLECTION_PROP_QUERY);
  _configure_tab(d, tab);
  if(!raw) update_view(get_active_rule(d)); // raw SQL mode has no value list
  dt_gui_freeze_end();
}

// =====================================================================================
// Section 10 — signals
// =====================================================================================

static void collection_updated(gpointer instance, dt_collection_change_t query_change,
                               dt_collection_properties_t changed_property, gpointer imgs, int next, gpointer self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)((dt_lib_module_t *)self)->data;
  d->view_rule = -1;
  get_active_rule(d)->typing = FALSE;

  // On a pure reload (no query change) only rebuild if a property we display actually changed.
  gboolean refresh = TRUE;
  if(query_change == DT_COLLECTION_CHANGE_RELOAD && changed_property != DT_COLLECTION_PROP_UNDEF)
  {
    refresh = FALSE;
    for(int i = 0; i <= d->active_rule; i++)
      if(_combo_get_active_collection(d->rule[i].combo) == changed_property)
      {
        refresh = TRUE;
        break;
      }
  }
  if(refresh) _lib_collect_gui_update(self);
}

static void filmrolls_updated(gpointer instance, gpointer self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)((dt_lib_module_t *)self)->data;
  d->view_rule = -1;
  _lib_collect_gui_update(self);
}

static void filmrolls_removed(gpointer instance, gpointer self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)((dt_lib_module_t *)self)->data;
  d->view_rule = -1;
  get_active_rule(d)->typing = FALSE;
  _lib_collect_gui_update(self);
}

static void preferences_changed(gpointer instance, gpointer self)
{
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF, NULL);
}

static void tag_changed(gpointer instance, gpointer self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)((dt_lib_module_t *)self)->data;
  gboolean uses_tag = FALSE;
  for(int i = 0; i < d->nb_rules; i++)
    if(_combo_get_active_collection(d->rule[i].combo) == DT_COLLECTION_PROP_TAG)
    {
      uses_tag = TRUE;
      break;
    }

  d->view_rule = -1;
  get_active_rule(d)->typing = FALSE;
  if(uses_tag)
  {
    dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_TAG, NULL);
    dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                      darktable.view_manager->proxy.module_collect.module);
  }
  _lib_collect_gui_update(self);
}

static void geotag_changed(gpointer instance, GList *imgs, const int locid, gpointer self)
{
  if(locid) return; // not our concern
  dt_lib_collect_t *d = (dt_lib_collect_t *)((dt_lib_module_t *)self)->data;
  if(_combo_get_active_collection(get_active_rule(d)->combo) == DT_COLLECTION_PROP_GEOTAGGING)
  {
    d->view_rule = -1;
    get_active_rule(d)->typing = FALSE;
    _lib_collect_gui_update(self);
    dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                    darktable.view_manager->proxy.module_collect.module);
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_GEOTAGGING,
                               NULL);
    dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(collection_updated),
                                      darktable.view_manager->proxy.module_collect.module);
  }
}

static void metadata_changed(gpointer instance, int type, gpointer self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)((dt_lib_module_t *)self)->data;
  if(type == DT_METADATA_SIGNAL_HIDDEN || type == DT_METADATA_SIGNAL_SHOWN)
  {
    d->view_rule = -1;
    _lib_collect_gui_update(self);
  }
  const int prop = _combo_get_active_collection(get_active_rule(d)->combo);
  if(type == DT_METADATA_SIGNAL_HIDDEN
     || (prop >= DT_COLLECTION_PROP_METADATA && prop < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER))
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_METADATA,
                               NULL);
}

#ifdef _WIN32
static void _mount_changed(GVolumeMonitor *volume_monitor, GMount *mount, dt_lib_module_t *self)
#else
static void _mount_changed(GUnixMountMonitor *monitor, dt_lib_module_t *self)
#endif
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;
  dt_film_set_folder_status();
  if(item_is_folder(_combo_get_active_collection(get_active_rule(d)->combo)))
  {
    d->view_rule = -1;
    _lib_collect_gui_update(self);
  }
}

// =====================================================================================
// Section 11 — preferences popup, construction & teardown
// =====================================================================================

// NB: the old hidden "preferences..." popup has been retired (TODO). Every collect setting it
// exposed now lives in the front widget: filmroll_sort / descending / folder_levels on the
// Folders tab, no_uncategorized on the Collections tab.

void gui_init(dt_lib_module_t *self)
{
  dt_lib_collect_t *d = (dt_lib_collect_t *)calloc(1, sizeof(dt_lib_collect_t));
  self->data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  d->active_rule = 0;
  d->nb_rules = 0;
  d->view_rule = -1;
  d->params = (dt_lib_collect_params_t *)malloc(sizeof(dt_lib_collect_params_t));

  // notebook: only its tab-bar is used; the content below is shared and reconfigured per tab
  d->notebook = GTK_WIDGET(dt_ui_notebook_new());
  dt_gui_add_class(d->notebook, "empty");
  dt_ui_notebook_page(GTK_NOTEBOOK(d->notebook), _("Folders"), _("Browse and manage the folders known to Ansel"));
  dt_ui_notebook_page(GTK_NOTEBOOK(d->notebook), _("Collections"), _("Browse and manage tags"));
  dt_ui_notebook_page(GTK_NOTEBOOK(d->notebook), _("Queries"), _("Build arbitrary collections"));
  gtk_widget_show_all(d->notebook);
  gtk_box_pack_start(GTK_BOX(self->widget), d->notebook, TRUE, TRUE, 0);
  gtk_notebook_set_scrollable(GTK_NOTEBOOK(d->notebook), TRUE);
  g_signal_connect(G_OBJECT(d->notebook), "switch_page", G_CALLBACK(_on_tab_switch), self);

  // one rule row per possible rule (only the relevant ones are shown per tab)
  for(int i = 0; i < MAX_RULES; i++)
  {
    d->rule[i].num = i;
    d->rule[i].typing = FALSE;
    d->rule[i].lib_collect = (void *)d;

    GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING));
    d->rule[i].hbox = GTK_WIDGET(box);
    gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(box), TRUE, TRUE, 0);
    gtk_widget_set_name(GTK_WIDGET(box), "lib-dtbutton");

    d->rule[i].combo = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
    dt_bauhaus_combobox_set_selected_text_align(d->rule[i].combo, DT_BAUHAUS_COMBOBOX_ALIGN_RIGHT);
    _populate_collect_combo(d->rule[i].combo);
    g_signal_connect(G_OBJECT(d->rule[i].combo), "value-changed", G_CALLBACK(combo_changed), d->rule + i);
    gtk_box_pack_start(box, d->rule[i].combo, FALSE, FALSE, 0);

    GtkBox *hbox = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING));
    gtk_box_pack_start(box, GTK_WIDGET(hbox), FALSE, FALSE, 0);

    // comparison-operator selector, shown only for numeric/date/rating properties
    d->rule[i].op_combo = gtk_combo_box_text_new();
    for(int o = 0; o < COLLECT_N_OPS; o++)
      gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(d->rule[i].op_combo), OP_LABELS[o]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(d->rule[i].op_combo), 0);
    gtk_widget_set_no_show_all(d->rule[i].op_combo, TRUE);
    gtk_widget_set_tooltip_text(d->rule[i].op_combo, _("comparison operator"));
    g_signal_connect(G_OBJECT(d->rule[i].op_combo), "changed", G_CALLBACK(_op_changed), d->rule + i);
    gtk_box_pack_start(hbox, d->rule[i].op_combo, FALSE, FALSE, 0);

    GtkWidget *w = gtk_search_entry_new();
    dt_accels_disconnect_on_text_input(w);
    d->rule[i].text = w;
    gtk_widget_add_events(w, GDK_FOCUS_CHANGE_MASK | GDK_KEY_PRESS_MASK);
    gtk_entry_set_placeholder_text(GTK_ENTRY(w), _("Search..."));
    g_signal_connect(G_OBJECT(w), "focus-in-event", G_CALLBACK(_entry_focus_in), d->rule + i);
    g_signal_connect(G_OBJECT(w), "changed", G_CALLBACK(entry_changed), d->rule + i);
    g_signal_connect(G_OBJECT(w), "activate", G_CALLBACK(entry_activated), d->rule + i);
    gtk_widget_set_name(GTK_WIDGET(w), "lib-collect-entry");
    gtk_box_pack_start(hbox, w, TRUE, TRUE, 0);
    gtk_entry_set_width_chars(GTK_ENTRY(w), 5);

    d->rule[i].button = gtk_button_new();
    gtk_widget_set_events(d->rule[i].button, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(G_OBJECT(d->rule[i].button), "button-press-event", G_CALLBACK(popup_button_callback),
                     d->rule + i);
    gtk_box_pack_start(hbox, d->rule[i].button, FALSE, FALSE, 0);
  }

  // Folders inline controls (sort + recursion), shown only on the Folders tab
  d->folders_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  d->recursive_check = gtk_check_button_new_with_label(_("include sub-folders"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->recursive_check), TRUE);
  g_signal_connect(G_OBJECT(d->recursive_check), "toggled", G_CALLBACK(_recursive_toggled), d);
  gtk_box_pack_start(GTK_BOX(d->folders_controls), d->recursive_check, FALSE, FALSE, 0);

  d->sort_by = dt_bauhaus_combobox_new(darktable.bauhaus, DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(d->sort_by, _("sort by"));
  dt_bauhaus_combobox_add(d->sort_by, _("name"));
  dt_bauhaus_combobox_add(d->sort_by, _("id"));
  // bauhaus widgets render at their own (short) natural height; in this horizontal row they would
  // otherwise stick to the top and sit higher than the native spin/toggle siblings, so center them
  gtk_widget_set_valign(d->sort_by, GTK_ALIGN_CENTER);
  g_signal_connect(G_OBJECT(d->sort_by), "value-changed", G_CALLBACK(_sort_by_changed), d);
  gtk_box_pack_start(GTK_BOX(d->folders_controls), d->sort_by, TRUE, TRUE, 0);

  d->sort_dir = dtgtk_togglebutton_new(dtgtk_cairo_paint_sortby, CPF_DIRECTION_UP, NULL);
  dt_gui_add_class(d->sort_dir, "dt_ignore_fg_state");
  gtk_widget_set_tooltip_text(d->sort_dir, _("toggle ascending / descending order"));
  g_signal_connect(G_OBJECT(d->sort_dir), "toggled", G_CALLBACK(_sort_dir_toggled), d);
  gtk_box_pack_start(GTK_BOX(d->folders_controls), d->sort_dir, FALSE, FALSE, 0);

  d->folder_levels = gtk_spin_button_new_with_range(1, 5, 1);
  gtk_widget_set_tooltip_text(d->folder_levels,
                              _("number of folder levels to show in film-roll names, from the right"));
  g_signal_connect(G_OBJECT(d->folder_levels), "value-changed", G_CALLBACK(_folder_levels_changed), d);
  gtk_box_pack_start(GTK_BOX(d->folders_controls), d->folder_levels, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->folders_controls, FALSE, FALSE, 0);

  // Collections inline controls, shown only on the Collections tab
  d->collections_controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  d->no_uncategorized = gtk_check_button_new_with_label(_("no 'uncategorized' group"));
  gtk_widget_set_tooltip_text(d->no_uncategorized,
                              _("do not group childless tags under an 'uncategorized' entry"));
  g_signal_connect(G_OBJECT(d->no_uncategorized), "toggled", G_CALLBACK(_no_uncategorized_toggled), d);
  gtk_box_pack_start(GTK_BOX(d->collections_controls), d->no_uncategorized, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->collections_controls, FALSE, FALSE, 0);

  // Queries raw-SQL escape, shown only on the Queries tab
  d->raw_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  d->raw_check = gtk_check_button_new_with_label(_("edit as raw SQL"));
  g_signal_connect(G_OBJECT(d->raw_check), "toggled", G_CALLBACK(_raw_toggled), d);
  gtk_box_pack_start(GTK_BOX(d->raw_box), d->raw_check, FALSE, FALSE, 0);
  d->raw_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(d->raw_entry),
                                 _("SQL WHERE expression, e.g. iso > 800 AND lens LIKE '%50mm%'"));
  dt_accels_disconnect_on_text_input(d->raw_entry);
  g_signal_connect(G_OBJECT(d->raw_entry), "activate", G_CALLBACK(_raw_entry_activated), d);
  gtk_box_pack_start(GTK_BOX(d->raw_box), d->raw_entry, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), d->raw_box, FALSE, FALSE, 0);

  // shared value view
  GtkTreeView *view = GTK_TREE_VIEW(gtk_tree_view_new());
  d->view = view;
  gtk_tree_view_set_headers_visible(view, FALSE);
  gtk_tree_view_set_activate_on_single_click(view, TRUE);
  gtk_widget_set_can_focus(GTK_WIDGET(view), TRUE);
  g_signal_connect(G_OBJECT(view), "button-press-event", G_CALLBACK(_view_button_pressed), d);
  g_signal_connect(G_OBJECT(view), "popup-menu", G_CALLBACK(_view_popup_menu), d);
  g_signal_connect(G_OBJECT(view), "row-activated", G_CALLBACK(_view_row_activated), d);
  g_signal_connect(G_OBJECT(view), "row-expanded", G_CALLBACK(_view_row_expanded), d);

  // accept images dragged from the lighttable thumbtable: drop on a folder row to move the
  // files there, on a tag row to attach the tag (handled in _view_drag_data_received)
  gtk_drag_dest_set(GTK_WIDGET(view), GTK_DEST_DEFAULT_ALL, target_list_internal, n_targets_internal,
                    GDK_ACTION_MOVE);
  g_signal_connect(G_OBJECT(view), "drag-data-received", G_CALLBACK(_view_drag_data_received), d);

  GtkTreeViewColumn *col = gtk_tree_view_column_new();
  gtk_tree_view_append_column(view, col);
  GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(col, renderer, TRUE);
  gtk_tree_view_column_set_cell_data_func(col, renderer, tree_count_show, NULL, NULL);
  gtk_tree_view_column_add_attribute(col, renderer, "weight", DT_LIB_COLLECT_COL_FONT);
  g_object_set(renderer, "strikethrough", TRUE, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, (gchar *)0);
  gtk_tree_view_column_add_attribute(col, renderer, "strikethrough-set", DT_LIB_COLLECT_COL_UNREACHABLE);

  GtkTreeModel *listmodel = GTK_TREE_MODEL(
      gtk_list_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING,
                         G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INT));
  d->listfilter = gtk_tree_model_filter_new(listmodel, NULL);
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(d->listfilter), DT_LIB_COLLECT_COL_VISIBLE);

  GtkTreeModel *treemodel = GTK_TREE_MODEL(
      gtk_tree_store_new(DT_LIB_COLLECT_NUM_COLS, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_STRING,
                         G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_UINT, G_TYPE_UINT, G_TYPE_INT));
  d->treefilter = gtk_tree_model_filter_new(treemodel, NULL);
  gtk_tree_model_filter_set_visible_column(GTK_TREE_MODEL_FILTER(d->treefilter), DT_LIB_COLLECT_COL_VISIBLE);
  g_object_unref(treemodel);

  // Static height: the collection list refreshes on selection/act-on, so a fixed user-set size keeps
  // the side-panel layout from jumping. Defaults to ~200px until the user drags the grip.
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(GTK_WIDGET(view), 200, "plugins/lighttable/collect/windowheight",
                                       DT_UI_RESIZE_STATIC),
                     TRUE, TRUE, 0);

  // proxy used by other code to force a refresh
  darktable.view_manager->proxy.module_collect.module = self;
  darktable.view_manager->proxy.module_collect.update = _lib_collect_gui_update;

  _lib_collect_gui_update(self);

#ifdef _WIN32
  d->vmonitor = g_volume_monitor_get();
  g_signal_connect(G_OBJECT(d->vmonitor), "mount-changed", G_CALLBACK(_mount_changed), self);
  g_signal_connect(G_OBJECT(d->vmonitor), "mount-added", G_CALLBACK(_mount_changed), self);
#else
  d->vmonitor = g_unix_mount_monitor_get();
  g_signal_connect(G_OBJECT(d->vmonitor), "mounts-changed", G_CALLBACK(_mount_changed), self);
#endif

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED, G_CALLBACK(collection_updated),
                                  self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED, G_CALLBACK(filmrolls_updated),
                                  self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE, G_CALLBACK(preferences_changed),
                                  self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_FILMROLLS_REMOVED, G_CALLBACK(filmrolls_removed),
                                  self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_TAG_CHANGED, G_CALLBACK(tag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_GEOTAG_CHANGED, G_CALLBACK(geotag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_METADATA_CHANGED, G_CALLBACK(metadata_changed),
                                  self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_collect_t *d = (dt_lib_collect_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(collection_updated), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(filmrolls_updated), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(preferences_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(filmrolls_removed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(tag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(geotag_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(metadata_changed), self);
  darktable.view_manager->proxy.module_collect.module = NULL;

  dt_free(d->params);
  g_object_unref(d->treefilter);
  g_object_unref(d->listfilter);
  g_object_unref(d->vmonitor);
  dt_free(self->data);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
