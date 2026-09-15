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

#include "widgets/color_well.h"

#include "system/macros.h"            // IS_NULL_PTR
#include "system/mem_alloc.h"         // dt_free
#include "widgets/widget_settings.h"  // DT_PIXEL_APPLY_DPI, dt_widget_stored_string
#include "widgets/widget_style.h"     // dt_gui_add_class

#include <glib/gi18n.h>
#include <math.h>
#include <string.h>

#define COLOR_WELL_KEY "dt-color-well"

/** The field of saturation and value, in logical pixels. */
#define COLOR_WELL_FIELD_WIDTH 208
#define COLOR_WELL_FIELD_HEIGHT 150
/** How wide the hue strip is, and how tall the opacity strip and the recent colours are. */
#define COLOR_WELL_STRIP_PIXELS 18
#define COLOR_WELL_HISTORY_PIXELS 20
#define COLOR_WELL_GAP_PIXELS 6
/** The previous and the current colour, side by side. */
#define COLOR_WELL_PREVIEW_WIDTH 40
/** A square of the checkerboard a transparent colour is shown over. */
#define COLOR_WELL_CHECKER_PIXELS 4
/** The ring marking the colour in the field. */
#define COLOR_WELL_MARKER_RADIUS 5.0

/** What a held button is dragging. */
typedef enum color_well_drag_t
{
  COLOR_WELL_DRAG_NONE = 0,
  COLOR_WELL_DRAG_FIELD,
  COLOR_WELL_DRAG_HUE,
  COLOR_WELL_DRAG_ALPHA,
} color_well_drag_t;

/**
 * The well's state, attached to its root and freed with it. The colour is held as hue,
 * saturation and value rather than as RGB, since a grey has no hue of its own: the hue strip keeps
 * the position the user left it at while the field is dragged down to black and back.
 */
typedef struct dt_color_well_t
{
  GtkWidget *root;
  GtkWidget *history_area;
  GtkWidget *field;
  GtkWidget *hue_strip;
  GtkWidget *alpha_strip;
  GtkWidget *alpha_row;   ///< the opacity's number and its unit
  GtkWidget *alpha_spin;
  GtkWidget *preview;
  GtkWidget *entry;
  gulong alpha_spin_handler;

  gboolean use_alpha;
  gchar *history_key;
  dt_color_well_changed_t changed;
  gpointer user_data;

  double hue;        ///< [0, 1)
  double saturation; ///< [0, 1]
  double value;      ///< [0, 1]
  double alpha;      ///< [0, 1]

  GdkRGBA original;  ///< the colour last set by the caller
  GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX];
  int history_count;
  GdkRGBA history_at_set[DT_COLOR_WELL_HISTORY_MAX]; ///< the recent colours when the colour was last set
  int history_at_set_count;

  color_well_drag_t drag;
  gboolean drag_reported; ///< the held drag has reported a LIVE change
  int history_cursor;     ///< the recent colour the keyboard is on, as a cell of the row
} dt_color_well_t;

static dt_color_well_t *_well(GtkWidget *widget)
{
  if(IS_NULL_PTR(widget)) return NULL;
  return (dt_color_well_t *)g_object_get_data(G_OBJECT(widget), COLOR_WELL_KEY);
}

/* --- colour arithmetic, no GTK ----------------------------------------------------------------- */

static double _clamp_unit(const double number)
{
  if(!(number > 0.0)) return 0.0;
  if(number > 1.0) return 1.0;
  return number;
}

void dt_color_well_hsv_to_rgb(const double hue, const double saturation, const double value, double *red,
                              double *green, double *blue)
{
  // Six sectors, each linear in one channel: which is why a gradient through the six primaries
  // and secondaries IS the hue strip, and not an approximation of it.
  const double wrapped_hue = hue - floor(hue);
  const double sector_position = wrapped_hue * 6.0;
  const int sector = (int)floor(sector_position) % 6;
  const double fraction = sector_position - floor(sector_position);
  const double lowest = value * (1.0 - saturation);
  const double falling = value * (1.0 - saturation * fraction);
  const double rising = value * (1.0 - saturation * (1.0 - fraction));
  switch(sector)
  {
    case 0:
      *red = value;
      *green = rising;
      *blue = lowest;
      break;
    case 1:
      *red = falling;
      *green = value;
      *blue = lowest;
      break;
    case 2:
      *red = lowest;
      *green = value;
      *blue = rising;
      break;
    case 3:
      *red = lowest;
      *green = falling;
      *blue = value;
      break;
    case 4:
      *red = rising;
      *green = lowest;
      *blue = value;
      break;
    default:
      *red = value;
      *green = lowest;
      *blue = falling;
      break;
  }
}

void dt_color_well_rgb_to_hsv(const double red, const double green, const double blue, double *hue,
                              double *saturation, double *value)
{
  const double highest = MAX(red, MAX(green, blue));
  const double lowest = MIN(red, MIN(green, blue));
  const double spread = highest - lowest;
  *value = highest;
  *saturation = highest > 0.0 ? spread / highest : 0.0;
  if(spread <= 0.0)
  {
    *hue = 0.0;
    return;
  }
  double sector_position = 0.0;
  if(highest == red)
    sector_position = (green - blue) / spread;
  else if(highest == green)
    sector_position = 2.0 + (blue - red) / spread;
  else
    sector_position = 4.0 + (red - green) / spread;
  double wrapped = sector_position / 6.0;
  if(wrapped < 0.0) wrapped += 1.0;
  // A hue a hair below zero wraps to one after rounding, which is red again: kept in [0, 1).
  if(wrapped >= 1.0) wrapped -= 1.0;
  *hue = wrapped;
}

static int _hex_digit(const char character)
{
  if(character >= '0' && character <= '9') return character - '0';
  if(character >= 'a' && character <= 'f') return character - 'a' + 10;
  if(character >= 'A' && character <= 'F') return character - 'A' + 10;
  return -1;
}

