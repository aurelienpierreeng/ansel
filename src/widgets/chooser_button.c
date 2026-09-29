/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "widgets/chooser_button.h"

#include "system/macros.h"            // IS_NULL_PTR
#include "system/mem_alloc.h"         // dt_free
#include "widgets/button.h"           // dtgtk_button_new
#include "widgets/color_well.h"       // the colour window's content
#include "widgets/dialog.h"           // dt_gui_refocus_parent
#include "widgets/paint.h"            // dtgtk_cairo_paint_cancel
#include "widgets/widget_settings.h"  // dt_widget_root_window
#include "widgets/widget_style.h"     // dt_gui_add_class

#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>   // conditional-ok: GDK_IS_WAYLAND_DISPLAY() is used only inside the same #ifdef
#endif
#include <glib/gi18n.h>

#define CHOOSER_BUTTON_KEY "dt-chooser-button"
/** How long the focus is given to settle before a colour window that lost it closes. */
#define CHOOSER_FOCUS_SETTLE_MS 100

typedef enum dt_chooser_kind_t
{
  DT_CHOOSER_COLOR = 0,
  DT_CHOOSER_FONT = 1,
} dt_chooser_kind_t;

/** How a colour window goes away. */
typedef enum dt_chooser_close_t
{
  DT_CHOOSER_CLOSE_KEEP = 0,  ///< the close button, a click outside, Return
  DT_CHOOSER_CLOSE_KEEP_AWAY, ///< the application lost the focus, or the window system took the window away
  DT_CHOOSER_CLOSE_GIVE_BACK, ///< Escape, or the caller closing it
  DT_CHOOSER_CLOSE_SILENT,    ///< the button is being destroyed: nothing is reported to anyone
} dt_chooser_close_t;

/**
 * What a chooser button holds, attached to the button and freed with it. The button is not
 * referenced: this lives exactly as long as the button does, so a pointer back to it is always
 * valid while anything here can run.
 */
typedef struct dt_chooser_button_t
{
  dt_chooser_kind_t kind;
  GtkWidget *button;
  GtkWidget *label;   ///< FONT: the ellipsised label; NULL for a colour
  GtkWidget *dialog;  ///< the open font dialog or colour window, NULL when there is none
  GtkWidget *well;    ///< COLOR: the colour well inside the open window
  GtkWindow *parent;  ///< weak: NULLed by GObject when the window goes
  GtkWindow *watched; ///< weak: the window whose losing the focus closes the colour window
  gulong watched_handler;
  guint close_source; ///< closes the colour window once the focus has settled
  gchar *title;
  gboolean use_alpha;
  gchar *history_key;
  GdkRGBA color;
  GdkRGBA opened;     ///< COLOR: the colour the open window started from
  gboolean reported;  ///< COLOR: the open window has reported a change
  gboolean closing;   ///< COLOR: the window is going, and what it emits on the way is not the user
  gboolean window_focused; ///< COLOR: the window system says the colour window itself has the focus
  gchar *font;        ///< never NULL
  dt_chooser_color_changed_t color_changed;
  dt_chooser_font_picked_t font_picked;
  gpointer user_data;
} dt_chooser_button_t;

static dt_chooser_button_t *_chooser(GtkWidget *button)
{
  if(IS_NULL_PTR(button)) return NULL;
  return (dt_chooser_button_t *)g_object_get_data(G_OBJECT(button), CHOOSER_BUTTON_KEY);
}

static void _chooser_forget_parent(dt_chooser_button_t *chooser)
{
  // GObject holds a write permission on the field for as long as the weak pointer is registered,
  // so it is withdrawn before the field changes or its struct is freed.
  if(IS_NULL_PTR(chooser->parent)) return;
  g_object_remove_weak_pointer(G_OBJECT(chooser->parent), (gpointer *)&chooser->parent);
  chooser->parent = NULL;
}

