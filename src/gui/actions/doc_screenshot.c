/*
    This file is part of Ansel.
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

#include "gui/actions/doc_screenshot.h"

#include "common/conf.h"          // dt_conf_get_string(), dt_conf_set_string()
#include "common/l10n.h"          // dt_l10n_get_global(), dt_l10n_get_current_code()
#include "gui/application.h"      // dt_gui_get_ui(), dt_gui_main_window()
#include "gui/window_manager.h"   // dt_ui_t, dt_ui_center_base(), DT_UI_PANEL_*
#include "system/macros.h"        // IS_NULL_PTR
#include "system/mem_alloc.h"     // dt_free
#include "widgets/bauhaus.h"      // DT_IS_BAUHAUS_WIDGET(), dt_bauhaus_widget_get_label()

#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>

/** Characters kept as-is in a generated file name. Everything else -- spaces, accents,
 * slashes, parentheses -- is replaced by an underscore by g_strcanon(). Applies only to
 * names derived from a row label: a path coming from the map is the documentation's own
 * and is used verbatim. */
#define DOC_SCREENSHOT_FILENAME_ALLOWED                                                                           \
  "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_."

/** Longest side of the preview shown for the selected row, in logical pixels. */
#define DOC_SCREENSHOT_PREVIEW_SIZE 360

/** Name of the widget-to-page map, looked up at the root of the destination folder. */
#define DOC_SCREENSHOT_MAP_FILE "screenshots.map"

/** Conf key holding the labels of the checked rows, joined by
 * DOC_SCREENSHOT_SEPARATOR.
 *
 * Deliberately NOT a confgen key and absent from Preferences: this is developer-mode state,
 * meaningless to anyone who never passed --doc. It is what makes a documentation pass
 * reproducible -- the same selection comes back for the next language instead of being
 * rebuilt from memory.
 *
 * The separator mirrors DT_FOLDER_SURVEY_STYLES_SEPARATOR for the same reason: a widget
 * label may contain any printable character, so only a control character can neither appear
 * in one nor break the one-value-per-line conf file. The same separator joins the labels of
 * a row's ancestors into the chain that identifies it across a refresh.
 *
 * Rows are remembered by label, the same key the map uses. Two rows sharing a label -- two
 * anonymous GtkBox rows, say -- therefore come back checked together; the rows that matter
 * to a documentation pass are the mapped ones, whose labels are unique by construction.
 */
#define DOC_SCREENSHOT_SELECTION_CONF_KEY "doc_screenshot/selection"
#define DOC_SCREENSHOT_SEPARATOR "\x1f"

/** What the three selection buttons ask of the model walk. */
typedef enum _doc_screenshot_select_t
{
  DOC_SCREENSHOT_SELECT_NONE = 0, ///< clear everything, displayed or not
  DOC_SCREENSHOT_SELECT_ALL,      ///< check every displayed row
  DOC_SCREENSHOT_SELECT_MAPPED    ///< check exactly the displayed rows the map names
} _doc_screenshot_select_t;

/** Columns of the widget tree.
 *
 * COL_TARGET is a G_TYPE_OBJECT, not a pointer: the store then references the widget it
 * holds, which is what keeps the row valid when a view change destroys the modules of the
 * view being left under an open window. A destroyed widget is merely unmapped afterwards,
 * and _save_widget_as_image() reports it as "cannot be drawn".
 *
 * A NULL target below the top level marks a lazy-expansion placeholder -- see _append_row().
 */
typedef enum _doc_screenshot_column_t
{
  COL_CHECKED = 0, ///< the capture check box
  COL_LABEL,       ///< displayed name, also the key into the widget-to-page map
  COL_PAGE,        ///< illustration this widget stands for, from the map. "" when unmapped
  COL_TARGET,      ///< widget this row captures. NULL on a section header and on a placeholder
  COL_CAPTURABLE,  ///< there is a target, so a check box is worth showing at all
  COL_ENABLED,     ///< the target is currently displayed. Always TRUE on a section header
  COL_COUNT
} _doc_screenshot_column_t;

/** The whole state of the feature: one window at a time, plus what the argument parser
 * handed over. Everything here is GUI-thread-only, like the menu entry that opens it. */
static struct
{
  GtkWidget *window;
  GtkWidget *view;     ///< the tree view
  GtkTreeStore *store; ///< its model, owned by the view
  GtkWidget *folder;   ///< destination folder chooser
  GtkWidget *status;   ///< result of the last capture
  GtkWidget *preview;  ///< what the selected row actually captures
  GtkWidget *geometry; ///< its size, under the preview
  GHashTable *names;   ///< GtkWidget* -> the name the application knows it by
  GHashTable *pages;   ///< row label -> path of the illustration, relative to the destination
  GHashTable *selected;///< set of checked row labels, mirrored to conf on every change
  gchar *directory;    ///< documentation root given after --doc, or NULL
  gboolean enabled;    ///< --doc was passed on the command line
} _g = { 0 };

/** Carried through the model walk of one capture run. */
typedef struct _doc_screenshot_capture_t
{
  const char *folder;
  const char *code; ///< language code inserted before the extension
  GHashTable *used; ///< base file name -> how many rows already claimed it
  int saved;
  int skipped; ///< checked, but its widget is not on screen right now
  int failed;
} _doc_screenshot_capture_t;


void dt_gui_doc_screenshot_enable(void)
{
  _g.enabled = TRUE;
}


gboolean dt_gui_doc_screenshot_enabled(void)
{
  return _g.enabled;
}


void dt_gui_doc_screenshot_set_directory(const char *path)
{
  dt_free(_g.directory);
  _g.directory = g_strdup(path);
}


/** Read back the selection of the previous run. */
static void _load_selection(void)
{
  gchar *conf = dt_conf_get_string(DOC_SCREENSHOT_SELECTION_CONF_KEY);
  gchar **labels
      = g_strsplit(!IS_NULL_PTR(conf) && conf[0] ? conf : "", DOC_SCREENSHOT_SEPARATOR, -1);
  dt_free(conf);

  for(gchar **label = labels; *label; label++)
    if((*label)[0] != '\0') g_hash_table_add(_g.selected, g_strdup(*label));

  g_strfreev(labels);
}


/** Mirror the selection to conf. Called on every change rather than at capture time, so a
 * session that ends without capturing -- or crashes -- still leaves the work behind. */
