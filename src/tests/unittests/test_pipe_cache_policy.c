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

/** Which pipeline nodes must keep a host-RAM copy of their output.
 *
 * These tests exist because nothing else can see this. The per-piece cache_output_on_ram flag
 * changes no exported pixel -- the export path drives the pipe directly and never reaches the
 * seal that computes it -- changes no hash, and produces no log unless someone is already
 * looking. When it was wrong, the symptom was a downstream module reading stale host bytes
 * from a rekeyed cacheline, only with OpenCL enabled, and it was found by dumping GPU buffers.
 *
 * The propagation test below is that bug, pinned.
 */

#include "develop/pipe_cache_policy.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_metadata_notify.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

/** A GPU-capable node with no reason of its own to want host data. */
static dt_dev_pipe_cache_policy_inputs_t _gpu_node_with_no_needs(void)
{
  dt_dev_pipe_cache_policy_inputs_t in = { 0 };
  in.supports_opencl = TRUE;
  return in;
}

/** A node that cannot run on the GPU at all: it produces host data by construction. */
static void _cpu_only_node_needs_its_input_on_host(void **state)
{
  (void)state;
  dt_dev_pipe_cache_policy_inputs_t in = { 0 };
  in.supports_opencl = FALSE;

  gboolean upstream = FALSE;
  const gboolean own = dt_dev_pipe_cache_policy_decide(&in, FALSE, &upstream);

  // It requires nothing of its OWN output -- nobody downstream asked for it ...
  assert_false(own);
  // ... but the node before it must publish to host, because this one reads on the CPU.
  assert_true(upstream);
}

static void _gpu_node_requires_nothing_on_its_own(void **state)
{
  (void)state;
  dt_dev_pipe_cache_policy_inputs_t in = _gpu_node_with_no_needs();

  gboolean upstream = TRUE;
  const gboolean own = dt_dev_pipe_cache_policy_decide(&in, FALSE, &upstream);

  assert_false(own);
  assert_false(upstream);
}

/**
 * THE REGRESSION. An enabled, GPU-capable node that needs no host input of its own must PASS
 * THROUGH a requirement inherited from further downstream, not replace it.
 *
 * This was an `=' rather than an `||'. Toggling on a GPU module with no host needs of its own
 * (iop/rawoverexposed.c: NO_HISTORY_STACK, GPU-capable, no GUI or histogram reason) erased the
 * TRUE that a CPU-only module further downstream (dither) had correctly established. colorout
 * then skipped its GPU-to-host readback, and dither read the stale bytes left in the cacheline's
 * previous life -- every hash and ROI in the chain individually correct.
 */
static void _gpu_node_propagates_an_inherited_requirement(void **state)
{
  (void)state;
  dt_dev_pipe_cache_policy_inputs_t in = _gpu_node_with_no_needs();

  gboolean upstream = FALSE;
  const gboolean own = dt_dev_pipe_cache_policy_decide(&in, TRUE, &upstream);

  // The inherited requirement reaches this node's own output ...
  assert_true(own);
  // ... AND keeps travelling to the node before it. An `=' here would make this FALSE.
  assert_true(upstream);
}

/** Each of the five own-input reasons must raise the upstream requirement by itself. */
static void _each_own_input_reason_raises_upstream(void **state)
{
  (void)state;
  const size_t offsets[] = {
    offsetof(dt_dev_pipe_cache_policy_inputs_t, active_in_gui),
    offsetof(dt_dev_pipe_cache_policy_inputs_t, module_hist_on),
    offsetof(dt_dev_pipe_cache_policy_inputs_t, global_hist_input_on),
    offsetof(dt_dev_pipe_cache_policy_inputs_t, has_autoset),
  };

  for(size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
  {
    dt_dev_pipe_cache_policy_inputs_t in = _gpu_node_with_no_needs();
    *(gboolean *)((char *)&in + offsets[i]) = TRUE;

    gboolean upstream = FALSE;
    dt_dev_pipe_cache_policy_decide(&in, FALSE, &upstream);
    assert_true(upstream);
  }
}

/** Each of the four own-output reasons must raise this node's own requirement by itself. */
static void _each_own_output_reason_raises_own(void **state)
{
  (void)state;
  const size_t offsets[] = {
    offsetof(dt_dev_pipe_cache_policy_inputs_t, authored_cache),
    offsetof(dt_dev_pipe_cache_policy_inputs_t, user_requested_cache),
    offsetof(dt_dev_pipe_cache_policy_inputs_t, color_picker_on),
    offsetof(dt_dev_pipe_cache_policy_inputs_t, global_hist_output_on),
  };

  for(size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++)
  {
    dt_dev_pipe_cache_policy_inputs_t in = _gpu_node_with_no_needs();
    *(gboolean *)((char *)&in + offsets[i]) = TRUE;

    gboolean upstream = FALSE;
    assert_true(dt_dev_pipe_cache_policy_decide(&in, FALSE, &upstream));
    // None of these four says anything about what the node BEFORE this one must do.
    assert_false(upstream);
  }
}

/** A NULL out-param is allowed: the last node in the walk has nobody upstream to tell. */
static void _null_upstream_pointer_is_allowed(void **state)
{
  (void)state;
  dt_dev_pipe_cache_policy_inputs_t in = _gpu_node_with_no_needs();
  in.color_picker_on = TRUE;

  assert_true(dt_dev_pipe_cache_policy_decide(&in, FALSE, NULL));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_cpu_only_node_needs_its_input_on_host),
    cmocka_unit_test(_gpu_node_requires_nothing_on_its_own),
    cmocka_unit_test(_gpu_node_propagates_an_inherited_requirement),
    cmocka_unit_test(_each_own_input_reason_raises_upstream),
    cmocka_unit_test(_each_own_output_reason_raises_own),
    cmocka_unit_test(_null_upstream_pointer_is_allowed),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
