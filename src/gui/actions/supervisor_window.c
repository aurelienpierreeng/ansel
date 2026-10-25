/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.
    Copyright (C) 2026 Guillaume Stutin.

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

#include "gui/actions/supervisor_window.h"
#include "caches/image_cache.h"
#include "caches/mipmap_cache.h"
#include "common/opencl.h"
#include "caches/pixelpipe_cache.h"
#include "common/conf.h"
#include "develop/supervisor.h"
#include "gui/application.h"
#include "system/sys_resources.h"
#include "widgets/scroll_wrap.h"
#include "widgets/widget_style.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>   // conditional-ok: GDK_IS_WAYLAND_DISPLAY() is used only inside the same #ifdef
#endif

// Cap on how many timeline rows we keep live (older ones are trimmed). The full
// log lives in the supervisor; this only bounds GTK widget pressure.
#define TIMELINE_MAX_ROWS 4000
// How far past the cap the list may grow before it is trimmed back to it. Removing the top
// rows shifts every other one, and GTK 3 then restyles them all: about 0.45 s at 4000 rows on a
// mock with the theme, however few go. Trimmed at every poll, 24 polls of 50 events took 14.8 s;
// trimmed past this slack, 5.8 s, a 1.8 s trim once every 1000 events.
#define TIMELINE_TRIM_SLACK (TIMELINE_MAX_ROWS / 4)
#define POLL_INTERVAL_MS 300
#define MEMORY_MAX_ROWS 500   // cap items shown per cache in the memory view
#define MEMORY_REFRESH_TICKS 4 // refresh the memory view every Nth poll tick when visible
#define MEMORY_LIST_ROWS 20    // how many rows a memory list shows until the user resizes it

static struct
{
  GtkWidget *window;
  GtkWidget *notebook;
  GtkWidget *timeline_list;
  GtkWidget *timeline_scroll;
  GtkWidget *grouped_list;
  GtkWidget *mem_box;     // vbox holding the memory view content
  GHashTable *mem_lists;  // memory section key -> its list's resizable area (a reference), kept across rebuilds
  GtkWidget *search_entry; // global search entry (in the toolbar)
  GtkWidget *search_list;  // search results page
  GtkWidget *count_label;
  GtkWidget *groupby;     // GtkComboBoxText: domain / thread / op
  GHashTable *decl_map;   // hex string -> GtkListBoxRow* (the create/declaration row)
  GHashTable *group_map;  // group key -> _group_t* (incremental grouped buckets)
  uint64_t last_seq;      // highest captured seq already displayed in the timeline
  uint64_t grouped_last_seq; // highest seq already folded into the grouped view
  gchar *scroll_target;   // hash of a timeline row to scroll into view (deferred)
  guint timer_id;
  guint tick;             // poll tick counter (for throttling the memory view)
  int page_timeline, page_grouped, page_memory, page_search; // notebook page indices
} _g = { 0 };

// One grouped bucket: its detail body (where event rows are appended) and the
// header label (whose count we keep in sync).
typedef struct _group_t
{
  GtkWidget *body;
  GtkWidget *header;
  int count;
  gchar *key;
} _group_t;

static void _group_free(gpointer p)
{
  _group_t *g = (_group_t *)p;
  if(!g) return;
  g_free(g->key);
  g_free(g); // widgets are owned by the list box
}

static gchar *_hashhex(const uint64_t h)
{
  return g_strdup_printf("0x%016" G_GINT64_MODIFIER "x", h);
}

static const char *_domain_color(const char *d)
{
  if(!g_strcmp0(d, "history"))   return "#7fbfff";
  if(!g_strcmp0(d, "node"))      return "#9fe0a0";
  if(!g_strcmp0(d, "cacheline")) return "#ffd080";
  if(!g_strcmp0(d, "backbuf"))   return "#ff9a9a";
  if(!g_strcmp0(d, "widget"))    return "#d0a8ff";
  if(!g_strcmp0(d, "thumbnail")) return "#bcbcbc";
  if(!g_strcmp0(d, "mipmap"))    return "#b0c4de";
  if(!g_strcmp0(d, "image"))     return "#e8c8a0";
  if(!g_strcmp0(d, "form"))      return "#f0a0c0";
  return "#dddddd";
}

static gchar *_pretty_json(const char *compact)
{
  JsonParser *p = json_parser_new();
  gchar *out = NULL;
  if(json_parser_load_from_data(p, compact, -1, NULL))
  {
    JsonGenerator *g = json_generator_new();
    json_generator_set_root(g, json_parser_get_root(p));
    json_generator_set_pretty(g, TRUE);
    out = json_generator_to_data(g, NULL);
    g_object_unref(g);
  }
  g_object_unref(p);
  return out ? out : g_strdup(compact);
}

static void _run_search(const char *query);

// Scroll a timeline row (centred) into the visible area of its scrolled window.
static void _scroll_row_into_view(GtkWidget *row)
{
  if(!row || !_g.timeline_scroll) return;
  GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_g.timeline_scroll));
  if(!vadj) return;
  gint y = 0;
  if(!gtk_widget_translate_coordinates(row, _g.timeline_list, 0, 0, NULL, &y)) return;
  GtkAllocation alloc;
  gtk_widget_get_allocation(row, &alloc);
  const double page = gtk_adjustment_get_page_size(vadj);
  const double upper = gtk_adjustment_get_upper(vadj);
  double target = (double)y + alloc.height / 2.0 - page / 2.0; // centre the row
  target = CLAMP(target, 0.0, MAX(0.0, upper - page));
  gtk_adjustment_set_value(vadj, target);
}

// Deferred so the scroll runs after the page switch / row expansion are laid out.
static gboolean _scroll_target_idle(gpointer u)
{
  if(_g.window && _g.scroll_target)
  {
    GtkWidget *row = (GtkWidget *)g_hash_table_lookup(_g.decl_map, _g.scroll_target);
    if(row) _scroll_row_into_view(row);
  }
  g_clear_pointer(&_g.scroll_target, g_free);
  return G_SOURCE_REMOVE;
}

// Clicking a hash jumps to the declaration (create event) of that object.
static gboolean _on_link(GtkLabel *label, gchar *uri, gpointer user_data)
{
  if(!_g.decl_map) return TRUE;
  GtkWidget *row = (GtkWidget *)g_hash_table_lookup(_g.decl_map, uri);
  if(row)
  {
    gtk_notebook_set_current_page(GTK_NOTEBOOK(_g.notebook), _g.page_timeline);
    GtkWidget *toggle = (GtkWidget *)g_object_get_data(G_OBJECT(row), "toggle");
    if(toggle) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), TRUE); // expand
    gtk_list_box_select_row(GTK_LIST_BOX(_g.timeline_list), GTK_LIST_BOX_ROW(row));
    gtk_widget_grab_focus(row);
    // Defer the explicit scroll: the page just switched and the row may not be
    // laid out yet, so translate_coordinates would be stale right now.
    g_free(_g.scroll_target);
    _g.scroll_target = g_strdup(uri);
    g_idle_add(_scroll_target_idle, NULL);
  }
  return TRUE; // handled: do not try to open as an URL
}

static void _on_search_this_hash(GtkMenuItem *item, gpointer u)
{
  const char *uri = (const char *)g_object_get_data(G_OBJECT(item), "uri");
  if(uri && _g.search_entry)
  {
    gtk_entry_set_text(GTK_ENTRY(_g.search_entry), uri); // triggers search-changed → _run_search
    gtk_notebook_set_current_page(GTK_NOTEBOOK(_g.notebook), _g.page_search);
  }
}

// Right-click over a hash link offers to search for that hash.
static gboolean _on_label_button(GtkWidget *label, GdkEventButton *e, gpointer u)
{
  if(e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY) return FALSE;
  const char *uri = gtk_label_get_current_uri(GTK_LABEL(label)); // link under the pointer
  // Inside a list box (timeline/grouped/search) the pointer-tracking that sets
  // the "current link" is unreliable, so fall back to the label's single own hash.
  if(!uri || !*uri) uri = (const char *)g_object_get_data(G_OBJECT(label), "own-hash");
  if(!uri || !*uri) return FALSE;

  GtkWidget *menu = gtk_menu_new();
  GtkWidget *mi = gtk_menu_item_new_with_label(_("Search for this hash"));
  g_object_set_data_full(G_OBJECT(mi), "uri", g_strdup(uri), g_free);
  g_signal_connect(mi, "activate", G_CALLBACK(_on_search_this_hash), NULL);
  gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
  gtk_widget_show_all(menu);
  gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
  return TRUE;
}

// Wire a hash-bearing label: left-click navigates, right-click offers search.
static void _wire_hash_label(GtkWidget *label)
{
  gtk_label_set_track_visited_links(GTK_LABEL(label), FALSE);
  g_signal_connect(label, "activate-link", G_CALLBACK(_on_link), NULL);
  gtk_widget_add_events(label, GDK_BUTTON_PRESS_MASK);
  g_signal_connect(label, "button-press-event", G_CALLBACK(_on_label_button), NULL);
}