gboolean dt_color_well_parse_hex(const char *text, GdkRGBA *color, gboolean *has_alpha)
{
  if(IS_NULL_PTR(text) || IS_NULL_PTR(color)) return FALSE;
  const char *start = text;
  while(g_ascii_isspace(*start)) start++;
  if(*start == '#') start++;
  size_t length = strlen(start);
  while(length > 0 && g_ascii_isspace(start[length - 1])) length--;

  int digits[8] = { 0 };
  if(length != 3 && length != 4 && length != 6 && length != 8) return FALSE;
  for(size_t idx = 0; idx < length; idx++)
  {
    digits[idx] = _hex_digit(start[idx]);
    if(digits[idx] < 0) return FALSE;
  }

  // A short form repeats each digit, as CSS reads it: #f80 is #ff8800.
  const gboolean short_form = length == 3 || length == 4;
  const int channel_count = short_form ? (int)length : (int)length / 2;
  double channels[4] = { 0.0, 0.0, 0.0, 1.0 };
  for(int channel = 0; channel < channel_count; channel++)
  {
    const int byte = short_form ? digits[channel] * 17 : digits[2 * channel] * 16 + digits[2 * channel + 1];
    channels[channel] = byte / 255.0;
  }

  color->red = channels[0];
  color->green = channels[1];
  color->blue = channels[2];
  const gboolean alpha_given = channel_count == 4;
  if(alpha_given) color->alpha = channels[3];
  if(!IS_NULL_PTR(has_alpha)) *has_alpha = alpha_given;
  return TRUE;
}

static int _byte_of(const double channel)
{
  return (int)lround(_clamp_unit(channel) * 255.0);
}

void dt_color_well_format_hex(const GdkRGBA *color, const gboolean with_alpha, char *text, const size_t size)
{
  if(IS_NULL_PTR(color) || IS_NULL_PTR(text) || size == 0) return;
  const int alpha_byte = _byte_of(color->alpha);
  if(with_alpha && alpha_byte != 255)
    g_snprintf(text, size, "#%02X%02X%02X%02X", _byte_of(color->red), _byte_of(color->green),
               _byte_of(color->blue), alpha_byte);
  else
    g_snprintf(text, size, "#%02X%02X%02X", _byte_of(color->red), _byte_of(color->green), _byte_of(color->blue));
}

gboolean dt_color_well_same_color(const GdkRGBA *first, const GdkRGBA *second)
{
  if(IS_NULL_PTR(first) || IS_NULL_PTR(second)) return FALSE;
  return _byte_of(first->red) == _byte_of(second->red) && _byte_of(first->green) == _byte_of(second->green)
         && _byte_of(first->blue) == _byte_of(second->blue) && _byte_of(first->alpha) == _byte_of(second->alpha);
}

int dt_color_well_history_push(GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX], const int count, const GdkRGBA *color)
{
  if(IS_NULL_PTR(history) || IS_NULL_PTR(color)) return MAX(count, 0);
  GdkRGBA kept[DT_COLOR_WELL_HISTORY_MAX];
  int kept_count = 0;
  kept[kept_count] = *color;
  kept_count++;
  const int old_count = CLAMP(count, 0, DT_COLOR_WELL_HISTORY_MAX);
  for(int idx = 0; idx < old_count && kept_count < DT_COLOR_WELL_HISTORY_MAX; idx++)
  {
    if(dt_color_well_same_color(&history[idx], color)) continue;
    kept[kept_count] = history[idx];
    kept_count++;
  }
  memcpy(history, kept, sizeof(GdkRGBA) * kept_count);
  return kept_count;
}

int dt_color_well_history_parse(const char *text, GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX])
{
  if(IS_NULL_PTR(text) || IS_NULL_PTR(history)) return 0;
  gchar **items = g_strsplit(text, ",", -1);
  int count = 0;
  for(int idx = 0; !IS_NULL_PTR(items[idx]) && count < DT_COLOR_WELL_HISTORY_MAX; idx++)
  {
    GdkRGBA color = { 0.0, 0.0, 0.0, 1.0 };
    if(!dt_color_well_parse_hex(items[idx], &color, NULL)) continue;
    history[count] = color;
    count++;
  }
  g_strfreev(items);
  return count;
}

gchar *dt_color_well_history_format(const GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX], const int count)
{
  GString *text = g_string_new(NULL);
  const int stored_count = IS_NULL_PTR(history) ? 0 : CLAMP(count, 0, DT_COLOR_WELL_HISTORY_MAX);
  for(int idx = 0; idx < stored_count; idx++)
  {
    // Always the eight digits: a list read back must not depend on whether a colour happened to
    // be opaque when it was written.
    if(idx > 0) g_string_append_c(text, ',');
    g_string_append_printf(text, "#%02X%02X%02X%02X", _byte_of(history[idx].red), _byte_of(history[idx].green),
                           _byte_of(history[idx].blue), _byte_of(history[idx].alpha));
  }
  return g_string_free(text, FALSE);
}

/* --- the well's colour ------------------------------------------------------------------------- */

static void _current_color(const dt_color_well_t *well, GdkRGBA *color)
{
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  dt_color_well_hsv_to_rgb(well->hue, well->saturation, well->value, &red, &green, &blue);
  color->red = red;
  color->green = green;
  color->blue = blue;
  color->alpha = well->use_alpha ? well->alpha : 1.0;
}

/** Take a colour, keeping the hue the strip shows when the colour has none of its own. */
static void _take_color(dt_color_well_t *well, const GdkRGBA *color)
{
  double hue = 0.0;
  double saturation = 0.0;
  double value = 0.0;
  dt_color_well_rgb_to_hsv(_clamp_unit(color->red), _clamp_unit(color->green), _clamp_unit(color->blue), &hue,
                           &saturation, &value);
  if(saturation > 0.0 && value > 0.0) well->hue = hue;
  well->saturation = saturation;
  well->value = value;
  well->alpha = well->use_alpha ? _clamp_unit(color->alpha) : 1.0;
}

static gboolean _colors_equal(const GdkRGBA *first, const GdkRGBA *second)
{
  return first->red == second->red && first->green == second->green && first->blue == second->blue
         && first->alpha == second->alpha;
}

static void _store_history(const dt_color_well_t *well)
{
  if(IS_NULL_PTR(well->history_key)) return;
  gchar *text = dt_color_well_history_format(well->history, well->history_count);
  dt_widget_store_string(well->history_key, text);
  dt_free(text);
}

