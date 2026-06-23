/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.

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
#include "common/darktable.h"
#include "common/mipmap_cache.h"
#include "develop/pixelpipe_cache.h"
#include "develop/supervisor.h"
#include "gui/gtk.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

// Cap on how many timeline rows we keep live (older ones are trimmed). The full
// log lives in the supervisor; this only bounds GTK widget pressure.
#define TIMELINE_MAX_ROWS 4000
#define POLL_INTERVAL_MS 300
#define MEMORY_MAX_ROWS 500   // cap items shown per cache in the memory view
#define MEMORY_REFRESH_TICKS 4 // refresh the memory view every Nth poll tick when visible

static struct
{
  GtkWidget *window;
  GtkWidget *stack;
  GtkWidget *timeline_list;
  GtkWidget *timeline_scroll;
  GtkWidget *grouped_list;
  GtkWidget *count_label;
  GtkWidget *groupby;     // GtkComboBoxText: domain / thread / op
  GtkWidget *mem_box;     // vbox holding the memory view content
  GHashTable *decl_map;   // hex string -> GtkListBoxRow* (the create/declaration row)
  uint64_t last_seq;      // highest captured seq already displayed in the timeline
  guint timer_id;
  guint tick;             // poll tick counter (for throttling the memory view)
  gboolean grouped_dirty; // grouped view needs a rebuild when next shown
  gboolean follow_tail;   // auto-scroll requested by the last append
} _g = { 0 };

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

// Clicking a hash jumps to the declaration (create event) of that object.
static gboolean _on_link(GtkLabel *label, gchar *uri, gpointer user_data)
{
  if(!_g.decl_map) return TRUE;
  GtkWidget *row = (GtkWidget *)g_hash_table_lookup(_g.decl_map, uri);
  if(row)
  {
    gtk_stack_set_visible_child_name(GTK_STACK(_g.stack), "timeline");
    GtkWidget *toggle = (GtkWidget *)g_object_get_data(G_OBJECT(row), "toggle");
    if(toggle) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), TRUE); // expand
    gtk_list_box_select_row(GTK_LIST_BOX(_g.timeline_list), GTK_LIST_BOX_ROW(row));
    gtk_widget_grab_focus(row); // scrolls the row into view
  }
  return TRUE; // handled: do not try to open as an URL
}

static GtkWidget *_event_body_from(const char *json, GArray *links);

static void _on_toggle(GtkToggleButton *t, gpointer u)
{
  const gboolean a = gtk_toggle_button_get_active(t);
  GtkWidget *rev = (GtkWidget *)g_object_get_data(G_OBJECT(t), "revealer");
  GtkWidget *arrow = (GtkWidget *)g_object_get_data(G_OBJECT(t), "arrow");

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
  if(arrow)
    gtk_image_set_from_icon_name(GTK_IMAGE(arrow), a ? "pan-down-symbolic" : "pan-end-symbolic",
                                 GTK_ICON_SIZE_BUTTON);
}

// A collapsible block: [arrow toggle][header label] + a revealer holding `body`.
// The header is a separate label (NOT the toggle), so its hash links stay
// clickable. Returns a vbox; the toggle button is stored as data "toggle".
static GtkWidget *_collapsible(const char *header_markup, GtkWidget *body, const gboolean expanded)
{
  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  GtkWidget *toggle = gtk_toggle_button_new();
  gtk_button_set_relief(GTK_BUTTON(toggle), GTK_RELIEF_NONE);
  gtk_widget_set_focus_on_click(toggle, FALSE);
  GtkWidget *arrow = gtk_image_new_from_icon_name(expanded ? "pan-down-symbolic" : "pan-end-symbolic",
                                                  GTK_ICON_SIZE_BUTTON);
  gtk_container_add(GTK_CONTAINER(toggle), arrow);

  GtkWidget *header = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(header), header_markup);
  gtk_label_set_xalign(GTK_LABEL(header), 0.0);
  gtk_label_set_track_visited_links(GTK_LABEL(header), FALSE);
  g_signal_connect(header, "activate-link", G_CALLBACK(_on_link), NULL);

  GtkWidget *revealer = gtk_revealer_new();
  gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), expanded);
  if(body) gtk_container_add(GTK_CONTAINER(revealer), body);

  g_object_set_data(G_OBJECT(toggle), "revealer", revealer);
  g_object_set_data(G_OBJECT(toggle), "arrow", arrow);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), expanded);
  g_signal_connect(toggle, "toggled", G_CALLBACK(_on_toggle), NULL);

  gtk_box_pack_start(GTK_BOX(hbox), toggle, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), header, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), revealer, FALSE, FALSE, 0);

  g_object_set_data(G_OBJECT(vbox), "toggle", toggle);
  return vbox;
}

