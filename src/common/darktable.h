/*
    This file is part of darktable,
    Copyright (C) 2009-2012 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010, 2012 Pascal de Bruijn.
    Copyright (C) 2010 Richard Hughes.
    Copyright (C) 2010-2020 Tobias Ellinghaus.
    Copyright (C) 2011, 2014-2015 Bruce Guenter.
    Copyright (C) 2011-2013, 2017 Ulrich Pegelow.
    Copyright (C) 2012 Ammon Riley.
    Copyright (C) 2012 Christian Himpel.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 James C. McPherson.
    Copyright (C) 2012 Jean-Sébastien Pédron.
    Copyright (C) 2012-2014 Jérémy Rosen.
    Copyright (C) 2012 Moritz Lipp.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Simon Spannagel.
    Copyright (C) 2013, 2021 Aldric Renaudin.
    Copyright (C) 2013, 2015, 2019-2021 Pascal Obry.
    Copyright (C) 2013-2017 Roman Lebedev.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2015 Matthias Gehre.
    Copyright (C) 2016-2019 Peter Budai.
    Copyright (C) 2016 Stuart Henderson.
    Copyright (C) 2018-2020, 2022-2026 Aurélien PIERRE.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2018 parafin.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2020 Andreas Schneider.
    Copyright (C) 2019-2022 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2020 David-Tillmann Schaefer.
    Copyright (C) 2020-2021 Diederik Ter Rahe.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Hubert Figuière.
    Copyright (C) 2021 Paolo DePetrillo.
    Copyright (C) 2021 Robert Bridge.
    Copyright (C) 2021 Roman Khatko.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Philippe Weyland.
    Copyright (C) 2023-2025 Alynx Zhou.
    Copyright (C) 2023 lologor.
    Copyright (C) 2023 Luca Zulberti.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once


// just to be sure. the build system should set this for us already:
#if defined __DragonFly__ || defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__
#define _WITH_DPRINTF
#define _WITH_GETLINE
#elif !defined _XOPEN_SOURCE && !defined _WIN32
#define _XOPEN_SOURCE 700 // for localtime_r and dprintf
#endif

// needs to be defined before any system header includes for control/conf.h to work in C++ code
#define __STDC_FORMAT_MACROS

#if !defined(O_BINARY)
// To have portable g_open() on *nix and on Windows
#define O_BINARY 0
#endif

#include "external/ThreadSafetyAnalysis.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "common/database.h"
#include "common/fp_mode.h"
#include "common/dtpthread.h"
#include "common/utility.h"

/* Low-level modular libs: darktable.h is the top-level orchestrator and only
 * INHERITS them. New low-level code should include the specific lib it needs
 * instead of this header, which additionally drags the whole application
 * (database, GTK, signals, the darktable_t global) into the translation unit. */
#include "common/macros.h"
#include "common/openmp.h"
#include "common/target_clones.h"
#include "common/mem_alloc.h"
#include "common/simd.h"
#include "common/hash.h"
#include "common/logging.h"
#include "common/times.h"
#include "common/glib_utils.h"
#include "common/module_versioning.h"
#include "common/paths.h"

/* Win32 API surface (windows.h/psapi) plus the #undef of the legacy `near`/`interface`
 * macros windows.h defines. The orchestrator carries it because app-level TUs
 * (darktable.c, main.c) call Win32 memory/file APIs directly. Low-level code must NOT
 * rely on inheriting this: identifiers colliding with those macros are renamed at the
 * source instead. */
#if defined _WIN32
#include "win/win.h"
#endif

#ifdef _WIN32
#include "win/getrusage.h"
#else
#include <sys/resource.h>
#endif
#include <stdint.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <inttypes.h>
#include <json-glib/json-glib.h>
#include <math.h>
#include <sqlite3.h>
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef _RELEASE
#include "common/poison.h"
#endif

#include "common/usermanual_url.h"

// for signal debugging symbols
#include "control/signal.h"

#ifdef __cplusplus
extern "C" {
#endif


// version of current performance configuration version
// if you want to run an updated version of the performance configuration later
// bump this number and make sure you have an updated logic in dt_configure_performance()
#define DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION 11
#define DT_PERF_INFOSIZE 4096


/** Stable, anonymous identifier for the current process/run (a random UUID
 * generated once). Sent to both crash reporting (Sentry) and usage analytics
 * (PostHog) so the same session can be correlated across the two without being
 * double-counted. Not tied to the user or machine. */
const char *dt_session_id(void);

/** Stable, anonymous per-installation identifier (a random UUID persisted in
 * conf). Used as the Sentry user id and the PostHog distinct_id so the same
 * installation/user can be de-duplicated across both systems. Not tied to the
 * machine or any account. */
const char *dt_install_id(void);

#ifdef __cplusplus
}
#endif

/********************************* */