static void _save_selection(void)
{
  // Sorted: a hash table enumerates in no defined order, and an unsorted value would rewrite
  // the conf line on every run for a selection that never changed.
  GList *labels = g_list_sort(g_hash_table_get_keys(_g.selected), (GCompareFunc)g_strcmp0);
  GString *value = g_string_new(NULL);
  for(GList *item = labels; item; item = g_list_next(item))
  {
    if(value->len) g_string_append(value, DOC_SCREENSHOT_SEPARATOR);
    g_string_append(value, (const char *)item->data);
  }
  g_list_free(labels);

  dt_conf_set_string(DOC_SCREENSHOT_SELECTION_CONF_KEY, value->str);
  g_string_free(value, TRUE);
}


/** Encode the rendered surface, choosing the encoder from the file extension.
 *
 * PNG goes through cairo: lossless, which is what text-heavy UI screenshots want, and the
 * default for anything the map does not name. Everything else goes through gdk-pixbuf,
 * because a documentation tree that already ships .jpg illustrations has to be refreshable
 * in place -- a .png dropped beside them would not replace them.
 *
 * @return 0 on success, 2 if the file cannot be written.
 */
static int _write_surface(cairo_surface_t *surface, const char *filename)
{
  const char *extension = strrchr(filename, '.');
  if(IS_NULL_PTR(extension) || !g_ascii_strcasecmp(extension, ".png"))
    return (cairo_surface_write_to_png(surface, filename) == CAIRO_STATUS_SUCCESS) ? 0 : 2;

  GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, cairo_image_surface_get_width(surface),
                                                  cairo_image_surface_get_height(surface));
  // gdk-pixbuf spells it "jpeg" where the documentation tree spells it ".jpg". JPEG drops
  // the alpha channel, which costs nothing here: the capture is painted over an opaque
  // background before anything else is drawn.
  const gboolean is_jpeg = !g_ascii_strcasecmp(extension, ".jpg") || !g_ascii_strcasecmp(extension, ".jpeg");
  const gboolean written = is_jpeg
                               ? gdk_pixbuf_save(pixbuf, filename, "jpeg", NULL, "quality", "95", NULL)
                               : gdk_pixbuf_save(pixbuf, filename, extension + 1, NULL, NULL);
  g_object_unref(pixbuf);
  return written ? 0 : 2;
}


/** Draw one widget into an offscreen surface, at its on-screen size.
 *
 * The widget is re-drawn rather than grabbed from the screen:
 * this panel sits on top of the application, and a screen grab would photograph it (and
 * whatever else overlaps). gtk_widget_draw() replays the widget's own draw handlers,
 * children included, at its current allocation, so the file holds exactly what the user
 * sees -- occluded or not.
 *
 * The main window's background is painted first because containers mostly declare no
 * background of their own: without it every gap between children comes out transparent.
 *
 * Note this deliberately does NOT call gtk_widget_show_all() on the widget first. That is
 * the right move for a widget built offscreen and never displayed; here every target is a
 * live widget inside the main window, and showing it recursively would reveal every child
 * the application deliberately hides -- collapsed module bodies, conditional buttons, mask
 * indicators -- and would not undo itself once the screenshot is taken.
 *
 * @param widget the widget to draw. Must be mapped: an unmapped one has no meaningful
 *               allocation, and a widget destroyed since the row was built is unmapped.
 * @return the surface, caller-owned, or NULL when the widget cannot be drawn.
 */
static cairo_surface_t *_render_widget(GtkWidget *widget)
{
  GtkAllocation alloc;
  gtk_widget_get_allocation(widget, &alloc);
  if(!gtk_widget_get_mapped(widget) || alloc.width < 1 || alloc.height < 1) return NULL;

  // GTK reports the allocation in logical pixels: carry the monitor's integer scale factor
  // so the capture stays as sharp as the on-screen rendering on a HiDPI screen.
  const int scale = gtk_widget_get_scale_factor(widget);
  cairo_surface_t *surface
      = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, alloc.width * scale, alloc.height * scale);
  cairo_t *cr = cairo_create(surface);
  cairo_scale(cr, (double)scale, (double)scale);

  gtk_render_background(gtk_widget_get_style_context(dt_gui_main_window()), cr, 0., 0., (double)alloc.width,
                        (double)alloc.height);
  gtk_widget_draw(widget, cr);
  cairo_destroy(cr);

  return surface;
}


/** Render one widget into an image file, creating the intermediate directories.
 *
 * @return 0 on success, 1 if the widget cannot be drawn, 2 if the file cannot be written.
 */
static int _save_widget_as_image(GtkWidget *widget, const char *filename)
{
  cairo_surface_t *surface = _render_widget(widget);
  if(IS_NULL_PTR(surface)) return 1;

  // A mapped path points into the documentation tree, whose folders need not exist yet.
  gchar *directory = g_path_get_dirname(filename);
  g_mkdir_with_parents(directory, 0755);
  dt_free(directory);

  const int result = _write_surface(surface, filename);
  cairo_surface_destroy(surface);
  return result;
}


/** Does this widget, or anything under it, show text?
 *
 * A capture of a purely graphical element -- an icon button, a colour swatch, a separator --
 * is the same picture in every language, so the language code has no business in its file
 * name: it would scatter identical copies through the documentation tree, one per language.
 *
 * Conservative by construction: whatever cannot be introspected counts as text. The other
 * way round is the expensive mistake -- every language would then write the same file, and
 * only the last one captured would survive.
 *
 * The icon buttons this mainly exists for are correctly seen as text-free: GtkDarktableButton
 * derives from GtkButton and paints its icon itself, so it owns no label child to find.
 */
static gboolean _widget_has_text(GtkWidget *widget)
{
  // Widgets that own text, plus the custom drawing surfaces that paint their own and say
  // nothing about it through any API -- bauhaus controls among them, whose numeric value
  // carries a localised decimal separator even when their label is empty.
  if(DT_IS_BAUHAUS_WIDGET(widget) || GTK_IS_ENTRY(widget) || GTK_IS_TEXT_VIEW(widget)
     || GTK_IS_DRAWING_AREA(widget))
    return TRUE;

  if(GTK_IS_LABEL(widget)) return gtk_label_get_text(GTK_LABEL(widget))[0] != '\0';

  if(!GTK_IS_CONTAINER(widget)) return FALSE;

  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  gboolean text = FALSE;
  for(GList *item = children; item && !text; item = g_list_next(item))
    text = _widget_has_text(GTK_WIDGET(item->data));
  g_list_free(children);

  return text;
}


