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
#include "widgets/dialog.h"           // dt_gui_refocus_parent
#include "widgets/widget_settings.h"  // dt_widget_root_window

#define CHOOSER_BUTTON_KEY "dt-chooser-button"

typedef enum dt_chooser_kind_t
{
  DT_CHOOSER_COLOR = 0,
  DT_CHOOSER_FONT = 1,
} dt_chooser_kind_t;

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
  GtkWidget *dialog;  ///< the open chooser, NULL when there is none
  GtkWindow *parent;  ///< weak: NULLed by GObject when the window goes
  gchar *title;
  gboolean use_alpha;
  GdkRGBA color;
  gchar *font;        ///< never NULL
  dt_chooser_color_picked_t color_picked;
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

static void _chooser_free(gpointer data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)data;
  _chooser_forget_parent(chooser);
  dt_free(chooser->title);
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
  if(chooser->dialog == dialog) chooser->dialog = NULL;
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
  const dt_chooser_kind_t kind = chooser->kind;
  GtkWidget *button = chooser->button;
  const dt_chooser_color_picked_t color_picked = chooser->color_picked;
  const dt_chooser_font_picked_t font_picked = chooser->font_picked;
  const gpointer picked_data = chooser->user_data;

  // Read the answer before the dialog goes, and close it before reporting: a caller acting on the
  // pick may refill this very button, or close its whole panel, and must find no dialog left open.
  GdkRGBA picked_color = { 0.0, 0.0, 0.0, 1.0 };
  gchar *picked_font = NULL;
  if(confirmed && kind == DT_CHOOSER_COLOR)
  {
    gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &picked_color);
  }
  else if(confirmed && kind == DT_CHOOSER_FONT)
  {
    PangoFontDescription *description = gtk_font_chooser_get_font_desc(GTK_FONT_CHOOSER(dialog));
    picked_font = _family_and_style(description);
    if(!IS_NULL_PTR(description)) pango_font_description_free(description);
  }

  _dialog_close(chooser, TRUE);

  // Nothing of `chooser` is read from here on: the callback may destroy the button, and it with it.
  if(confirmed && kind == DT_CHOOSER_COLOR && !IS_NULL_PTR(color_picked))
    color_picked(button, &picked_color, picked_data);
  else if(confirmed && kind == DT_CHOOSER_FONT && !IS_NULL_PTR(font_picked))
    font_picked(button, picked_font, picked_data);
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

static void _button_clicked(GtkButton *button, gpointer user_data)
{
  dt_chooser_button_t *chooser = (dt_chooser_button_t *)user_data;
  if(!IS_NULL_PTR(chooser->dialog))
  {
    gtk_window_present(GTK_WINDOW(chooser->dialog));
    return;
  }

  GtkWindow *parent = _dialog_parent(chooser);
  GtkWidget *dialog = NULL;
  if(chooser->kind == DT_CHOOSER_COLOR)
  {
    dialog = gtk_color_chooser_dialog_new(chooser->title, parent);
    gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(dialog), chooser->use_alpha);
    gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(dialog), &chooser->color);
  }
  else
  {
    dialog = gtk_font_chooser_dialog_new(chooser->title, parent);
    // The size is set elsewhere, next to the face, and a face picked here must not change it.
    gtk_font_chooser_set_level(GTK_FONT_CHOOSER(dialog),
                               GTK_FONT_CHOOSER_LEVEL_FAMILY | GTK_FONT_CHOOSER_LEVEL_STYLE);
    if(chooser->font[0] != '\0') gtk_font_chooser_set_font(GTK_FONT_CHOOSER(dialog), chooser->font);
  }

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
  _dialog_close((dt_chooser_button_t *)user_data, FALSE);
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

GtkWidget *dt_chooser_button_color_new(const char *title, const gboolean use_alpha, dt_chooser_color_picked_t picked,
                                       gpointer user_data)
{
  // The paint callback reads its data when the button draws, so the button is created with none
  // and given the chooser once it exists: nothing draws in between.
  GtkWidget *button = dtgtk_button_new(_paint_swatch, 0, NULL);
  dt_chooser_button_t *chooser = _chooser_attach(button, DT_CHOOSER_COLOR, title);
  chooser->use_alpha = use_alpha;
  chooser->color_picked = picked;
  chooser->user_data = user_data;
  DTGTK_BUTTON(button)->icon_data = chooser;
  return button;
}

void dt_chooser_button_set_color(GtkWidget *button, const GdkRGBA *color)
{
  dt_chooser_button_t *chooser = _chooser(button);
  if(IS_NULL_PTR(chooser) || IS_NULL_PTR(color) || chooser->kind != DT_CHOOSER_COLOR) return;
  if(gdk_rgba_equal(&chooser->color, color)) return;
  chooser->color = *color;
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
  _dialog_close(chooser, TRUE);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