static GtkWidget *_event_body_from(const char *json, GArray *links);

static void _on_toggle(GtkToggleButton *t, gpointer u)
{
  const gboolean a = gtk_toggle_button_get_active(t);
  GtkWidget *rev = (GtkWidget *)g_object_get_data(G_OBJECT(t), "revealer");

  // Lazily build an event's detail body the first time it is expanded, so the
  // JSON is not parsed for rows the user never opens (important under streaming).
  if(a && rev && !gtk_bin_get_child(GTK_BIN(rev)))
  {
    const char *json = (const char *)g_object_get_data(G_OBJECT(t), "lazy_json");
    if(json)
    {
      GtkWidget *body = _event_body_from(json, (GArray *)g_object_get_data(G_OBJECT(t), "lazy_links"));
      gtk_container_add(GTK_CONTAINER(rev), body);
      gtk_widget_show_all(body);
    }
  }
  if(rev) gtk_revealer_set_reveal_child(GTK_REVEALER(rev), a);
  gtk_button_set_label(GTK_BUTTON(t), a ? "▾" : "▸");
}

// A collapsible block: [arrow toggle][header label] + a revealer holding `body`.
// The header is a separate label (NOT the toggle), so its hash links stay
// clickable. Returns a vbox; the toggle button is stored as data "toggle".
// The arrow is the button's text, not an icon: on Windows, every GtkImage drawn keeps a DC and a
// DIB section, and with one arrow per event row that ran Ansel out of GDI objects (see
// doc/gtk-patterns.md). A label costs none.
static GtkWidget *_collapsible(const char *header_markup, GtkWidget *body, const gboolean expanded)
{
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  GtkWidget *toggle = gtk_toggle_button_new_with_label(expanded ? "▾" : "▸");
  gtk_button_set_relief(GTK_BUTTON(toggle), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click(toggle, FALSE);

  GtkWidget *header = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(header), header_markup);
  gtk_label_set_xalign(GTK_LABEL(header), 0.0);
  _wire_hash_label(header);

  GtkWidget *revealer = gtk_revealer_new();
  gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), expanded);
  if(body) gtk_container_add(GTK_CONTAINER(revealer), body);

  g_object_set_data(G_OBJECT(toggle), "revealer", revealer);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), expanded);
  g_signal_connect(toggle, "toggled", G_CALLBACK(_on_toggle), NULL);

  gtk_box_pack_start(GTK_BOX(hbox), toggle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), header, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), revealer, FALSE, FALSE, 0);

  g_object_set_data(G_OBJECT(vbox), "toggle", toggle);
  g_object_set_data(G_OBJECT(vbox), "header", header); // so callers can update the label
  return vbox;
}

static gchar *_header_markup(const dt_sv_logged_event_t *ev)
{
  gchar *hx = _hashhex(ev->hash);
  gchar *e_op = g_markup_escape_text(ev->op, -1);
  gchar *e_dom = g_markup_escape_text(ev->domain, -1);
  gchar *e_thr = g_markup_escape_text(ev->thread, -1);
  gchar *e_mn_raw = g_markup_escape_text(ev->mnemonic[0] ? ev->mnemonic : "-", -1);
  // Raster masks are side-band cachelines, not module outputs: dim the domain
  // badge and tag the title so they read as secondary entries among plain
  // image cachelines of the same module.
  const gboolean is_raster_mask
      = !g_strcmp0(ev->domain, "cacheline") && !g_strcmp0(ev->cache_name, "raster mask");
  gchar *e_mn = is_raster_mask ? g_strdup_printf("%s - raster mask", e_mn_raw) : g_strdup(e_mn_raw);
  g_free(e_mn_raw);
  gchar *out = g_strdup_printf(
      "<tt>%9.3f</tt>  <b>%-7s</b>  <span foreground=\"%s\">%-10s</span>  <b>%-16s</b>  "
      "<a href=\"%s\"><tt>%s</tt></a>  <span size=\"small\"><i>%s</i></span>",
      ev->ts, e_op, is_raster_mask ? "#b78d45" : _domain_color(ev->domain), e_dom, e_mn, hx, hx, e_thr);
  g_free(hx);
  g_free(e_op);
  g_free(e_dom);
  g_free(e_thr);
  g_free(e_mn);
  return out;
}

// Build the detail body for an event: clickable linked hashes + full record.
static GtkWidget *_event_body_from(const char *json, GArray *links)
{
  GtkWidget *detail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(detail, 28);
  gtk_widget_set_margin_bottom(detail, 6);

  if(links && links->len)
  {
    GString *ls = g_string_new(NULL);
    for(guint i = 0; i < links->len; i++)
    {
      const dt_sv_link_t *lk = &g_array_index(links, dt_sv_link_t, i);
      gchar *lhx = _hashhex(lk->hash);
      g_string_append_printf(ls, "%s%s → <a href=\"%s\"><tt>%s</tt></a>", i ? "      " : "", lk->label,
                             lhx, lhx);
      g_free(lhx);
    }
    GtkWidget *lw = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lw), ls->str);
    gtk_label_set_xalign(GTK_LABEL(lw), 0.0);
    _wire_hash_label(lw);
    gtk_box_pack_start(GTK_BOX(detail), lw, FALSE, FALSE, 0);
    g_string_free(ls, TRUE);
  }

  gchar *pretty = _pretty_json(json);
  gchar *escaped = g_markup_escape_text(pretty, -1);
  gchar *mono = g_strdup_printf("<tt>%s</tt>", escaped);
  GtkWidget *body = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(body), mono);
  gtk_label_set_xalign(GTK_LABEL(body), 0.0);
  gtk_label_set_selectable(GTK_LABEL(body), TRUE);
  gtk_box_pack_start(GTK_BOX(detail), body, FALSE, FALSE, 0);
  g_free(pretty);
  g_free(escaped);
  g_free(mono);
  return detail;
}

static void _links_free(gpointer p)
{
  if(p) g_array_free((GArray *)p, TRUE);
}

// Collapsed event row; the detail body is built lazily on first expand.
static GtkWidget *_event_widget(const dt_sv_logged_event_t *ev)
{
  gchar *hm = _header_markup(ev);
  GtkWidget *w = _collapsible(hm, NULL, FALSE);
  g_free(hm);

  GtkWidget *toggle = (GtkWidget *)g_object_get_data(G_OBJECT(w), "toggle");
  g_object_set_data_full(G_OBJECT(toggle), "lazy_json", g_strdup(ev->json), g_free);
  if(ev->links && ev->links->len)
  {
    GArray *copy = g_array_new(FALSE, FALSE, sizeof(dt_sv_link_t));
    g_array_append_vals(copy, ev->links->data, ev->links->len);
    g_object_set_data_full(G_OBJECT(toggle), "lazy_links", copy, _links_free);
  }

  // The header carries exactly one hash (the event's own): stash it so right-click
  // search works even when the list box defeats the label's link tracking.
  GtkWidget *header = (GtkWidget *)g_object_get_data(G_OBJECT(w), "header");
  if(header) g_object_set_data_full(G_OBJECT(header), "own-hash", _hashhex(ev->hash), g_free);
  return w;
}

// Append one event to the timeline list and register it as a declaration when
// it is a create event.
static void _timeline_append(const dt_sv_logged_event_t *ev)
{
  GtkWidget *w = _event_widget(ev);
  gtk_widget_show_all(w);
  gtk_container_add(GTK_CONTAINER(_g.timeline_list), w);
  GtkWidget *row = gtk_widget_get_parent(w); // the auto-created GtkListBoxRow
  g_object_set_data(G_OBJECT(row), "toggle", g_object_get_data(G_OBJECT(w), "toggle"));
  if(!g_strcmp0(ev->op, "create"))
  {
    gchar *hx = _hashhex(ev->hash);
    g_object_set_data_full(G_OBJECT(row), "svhash", g_strdup(hx), g_free);
    g_hash_table_replace(_g.decl_map, hx, row); // map owns hx
  }
}

// Make room for `incoming` rows: once they would take the list past the cap and its slack, drop
// the oldest rows down to the cap.
static void _timeline_trim(const guint incoming)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(_g.timeline_list));
  const int rows = (int)g_list_length(children) + (int)incoming;
  int excess = rows > TIMELINE_MAX_ROWS + TIMELINE_TRIM_SLACK ? rows - TIMELINE_MAX_ROWS : 0;
  for(GList *l = children; l && excess > 0; l = l->next, excess--)
  {
    GtkWidget *row = GTK_WIDGET(l->data);
    const char *hx = (const char *)g_object_get_data(G_OBJECT(row), "svhash");
    if(hx && g_hash_table_lookup(_g.decl_map, hx) == row) g_hash_table_remove(_g.decl_map, hx);
    gtk_widget_destroy(row);
  }
  g_list_free(children);
}

static void _clear_list(GtkWidget *list)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(list));
  for(GList *l = children; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
  g_list_free(children);
}