/** Insert the language code before the extension: `modules/exposure.jpg` for "fr" becomes
 * `modules/exposure.fr.jpg`. The extension is looked for after the last separator, so a dot
 * in a folder name is not mistaken for one. */
static gchar *_localized_path(const char *relative, const char *code)
{
  const char *base = strrchr(relative, '/');
  const char *extension = strrchr(IS_NULL_PTR(base) ? relative : base, '.');
  if(IS_NULL_PTR(extension)) return g_strdup_printf("%s.%s", relative, code);

  return g_strdup_printf("%.*s.%s%s", (int)(extension - relative), relative, code, extension);
}


/** Read the widget-to-page map at the root of the destination folder.
 *
 * One entry per line, `#` starts a comment:
 *
 *     Exposure    = content/modules/exposure/exposure.jpg
 *     Left panel  = content/interface/left-panel.png
 *
 * The key is the row label as the tree displays it; the value is where the illustration
 * lives, relative to the destination folder, extension included -- the documentation's own
 * extension, which is what lets its .jpg files be refreshed rather than shadowed.
 *
 * A missing map is not a failure: the panel then writes PNGs named after the rows.
 */
static void _load_pages(void)
{
  g_hash_table_remove_all(_g.pages);

  gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(_g.folder));
  if(IS_NULL_PTR(folder)) return;

  gchar *file = g_build_filename(folder, DOC_SCREENSHOT_MAP_FILE, NULL);
  gchar *contents = NULL;
  const gboolean loaded = g_file_get_contents(file, &contents, NULL, NULL);
  dt_free(file);
  dt_free(folder);
  if(!loaded) return;

  gchar **lines = g_strsplit(contents, "\n", -1);
  for(int line = 0; lines[line]; line++)
  {
    gchar *entry = g_strstrip(lines[line]);
    gchar *separator = strchr(entry, '=');
    if(entry[0] == '#' || IS_NULL_PTR(separator)) continue;

    *separator = '\0';
    // The table owns both halves.
    g_hash_table_insert(_g.pages, g_strdup(g_strstrip(entry)), g_strdup(g_strstrip(separator + 1)));
  }
  g_strfreev(lines);
  dt_free(contents);
}


/** First text found under this widget, or NULL if there is none.
 *
 * Only a hint, used to tell anonymous containers apart: a row reading `GtkBox` says nothing,
 * and the tree grows dozens of them; `GtkBox: Add to library` names the thing the reader is
 * actually looking at. Borrowed from the widget that owns it, so it is copied on sight.
 */
static const char *_first_text(GtkWidget *widget)
{
  const char *own = GTK_IS_LABEL(widget)             ? gtk_label_get_text(GTK_LABEL(widget))
                    : DT_IS_BAUHAUS_WIDGET(widget)   ? dt_bauhaus_widget_get_label(widget)
                    : GTK_IS_BUTTON(widget)          ? gtk_button_get_label(GTK_BUTTON(widget))
                                                     : NULL;
  if(!IS_NULL_PTR(own) && own[0] != '\0') return own;
  if(!GTK_IS_CONTAINER(widget)) return NULL;

  GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
  const char *text = NULL;
  for(GList *item = children; item && IS_NULL_PTR(text); item = g_list_next(item))
    text = _first_text(GTK_WIDGET(item->data));
  g_list_free(children);

  return text;
}


/** Walk down a chain of single-child containers that carry no identity of their own.
 *
 * A panel reaches its modules through GtkScrolledWindow > GtkViewport > GtkBox > ..., each
 * holding exactly one child and covering the same pixels as it. One row per link is three
 * rows of noise standing for a single picture. So the row stands for the OUTERMOST widget of
 * the chain -- the biggest, the one worth capturing, padding included -- while its name and
 * its children are taken from the innermost.
 *
 * The descent only ever folds away pure layout -- boxes, grids, scrolled windows, viewports,
 * overlays -- and stops at anything with an identity: a widget the application named (a
 * panel, a module), one carrying its own CSS name from ansel.css, or any other kind of
 * container. Those are things a reader asks for by name, and folding them away would make
 * them unreachable.
 */
static GtkWidget *_unwrap(GtkWidget *widget)
{
  while(GTK_IS_CONTAINER(widget))
  {
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    GtkWidget *only = (!IS_NULL_PTR(children) && IS_NULL_PTR(g_list_next(children)))
                          ? GTK_WIDGET(children->data)
                          : NULL;
    g_list_free(children);
    if(IS_NULL_PTR(only)) break;

    // `only`'s row is the one being dropped, so `only` is what has to be worth dropping:
    // pure layout, with no identity of its own. A GtkButton also holds a single child, and
    // folding it away would delete the very row someone wants to capture.
    const gboolean anonymous = IS_NULL_PTR(g_hash_table_lookup(_g.names, only))
                               && !g_strcmp0(gtk_widget_get_name(only), G_OBJECT_TYPE_NAME(only));
    const gboolean layout = GTK_IS_BOX(only) || GTK_IS_GRID(only) || GTK_IS_SCROLLED_WINDOW(only)
                            || GTK_IS_VIEWPORT(only) || GTK_IS_OVERLAY(only);
    if(!anonymous || !layout) break;

    widget = only;
  }

  return widget;
}


/** Name one widget for the tree, best source first.
 *
 * The registry map answers for everything the application knows by name -- panels and
 * modules -- whichever path the user reached it by. Below that we are walking anonymous
 * toolkit plumbing, where the widget's own text (a label, a button, a bauhaus control) says
 * far more than its type, and the CSS name set by ansel.css says more than nothing.
 * gtk_widget_get_name() falls back to the type name when no name was ever set, which is why
 * it is compared against the type rather than tested for NULL.
 *
 * @return a newly allocated string, never NULL.
 */
