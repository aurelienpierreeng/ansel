/*
    This file is part of darktable,
    Copyright (C) 2009-2016 johannes hanika.
    Copyright (C) 2010 Brian Teague.
    Copyright (C) 2010, 2015 Bruce Guenter.
    Copyright (C) 2010-2013 Henrik Andersson.
    Copyright (C) 2010 Pascal de Bruijn.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2010-2019 Tobias Ellinghaus.
    Copyright (C) 2010 Wyatt Olson.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2013, 2016 Ulrich Pegelow.
    Copyright (C) 2012-2014, 2019-2020 Aldric Renaudin.
    Copyright (C) 2012 James C. McPherson.
    Copyright (C) 2012-2013 José Carlos García Sogo.
    Copyright (C) 2012 Michal Babej.
    Copyright (C) 2012 Michal Fabik.
    Copyright (C) 2012-2015 parafin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2013 Simon Spannagel.
    Copyright (C) 2013-2014 Jérémy Rosen.
    Copyright (C) 2013, 2018-2022 Pascal Obry.
    Copyright (C) 2013 Pierre Le Magourou.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2016 Asma.
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2018 Andreas Schneider.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2019, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2024-2026 Guillaume Stutin.
    
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "widgets/bauhaus.h"
#include "darktable.h"
#include "common/logging.h"
#include "common/selection.h"
#include "control/control.h"
#include "develop/develop.h"

#include "gui/application.h"
#include "views/view.h"

#include <assert.h>
#include <gdk/gdkkeysyms.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include "gui/screen_metrics.h"
#include "control/signal.h"

static dt_control_pointer_input_t _pointer_input = { 0 };

void dt_control_set_pointer_input(const dt_control_pointer_input_t *input)
{
  if(IS_NULL_PTR(input)) return;
  _pointer_input = *input;
}

void dt_control_get_pointer_input(dt_control_pointer_input_t *input)
{
  if(IS_NULL_PTR(input)) return;
  *input = _pointer_input;
}

void dt_control_init(dt_control_t *s)
{
  // same thread as init
  s->gui_thread = pthread_self();
  s->cursor.shape = GDK_LEFT_PTR;
  s->cursor.current_shape = GDK_LEFT_PTR;
  s->cursor.shape_str = NULL;
  s->cursor.current_shape_str = NULL;
  s->cursor.hide = FALSE;
  // s->last_expose_time = dt_get_wtime();
  s->log_pos = s->log_ack = 0;
  s->log_busy = 0;
  s->log_message_timeout_id = 0;
  dt_pthread_mutex_init(&(s->log_mutex), NULL);

  s->toast_pos = s->toast_ack = 0;
  s->toast_busy = 0;
  s->toast_message_timeout_id = 0;
  dt_pthread_mutex_init(&(s->toast_mutex), NULL);

  pthread_cond_init(&s->cond, NULL);
  dt_pthread_mutex_init(&s->cond_mutex, NULL);
  dt_pthread_mutex_init(&s->queue_mutex, NULL);
  dt_pthread_mutex_init(&s->res_mutex, NULL);
  dt_pthread_mutex_init(&s->run_mutex, NULL);
  dt_pthread_mutex_init(&(s->global_mutex), NULL);
  dt_pthread_mutex_init(&(s->progress_system.mutex), NULL);

  // start threads
  dt_control_jobs_init(s);

  s->mouse_over_id = -1;
  s->keyboard_over_id = -1;
  s->cursor.lock = FALSE;
}

/** @brief Human-readable name of a GDK cursor shape, for debug output.
 *
 * @details The returned string is a static literal owned by this table: it must NOT be
 * freed, and it stays valid for the lifetime of the process. This used to g_strdup() its
 * result from a 90-case switch, and neither of the two call sites freed it -- one of them
 * could not, because it selected between the allocated name and a plain literal with a
 * ternary. Static storage removes both the leak and the question.
 *
 * @param cursor the shape to name.
 * @return the enum constant's spelling, never NULL. Unknown values -- GDK gained a shape,
 * or the value is garbage -- give "GDK_CURSOR_UNKNOWN" rather than NULL, because every
 * caller feeds this straight to a "%s" and passing NULL there is undefined behaviour.
 */