static void _update_count(void)
{
  gchar *s = g_strdup_printf(_("%u events"), dt_supervisor_events_count());
  gtk_label_set_text(GTK_LABEL(_g.count_label), s);
  g_free(s);
}

static const char *_group_key(const dt_sv_logged_event_t *ev, const char *by)
{
  if(!g_strcmp0(by, "thread")) return ev->thread;
  if(!g_strcmp0(by, "op")) return ev->op;
  return ev->domain;
}

static void _group_set_header(_group_t *g)
{
  gchar *e = g_markup_escape_text(g->key && *g->key ? g->key : "(none)", -1);
  gchar *hm = g_strdup_printf("<b>%s</b>  <span size=\"small\">(%d)</span>", e, g->count);
  gtk_label_set_markup(GTK_LABEL(g->header), hm);
  g_free(e);
  g_free(hm);
}

// Append one event into its grouped bucket (creating the bucket if needed),
// preserving existing groups/rows and their expansion state.
static void _grouped_append(const dt_sv_logged_event_t *ev, const char *by)
{
  const char *k = _group_key(ev, by);
  _group_t *g = (_group_t *)g_hash_table_lookup(_g.group_map, k);
  if(!g)
  {
    g = g_new0(_group_t, 1);
    g->key = g_strdup(k);
    g->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(g->body, 12);
    GtkWidget *coll = _collapsible("", g->body, FALSE);
    g->header = (GtkWidget *)g_object_get_data(G_OBJECT(coll), "header");
    gtk_container_add(GTK_CONTAINER(_g.grouped_list), coll);
    gtk_widget_show_all(coll);
    g_hash_table_insert(_g.group_map, g_strdup(k), g);
  }
  GtkWidget *row = _event_widget(ev);
  gtk_box_pack_start(GTK_BOX(g->body), row, FALSE, FALSE, 0);
  gtk_widget_show_all(row);
  g->count++;
  _group_set_header(g);
}

// Fold all events newer than the grouped cursor into the buckets (live append).
static void _grouped_catch_up(void)
{
  uint64_t newlast = _g.grouped_last_seq;
  GPtrArray *evs = dt_supervisor_events_snapshot_since(_g.grouped_last_seq, &newlast);
  if(evs->len)
  {
    gchar *by = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(_g.groupby));
    for(guint i = 0; i < evs->len; i++) _grouped_append(g_ptr_array_index(evs, i), by ? by : "domain");
    g_free(by);
    _g.grouped_last_seq = newlast;
  }
  dt_supervisor_events_free(evs);
}

// Full rebuild (group-by change / Clear): wipe buckets and re-fold everything.
static void _rebuild_grouped_now(void)
{
  _clear_list(_g.grouped_list);
  g_hash_table_remove_all(_g.group_map);
  _g.grouped_last_seq = 0;
  _grouped_catch_up();
}

// ---- Memory view -------------------------------------------------------------

// A clickable memory item (markup carries the navigation link). When `trailing`
// is non-NULL it is packed at the right edge as a separate label, so the trailing
// column (e.g. vRAM) lines up vertically across rows. An `evictable` item wears the
// theme's disabled look, without being insensitive: its link stays clickable.
static void _add_mem_item(GtkWidget *box, const char *markup, const char *trailing, const gboolean evictable)
{
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(lbl), markup);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
  /* A long name gives way: every row shares the width of the widest minimum among them, so one
   * file name too long for the window would push every trailing column out of it. */
  gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
  _wire_hash_label(lbl);

  if(trailing && *trailing)
  {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(row, 12);
    gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0); // expands → pushes trailing to the right
    GtkWidget *tl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(tl), trailing);
    gtk_label_set_xalign(GTK_LABEL(tl), 1.0);
    gtk_box_pack_end(GTK_BOX(row), tl, FALSE, FALSE, 0);
    if(evictable) gtk_style_context_add_class(gtk_widget_get_style_context(row), "dt_supervisor_evictable");
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
  }
  else
  {
    gtk_widget_set_margin_start(lbl, 12);
    if(evictable) gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "dt_supervisor_evictable");
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
  }
}

// "12 s", "14 min", "2 h": how long ago, to the unit that keeps it under two digits.
static gchar *_age_text(const int64_t us)
{
  const int64_t s = us / G_USEC_PER_SEC;
  if(s < 60) return g_strdup_printf("%" G_GINT64_FORMAT " s", s);
  if(s < 3600) return g_strdup_printf("%" G_GINT64_FORMAT " min", s / 60);
  return g_strdup_printf("%" G_GINT64_FORMAT " h", s / 3600);
}

// Held entries first, then each group by what it holds in RAM, descending.
static gint _cmp_ram_size(gconstpointer a, gconstpointer b)
{
  const dt_pixel_cache_stats_entry_t *x = a, *y = b;
  const gboolean x_held = x->refcount > 0, y_held = y->refcount > 0;
  if(x_held != y_held) return y_held - x_held;
  return (y->host_bytes > x->host_bytes) - (y->host_bytes < x->host_bytes);
}

static gint _cmp_mip_size(gconstpointer a, gconstpointer b)
{
  const dt_mipmap_cache_stats_entry_t *x = a, *y = b;
  return (y->size > x->size) - (y->size < x->size); // descending
}

static gint _cmp_image_imgid(gconstpointer a, gconstpointer b)
{
  const dt_image_cache_stats_entry_t *x = a, *y = b;
  return x->imgid - y->imgid; // ascending (entries are all the same size)
}

/* The memory view's bars. A bar is a total and the segments of it something holds, packed from the
 * left, each in the colour of what holds it -- the same colour in every bar. A segment counted but
 * not held as such (reserved, or left to others) is hatched; the one left to others sits at the
 * right end. The track is what is still free. The colours are categorical slots validated against
 * the window's grey (#686868): none reaches 3:1 on it, so a bar of several segments has a legend
 * naming each with its size, and every bar a tooltip naming the segment under the pointer. */
#define BAR_SEGMENTS 4
#define BAR_SEGMENT_GAP 2.0
#define COLOR_PIPELINE "#3987e5"
#define COLOR_ALLOCATIONS "#d95926"
#define COLOR_MIPMAP "#199e70"

typedef struct _bar_segment_t
{
  const char *label;
  size_t bytes;
  const char *color; // NULL: neutral
  gboolean hatched;  // counted, but not memory held as such
  gboolean right;    // packed from the right end
  size_t reserved;   // more of it counted, but not held as such: drawn with it, told apart in its text
} _bar_segment_t;

typedef struct _bar_t
{
  size_t total;
  const char *free_label;
  int count;
  _bar_segment_t seg[BAR_SEGMENTS];
} _bar_t;

static size_t _bar_segment_size(const _bar_segment_t *seg)
{
  return seg->bytes + seg->reserved;
}

// Every segment, the one left to others included.
static size_t _bar_used(const _bar_t *bar)
{
  size_t used = 0;
  for(int k = 0; k < bar->count; k++) used += _bar_segment_size(&bar->seg[k]);
  return used;
}

// What the bar's owner counts: every segment but the one left to others.
static size_t _bar_held(const _bar_t *bar)
{
  size_t held = 0;
  for(int k = 0; k < bar->count; k++)
    if(!bar->seg[k].right) held += _bar_segment_size(&bar->seg[k]);
  return held;
}

static size_t _bar_free(const _bar_t *bar)
{
  const size_t used = _bar_used(bar);
  return bar->total > used ? bar->total - used : 0;
}

// Where segment k lies in a bar `width` wide. Segments that outgrow the total (a budget kernel
// pressure lowered under its usage) scale the bar to their sum.
static void _bar_segment_span(const _bar_t *bar, const double width, const int k, double *x0, double *x1)
{
  const double scale = width / (double)MAX(bar->total, _bar_used(bar));
  size_t before = 0;
  if(bar->seg[k].right)
  {
    for(int i = k + 1; i < bar->count; i++)
      if(bar->seg[i].right) before += _bar_segment_size(&bar->seg[i]);
    *x1 = width - before * scale;
    *x0 = *x1 - _bar_segment_size(&bar->seg[k]) * scale;
    return;
  }
  for(int i = 0; i < k; i++)
    if(!bar->seg[i].right) before += _bar_segment_size(&bar->seg[i]);
  *x0 = before * scale;
  *x1 = *x0 + _bar_segment_size(&bar->seg[k]) * scale;
}