static gchar *_widget_label(GtkWidget *widget, const int index)
{
  const char *known = (const char *)g_hash_table_lookup(_g.names, widget);
  if(!IS_NULL_PTR(known)) return g_strdup(known);

  const char *type = G_OBJECT_TYPE_NAME(widget);
  const char *text = GTK_IS_LABEL(widget)           ? gtk_label_get_text(GTK_LABEL(widget))
                     : GTK_IS_BUTTON(widget)        ? gtk_button_get_label(GTK_BUTTON(widget))
                     : DT_IS_BAUHAUS_WIDGET(widget) ? dt_bauhaus_widget_get_label(widget)
                                                    : NULL;
  if(!IS_NULL_PTR(text) && text[0] != '\0') return g_strdup_printf("%s (%s)", text, type);

  const char *name = gtk_widget_get_name(widget);
  if(g_strcmp0(name, type)) return g_strdup_printf("%s (%s)", name, type);

  // Anonymous plumbing. What it contains identifies it far better than its rank among its
  // siblings does; the rank is only there for the ones that contain no text at all.
  const char *hint = _first_text(widget);
  return IS_NULL_PTR(hint) ? g_strdup_printf("%s #%d", type, index)
                           : g_strdup_printf("%s: %s", type, hint);
}


/** Append one capturable row under @p parent, and make it expandable if the widget has
 * children of its own.
 *
 * Children are not built here: a full walk of the main window would materialise thousands
 * of rows the user will never open. Instead a container gets a single placeholder child --
 * a row with no target, which is what gives the tree view its expander arrow -- and
 * _on_row_expanded() swaps it for the real children the first time it is opened.
 *
 * @param label_override the application's own name for this widget, or NULL to derive one.
 */
static void _append_row(GtkTreeIter *parent, GtkWidget *target, const char *label_override, const int index)
{
  if(IS_NULL_PTR(target)) return;

  // The row captures the outer widget but is named and unfolded from the innermost one of
  // its wrapper chain -- see _unwrap(). _on_row_expanded() unwraps the same way, so the
  // children it later lists are the ones this placeholder promises.
  GtkWidget *inner = _unwrap(target);
  gchar *label = IS_NULL_PTR(label_override) ? _widget_label(inner, index) : g_strdup(label_override);
  const char *page = (const char *)g_hash_table_lookup(_g.pages, label);

  GtkTreeIter iter;
  gtk_tree_store_append(_g.store, &iter, parent);
  // Checked from the remembered selection, whether or not the widget is on screen: a row
  // greyed out here is one this pass will report as skipped, not one to silently forget.
  gtk_tree_store_set(_g.store, &iter, COL_CHECKED, g_hash_table_contains(_g.selected, label), COL_LABEL,
                     label, COL_PAGE, IS_NULL_PTR(page) ? "" : page, COL_TARGET, target, COL_CAPTURABLE,
                     TRUE, COL_ENABLED, gtk_widget_get_mapped(target), -1);
  dt_free(label);

  if(!GTK_IS_CONTAINER(inner)) return;

  GList *children = gtk_container_get_children(GTK_CONTAINER(inner));
  if(!IS_NULL_PTR(children))
  {
    GtkTreeIter placeholder;
    gtk_tree_store_append(_g.store, &placeholder, &iter);
    gtk_tree_store_set(_g.store, &placeholder, COL_CHECKED, FALSE, COL_LABEL, "…", COL_PAGE, "", COL_TARGET,
                       NULL, COL_CAPTURABLE, FALSE, COL_ENABLED, FALSE, -1);
  }
  g_list_free(children);
}


/* Registered module inventories. Filled from src/darktable.c -- see the header for why the
 * panel cannot look the modules up itself. Sections appear in registration order. */
typedef struct _doc_source_t
{
  gchar *section;
  dt_gui_doc_screenshot_source_t fill;
} _doc_source_t;

static GList *_sources = NULL; // _doc_source_t*, owned here, alive for the process

void dt_gui_doc_screenshot_register_source(const char *section, dt_gui_doc_screenshot_source_t source)
{
  if(IS_NULL_PTR(section) || IS_NULL_PTR(source)) return;

  _doc_source_t *entry = (_doc_source_t *)g_malloc0(sizeof(_doc_source_t));
  entry->section = g_strdup(section);
  entry->fill = source;
  _sources = g_list_append(_sources, entry);
}

/** The sink a source calls back into, once per widget. @p inventory is the section iterator
 * handed to the source; the name is copied into the widget-to-name table the capture and the
 * file naming both read. */
void dt_gui_doc_screenshot_add_target(void *inventory, GtkWidget *widget, const char *name)
{
  if(IS_NULL_PTR(inventory) || IS_NULL_PTR(widget) || IS_NULL_PTR(name)) return;

  g_hash_table_insert(_g.names, widget, g_strdup(name));
  _append_row((GtkTreeIter *)inventory, widget, NULL, 0);
}

/** Append a top-level grouping row. It captures nothing: it is a way in, not a target. */
static void _append_section(const char *title, GtkTreeIter *section)
{
  gtk_tree_store_append(_g.store, section, NULL);
  gtk_tree_store_set(_g.store, section, COL_CHECKED, FALSE, COL_LABEL, title, COL_PAGE, "", COL_TARGET, NULL,
                     COL_CAPTURABLE, FALSE, COL_ENABLED, TRUE, -1);
}


/** Replace a row's placeholder by its widget's real children, the first time it is opened. */
static void _on_row_expanded(GtkTreeView *view, GtkTreeIter *iter, GtkTreePath *path, gpointer user_data)
{
  GtkTreeModel *model = GTK_TREE_MODEL(_g.store);
  GtkTreeIter placeholder;
  if(!gtk_tree_model_iter_children(model, &placeholder, iter)) return;

  // Below the top level, only a placeholder carries no target: anything else is already built.
  GObject *probe = NULL;
  gtk_tree_model_get(model, &placeholder, COL_TARGET, &probe, -1);
  if(!IS_NULL_PTR(probe))
  {
    g_object_unref(probe);
    return;
  }

  GObject *target = NULL;
  gtk_tree_model_get(model, iter, COL_TARGET, &target, -1);
  GList *children = gtk_container_get_children(GTK_CONTAINER(_unwrap(GTK_WIDGET(target))));
  int index = 1;
  for(GList *item = children; item; item = g_list_next(item), index++)
    _append_row(iter, GTK_WIDGET(item->data), NULL, index);
  g_list_free(children);
  g_object_unref(target);

  // Dropped last, so the row is never momentarily childless -- which would collapse the
  // expander arrow under the very expansion that is being served.
  gtk_tree_store_remove(_g.store, &placeholder);
}


