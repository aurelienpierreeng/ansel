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

#ifndef DT_WIDGETS_COLOR_WELL_H
#define DT_WIDGETS_COLOR_WELL_H

/* A colour picker shown whole: the colours used last, a field of saturation and value beside a
 * strip of hues, an opacity strip, and the colour as a hexadecimal number beside what it was.
 *
 * Named a well, as the palette's wells are, so it is not mistaken for gui/color_picker_proxy.h,
 * which picks a colour out of the image.
 *
 * GtkColorChooser offers a palette first and hides the precise part behind "Custom", and its
 * opacity with it: picking one exact half-transparent colour took three clicks before anything
 * could be dragged. Everything here is on screen at once.
 *
 * Colours are straight -- not premultiplied -- display values in [0, 1], alpha included, as
 * GdkRGBA carries them. The well owns the colour only while the user handles it: the caller
 * sets it, hears every change, and sets it again when its own value moved.
 *
 * Changes are reported in two phases. LIVE follows a drag in the field or the strips, once per
 * motion that changed the colour. COMMIT ends a gesture: a drag let go of, a recent colour or
 * the previous colour clicked, a hexadecimal number or an opacity validated, an arrow key on the
 * field or a strip, space or Return on a recent colour. A gesture that changed nothing reports
 * nothing, and a colour equal to the byte -- what the number shows -- is no change.
 *
 * Every control takes the keyboard: Tab moves between them, the arrows move the field and the strips
 * by a hundredth of their range (a tenth with Shift) and walk the recent colours, and space or Return
 * picks the recent colour under the keyboard or goes back to the previous colour.
 *
 * The recent colours are one list per key, stored through dt_widget_store_string() -- the host
 * decides where. A COMMIT puts the colour at the head of the list, taking it out of wherever else
 * it was, and every commit since the caller last set the colour REPLACES that head rather than
 * stacking: however long the user hesitates, one visit to the well leaves one colour in the list,
 * the last one kept. A well without opacity offers only the opaque colours of the list. */

#include <gtk/gtk.h>

G_BEGIN_DECLS

/** How many recent colours a list keeps. */
#define DT_COLOR_WELL_HISTORY_MAX 10

/** Room for "#RRGGBBAA" and its terminator. */
#define DT_COLOR_WELL_HEX_SIZE 10

typedef enum dt_color_well_phase_t
{
  DT_COLOR_WELL_LIVE = 0, ///< the colour follows a drag still held
  DT_COLOR_WELL_COMMIT,   ///< a gesture ended on this colour
} dt_color_well_phase_t;

typedef void (*dt_color_well_changed_t)(GtkWidget *well, const GdkRGBA *color, dt_color_well_phase_t phase,
                                        gpointer user_data);

/**
 * @brief Build a colour well.
 * @param history_key the stored string the recent colours are read from and written to; NULL keeps
 * them in this well alone.
 * @param use_alpha whether the opacity is offered. Without it every colour reported is opaque.
 * @param changed called for every change, LIVE and COMMIT; may be NULL. It may do anything with the
 * change but destroy the well: some gestures read the well again after reporting.
 * @param user_data handed back to @p changed.
 */
GtkWidget *dt_color_well_new(const char *history_key, gboolean use_alpha, dt_color_well_changed_t changed,
                             gpointer user_data);

/**
 * @brief Show a colour, reporting nothing. It becomes the colour the preview shows as the previous
 * one, dt_color_well_revert() goes back to, and the next commit's entry in the recent colours
 * starts from: a new visit.
 */
void dt_color_well_set_color(GtkWidget *well, const GdkRGBA *color);

/** @brief The colour the well holds now. */
void dt_color_well_get_color(GtkWidget *well, GdkRGBA *color);

/**
 * @brief End what is in flight: a drag still held is let go of, a hexadecimal number typed and not
 * validated is applied if it reads as a colour, digits typed into the opacity are applied. Each
 * reports as its own gesture would.
 * @return FALSE when the typed number does not read as a colour: it is left as typed, marked as an
 * error, and nothing is applied from it.
 */
gboolean dt_color_well_commit_pending(GtkWidget *well);

/** @brief Give the keyboard to the hexadecimal number, its text selected, so a colour can be typed now. */
void dt_color_well_grab_focus(GtkWidget *well);

/**
 * @brief Go back to the colour last set and the recent colours as they were then, reporting
 * nothing, storing the recent colours back. A drag still held is let go of without a report.
 */
void dt_color_well_revert(GtkWidget *well);

/**
 * @brief Read a hexadecimal colour: #rgb, #rgba, #rrggbb or #rrggbbaa, the # optional, spaces around
 * it ignored, letters in either case.
 * @param color written only when the text reads as a colour; a text without alpha digits leaves
 * its alpha alone.
 * @param has_alpha whether the text carried alpha digits; may be NULL.
 * @return whether the text reads as a colour.
 */
gboolean dt_color_well_parse_hex(const char *text, GdkRGBA *color, gboolean *has_alpha);

/**
 * @brief Write a colour as "#RRGGBB", or "#RRGGBBAA" when @p with_alpha and it is not opaque to the
 * byte. Each channel is rounded to the nearest of 256 steps.
 */
void dt_color_well_format_hex(const GdkRGBA *color, gboolean with_alpha, char *text, size_t size);

/**
 * @brief Whether two colours are the same to the byte, opacity included: the precision a colour is
 * shown, typed and stored at, and so what tells a change from none.
 */
gboolean dt_color_well_same_color(const GdkRGBA *first, const GdkRGBA *second);

/**
 * @brief Put a colour at the head of a list of recent colours: taken out of wherever it already
 * was -- equal to the byte, as the list is stored -- and the list cut at DT_COLOR_WELL_HISTORY_MAX.
 * @return the list's new length.
 */
int dt_color_well_history_push(GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX], int count, const GdkRGBA *color);

/** @brief Read a stored list of recent colours; what does not read as a colour is skipped. @return its length. */
int dt_color_well_history_parse(const char *text, GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX]);

/** @brief Write a list of recent colours as it is stored. To free with g_free(). */
gchar *dt_color_well_history_format(const GdkRGBA history[DT_COLOR_WELL_HISTORY_MAX], int count);

/**
 * @brief Convert hue, saturation and value, each in [0, 1], to display RGB.
 * @param hue 0 and 1 are both red.
 */
void dt_color_well_hsv_to_rgb(double hue, double saturation, double value, double *red, double *green,
                              double *blue);

/** @brief Convert display RGB to hue, saturation and value; a grey's hue is 0. */
void dt_color_well_rgb_to_hsv(double red, double green, double blue, double *hue, double *saturation,
                              double *value);

G_END_DECLS

#endif // DT_WIDGETS_COLOR_WELL_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