static const char *_get_cursor_name(dt_cursor_t cursor)
{
  static const struct
  {
    dt_cursor_t shape;
    const char *name;
  } cursor_names[] = {
  { GDK_X_CURSOR, "GDK_X_CURSOR" },
  { GDK_ARROW, "GDK_ARROW" },
  { GDK_BASED_ARROW_DOWN, "GDK_BASED_ARROW_DOWN" },
  { GDK_BASED_ARROW_UP, "GDK_BASED_ARROW_UP" },
  { GDK_BOAT, "GDK_BOAT" },
  { GDK_BOGOSITY, "GDK_BOGOSITY" },
  { GDK_BOTTOM_LEFT_CORNER, "GDK_BOTTOM_LEFT_CORNER" },
  { GDK_BOTTOM_RIGHT_CORNER, "GDK_BOTTOM_RIGHT_CORNER" },
  { GDK_BOTTOM_SIDE, "GDK_BOTTOM_SIDE" },
  { GDK_BOTTOM_TEE, "GDK_BOTTOM_TEE" },
  { GDK_BOX_SPIRAL, "GDK_BOX_SPIRAL" },
  { GDK_CENTER_PTR, "GDK_CENTER_PTR" },
  { GDK_CIRCLE, "GDK_CIRCLE" },
  { GDK_CLOCK, "GDK_CLOCK" },
  { GDK_COFFEE_MUG, "GDK_COFFEE_MUG" },
  { GDK_CROSS, "GDK_CROSS" },
  { GDK_CROSS_REVERSE, "GDK_CROSS_REVERSE" },
  { GDK_CROSSHAIR, "GDK_CROSSHAIR" },
  { GDK_DIAMOND_CROSS, "GDK_DIAMOND_CROSS" },
  { GDK_DOT, "GDK_DOT" },
  { GDK_DOTBOX, "GDK_DOTBOX" },
  { GDK_DOUBLE_ARROW, "GDK_DOUBLE_ARROW" },
  { GDK_DRAFT_LARGE, "GDK_DRAFT_LARGE" },
  { GDK_DRAFT_SMALL, "GDK_DRAFT_SMALL" },
  { GDK_DRAPED_BOX, "GDK_DRAPED_BOX" },
  { GDK_EXCHANGE, "GDK_EXCHANGE" },
  { GDK_FLEUR, "GDK_FLEUR" },
  { GDK_GOBBLER, "GDK_GOBBLER" },
  { GDK_GUMBY, "GDK_GUMBY" },
  { GDK_HAND1, "GDK_HAND1" },
  { GDK_HAND2, "GDK_HAND2" },
  { GDK_HEART, "GDK_HEART" },
  { GDK_ICON, "GDK_ICON" },
  { GDK_IRON_CROSS, "GDK_IRON_CROSS" },
  { GDK_LEFT_PTR, "GDK_LEFT_PTR" },
  { GDK_LEFT_SIDE, "GDK_LEFT_SIDE" },
  { GDK_LEFT_TEE, "GDK_LEFT_TEE" },
  { GDK_LEFTBUTTON, "GDK_LEFTBUTTON" },
  { GDK_LL_ANGLE, "GDK_LL_ANGLE" },
  { GDK_LR_ANGLE, "GDK_LR_ANGLE" },
  { GDK_MAN, "GDK_MAN" },
  { GDK_MIDDLEBUTTON, "GDK_MIDDLEBUTTON" },
  { GDK_MOUSE, "GDK_MOUSE" },
  { GDK_PENCIL, "GDK_PENCIL" },
  { GDK_PIRATE, "GDK_PIRATE" },
  { GDK_PLUS, "GDK_PLUS" },
  { GDK_QUESTION_ARROW, "GDK_QUESTION_ARROW" },
  { GDK_RIGHT_PTR, "GDK_RIGHT_PTR" },
  { GDK_RIGHT_SIDE, "GDK_RIGHT_SIDE" },
  { GDK_RIGHT_TEE, "GDK_RIGHT_TEE" },
  { GDK_RIGHTBUTTON, "GDK_RIGHTBUTTON" },
  { GDK_RTL_LOGO, "GDK_RTL_LOGO" },
  { GDK_SAILBOAT, "GDK_SAILBOAT" },
  { GDK_SB_DOWN_ARROW, "GDK_SB_DOWN_ARROW" },
  { GDK_SB_H_DOUBLE_ARROW, "GDK_SB_H_DOUBLE_ARROW" },
  { GDK_SB_LEFT_ARROW, "GDK_SB_LEFT_ARROW" },
  { GDK_SB_RIGHT_ARROW, "GDK_SB_RIGHT_ARROW" },
  { GDK_SB_UP_ARROW, "GDK_SB_UP_ARROW" },
  { GDK_SB_V_DOUBLE_ARROW, "GDK_SB_V_DOUBLE_ARROW" },
  { GDK_SHUTTLE, "GDK_SHUTTLE" },
  { GDK_SIZING, "GDK_SIZING" },
  { GDK_SPIDER, "GDK_SPIDER" },
  { GDK_SPRAYCAN, "GDK_SPRAYCAN" },
  { GDK_STAR, "GDK_STAR" },
  { GDK_TARGET, "GDK_TARGET" },
  { GDK_TCROSS, "GDK_TCROSS" },
  { GDK_TOP_LEFT_ARROW, "GDK_TOP_LEFT_ARROW" },
  { GDK_TOP_LEFT_CORNER, "GDK_TOP_LEFT_CORNER" },
  { GDK_TOP_RIGHT_CORNER, "GDK_TOP_RIGHT_CORNER" },
  { GDK_TOP_SIDE, "GDK_TOP_SIDE" },
  { GDK_TOP_TEE, "GDK_TOP_TEE" },
  { GDK_TREK, "GDK_TREK" },
  { GDK_UL_ANGLE, "GDK_UL_ANGLE" },
  { GDK_UMBRELLA, "GDK_UMBRELLA" },
  { GDK_UR_ANGLE, "GDK_UR_ANGLE" },
  { GDK_WATCH, "GDK_WATCH" },
  { GDK_XTERM, "GDK_XTERM" },
  { GDK_LAST_CURSOR, "GDK_LAST_CURSOR" },
  { GDK_BLANK_CURSOR, "GDK_BLANK_CURSOR" },
  { GDK_CURSOR_IS_PIXMAP, "GDK_CURSOR_IS_PIXMAP" },
  };

  for(size_t i = 0; i < sizeof(cursor_names) / sizeof(cursor_names[0]); i++)
    if(cursor_names[i].shape == cursor) return cursor_names[i].name;

  return "GDK_CURSOR_UNKNOWN";
}