/** Identify a row by the chain of labels from the root down to it.
 *
 * A refresh rebuilds every row, so a GtkTreePath saved beforehand means nothing afterwards --
 * it is a position in a model that no longer exists. The labels do survive: they are derived
 * from the widgets themselves, and are what the reader was looking at.
 */
static gchar *_label_chain(GtkTreePath *path)
{
  GtkTreeModel *model = GTK_TREE_MODEL(_g.store);
  gint depth = 0;
  gint *indices = gtk_tree_path_get_indices_with_depth(path, &depth);

  GString *chain = g_string_new(NULL);
  GtkTreePath *walk = gtk_tree_path_new();
  for(gint level = 0; level < depth; level++)
  {
    gtk_tree_path_append_index(walk, indices[level]);

    GtkTreeIter iter;
    if(!gtk_tree_model_get_iter(model, &iter, walk)) break;

    gchar *label = NULL;
    gtk_tree_model_get(model, &iter, COL_LABEL, &label, -1);
    if(chain->len) g_string_append(chain, DOC_SCREENSHOT_SEPARATOR);
    g_string_append(chain, IS_NULL_PTR(label) ? "" : label);
    dt_free(label);
  }
  gtk_tree_path_free(walk);

  return g_string_free(chain, FALSE);
}


/** Collect one expanded row, as the "map expanded rows" callback. */
static void _collect_expanded(GtkTreeView *view, GtkTreePath *path, gpointer user_data)
{
  GList **chains = (GList **)user_data;
  *chains = g_list_prepend(*chains, _label_chain(path));
}


/** Walk a saved label chain back down the rebuilt tree.
 *
 * Each level is unfolded on the way through, because that is what builds the next one: the
 * children behind a placeholder do not exist until the row is expanded. The last level is
 * deliberately left folded -- the caller decides whether it should be expanded, selected, or
 * both.
 *
 * @return TRUE and the iter of the last label, FALSE if the chain no longer resolves (the
 *         module was unloaded, the view changed, the widget tree moved under it).
 */
static gboolean _find_row(const char *chain, GtkTreeIter *found)
{
  GtkTreeModel *model = GTK_TREE_MODEL(_g.store);
  gchar **labels = g_strsplit(chain, DOC_SCREENSHOT_SEPARATOR, -1);
  GtkTreeIter iter;
  gboolean have = FALSE;

  for(gchar **label = labels; *label; label++)
  {
    if(have)
    {
      GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
      gtk_tree_view_expand_row(GTK_TREE_VIEW(_g.view), path, FALSE);
      gtk_tree_path_free(path);
    }

    GtkTreeIter child;
    if(!gtk_tree_model_iter_children(model, &child, have ? &iter : NULL))
    {
      have = FALSE;
      break;
    }

    gboolean matched = FALSE;
    do
    {
      gchar *text = NULL;
      gtk_tree_model_get(model, &child, COL_LABEL, &text, -1);
      matched = !g_strcmp0(text, *label);
      dt_free(text);
    } while(!matched && gtk_tree_model_iter_next(model, &child));

    if(!matched)
    {
      have = FALSE;
      break;
    }

    iter = child;
    have = TRUE;
  }
  g_strfreev(labels);

  if(have) *found = iter;
  return have;
}


/** Rebuild the tree from the application's own registries, against the current destination.
 *
 * The top level is the handful of entry points a manual actually starts from -- the module
 * lists, the panels, the window -- and everything below is the live widget tree, reached by
 * drilling down. Both routes name a module identically because _widget_label() consults the
 * same map, built here.
 *
 * Carries the "clicked" signature so the Refresh button and the folder chooser call it
 * directly; the arguments are unused.
 */
static void _populate(GtkWidget *widget, gpointer user_data)
{
  // Remember what is unfolded and where the cursor sits. A refresh replaces every row, and
  // collapsing the tree back to its roots would throw away the reader's place at exactly the
  // moment they need it: the refresh loop is "change something in the application, refresh,
  // look at the same row again".
  GList *expanded = NULL;
  gtk_tree_view_map_expanded_rows(GTK_TREE_VIEW(_g.view), _collect_expanded, &expanded);

  gchar *selected = NULL;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(_g.view));
  GtkTreeModel *model = NULL;
  GtkTreeIter cursor;
  if(gtk_tree_selection_get_selected(selection, &model, &cursor))
  {
    GtkTreePath *path = gtk_tree_model_get_path(model, &cursor);
    selected = _label_chain(path);
    gtk_tree_path_free(path);
  }

  gtk_tree_store_clear(_g.store);
  g_hash_table_remove_all(_g.names);
  _load_pages();

  dt_ui_t *const ui = dt_gui_get_ui();
  GtkTreeIter section;

  // Module inventories, in registration order. The panel does not know what a module is --
  // see dt_gui_doc_screenshot_register_source() and its callers in src/darktable.c. The
  // sections are appended even when a source yields nothing, so the inventory reads the same
  // whichever view is current: the darkroom list is only populated while an image is open.
  for(GList *item = _sources; item; item = g_list_next(item))
  {
    const _doc_source_t *const src = (const _doc_source_t *)item->data;
    _append_section(src->section, &section);
    src->fill(&section);
  }

  // Named before use, so drilling down into the window finds them under these names too.
  g_hash_table_insert(_g.names, ui->top_panel, g_strdup(_("Menu bar")));
  g_hash_table_insert(_g.names, ui->panels[DT_UI_PANEL_TOP], g_strdup(_("Top panel")));
  g_hash_table_insert(_g.names, ui->panels[DT_UI_PANEL_LEFT], g_strdup(_("Left panel")));
  g_hash_table_insert(_g.names, ui->panels[DT_UI_PANEL_RIGHT], g_strdup(_("Right panel")));
  g_hash_table_insert(_g.names, ui->panels[DT_UI_PANEL_BOTTOM], g_strdup(_("Bottom panel")));
  g_hash_table_insert(_g.names, dt_ui_center_base(ui), g_strdup(_("Center area")));

  _append_section(_("Panels"), &section);
  _append_row(&section, ui->top_panel, NULL, 0);
  _append_row(&section, ui->panels[DT_UI_PANEL_TOP], NULL, 0);
  _append_row(&section, ui->panels[DT_UI_PANEL_LEFT], NULL, 0);
  _append_row(&section, ui->panels[DT_UI_PANEL_RIGHT], NULL, 0);
  _append_row(&section, ui->panels[DT_UI_PANEL_BOTTOM], NULL, 0);
  _append_row(&section, dt_ui_center_base(ui), NULL, 0);

  // Last: the whole window, from which everything else is reachable.
  _append_section(_("Window"), &section);
  _append_row(&section, dt_gui_main_window(), _("Main window"), 0);

  // Unfold what was unfolded. _find_row() expands each chain's ancestors on its way down, so
  // the order these come back in does not matter; a chain that no longer resolves -- its
  // module unloaded, its widget moved -- is simply dropped.
  for(GList *item = expanded; item; item = g_list_next(item))
  {
    GtkTreeIter iter;
    if(!_find_row((const char *)item->data, &iter)) continue;

    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(_g.store), &iter);
    gtk_tree_view_expand_row(GTK_TREE_VIEW(_g.view), path, FALSE);
    gtk_tree_path_free(path);
  }
  g_list_free_full(expanded, g_free);

  if(IS_NULL_PTR(selected)) return;

  // Put the cursor back and scroll to it. Selecting fires _on_row_selected(), so the preview
  // comes back with it -- which is the whole point of refreshing while watching one widget.
  GtkTreeIter iter;
  if(_find_row(selected, &iter))
  {
    GtkTreePath *path = gtk_tree_model_get_path(GTK_TREE_MODEL(_g.store), &iter);
    gtk_tree_view_expand_to_path(GTK_TREE_VIEW(_g.view), path);
    gtk_tree_selection_select_iter(selection, &iter);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(_g.view), path, NULL, FALSE, 0., 0.);
    gtk_tree_path_free(path);
  }
  dt_free(selected);
}