/** Stop listening to the window the colour window watches, and forget any close waiting on it. */
static void _chooser_forget_watched(dt_chooser_button_t *chooser)
{
  if(chooser->close_source != 0)
  {
    g_source_remove(chooser->close_source);
    chooser->close_source = 0;
  }
  if(IS_NULL_PTR(chooser->watched)) return;
  g_signal_handler_disconnect(chooser->watched, chooser->watched_handler);
  chooser->watched_handler = 0;
  g_object_remove_weak_pointer(G_OBJECT(chooser->watched), (gpointer *)&chooser->watched);
  chooser->watched = NULL;
}

static void _chooser_free(gpointer data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)data;
  _chooser_forget_watched(chooser);
  _chooser_forget_parent(chooser);
  dt_free(chooser->title);
  dt_free(chooser->history_key);
  dt_free(chooser->font);
  dt_free(chooser);
}

/** The font description reduced to what the chooser offers and the label shows: family and style. */
static gchar *_family_and_style(const PangoFontDescription *description)
{
  if(IS_NULL_PTR(description)) return g_strdup("");
  PangoFontDescription *face = pango_font_description_copy(description);
  pango_font_description_unset_fields(face, PANGO_FONT_MASK_SIZE);
  gchar *text = pango_font_description_to_string(face);
  pango_font_description_free(face);
  return text;
}

static void _dialog_destroyed(GtkWidget *dialog, gpointer user_data)
{
  // Gone without us: destroyed with its parent window, most likely.
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  if(chooser->dialog != dialog) return;
  chooser->dialog = NULL;
  chooser->well = NULL;
  _chooser_forget_watched(chooser);
}

static void _dialog_close(dt_chooser_button_t *chooser, const gboolean refocus)
{
  GtkWidget *dialog = chooser->dialog;
  if(IS_NULL_PTR(dialog)) return;
  chooser->dialog = NULL;
  g_signal_handlers_disconnect_by_data(dialog, chooser);
  GtkWindow *dialog_parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
  gtk_widget_destroy(dialog);
  // The transient hint does not reliably hand the focus back on every platform; a button being
  // destroyed has no business raising a window, though, since its window may be going too.
  if(refocus) dt_gui_refocus_parent(dialog_parent);
}

static void _dialog_response(GtkDialog *dialog, const gint response, gpointer user_data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  const gboolean confirmed = response == GTK_RESPONSE_OK;
  GtkWidget *button = chooser->button;
  const dt_chooser_font_picked_t font_picked = chooser->font_picked;
  const gpointer picked_data = chooser->user_data;

  // Read the answer before the dialog goes, and close it before reporting: a caller acting on the
  // pick may refill this very button, or close its whole panel, and must find no dialog left open.
  gchar *picked_font = NULL;
  if(confirmed)
  {
    PangoFontDescription *description = gtk_font_chooser_get_font_desc(GTK_FONT_CHOOSER(dialog));
    picked_font = _family_and_style(description);
    if(!IS_NULL_PTR(description)) pango_font_description_free(description);
  }

  _dialog_close(chooser, TRUE);

  // Nothing of `chooser` is read from here on: the callback may destroy the button, and it with it.
  if(confirmed && !IS_NULL_PTR(font_picked)) font_picked(button, picked_font, picked_data);
  dt_free(picked_font);
}

static GtkWindow *_dialog_parent(const dt_chooser_button_t *chooser)
{
  if(!IS_NULL_PTR(chooser->parent)) return chooser->parent;
  GtkWidget *toplevel = gtk_widget_get_toplevel(chooser->button);
  if(GTK_IS_WINDOW(toplevel) && gtk_widget_is_toplevel(toplevel)) return GTK_WINDOW(toplevel);
  GtkWidget *root = dt_widget_root_window();
  if(GTK_IS_WINDOW(root)) return GTK_WINDOW(root);
  return NULL;
}

/* --- the colour window ------------------------------------------------------------------------- */

/**
 * Close the colour window. Whatever is typed or held in it counts when it is kept. The window goes
 * BEFORE the caller hears of it, which is told last: acting on the answer may refill this button,
 * or destroy it and this struct with it.
 */