void dt_control_forbid_change_cursor()
{
  darktable.control->cursor.lock = TRUE;
}

void dt_control_allow_change_cursor()
{
  darktable.control->cursor.lock = FALSE;
}

static void _control_set_cursor_on_widget(GtkWidget *widget, GdkCursor *cursor)
{
  if(IS_NULL_PTR(widget)) return;

  GdkWindow *window = gtk_widget_get_window(widget);
  if(!IS_NULL_PTR(window)) gdk_window_set_cursor(window, cursor);
}

static void _control_apply_cursor(GdkCursor *cursor)
{
  GtkWidget *main_window = dt_gui_main_window();
  GtkWidget *center_base = dt_ui_center_base(dt_gui_get_ui());
  GtkWidget *center = dt_gui_center_widget();

  _control_set_cursor_on_widget(main_window, cursor);

  if(gtk_widget_get_window(center_base) != gtk_widget_get_window(main_window))
    _control_set_cursor_on_widget(center_base, cursor);

  if(gtk_widget_get_window(center) != gtk_widget_get_window(center_base)
     && gtk_widget_get_window(center) != gtk_widget_get_window(main_window))
    _control_set_cursor_on_widget(center, cursor);
}

static void _control_store_current_cursor(const dt_cursor_t shape, const char *shape_str)
{
  gchar *current_shape_str = g_strdup(shape_str);

  darktable.control->cursor.current_shape = shape;
  g_free(darktable.control->cursor.current_shape_str);
  darktable.control->cursor.current_shape_str = current_shape_str;
}

/** \brief Apply a GTK named cursor without changing the queued cursor.
 *
 * Named cursors report GDK_CURSOR_IS_PIXMAP as their type, which is only a
 * marker for GTK's cursor cache. The queued cursor state is owned by
 * dt_control_queue_cursor_by_name(), so temporary named cursors such as the
 * busy cursor do not overwrite the cursor that should be restored later.
 */
void dt_control_change_cursor_by_name(const char *curs_str)
{
  if(IS_NULL_PTR(curs_str)) return;

  if(!darktable.control->cursor.lock)
  {
    const gboolean hide = darktable.control->cursor.hide;
    const gboolean current_is_named_cursor =
      !IS_NULL_PTR(darktable.control->cursor.current_shape_str)
      && g_strcmp0(darktable.control->cursor.current_shape_str, curs_str) == 0;

    // "progress" is a special cursor that should not overwrite the queued cursor.
    if(g_strcmp0(curs_str, "progress") != 0)
      dt_control_queue_cursor_by_name(curs_str);
    
    if(hide)
    {
      if(IS_NULL_PTR(darktable.control->cursor.current_shape_str)
         && darktable.control->cursor.current_shape == GDK_BLANK_CURSOR)
        return;
    }
    else if(current_is_named_cursor)
      return;

    // We choose the GTK constructor here because named cursors are pixmaps in
    // GTK's cache, while the hidden cursor is one of the standard shapes.
    const dt_cursor_t chosen_shape = hide ? GDK_BLANK_CURSOR : GDK_CURSOR_IS_PIXMAP;
    GdkCursor *cursor_shape = hide
      ? gdk_cursor_new_for_display(gdk_display_get_default(), GDK_BLANK_CURSOR)
      : gdk_cursor_new_from_name(gdk_display_get_default(), curs_str);

    if(IS_NULL_PTR(cursor_shape)) return;
    _control_apply_cursor(cursor_shape);
    _control_store_current_cursor(chosen_shape, hide ? NULL : curs_str);

    if(dt_get_debug_flags() & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_CONTROL, "Changing cursor to `%s`\n", hide ? "GDK_BLANK_CURSOR" : curs_str);

    g_object_unref(cursor_shape);
  }
}

/**
 * @brief Apply a named cursor immediatelly and flush display updates for immediate feedback.
 *
 * @details
 * Some synchronous UI-thread operations perform heavy work right after cursor setup.
 * Without an explicit display flush, GTK may delay painting and the busy cursor can
 * remain invisible until the computation already finished.
 *
 * @param curs_str GTK cursor name (for example `"progress"`).
 */
void dt_control_change_cursor_by_name_and_flush(const char *curs_str)
{
  if(IS_NULL_PTR(curs_str)) return;
  dt_control_change_cursor_by_name(curs_str);
  GdkDisplay *display = gdk_display_get_default();
  if(!IS_NULL_PTR(display)) gdk_display_flush(display);
}

void dt_control_commit_cursor()
{
  //fprintf(stderr, "Committing cursor \n");
  if(darktable.control->log_busy > 0) return;

  if(IS_NULL_PTR(darktable.control->cursor.shape_str))
    dt_control_change_cursor(darktable.control->cursor.shape);
  else
    dt_control_change_cursor_by_name(darktable.control->cursor.shape_str);
}

