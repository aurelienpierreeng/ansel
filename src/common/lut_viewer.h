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
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/dtpthread.h"
#include "common/gui_module_api.h"
#include "common/iop_profile.h"

#include <stddef.h>
#include <gtk/gtk.h>

typedef struct dt_lut_viewer_t dt_lut_viewer_t;

typedef struct dt_lut_viewer_control_node_t
{
  float input_rgb[3];
  float output_rgb[3];
} dt_lut_viewer_control_node_t;

/**
 * Build a reusable GTK widget able to preview a 3D LUT as target/output RGB
 * samples projected from the RGB cube onto a Cairo surface.
 */
dt_lut_viewer_t *dt_lut_viewer_new(dt_gui_module_t *module);

/**
 * Release the viewer private state. The GTK widget hierarchy is still owned by
 * its parent container and will be destroyed there.
 */
void dt_lut_viewer_destroy(dt_lut_viewer_t **viewer);

/**
 * Return the top-level GTK widget packing the controls and the drawing area.
 */
GtkWidget *dt_lut_viewer_get_widget(dt_lut_viewer_t *viewer);

/**
 * Update the currently displayed LUT. The viewer does not take ownership of
 * the CLUT memory and expects it to stay valid until the next update. If a
 * read/write lock is provided, the viewer will acquire a read lock while
 * rebuilding its cached Cairo surface from that CLUT.
 */
void dt_lut_viewer_set_lut(dt_lut_viewer_t *viewer, const float *clut, uint16_t level,
                           dt_pthread_rwlock_t *clut_lock,
                           const dt_iop_order_iccprofile_info_t *lut_profile,
                           const dt_iop_order_iccprofile_info_t *display_profile);

/**
 * Update the list of sparse control nodes displayed by the viewer when the
 * caller enables the dedicated control-node mode. The viewer does not take
 * ownership of this memory and expects it to stay valid until the next update.
 */
void dt_lut_viewer_set_control_nodes(dt_lut_viewer_t *viewer,
                                     const dt_lut_viewer_control_node_t *control_nodes,
                                     size_t control_node_count);

/**
 * Queue a redraw of the drawing area after the cache has been invalidated.
 */
void dt_lut_viewer_queue_draw(dt_lut_viewer_t *viewer);