static void _color_window_close(dt_chooser_button_t *chooser, const dt_chooser_close_t how)
{
  GtkWidget *window = chooser->dialog;
  if(IS_NULL_PTR(window) || chooser->closing) return;
  const gboolean keeping = how == DT_CHOOSER_CLOSE_KEEP || how == DT_CHOOSER_CLOSE_KEEP_AWAY;
  if(keeping) dt_color_well_commit_pending(chooser->well);
  chooser->closing = TRUE;

  GtkWidget *button = chooser->button;
  const dt_chooser_color_changed_t changed = chooser->color_changed;
  const gpointer changed_data = chooser->user_data;
  const gboolean reported = chooser->reported;
  const GdkRGBA opened = chooser->opened;
  GdkRGBA current;
  dt_color_well_get_color(chooser->well, &current);
  // A colour dragged away and back is no change, and is given back rather than kept: kept, it
  // would be an undo step that undoes nothing. Back means back to the byte, what the number shows.
  const gboolean keep = keeping && !dt_color_well_same_color(&current, &opened);
  // Given back, the recent colours go back as well: the colour never left the window.
  if(!keep) dt_color_well_revert(chooser->well);
  chooser->color = keep ? current : opened;

  _chooser_forget_watched(chooser);
  chooser->dialog = NULL;
  chooser->well = NULL;
  chooser->reported = FALSE;
  g_signal_handlers_disconnect_by_data(window, chooser);
  GtkWindow *window_parent = gtk_window_get_transient_for(GTK_WINDOW(window));
  gtk_widget_destroy(window);
  chooser->closing = FALSE;
  gtk_widget_queue_draw(button);
  if(how == DT_CHOOSER_CLOSE_SILENT) return;
  // A popup never takes the focus from its parent, but the transient hint is not trusted to leave it
  // there on every platform. Only when the application still has the focus, though: gone to another
  // program, the parent would be raised over it, or flash for attention, a moment after the user left.
  if(how != DT_CHOOSER_CLOSE_KEEP_AWAY) dt_gui_refocus_parent(window_parent);

  if(!reported || IS_NULL_PTR(changed)) return;
  if(keep)
    changed(button, &current, DT_CHOOSER_COLOR_COMMIT, changed_data);
  else
    changed(button, &opened, DT_CHOOSER_COLOR_CANCEL, changed_data);
}

static void _well_changed(GtkWidget *well, const GdkRGBA *color, const dt_color_well_phase_t phase,
                          gpointer user_data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  // A typed number applied by the focus leaving the entry while the window is destroyed is not a pick.
  if(chooser->closing) return;
  // A well's own commit is a step of the window's gesture: the window's closing ends it. A drag let go
  // of where its last motion was commits the colour that motion already sent, and the caller, which
  // repaints what it is sent, is not sent it twice.
  if(gdk_rgba_equal(&chooser->color, color)) return;
  chooser->color = *color;
  chooser->reported = TRUE;
  gtk_widget_queue_draw(chooser->button);
  if(!IS_NULL_PTR(chooser->color_changed))
    chooser->color_changed(chooser->button, color, DT_CHOOSER_COLOR_LIVE, chooser->user_data);
}

static gboolean _color_window_key(GtkWidget *window, GdkEventKey *event, gpointer user_data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  if(event->keyval == GDK_KEY_Escape)
  {
    _color_window_close(chooser, DT_CHOOSER_CLOSE_GIVE_BACK);
    return TRUE;
  }
  if(event->keyval == GDK_KEY_Return || event->keyval == GDK_KEY_KP_Enter)
  {
    // The control with the keyboard has the key first: a recent colour picks itself before the window
    // closes on it, the number applies itself. A window's own handler runs before it hands keys on.
    gtk_window_propagate_key_event(GTK_WINDOW(window), event);
    if(IS_NULL_PTR(chooser->dialog)) return TRUE;
    // A number that does not read as a colour stays on screen, marked, rather than closing on nothing.
    if(!dt_color_well_commit_pending(chooser->well)) return TRUE;
    _color_window_close(chooser, DT_CHOOSER_CLOSE_KEEP);
    return TRUE;
  }
  return FALSE;
}

/**
 * The window holds the grab, so a press anywhere else in the application is delivered here, still
 * naming the window it happened in, and to nothing else: a click on the canvas that dismisses the
 * colour is spent on that, and does not also select, drag or deselect what is under it.
 */