static gboolean _bar_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  const _bar_t *bar = (const _bar_t *)g_object_get_data(G_OBJECT(widget), "bar");
  const double width = gtk_widget_get_allocated_width(widget);
  const double height = gtk_widget_get_allocated_height(widget);

  // The track: what is still free.
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.08);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_fill(cr);
  if(IS_NULL_PTR(bar) || (bar->total == 0 && _bar_used(bar) == 0)) return FALSE;

  for(int k = 0; k < bar->count; k++)
  {
    const _bar_segment_t *seg = &bar->seg[k];
    if(_bar_segment_size(seg) == 0) continue;
    double x0 = 0.0, x1 = 0.0;
    _bar_segment_span(bar, width, k, &x0, &x1);
    GdkRGBA c = { 1.0, 1.0, 1.0, 0.45 };
    if(seg->color) gdk_rgba_parse(&c, seg->color);
    if(seg->hatched)
    {
      cairo_save(cr);
      cairo_rectangle(cr, x0, 0.0, x1 - x0, height);
      cairo_clip(cr);
      if(seg->color)
        cairo_set_source_rgba(cr, c.red, c.green, c.blue, 0.25);
      else
        cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.20);
      cairo_paint(cr);
      if(seg->color)
        gdk_cairo_set_source_rgba(cr, &c);
      else
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.30);
      cairo_set_line_width(cr, 1.0);
      for(double x = x0 - height; x < x1; x += 6.0)
      {
        cairo_move_to(cr, x, height);
        cairo_line_to(cr, x + height, 0.0);
      }
      cairo_stroke(cr);
      cairo_restore(cr);
      continue;
    }
    // A surface gap separates a segment from the next; a sliver too thin for one keeps a pixel.
    gdk_cairo_set_source_rgba(cr, &c);
    cairo_rectangle(cr, x0, 0.0, MAX(x1 - x0 - BAR_SEGMENT_GAP, 1.0), height);
    cairo_fill(cr);
  }
  return FALSE;
}

// "12.3 MiB", or "12.3 MiB (4.5 reserved)" for a segment counting more than it holds.
static gchar *_bar_segment_text(const _bar_segment_t *seg)
{
  return seg->reserved
             ? g_strdup_printf(_("%.1f MiB (%.1f reserved)"), _bar_segment_size(seg) / 1048576.0,
                               seg->reserved / 1048576.0)
             : g_strdup_printf("%.1f MiB", seg->bytes / 1048576.0);
}

static gboolean _bar_tooltip(GtkWidget *widget, gint x, gint y, gboolean keyboard, GtkTooltip *tooltip,
                             gpointer user_data)
{
  const _bar_t *bar = (const _bar_t *)g_object_get_data(G_OBJECT(widget), "bar");
  if(IS_NULL_PTR(bar) || bar->total == 0) return FALSE;

  const double width = gtk_widget_get_allocated_width(widget);
  const _bar_segment_t free_seg = { bar->free_label, _bar_free(bar), NULL, FALSE, FALSE, 0 };
  const _bar_segment_t *seg = &free_seg;
  for(int k = 0; k < bar->count; k++)
  {
    double x0 = 0.0, x1 = 0.0;
    _bar_segment_span(bar, width, k, &x0, &x1);
    if(_bar_segment_size(&bar->seg[k]) == 0 || x < x0 || x >= x1) continue;
    seg = &bar->seg[k];
    break;
  }
  gchar *size = _bar_segment_text(seg);
  gchar *text = g_strdup_printf("%s: %s, %.0f%%", seg->label, size,
                                100.0 * (double)_bar_segment_size(seg) / (double)bar->total);
  gtk_tooltip_set_text(tooltip, text);
  g_free(size);
  g_free(text);
  return TRUE;
}

/* One legend block: its swatch, then its label with its size on the line below, both lined up
 * clear of the swatch. Only the swatch wears the colour. */
static GtkWidget *_bar_legend_block(const _bar_segment_t *seg, const char *swatch)
{
  gchar *mark = seg->color ? g_markup_printf_escaped("<span foreground=\"%s\">%s</span>", seg->color, swatch)
                           : g_markup_escape_text(swatch, -1);
  GtkWidget *swatch_label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(swatch_label), mark);
  gtk_widget_set_valign(swatch_label, GTK_ALIGN_START);
  g_free(mark);

  gchar *size = _bar_segment_text(seg);
  gchar *text = g_markup_printf_escaped("%s\n%s", seg->label, size);
  GtkWidget *text_label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(text_label), text);
  gtk_label_set_xalign(GTK_LABEL(text_label), 0.0);
  g_free(size);
  g_free(text);

  GtkWidget *block = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  gtk_box_pack_start(GTK_BOX(block), swatch_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(block), text_label, FALSE, FALSE, 0);
  return block;
}

// A bar, then its legend.
static void _add_bar(GtkWidget *box, const _bar_t *bar)
{
  GtkWidget *area = gtk_drawing_area_new();
  gtk_widget_set_size_request(area, -1, 16);
  _bar_t *copy = g_new(_bar_t, 1);
  *copy = *bar;
  g_object_set_data_full(G_OBJECT(area), "bar", copy, g_free);
  g_signal_connect(area, "draw", G_CALLBACK(_bar_draw), NULL);
  gtk_widget_set_has_tooltip(area, TRUE);
  g_signal_connect(area, "query-tooltip", G_CALLBACK(_bar_tooltip), NULL);
  gtk_box_pack_start(GTK_BOX(box), area, FALSE, FALSE, 0);

  // A single segment needs no legend: the title names it.
  if(bar->count < 2) return;
  /* In the bar's order: the segments packed from the left, what is free, then, against the right
   * edge as in the bar, the segment left to others. */
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
  GtkWidget *legend = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(legend), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(legend), BAR_SEGMENTS + 1);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(legend), 24);
  for(int k = 0; k < bar->count; k++)
    if(!bar->seg[k].right)
      gtk_container_add(GTK_CONTAINER(legend), _bar_legend_block(&bar->seg[k], bar->seg[k].hatched ? "▨" : "■"));
  const _bar_segment_t free_seg = { bar->free_label, _bar_free(bar), NULL, FALSE, FALSE, 0 };
  gtk_container_add(GTK_CONTAINER(legend), _bar_legend_block(&free_seg, "□"));
  gtk_box_pack_start(GTK_BOX(row), legend, TRUE, TRUE, 0);
  for(int k = bar->count - 1; k >= 0; k--)
    if(bar->seg[k].right)
      gtk_box_pack_end(GTK_BOX(row), _bar_legend_block(&bar->seg[k], bar->seg[k].hatched ? "▨" : "■"), FALSE,
                       FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
}

static void _rebuild_memory(void);

static gboolean _rebuild_memory_idle(gpointer user_data)
{
  _rebuild_memory();
  return G_SOURCE_REMOVE;
}

/* A section is rebuilt at once rather than at the next refresh, folded or opened: what it folds is
 * built only while it is open. Not from here: the rebuild destroys this expander. */
// The conf key of a memory section's `setting`: "ui/supervisor/memory/ram/pipeline/folded".
static gchar *_mem_conf_key(const char *section, const char *setting)
{
  return g_strdup_printf("ui/supervisor/memory/%s/%s", section, setting);
}

static void _on_mem_section_expanded(GObject *expander, GParamSpec *pspec, gpointer user_data)
{
  gchar *key = _mem_conf_key((const char *)g_object_get_data(expander, "section"), "folded");
  dt_conf_set_bool(key, !gtk_expander_get_expanded(GTK_EXPANDER(expander)));
  g_free(key);
  g_idle_add(_rebuild_memory_idle, NULL);
}

/* A section of the memory view, packed in `parent`: its title -- `title` in bold, then what its
 * bar's owner holds over its total -- with its bar and its legend under it, and what it folds: a list
 * or the sections of what the memory it shows holds, indented under it. A section without a bar is
 * named by its title alone. The expander holds the title only and folds nothing itself: holding the
 * bar too, it centred its arrow on the bar. The view is rebuilt at every refresh, so which sections
 * are folded is kept by `key`, in the conf so that it outlasts the window, not by their widgets,
 * and what a folded section folds is not built at all: returns the box it goes in, NULL when
 * folded. */
static GtkWidget *_add_mem_section(GtkWidget *parent, const char *key, const char *title, const _bar_t *bar)
{
  GtkWidget *hdr = gtk_label_new(NULL);
  const size_t held = bar ? _bar_held(bar) : 0;
  gchar *t = bar ? g_markup_printf_escaped("<b>%s</b>   %.1f / %.1f MiB (%.0f%%)", title, held / 1048576.0,
                                           bar->total / 1048576.0,
                                           bar->total ? 100.0 * (double)held / (double)bar->total : 0.0)
                 : g_markup_printf_escaped("<b>%s</b>", title);
  gtk_label_set_markup(GTK_LABEL(hdr), t);
  g_free(t);
  gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(hdr), PANGO_ELLIPSIZE_END);
  // Clear of the expander's arrow, which the theme packs against it.
  gtk_widget_set_margin_start(hdr, 6);

  // Until the user folds or opens it, a top section is open and what it holds is folded.
  const gboolean top = parent == _g.mem_box;
  gchar *folded = _mem_conf_key(key, "folded");
  const gboolean expanded = dt_conf_key_exists(folded) ? !dt_conf_get_bool(folded) : top;
  g_free(folded);
  GtkWidget *expander = gtk_expander_new(NULL);
  gtk_expander_set_label_widget(GTK_EXPANDER(expander), hdr);
  gtk_expander_set_label_fill(GTK_EXPANDER(expander), TRUE);
  gtk_expander_set_expanded(GTK_EXPANDER(expander), expanded);
  g_object_set_data_full(G_OBJECT(expander), "section", g_strdup(key), g_free);
  g_signal_connect(expander, "notify::expanded", G_CALLBACK(_on_mem_section_expanded), NULL);

  GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_top(section, top ? 8 : 6);
  if(!top) gtk_widget_set_margin_start(section, 16);
  gtk_box_pack_start(GTK_BOX(section), expander, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(parent), section, FALSE, FALSE, 0);
  if(bar)
  {
    /* Lined up with the title's text: what the expander asks beyond its label is its arrow and their
     * spacing, as wide as the theme makes them, and the label's own margin comes on top. A hidden
     * widget asks nothing, and the theme reaches them once they are in the window. */
    gtk_widget_show(expander);
    gtk_widget_show(hdr);
    int expander_width = 0, label_width = 0;
    gtk_widget_get_preferred_width(expander, NULL, &expander_width);
    gtk_widget_get_preferred_width(hdr, NULL, &label_width);
    GtkWidget *bar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(bar_box, MAX(expander_width - label_width, 0) + gtk_widget_get_margin_start(hdr));
    _add_bar(bar_box, bar);
    gtk_box_pack_start(GTK_BOX(section), bar_box, FALSE, FALSE, 0);
  }
  if(!expanded) return NULL;

  GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_box_pack_start(GTK_BOX(section), body, FALSE, FALSE, 0);
  return body;
}

