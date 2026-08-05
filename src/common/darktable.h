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

#define DT_MODULE_VERSION 23 // version of dt's module interface

// version of current performance configuration version
// if you want to run an updated version of the performance configuration later
// bump this number and make sure you have an updated logic in dt_configure_performance()
#define DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION 11
#define DT_PERF_INFOSIZE 4096

// every module has to define this:
#ifdef _DEBUG
#define DT_MODULE(MODVER)                                                                                    \
  int dt_module_dt_version()                                                                                 \
  {                                                                                                          \
    return -DT_MODULE_VERSION;                                                                               \
  }                                                                                                          \
  int dt_module_mod_version()                                                                                \
  {                                                                                                          \
    return MODVER;                                                                                           \
  }
#else
#define DT_MODULE(MODVER)                                                                                    \
  int dt_module_dt_version()                                                                                 \
  {                                                                                                          \
    return DT_MODULE_VERSION;                                                                                \
  }                                                                                                          \
  int dt_module_mod_version()                                                                                \
  {                                                                                                          \
    return MODVER;                                                                                           \
  }
#endif

#define DT_MODULE_INTROSPECTION(MODVER, PARAMSTYPE) DT_MODULE(MODVER)

// ..to be able to compare it against this:
static inline int dt_version()
{
#ifdef _DEBUG
  return -DT_MODULE_VERSION;
#else
  return DT_MODULE_VERSION;
#endif
}

// returns the darktable version as <major>.<minor>
char *dt_version_major_minor();

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

#define DT_IMAGE_DBLOCKS 64

// Default code for imgid meaning the picture is unknown or invalid
#define UNKNOWN_IMAGE -1

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

typedef float dt_boundingbox_t[4];  //(x,y) of upperleft, then (x,y) of lowerright

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

typedef struct
{
  double clock;
  double user;
} dt_times_t;

extern darktable_t darktable;

int dt_init(int argc, char *argv[], const gboolean init_gui, const gboolean load_data);
void dt_cleanup();

/* ------------------------------------------------------------------------------------------
 * Widget-callback suppression (replaces the legacy raw `darktable.gui->reset` counter).
 *
 * Programmatic widget updates must not re-trigger their own "value-changed" handlers. Bracket
 * such updates with dt_gui_freeze_begin()/dt_gui_freeze_end() (or the dt_gui_widget_freeze()
 * scope guard), and open every widget callback with `if(dt_gui_widgets_suppressed()) return;`.
 *
 * The depth is managed centrally: it is mutated only on the GUI thread (off-thread begin/end
 * are no-ops, so worker threads -- e.g. thumbnail/export reload_defaults -- can never race or
 * drift it), clamped at >= 0, and any unbalanced end is logged with its file:line rather than
 * silently drifting negative and disabling suppression for the rest of the session.
 * ------------------------------------------------------------------------------------------ */
gboolean dt_gui_widgets_suppressed(void);
void dt_gui_freeze_begin_(const char *file, int line);
void dt_gui_freeze_end_(const char *file, int line);
void dt_gui_freeze_reset(void); // hard-reset depth to 0 (GUI init only)
#define dt_gui_freeze_begin() dt_gui_freeze_begin_(__FILE__, __LINE__)
#define dt_gui_freeze_end()   dt_gui_freeze_end_(__FILE__, __LINE__)

typedef struct { const char *file; int line; } dt_gui_freeze_token_t;
static inline void dt_gui_freeze_release_(dt_gui_freeze_token_t *t)
{
  dt_gui_freeze_end_(t->file, t->line);
}
// Scope guard: begins a freeze that is automatically ended when the enclosing block exits,
// including via early return/goto/break. Use it for spans that contain an early exit (the
// raw begin/end pair would leak the depth on such paths).
#define DT_FREEZE_CAT_(a, b) a##b
#define DT_FREEZE_CAT(a, b) DT_FREEZE_CAT_(a, b)
#define dt_gui_widget_freeze()                                                       \
  dt_gui_freeze_token_t DT_FREEZE_CAT(_dt_freeze_guard_, __LINE__)                    \
      __attribute__((cleanup(dt_gui_freeze_release_))) = { __FILE__, __LINE__ };      \
  dt_gui_freeze_begin_(__FILE__, __LINE__)

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

// check whether the specified mask of modifier keys exactly matches, among the set Shift+Control+(Alt/Meta).
// ignores the state of any other shifting keys
static inline gboolean dt_modifier_is(const GdkModifierType state, const GdkModifierType desired_modifier_mask)
{
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
//TODO: on Macs, remap the GDK_CONTROL_MASK bit in desired_modifier_mask to be the bit for the Cmd key
  return (state & modifiers) == desired_modifier_mask;
}

// check whether the given modifier state includes AT LEAST the specified mask of modifier keys
static inline gboolean dt_modifiers_include(const GdkModifierType state, const GdkModifierType desired_modifier_mask)
{
//TODO: on Macs, remap the GDK_CONTROL_MASK bit in desired_modifier_mask to be the bit for the Cmd key
  const GdkModifierType modifiers = gtk_accelerator_get_default_mod_mask();
  // check whether all modifier bits of interest are turned on
  return (state & (modifiers & desired_modifier_mask)) == desired_modifier_mask;
}