static gboolean _color_window_pressed(GtkWidget *window, GdkEventButton *event, gpointer user_data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  const GdkWindow *own = gtk_widget_get_window(window);
  if(!IS_NULL_PTR(event->window) && gdk_window_get_toplevel(event->window) == own) return FALSE;
  _color_window_close(chooser, DT_CHOOSER_CLOSE_KEEP);
  return TRUE;
}

static gboolean _color_window_close_later(gpointer user_data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  if(IS_NULL_PTR(chooser->dialog))
  {
    chooser->close_source = 0;
    return G_SOURCE_REMOVE;
  }
  const gboolean unmapped = !gtk_widget_get_mapped(chooser->dialog);
  // A menu opened inside the window -- the number's own context menu -- grabs the keyboard, and on X11 a
  // grab reads as the parent losing the focus. The application has not lost it: asked again once the
  // grab is over, by which time the focus has come back from the menu, or truly gone.
  const GtkWidget *grab = gtk_grab_get_current();
  if(!unmapped && !IS_NULL_PTR(grab) && grab != chooser->dialog) return G_SOURCE_CONTINUE;
  chooser->close_source = 0;
  // Still unfocused once the focus has settled, and the window has not taken the focus itself --
  // which a popup never does on X11 or Wayland, and does on Broadway, the moment it maps.
  const gboolean parent_active = !IS_NULL_PTR(chooser->watched) && gtk_window_is_active(chooser->watched);
  if((parent_active || chooser->window_focused) && !unmapped) return G_SOURCE_REMOVE;
  _color_window_close(chooser, DT_CHOOSER_CLOSE_KEEP_AWAY);
  return G_SOURCE_REMOVE;
}

static void _color_window_schedule_close(dt_chooser_button_t *chooser)
{
  if(chooser->closing || chooser->close_source != 0) return;
  // Not an idle: the parent's losing the focus and the window's taking it are two events, and the
  // second may still be on its way when an idle runs.
  chooser->close_source = g_timeout_add(CHOOSER_FOCUS_SETTLE_MS, _color_window_close_later, chooser);
}

static void _watched_active_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
  // The application lost the focus: another program was clicked, or the desktop switched.
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  if(gtk_window_is_active(GTK_WINDOW(object))) return;
  _color_window_schedule_close(chooser);
}

static gboolean _color_window_state(GtkWidget *window, GdkEventWindowState *event, gpointer user_data)
{
  // Where the window takes the focus of its own, losing it is the application losing it: the parent
  // was already inactive from the moment the window mapped.
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  if(!(event->changed_mask & GDK_WINDOW_STATE_FOCUSED)) return FALSE;
  chooser->window_focused = (event->new_window_state & GDK_WINDOW_STATE_FOCUSED) != 0;
  if(!chooser->window_focused) _color_window_schedule_close(chooser);
  return FALSE;
}

static gboolean _color_window_unmapped(GtkWidget *window, GdkEvent *event, gpointer user_data)
{
  // A Wayland compositor dismisses a popup by unmapping it, on a click outside the application.
  _color_window_schedule_close((dt_chooser_button_t *)user_data);
  return FALSE;
}

static void _color_window_close_clicked(GtkButton *close_button, gpointer user_data)
{
  _color_window_close((dt_chooser_button_t *)user_data, DT_CHOOSER_CLOSE_KEEP);
}

/**
 * Put the colour window under the button, or above it where the screen has no room below, and
 * inside the screen's work area, before it maps. Wayland lets no client place a window, and is asked
 * to do the same against the button's rectangle; elsewhere the window moves itself, which X11
 * honours for a popup exactly.
 */
