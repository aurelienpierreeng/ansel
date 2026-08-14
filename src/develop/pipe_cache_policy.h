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

/** @file develop/pipe_cache_policy.h
 *
 * @brief Whether one pipeline node must keep a host-RAM copy of its output.
 *
 * @details Split out of _seal_opencl_cache_policy() so the decision is a pure function of
 * named inputs. That is not tidiness: the outcome it computes -- per-piece
 * cache_output_on_ram -- is invisible to every check this project runs. It changes no pixel
 * in an export (the export path drives the pipe directly and never reaches the seal), it
 * changes no hash, and it produces no log unless someone is already looking. The one time it
 * was wrong, the symptom was a downstream module silently reading stale host bytes from a
 * rekeyed cacheline, and it was found by dumping OpenCL buffers, not by a test.
 *
 * A pure function can be pinned by one, which is what src/tests/unittests/test_pipe_cache_policy.c
 * does. Gathering the inputs -- which of them come from the GUI, which from the pipe -- stays
 * in dev_pixelpipe.c, where it can see those things.
 */

#ifndef DT_DEVELOP_PIPE_CACHE_POLICY_H
#define DT_DEVELOP_PIPE_CACHE_POLICY_H

#include <glib.h>

#include "system/macros.h"   // IS_NULL_PTR

/** @brief Everything the decision depends on, named. */
typedef struct dt_dev_pipe_cache_policy_inputs_t
{
  /** The module authored this itself (piece->cache_output_on_ram before the seal). */
  gboolean authored_cache;
  /** The user pinned it through /plugins/<op>/cache. */
  gboolean user_requested_cache;
  /** This node can run on the GPU at all: OpenCL inited, the piece is CL-ready, the module
   *  has a process_cl. A node that cannot is by definition producing host data. */
  gboolean supports_opencl;
  /** A colour picker is sampling this module's output. */
  gboolean color_picker_on;
  /** The global histogram reads this module's OUTPUT. */
  gboolean global_hist_output_on;
  /** The global histogram reads this module's INPUT, so the module BEFORE it must publish. */
  gboolean global_hist_input_on;
  /** The module computes its own histogram from its input. */
  gboolean module_hist_on;
  /** This is the module the darkroom currently has focused. */
  gboolean active_in_gui;
  /** An autoset pass wants this module's input. */
  gboolean has_autoset;
} dt_dev_pipe_cache_policy_inputs_t;

/**
 * @brief Decide one node's host-cache requirement, walking the pipe from the end backwards.
 *
 * @param in The node's own inputs.
 * @param inherited_requirement What the nodes downstream of this one already established.
 * @param[out] upstream_requirement What to hand the node before this one. Never smaller than
 * @p inherited_requirement -- see the note below, it is the whole reason this is a function
 * and not an expression.
 *
 * @return TRUE when this node must keep its output in host RAM.
 */
static inline gboolean dt_dev_pipe_cache_policy_decide(const dt_dev_pipe_cache_policy_inputs_t *in,
                                                       const gboolean inherited_requirement,
                                                       gboolean *upstream_requirement)
{
  // What THIS node needs from the node before it.
  const gboolean own_input_requirement
      = !in->supports_opencl || in->active_in_gui || in->module_hist_on
        || in->global_hist_input_on || in->has_autoset;

  // A GPU-capable node that needs no host input of its own must not ERASE a requirement
  // inherited from further downstream -- a CPU-only module reached through it, or one that is
  // disabled right now but was enabled a moment ago and left a stale host-less cacheline
  // behind. This was an `=' once, and an intermediate GPU module silently reset the flag; the
  // module before it then skipped a readback that a later, non-adjacent consumer needed, and
  // read the previous life of a rekeyed cacheline. Only reproducible with OpenCL enabled.
  if(!IS_NULL_PTR(upstream_requirement))
    *upstream_requirement = own_input_requirement || inherited_requirement;

  return in->authored_cache || in->user_requested_cache || in->color_picker_on
         || in->global_hist_output_on || inherited_requirement;
}

#endif // DT_DEVELOP_PIPE_CACHE_POLICY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