/** Write the colour into the controls that show it as a number, reporting nothing, and redraw. */
static void _refill_controls(dt_color_well_t *well)
{
  GdkRGBA color;
  _current_color(well, &color);
  char hex[DT_COLOR_WELL_HEX_SIZE];
  dt_color_well_format_hex(&color, well->use_alpha, hex, sizeof(hex));
  if(g_strcmp0(gtk_entry_get_text(GTK_ENTRY(well->entry)), hex) != 0)
    gtk_entry_set_text(GTK_ENTRY(well->entry), hex);
  dt_gui_remove_class(well->entry, "error");

  // Shown in whole percents, and set to exactly what is shown: a spin holding 53.7 behind a "54"
  // reads its own text back as a change the moment it is updated, and snaps the opacity to it.
  const double percent = round(well->alpha * 100.0);
  if(gtk_spin_button_get_value(GTK_SPIN_BUTTON(well->alpha_spin)) != percent)
  {
    g_signal_handler_block(well->alpha_spin, well->alpha_spin_handler);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(well->alpha_spin), percent);
    g_signal_handler_unblock(well->alpha_spin, well->alpha_spin_handler);
  }

  gtk_widget_queue_draw(well->field);
  gtk_widget_queue_draw(well->hue_strip);
  gtk_widget_queue_draw(well->alpha_strip);
  gtk_widget_queue_draw(well->preview);
}

static void _report(dt_color_well_t *well, const dt_color_well_phase_t phase)
{
  GdkRGBA color;
  _current_color(well, &color);
  if(phase == DT_COLOR_WELL_COMMIT)
  {
    // One visit, one entry: the list the visit started from, with this colour at its head.
    memcpy(well->history, well->history_at_set, sizeof(well->history));
    well->history_count = dt_color_well_history_push(well->history, well->history_at_set_count, &color);
    _store_history(well);
    gtk_widget_queue_draw(well->history_area);
  }
  // Last in this function, but not in every gesture: a drag's release reads the well again after its last
  // motion reported, which is why the caller may do anything with the change but destroy the well.
  if(!IS_NULL_PTR(well->changed)) well->changed(well->root, &color, phase, well->user_data);
}

/**
 * A whole gesture on one colour: a recent colour, the previous one, a typed number, a key. Reports a change
 * only, and a change is one the byte shows. The recent colours are bytes and the caller's colour is
 * usually not, so the swatch showing exactly the colour at hand is a hair away from it: taken, it would
 * be an edit nothing on screen can tell from none, and an undo step that undoes nothing. The well then
 * keeps the colour it held, and only the numbers are written again -- "#f80" typed reads "#FF8800".
 */
static void _commit_color(dt_color_well_t *well, const GdkRGBA *color)
{
  GdkRGBA before;
  _current_color(well, &before);
  const double hue = well->hue;
  const double saturation = well->saturation;
  const double value = well->value;
  const double alpha = well->alpha;
  _take_color(well, color);
  GdkRGBA after;
  _current_color(well, &after);
  const gboolean unchanged = dt_color_well_same_color(&before, &after);
  if(unchanged)
  {
    well->hue = hue;
    well->saturation = saturation;
    well->value = value;
    well->alpha = alpha;
  }
  _refill_controls(well);
  if(unchanged) return;
  _report(well, DT_COLOR_WELL_COMMIT);
}

/**
 * A whole gesture held as hue, saturation and value, as a key moves them: the marker always goes where
 * the key sent it, since at black or in the greys a step moves the field and not the colour, and only a
 * colour the byte shows as changed is reported -- the next step that changes one reports it.
 */
static void _commit_hsv(dt_color_well_t *well, const double hue, const double saturation, const double value,
                        const double alpha)
{
  GdkRGBA before;
  _current_color(well, &before);
  well->hue = hue;
  well->saturation = saturation;
  well->value = value;
  well->alpha = well->use_alpha ? alpha : 1.0;
  GdkRGBA after;
  _current_color(well, &after);
  _refill_controls(well);
  if(dt_color_well_same_color(&before, &after)) return;
  _report(well, DT_COLOR_WELL_COMMIT);
}

/* --- drawing ------------------------------------------------------------------------------------ */

/** A checkerboard, so a transparent colour does not read as a darker or lighter opaque one. */
static void _paint_checker(cairo_t *cr, const double x, const double y, const double width, const double height)
{
  const double cell = DT_PIXEL_APPLY_DPI(COLOR_WELL_CHECKER_PIXELS);
  cairo_save(cr);
  cairo_rectangle(cr, x, y, width, height);
  cairo_clip(cr);
  cairo_set_source_rgb(cr, 0.8, 0.8, 0.8);
  cairo_paint(cr);
  cairo_set_source_rgb(cr, 0.5, 0.5, 0.5);
  for(int row = 0; row * cell < height; row++)
  {
    for(int column = row % 2; column * cell < width; column += 2)
    {
      cairo_rectangle(cr, x + column * cell, y + row * cell, cell, cell);
    }
  }
  cairo_fill(cr);
  cairo_restore(cr);
}

/** A mark drawn twice, dark under light, so it reads over any colour it sits on, as thick as the UI is scaled. */
static void _stroke_contrasted(cairo_t *cr)
{
  cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(3.0));
  cairo_stroke_preserve(cr);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  cairo_set_line_width(cr, DT_PIXEL_APPLY_DPI(1.5));
  cairo_stroke(cr);
}

/**
 * The width of a hairline outline: one pixel of the UI, rounded to a whole one so that, stroked half a
 * width inside a whole-pixel edge, it covers whole pixels and stays sharp.
 */
static double _outline_width(void)
{
  return MAX(1.0, round(DT_PIXEL_APPLY_DPI(1.0)));
}

/** The keyboard is on this control: a contrasted rectangle just inside its edge. */
static void _paint_focus(GtkWidget *widget, cairo_t *cr)
{
  if(!gtk_widget_has_visible_focus(widget)) return;
  const double inset = DT_PIXEL_APPLY_DPI(1.5);
  cairo_new_path(cr);
  cairo_rectangle(cr, inset, inset, gtk_widget_get_allocated_width(widget) - 2.0 * inset,
                  gtk_widget_get_allocated_height(widget) - 2.0 * inset);
  _stroke_contrasted(cr);
}