static gchar *_header_markup(const dt_sv_logged_event_t *ev)
{
  gchar *hx = _hashhex(ev->hash);
  gchar *e_op = g_markup_escape_text(ev->op, -1);
  gchar *e_dom = g_markup_escape_text(ev->domain, -1);
  gchar *e_thr = g_markup_escape_text(ev->thread, -1);
  gchar *out = g_strdup_printf(
      "<tt>%9.3f</tt>  <b>%-7s</b>  <span foreground=\"%s\">%-10s</span>  "
      "<a href=\"%s\"><tt>%s</tt></a>  <span size=\"small\"><i>%s</i></span>",
      ev->ts, e_op, _domain_color(ev->domain), e_dom, hx, hx, e_thr);
  g_free(hx);
  g_free(e_op);
  g_free(e_dom);
  g_free(e_thr);
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
    gtk_label_set_track_visited_links(GTK_LABEL(lw), FALSE);
    g_signal_connect(lw, "activate-link", G_CALLBACK(_on_link), NULL);
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

static void _timeline_trim(void)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(_g.timeline_list));
  int excess = (int)g_list_length(children) - TIMELINE_MAX_ROWS;
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

static void _rebuild_grouped_now(void)
{
  _clear_list(_g.grouped_list);

  GPtrArray *events = dt_supervisor_events_snapshot();
  gchar *by = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(_g.groupby));
  const char *byk = by ? by : "domain";

  GHashTable *bodies = g_hash_table_new(g_str_hash, g_str_equal); // key -> body vbox
  GHashTable *counts = g_hash_table_new(g_str_hash, g_str_equal); // key -> count (as int)
  GPtrArray *order = g_ptr_array_new();

  for(guint i = 0; i < events->len; i++)
  {
    const dt_sv_logged_event_t *ev = g_ptr_array_index(events, i);
    const char *k = _group_key(ev, byk);
    GtkWidget *body = g_hash_table_lookup(bodies, k);
    if(!body)
    {
      body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      gtk_widget_set_margin_start(body, 12);
      g_hash_table_insert(bodies, (gpointer)k, body);
      g_ptr_array_add(order, (gpointer)k);
    }
    gtk_box_pack_start(GTK_BOX(body), _event_widget(ev), FALSE, FALSE, 0);
    g_hash_table_insert(counts, (gpointer)k,
                        GINT_TO_POINTER(GPOINTER_TO_INT(g_hash_table_lookup(counts, k)) + 1));
  }

  for(guint i = 0; i < order->len; i++)
  {
    const char *k = g_ptr_array_index(order, i);
    GtkWidget *body = g_hash_table_lookup(bodies, k);
    const int n = GPOINTER_TO_INT(g_hash_table_lookup(counts, k));
    gchar *e = g_markup_escape_text(k && *k ? k : "(none)", -1);
    gchar *hm = g_strdup_printf("<b>%s</b>  <span size=\"small\">(%d)</span>", e, n);
    gtk_container_add(GTK_CONTAINER(_g.grouped_list), _collapsible(hm, body, FALSE));
    g_free(e);
    g_free(hm);
  }

  g_free(by);
  g_ptr_array_free(order, TRUE);
  g_hash_table_destroy(bodies);
  g_hash_table_destroy(counts);
  dt_supervisor_events_free(events);
  gtk_widget_show_all(_g.grouped_list);
}

// ---- Memory view -------------------------------------------------------------

static void _add_usage_bar(GtkWidget *box, const char *title, const size_t cur, const size_t max)
{
  GtkWidget *hdr = gtk_label_new(NULL);
  gchar *t = g_markup_printf_escaped("<b>%s</b>", title);
  gtk_label_set_markup(GTK_LABEL(hdr), t);
  gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
  gtk_widget_set_margin_top(hdr, 8);
  g_free(t);
  gtk_box_pack_start(GTK_BOX(box), hdr, FALSE, FALSE, 0);

  const double frac = max ? CLAMP((double)cur / (double)max, 0.0, 1.0) : 0.0;
  GtkWidget *bar = gtk_progress_bar_new();
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), frac);
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), TRUE);
  gchar *txt = g_strdup_printf("%.1f / %.1f MiB  (%.0f%%)", cur / 1048576.0, max / 1048576.0, frac * 100.0);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar), txt);
  g_free(txt);
  gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 0);
}

