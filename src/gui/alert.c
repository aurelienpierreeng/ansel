/*
    This file is part of Ansel,
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

#include "gui/alert.h"

#include "gui/application.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "widgets/dialog.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"

#include <glib/gi18n.h>
#include <gtk/gtk.h>

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h" // conditional-ok: its calls below are under the same test
#endif

struct dt_gui_alert_t
{
  dt_gui_alert_kind_t kind;
  GtkWidget *window;
  GtkWidget *column;  // right of the icon: the heading, then what the caller adds, then the buttons
  GtkWidget *buttons; // NULL until the first button is added
  GtkWidget *focus;   // the first button, which has the focus when the window shows
  dt_gui_alert_callback_t cancel; // a QUESTION's answer to Escape and the title bar
  void *cancel_data;
};

// One per button, freed with its "clicked" closure.
typedef struct dt_gui_alert_button_t
{
  dt_gui_alert_t *alert;
  dt_gui_alert_callback_t callback;
  void *data;
} dt_gui_alert_button_t;

// The window owns the alert: whatever destroys it -- dt_gui_alert_destroy(), or the user closing
// a NOTICE -- frees it.
static void _alert_window_destroyed(GtkWidget *window __attribute__((unused)), gpointer user_data)
{
  dt_gui_alert_t *alert = (dt_gui_alert_t *)user_data;
  // A notice closed by the user hands the focus back to where they were working. A question or a
  // progress leaves that to its owner, who knows what comes next.
  if(alert->kind == DT_GUI_ALERT_NOTICE) dt_gui_refocus_parent(GTK_WINDOW(dt_gui_main_window()));
  dt_free(alert);
}

static void _alert_cancel(dt_gui_alert_t *alert)
{
  if(!IS_NULL_PTR(alert->cancel)) alert->cancel(alert, alert->cancel_data);
}

static gboolean _alert_delete(GtkWidget *window __attribute__((unused)), GdkEvent *event __attribute__((unused)),
                              gpointer user_data)
{
  dt_gui_alert_t *alert = (dt_gui_alert_t *)user_data;
  switch(alert->kind)
  {
    case DT_GUI_ALERT_NOTICE:
      return FALSE; // closed, as any window
    case DT_GUI_ALERT_QUESTION:
      _alert_cancel(alert);
      return TRUE;
    case DT_GUI_ALERT_PROGRESS:
    default:
      // Closing it would stop nothing: it offers no button to, and refuses a delete-event sent
      // some other way (Alt+F4, a task bar).
      return TRUE;
  }
}

static gboolean _alert_key(GtkWidget *window, const GdkEventKey *event, gpointer user_data)
{
  if(event->keyval != GDK_KEY_Escape) return FALSE;
  dt_gui_alert_t *alert = (dt_gui_alert_t *)user_data;
  switch(alert->kind)
  {
    case DT_GUI_ALERT_NOTICE:
      gtk_widget_destroy(window);
      return TRUE;
    case DT_GUI_ALERT_QUESTION:
      _alert_cancel(alert);
      return TRUE;
    case DT_GUI_ALERT_PROGRESS:
    default:
      return FALSE;
  }
}

dt_gui_alert_t *dt_gui_alert_new(const dt_gui_alert_kind_t kind, const char *title, const char *heading)
{
  dt_gui_alert_t *alert = g_new0(dt_gui_alert_t, 1);
  alert->kind = kind;

  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  alert->window = window;
#ifdef GDK_WINDOWING_QUARTZ
  // Like every other window of ours: it must not open as a full-screen space of its own, and
  // it must show over the one the main window may have left.
  dt_osx_disallow_fullscreen(window);
#endif
  gtk_window_set_icon_name(GTK_WINDOW(window), "ansel");
  gtk_window_set_title(GTK_WINDOW(window), title);
  gtk_window_set_resizable(GTK_WINDOW(window), FALSE);

  GtkWidget *main_window = dt_gui_main_window();
  GtkWidget *icon = NULL;
  switch(kind)
  {
    case DT_GUI_ALERT_NOTICE:
      // Not modal: the user may go on working, and the window waits for them above the main window.
      // Being transient for it is what keeps it there, and only there. Not gtk_window_set_keep_above():
      // on Windows that is HWND_TOPMOST, above every application on the desktop.
      if(GTK_IS_WINDOW(main_window)) gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(main_window));
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
      icon = gtk_image_new_from_icon_name("dialog-warning", GTK_ICON_SIZE_DIALOG);
      break;
    case DT_GUI_ALERT_QUESTION:
      // The main window is still up, and nothing in it may change while the question is open.
      if(GTK_IS_WINDOW(main_window)) gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(main_window));
      gtk_window_set_modal(GTK_WINDOW(window), TRUE);
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
      icon = gtk_image_new_from_icon_name("dialog-warning", GTK_ICON_SIZE_DIALOG);
      break;
    case DT_GUI_ALERT_PROGRESS:
    default:
    {
      // The main window may be hidden: centred on the screen, not on it.
      gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
      gtk_window_set_deletable(GTK_WINDOW(window), FALSE);
      // A window group of its own. A grab held in the default group, the one every other window
      // of ours is in -- as dt_gui_closing_wait() holds one -- would take this window's clicks
      // too, and what the caller added could not be used.
      GtkWindowGroup *group = gtk_window_group_new();
      gtk_window_group_add_window(group, GTK_WINDOW(window));
      g_object_unref(group);
      icon = gtk_spinner_new();
      gtk_spinner_start(GTK_SPINNER(icon));
      break;
    }
  }

  g_signal_connect(window, "delete-event", G_CALLBACK(_alert_delete), alert);
  g_signal_connect(window, "key-press-event", G_CALLBACK(_alert_key), alert);
  g_signal_connect(window, "destroy", G_CALLBACK(_alert_window_destroyed), alert);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_PIXEL_APPLY_DPI(12));
  gtk_container_set_border_width(GTK_CONTAINER(box), DT_PIXEL_APPLY_DPI(16));
  gtk_container_add(GTK_CONTAINER(window), box);

  gtk_widget_set_valign(icon, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(box), icon, FALSE, FALSE, 0);

  alert->column = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(8));
  gtk_box_pack_start(GTK_BOX(box), alert->column, TRUE, TRUE, 0);

  GtkWidget *label = gtk_label_new(NULL);
  gchar *markup = g_markup_printf_escaped("<b>%s</b>", heading);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  dt_free(markup);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_box_pack_start(GTK_BOX(alert->column), label, FALSE, FALSE, 0);

  return alert;
}

GtkWidget *dt_gui_alert_add_text(dt_gui_alert_t *alert, const char *text)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
  gtk_box_pack_start(GTK_BOX(alert->column), label, FALSE, FALSE, 0);
  return label;
}

void dt_gui_alert_add_widget(dt_gui_alert_t *alert, GtkWidget *widget)
{
  gtk_box_pack_start(GTK_BOX(alert->column), widget, FALSE, FALSE, 0);
}

static void _alert_button_clicked(GtkButton *button __attribute__((unused)), gpointer user_data)
{
  const dt_gui_alert_button_t *clicked = (const dt_gui_alert_button_t *)user_data;
  // The callback may destroy the alert: nothing touches it after.
  clicked->callback(clicked->alert, clicked->data);
}

void dt_gui_alert_add_button(dt_gui_alert_t *alert, const char *label, dt_gui_alert_callback_t callback,
                             void *data)
{
  // The row is packed from the end of the column, so that it stays at the bottom whatever is
  // added after it.
  if(IS_NULL_PTR(alert->buttons))
  {
    alert->buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(alert->buttons), GTK_BUTTONBOX_END);
    gtk_box_set_spacing(GTK_BOX(alert->buttons), DT_PIXEL_APPLY_DPI(6));
    gtk_widget_set_margin_top(alert->buttons, DT_PIXEL_APPLY_DPI(6));
    gtk_box_pack_end(GTK_BOX(alert->column), alert->buttons, FALSE, FALSE, 0);
  }

  GtkWidget *button = gtk_button_new_with_label(label);
  dt_gui_alert_button_t *clicked = g_new0(dt_gui_alert_button_t, 1);
  clicked->alert = alert;
  clicked->callback = callback;
  clicked->data = data;
  g_signal_connect_data(button, "clicked", G_CALLBACK(_alert_button_clicked), clicked, (GClosureNotify)g_free, 0);
  gtk_container_add(GTK_CONTAINER(alert->buttons), button);
  if(IS_NULL_PTR(alert->focus)) alert->focus = button;
}

void dt_gui_alert_set_cancel(dt_gui_alert_t *alert, dt_gui_alert_callback_t callback, void *data)
{
  alert->cancel = callback;
  alert->cancel_data = data;
}

void dt_gui_alert_show(dt_gui_alert_t *alert)
{
  gtk_widget_show_all(alert->window);
  if(!IS_NULL_PTR(alert->focus)) gtk_widget_grab_focus(alert->focus);
  gtk_window_present(GTK_WINDOW(alert->window));
#ifdef GDK_WINDOWING_QUARTZ
  // A question or a progress answers the user, but not always from the application in front: the
  // Dock's Quit, or Cmd+Q through the application switcher, leave another one active, and a window
  // opened by an application in the background opens behind the windows of the one that is not.
  // A notice comes unasked, and does not take the focus from another application.
  if(alert->kind != DT_GUI_ALERT_NOTICE) dt_osx_focus_window();
#endif
}

void dt_gui_alert_destroy(dt_gui_alert_t *alert)
{
  gtk_widget_destroy(alert->window); // frees the alert, see _alert_window_destroyed()
}

/* --- dt_gui_alert(): a NOTICE with one OK button, from any thread --------- */