void dt_control_change_cursor_EXT(dt_cursor_t cursor, const char *file, int line)
{
  const gboolean hide = darktable.control->cursor.hide;

  // GDK_CURSOR_IS_PIXMAP is returned by GTK for named cursors and custom pixmaps.
  // It is not a cursor shape that can be constructed with gdk_cursor_new_for_display().
  const dt_cursor_t requested_shape = cursor == GDK_CURSOR_IS_PIXMAP ? GDK_LEFT_PTR : cursor;
  const dt_cursor_t chosen_shape = hide ? GDK_BLANK_CURSOR : requested_shape;

  // Keep the requested cursor queued even if the visible cursor stays blank.
  dt_control_queue_cursor_EXT(requested_shape, file, line);

  if(IS_NULL_PTR(darktable.control->cursor.current_shape_str)
     && darktable.control->cursor.current_shape == chosen_shape)
    return;

  if(!darktable.control->cursor.lock)
  {
    GdkCursor *cursor_shape = gdk_cursor_new_for_display(gdk_display_get_default(), chosen_shape);
    if(IS_NULL_PTR(cursor_shape)) return;
    _control_apply_cursor(cursor_shape);
    g_object_unref(cursor_shape);
    _control_store_current_cursor(chosen_shape, NULL);

    if(dt_get_debug_flags() & DT_DEBUG_VERBOSE)
      dt_print(DT_DEBUG_CONTROL,
               "Changing cursor to `%s`, requested from %s:%d\n",
               hide ? "GDK_BLANK_CURSOR" : _get_cursor_name(requested_shape), file, line);
  }
}

void dt_control_queue_cursor_EXT(dt_cursor_t cursor, const char *file, int line)
{
  const dt_cursor_t requested_shape = cursor == GDK_CURSOR_IS_PIXMAP ? GDK_LEFT_PTR : cursor;

  if(darktable.control->cursor.shape == requested_shape
     && IS_NULL_PTR(darktable.control->cursor.shape_str))
    return;

  if(dt_get_debug_flags() & DT_DEBUG_VERBOSE)
    dt_print(DT_DEBUG_CONTROL, "Queue cursor to `%s`, requested from %s:%d\n", _get_cursor_name(requested_shape), file, line);

  g_free(darktable.control->cursor.shape_str);
  darktable.control->cursor.shape_str = NULL;
  darktable.control->cursor.shape = requested_shape;
}

/** \brief Queue a GTK named cursor for the next cursor commit.
 *
 * The cursor object is created only to validate the theme name. The queued
 * ownership remains the string because GTK exposes named cursors as
 * GDK_CURSOR_IS_PIXMAP, which cannot be passed back to gdk_cursor_new_for_display().
 */
void dt_control_queue_cursor_by_name(const char *curs_str)
{
  if(IS_NULL_PTR(curs_str)) return;
  // "progress" is a special cursor that should not overwrite the queued cursor.
  // Use dt_change_cursor_by_name() to set it directly without queuing.
  if(g_strcmp0(curs_str, "progress") == 0)
    return;

  if(g_strcmp0(darktable.control->cursor.shape_str, curs_str) == 0) return;

  GdkCursor *cursor = gdk_cursor_new_from_name(gdk_display_get_default(), curs_str);
  if(IS_NULL_PTR(cursor)) return;
  g_object_unref(cursor);

  g_free(darktable.control->cursor.shape_str);
  darktable.control->cursor.shape_str = g_strdup(curs_str);
}

void dt_control_set_cursor_visible_EXT(gboolean visible, const char *file, int line)
{
  if(dt_get_debug_flags() & DT_DEBUG_VERBOSE)
    dt_print(DT_DEBUG_CONTROL, "%s cursor, requested from %s:%d\n", visible ? "Show" : "Hide", file, line);
  darktable.control->cursor.hide = !visible;
}

void dt_control_mouse_is_dragging(gboolean state)
{
  dt_gui_get_global()->mouse.is_dragging = state;
}

void dt_control_mouse_is_painting(gboolean state)
{
  dt_gui_get_global()->mouse.is_painting = state;
}

int dt_control_running()
{
  // FIXME: when shutdown, run_mutex is not inited anymore!
  dt_control_t *s = darktable.control;
  dt_pthread_mutex_lock(&s->run_mutex);
  int running = s->running;
  dt_pthread_mutex_unlock(&s->run_mutex);
  return running;
}

void dt_control_quit()
{
  dt_gui_gtk_quit();
  // thread safe quit, 1st pass:
  dt_pthread_mutex_lock(&darktable.control->cond_mutex);
  dt_pthread_mutex_lock(&darktable.control->run_mutex);
  darktable.control->running = 0;
  dt_pthread_mutex_unlock(&darktable.control->run_mutex);
  dt_pthread_mutex_unlock(&darktable.control->cond_mutex);

  if(gtk_main_level() > 0) gtk_main_quit();
}