// A single clickable memory item (markup carries the navigation link).
static void _add_mem_item(GtkWidget *box, const char *markup)
{
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(lbl), markup);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
  gtk_label_set_track_visited_links(GTK_LABEL(lbl), FALSE);
  gtk_widget_set_margin_start(lbl, 12);
  g_signal_connect(lbl, "activate-link", G_CALLBACK(_on_link), NULL);
  gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
}

static gint _cmp_pixel_size(gconstpointer a, gconstpointer b)
{
  const dt_pixel_cache_stats_entry_t *x = a, *y = b;
  return (y->size > x->size) - (y->size < x->size); // descending
}

static gint _cmp_mip_size(gconstpointer a, gconstpointer b)
{
  const dt_mipmap_cache_stats_entry_t *x = a, *y = b;
  return (y->size > x->size) - (y->size < x->size); // descending
}

static void _rebuild_memory(void)
{
  if(!_g.mem_box) return;
  _clear_list(_g.mem_box);

  // Pipeline cache
  size_t cur = 0, max = 0;
  dt_dev_pixelpipe_cache_get_usage(darktable.pixelpipe_cache, &cur, &max);
  GArray *pe = dt_dev_pixelpipe_cache_get_entries_stats(darktable.pixelpipe_cache);
  gchar *ptitle = g_strdup_printf(_("Pipeline cache — %u items"), pe->len);
  _add_usage_bar(_g.mem_box, ptitle, cur, max);
  g_free(ptitle);
  g_array_sort(pe, _cmp_pixel_size);
  for(guint i = 0; i < pe->len && i < MEMORY_MAX_ROWS; i++)
  {
    const dt_pixel_cache_stats_entry_t *e = &g_array_index(pe, dt_pixel_cache_stats_entry_t, i);
    gchar *hx = _hashhex(e->hash);
    gchar *name = g_markup_escape_text(e->name[0] ? e->name : "-", -1);
    gchar *m = g_strdup_printf("<a href=\"%s\"><tt>%s</tt></a>  %.2f MiB  refs=%d hits=%d  <i>%s</i>", hx, hx,
                               e->size / 1048576.0, e->refcount, e->hits, name);
    _add_mem_item(_g.mem_box, m);
    g_free(hx);
    g_free(name);
    g_free(m);
  }
  g_array_free(pe, TRUE);

  // Mipmap cache
  dt_mipmap_cache_get_usage(darktable.mipmap_cache, &cur, &max);
  GArray *me = dt_mipmap_cache_get_entries_stats(darktable.mipmap_cache);
  gchar *mtitle = g_strdup_printf(_("Mipmap cache — %u items"), me->len);
  _add_usage_bar(_g.mem_box, mtitle, cur, max);
  g_free(mtitle);
  g_array_sort(me, _cmp_mip_size);
  for(guint i = 0; i < me->len && i < MEMORY_MAX_ROWS; i++)
  {
    const dt_mipmap_cache_stats_entry_t *e = &g_array_index(me, dt_mipmap_cache_stats_entry_t, i);
    gchar *hx = _hashhex(dt_supervisor_mipmap_key(e->imgid, e->mip));
    gchar *m = g_strdup_printf("<a href=\"%s\">image #%d · mip %d</a>  %.2f MiB", hx, e->imgid, e->mip,
                               e->size / 1048576.0);
    _add_mem_item(_g.mem_box, m);
    g_free(hx);
    g_free(m);
  }
  g_array_free(me, TRUE);

  gtk_widget_show_all(_g.mem_box);
}

static gboolean _scroll_bottom_idle(gpointer u)
{
  if(!_g.timeline_scroll) return G_SOURCE_REMOVE;
  GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_g.timeline_scroll));
  if(adj) gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
  return G_SOURCE_REMOVE;
}

