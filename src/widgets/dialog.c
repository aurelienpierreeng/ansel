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

#include "widgets/dialog.h"

#include "widgets/accelerators.h"     // dt_accels_disable around a modal text entry
#include "widgets/widget_settings.h"  // DT_GUI_BOX_SPACING, DT_PIXEL_APPLY_DPI, root window
#include "system/mem_alloc.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

typedef struct result_t
{
  enum {RESULT_NONE, RESULT_NO, RESULT_YES} result;
  char *entry_text;
  GtkWidget *window, *entry, *button_yes, *button_no;
} result_t;

typedef struct _three_choice_result_t
{
  int result;
  GtkWidget *window, *button_first, *button_second, *button_third;
} _three_choice_result_t;

static void _gtk_main_quit_safe(GtkWidget *widget, gpointer data)
{
  (void)widget;
  (void)data;
  if(gtk_main_level() > 0) gtk_main_quit();
}

static void _yes_no_button_handler(GtkButton *button, gpointer data)
{
  result_t *result = (result_t *)data;

  if((void *)button == (void *)result->button_yes)
    result->result = RESULT_YES;
  else if((void *)button == (void *)result->button_no)
    result->result = RESULT_NO;

  if(result->entry)
    result->entry_text = g_strdup(gtk_entry_get_text(GTK_ENTRY(result->entry)));
  gtk_widget_destroy(result->window);
  _gtk_main_quit_safe(NULL, NULL);
}

void dt_gui_refocus_parent(GtkWindow *parent)
{
  if(!GTK_IS_WINDOW(parent))
  {
    GtkWidget *main_window = dt_widget_root_window();
    if(GTK_IS_WINDOW(main_window)) parent = GTK_WINDOW(main_window);
  }

  if(GTK_IS_WINDOW(parent)) gtk_window_present(parent);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_focus_window();
#endif
}

gboolean dt_gui_show_standalone_yes_no_dialog(const char *title, const char *markup, const char *no_text,
                                              const char *yes_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  // before the CSS theme is loaded there is no styling at all, so pad by hand
  const int padding = dt_widget_theme_loaded() ? 0 : 5;

  gtk_window_set_icon_name(GTK_WINDOW(window), "ansel");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(_gtk_main_quit_safe), NULL);

  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

  {
    GtkWidget *main_window = dt_widget_root_window();
    if(GTK_IS_WINDOW(main_window))
    {
      GtkWindow *win = GTK_WINDOW(main_window);
      gtk_window_set_transient_for(GTK_WINDOW(window), win);
      gtk_window_set_modal(GTK_WINDOW(window), TRUE);
      if(gtk_widget_get_visible(GTK_WIDGET(win)))
      {
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
      }
    }
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *mhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(vbox), mhbox, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING), TRUE, TRUE, padding);
  }

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(mhbox), label, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING), TRUE, TRUE, padding);
  }

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  result_t result = {.result = RESULT_NONE, .window = window};

  GtkWidget *button;

  if(no_text)
  {
    button = gtk_button_new_with_label(no_text);
    result.button_no = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(yes_text)
  {
    button = gtk_button_new_with_label(yes_text);
    result.button_yes = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  return result.result == RESULT_YES;
}

static void _three_choice_button_handler(GtkButton *button, gpointer data)
{
  _three_choice_result_t *result = (_three_choice_result_t *)data;

  if((void *)button == (void *)result->button_first)
    result->result = 0;
  else if((void *)button == (void *)result->button_second)
    result->result = 1;
  else if((void *)button == (void *)result->button_third)
    result->result = 2;

  gtk_widget_destroy(result->window);
  _gtk_main_quit_safe(NULL, NULL);
}

int dt_gui_show_standalone_three_choice_dialog(const char *title, const char *markup, const char *first_text,
                                               const char *second_text, const char *third_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  // before the CSS theme is loaded there is no styling at all, so pad by hand
  const int padding = dt_widget_theme_loaded() ? 0 : 5;

  gtk_window_set_icon_name(GTK_WINDOW(window), "ansel");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(_gtk_main_quit_safe), NULL);

  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

  {
    GtkWidget *main_window = dt_widget_root_window();
    if(GTK_IS_WINDOW(main_window))
    {
      GtkWindow *win = GTK_WINDOW(main_window);
      gtk_window_set_transient_for(GTK_WINDOW(window), win);
      gtk_window_set_modal(GTK_WINDOW(window), TRUE);
      if(gtk_widget_get_visible(GTK_WIDGET(win)))
      {
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
      }
    }
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *mhbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(vbox), mhbox, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING), TRUE, TRUE, padding);
  }

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(mhbox), label, TRUE, TRUE, padding);

  if(padding)
  {
    gtk_box_pack_start(GTK_BOX(mhbox),
                       gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING), TRUE, TRUE, padding);
  }

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  _three_choice_result_t result = { .result = -1, .window = window };

  GtkWidget *button;

  if(first_text)
  {
    button = gtk_button_new_with_label(first_text);
    result.button_first = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_three_choice_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(second_text)
  {
    button = gtk_button_new_with_label(second_text);
    result.button_second = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_three_choice_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(third_text)
  {
    button = gtk_button_new_with_label(third_text);
    result.button_third = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_three_choice_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  return result.result;
}

char *dt_gui_show_standalone_string_dialog(const char *title, const char *markup, const char *placeholder,
                                           const char *no_text, const char *yes_text)
{
  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(window);
#endif

  gtk_window_set_icon_name(GTK_WINDOW(window), "ansel");
  gtk_window_set_title(GTK_WINDOW(window), title);
  g_signal_connect(window, "destroy", G_CALLBACK(_gtk_main_quit_safe), NULL);

  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_MOUSE);

  {
    GtkWidget *main_window = dt_widget_root_window();
    if(GTK_IS_WINDOW(main_window))
    {
      GtkWindow *win = GTK_WINDOW(main_window);
      gtk_window_set_transient_for(GTK_WINDOW(window), win);
      if(gtk_widget_get_visible(GTK_WIDGET(win)))
      {
        gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER_ON_PARENT);
      }
    }
  }

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_margin_start(vbox, 10);
  gtk_widget_set_margin_end(vbox, 10);
  gtk_widget_set_margin_top(vbox, 7);
  gtk_widget_set_margin_bottom(vbox, 5);
  gtk_container_add(GTK_CONTAINER(window), vbox);

  GtkWidget *label = gtk_label_new(NULL);
  gtk_label_set_markup(GTK_LABEL(label), markup);
  gtk_box_pack_start(GTK_BOX(vbox), label, TRUE, TRUE, 0);

  GtkWidget *entry = gtk_entry_new();
  dt_accels_disconnect_on_text_input(entry);

  g_object_ref(entry);
  if(placeholder)
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
  gtk_box_pack_start(GTK_BOX(vbox), entry, TRUE, TRUE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_margin_top(hbox, 10);
  gtk_box_pack_start(GTK_BOX(vbox), hbox, TRUE, TRUE, 0);

  result_t result = {.result = RESULT_NONE, .window = window, .entry = entry};

  GtkWidget *button;

  if(no_text)
  {
    button = gtk_button_new_with_label(no_text);
    result.button_no = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  if(yes_text)
  {
    button = gtk_button_new_with_label(yes_text);
    result.button_yes = button;
    g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_yes_no_button_handler), &result);
    gtk_box_pack_start(GTK_BOX(hbox), button, TRUE, TRUE, 0);
  }

  gtk_widget_show_all(window);
  gtk_main();

  if(result.result == RESULT_YES)
    return result.entry_text;

  dt_free(result.entry_text);
  return NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