/* A section's list: a recessed area of a fixed height, which the user changes by the grip on its
 * bottom edge, kept in the conf. Fitted to its rows, a list first seen empty stayed one row tall
 * as it filled. The view is rebuilt at every refresh, but not the area: kept by its section,
 * unpacked before the rebuild and packed again around new rows, it keeps its height and where it
 * was scrolled. Its rows are swapped between two allocations, so its scroll position is never
 * clamped to an empty list. Returns the box the rows go in, emptied. */
static GtkWidget *_mem_list(GtkWidget *body, const char *section)
{
  GtkWidget *area = (GtkWidget *)g_hash_table_lookup(_g.mem_lists, section);
  if(IS_NULL_PTR(area))
  {
    GtkWidget *list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gchar *key = _mem_conf_key(section, "height");
    /* Until the grip sets it, MEMORY_LIST_ROWS rows. The list may be empty yet, so a row is measured
     * on a probe built like one, in the window, where the theme reaches it. */
    if(!dt_conf_key_exists(key))
    {
      GtkWidget *probe = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_box_pack_start(GTK_BOX(body), probe, FALSE, FALSE, 0);
      _add_mem_item(probe, "<a href=\"0\"><tt>0x0</tt></a>  <i>0</i>", "<tt>0</tt>", FALSE);
      // A hidden widget asks for nothing.
      gtk_widget_show_all(probe);
      int row = 0;
      gtk_widget_get_preferred_height(probe, NULL, &row);
      gtk_widget_destroy(probe);
      if(row > 0) dt_conf_set_int(key, MEMORY_LIST_ROWS * (row + gtk_box_get_spacing(GTK_BOX(list))));
    }
    area = dt_ui_scroll_wrap(list, 20, key, DT_UI_RESIZE_STATIC);
    g_free(key);
    dt_gui_add_class(dt_ui_scroll_wrap_get_scrolled_window(area), "dt_recessed_scroll");
    g_object_set_data(G_OBJECT(area), "list", list);
    g_hash_table_insert(_g.mem_lists, g_strdup(section), g_object_ref_sink(area));
  }
  GtkWidget *list = (GtkWidget *)g_object_get_data(G_OBJECT(area), "list");
  _clear_list(list);
  gtk_box_pack_start(GTK_BOX(body), area, FALSE, FALSE, 0);
  return list;
}

// Held lines first, then each group by what it holds on the device passed in `data`, descending.
static gint _cmp_device_size(gconstpointer a, gconstpointer b, gpointer data)
{
  const dt_pixel_cache_stats_entry_t *x = a, *y = b;
  const int devid = GPOINTER_TO_INT(data);
  const gboolean x_held = x->refcount > 0, y_held = y->refcount > 0;
  if(x_held != y_held) return y_held - x_held;
  const size_t xb = x->devices[devid].cl_bytes, yb = y->devices[devid].cl_bytes;
  return (yb > xb) - (yb < xb);
}

/* One OpenCL device: a bar over its memory, then, under it, the pipeline lines holding buffers on
 * it. Ansel's own accounting gives what it holds there; what the lines hold is one part of that,
 * everything else -- modules' working buffers, and what they keep there for longer -- the other.
 * The device's headroom, what Ansel leaves to other applications there, is hatched at the right
 * end. What they actually hold is not known to OpenCL, so the track counts it as free. */
static void _add_device_section(GArray *pe, const int devid)
{
  size_t lines = 0;
  for(guint i = 0; i < pe->len; i++)
    lines += g_array_index(pe, dt_pixel_cache_stats_entry_t, i).devices[devid].cl_bytes;
  // Read apart from the lines' buffers, hence the clamp.
  const size_t in_use = dt_opencl_get_device_memory_in_use(devid);
  const _bar_t bar = {
    .total = dt_opencl_get_device_max_global_mem(devid),
    .free_label = _("free in the budget"),
    .count = 3,
    .seg = { { _("pipeline cache, lines in vRAM"), lines, COLOR_PIPELINE, FALSE, FALSE },
             { _("allocations outside the lines"), in_use > lines ? in_use - lines : 0, COLOR_ALLOCATIONS, FALSE,
               FALSE },
             { _("headroom for display and applications"), dt_opencl_get_device_headroom(devid), NULL, TRUE,
               TRUE } },
  };
  const char *name = dt_opencl_get_device_name(devid);
  gchar *title = name ? g_strdup_printf(_("vRAM — %s"), name) : g_strdup_printf(_("vRAM — device %d"), devid);
  gchar *key = g_strdup_printf("vram-%d", devid);
  GtkWidget *body = _add_mem_section(_g.mem_box, key, title, &bar);
  g_free(key);
  g_free(title);
  if(!body) return;

  // Its size is the device bar's first segment.
  guint count = 0;
  for(guint i = 0; i < pe->len; i++)
    if(g_array_index(pe, dt_pixel_cache_stats_entry_t, i).devices[devid].cl_count) count++;
  gchar *ltitle = g_strdup_printf(_("Pipeline cache — %u items"), count);
  key = g_strdup_printf("vram-%d/pipeline", devid);
  GtkWidget *lbody = _add_mem_section(body, key, ltitle, NULL);
  g_free(ltitle);
  if(!lbody)
  {
    g_free(key);
    return;
  }
  GtkWidget *list = _mem_list(lbody, key);

  g_array_sort_with_data(pe, _cmp_device_size, GINT_TO_POINTER(devid));
  guint shown = 0;
  for(guint i = 0; i < pe->len && shown < MEMORY_MAX_ROWS; i++)
  {
    const dt_pixel_cache_stats_entry_t *e = &g_array_index(pe, dt_pixel_cache_stats_entry_t, i);
    const dt_pixel_cache_stats_device_t *d = &e->devices[devid];
    if(d->cl_count == 0) continue;
    gchar *hx = _hashhex(e->hash);
    gchar *ename = g_markup_escape_text(e->name[0] ? e->name : "-", -1);
    gchar *m = g_strdup_printf("<a href=\"%s\"><tt>%s</tt></a>  refs=%d hits=%d  <i>%s</i>", hx, hx, e->refcount,
                               e->hits, ename);
    gchar *column = g_strdup_printf("<tt>%8.2f MiB %2d buf</tt>", d->cl_bytes / 1048576.0, d->cl_count);
    _add_mem_item(list, m, column, e->refcount == 0);
    g_free(hx);
    g_free(ename);
    g_free(m);
    g_free(column);
    shown++;
  }
  g_free(key);
}