static gboolean _poll(gpointer u)
{
  if(!_g.window) return G_SOURCE_REMOVE;

  // The memory view reflects live cache state (changes even without new events),
  // so refresh it on a throttled cadence whenever its page is visible.
  _g.tick++;
  if((_g.tick % MEMORY_REFRESH_TICKS) == 0
     && !g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(_g.stack)), "memory"))
    _rebuild_memory();

  uint64_t newlast = _g.last_seq;
  GPtrArray *evs = dt_supervisor_events_snapshot_since(_g.last_seq, &newlast);
  if(evs->len)
  {
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(_g.timeline_scroll));
    const gboolean at_bottom = !adj
        || gtk_adjustment_get_value(adj)
               >= gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) - 4.0;

    for(guint i = 0; i < evs->len; i++) _timeline_append(g_ptr_array_index(evs, i));
    _g.last_seq = newlast;
    _timeline_trim();
    _update_count();
    // The timeline updates live; the grouped view is rebuilt on demand (Refresh,
    // group-by change, or when its page is shown) to avoid rebuilding thousands
    // of widgets every tick during heavy streaming.
    _g.grouped_dirty = TRUE;
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
  _g.grouped_dirty = FALSE;
  _rebuild_memory();
}

static void _on_refresh(GtkButton *b, gpointer u) { _full_reload(); }

static void _on_clear(GtkButton *b, gpointer u)
{
  dt_supervisor_events_clear();
  _full_reload();
}

static void _on_groupby_changed(GtkComboBox *c, gpointer u)
{
  _rebuild_grouped_now();
  _g.grouped_dirty = FALSE;
}

static void _on_record_toggled(GtkToggleButton *t, gpointer u)
{
  dt_supervisor_set_recording(gtk_toggle_button_get_active(t));
}

static void _on_page_changed(GObject *o, GParamSpec *p, gpointer u)
{
  const char *vis = gtk_stack_get_visible_child_name(GTK_STACK(_g.stack));
  if(_g.grouped_dirty && !g_strcmp0(vis, "grouped"))
  {
    _rebuild_grouped_now();
    _g.grouped_dirty = FALSE;
  }
  else if(!g_strcmp0(vis, "memory"))
    _rebuild_memory();
}

static void _on_destroy(GtkWidget *w, gpointer u)
{
  if(_g.timer_id) g_source_remove(_g.timer_id);
  dt_supervisor_set_recording(FALSE); // stop capturing once the viewer is gone
  if(_g.decl_map) g_hash_table_destroy(_g.decl_map);
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

  _g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(_g.window), _("Event supervisor"));
  gtk_window_set_default_size(GTK_WINDOW(_g.window), 1000, 640);
  gtk_window_set_transient_for(GTK_WINDOW(_g.window), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  g_signal_connect(_g.window, "destroy", G_CALLBACK(_on_destroy), NULL);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(_g.window), vbox);

  // toolbar
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);

  GtkWidget *record = gtk_check_button_new_with_label(_("Record"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(record), TRUE);
  dt_supervisor_set_recording(TRUE); // start capturing on open
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
  gtk_combo_box_set_active(GTK_COMBO_BOX(_g.groupby), 0);
  g_signal_connect(_g.groupby, "changed", G_CALLBACK(_on_groupby_changed), NULL);
  gtk_box_pack_start(GTK_BOX(bar), _g.groupby, FALSE, FALSE, 0);

  _g.count_label = gtk_label_new("");
  gtk_box_pack_end(GTK_BOX(bar), _g.count_label, FALSE, FALSE, 0);

  // stack switcher + stack
  GtkWidget *switcher = gtk_stack_switcher_new();
  gtk_box_pack_start(GTK_BOX(vbox), switcher, FALSE, FALSE, 0);
  _g.stack = gtk_stack_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(_g.stack));
  g_signal_connect(_g.stack, "notify::visible-child", G_CALLBACK(_on_page_changed), NULL);
  gtk_box_pack_start(GTK_BOX(vbox), _g.stack, TRUE, TRUE, 0);

  _g.timeline_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_g.timeline_list), GTK_SELECTION_SINGLE);
  _g.timeline_scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(_g.timeline_scroll), _g.timeline_list);
  gtk_stack_add_titled(GTK_STACK(_g.stack), _g.timeline_scroll, "timeline", _("Timeline"));

  _g.grouped_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(_g.grouped_list), GTK_SELECTION_NONE);
  GtkWidget *sw2 = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sw2), _g.grouped_list);
  gtk_stack_add_titled(GTK_STACK(_g.stack), sw2, "grouped", _("Grouped"));

  _g.mem_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_container_set_border_width(GTK_CONTAINER(_g.mem_box), 6);
  GtkWidget *sw3 = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sw3), _g.mem_box);
  gtk_stack_add_titled(GTK_STACK(_g.stack), sw3, "memory", _("Memory"));

  gtk_widget_show_all(_g.window);
  _full_reload();
  _g.timer_id = g_timeout_add(POLL_INTERVAL_MS, _poll, NULL);
}