void dt_control_shutdown(dt_control_t *s)
{
  dt_pthread_mutex_lock(&s->cond_mutex);
  dt_pthread_mutex_lock(&s->run_mutex);
  s->running = 0;
  dt_pthread_mutex_unlock(&s->run_mutex);
  dt_pthread_mutex_unlock(&s->cond_mutex);
  pthread_cond_broadcast(&s->cond);

  /* then wait for kick_on_workers_thread */
  pthread_join(s->kick_on_workers_thread, NULL);

  int k;
  for(k = 0; k < s->num_threads; k++)
    // pthread_kill(s->thread[k], 9);
    pthread_join(s->thread[k], NULL);
  for(k = 0; k < DT_CTL_WORKER_RESERVED; k++)
    // pthread_kill(s->thread_res[k], 9);
    pthread_join(s->thread_res[k], NULL);

  // Workers are joined: nothing else touches the queues now. Dispose every still-queued job here,
  // while the plug-in .so files that own their callbacks (state_changed_cb, params_destroy) are
  // still loaded. dt_control_cleanup() -> dt_control_jobs_cleanup() runs much later, after
  // dt_lib_cleanup() has dlclose()'d those modules, so cancelling a leftover job there jumps into
  // an unmapped callback and crashes (e.g. a pending textnotes-load job -> SIGSEGV).
  dt_control_jobs_drain(s);
}

void dt_control_cleanup(dt_control_t *s)
{
  // A vacuum used to be contemplated here. It belongs to whoever owns the connection, and
  // src/database now does it -- VACUUM + ANALYZE on both schemas, on the maintenance policy
  // the user configured. Nothing for the control layer to decide.
  dt_control_jobs_cleanup(s);
  g_free(s->cursor.shape_str);
  g_free(s->cursor.current_shape_str);
  dt_pthread_mutex_destroy(&s->queue_mutex);
  dt_pthread_mutex_destroy(&s->cond_mutex);
  dt_pthread_mutex_destroy(&s->log_mutex);
  dt_pthread_mutex_destroy(&s->toast_mutex);
  dt_pthread_mutex_destroy(&s->res_mutex);
  dt_pthread_mutex_destroy(&s->run_mutex);
  dt_pthread_mutex_destroy(&s->progress_system.mutex);
}


// ================================================================================
//  gui functions:
// ================================================================================

gboolean dt_control_configure(GtkWidget *da, GdkEventConfigure *event, gpointer user_data)
{
  // re-configure all components:
  dt_view_manager_configure(darktable.view_manager, event->width, event->height);
  return TRUE;
}

static GdkRGBA lookup_color(GtkStyleContext *context, const char *name)
{
  GdkRGBA color, fallback = {1.0, 0.0, 0.0, 1.0};
  if(!gtk_style_context_lookup_color (context, name, &color))
    color = fallback;
  return color;
}

void dt_control_draw_busy_msg(cairo_t *cr, int width, int height)
{
  PangoRectangle ink;
  PangoLayout *layout;
  PangoFontDescription *desc = pango_font_description_copy_static(dt_bauhaus_get_global()->pango_font_desc);
  const float fontsize = DT_PIXEL_APPLY_DPI(14);
  pango_font_description_set_absolute_size(desc, fontsize * PANGO_SCALE);
  pango_font_description_set_weight(desc, PANGO_WEIGHT_BOLD);
  layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, desc);
  // A COPY: a worker thread frees this string on the next dt_set_main_message(), which the
  // pipeline issues once per module per frame while the darkroom renders.
  char *const message = dt_get_main_message_copy();
  pango_layout_set_text(layout, !IS_NULL_PTR(message) ? message : _("Working..."), -1);
  dt_free(message);
  pango_layout_get_pixel_extents(layout, &ink, NULL);
  if(ink.width > width * 0.98)
  {
    pango_layout_set_text(layout, "...", -1);
    pango_layout_get_pixel_extents(layout, &ink, NULL);
  }
  const float xc = width / 2.0, yc = height * 0.85 - DT_PIXEL_APPLY_DPI(30), wd = ink.width * .5f;
  cairo_move_to(cr, xc - wd, yc + 1. / 3. * fontsize - fontsize);
  pango_cairo_layout_path(cr, layout);
  cairo_set_line_width(cr, 2.0);
  dt_widget_set_source_rgb(cr, DT_GUI_COLOR_LOG_BG);
  cairo_stroke_preserve(cr);
  dt_widget_set_source_rgb(cr, DT_GUI_COLOR_LOG_FG);
  cairo_fill(cr);
  pango_font_description_free(desc);
  g_object_unref(layout);
}

gboolean dt_control_button_down(int which)
{
  // The mask is device state, so any of our windows answers the same thing; the centre widget
  // is the one every caller is dragging over.
  GtkWidget *center = dt_gui_center_widget();
  GdkWindow *window = IS_NULL_PTR(center) ? NULL : gtk_widget_get_window(center);
  GdkDisplay *display = IS_NULL_PTR(window) ? NULL : gdk_window_get_display(window);
  GdkSeat *seat = IS_NULL_PTR(display) ? NULL : gdk_display_get_default_seat(display);
  GdkDevice *pointer = IS_NULL_PTR(seat) ? NULL : gdk_seat_get_pointer(seat);
  if(IS_NULL_PTR(window) || IS_NULL_PTR(pointer)) return FALSE;

  GdkModifierType mask = 0;
  gdk_window_get_device_position(window, pointer, NULL, NULL, &mask);

  switch(which)
  {
    case 1:  return (mask & GDK_BUTTON1_MASK) != 0;
    case 2:  return (mask & GDK_BUTTON2_MASK) != 0;
    case 3:  return (mask & GDK_BUTTON3_MASK) != 0;
    default: return FALSE;
  }
}

