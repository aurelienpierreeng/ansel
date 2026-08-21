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

#ifndef DT_GUI_SCREEN_METRICS_H
#define DT_GUI_SCREEN_METRICS_H

#include "system/surface_scaling.h"

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
 * This is state, so it is not in src/system: that directory answers "is this stateless?" with
 * its own name, and one exception would cost every reader the check the arrangement exists to
 * avoid. The scaling arithmetic these values feed has no state and did stay there, in
 * system/surface_scaling.h, taking the scale as an argument. The constructors below are those
 * same functions bound to the current screen.
 *
 * It lives in gui/ because the GUI is what resolves these values -- it is the only thing that
 * can interrogate a display. That makes every reader below layer 4 an upward include, which
 * is not a regression but the coupling becoming visible: code in common/ that needs to know
 * the screen's DPI always depended on there being a screen. Callers that know their own scale
 * should use system/surface_scaling.h directly and depend on nothing.
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

/* The scaling helpers from system/surface_scaling.h, bound to the current screen. Callers that
 * already know their scale -- an export at a fixed size, a test -- should use those directly
 * and stay independent of the display. */

static inline cairo_surface_t *dt_cairo_image_surface_create(cairo_format_t format, int width, int height)
{
  return dt_cairo_surface_create_at_scale(format, width, height, dt_screen_ppd());
}

static inline cairo_surface_t *dt_cairo_image_surface_create_for_data(unsigned char *data, cairo_format_t format,
                                                                     int width, int height, int stride)
{
  return dt_cairo_surface_create_for_data_at_scale(data, format, width, height, stride, dt_screen_ppd());
}

static inline cairo_surface_t *dt_cairo_image_surface_create_from_png(const char *filename)
{
  return dt_cairo_surface_create_from_png_at_scale(filename, dt_screen_ppd());
}

static inline int dt_cairo_image_surface_get_width(cairo_surface_t *surface)
{
  return dt_cairo_surface_logical_width(surface, dt_screen_ppd());
}

static inline int dt_cairo_image_surface_get_height(cairo_surface_t *surface)
{
  return dt_cairo_surface_logical_height(surface, dt_screen_ppd());
}

G_END_DECLS

#endif // DT_GUI_SCREEN_METRICS_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
