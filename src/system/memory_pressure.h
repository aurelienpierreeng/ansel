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

#ifndef DT_SYSTEM_MEMORY_PRESSURE_H
#define DT_SYSTEM_MEMORY_PRESSURE_H

/* Kernel memory pressure, as Linux PSI ("pressure stall information") reports it.
 *
 * A budget planned at startup and a floor on available RAM both answer "how much memory is
 * left". Neither sees a system that still has memory available on paper but spends its time
 * reclaiming it: swap full, other applications' pages evicted and faulted straight back in.
 * That stall is what systemd-oomd watches, and it kills on it long before available RAM runs
 * out. This reads the same counters, so the caches can give memory back first. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DT_MEMORY_PRESSURE_MAX_LEVELS 16

/* Cumulative time, in microseconds, during which every non-idle task was stalled on memory --
 * the `total` of PSI's "full" line -- for each level that can come under pressure on our
 * behalf: [0] the whole system (/proc/pressure/memory), then each cgroup v2 from the process's
 * own up to the top of the hierarchy. Returns the number of levels written, 0 where the platform
 * has no PSI.
 *
 * The counters only grow, so the share of a window is the difference of two reads divided by
 * the window's length. The levels come in the same order for as long as the process stays in
 * the same cgroup. Stateless: every call reads the kernel. */
int dt_memory_pressure_read_full_stall(uint64_t *total_us, int max_levels);

/* Have the kernel wake us instead of polling: arm a PSI trigger -- `stall_us` of full stall within
 * any `window_us` -- on every level that accepts one from this process. That is the whole system,
 * and each cgroup above the process that it may write to: under systemd its own scope and
 * app.slice, since the session's user@.service belongs to root. Unprivileged triggers need a
 * window that is a multiple of 2 s; the kernel refuses the others.
 *
 * Writes up to `max_fds` descriptors to `fds` and returns how many. The caller poll()s them for
 * POLLPRI, reads POLLERR as "this level is gone", and closes them. Returns 0 where the platform
 * has no PSI triggers. Stateless. */
int dt_memory_pressure_triggers_open(int *fds, int max_fds, uint64_t stall_us, uint64_t window_us);

#ifdef __cplusplus
}
#endif

#endif // DT_SYSTEM_MEMORY_PRESSURE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