void *dt_control_expose(void *voidptr)
{
  int pointerx, pointery;
  if(IS_NULL_PTR(dt_gui_get_global()->surface)) return NULL;
  const int width = dt_cairo_image_surface_get_width(dt_gui_get_global()->surface);
  const int height = dt_cairo_image_surface_get_height(dt_gui_get_global()->surface);
  GtkWidget *widget = dt_gui_center_widget();
  gdk_window_get_device_position(gtk_widget_get_window(widget),
      gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))),
      &pointerx, &pointery, NULL);

  // create a gtk-independent surface to draw on
  cairo_surface_t *cst = dt_cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
  cairo_t *cr = cairo_create(cst);

  // TODO: control_expose: only redraw the part not overlapped by temporary control panel show!
  //
  darktable.control->width = width;
  darktable.control->height = height;

  GtkStyleContext *context = gtk_widget_get_style_context(widget);

  // look up some colors once
  GdkRGBA bg_color = lookup_color(context, "bg_color");

  gdk_cairo_set_source_rgba(cr, &bg_color);
  cairo_save(cr);
  cairo_rectangle(cr, 0, 0, width, height);
  cairo_paint(cr);
  cairo_clip(cr);
  cairo_new_path(cr);
  // draw view
  dt_view_manager_expose(darktable.view_manager, cr, width, height, pointerx, pointery);
  cairo_restore(cr);

  // draw busy indicator
  dt_pthread_mutex_lock(&darktable.control->log_mutex);

  if(darktable.control->log_busy > 0)
  {
    dt_control_draw_busy_msg(cr, width, height);
    // force set the cursor to arrow with busy indicator
    dt_control_change_cursor_by_name("progress");
  }
  else // Apply cursor change
    dt_control_commit_cursor();

  dt_pthread_mutex_unlock(&darktable.control->log_mutex);

  // Draw progress bar (guard during early startup)
  if(darktable.develop && darktable.develop->progress.total > 0)
  {
    const float progress_h = DT_PIXEL_APPLY_DPI(5);
    cairo_rectangle(cr, 0, height - progress_h,
      width * (float)darktable.develop->progress.completed / (float)darktable.develop->progress.total,
      progress_h);
    cairo_set_source_rgba(cr, 0., 0., 0., 0.33);
    cairo_fill(cr);
  }
  cairo_destroy(cr);

  cairo_t *cr_pixmap = cairo_create(dt_gui_get_global()->surface);
  cairo_set_source_surface(cr_pixmap, cst, 0, 0);
  cairo_paint(cr_pixmap);
  cairo_destroy(cr_pixmap);

  cairo_surface_destroy(cst);
  return NULL;
}

void dt_control_mouse_leave()
{
  dt_view_manager_mouse_leave(darktable.view_manager);
}

void dt_control_mouse_enter()
{
  dt_view_manager_mouse_enter(darktable.view_manager);
}

void dt_control_mouse_moved(double x, double y, double pressure, int which)
{
  dt_view_manager_mouse_moved(darktable.view_manager, x, y, pressure, which);
}

void dt_control_key_pressed(GdkEventKey *event)
{
  dt_view_manager_key_pressed(darktable.view_manager, event);
}

void dt_control_button_released(double x, double y, int which, uint32_t state)
{

  dt_view_manager_button_released(darktable.view_manager, x, y, which, state);
}

static void _dt_ctl_switch_mode_prepare()
{
  dt_gui_get_global()->center_tooltip = 0;
  GtkWidget *widget = dt_gui_center_widget();
  gtk_widget_set_tooltip_text(widget, "");
}

static gboolean _dt_ctl_switch_mode_to(gpointer user_data)
{
  const char *mode = (const char*)user_data;
  _dt_ctl_switch_mode_prepare();
  dt_view_manager_switch(darktable.view_manager, mode);
  return FALSE;
}

static gboolean _dt_ctl_switch_mode_to_by_view(gpointer user_data)
{
  const dt_view_t *view = (const dt_view_t*)user_data;
  _dt_ctl_switch_mode_prepare();
  dt_view_manager_switch_by_view(darktable.view_manager, view);
  return FALSE;
}

void dt_ctl_switch_mode_to(const char *mode)
{
  const dt_view_t *current_view = dt_view_manager_get_current_view(darktable.view_manager);
  if(current_view && !strcmp(mode, current_view->module_name))
  {
    // if we are not in lighttable, we switch back to that view
    if(strcmp(current_view->module_name, "lighttable")) dt_ctl_switch_mode_to("lighttable");
    return;
  }

  g_main_context_invoke(NULL, _dt_ctl_switch_mode_to, (gpointer)mode);
}

void dt_ctl_switch_mode_to_by_view(const dt_view_t *view)
{
  if(view == dt_view_manager_get_current_view(darktable.view_manager)) return;
  g_main_context_invoke(NULL, _dt_ctl_switch_mode_to_by_view, (gpointer)view);
}