/**
 * Saturation across, value down, both drawn as gradients rather than pixels: white to the pure hue
 * is exactly the saturation ramp in display values, and black laid over it with the opacity of one
 * less the value multiplies it by the value, which is what value means. A gradient is rasterised
 * at the surface's own density, so the field is as sharp at a device scale of 2 as at 1.
 */
static gboolean _field_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  const dt_color_well_t *well = (const dt_color_well_t *)user_data;
  const double width = gtk_widget_get_allocated_width(widget);
  const double height = gtk_widget_get_allocated_height(widget);
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  dt_color_well_hsv_to_rgb(well->hue, 1.0, 1.0, &red, &green, &blue);

  cairo_pattern_t *saturation_ramp = cairo_pattern_create_linear(0.0, 0.0, width, 0.0);
  cairo_pattern_add_color_stop_rgb(saturation_ramp, 0.0, 1.0, 1.0, 1.0);
  cairo_pattern_add_color_stop_rgb(saturation_ramp, 1.0, red, green, blue);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_set_source(cr, saturation_ramp);
  cairo_fill(cr);
  cairo_pattern_destroy(saturation_ramp);

  cairo_pattern_t *value_ramp = cairo_pattern_create_linear(0.0, 0.0, 0.0, height);
  cairo_pattern_add_color_stop_rgba(value_ramp, 0.0, 0.0, 0.0, 0.0, 0.0);
  cairo_pattern_add_color_stop_rgba(value_ramp, 1.0, 0.0, 0.0, 0.0, 1.0);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_set_source(cr, value_ramp);
  cairo_fill(cr);
  cairo_pattern_destroy(value_ramp);

  const double marker_x = well->saturation * width;
  const double marker_y = (1.0 - well->value) * height;
  cairo_new_path(cr);
  cairo_arc(cr, marker_x, marker_y, DT_PIXEL_APPLY_DPI(COLOR_WELL_MARKER_RADIUS), 0.0, 2.0 * G_PI);
  _stroke_contrasted(cr);
  _paint_focus(widget, cr);
  return TRUE;
}

/** The hues top to bottom, red at both ends: six linear sectors, so seven stops draw them exactly. */
static gboolean _hue_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  const dt_color_well_t *well = (const dt_color_well_t *)user_data;
  const double width = gtk_widget_get_allocated_width(widget);
  const double height = gtk_widget_get_allocated_height(widget);
  cairo_pattern_t *hues = cairo_pattern_create_linear(0.0, 0.0, 0.0, height);
  for(int stop = 0; stop <= 6; stop++)
  {
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    dt_color_well_hsv_to_rgb(stop / 6.0, 1.0, 1.0, &red, &green, &blue);
    cairo_pattern_add_color_stop_rgb(hues, stop / 6.0, red, green, blue);
  }
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_set_source(cr, hues);
  cairo_fill(cr);
  cairo_pattern_destroy(hues);

  const double inset = DT_PIXEL_APPLY_DPI(1.5);
  const double marker_y = CLAMP(well->hue * height, inset, height - inset);
  const double marker_half = DT_PIXEL_APPLY_DPI(2.0);
  cairo_rectangle(cr, inset, marker_y - marker_half, width - 2.0 * inset, 2.0 * marker_half);
  _stroke_contrasted(cr);
  _paint_focus(widget, cr);
  return TRUE;
}

/** The colour from transparent to opaque, over the checkerboard its transparency shows against. */
static gboolean _alpha_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  const dt_color_well_t *well = (const dt_color_well_t *)user_data;
  const double width = gtk_widget_get_allocated_width(widget);
  const double height = gtk_widget_get_allocated_height(widget);
  GdkRGBA color;
  _current_color(well, &color);
  _paint_checker(cr, 0.0, 0.0, width, height);
  cairo_pattern_t *opacity = cairo_pattern_create_linear(0.0, 0.0, width, 0.0);
  cairo_pattern_add_color_stop_rgba(opacity, 0.0, color.red, color.green, color.blue, 0.0);
  cairo_pattern_add_color_stop_rgba(opacity, 1.0, color.red, color.green, color.blue, 1.0);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_set_source(cr, opacity);
  cairo_fill(cr);
  cairo_pattern_destroy(opacity);

  const double inset = DT_PIXEL_APPLY_DPI(1.5);
  const double marker_x = CLAMP(well->alpha * width, inset, width - inset);
  const double marker_half = DT_PIXEL_APPLY_DPI(2.0);
  cairo_rectangle(cr, marker_x - marker_half, inset, 2.0 * marker_half, height - 2.0 * inset);
  _stroke_contrasted(cr);
  _paint_focus(widget, cr);
  return TRUE;
}

static void _paint_color_over_checker(cairo_t *cr, const GdkRGBA *color, const double x, const double y,
                                      const double width, const double height)
{
  if(color->alpha < 1.0) _paint_checker(cr, x, y, width, height);
  cairo_rectangle(cr, x, y, width, height);
  cairo_set_source_rgba(cr, color->red, color->green, color->blue, color->alpha);
  cairo_fill(cr);
}

/** The colour last set on the left, the colour now on the right. */
static gboolean _preview_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  const dt_color_well_t *well = (const dt_color_well_t *)user_data;
  const double width = gtk_widget_get_allocated_width(widget);
  const double height = gtk_widget_get_allocated_height(widget);
  const double half = floor(width / 2.0);
  GdkRGBA current;
  _current_color(well, &current);
  _paint_color_over_checker(cr, &well->original, 0.0, 0.0, half, height);
  _paint_color_over_checker(cr, &current, half, 0.0, width - half, height);
  const double line = _outline_width();
  cairo_rectangle(cr, line / 2.0, line / 2.0, width - line, height - line);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.5);
  cairo_set_line_width(cr, line);
  cairo_stroke(cr);
  // What a key gives back is the left half, so that is where the keyboard is shown.
  if(gtk_widget_has_visible_focus(widget))
  {
    const double inset = DT_PIXEL_APPLY_DPI(1.5);
    cairo_rectangle(cr, inset, inset, half - 2.0 * inset, height - 2.0 * inset);
    _stroke_contrasted(cr);
  }
  return TRUE;
}

