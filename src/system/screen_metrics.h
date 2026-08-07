/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

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

#ifndef DT_SYSTEM_SCREEN_METRICS_H
#define DT_SYSTEM_SCREEN_METRICS_H

#include <cairo.h>
#include <glib.h>

G_BEGIN_DECLS

/* How dense the screen is, and how much cairo has to draw per logical pixel.
 *
 * These are hardware facts -- the same category as the monitor's ICC profile in
 * system/display_profile.h -- and they had ended up owned by the GUI (dt_gui_gtk_t) and
 * mirrored in widgets/widget_settings.c, because that is who resolves them. Anyone below
 * layer 4 who needed them (the crash reporter, the telemetry payload, the SVG rasteriser)
 * therefore had to include gui/gtk.h to read three doubles.
 *
 * They live here instead, as the single copy. Whoever can interrogate the display pushes
 * them in once -- dt_gui_gtk_init() does, from GDK -- and everyone else reads.
 *
 * Before that push, the getters return neutral values (1.0 scaling, 96 dpi), which is what
 * makes a headless run and early startup work without a single "is there a GUI?" test.
 */

/** Screen resolution in dots per inch. Defaults to 96.0 (the X/GDK default). */
double dt_screen_dpi(void);
void dt_screen_set_dpi(double dpi);

/** dpi / 96, i.e. how much to scale UI lengths by. Defaults to 1.0. */
double dt_screen_dpi_factor(void);
void dt_screen_set_dpi_factor(double factor);

/** Device pixels per logical pixel (HiDPI/Retina scaling). Defaults to 1.0. */
double dt_screen_ppd(void);
void dt_screen_set_ppd(double ppd);

/** TRUE once something has actually interrogated a display and pushed real values.
 *
 *  The getters above are always safe to call, but they answer with neutral defaults until
 *  this returns TRUE. Use it where reporting an invented 96dpi/1.0 would be worse than
 *  reporting nothing -- a crash report or a telemetry payload -- and NOT as a "is there a
 *  GUI?" test for scaling, where the neutral defaults are exactly the right answer. */
gboolean dt_screen_metrics_probed(void);

/** Height of the theme font's "em", in pixels. Defaults to 16.0. */
double dt_screen_em_size(void);
void dt_screen_set_em_size(double em);

/* Device-scale-aware cairo surfaces: allocate at ppd resolution and tell cairo about it, so
 * callers keep drawing in logical coordinates and get a sharp result on a HiDPI screen. */

static inline cairo_surface_t *dt_cairo_image_surface_create(cairo_format_t format, int width, int height)
{
  cairo_surface_t *cst = cairo_image_surface_create(format, width * dt_screen_ppd(), height * dt_screen_ppd());
  cairo_surface_set_device_scale(cst, dt_screen_ppd(), dt_screen_ppd());
  return cst;
}

static inline cairo_surface_t *dt_cairo_image_surface_create_for_data(unsigned char *data, cairo_format_t format,
                                                                     int width, int height, int stride)
{
  cairo_surface_t *cst = cairo_image_surface_create_for_data(data, format, width, height, stride);
  cairo_surface_set_device_scale(cst, dt_screen_ppd(), dt_screen_ppd());
  return cst;
}

G_END_DECLS

#endif // DT_SYSTEM_SCREEN_METRICS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