static void _color_window_place(const dt_chooser_button_t *chooser, GtkWidget *window)
{
  GtkWidget *toplevel = gtk_widget_get_toplevel(chooser->button);
  if(!GTK_IS_WINDOW(toplevel) || !gtk_widget_get_realized(chooser->button)) return;
  gint button_x = 0;
  gint button_y = 0;
  if(!gtk_widget_translate_coordinates(chooser->button, toplevel, 0, 0, &button_x, &button_y)) return;
  GtkAllocation allocation;
  gtk_widget_get_allocation(chooser->button, &allocation);
  GtkRequisition natural;
  gtk_widget_get_preferred_size(window, NULL, &natural);

#ifdef GDK_WINDOWING_WAYLAND
  if(GDK_IS_WAYLAND_DISPLAY(gtk_widget_get_display(window)))
  {
    const GdkRectangle anchor = { button_x, button_y, MAX(allocation.width, 1), MAX(allocation.height, 1) };
    // The rectangle is in the transient parent's coordinates, which must then be the button's toplevel.
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(toplevel));
    gdk_window_move_to_rect(gtk_widget_get_window(window), &anchor, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST,
                            GDK_ANCHOR_FLIP_Y | GDK_ANCHOR_SLIDE, 0, 0);
    return;
  }
#endif

  GdkWindow *toplevel_window = gtk_widget_get_window(toplevel);
  if(IS_NULL_PTR(toplevel_window)) return;
  gint origin_x = 0;
  gint origin_y = 0;
  gdk_window_get_origin(toplevel_window, &origin_x, &origin_y);
  const gint screen_x = origin_x + button_x;
  const gint screen_y = origin_y + button_y;
  gint target_x = screen_x;
  gint target_y = screen_y + allocation.height;
  // The button's own monitor: a main window spanning two screens belongs to the one holding most of it,
  // and the window would be kept on that one, away from a button on the other.
  const gint button_center_x = screen_x + allocation.width / 2;
  const gint button_center_y = screen_y + allocation.height / 2;
  GdkMonitor *monitor
      = gdk_display_get_monitor_at_point(gtk_widget_get_display(window), button_center_x, button_center_y);
  GdkRectangle workarea = { 0, 0, 0, 0 };
  if(!IS_NULL_PTR(monitor)) gdk_monitor_get_workarea(monitor, &workarea);
  // A backend that knows no monitor reports an empty work area, and clamping into it would send the
  // window to the origin of the screen.
  if(workarea.width > 0 && workarea.height > 0)
  {
    const gint workarea_bottom = workarea.y + workarea.height;
    if(target_y + natural.height > workarea_bottom && screen_y - natural.height >= workarea.y)
      target_y = screen_y - natural.height;
    target_x = CLAMP(target_x, workarea.x, MAX(workarea.x, workarea.x + workarea.width - natural.width));
    target_y = CLAMP(target_y, workarea.y, MAX(workarea.y, workarea_bottom - natural.height));
  }
  gtk_window_move(GTK_WINDOW(window), target_x, target_y);
}

/**
 * Tell the window it has the keyboard. A popup never receives the focus from the window system:
 * the keys reach it through the grab, but the entry would draw no cursor and the window would
 * read as inactive. GTK's own popups with an entry in them do the same.
 */
static void _color_window_focus_in(GtkWidget *window)
{
  GdkWindow *gdk_window = gtk_widget_get_window(window);
  GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(window));
  GdkDevice *keyboard = IS_NULL_PTR(seat) ? NULL : gdk_seat_get_keyboard(seat);
  if(IS_NULL_PTR(gdk_window)) return;
  GdkEvent *focus_event = gdk_event_new(GDK_FOCUS_CHANGE);
  focus_event->focus_change.window = g_object_ref(gdk_window);
  focus_event->focus_change.send_event = TRUE;
  focus_event->focus_change.in = TRUE;
  if(!IS_NULL_PTR(keyboard)) gdk_event_set_device(focus_event, keyboard);
  gtk_widget_send_focus_change(window, focus_event);
  gdk_event_free(focus_event);
}