/**
 * Where a cell of the recent colours is drawn: equal cells on whole pixels, the pixels a division leaves
 * over spread through the gaps, so the row fills its width and every outline lands on the pixel grid.
 */
static void _history_cell(GtkWidget *widget, const int cell, double *x, double *width)
{
  const int allocated = gtk_widget_get_allocated_width(widget);
  const int gap = (int)lround(DT_PIXEL_APPLY_DPI(3.0));
  const int cell_width = MAX((allocated - gap * (DT_COLOR_WELL_HISTORY_MAX - 1)) / DT_COLOR_WELL_HISTORY_MAX, 1);
  const int spare = MAX(allocated - cell_width * DT_COLOR_WELL_HISTORY_MAX, 0);
  const long spare_before = lround((double)cell * spare / (DT_COLOR_WELL_HISTORY_MAX - 1));
  *x = (double)(cell * cell_width + spare_before);
  *width = (double)cell_width;
}

/**
 * Whether a recent colour is offered here. The list is shared by every well under its key, and one
 * without opacity cannot take a transparent colour: shown, it would be picked opaque -- another colour
 * than the swatch, pushed as another entry beside it.
 */
static gboolean _history_offered(const dt_color_well_t *well, const int index)
{
  return well->use_alpha || _byte_of(well->history[index].alpha) == 255;
}

/** How many cells of the row hold a colour. */
static int _history_cells_filled(const dt_color_well_t *well)
{
  int filled = 0;
  for(int index = 0; index < well->history_count; index++)
  {
    if(_history_offered(well, index)) filled++;
  }
  return filled;
}

/** The recent colour a cell shows, as its place in the list, or -1 for an empty cell. */
static int _history_index_of_cell(const dt_color_well_t *well, const int cell)
{
  int filled = 0;
  for(int index = 0; index < well->history_count; index++)
  {
    if(!_history_offered(well, index)) continue;
    if(filled == cell) return index;
    filled++;
  }
  return -1;
}

/** Which recent colour a position is on, as its place in the list, or -1 between cells and past the list. */
static int _history_index_at(const dt_color_well_t *well, const double x)
{
  for(int cell = 0; cell < DT_COLOR_WELL_HISTORY_MAX; cell++)
  {
    double cell_x = 0.0;
    double cell_width = 0.0;
    _history_cell(well->history_area, cell, &cell_x, &cell_width);
    if(x >= cell_x && x < cell_x + cell_width) return _history_index_of_cell(well, cell);
  }
  return -1;
}

static gboolean _history_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  const dt_color_well_t *well = (const dt_color_well_t *)user_data;
  const double height = gtk_widget_get_allocated_height(widget);
  const double line = _outline_width();
  GdkRGBA foreground;
  gtk_style_context_get_color(gtk_widget_get_style_context(widget), gtk_widget_get_state_flags(widget),
                              &foreground);
  for(int cell = 0; cell < DT_COLOR_WELL_HISTORY_MAX; cell++)
  {
    double cell_x = 0.0;
    double cell_width = 0.0;
    _history_cell(widget, cell, &cell_x, &cell_width);
    const int index = _history_index_of_cell(well, cell);
    if(index >= 0) _paint_color_over_checker(cr, &well->history[index], cell_x, 0.0, cell_width, height);
    // An empty slot is outlined dimly, so the row reads as a place colours will go.
    cairo_rectangle(cr, cell_x + line / 2.0, line / 2.0, cell_width - line, height - line);
    cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue, index >= 0 ? 0.5 : 0.2);
    cairo_set_line_width(cr, line);
    cairo_stroke(cr);
  }
  if(gtk_widget_has_visible_focus(widget) && _history_cells_filled(well) > 0)
  {
    double cell_x = 0.0;
    double cell_width = 0.0;
    _history_cell(widget, MIN(well->history_cursor, _history_cells_filled(well) - 1), &cell_x, &cell_width);
    const double inset = DT_PIXEL_APPLY_DPI(1.5);
    cairo_rectangle(cr, cell_x + inset, inset, cell_width - 2.0 * inset, height - 2.0 * inset);
    _stroke_contrasted(cr);
  }
  return TRUE;
}

/* --- gestures ----------------------------------------------------------------------------------- */

/** One position of a held drag: the colour follows it, and a colour that moved is reported LIVE. */
static void _drag_to(dt_color_well_t *well, const double x, const double y)
{
  GdkRGBA before;
  _current_color(well, &before);
  const double hue_before = well->hue;
  switch(well->drag)
  {
    case COLOR_WELL_DRAG_FIELD:
    {
      const double width = MAX(gtk_widget_get_allocated_width(well->field), 1);
      const double height = MAX(gtk_widget_get_allocated_height(well->field), 1);
      well->saturation = _clamp_unit(x / width);
      well->value = 1.0 - _clamp_unit(y / height);
      break;
    }
    case COLOR_WELL_DRAG_HUE:
    {
      const double height = MAX(gtk_widget_get_allocated_height(well->hue_strip), 1);
      // The bottom edge is red again; kept below one so the strip's marker stays at the bottom.
      well->hue = MIN(_clamp_unit(y / height), 1.0 - 1e-9);
      break;
    }
    case COLOR_WELL_DRAG_ALPHA:
    {
      const double width = MAX(gtk_widget_get_allocated_width(well->alpha_strip), 1);
      well->alpha = _clamp_unit(x / width);
      break;
    }
    default:
      return;
  }
  GdkRGBA after;
  _current_color(well, &after);
  const gboolean moved = !_colors_equal(&before, &after);
  if(!moved && hue_before == well->hue) return;
  _refill_controls(well);
  // A grey's hue changes nothing a caller could see, only the field.
  if(!moved) return;
  well->drag_reported = TRUE;
  _report(well, DT_COLOR_WELL_LIVE);
}

/** Let go of the held drag: a commit when it changed the colour, unless the caller asked for silence. */
static void _drag_end(dt_color_well_t *well, const gboolean report)
{
  if(well->drag == COLOR_WELL_DRAG_NONE) return;
  const gboolean reported = well->drag_reported;
  well->drag = COLOR_WELL_DRAG_NONE;
  well->drag_reported = FALSE;
  if(report && reported) _report(well, DT_COLOR_WELL_COMMIT);
}

