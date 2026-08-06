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

#ifndef DT_COMMON_GLOBAL_MUTEXES_H
#define DT_COMMON_GLOBAL_MUTEXES_H

/* Process-wide locks owned by the orchestrator (common/darktable.c).
 *
 * They are process-wide because the thing they serialize is process-wide -- a
 * non-reentrant third-party library, or a resource the whole application shares.
 * There is no per-call context to carry them on, so an accessor is the end state
 * here, not a stepping stone: see doc/globals-migration.md, category "process-wide
 * buses".
 *
 * Declared here so that the modules which need them (iop/lens.cc, iop/watermark.c,
 * imageio/storage/disk.c, common/imageio_rawspeed.cc, ...) do not have to include
 * common/darktable.h, and therefore the whole application, to take a lock. */

#include "common/dtpthread.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Serializes plugin calls that are not thread-safe, notably at export time when
 *  several pipelines would otherwise enter the same library concurrently. */
dt_pthread_mutex_t *dt_plugin_threadsafe_mutex(void);

/** Prevents concurrent export/thumbnail pipelines from running at the same time.
 *  This buys no throughput -- the CPU is the bottleneck and the pixel code is already
 *  multi-threaded internally through OpenMP -- it bounds peak memory instead. */
dt_pthread_mutex_t *dt_pipeline_threadsafe_mutex(void);

/** Exiv2 readMetadata() was not thread-safe prior to 0.27. */
dt_pthread_mutex_t *dt_exiv2_threadsafe_mutex(void);

/** RawSpeed readFile() is apparently not thread-safe. */
dt_pthread_mutex_t *dt_readfile_mutex(void);

/** Serializes SQL transactions and image metadata/history reads and writes across all
 *  pipeline jobs and threads: sqlite refuses to start a transaction within a
 *  transaction, which is what "too many" concurrent writers produce. */
dt_pthread_rwlock_t *dt_database_threadsafe_lock(void);

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_GLOBAL_MUTEXES_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