static gboolean _dt_ctl_open_image_in_darkroom(gpointer user_data)
{
  const int32_t imgid = GPOINTER_TO_INT(user_data);
  _dt_ctl_switch_mode_prepare();

  // Round-trip through the lighttable, the same way the darkroom changes image from the
  // filmstrip (_dev_change_image() in views/darkroom.c): everything is torn down and re-inited
  // through the regular enter()/leave() path instead of a partial in-place reload. Skipped when
  // the lighttable is already the current view -- there is no darkroom state to tear down then,
  // and re-entering it would needlessly rebuild the grid and the whole panel layout.
  const dt_view_t *current_view = dt_view_manager_get_current_view(darktable.view_manager);
  if(IS_NULL_PTR(current_view) || g_strcmp0(current_view->module_name, "lighttable"))
    dt_view_manager_switch(darktable.view_manager, "lighttable");

  // Publish the target AFTER leaving the darkroom, and from the GUI thread: the darkroom's
  // leave() restores the selection to the image it was editing, and any pointer motion over the
  // grid rewrites the mouse-over id. Both would silently overwrite a target published earlier
  // from a worker thread, and the darkroom would re-open the previous image instead.
  dt_control_set_mouse_over_id(imgid);
  dt_selection_select_single(dt_selection_get_global(), imgid);

  dt_view_manager_switch(darktable.view_manager, "darkroom");
  return FALSE;
}

void dt_ctl_open_image_in_darkroom(const int32_t imgid)
{
  g_main_context_invoke(NULL, _dt_ctl_open_image_in_darkroom, GINT_TO_POINTER(imgid));
}

static gboolean _dt_ctl_log_message_timeout_callback(gpointer data)
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  if(darktable.control->log_ack != darktable.control->log_pos)
    darktable.control->log_ack = (darktable.control->log_ack + 1) % DT_CTL_LOG_SIZE;
  darktable.control->log_message_timeout_id = 0;
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_log_redraw();
  return FALSE;
}

static gboolean _dt_ctl_toast_message_timeout_callback(gpointer data)
{
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  if(darktable.control->toast_ack != darktable.control->toast_pos)
    darktable.control->toast_ack = (darktable.control->toast_ack + 1) % DT_CTL_TOAST_SIZE;
  darktable.control->toast_message_timeout_id = 0;
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
  dt_control_toast_redraw();
  return FALSE;
}

void dt_control_button_pressed(double x, double y, double pressure, int which, int type, uint32_t state)
{
  darktable.control->button_x = x;
  darktable.control->button_y = y;
  // adding pressure to this data structure is not needed right now. should the need ever arise: here is the
  // place to do it :)
  //const float wd = darktable.control->width;
  const float ht = darktable.control->height;

  // ack log message:
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  const float /*xc = wd/4.0-20,*/ yc = ht * 0.85 + 10;
  if(darktable.control->log_ack != darktable.control->log_pos)
    if(which == 1 /*&& x > xc - 10 && x < xc + 10*/ && y > yc - 10 && y < yc + 10)
    {
      if(darktable.control->log_message_timeout_id)
      {
        g_source_remove(darktable.control->log_message_timeout_id);
        darktable.control->log_message_timeout_id = 0;
      }
      darktable.control->log_ack = (darktable.control->log_ack + 1) % DT_CTL_LOG_SIZE;
      dt_pthread_mutex_unlock(&darktable.control->log_mutex);
      return;
    }
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);

  // ack toast message:
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  if(darktable.control->toast_ack != darktable.control->toast_pos)
    if(which == 1 /*&& x > xc - 10 && x < xc + 10*/ && y > yc - 10 && y < yc + 10)
    {
      if(darktable.control->toast_message_timeout_id)
      {
        g_source_remove(darktable.control->toast_message_timeout_id);
        darktable.control->toast_message_timeout_id = 0;
      }
      darktable.control->toast_ack = (darktable.control->toast_ack + 1) % DT_CTL_TOAST_SIZE;
      dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
      return;
    }
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);

  dt_view_manager_button_pressed(darktable.view_manager, x, y, pressure, which, type, state);
}

static gboolean _redraw_center(gpointer user_data)
{
  dt_control_log_redraw();
  dt_control_toast_redraw();
  return FALSE; // don't call this again
}

void dt_control_log(const char *msg, ...)
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  va_list ap;
  va_start(ap, msg);
  char *escaped_msg = g_markup_vprintf_escaped(msg, ap);
  const int msglen = strlen(escaped_msg);
  g_strlcpy(darktable.control->log_message[darktable.control->log_pos], escaped_msg, DT_CTL_LOG_MSG_SIZE);
  dt_free(escaped_msg);
  va_end(ap);
  if(darktable.control->log_message_timeout_id)
    g_source_remove(darktable.control->log_message_timeout_id);
  darktable.control->log_ack = darktable.control->log_pos;
  darktable.control->log_pos = (darktable.control->log_pos + 1) % DT_CTL_LOG_SIZE;

  darktable.control->log_message_timeout_id
    = g_timeout_add(DT_CTL_LOG_TIMEOUT + 1000 * (msglen / 40),
                    _dt_ctl_log_message_timeout_callback, NULL);
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  // redraw center later in gui thread:
  g_idle_add(_redraw_center, 0);
}