int dt_capabilities_check(char *capability);
void dt_capabilities_add(char *capability);
void dt_capabilities_remove(char *capability);
void dt_capabilities_cleanup();

static inline double dt_get_wtime(void)
{
  struct timeval time;
  gettimeofday(&time, NULL);
  return time.tv_sec - 1290608000 + (1.0 / 1000000.0) * time.tv_usec;
}

static inline void dt_get_times(dt_times_t *t)
{
  struct rusage ru;

  getrusage(RUSAGE_SELF, &ru);
  t->clock = dt_get_wtime();
  t->user = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * (1.0 / 1000000.0);
}

void dt_show_times(const dt_times_t *start, const char *prefix);

void dt_show_times_f(const dt_times_t *start, const char *prefix, const char *suffix, ...) __attribute__((format(printf, 3, 4)));

/** \brief check if file is a supported image */
gboolean dt_supported_image(const gchar *filename);

// a few macros and helper functions to speed up certain frequently-used GLib operations
#define g_list_is_singleton(list) ((list) && (!(list)->next))
static inline gboolean g_list_shorter_than(const GList *list, unsigned len)
{
  // instead of scanning the full list to compute its length and then comparing against the limit,
  // bail out as soon as the limit is reached.  Usage: g_list_shorter_than(l,4) instead of g_list_length(l)<4
  while (len-- > 0)
  {
    if (!list) return TRUE;
    list = g_list_next(list);
  }
  return FALSE;
}

// advance the list by one position, unless already at the final node
static inline GList *g_list_next_bounded(GList *list)
{
  return g_list_next(list) ? g_list_next(list) : list;
}

static inline const GList *g_list_next_wraparound(const GList *list, const GList *head)
{
  return g_list_next(list) ? g_list_next(list) : head;
}

static inline const GList *g_list_prev_wraparound(const GList *list)
{
  // return the prior element of the list, unless already on the first element; in that case, return the last
  // element of the list.
  return g_list_previous(list) ? g_list_previous(list) : g_list_last((GList*)list);
}

void dt_print_mem_usage();

void dt_configure_runtime_performance(dt_sys_resources_t *resources, gboolean init_gui);

// helper function which loads whatever image_to_load points to: single image files or whole directories
// it tells you if it was a single image or a directory in single_image (when it's not NULL)
int dt_load_from_string(const gchar *image_to_load, gboolean open_image_in_dr, gboolean *single_image);

/** define for max path/filename length */
#define DT_MAX_FILENAME_LEN 256

#ifndef PATH_MAX
/*
 * from /usr/include/linux/limits.h (Linux 3.16.5)
 * Some systems might not define it (e.g. Hurd)
 *
 * We do NOT depend on any specific value of this env variable.
 * If you want constant value across all systems, use DT_MAX_PATH_FOR_PARAMS!
 */
#define PATH_MAX 4096
#endif

/*
 * ONLY TO BE USED FOR PARAMS!!! (e.g. dt_imageio_disk_t)
 *
 * WARNING: this should *NEVER* be changed, as it will break params,
 *          created with previous DT_MAX_PATH_FOR_PARAMS.
 */
#define DT_MAX_PATH_FOR_PARAMS 4096

static inline gchar *dt_string_replace(const char *string, const char *to_replace)
{
  if(IS_NULL_PTR(string) || IS_NULL_PTR(to_replace)) return NULL;
  gchar **split = g_strsplit(string, to_replace, -1);
  gchar *text = g_strjoinv("", split);
  g_strfreev(split);
  return text;
}

// Remove underscore from GUI labels containing mnemonics
static inline gchar *delete_underscore(const char *s)
{
  return dt_string_replace(s, "_");
}

/**
 * @brief Remove Pango/Gtk markup and accels mnemonics from text labels.
 * If the markup parsing fails, fallback to returning a copy of the original string.
 *
 * @param s Original string to clean
 * @return gchar* Newly-allocated string. The caller is responsible for freeing it.
 */
static inline gchar *strip_markup(const char *s)
{
  if(IS_NULL_PTR(s)) return g_strdup("");

  PangoAttrList *attrs = NULL;
  gchar *plain = NULL;

  const gchar *underscore = "_";
  gunichar mnemonic = underscore[0];
  if(!pango_parse_markup(s, -1, mnemonic, &attrs, &plain, NULL, NULL))
    plain = delete_underscore(s);

  pango_attr_list_unref(attrs);
  return plain;
}

/**
 * @brief Append a constant filename to a variable, stack-based, fixed-sized, directory, 
 * and add a `/` in-between
 * 
 * @param destination 
 * @param variable 
 * @param string 
 */
void dt_concat_path_file(char destination[PATH_MAX], const char path[PATH_MAX], const char *const file);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