static void _color_window_open(dt_chooser_button_t *chooser)
{
  GtkWindow *parent = _dialog_parent(chooser);
  GtkWidget *window = gtk_window_new(GTK_WINDOW_POPUP);
  gtk_widget_set_name(window, "dt-chooser-color-window");
  dt_gui_add_class(window, "dt-color-well-popup");
  // Modal: nothing else in the application takes input while it is open, so the colour cannot
  // land on something other than what the button showed when it was clicked.
  gtk_window_set_modal(GTK_WINDOW(window), TRUE);
  gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  // A menu to the window system: placed against the button on Wayland, never made fullscreen on macOS.
  gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_POPUP_MENU);
  if(!IS_NULL_PTR(parent)) gtk_window_set_transient_for(GTK_WINDOW(window), parent);
  gtk_window_set_attached_to(GTK_WINDOW(window), chooser->button);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
  gtk_widget_add_events(window, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  // The frame is the box's: a window's own CSS padding does not reach its child.
  dt_gui_add_class(box, "dt-color-well-frame");
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
  GtkWidget *title = gtk_label_new(chooser->title);
  gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(title), 1);
  dt_gui_add_class(title, "dt-color-well-title");
  gtk_box_pack_start(GTK_BOX(header), title, TRUE, TRUE, 0);
  GtkWidget *close_button = dtgtk_button_new(dtgtk_cairo_paint_cancel, 0, NULL);
  gtk_widget_set_name(close_button, "dt-chooser-color-close");
  gtk_widget_set_can_focus(close_button, FALSE);
  gtk_widget_set_tooltip_text(close_button, _("Close, keeping the colour. Escape gives the colour back."));
  g_signal_connect(close_button, "clicked", G_CALLBACK(_color_window_close_clicked), chooser);
  gtk_box_pack_end(GTK_BOX(header), close_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), header, FALSE, FALSE, 0);

  GtkWidget *well = dt_color_well_new(chooser->history_key, chooser->use_alpha, _well_changed, chooser);
  dt_color_well_set_color(well, &chooser->color);
  gtk_box_pack_start(GTK_BOX(box), well, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(window), box);

  chooser->dialog = window;
  chooser->well = well;
  chooser->reported = FALSE;
  chooser->window_focused = FALSE;
  // What the well made of the colour, not the colour: the hue, saturation and value it is held in
  // round-trip it, and closing must compare like with like.
  dt_color_well_get_color(well, &chooser->opened);

  g_signal_connect(window, "key-press-event", G_CALLBACK(_color_window_key), chooser);
  g_signal_connect(window, "button-press-event", G_CALLBACK(_color_window_pressed), chooser);
  g_signal_connect(window, "unmap-event", G_CALLBACK(_color_window_unmapped), chooser);
  g_signal_connect(window, "window-state-event", G_CALLBACK(_color_window_state), chooser);
  g_signal_connect(window, "destroy", G_CALLBACK(_dialog_destroyed), chooser);
  if(!IS_NULL_PTR(parent))
  {
    chooser->watched = parent;
    g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&chooser->watched);
    chooser->watched_handler
        = g_signal_connect(parent, "notify::is-active", G_CALLBACK(_watched_active_changed), chooser);
  }

  gtk_widget_show_all(box);
  gtk_widget_realize(window);
  _color_window_place(chooser, window);
  gtk_widget_show(window);
  _color_window_focus_in(window);
  dt_color_well_grab_focus(well);
}

/* --- the button --------------------------------------------------------------------------------- */

static void _button_clicked(GtkButton *button, gpointer user_data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  if(!IS_NULL_PTR(chooser->dialog))
  {
    gtk_window_present(GTK_WINDOW(chooser->dialog));
    return;
  }

  if(chooser->kind == DT_CHOOSER_COLOR)
  {
    _color_window_open(chooser);
    return;
  }

  GtkWindow *parent = _dialog_parent(chooser);
  GtkWidget *dialog = gtk_font_chooser_dialog_new(chooser->title, parent);
  // The size is set elsewhere, next to the face, and a face picked here must not change it.
  gtk_font_chooser_set_level(GTK_FONT_CHOOSER(dialog),
                             GTK_FONT_CHOOSER_LEVEL_FAMILY | GTK_FONT_CHOOSER_LEVEL_STYLE);
  if(chooser->font[0] != '\0') gtk_font_chooser_set_font(GTK_FONT_CHOOSER(dialog), chooser->font);

  // Modal, so the pick cannot land on something else than what the button showed when it was
  // clicked: nothing else in the application takes input until the dialog is answered.
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
  chooser->dialog = dialog;
  g_signal_connect(G_OBJECT(dialog), "response", G_CALLBACK(_dialog_response), chooser);
  g_signal_connect(G_OBJECT(dialog), "destroy", G_CALLBACK(_dialog_destroyed), chooser);
  gtk_widget_show(dialog);
}

