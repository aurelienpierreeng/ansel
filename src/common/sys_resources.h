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

#ifndef DT_COMMON_SYS_RESOURCES_H
#define DT_COMMON_SYS_RESOURCES_H

/* Memory and worker budgets. The values are owned by the orchestrator
 * (common/darktable.c, which fills dt_sys_resources_t once at startup), but the
 * consumers are low-level compute units -- the pixelpipe cache, tiling, the mipmap
 * cache, the memory arena -- which must not have to include the whole application
 * to ask how much RAM they may use. */

#include <glib.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dt_sys_resources_t
{
  size_t total_memory;          // All RAM on system
  size_t mipmap_memory;         // RAM allocated to mipmap cache
  size_t headroom_memory;       // RAM left to OS & other Apps
  size_t pixelpipe_memory;      // RAM used by the pixelpipe cache (approx.)
  size_t pressure_floor_memory; // System-wide available RAM under which we shed caches
} dt_sys_resources_t;

void dt_configure_runtime_performance(dt_sys_resources_t *resources, gboolean init_gui);

// Number of workers, on top of reserved workers (1 for main preview, 1 for thumbnail in darkroom)
// This is currently set to 2, so 4 workers total, without user config.
// Workers will process a queue of jobs that they share together (except for reserved ones).
// It is useless to use more than 2 workers
// since those jobs very often lock some mutex that prevents concurrent running.
// All jobs finding an idle worker will "start" immediately, as far as the OS knows from outside the program,
// but may do nothing internally except for waiting a mutex locked by another worker/thread.
// In that situation, we loose the ability to flush the queue, since jobs are "running".
// So it's better to have few workers with long queues, rather
// than many workers, to be able to control queued jobs.
int dt_worker_threads();

// Get the remaining memory available for pipeline allocations,
// once we subtracted caches memory and headroom from system memory
size_t dt_get_available_mem();

// Get the maximum size for the whole mipmap cache
size_t dt_get_mipmap_mem();

// Get the total memory (bytes) the process budgets against: physical RAM, capped by
// a container/cgroup limit and the host_memory_limit config. Set once at startup by
// dt_configure_runtime_performance(), never mutated afterwards.
size_t dt_get_total_mem(void);

// Probe the system for currently-available (free + reclaimable) physical RAM, in bytes.
// This is a live system-wide measurement, unrelated to our internal budgets: it shrinks
// when OTHER applications allocate memory. On Linux it also honors a cgroup v2 memory
// limit (containers, Flatpak, systemd slices) when one is set. Returns 0 when the
// platform gives us no way to know — callers must treat 0 as "no information", not as
// "out of memory".
// The value is cached for a few tens of milliseconds (the probe reads several /proc and
// /sys files), so it may lag reality by that much.
size_t dt_get_system_available_mem(void);

// Drop the cached probe value so the next dt_get_system_available_mem() re-reads the OS.
// For callers that just changed the situation themselves (freeing caches) and need the
// resulting number to be ground truth rather than a pre-change snapshot.
void dt_invalidate_system_available_mem(void);

// System-wide available RAM floor (bytes) under which caches must be shed to
// keep the OS and other applications breathing, regardless of anselrc budgets.
// See dt_configure_runtime_performance() for how it is derived.
size_t dt_get_memory_pressure_floor(void);

void dt_print_mem_usage();

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_SYS_RESOURCES_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