static gboolean _area_pressed(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  if(event->button != 1 || event->type != GDK_BUTTON_PRESS) return TRUE;
  // A press while another drag is held -- a second button's release lost to a grab -- ends that one first.
  _drag_end(well, TRUE);
  if(widget == well->field)
    well->drag = COLOR_WELL_DRAG_FIELD;
  else if(widget == well->hue_strip)
    well->drag = COLOR_WELL_DRAG_HUE;
  else
    well->drag = COLOR_WELL_DRAG_ALPHA;
  well->drag_reported = FALSE;
  _drag_to(well, event->x, event->y);
  return TRUE;
}

static gboolean _area_moved(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  if(well->drag == COLOR_WELL_DRAG_NONE) return FALSE;
  _drag_to(well, event->x, event->y);
  return TRUE;
}

static gboolean _area_released(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  if(event->button != 1) return TRUE;
  _drag_to(well, event->x, event->y);
  _drag_end(well, TRUE);
  return TRUE;
}

static gboolean _area_grab_broken(GtkWidget *widget, GdkEvent *event, gpointer user_data)
{
  // The release will not come: the drag ends where the pointer last was.
  _drag_end((dt_color_well_t *)user_data, TRUE);
  return FALSE;
}

static gboolean _history_pressed(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  if(event->button != 1 || event->type != GDK_BUTTON_PRESS) return TRUE;
  const int index = _history_index_at(well, event->x);
  if(index < 0) return TRUE;
  const GdkRGBA picked = well->history[index];
  _commit_color(well, &picked);
  return TRUE;
}

/** A step of a key: a tenth of the range with Shift, a hundredth without. */
static double _key_step(const GdkEventKey *event)
{
  return (event->state & GDK_SHIFT_MASK) ? 0.1 : 0.01;
}

/**
 * The arrows move what the focused field or strip sets, as a drag would, and each key is a whole gesture:
 * reported as a commit, so a key held down repeats edits the way clicks would, never a drag whose end
 * depends on a release the window may not see.
 * Across the field saturation, down it value; along a strip its own quantity. Up is brighter, and on the
 * hue strip up goes towards its top, which is where the hues start.
 */
static gboolean _area_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  const double step = _key_step(event);
  double across = 0.0;
  double along = 0.0;
  switch(event->keyval)
  {
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      across = -step;
      break;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      across = step;
      break;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
      along = step;
      break;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
      along = -step;
      break;
    default:
      return FALSE;
  }
  // A drag held with the pointer is a gesture of its own, ended before the key's.
  _drag_end(well, TRUE);
  double hue = well->hue;
  double saturation = well->saturation;
  double value = well->value;
  double alpha = well->alpha;
  if(widget == well->field)
  {
    saturation = _clamp_unit(saturation + across);
    value = _clamp_unit(value + along);
  }
  else if(widget == well->hue_strip)
  {
    // Kept below one, as the drag keeps it: the bottom edge is red again, and the marker stays there.
    hue = MIN(_clamp_unit(hue + across - along), 1.0 - 1e-9);
  }
  else if(widget == well->alpha_strip)
  {
    alpha = _clamp_unit(alpha + across + along);
  }
  else
  {
    return FALSE;
  }
  _commit_hsv(well, hue, saturation, value, alpha);
  return TRUE;
}

/** The recent colours from the keyboard: the arrows, Home and End move along them, space or Return picks one. */
static gboolean _history_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  const int filled = _history_cells_filled(well);
  if(filled == 0) return FALSE;
  int cursor = CLAMP(well->history_cursor, 0, filled - 1);
  switch(event->keyval)
  {
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
      cursor = MAX(cursor - 1, 0);
      break;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
      cursor = MIN(cursor + 1, filled - 1);
      break;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
      cursor = 0;
      break;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
      cursor = filled - 1;
      break;
    case GDK_KEY_space:
    case GDK_KEY_KP_Space:
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      const int index = _history_index_of_cell(well, cursor);
      if(index < 0) return FALSE;
      const GdkRGBA picked = well->history[index];
      _commit_color(well, &picked);
      // The colour picked went to the head of the list: the keyboard follows it there.
      well->history_cursor = 0;
      for(int cell = 0; cell < _history_cells_filled(well); cell++)
      {
        const int shown = _history_index_of_cell(well, cell);
        if(shown < 0 || !dt_color_well_same_color(&well->history[shown], &picked)) continue;
        well->history_cursor = cell;
        break;
      }
      gtk_widget_queue_draw(widget);
      return TRUE;
    }
    default:
      return FALSE;
  }
  well->history_cursor = cursor;
  gtk_widget_queue_draw(widget);
  return TRUE;
}

/** Space or Return on the colour before and the colour now goes back to the colour before, as its half clicked. */
static gboolean _preview_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  if(event->keyval != GDK_KEY_space && event->keyval != GDK_KEY_KP_Space && event->keyval != GDK_KEY_Return
     && event->keyval != GDK_KEY_KP_Enter)
    return FALSE;
  const GdkRGBA previous = well->original;
  _commit_color(well, &previous);
  return TRUE;
}

static gboolean _history_tooltip(GtkWidget *widget, const gint x, const gint y, const gboolean keyboard,
                                 GtkTooltip *tooltip, gpointer user_data)
{
  const dt_color_well_t *well = (const dt_color_well_t *)user_data;
  const int index = _history_index_at(well, x);
  if(index < 0) return FALSE;
  char hex[DT_COLOR_WELL_HEX_SIZE];
  dt_color_well_format_hex(&well->history[index], well->use_alpha, hex, sizeof(hex));
  gtk_tooltip_set_text(tooltip, hex);
  return TRUE;
}

static gboolean _preview_pressed(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  if(event->button != 1 || event->type != GDK_BUTTON_PRESS) return TRUE;
  if(event->x >= gtk_widget_get_allocated_width(widget) / 2.0) return TRUE;
  const GdkRGBA previous = well->original;
  _commit_color(well, &previous);
  return TRUE;
}

