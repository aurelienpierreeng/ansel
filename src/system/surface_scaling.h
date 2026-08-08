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

#ifndef DT_SYSTEM_SURFACE_SCALING_H
#define DT_SYSTEM_SURFACE_SCALING_H

/* Device-scale-aware cairo surfaces: allocate at `scale` resolution and tell cairo about it,
 * so callers keep drawing in logical coordinates and get a sharp result on a HiDPI screen.
 *
 * The scale is an argument, not a global. That is what keeps this file -- and therefore
 * src/system -- stateless: the arithmetic is the same whoever asks and whatever the display
 * is doing. Where the current screen's scale comes from is gui/screen_metrics.h, which
 * owns that value and offers the same three constructors already bound to it.
 */

#include <cairo.h>
#include <glib.h>

G_BEGIN_DECLS

static inline cairo_surface_t *dt_cairo_surface_create_at_scale(cairo_format_t format, int width, int height,
                                                                double scale)
{
  cairo_surface_t *cst = cairo_image_surface_create(format, width * scale, height * scale);
  cairo_surface_set_device_scale(cst, scale, scale);
  return cst;
}

static inline cairo_surface_t *dt_cairo_surface_create_for_data_at_scale(unsigned char *data,
                                                                        cairo_format_t format, int width,
                                                                        int height, int stride, double scale)
{
  cairo_surface_t *cst = cairo_image_surface_create_for_data(data, format, width, height, stride);
  cairo_surface_set_device_scale(cst, scale, scale);
  return cst;
}

static inline cairo_surface_t *dt_cairo_surface_create_from_png_at_scale(const char *filename, double scale)
{
  cairo_surface_t *cst = cairo_image_surface_create_from_png(filename);
  cairo_surface_set_device_scale(cst, scale, scale);
  return cst;
}

/* The size a device-scaled surface occupies in logical coordinates -- what the caller asked
 * for, not the scale-multiplied pixel count cairo actually allocated. */

static inline int dt_cairo_surface_logical_width(cairo_surface_t *surface, double scale)
{
  return cairo_image_surface_get_width(surface) / scale;
}

static inline int dt_cairo_surface_logical_height(cairo_surface_t *surface, double scale)
{
  return cairo_image_surface_get_height(surface) / scale;
}

G_END_DECLS

#endif // DT_SYSTEM_SURFACE_SCALING_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
