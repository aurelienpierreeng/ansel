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

/* NO include guard -- a TRIPWIRE instead, on purpose.
 *
 * A guard makes a second inclusion a silent no-op. For the application orchestrator
 * that is exactly the wrong behaviour: this header must be included at most ONCE, by a
 * translation unit that genuinely needs the application (an entry point calling
 * dt_init(), or a subsystem that owns one of the `darktable` members). Reaching it a
 * second time means it arrived through a path nobody intended -- most likely a header
 * started including it again, which is what this whole series removed.
 *
 * So: fail loudly rather than absorb it. If you hit this #error, do not add a guard --
 * find who included it and give that code the specific lib it actually needs
 * (common/logging.h, common/mem_alloc.h, ... ) or the accessor for the global it wants
 * (dt_dev_get_global(), dt_control_get_global(), ...). See doc/include-graph.md.
 *
 * NEVER include this header from another header. As of this commit, zero headers do. */
#ifdef DT_DARKTABLE_H
#error "darktable.h included more than once in this translation unit -- see the comment at the top of that file"
#endif
#define DT_DARKTABLE_H


// just to be sure. the build system should set this for us already:
#if defined __DragonFly__ || defined __FreeBSD__ || defined __NetBSD__ || defined __OpenBSD__
#define _WITH_DPRINTF
#define _WITH_GETLINE
#elif !defined _XOPEN_SOURCE && !defined _WIN32
#define _XOPEN_SOURCE 700 // for localtime_r and dprintf
#endif

// needs to be defined before any system header includes for control/conf.h to work in C++ code
#define __STDC_FORMAT_MACROS

/* O_BINARY moved to common/paths.h, next to the other path/file portability. */

#include "external/ThreadSafetyAnalysis.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Only what the DECLARATIONS below actually need. darktable.h used to carry a large
 * umbrella of includes for the benefit of its consumers; that is what welded the whole
 * application into every translation unit. A file needing dt_print(), dt_alloc_align()
 * or DT_MODULE() must include common/logging.h, common/mem_alloc.h or
 * common/module_versioning.h itself. */
#include "common/dtpthread.h"   // dt_pthread_mutex_t / rwlock members of darktable_t
#include "system/sys_resources.h" // dt_sys_resources_t member of darktable_t
#include "control/signal.h"     // DT_SIGNAL_COUNT sizes the unmuted_signal_dbg array

#include <glib.h>
#include <json-glib/json-glib.h>
#include <stdint.h>

/* win/win.h (windows.h/psapi + the #undef of the legacy `near`/`grp2`/`interface`
 * macros) now comes in through common/macros.h, which every TU includes. It used to
 * live here, which meant dropping this header silently dropped the shim too --
 * a MinGW-only breakage, far from its cause. Nothing to do here any more. */

#ifndef _RELEASE
/* poison.h #pragma-poisons malloc/fopen/... so they cannot be used unqualified. It
 * MUST come after every system header that legitimately declares them -- glib/gstdio.h
 * is included here for exactly that reason, not for the benefit of consumers. */
#include <glib/gstdio.h>
#include "common/poison.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif


// version of current performance configuration version
// if you want to run an updated version of the performance configuration later
// bump this number and make sure you have an updated logic in dt_configure_performance()
#define DT_CURRENT_PERFORMANCE_CONFIGURE_VERSION 11
#define DT_PERF_INFOSIZE 4096


/* dt_session_id() / dt_install_id() moved to common/anonymous_ids.h */

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
  struct dt_sys_resources_t dtresources;

  // Working message displayed over the main preview when working
  char *main_message;
} darktable_t;


extern darktable_t darktable;

int dt_init(int argc, char *argv[], const gboolean init_gui, const gboolean load_data);
void dt_cleanup();


/* Memory budgets and dt_worker_threads() moved to common/sys_resources.h */


/* dt_capabilities_* moved to common/capabilities.h */


/* dt_supported_image() moved to common/image_extensions.h */


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
