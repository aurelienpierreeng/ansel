/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.

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

/** @file control/input.h
 *
 * @brief Which mouse buttons are held, asked of the toolkit rather than remembered.
 *
 * @details dt_control_t used to carry `button_down' and `button_down_which', written by our
 * button-press and button-release handlers and read by drag state machines in the darkroom, in
 * crop, clipping and vignette. That is a copy of something GDK already knows: the pointer's
 * button mask is device state, available at any moment.
 *
 * A copy of live state has to be refreshed to stay true, and is wrong in between. Every reader
 * consulted it in the middle of a drag, so a missed release -- a broken grab, a window switch,
 * a crash in a handler between press and release -- left a stuck drag that nothing would clear
 * until the next press. Asking the source cannot go stale.
 *
 * views/darkroom.c already made the point on its own: at darkroom.c:2433 it calls
 * gdk_window_get_device_position() for the pointer position and passes NULL for the fifth
 * argument -- the GdkModifierType out-param that carries exactly these bits -- and then read
 * the stored copy instead.
 *
 * NOTE what is deliberately NOT here. `button_x'/`button_y' are the position at PRESS time, an
 * anchor a drag measures against; GDK can say where the pointer is, never where it went down,
 * so that is real remembered state and belongs to whoever is dragging (views/darkroom.c is the
 * only user, and writes them itself). `button_type' -- single/double/triple click -- is an
 * event classification GDK computes when delivering the event and does not expose as device
 * state; it had no reader at all and is gone.
 */

#ifndef DT_CONTROL_INPUT_H
#define DT_CONTROL_INPUT_H

#include <glib.h>

/* C linkage: see control/user_message.h for why every header split out of control.h needs it. */
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Is mouse button @p which currently held down?
 *
 * @param which 1 = left, 2 = middle, 3 = right, as in GdkEventButton.button.
 *
 * @return TRUE while the button is physically down. Asked of GDK on every call, so it cannot
 * be stale; FALSE when there is no display, no seat or no pointer (headless included).
 *
 * @details This reports the DEVICE's state, not "a drag we started". A button pressed over
 * another window and still held when the pointer enters ours reads TRUE here, where the old
 * stored flag -- set only by our own press handler -- read FALSE. Every current caller pairs it
 * with its own grab or hover test, so the distinction does not reach behaviour; a new caller
 * that needs "did the press land on us" wants a press handler, not this.
 */
gboolean dt_control_button_down(int which);

#ifdef __cplusplus
}
#endif

#endif // DT_CONTROL_INPUT_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