/** The typed number, applied if it reads as a colour and says something else than the colour shown. */
static gboolean _apply_entry(dt_color_well_t *well)
{
  const char *text = gtk_entry_get_text(GTK_ENTRY(well->entry));
  GdkRGBA typed;
  _current_color(well, &typed);
  if(!dt_color_well_parse_hex(text, &typed, NULL))
  {
    dt_gui_add_class(well->entry, "error");
    return FALSE;
  }
  // Six digits name a colour and leave the opacity to its own strip, which kept its value above. What
  // was typed is shown back as the well writes it, changed or not: "#f80" reads "#FF8800" once applied.
  _commit_color(well, &typed);
  return TRUE;
}

static void _entry_activated(GtkEntry *entry, gpointer user_data)
{
  _apply_entry((dt_color_well_t *)user_data);
}

static gboolean _entry_focus_out(GtkWidget *entry, GdkEventFocus *event, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  GdkRGBA color;
  _current_color(well, &color);
  char shown[DT_COLOR_WELL_HEX_SIZE];
  dt_color_well_format_hex(&color, well->use_alpha, shown, sizeof(shown));
  if(g_strcmp0(gtk_entry_get_text(GTK_ENTRY(entry)), shown) != 0) _apply_entry(well);
  return FALSE;
}

static void _entry_changed(GtkEditable *editable, gpointer user_data)
{
  dt_gui_remove_class(GTK_WIDGET(editable), "error");
}

static void _alpha_spin_changed(GtkSpinButton *spin, gpointer user_data)
{
  dt_color_well_t *well = (dt_color_well_t *)user_data;
  const double percent = gtk_spin_button_get_value(spin);
  // The strip is finer than a percent: a spin updated on the value it already shows moves nothing.
  if(round(well->alpha * 100.0) == percent) return;
  GdkRGBA color;
  _current_color(well, &color);
  color.alpha = percent / 100.0;
  _commit_color(well, &color);
}

/* --- building ----------------------------------------------------------------------------------- */

static void _well_free(gpointer data)
{
  dt_color_well_t *well = (dt_color_well_t *)data;
  dt_free(well->history_key);
  dt_free(well);
}

/**
 * A drawn control of the well. Each takes the keyboard, with Tab, and a click does not move it there: the
 * number stays where typing goes until a key asks for another control.
 */
static GtkWidget *_drawing_area(dt_color_well_t *well, const int width, const int height, const char *name,
                                const char *tooltip, GCallback draw)
{
  GtkWidget *area = gtk_drawing_area_new();
  gtk_widget_set_name(area, name);
  gtk_widget_set_size_request(area, width, height);
  gtk_widget_set_tooltip_text(area, tooltip);
  gtk_widget_set_can_focus(area, TRUE);
  gtk_widget_set_focus_on_click(area, FALSE);
  gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK
                                  | GDK_KEY_PRESS_MASK);
  g_signal_connect(area, "draw", draw, well);
  return area;
}

static GtkWidget *_drag_area(dt_color_well_t *well, const int width, const int height, const char *name,
                             const char *tooltip, GCallback draw)
{
  GtkWidget *area = _drawing_area(well, width, height, name, tooltip, draw);
  g_signal_connect(area, "button-press-event", G_CALLBACK(_area_pressed), well);
  g_signal_connect(area, "motion-notify-event", G_CALLBACK(_area_moved), well);
  g_signal_connect(area, "button-release-event", G_CALLBACK(_area_released), well);
  g_signal_connect(area, "grab-broken-event", G_CALLBACK(_area_grab_broken), well);
  g_signal_connect(area, "key-press-event", G_CALLBACK(_area_key_pressed), well);
  return area;
}