/** Draw the selected row's widget into the preview.
 *
 * This is the answer to "which of these twenty GtkBox rows is the one I want": a name can
 * only go so far on anonymous plumbing, and the picture is unambiguous. It costs one render
 * per selection change, which is what this panel does for a living anyway.
 */
static void _on_row_selected(GtkTreeSelection *selection, gpointer user_data)
{
  gtk_image_clear(GTK_IMAGE(_g.preview));
  gtk_label_set_text(GTK_LABEL(_g.geometry), "");

  GtkTreeModel *model = NULL;
  GtkTreeIter iter;
  if(!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

  GObject *target = NULL;
  gtk_tree_model_get(model, &iter, COL_TARGET, &target, -1);
  if(IS_NULL_PTR(target)) return; // a section header, or a placeholder not yet unfolded

  cairo_surface_t *surface = _render_widget(GTK_WIDGET(target));
  if(IS_NULL_PTR(surface))
  {
    gtk_label_set_text(GTK_LABEL(_g.geometry), _("not displayed right now"));
    g_object_unref(target);
    return;
  }

  const int width = cairo_image_surface_get_width(surface);
  const int height = cairo_image_surface_get_height(surface);
  GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0, width, height);
  cairo_surface_destroy(surface);

  // Shrunk to fit, never enlarged: a 24 px icon button has to read as a 24 px icon button.
  const double fit = MIN(1.0, (double)DOC_SCREENSHOT_PREVIEW_SIZE / (double)MAX(width, height));
  GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pixbuf, MAX(1, (int)(width * fit)),
                                              MAX(1, (int)(height * fit)), GDK_INTERP_BILINEAR);
  gtk_image_set_from_pixbuf(GTK_IMAGE(_g.preview), scaled);
  g_object_unref(scaled);
  g_object_unref(pixbuf);

  gchar *size = g_strdup_printf(_("%d x %d px"), width, height);
  gtk_label_set_text(GTK_LABEL(_g.geometry), size);
  dt_free(size);

  g_object_unref(target);
}


/** Flip one check box. The model owns the state; the renderer only reports the click. */
static void _on_toggled(GtkCellRendererToggle *renderer, gchar *path_string, gpointer user_data)
{
  GtkTreeIter iter;
  if(!gtk_tree_model_get_iter_from_string(GTK_TREE_MODEL(_g.store), &iter, path_string)) return;

  gboolean checked = FALSE;
  gchar *label = NULL;
  gtk_tree_model_get(GTK_TREE_MODEL(_g.store), &iter, COL_CHECKED, &checked, COL_LABEL, &label, -1);
  gtk_tree_store_set(_g.store, &iter, COL_CHECKED, !checked, -1);

  if(checked)
    g_hash_table_remove(_g.selected, label);
  else
    g_hash_table_add(_g.selected, g_strdup(label));

  dt_free(label);
  _save_selection();
}


static gboolean _select_row(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data)
{
  const _doc_screenshot_select_t mode = (_doc_screenshot_select_t)GPOINTER_TO_INT(user_data);

  gboolean capturable = FALSE;
  gboolean enabled = FALSE;
  gchar *label = NULL;
  gchar *page = NULL;
  gtk_tree_model_get(model, iter, COL_CAPTURABLE, &capturable, COL_ENABLED, &enabled, COL_LABEL, &label,
                     COL_PAGE, &page, -1);

  // A row we cannot see right now is left alone by "all" and "mapped": a documentation pass
  // captures the lighttable modules from lighttable and the darkroom ones from darkroom, and
  // the second half must not drop what the first half picked. "Select none" is the eraser,
  // and the only way to forget rows that are off screen.
  const gboolean reachable = capturable && (enabled || mode == DOC_SCREENSHOT_SELECT_NONE);
  if(reachable)
  {
    const gboolean wanted = (mode == DOC_SCREENSHOT_SELECT_MAPPED)
                                ? !IS_NULL_PTR(page) && page[0] != '\0'
                                : (mode == DOC_SCREENSHOT_SELECT_ALL);
    gtk_tree_store_set(_g.store, iter, COL_CHECKED, wanted, -1);

    if(wanted)
      g_hash_table_add(_g.selected, g_strdup(label));
    else
      g_hash_table_remove(_g.selected, label);
  }

  dt_free(page);
  dt_free(label);
  return FALSE; // walk the whole model
}