typedef struct dt_alert_message_t
{
  gchar *title;
  gchar *message;
  gchar *item;
} dt_alert_message_t;

// The dt_gui_alert() windows on screen, by title and message: the box that lists each one's items.
// GUI thread only.
static GHashTable *_alert_windows = NULL;

static void _alert_message_free(gpointer data)
{
  dt_alert_message_t *message = (dt_alert_message_t *)data;
  dt_free(message->title);
  dt_free(message->message);
  dt_free(message->item);
  dt_free(message);
}

// The list goes with its window, whatever closes it -- OK, its title bar, Escape: the message is
// free for the next alert.
static void _alert_window_items_destroyed(GtkWidget *block __attribute__((unused)), gpointer user_data)
{
  g_hash_table_remove(_alert_windows, (const gchar *)user_data);
}

static void _alert_ok_clicked(dt_gui_alert_t *alert, void *data __attribute__((unused)))
{
  dt_gui_alert_destroy(alert);
}

static gboolean _alert_show(gpointer user_data)
{
  const dt_alert_message_t *message = (const dt_alert_message_t *)user_data;
  if(IS_NULL_PTR(_alert_windows))
    _alert_windows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  // One window per kind of message: its title and its text, which say what went wrong, never where.
  // The same pair again while its window is up only adds its item to that window's list. A title
  // is one line: the first line break of the key ends it.
  gchar *key = g_strconcat(message->title, "\n", message->message, NULL);
  dt_gui_alert_t *alert = NULL;
  GtkWidget *block = g_hash_table_lookup(_alert_windows, key);
  if(IS_NULL_PTR(block))
  {
    alert = dt_gui_alert_new(DT_GUI_ALERT_NOTICE, message->title, message->title);
    dt_gui_alert_add_text(alert, message->message);
    // Under the message, the list of its items, which appears with the first one.
    block = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    dt_gui_alert_add_widget(alert, block);
    dt_gui_alert_add_button(alert, _("OK"), _alert_ok_clicked, NULL);
    // Each item once: a module failing on the same image at every change in the darkroom lists
    // that image once.
    g_object_set_data_full(G_OBJECT(block), "listed",
                           g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
                           (GDestroyNotify)g_hash_table_destroy);

    // The table takes the key; the destroy handler gets a copy of its own, freed with the closure.
    g_signal_connect_data(block, "destroy", G_CALLBACK(_alert_window_items_destroyed), g_strdup(key),
                          (GClosureNotify)g_free, 0);
    g_hash_table_insert(_alert_windows, key, block);
  }
  else
    dt_free(key);

  gboolean added = FALSE;
  if(!IS_NULL_PTR(message->item)
     && g_hash_table_add(g_object_get_data(G_OBJECT(block), "listed"), g_strdup(message->item)))
  {
    // The list, as the closing window's: as tall as its lines up to a point, then it scrolls -- a
    // module failing on every thumbnail of a film roll lists hundreds of images.
    GtkWidget *items = g_object_get_data(G_OBJECT(block), "items");
    if(IS_NULL_PTR(items))
    {
      items = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
      GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
      dt_gui_add_class(scroll, "dt_recessed_scroll");
      gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
      gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), DT_PIXEL_APPLY_DPI(150));
      gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
      gtk_container_add(GTK_CONTAINER(scroll), items);
      gtk_box_pack_start(GTK_BOX(block), scroll, FALSE, FALSE, 0);
      g_object_set_data(G_OBJECT(block), "items", items);
    }
    // A path has no spaces to break at: it wraps anywhere, whole, and can be selected to be copied.
    GtkWidget *label = gtk_label_new(message->item);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 60);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_box_pack_start(GTK_BOX(items), label, FALSE, FALSE, 0);
    added = TRUE;
  }

  // A new window shows once it holds its first item, to be centred with it. A repeat that adds
  // nothing leaves the window where it is: the darkroom runs its pipe again at every change, and
  // the window would take the focus back each time.
  if(!IS_NULL_PTR(alert))
    dt_gui_alert_show(alert);
  else if(added)
  {
    gtk_widget_show_all(block);
    gtk_window_present(GTK_WINDOW(gtk_widget_get_toplevel(block)));
  }
  return G_SOURCE_REMOVE;
}

void dt_gui_alert(const char *title, const char *message, const char *item)
{
  dt_alert_message_t *copy = g_new0(dt_alert_message_t, 1);
  copy->title = g_strdup(title);
  copy->message = g_strdup(message);
  copy->item = g_strdup(item);
  // Always deferred, even on the GUI thread: a caller may hold a lock -- the pixelpipe cache
  // alerts from under its own -- and building a window must not run inside it.
  g_idle_add_full(G_PRIORITY_DEFAULT, _alert_show, copy, _alert_message_free);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