static void _rebuild_memory(void)
{
  if(!_g.mem_box) return;
  // Out of the sections first: destroying a container destroys its children, referenced or not.
  GHashTableIter it;
  gpointer area = NULL;
  g_hash_table_iter_init(&it, _g.mem_lists);
  while(g_hash_table_iter_next(&it, NULL, &area))
  {
    GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(area));
    if(parent) gtk_container_remove(GTK_CONTAINER(parent), GTK_WIDGET(area));
  }
  _clear_list(_g.mem_box);

  // Pipeline cache
  size_t cur = 0, max = 0;
  dt_dev_pixelpipe_cache_get_usage(NULL, &max);
  GArray *pe = dt_dev_pixelpipe_cache_get_entries_stats();

  /* The budget is not all RAM: a line kept on the device only reserves its size without a host
   * buffer. What modules allocate there -- working buffers for one run, or lookup tables and state
   * kept for longer -- counts in it without being lines. Lines and allocations are what the usage
   * sums, read under one hold of the cache lock. */
  const gboolean gpu = dt_opencl_is_enabled();
  size_t in_ram = 0, reserved = 0, allocated = 0;
  guint lines = 0;
  for(guint i = 0; i < pe->len; i++)
  {
    const dt_pixel_cache_stats_entry_t *e = &g_array_index(pe, dt_pixel_cache_stats_entry_t, i);
    if(e->allocation)
    {
      allocated += e->size;
      continue;
    }
    in_ram += e->host_bytes;
    reserved += e->size - e->host_bytes;
    lines++;
  }

  size_t mip_cur = 0, mip_max = 0;
  dt_mipmap_cache_get_usage(&mip_cur, &mip_max);

  /* The startup plan gives the OS and other applications their headroom (preferences), the
   * mipmap cache its budget, and the pipeline cache the rest -- so the system's share is what
   * the two cache budgets leave of the total. */
  const size_t total = dt_get_total_mem();
  const size_t budgets = mip_max + dt_dev_pixelpipe_cache_get_max_memory();
  const _bar_t ram_bar = {
    .total = total,
    .free_label = _("free in the cache budgets"),
    .count = 3,
    /* Its lines in RAM and its allocations, and what lines kept in vRAM only reserve of its
     * budget: no RAM, but the cache evicts and refuses on it as on the rest. The bar below tells
     * the three apart. */
    .seg = { { _("pipeline cache (RAM)"), in_ram + allocated, COLOR_PIPELINE, FALSE, FALSE, reserved },
             { _("mipmap cache"), mip_cur, COLOR_MIPMAP, FALSE, FALSE },
             { _("headroom for OS and applications"), total > budgets ? total - budgets : 0, NULL, TRUE, TRUE } },
  };
  // The caches it holds, under it; each device, with what it holds, after it.
  GtkWidget *rbody = _add_mem_section(_g.mem_box, "ram", _("RAM"), &ram_bar);

  /* The budget is not all RAM: a line an OpenCL module keeps on the device only reserves its size,
   * so it is hatched in the pipeline cache's colour. */
  const _bar_t pipeline = {
    .total = max,
    .free_label = _("free in the budget"),
    .count = gpu ? 3 : 2,
    .seg = { { gpu ? _("lines in RAM") : _("lines"), in_ram, COLOR_PIPELINE, FALSE, FALSE },
             { _("allocations outside the lines"), allocated, COLOR_ALLOCATIONS, FALSE, FALSE },
             { _("reserved by lines in vRAM only"), reserved, COLOR_PIPELINE, TRUE, FALSE } },
  };
  gchar *ptitle = g_strdup_printf(gpu ? _("Pipeline cache (RAM budget) — %u items") : _("Pipeline cache — %u items"),
                                  lines);
  GtkWidget *pbody = rbody ? _add_mem_section(rbody, "ram/pipeline", ptitle, &pipeline) : NULL;
  g_free(ptitle);

  /* What the pipeline cache holds in RAM: its lines' host buffers and the allocations, each after a
   * swatch in its segment's colour. A line kept on a device only is listed under that device alone;
   * one with buffers in both memories, under both, each with what it holds there. The age tells a
   * working buffer, which lives for one module's run, from what is kept for longer. */
  if(pbody)
  {
    GtkWidget *list = _mem_list(pbody, "ram/pipeline");
    g_array_sort(pe, _cmp_ram_size);
    guint shown = 0;
    for(guint i = 0; i < pe->len && shown < MEMORY_MAX_ROWS; i++)
    {
      const dt_pixel_cache_stats_entry_t *e = &g_array_index(pe, dt_pixel_cache_stats_entry_t, i);
      if(e->host_bytes == 0) continue;
      gchar *name = g_markup_escape_text(e->name[0] ? e->name : "-", -1);
      /* A fixed-width monospace column, so the sizes line up across rows. A stale host buffer
       * occupies RAM but holds the pixels of another hash. The age is one phrase, aligned right: padded
       * between its verb and its duration, the verb read as a column title. */
      gchar *age = _age_text(e->age_us);
      gchar *when = g_strdup_printf("%s %s ago", e->allocation ? "allocated" : "used", age);
      gchar *column = g_strdup_printf("<tt>%8.2f MiB%s  %20s</tt>", e->host_bytes / 1048576.0,
                                      e->host_stale ? " stale" : "      ", when);
      g_free(age);
      g_free(when);
      if(e->allocation)
      {
        // Named after where it was allocated; its hash is its address, which no event names.
        gchar *m = g_strdup_printf("<span foreground=\"%s\">■</span> %s  <i>%s</i>", COLOR_ALLOCATIONS,
                                   _("allocation"), name);
        _add_mem_item(list, m, column, FALSE);
        g_free(m);
      }
      else
      {
        gchar *hx = _hashhex(e->hash);
        gchar *m = g_strdup_printf("<span foreground=\"%s\">■</span> <a href=\"%s\"><tt>%s</tt></a>  refs=%d "
                                   "hits=%d  <i>%s</i>",
                                   COLOR_PIPELINE, hx, hx, e->refcount, e->hits, name);
        /* A line nobody holds is what the LRU, the pressure valve and the flushes may free. They
         * also skip a line someone has locked, but whoever writes or reads a line holds it too. */
        _add_mem_item(list, m, column, e->refcount == 0);
        g_free(hx);
        g_free(m);
      }
      g_free(name);
      g_free(column);
      shown++;
    }
  }

  // Mipmap cache
  GArray *me = dt_mipmap_cache_get_entries_stats();
  const _bar_t mipmap = {
    .total = mip_max,
    .free_label = _("free in the budget"),
    .count = 1,
    .seg = { { _("mipmap cache"), mip_cur, COLOR_MIPMAP, FALSE, FALSE } },
  };
  gchar *mtitle = g_strdup_printf(_("Mipmap cache — %u items"), me->len);
  GtkWidget *mbody = rbody ? _add_mem_section(rbody, "ram/mipmap", mtitle, &mipmap) : NULL;
  g_free(mtitle);
  GtkWidget *mlist = mbody ? _mem_list(mbody, "ram/mipmap") : NULL;
  if(mlist) g_array_sort(me, _cmp_mip_size);
  for(guint i = 0; mlist && i < me->len && i < MEMORY_MAX_ROWS; i++)
  {
    const dt_mipmap_cache_stats_entry_t *e = &g_array_index(me, dt_mipmap_cache_stats_entry_t, i);
    gchar *hx = _hashhex(dt_supervisor_mipmap_key(e->imgid, e->mip));
    gchar *m = g_strdup_printf("<a href=\"%s\">image #%d · mip %d</a>  %.2f MiB", hx, e->imgid, e->mip,
                               e->size / 1048576.0);
    _add_mem_item(mlist, m, NULL, FALSE);
    g_free(hx);
    g_free(m);
  }
  g_array_free(me, TRUE);

  // Image cache
  dt_image_cache_get_usage(&cur, &max);
  GArray *ie = dt_image_cache_get_entries_stats();
  // Not in the RAM bar: its colour is neutral.
  const _bar_t image = {
    .total = max,
    .free_label = _("free in the budget"),
    .count = 1,
    .seg = { { _("image cache"), cur, NULL, FALSE, FALSE } },
  };
  gchar *ititle = g_strdup_printf(_("Image cache — %u items"), ie->len);
  GtkWidget *ibody = rbody ? _add_mem_section(rbody, "ram/image", ititle, &image) : NULL;
  g_free(ititle);
  GtkWidget *ilist = ibody ? _mem_list(ibody, "ram/image") : NULL;
  if(ilist) g_array_sort(ie, _cmp_image_imgid);
  for(guint i = 0; ilist && i < ie->len && i < MEMORY_MAX_ROWS; i++)
  {
    const dt_image_cache_stats_entry_t *e = &g_array_index(ie, dt_image_cache_stats_entry_t, i);
    gchar *hx = _hashhex(dt_supervisor_image_key(e->imgid));
    gchar *fn = g_markup_escape_text(e->filename[0] ? e->filename : "-", -1);
    gchar *m = g_strdup_printf("<a href=\"%s\">image #%d</a>  <i>%s</i>", hx, e->imgid, fn);
    _add_mem_item(ilist, m, NULL, FALSE);
    g_free(hx);
    g_free(fn);
    g_free(m);
  }
  g_array_free(ie, TRUE);

  if(gpu)
    for(int devid = 0; devid < dt_opencl_get_num_devices(); devid++) _add_device_section(pe, devid);
  g_array_free(pe, TRUE);

  gtk_widget_show_all(_g.mem_box);
}

// ---- Search ------------------------------------------------------------------

// TRUE if the hash's hex (lowercased, with 0x) contains the needle.
static gboolean _hex_contains(const uint64_t h, const char *needle)
{
  gchar *hx = _hashhex(h);
  gchar *lh = g_ascii_strdown(hx, -1);
  const gboolean m = strstr(lh, needle) != NULL;
  g_free(hx);
  g_free(lh);
  return m;
}