/** Check or uncheck rows: all, none, or exactly the ones the map names. Only rows that are
 * currently built are affected -- rows still folded behind a placeholder do not exist yet.
 * The mode arrives as the user data of the three buttons sharing this callback. */
static void _on_select(GtkButton *button, gpointer user_data)
{
  // Rows that were never unfolded are not in the model, so clearing has to go through the
  // remembered set directly to reach them.
  if(GPOINTER_TO_INT(user_data) == DOC_SCREENSHOT_SELECT_NONE) g_hash_table_remove_all(_g.selected);

  gtk_tree_model_foreach(GTK_TREE_MODEL(_g.store), _select_row, user_data);
  _save_selection();
}


static gboolean _capture_row(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data)
{
  _doc_screenshot_capture_t *capture = (_doc_screenshot_capture_t *)user_data;

  gboolean checked = FALSE;
  gchar *label = NULL;
  gchar *page = NULL;
  GObject *target = NULL;
  gtk_tree_model_get(model, iter, COL_CHECKED, &checked, COL_LABEL, &label, COL_PAGE, &page, COL_TARGET,
                     &target, -1);

  if(checked && !IS_NULL_PTR(target))
  {
    // Only a capture that can differ between languages gets the language code.
    const gboolean localized = _widget_has_text(GTK_WIDGET(target));

    gchar *relative = NULL;
    if(!IS_NULL_PTR(page) && page[0] != '\0')
    {
      // A mapped row goes exactly where the documentation expects its illustration.
      relative = localized ? _localized_path(page, capture->code) : g_strdup(page);
    }
    else
    {
      // An unmapped one lands at the root under its own name. Two rows can legitimately
      // carry the same name -- an unnamed GtkBox, or a module reached both from its section
      // and by drilling down its panel -- so number the repeats rather than have them
      // overwrite each other.
      const int seen = GPOINTER_TO_INT(g_hash_table_lookup(capture->used, label));
      g_hash_table_insert(capture->used, g_strdup(label), GINT_TO_POINTER(seen + 1));

      gchar *base = seen ? g_strdup_printf("%s_%d", label, seen + 1) : g_strdup(label);
      relative = localized ? g_strdup_printf("%s.%s.png", base, capture->code)
                           : g_strconcat(base, ".png", NULL);
      dt_free(base);
      g_strcanon(relative, DOC_SCREENSHOT_FILENAME_ALLOWED, '_');
    }

    // The three return codes finally earn their keep: a row whose widget is simply not on
    // screen is pending, not broken -- a remembered selection spans both views, and only one
    // of them is up at a time.
    gchar *file = g_build_filename(capture->folder, relative, NULL);
    const int result = _save_widget_as_image(GTK_WIDGET(target), file);
    if(result == 1)
      capture->skipped++;
    else if(result)
      capture->failed++;
    else
      capture->saved++;

    dt_free(file);
    dt_free(relative);
  }

  dt_free(page);
  dt_free(label);
  if(!IS_NULL_PTR(target)) g_object_unref(target);
  return FALSE; // walk the whole model
}


/** Write one image per checked row into the destination, then report the tally. */
static void _on_capture(GtkButton *button, gpointer user_data)
{
  gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(_g.folder));
  if(IS_NULL_PTR(folder))
  {
    gtk_label_set_text(GTK_LABEL(_g.status), _("Pick a destination folder first."));
    return;
  }

  // The documentation names its English files "en"; the untranslated locale calls itself "C".
  const char *code = dt_l10n_get_current_code(dt_l10n_get_global());
  if(!g_strcmp0(code, "C")) code = "en";

  _doc_screenshot_capture_t capture = { .folder = folder,
                                        .code = code,
                                        .used = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
                                        .saved = 0,
                                        .skipped = 0,
                                        .failed = 0 };
  gtk_tree_model_foreach(GTK_TREE_MODEL(_g.store), _capture_row, &capture);
  g_hash_table_destroy(capture.used);

  gchar *report = g_strdup_printf(_("%d image(s) written for language \"%s\" in %s "
                                    "-- %d skipped (not displayed), %d failed"),
                                  capture.saved, code, folder, capture.skipped, capture.failed);
  gtk_label_set_text(GTK_LABEL(_g.status), report);
  dt_free(report);
  dt_free(folder);
}


static void _on_destroy(GtkWidget *widget, gpointer user_data)
{
  // The store is owned by the view and dies with it, releasing its references on the widgets
  // the rows pointed at. The destination directory outlives the window: it came from the
  // command line and pre-fills the chooser again next time.
  g_hash_table_destroy(_g.names);
  g_hash_table_destroy(_g.pages);
  g_hash_table_destroy(_g.selected);
  _g.names = NULL;
  _g.pages = NULL;
  _g.selected = NULL;
  _g.store = NULL;
  _g.view = NULL;
  _g.window = NULL;
  _g.folder = NULL;
  _g.status = NULL;
  _g.preview = NULL;
  _g.geometry = NULL;
}