static void _button_destroyed(GtkWidget *button, gpointer user_data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  if(chooser->kind == DT_CHOOSER_COLOR)
    _color_window_close(chooser, DT_CHOOSER_CLOSE_SILENT);
  else
    _dialog_close(chooser, FALSE);
}

/**
 * The swatch, painted in the button's content box. A colour that is not opaque goes over a
 * checkerboard, or a half-transparent black would read as a grey; the outline is the button's own
 * foreground, so a swatch the colour of the panel still shows where it is.
 */
static void _paint_swatch(cairo_t *cr, const gint x, const gint y, const gint w, const gint h, const gint flags,
                          void *data)
{
  const dt_chooser_button_t *chooser = (const dt_chooser_button_t *)data;
  cairo_pattern_t *foreground = cairo_pattern_reference(cairo_get_source(cr));
  const double cell = MAX(w, h) / 4.0;

  cairo_save(cr);
  cairo_rectangle(cr, x, y, w, h);
  cairo_clip(cr);

  if(chooser->color.alpha < 1.0)
  {
    cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
    cairo_paint(cr);
    cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
    for(int row = 0; row * cell < h; row++)
    {
      for(int column = row % 2; column * cell < w; column += 2)
      {
        cairo_rectangle(cr, x + column * cell, y + row * cell, cell, cell);
      }
    }
    cairo_fill(cr);
  }

  cairo_set_source_rgba(cr, chooser->color.red, chooser->color.green, chooser->color.blue, chooser->color.alpha);
  cairo_paint(cr);

  cairo_set_source(cr, foreground);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0);
  cairo_stroke(cr);
  cairo_restore(cr);

  cairo_pattern_destroy(foreground);
}

static dt_chooser_button_t *_chooser_attach(GtkWidget *button, const dt_chooser_kind_t kind, const char *title)
{
  dt_chooser_button_t *chooser = g_malloc0(sizeof(dt_chooser_button_t));
  chooser->kind = kind;
  chooser->button = button;
  chooser->title = g_strdup(IS_NULL_PTR(title) ? "" : title);
  chooser->font = g_strdup("");
  chooser->color.alpha = 1.0;
  // A click must leave the focus where it was. The button would otherwise hold it after the dialog
  // closes and the focus comes back to the window, drawn as focused, and keep the plain keys from a
  // view whose panel blocks them until something else is clicked. The keyboard still reaches it.
  gtk_widget_set_focus_on_click(button, FALSE);
  g_object_set_data_full(G_OBJECT(button), CHOOSER_BUTTON_KEY, chooser, _chooser_free);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_button_clicked), chooser);
  g_signal_connect(G_OBJECT(button), "destroy", G_CALLBACK(_button_destroyed), chooser);
  return chooser;
}

GtkWidget *dt_chooser_button_color_new(const char *title, const gboolean use_alpha, const char *history_key,
                                       dt_chooser_color_changed_t changed, gpointer user_data)
{
  // The paint callback reads its data when the button draws, so the button is created with none
  // and given the chooser once it exists: nothing draws in between.
  GtkWidget *button = dtgtk_button_new(_paint_swatch, 0, NULL);
  dt_chooser_button_t *chooser = _chooser_attach(button, DT_CHOOSER_COLOR, title);
  chooser->use_alpha = use_alpha;
  chooser->history_key = IS_NULL_PTR(history_key) ? NULL : g_strdup(history_key);
  chooser->color_changed = changed;
  chooser->user_data = user_data;
  DTGTK_BUTTON(button)->icon_data = chooser;
  return button;
}

void dt_chooser_button_set_color(GtkWidget *button, const GdkRGBA *color)
{
  dt_chooser_button_t *chooser = _chooser(button);
  if(IS_NULL_PTR(chooser) || IS_NULL_PTR(color) || chooser->kind != DT_CHOOSER_COLOR) return;
  // The window's colour is on the document already, and the caller refilling from it is a step
  // behind the drag: what the window holds wins until it closes.
  if(!IS_NULL_PTR(chooser->dialog) && chooser->reported) return;
  if(gdk_rgba_equal(&chooser->color, color)) return;
  chooser->color = *color;
  if(!IS_NULL_PTR(chooser->well))
  {
    dt_color_well_set_color(chooser->well, color);
    dt_color_well_get_color(chooser->well, &chooser->opened);
  }
  gtk_widget_queue_draw(button);
}