// An event matches when:
//  - its own hash or any linked hash contains `hash_needle` (hex, 0x stripped), or
//  - its full record (JSON: module/op/domain/widget/filename/parameters/…)
//    contains `text_needle` as a case-insensitive substring.
static gboolean _event_matches(const dt_sv_logged_event_t *ev, const char *hash_needle,
                               const char *text_needle)
{
  if(*hash_needle)
  {
    if(_hex_contains(ev->hash, hash_needle)) return TRUE;
    if(ev->links)
      for(guint i = 0; i < ev->links->len; i++)
        if(_hex_contains(g_array_index(ev->links, dt_sv_link_t, i).hash, hash_needle)) return TRUE;
  }
  if(*text_needle && ev->json)
  {
    gchar *jl = g_ascii_strdown(ev->json, -1);
    const gboolean m = strstr(jl, text_needle) != NULL;
    g_free(jl);
    if(m) return TRUE;
  }
  return FALSE;
}

static void _run_search(const char *query)
{
  if(!_g.search_list) return;
  _clear_list(_g.search_list);
  if(!query || !*query)
  {
    gtk_widget_show_all(_g.search_list);
    return;
  }

  // text_needle matches any field of the record; hash_needle (0x stripped) matches hashes.
  gchar *text_needle = g_ascii_strdown(query, -1);
  const char *hash_needle = g_str_has_prefix(text_needle, "0x") ? text_needle + 2 : text_needle;

  GPtrArray *evs = dt_supervisor_events_snapshot();
  for(guint i = 0; i < evs->len; i++)
  {
    const dt_sv_logged_event_t *ev = g_ptr_array_index(evs, i);
    if(_event_matches(ev, hash_needle, text_needle))
      gtk_container_add(GTK_CONTAINER(_g.search_list), _event_widget(ev));
  }
  dt_supervisor_events_free(evs);
  g_free(text_needle);
  gtk_widget_show_all(_g.search_list);
}

static void _on_search_changed(GtkSearchEntry *e, gpointer u)
{
  const char *txt = gtk_entry_get_text(GTK_ENTRY(e));
  // Typing a query jumps to the Search page automatically.
  if(txt && *txt) gtk_notebook_set_current_page(GTK_NOTEBOOK(_g.notebook), _g.page_search);
  _run_search(txt);
}

static gboolean _scroll_bottom_idle(gpointer u)
{
  if(!_g.timeline_scroll) return G_SOURCE_REMOVE;
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_g.timeline_scroll));
  if(adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
  return G_SOURCE_REMOVE;
}

/* A mouse button held over the window: a drag in progress, maybe on a list's grip, which a rebuild
 * of the memory view would cut short by unpacking the grip under the pointer. */
static gboolean _pointer_button_down(void)
{
  GdkWindow *window = gtk_widget_get_window(_g.window);
  if(IS_NULL_PTR(window)) return FALSE;
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(window)));
  GdkModifierType mask = 0;
  gdk_window_get_device_position(window, pointer, NULL, NULL, &mask);
  return (mask & (GDK_BUTTON1_MASK | GDK_BUTTON2_MASK | GDK_BUTTON3_MASK)) != 0;
}

static gboolean _poll(gpointer u)
{
  if(!_g.window) return G_SOURCE_REMOVE;

  _g.tick++;
  const int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(_g.notebook));
  // The grouped view appends new events live (incrementally, preserving folds)
  // while it is the visible page.
  if(page == _g.page_grouped) _grouped_catch_up();
  // The memory view reflects live cache state; refresh it on a throttled cadence.
  // The search view is intentionally NOT auto-refreshed here: rebuilding it would
  // collapse any rows the user expanded. It updates on input / Refresh instead.
  if((_g.tick % MEMORY_REFRESH_TICKS) == 0 && page == _g.page_memory && !_pointer_button_down())
    _rebuild_memory();

  uint64_t newlast = _g.last_seq;
  GPtrArray *evs = dt_supervisor_events_snapshot_since(_g.last_seq, &newlast);
  if(evs->len)
  {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_g.timeline_scroll));
    const gboolean at_bottom = !adj
        || gtk_adjustment_get_value(adj)
               >= gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) - 4.0;

    /* Appending a row to a list box in the window costs GTK 3 time in proportion to the rows
     * already there, so a batch never takes the list past its cap and slack, even for a moment:
     * older events would be trimmed right away. A large batch -- the whole log, when the window
     * opens -- is built with the list out of the window, where every row costs the same; below
     * half the cap, appending in place is cheaper. Measured on a mock with the theme: 4000 rows
     * appended in place 4.2 s, built out of the window 2.9 s; a 12500-row batch in place hung
     * the GUI thread. */
    const guint first = evs->len > TIMELINE_MAX_ROWS ? evs->len - TIMELINE_MAX_ROWS : 0;
    const guint incoming = evs->len - first;
    GtkWidget *viewport = incoming > TIMELINE_MAX_ROWS / 2 ? gtk_widget_get_parent(_g.timeline_list) : NULL;
    if(viewport)
    {
      g_object_ref(_g.timeline_list);
      gtk_container_remove(GTK_CONTAINER(viewport), _g.timeline_list);
    }
    _timeline_trim(incoming);
    for(guint i = first; i < evs->len; i++) _timeline_append(g_ptr_array_index(evs, i));
    if(viewport)
    {
      gtk_container_add(GTK_CONTAINER(viewport), _g.timeline_list);
      g_object_unref(_g.timeline_list);
    }
    _g.last_seq = newlast;
    _update_count();
    if(at_bottom) g_idle_add(_scroll_bottom_idle, NULL);
  }
  dt_supervisor_events_free(evs);
  return G_SOURCE_CONTINUE;
}

static void _full_reload(void)
{
  _clear_list(_g.timeline_list);
  g_hash_table_remove_all(_g.decl_map);
  _g.last_seq = 0;
  _poll(NULL); // appends everything since the start
  _rebuild_grouped_now();
  _rebuild_memory();
  _run_search(gtk_entry_get_text(GTK_ENTRY(_g.search_entry)));
}

static void _on_refresh(GtkButton *b, gpointer u) { _full_reload(); }

static void _on_clear(GtkButton *b, gpointer u)
{
  dt_supervisor_events_clear();
  _full_reload();
}

/* The page shown, the group-by key and the Record box, kept for the next time the window opens. The
 * page is kept by name, not by index, so that it survives a change in the order of the tabs. */
#define SUPERVISOR_CONF_PAGE "ui/supervisor/page"
#define SUPERVISOR_CONF_GROUP_BY "ui/supervisor/group_by"
#define SUPERVISOR_CONF_RECORD "ui/supervisor/record"

static const char *_page_name(const int page)
{
  if(page == _g.page_timeline) return "timeline";
  if(page == _g.page_grouped) return "grouped";
  if(page == _g.page_search) return "search";
  return "memory";
}

static int _page_from_name(const char *name)
{
  if(!g_strcmp0(name, "timeline")) return _g.page_timeline;
  if(!g_strcmp0(name, "grouped")) return _g.page_grouped;
  if(!g_strcmp0(name, "search")) return _g.page_search;
  return _g.page_memory;
}

static void _on_groupby_changed(GtkComboBox *c, gpointer u)
{
  gchar *by = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(c));
  if(by) dt_conf_set_string(SUPERVISOR_CONF_GROUP_BY, by);
  g_free(by);
  _rebuild_grouped_now(); // regroup everything under the new key
}

static void _on_record_toggled(GtkToggleButton *t, gpointer u)
{
  const gboolean active = gtk_toggle_button_get_active(t);
  dt_conf_set_bool(SUPERVISOR_CONF_RECORD, active);
  dt_supervisor_set_recording(active);
}

static void _on_page_changed(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer u)
{
  dt_conf_set_string(SUPERVISOR_CONF_PAGE, _page_name((int)page_num));
  if((int)page_num == _g.page_grouped)
    _grouped_catch_up(); // fold in whatever arrived while the page was hidden
  else if((int)page_num == _g.page_memory)
    _rebuild_memory();
  else if((int)page_num == _g.page_search)
    _run_search(gtk_entry_get_text(GTK_ENTRY(_g.search_entry)));
}

/* Where the user left the window and how large they made it. */
#define SUPERVISOR_CONF_X "ui/supervisor/window_x"
#define SUPERVISOR_CONF_Y "ui/supervisor/window_y"
#define SUPERVISOR_CONF_WIDTH "ui/supervisor/window_width"
#define SUPERVISOR_CONF_HEIGHT "ui/supervisor/window_height"

// Wayland gives a client neither its own position nor the right to set it: there, the size only.
static gboolean _window_position_is_usable(GtkWidget *window)
{
#ifdef GDK_WINDOWING_WAYLAND
  return !GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(window));
#else
  return TRUE;
#endif
}

/* Kept as the user moves and resizes the window, not when it closes: Ansel can quit with it open.
 * A maximized or fullscreen window has no geometry of its own to keep. */