GtkWidget *dt_color_well_new(const char *history_key, const gboolean use_alpha, dt_color_well_changed_t changed,
                             gpointer user_data)
{
  dt_color_well_t *well = g_malloc0(sizeof(dt_color_well_t));
  well->use_alpha = use_alpha;
  well->history_key = IS_NULL_PTR(history_key) ? NULL : g_strdup(history_key);
  well->changed = changed;
  well->user_data = user_data;
  well->value = 1.0;
  well->alpha = 1.0;
  well->original.alpha = 1.0;
  if(!IS_NULL_PTR(well->history_key))
  {
    gchar *stored = dt_widget_stored_string(well->history_key);
    well->history_count = dt_color_well_history_parse(stored, well->history);
    dt_free(stored);
  }
  memcpy(well->history_at_set, well->history, sizeof(well->history));
  well->history_at_set_count = well->history_count;

  const int gap = DT_PIXEL_APPLY_DPI(COLOR_WELL_GAP_PIXELS);
  const int strip = DT_PIXEL_APPLY_DPI(COLOR_WELL_STRIP_PIXELS);
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, gap);
  well->root = root;
  gtk_widget_set_name(root, "dt-color-well");
  dt_gui_add_class(root, "dt-color-well");

  well->history_area = _drawing_area(well, -1, DT_PIXEL_APPLY_DPI(COLOR_WELL_HISTORY_PIXELS),
                                     "dt-color-well-history", NULL, G_CALLBACK(_history_draw));
  // Each cell names its own colour: one tooltip for the whole row would say nothing useful.
  gtk_widget_set_has_tooltip(well->history_area, TRUE);
  g_signal_connect(well->history_area, "query-tooltip", G_CALLBACK(_history_tooltip), well);
  g_signal_connect(well->history_area, "button-press-event", G_CALLBACK(_history_pressed), well);
  g_signal_connect(well->history_area, "key-press-event", G_CALLBACK(_history_key_pressed), well);
  gtk_box_pack_start(GTK_BOX(root), well->history_area, FALSE, FALSE, 0);

  GtkWidget *pick_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, gap);
  well->field = _drag_area(well, DT_PIXEL_APPLY_DPI(COLOR_WELL_FIELD_WIDTH),
                           DT_PIXEL_APPLY_DPI(COLOR_WELL_FIELD_HEIGHT), "dt-color-well-field",
                           _("Saturation across, brightness down: drag, or use the arrow keys, to pick"),
                           G_CALLBACK(_field_draw));
  gtk_box_pack_start(GTK_BOX(pick_row), well->field, TRUE, TRUE, 0);
  well->hue_strip = _drag_area(well, strip, -1, "dt-color-well-hue",
                               _("Hue: drag, or use the arrow keys, to pick"), G_CALLBACK(_hue_draw));
  gtk_box_pack_start(GTK_BOX(pick_row), well->hue_strip, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), pick_row, TRUE, TRUE, 0);

  // The opacity strip is as wide as the field and the hues together: the finest control of the three
  // gets the most room, and its number sits with the colour's number below.
  well->alpha_strip = _drag_area(well, -1, strip, "dt-color-well-alpha",
                                 _("Opacity: drag, or use the arrow keys, to set"), G_CALLBACK(_alpha_draw));
  gtk_box_pack_start(GTK_BOX(root), well->alpha_strip, FALSE, FALSE, 0);

  GtkWidget *number_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, gap);
  well->preview = _drawing_area(well, DT_PIXEL_APPLY_DPI(COLOR_WELL_PREVIEW_WIDTH), -1, "dt-color-well-preview",
                                _("The colour before and the colour now: click the left half to go back"),
                                G_CALLBACK(_preview_draw));
  g_signal_connect(well->preview, "button-press-event", G_CALLBACK(_preview_pressed), well);
  g_signal_connect(well->preview, "key-press-event", G_CALLBACK(_preview_key_pressed), well);
  gtk_box_pack_start(GTK_BOX(number_row), well->preview, FALSE, FALSE, 0);
  well->entry = gtk_entry_new();
  gtk_widget_set_name(well->entry, "dt-color-well-hex");
  gtk_entry_set_width_chars(GTK_ENTRY(well->entry), DT_COLOR_WELL_HEX_SIZE - 1);
  gtk_entry_set_max_length(GTK_ENTRY(well->entry), 16);
  gtk_widget_set_tooltip_text(well->entry,
                              use_alpha ? _("The colour in hexadecimal: #RRGGBB, or #RRGGBBAA with its opacity. "
                                            "Six digits keep the opacity as it is.")
                                        : _("The colour in hexadecimal: #RRGGBB"));
  g_signal_connect(well->entry, "activate", G_CALLBACK(_entry_activated), well);
  g_signal_connect(well->entry, "focus-out-event", G_CALLBACK(_entry_focus_out), well);
  g_signal_connect(well->entry, "changed", G_CALLBACK(_entry_changed), well);
  gtk_box_pack_start(GTK_BOX(number_row), well->entry, TRUE, TRUE, 0);

  well->alpha_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
  well->alpha_spin = gtk_spin_button_new_with_range(0.0, 100.0, 1.0);
  gtk_widget_set_name(well->alpha_spin, "dt-color-well-opacity");
  gtk_spin_button_set_digits(GTK_SPIN_BUTTON(well->alpha_spin), 0);
  gtk_entry_set_width_chars(GTK_ENTRY(well->alpha_spin), 3);
  gtk_widget_set_tooltip_text(well->alpha_spin, _("Opacity, in percent"));
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(well->alpha_spin), 100.0);
  well->alpha_spin_handler
      = g_signal_connect(well->alpha_spin, "value-changed", G_CALLBACK(_alpha_spin_changed), well);
  gtk_box_pack_start(GTK_BOX(well->alpha_row), well->alpha_spin, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(well->alpha_row), gtk_label_new("%"), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(number_row), well->alpha_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), number_row, FALSE, FALSE, 0);
  // Built either way, so every function can refill them; only never shown without the alpha.
  gtk_widget_set_no_show_all(well->alpha_strip, !use_alpha);
  gtk_widget_set_no_show_all(well->alpha_row, !use_alpha);

  g_object_set_data_full(G_OBJECT(root), COLOR_WELL_KEY, well, _well_free);
  _refill_controls(well);
  return root;
}

void dt_color_well_set_color(GtkWidget *widget, const GdkRGBA *color)
{
  dt_color_well_t *well = _well(widget);
  if(IS_NULL_PTR(well) || IS_NULL_PTR(color)) return;
  _drag_end(well, FALSE);
  _take_color(well, color);
  _current_color(well, &well->original);
  memcpy(well->history_at_set, well->history, sizeof(well->history));
  well->history_at_set_count = well->history_count;
  _refill_controls(well);
}

void dt_color_well_get_color(GtkWidget *widget, GdkRGBA *color)
{
  if(IS_NULL_PTR(color)) return;
  const dt_color_well_t *well = _well(widget);
  if(IS_NULL_PTR(well))
  {
    const GdkRGBA opaque_black = { 0.0, 0.0, 0.0, 1.0 };
    *color = opaque_black;
    return;
  }
  _current_color(well, color);
}

gboolean dt_color_well_commit_pending(GtkWidget *widget)
{
  dt_color_well_t *well = _well(widget);
  if(IS_NULL_PTR(well)) return TRUE;
  _drag_end(well, TRUE);
  if(well->use_alpha) gtk_spin_button_update(GTK_SPIN_BUTTON(well->alpha_spin));
  GdkRGBA color;
  _current_color(well, &color);
  char shown[DT_COLOR_WELL_HEX_SIZE];
  dt_color_well_format_hex(&color, well->use_alpha, shown, sizeof(shown));
  if(g_strcmp0(gtk_entry_get_text(GTK_ENTRY(well->entry)), shown) == 0) return TRUE;
  return _apply_entry(well);
}

void dt_color_well_grab_focus(GtkWidget *widget)
{
  dt_color_well_t *well = _well(widget);
  if(IS_NULL_PTR(well)) return;
  gtk_widget_grab_focus(well->entry);
}

void dt_color_well_revert(GtkWidget *widget)
{
  dt_color_well_t *well = _well(widget);
  if(IS_NULL_PTR(well)) return;
  _drag_end(well, FALSE);
  _take_color(well, &well->original);
  gboolean history_moved = well->history_count != well->history_at_set_count;
  for(int index = 0; index < well->history_count && !history_moved; index++)
  {
    if(!dt_color_well_same_color(&well->history[index], &well->history_at_set[index])) history_moved = TRUE;
  }
  memcpy(well->history, well->history_at_set, sizeof(well->history));
  well->history_count = well->history_at_set_count;
  if(history_moved) _store_history(well);
  _refill_controls(well);
  gtk_widget_queue_draw(well->history_area);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