static void _toast_log(const gboolean markup, const char *msg, va_list ap)
{
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);

  // if we don't want markup, we escape <>&... so they are not interpreted later
  if(markup)
    vsnprintf(darktable.control->toast_message[darktable.control->toast_pos], DT_CTL_TOAST_MSG_SIZE, msg, ap);
  else
  {
    char *escaped_msg = g_markup_vprintf_escaped(msg, ap);
    g_strlcpy(darktable.control->toast_message[darktable.control->toast_pos], escaped_msg, DT_CTL_TOAST_MSG_SIZE);
    dt_free(escaped_msg);
  }

  if(darktable.control->toast_message_timeout_id) g_source_remove(darktable.control->toast_message_timeout_id);
  darktable.control->toast_ack = darktable.control->toast_pos;
  darktable.control->toast_pos = (darktable.control->toast_pos + 1) % DT_CTL_TOAST_SIZE;
  darktable.control->toast_message_timeout_id
      = g_timeout_add(DT_CTL_TOAST_TIMEOUT, _dt_ctl_toast_message_timeout_callback, NULL);
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
  // redraw center later in gui thread:
  g_idle_add(_redraw_center, 0);
}

void dt_toast_log(const char *msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  _toast_log(FALSE, msg, ap);
  va_end(ap);
}

void dt_toast_markup_log(const char *msg, ...)
{
  va_list ap;
  va_start(ap, msg);
  _toast_log(TRUE, msg, ap);
  va_end(ap);
}

void dt_control_log_busy_enter()
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  darktable.control->log_busy++;
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_redraw_center();
}

void dt_control_toast_busy_enter()
{
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  darktable.control->toast_busy++;
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
  dt_control_queue_redraw_center();
}

void dt_control_log_busy_leave()
{
  dt_pthread_mutex_lock(&darktable.control->log_mutex);
  darktable.control->log_busy--;
  dt_pthread_mutex_unlock(&darktable.control->log_mutex);
  dt_control_queue_redraw_center();
}

void dt_control_toast_busy_leave()
{
  dt_pthread_mutex_lock(&darktable.control->toast_mutex);
  darktable.control->toast_busy--;
  dt_pthread_mutex_unlock(&darktable.control->toast_mutex);
  dt_control_queue_redraw_center();
}

void dt_control_queue_redraw()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_REDRAW_ALL);
}

void dt_control_queue_redraw_center()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_REDRAW_CENTER);
}

void dt_control_navigation_redraw()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_NAVIGATION_REDRAW);
}

void dt_control_log_redraw()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_LOG_REDRAW);
}

void dt_control_toast_redraw()
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_TOAST_REDRAW);
}

typedef struct dt_control_redraw_widget_t
{
  GWeakRef widget_ref;
} dt_control_redraw_widget_t;

static int _widget_queue_draw(void *widget_data)
{
  dt_control_redraw_widget_t *const redraw_widget = (dt_control_redraw_widget_t *)widget_data;
  GtkWidget *const widget = GTK_WIDGET(g_weak_ref_get(&redraw_widget->widget_ref));
  if(!IS_NULL_PTR(widget))
  {
    gtk_widget_queue_draw(widget);
    g_object_unref(widget);
  }

  return FALSE;
}

static void _widget_queue_draw_cleanup(void *widget_data)
{
  dt_control_redraw_widget_t *const redraw_widget = (dt_control_redraw_widget_t *)widget_data;
  g_weak_ref_clear(&redraw_widget->widget_ref);
  dt_free(redraw_widget);
}

void dt_control_queue_redraw_widget(GtkWidget *widget)
{
  if(dt_control_running())
  {
    dt_control_redraw_widget_t *const redraw_widget = calloc(1, sizeof(dt_control_redraw_widget_t));
    if(IS_NULL_PTR(redraw_widget)) return;

    g_weak_ref_init(&redraw_widget->widget_ref, G_OBJECT(widget));
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, _widget_queue_draw, redraw_widget, _widget_queue_draw_cleanup);
  }
}

void dt_control_hinter_message(const struct dt_control_t *s, const char *message)
{
  dt_hinter_set_message(dt_gui_get_ui(), message);
}

int32_t dt_control_get_mouse_over_id()
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  const int32_t result = darktable.control->mouse_over_id;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
  return result;
}

void dt_control_set_mouse_over_id(int32_t value)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  if(darktable.control->mouse_over_id != value)
  {
    darktable.control->mouse_over_id = value;

    // If we reset mouse_over_id to -1, aka "none" signal,
    // reset also the keyboard_over_id, in a "loose focus" way,
    // to keep only the selection for common/act_on.h
    if(value < 0) darktable.control->keyboard_over_id = value;
    dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
  }
  else
    dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}

int32_t dt_control_get_keyboard_over_id()
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  const int32_t result = darktable.control->keyboard_over_id;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
  return result;
}

void dt_control_set_keyboard_over_id(int32_t value)
{
  dt_pthread_mutex_lock(&(darktable.control->global_mutex));
  darktable.control->keyboard_over_id = value;
  dt_pthread_mutex_unlock(&(darktable.control->global_mutex));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