static gboolean _on_configure(GtkWidget *window, GdkEventConfigure *event, gpointer user_data)
{
  GdkWindow *gdk_window = gtk_widget_get_window(window);
  if(IS_NULL_PTR(gdk_window)
     || (gdk_window_get_state(gdk_window) & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN)))
    return FALSE;

  int width = 0, height = 0;
  gtk_window_get_size(GTK_WINDOW(window), &width, &height);
  dt_conf_set_int(SUPERVISOR_CONF_WIDTH, width);
  dt_conf_set_int(SUPERVISOR_CONF_HEIGHT, height);
  if(!_window_position_is_usable(window)) return FALSE;

  int x = 0, y = 0;
  gtk_window_get_position(GTK_WINDOW(window), &x, &y);
  dt_conf_set_int(SUPERVISOR_CONF_X, x);
  dt_conf_set_int(SUPERVISOR_CONF_Y, y);
  return FALSE;
}

/* Puts the window back as it was left, before it is mapped. With nothing stored, the default size,
 * and the window manager places it. A position saved on a monitor no longer attached would strand
 * the window off screen, so it is clamped into the work area of the monitor it lands on. */
static void _restore_geometry(GtkWidget *window)
{
  int width = 1000, height = 640;
  if(dt_conf_key_exists(SUPERVISOR_CONF_WIDTH) && dt_conf_key_exists(SUPERVISOR_CONF_HEIGHT))
  {
    width = MAX(dt_conf_get_int(SUPERVISOR_CONF_WIDTH), 1);
    height = MAX(dt_conf_get_int(SUPERVISOR_CONF_HEIGHT), 1);
  }
  gtk_window_set_default_size(GTK_WINDOW(window), width, height);

  if(!_window_position_is_usable(window) || !dt_conf_key_exists(SUPERVISOR_CONF_X)
     || !dt_conf_key_exists(SUPERVISOR_CONF_Y))
    return;

  int x = dt_conf_get_int(SUPERVISOR_CONF_X);
  int y = dt_conf_get_int(SUPERVISOR_CONF_Y);
  GdkDisplay *display = gtk_widget_get_display(window);
  GdkMonitor *monitor = gdk_display_get_monitor_at_point(display, x + width / 2, y + height / 2);
  if(IS_NULL_PTR(monitor)) monitor = gdk_display_get_primary_monitor(display);
  if(IS_NULL_PTR(monitor) && gdk_display_get_n_monitors(display) > 0) monitor = gdk_display_get_monitor(display, 0);
  if(!IS_NULL_PTR(monitor))
  {
    GdkRectangle workarea = { 0 };
    gdk_monitor_get_workarea(monitor, &workarea);
    x = CLAMP(x, workarea.x, workarea.x + MAX(0, workarea.width - width));
    y = CLAMP(y, workarea.y, workarea.y + MAX(0, workarea.height - height));
  }
  gtk_window_move(GTK_WINDOW(window), x, y);
}

static void _on_destroy(GtkWidget *w, gpointer u)
{
  if(_g.timer_id) g_source_remove(_g.timer_id);
  dt_supervisor_set_recording(FALSE); // stop capturing once the viewer is gone
  if(_g.decl_map) g_hash_table_destroy(_g.decl_map);
  if(_g.group_map) g_hash_table_destroy(_g.group_map);
  if(_g.mem_lists) g_hash_table_destroy(_g.mem_lists);
  g_free(_g.scroll_target);
  memset(&_g, 0, sizeof(_g));
}

void dt_gui_supervisor_window_show(void)
{
  if(_g.window)
  {
    gtk_window_present(GTK_WINDOW(_g.window));
    return;
  }

  _g.decl_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  _g.group_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, _group_free);
  _g.mem_lists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);

  _g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(_g.window), _("Event supervisor"));
  gtk_window_set_transient_for(GTK_WINDOW(_g.window), GTK_WINDOW(dt_gui_main_window()));
  _restore_geometry(_g.window);
  g_signal_connect(_g.window, "configure-event", G_CALLBACK(_on_configure), NULL);
  g_signal_connect(_g.window, "destroy", G_CALLBACK(_on_destroy), NULL);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(_g.window), vbox);

  // toolbar
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

  GtkWidget *record = gtk_check_button_new_with_label(_("Record"));
  // Capturing on open unless the user unticked it last time.
  const gboolean recording = !dt_conf_key_exists(SUPERVISOR_CONF_RECORD) || dt_conf_get_bool(SUPERVISOR_CONF_RECORD);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(record), recording);
  dt_supervisor_set_recording(recording);
  g_signal_connect(record, "toggled", G_CALLBACK(_on_record_toggled), NULL);
  gtk_box_pack_start(GTK_BOX(bar), record, FALSE, FALSE, 0);

  GtkWidget *refresh = gtk_button_new_with_label(_("Refresh"));
  g_signal_connect(refresh, "clicked", G_CALLBACK(_on_refresh), NULL);
  gtk_box_pack_start(GTK_BOX(bar), refresh, FALSE, FALSE, 0);

  GtkWidget *clear = gtk_button_new_with_label(_("Clear"));
  g_signal_connect(clear, "clicked", G_CALLBACK(_on_clear), NULL);
  gtk_box_pack_start(GTK_BOX(bar), clear, FALSE, FALSE, 0);

  gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(_("Group by")), FALSE, FALSE, 0);
  _g.groupby = gtk_combo_box_text_new();
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(_g.groupby), "domain");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(_g.groupby), "thread");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(_g.groupby), "op");
  // The combo's ids are its texts, so the kept key selects its entry; an unknown one falls back to domain.
  gtk_combo_box_set_id_column(GTK_COMBO_BOX(_g.groupby), 0);
  if(!dt_conf_key_exists(SUPERVISOR_CONF_GROUP_BY)
     || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(_g.groupby), dt_conf_get_string_const(SUPERVISOR_CONF_GROUP_BY)))
    gtk_combo_box_set_active(GTK_COMBO_BOX(_g.groupby), 0);
  g_signal_connect(_g.groupby, "changed", G_CALLBACK(_on_groupby_changed), NULL);
  gtk_box_pack_start(GTK_BOX(bar), _g.groupby, FALSE, FALSE, 0);

  // global search entry: query all events by hash (own or linked)
  _g.search_entry = gtk_search_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(_g.search_entry), _("search hash or text…"));
  gtk_widget_set_size_request(_g.search_entry, 220, -1);
  g_signal_connect(_g.search_entry, "search-changed", G_CALLBACK(_on_search_changed), NULL);
  gtk_box_pack_end(GTK_BOX(bar), _g.search_entry, FALSE, FALSE, 0);

  _g.count_label = gtk_label_new("");
  gtk_box_pack_end(GTK_BOX(bar), _g.count_label, FALSE, FALSE, 0);

  // notebook with the four pages, tabs spread across the full width
  _g.notebook = gtk_notebook_new();
  gtk_box_pack_start(GTK_BOX(vbox), _g.notebook, TRUE, TRUE, 0);

  _g.mem_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_container_set_border_width(GTK_CONTAINER(_g.mem_box), 6);
  GtkWidget *sw3 = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sw3), _g.mem_box);
  _g.page_memory = gtk_notebook_append_page(GTK_NOTEBOOK(_g.notebook), sw3, gtk_label_new(_("Memory")));

  _g.timeline_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_g.timeline_list), GTK_SELECTION_SINGLE);
  _g.timeline_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(_g.timeline_scroll), _g.timeline_list);
  _g.page_timeline = gtk_notebook_append_page(GTK_NOTEBOOK(_g.notebook), _g.timeline_scroll,
                                              gtk_label_new(_("Timeline")));

  _g.grouped_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_g.grouped_list), GTK_SELECTION_NONE);
  GtkWidget *sw2 = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sw2), _g.grouped_list);
  _g.page_grouped = gtk_notebook_append_page(GTK_NOTEBOOK(_g.notebook), sw2, gtk_label_new(_("Grouped")));

  _g.search_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_g.search_list), GTK_SELECTION_NONE);
  GtkWidget *sw4 = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sw4), _g.search_list);
  _g.page_search = gtk_notebook_append_page(GTK_NOTEBOOK(_g.notebook), sw4, gtk_label_new(_("Search")));

  // make each tab expand to fill the notebook width
  GList *pages = gtk_container_get_children(GTK_CONTAINER(_g.notebook));
  for(GList *l = pages; l; l = l->next)
    gtk_container_child_set(GTK_CONTAINER(_g.notebook), GTK_WIDGET(l->data), "tab-expand", TRUE,
                            "tab-fill", TRUE, NULL);
  g_list_free(pages);

  gtk_widget_show_all(_g.window);
  /* The page left open last time, the memory view at first. Selected before the handler is connected:
   * appending the first page made it current, which would otherwise have been kept over it. A notebook
   * shows a page only once it is visible, hence after show_all. */
  gtk_notebook_set_current_page(GTK_NOTEBOOK(_g.notebook),
                                _page_from_name(dt_conf_get_string_const(SUPERVISOR_CONF_PAGE)));
  g_signal_connect(_g.notebook, "switch-page", G_CALLBACK(_on_page_changed), NULL);
  _full_reload();
  _g.timer_id = g_timeout_add(POLL_INTERVAL_MS, _poll, NULL);
}