void dt_chooser_button_get_color(GtkWidget *button, GdkRGBA *color)
{
  if(IS_NULL_PTR(color)) return;
  const dt_chooser_button_t *chooser = _chooser(button);
  if(IS_NULL_PTR(chooser))
  {
    const GdkRGBA opaque_black = { 0.0, 0.0, 0.0, 1.0 };
    *color = opaque_black;
    return;
  }
  *color = chooser->color;
}

GtkWidget *dt_chooser_button_font_new(const char *title, const int label_chars, dt_chooser_font_picked_t picked,
                                      gpointer user_data)
{
  GtkWidget *button = gtk_button_new();
  gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE);
  dt_chooser_button_t *chooser = _chooser_attach(button, DT_CHOOSER_FONT, title);
  chooser->font_picked = picked;
  chooser->user_data = user_data;

  // As wide as its character count however long or short the name: the width a label asks for is
  // its natural width, and only a fixed one keeps the row from resizing on every refill.
  GtkWidget *label = gtk_label_new(NULL);
  const int chars = MAX(label_chars, 1);
  gtk_label_set_width_chars(GTK_LABEL(label), chars);
  gtk_label_set_max_width_chars(GTK_LABEL(label), chars);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
  gtk_container_add(GTK_CONTAINER(button), label);
  chooser->label = label;
  return button;
}

void dt_chooser_button_set_font(GtkWidget *button, const char *font)
{
  dt_chooser_button_t *chooser = _chooser(button);
  if(IS_NULL_PTR(chooser) || chooser->kind != DT_CHOOSER_FONT) return;
  const char *wanted = IS_NULL_PTR(font) ? "" : font;
  if(!g_strcmp0(chooser->font, wanted)) return;
  dt_free(chooser->font);
  chooser->font = g_strdup(wanted);

  gchar *shown = NULL;
  if(wanted[0] != '\0')
  {
    PangoFontDescription *description = pango_font_description_from_string(wanted);
    shown = _family_and_style(description);
    pango_font_description_free(description);
  }
  gtk_label_set_text(GTK_LABEL(chooser->label), IS_NULL_PTR(shown) ? "" : shown);
  dt_free(shown);
}

const char *dt_chooser_button_get_font(GtkWidget *button)
{
  const dt_chooser_button_t *chooser = _chooser(button);
  if(IS_NULL_PTR(chooser)) return "";
  return chooser->font;
}

GtkWidget *dt_chooser_button_get_label(GtkWidget *button)
{
  const dt_chooser_button_t *chooser = _chooser(button);
  if(IS_NULL_PTR(chooser)) return NULL;
  return chooser->label;
}

void dt_chooser_button_set_parent(GtkWidget *button, GtkWindow *parent)
{
  dt_chooser_button_t *chooser = _chooser(button);
  if(IS_NULL_PTR(chooser) || chooser->parent == parent) return;
  _chooser_forget_parent(chooser);
  if(IS_NULL_PTR(parent)) return;
  chooser->parent = parent;
  g_object_add_weak_pointer(G_OBJECT(parent), (gpointer *)&chooser->parent);
}

gboolean dt_chooser_button_is_open(GtkWidget *button)
{
  const dt_chooser_button_t *chooser = _chooser(button);
  return !IS_NULL_PTR(chooser) && !IS_NULL_PTR(chooser->dialog);
}

void dt_chooser_button_close(GtkWidget *button)
{
  dt_chooser_button_t *chooser = _chooser(button);
  if(IS_NULL_PTR(chooser)) return;
  if(chooser->kind == DT_CHOOSER_COLOR)
    _color_window_close(chooser, DT_CHOOSER_CLOSE_GIVE_BACK);
  else
    _dialog_close(chooser, TRUE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