void dt_gui_doc_screenshot_window_show(void)
{
  if(_g.window)
  {
    gtk_window_present(GTK_WINDOW(_g.window));
    return;
  }

  _g.names = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  _g.pages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  _g.selected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  _load_selection();

  _g.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(_g.window), _("Documentation screenshots"));
  gtk_window_set_default_size(GTK_WINDOW(_g.window), 1100, 760);
  gtk_window_set_transient_for(GTK_WINDOW(_g.window), GTK_WINDOW(dt_gui_main_window()));
  g_signal_connect(_g.window, "destroy", G_CALLBACK(_on_destroy), NULL);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
  gtk_container_add(GTK_CONTAINER(_g.window), vbox);

  // Destination folder, and a way to rebuild the tree after a view change or an image load
  GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(vbox), bar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bar), gtk_label_new(_("Documentation root")), FALSE, FALSE, 0);

  _g.folder = gtk_file_chooser_button_new(_("Documentation root"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
  // Not "..." MACRO "..." inside _(): gettext extracts the source text verbatim and would
  // never match the concatenated string at runtime.
  gchar *tooltip = g_strdup_printf(_("Widget-to-page map: %s at the root of this folder, "
                                     "reloaded whenever the folder changes."),
                                   DOC_SCREENSHOT_MAP_FILE);
  gtk_widget_set_tooltip_text(_g.folder, tooltip);
  dt_free(tooltip);
  const char *pictures = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
  const char *destination = !IS_NULL_PTR(_g.directory) ? _g.directory
                            : !IS_NULL_PTR(pictures)   ? pictures
                                                       : g_get_home_dir();
  gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(_g.folder), destination);
  // Rebuilding on "file-set" is what reloads the map: the pages shown per row must describe
  // the folder the capture will actually write into.
  g_signal_connect(_g.folder, "file-set", G_CALLBACK(_populate), NULL);
  gtk_box_pack_start(GTK_BOX(bar), _g.folder, TRUE, TRUE, 0);

  GtkWidget *refresh = gtk_button_new_with_label(_("Refresh"));
  g_signal_connect(refresh, "clicked", G_CALLBACK(_populate), NULL);
  gtk_box_pack_start(GTK_BOX(bar), refresh, FALSE, FALSE, 0);

  // The tree: the first column carries the expander arrow, the check box and the name, so a
  // row reads as a single line however deep it sits; the second shows what the map binds it
  // to, which is the difference between a stray capture and a documentation update.
  _g.store = gtk_tree_store_new(COL_COUNT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_OBJECT,
                                G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
  _g.view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(_g.store));
  g_object_unref(_g.store); // the view owns the model from here on
  gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(_g.view), TRUE);
  g_signal_connect(_g.view, "row-expanded", G_CALLBACK(_on_row_expanded), NULL);

  GtkTreeViewColumn *widgets = gtk_tree_view_column_new();
  gtk_tree_view_column_set_title(widgets, _("Widget"));
  gtk_tree_view_column_set_expand(widgets, TRUE);
  GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
  g_signal_connect(toggle, "toggled", G_CALLBACK(_on_toggled), NULL);
  gtk_tree_view_column_pack_start(widgets, toggle, FALSE);
  gtk_tree_view_column_add_attribute(widgets, toggle, "active", COL_CHECKED);
  gtk_tree_view_column_add_attribute(widgets, toggle, "visible", COL_CAPTURABLE);
  // Activatable on COL_CAPTURABLE, NOT on COL_ENABLED: a row is checked to say "capture this
  // one", and that intent is worth expressing for a module of the other view or a collapsed
  // module's body, neither of which is on screen at the moment the user ticks it. The
  // selection is remembered and the capture reports what was not displayed as skipped, so
  // the natural workflow -- tick everything now, switch view, capture again -- needs the box
  // to be clickable while the widget is not up. Only the greyed label says "not right now".
  gtk_tree_view_column_add_attribute(widgets, toggle, "activatable", COL_CAPTURABLE);

  GtkCellRenderer *text = gtk_cell_renderer_text_new();
  gtk_tree_view_column_pack_start(widgets, text, TRUE);
  gtk_tree_view_column_add_attribute(widgets, text, "text", COL_LABEL);
  gtk_tree_view_column_add_attribute(widgets, text, "sensitive", COL_ENABLED);
  gtk_tree_view_append_column(GTK_TREE_VIEW(_g.view), widgets);

  GtkCellRenderer *page = gtk_cell_renderer_text_new();
  GtkTreeViewColumn *pages = gtk_tree_view_column_new_with_attributes(_("Documentation page"), page, "text",
                                                                     COL_PAGE, "sensitive", COL_ENABLED, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(_g.view), pages);

  g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(_g.view)), "changed",
                   G_CALLBACK(_on_row_selected), NULL);

  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(scroll), _g.view);

  // Tree and preview side by side: clicking a row answers "what is this one" immediately,
  // which no amount of naming can do for a stack of anonymous containers.
  _g.preview = gtk_image_new();
  _g.geometry = gtk_label_new("");
  GtkWidget *preview_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_pack_start(GTK_BOX(preview_box), _g.preview, TRUE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(preview_box), _g.geometry, FALSE, FALSE, 0);

  GtkWidget *preview_frame = gtk_frame_new(_("Preview"));
  gtk_container_set_border_width(GTK_CONTAINER(preview_box), 6);
  gtk_container_add(GTK_CONTAINER(preview_frame), preview_box);
  gtk_widget_set_size_request(preview_frame, DOC_SCREENSHOT_PREVIEW_SIZE + 24, -1);

  GtkWidget *panes = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_paned_pack1(GTK_PANED(panes), scroll, TRUE, FALSE);
  gtk_paned_pack2(GTK_PANED(panes), preview_frame, FALSE, FALSE);
  gtk_box_pack_start(GTK_BOX(vbox), panes, TRUE, TRUE, 0);

  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_pack_start(GTK_BOX(vbox), actions, FALSE, FALSE, 0);

  GtkWidget *select_mapped = gtk_button_new_with_label(_("Select mapped"));
  gtk_widget_set_tooltip_text(select_mapped, _("check every displayed row the map names"));
  g_signal_connect(select_mapped, "clicked", G_CALLBACK(_on_select), GINT_TO_POINTER(2));
  gtk_box_pack_start(GTK_BOX(actions), select_mapped, FALSE, FALSE, 0);

  GtkWidget *select_all = gtk_button_new_with_label(_("Select all"));
  gtk_widget_set_tooltip_text(select_all, _("only affects the rows already unfolded"));
  g_signal_connect(select_all, "clicked", G_CALLBACK(_on_select), GINT_TO_POINTER(TRUE));
  gtk_box_pack_start(GTK_BOX(actions), select_all, FALSE, FALSE, 0);

  GtkWidget *select_none = gtk_button_new_with_label(_("Select none"));
  g_signal_connect(select_none, "clicked", G_CALLBACK(_on_select), GINT_TO_POINTER(FALSE));
  gtk_box_pack_start(GTK_BOX(actions), select_none, FALSE, FALSE, 0);

  GtkWidget *capture = gtk_button_new_with_label(_("Capture"));
  g_signal_connect(capture, "clicked", G_CALLBACK(_on_capture), NULL);
  gtk_box_pack_end(GTK_BOX(actions), capture, FALSE, FALSE, 0);

  _g.status = gtk_label_new(NULL);
  gtk_label_set_line_wrap(GTK_LABEL(_g.status), TRUE);
  gtk_widget_set_halign(_g.status, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(vbox), _g.status, FALSE, FALSE, 0);

  gtk_widget_show_all(_g.window);
  _populate(NULL, NULL);
}
