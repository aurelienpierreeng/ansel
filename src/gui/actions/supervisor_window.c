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
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/opencl.h"
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
  GtkWidget *notebook;
  GtkWidget *timeline_list;
  GtkWidget *timeline_scroll;
  GtkWidget *grouped_list;
  GtkWidget *mem_box;     // vbox holding the memory view content
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
  _wire_hash_label(header);

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
  g_object_set_data(G_OBJECT(vbox), "header", header); // so callers can update the label
  return vbox;
}

static gchar *_header_markup(const dt_sv_logged_event_t *ev)
{
  gchar *hx = _hashhex(ev->hash);
  gchar *e_op = g_markup_escape_text(ev->op, -1);
  gchar *e_dom = g_markup_escape_text(ev->domain, -1);
  gchar *e_thr = g_markup_escape_text(ev->thread, -1);
  gchar *e_mn = g_markup_escape_text(ev->mnemonic[0] ? ev->mnemonic : "-", -1);
  gchar *out = g_strdup_printf(
      "<tt>%9.3f</tt>  <b>%-7s</b>  <span foreground=\"%s\">%-10s</span>  <b>%-16s</b>  "
      "<a href=\"%s\"><tt>%s</tt></a>  <span size=\"small\"><i>%s</i></span>",
      ev->ts, e_op, _domain_color(ev->domain), e_dom, e_mn, hx, hx, e_thr);
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

// A clickable memory item (markup carries the navigation link). When `trailing`
// is non-NULL it is packed at the right edge as a separate label, so the trailing
// column (e.g. vRAM) lines up vertically across rows.
static void _add_mem_item(GtkWidget *box, const char *markup, const char *trailing)
{
  GtkWidget *lbl = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(lbl), markup);
  gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
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
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
  }
  else
  {
    gtk_widget_set_margin_start(lbl, 12);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
  }
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

static gint _cmp_image_imgid(gconstpointer a, gconstpointer b)
{
  const dt_image_cache_stats_entry_t *x = a, *y = b;
  return x->imgid - y->imgid; // ascending (entries are all the same size)
}

static void _rebuild_memory(void)
{
  if(!_g.mem_box) return;
  _clear_list(_g.mem_box);

  // Pipeline cache
  size_t cur = 0, max = 0;
  dt_dev_pixelpipe_cache_get_usage(darktable.pixelpipe_cache, &cur, &max);
  GArray *pe = dt_dev_pixelpipe_cache_get_entries_stats(darktable.pixelpipe_cache);

  const gboolean gpu = dt_opencl_is_enabled();
  size_t vram_used = 0;
  for(guint i = 0; i < pe->len; i++) vram_used += g_array_index(pe, dt_pixel_cache_stats_entry_t, i).cl_bytes;

  gchar *ptitle = g_strdup_printf(gpu ? _("Pipeline cache (RAM) — %u items") : _("Pipeline cache — %u items"),
                                  pe->len);
  _add_usage_bar(_g.mem_box, ptitle, cur, max);
  g_free(ptitle);
  if(gpu)
    _add_usage_bar(_g.mem_box, _("Pipeline cache (vRAM)"), vram_used,
                   dt_dev_pixelpipe_cache_get_vram_total());

  g_array_sort(pe, _cmp_pixel_size);
  for(guint i = 0; i < pe->len && i < MEMORY_MAX_ROWS; i++)
  {
    const dt_pixel_cache_stats_entry_t *e = &g_array_index(pe, dt_pixel_cache_stats_entry_t, i);
    gchar *hx = _hashhex(e->hash);
    gchar *name = g_markup_escape_text(e->name[0] ? e->name : "-", -1);
    // fixed-width monospace trailing column so the vRAM figures line up vertically
    gchar *vram = (gpu && e->cl_count > 0)
                      ? g_strdup_printf("<tt>+%8.2f MiB vRAM (%2d buf)</tt>", e->cl_bytes / 1048576.0,
                                        e->cl_count)
                      : NULL;
    gchar *m = g_strdup_printf("<a href=\"%s\"><tt>%s</tt></a>  %.2f MiB  refs=%d hits=%d  <i>%s</i>", hx, hx,
                               e->size / 1048576.0, e->refcount, e->hits, name);
    _add_mem_item(_g.mem_box, m, vram);
    g_free(hx);
    g_free(name);
    g_free(vram);
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
    _add_mem_item(_g.mem_box, m, NULL);
    g_free(hx);
    g_free(m);
  }
  g_array_free(me, TRUE);

  // Image cache
  dt_image_cache_get_usage(darktable.image_cache, &cur, &max);
  GArray *ie = dt_image_cache_get_entries_stats(darktable.image_cache);
  gchar *ititle = g_strdup_printf(_("Image cache — %u items"), ie->len);
  _add_usage_bar(_g.mem_box, ititle, cur, max);
  g_free(ititle);
  g_array_sort(ie, _cmp_image_imgid);
  for(guint i = 0; i < ie->len && i < MEMORY_MAX_ROWS; i++)
  {
    const dt_image_cache_stats_entry_t *e = &g_array_index(ie, dt_image_cache_stats_entry_t, i);
    gchar *hx = _hashhex(dt_supervisor_image_key(e->imgid));
    gchar *fn = g_markup_escape_text(e->filename[0] ? e->filename : "-", -1);
    gchar *m = g_strdup_printf("<a href=\"%s\">image #%d</a>  <i>%s</i>", hx, e->imgid, fn);
    _add_mem_item(_g.mem_box, m, NULL);
    g_free(hx);
    g_free(fn);
    g_free(m);
  }
  g_array_free(ie, TRUE);

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
  if((_g.tick % MEMORY_REFRESH_TICKS) == 0 && page == _g.page_memory) _rebuild_memory();

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

static void _on_groupby_changed(GtkComboBox *c, gpointer u)
{
  _rebuild_grouped_now(); // regroup everything under the new key
}

static void _on_record_toggled(GtkToggleButton *t, gpointer u)
{
  dt_supervisor_set_recording(gtk_toggle_button_get_active(t));
}

static void _on_page_changed(GtkNotebook *nb, GtkWidget *page, guint page_num, gpointer u)
{
  if((int)page_num == _g.page_grouped)
    _grouped_catch_up(); // fold in whatever arrived while the page was hidden
  else if((int)page_num == _g.page_memory)
    _rebuild_memory();
  else if((int)page_num == _g.page_search)
    _run_search(gtk_entry_get_text(GTK_ENTRY(_g.search_entry)));
}

static void _on_destroy(GtkWidget *w, gpointer u)
{
  if(_g.timer_id) g_source_remove(_g.timer_id);
  dt_supervisor_set_recording(FALSE); // stop capturing once the viewer is gone
  if(_g.decl_map) g_hash_table_destroy(_g.decl_map);
  if(_g.group_map) g_hash_table_destroy(_g.group_map);
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
  g_signal_connect(_g.notebook, "switch-page", G_CALLBACK(_on_page_changed), NULL);
  gtk_box_pack_start(GTK_BOX(vbox), _g.notebook, TRUE, TRUE, 0);

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

  _g.mem_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_container_set_border_width(GTK_CONTAINER(_g.mem_box), 6);
  GtkWidget *sw3 = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(sw3), _g.mem_box);
  _g.page_memory = gtk_notebook_append_page(GTK_NOTEBOOK(_g.notebook), sw3, gtk_label_new(_("Memory")));

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
  _full_reload();
  _g.timer_id = g_timeout_add(POLL_INTERVAL_MS, _poll, NULL);
}