#ifdef __cplusplus
extern "C" {
#endif


/********************************* */

struct dt_gui_gtk_t;
struct dt_control_t;
struct dt_develop_t;
struct dt_mipmap_cache_t;
struct dt_image_cache_t;
struct dt_lib_t;
struct dt_conf_t;
struct dt_points_t;
struct dt_imageio_t;
struct dt_bauhaus_t;
struct dt_undo_t;
struct dt_colorspaces_t;
struct dt_l10n_t;

typedef struct dt_sys_resources_t
{
  size_t total_memory;          // All RAM on system
  size_t mipmap_memory;         // RAM allocated to mipmap cache
  size_t headroom_memory;       // RAM left to OS & other Apps
  size_t pixelpipe_memory;      // RAM used by the pixelpipe cache (approx.)
  size_t pressure_floor_memory; // System-wide available RAM under which we shed caches
} dt_sys_resources_t;

typedef struct darktable_t
{
  int32_t num_openmp_threads;

  int32_t unmuted;
  GList *iop;
  GList *iop_order_list;
  GList *iop_order_rules;

  // Keep track of optional features that may depend on environnement
  // ond compiling options : OpenCL, libsecret, kwallet
  GList *capabilities;
  JsonParser *noiseprofile_parser;
  struct dt_conf_t *conf;
  struct dt_develop_t *develop;
  struct dt_lib_t *lib;
  struct dt_view_manager_t *view_manager;
  struct dt_control_t *control;
  struct dt_control_signal_t *signals;
  struct dt_gui_gtk_t *gui;
  struct dt_mipmap_cache_t *mipmap_cache;
  struct dt_image_cache_t *image_cache;
  struct dt_bauhaus_t *bauhaus;
  const struct dt_database_t *db;
  const struct dt_pwstorage_t *pwstorage;
  struct dt_collection_t *collection;
  struct dt_selection_t *selection;
  struct dt_points_t *points;
  struct dt_imageio_t *imageio;
  struct dt_opencl_t *opencl;
  struct dt_dbus_t *dbus;
  struct dt_undo_t *undo;
  struct dt_colorspaces_t *color_profiles;
  struct dt_l10n_t *l10n;
  struct dt_dev_pixelpipe_cache_t *pixelpipe_cache;

  // Protects from concurrent writing at export time
  dt_pthread_mutex_t plugin_threadsafe;

  // Protect appending/removing GList links to the darktable.capabilities list
  dt_pthread_mutex_t capabilities_threadsafe;

  // Exiv2 readMetadata() was not thread-safe prior to 0.27
  // FIXME: Is it now ?
  dt_pthread_mutex_t exiv2_threadsafe;

  // RawSpeed readFile() method is apparently not thread-safe
  dt_pthread_mutex_t readFile_mutex;

  // Prevent concurrent export/thumbnail pipelines from runnnig at the same time
  // It brings no additional performance since the CPU is our bottleneck,
  // and CPU pixel code is already multi-threaded internally through OpenMP
  dt_pthread_mutex_t pipeline_threadsafe;

  // Building SQL transactions through `dt_database_start_transaction_debug()`
  // from "too many" threads (like loading all thumbnails from a new collection)
  // leads to SQL error:
  // `BEGIN": cannot start a transaction within a transaction`
  // Also, we need to ensure that image metadata/history reads & writes
  // happen each in their all time, from all pipeline jobs/threads.
  dt_pthread_rwlock_t database_threadsafe;

  char *progname;
  char *datadir;
  char *sharedir;
  char *moduledir;
  char *localedir;
  char *tmpdir;
  char *configdir;
  char *cachedir;
  char *kerneldir;
  GList *guides;
  double start_wtime;
  GList *themes;
  int32_t unmuted_signal_dbg_acts;
  gboolean unmuted_signal_dbg[DT_SIGNAL_COUNT];
  GTimeZone *utc_tz;
  GDateTime *origin_gdt;
  struct dt_sys_resources_t dtresources;

  // Working message displayed over the main preview when working
  char *main_message;
} darktable_t;


extern darktable_t darktable;

int dt_init(int argc, char *argv[], const gboolean init_gui, const gboolean load_data);
void dt_cleanup();


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


int dt_capabilities_check(char *capability);
void dt_capabilities_add(char *capability);
void dt_capabilities_remove(char *capability);
void dt_capabilities_cleanup();


/** \brief check if file is a supported image */
gboolean dt_supported_image(const gchar *filename);


void dt_print_mem_usage();

void dt_configure_runtime_performance(dt_sys_resources_t *resources, gboolean init_gui);

// helper function which loads whatever image_to_load points to: single image files or whole directories
// it tells you if it was a single image or a directory in single_image (when it's not NULL)
int dt_load_from_string(const gchar *image_to_load, gboolean open_image_in_dr, gboolean *single_image);





#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
