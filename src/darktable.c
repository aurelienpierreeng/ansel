/*
    This file is part of darktable,
    Copyright (C) 2009-2014 johannes hanika.
    Copyright (C) 2010-2011, 2015 Bruce Guenter.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010 Richard Hughes.
    Copyright (C) 2010-2020 Tobias Ellinghaus.
    Copyright (C) 2011 Alexey Dokuchaev.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 calca.
    Copyright (C) 2011-2012 Christian Tellefsen.
    Copyright (C) 2011 David Bremner.
    Copyright (C) 2011-2012 Edouard Gomez.
    Copyright (C) 2011 Kanstantsin Shautsou.
    Copyright (C) 2011-2015 Pascal de Bruijn.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2012, 2016-2018 Ulrich Pegelow.
    Copyright (C) 2012 James C. McPherson.
    Copyright (C) 2012 Jeroen Hegeman.
    Copyright (C) 2012-2015 Jérémy Rosen.
    Copyright (C) 2012-2013, 2015, 2018-2019 parafin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013, 2021 Aldric Renaudin.
    Copyright (C) 2013 Jens Fendler.
    Copyright (C) 2013, 2015, 2017, 2019-2022 Pascal Obry.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 sthen.
    Copyright (C) 2013 Stuart Henderson.
    Copyright (C) 2014 Fernando R.
    Copyright (C) 2014 Moritz Lipp.
    Copyright (C) 2014-2015 Pedro Côrte-Real.
    Copyright (C) 2014-2017, 2020 Roman Lebedev.
    Copyright (C) 2015, 2017, 2019 Dan Torop.
    Copyright (C) 2015 Jean-Sébastien Pédron.
    Copyright (C) 2015 K. Adam Christensen.
    Copyright (C) 2015 Matthias Gehre.
    Copyright (C) 2016-2018 Peter Budai.
    Copyright (C) 2017, 2021 luzpaz.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Alexis Mousset.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 Felipe Contreras.
    Copyright (C) 2019-2022 Hanno Schwalm.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020 David-Tillmann Schaefer.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 Matthieu Volat.
    Copyright (C) 2020-2022 Philippe Weyland.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Ilya Kurdyukov.
    Copyright (C) 2021 Paolo DePetrillo.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2023 lologor.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2023 Maurizio Paglia.
    Copyright (C) 2024 Alynx Zhou.
    Copyright (C) 2025-2026 Guillaume Stutin.
    Copyright (C) 2025 Miguel Moquillon.
    
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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif


#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__DragonFly__)
#include <malloc.h>
#endif
#ifdef __APPLE__
#include <sys/malloc.h>
#endif

/* Platform memory-query APIs used by dt_get_total_memory()/dt_get_system_available_mem()
 * below: sysctl(CTL_HW, ...) on Apple/BSD and the mach host/task statistics on Apple.
 * These used to arrive through darktable.h's OS block; this TU is their only
 * non-external consumer in the tree (common/telemetry.c carries its own), so it declares
 * them itself. */
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif
#if defined(__DragonFly__) || defined(__FreeBSD__)
#include <sys/sysctl.h>
#include <sys/types.h>
#endif
#if defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#endif

#include "common/collection.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "gui/common/database_gui.h"
#include "colorprofiles/colorspaces.h"
#include "metadata/colorlabels.h"
#include "darktable.h"
#include "common/anonymous_ids.h"
#include "system/capabilities.h"
#include "common/global_mutexes.h"
#include "system/sys_resources.h"
#include "common/datetime.h"
#include "metadata/exif.h"
#include "history/history.h"
#include "database/history_repository.h"
#include "common/selection.h"
#include "gui/privacy_consent.h"
#include "common/sentry.h"
#include "common/telemetry.h"
#include "common/system_signal_handling.h"
#include "widgets/bauhaus.h"
#include "gui/presets.h"
#include "gui/splash.h"

#include "common/file_location.h"
#include "common/film.h"
#include "common/folder_survey.h"
#include "gui/common/folder_survey_gui.h"
#include "common/grealpath.h"
#include "common/image.h"
#include "caches/image_cache.h"
#include "database/database.h"
#include "common/image_extensions.h"
#include "imageio/imageio_module.h"
#include "develop/iop_order.h"
#include "common/l10n.h"
#include "metadata/metadata.h"
#include "common/image_notify.h"
#include "develop/dev_history_gui.h"
#include "gui/import.h"
#include "develop/pipeline_notify.h"
#include "history/notify.h"
#include "history/presets.h"
#include "metadata/notify.h"
#include "caches/mipmap_cache.h"
#include "common/noiseprofiles.h"
#include "common/opencl.h"
#include "common/points.h"
#include "system/resource_limits.h"
#include "metadata/tags.h"
#include "common/styles.h"
#include "common/undo.h"
#include "system/fp_mode.h"
#include "common/conf.h"
#include "control/control.h"
#include "control/crawler.h"
#include "control/jobs/film_jobs.h"
#include "control/signal.h"
#include "develop/dev_pixelpipe.h"
#include "develop/imageop.h"
#include "develop/supervisor.h"

#include "gui/application.h"
#include "develop/gui_throttle.h"
#include "gui/guides.h"
#include "gui/presets.h"
#include "libs/lib.h"
#include "views/view.h"
#include "conf_gen.h"

#include <errno.h>
#if !defined(_WIN32) && !defined(__APPLE__)
#include <fontconfig/fontconfig.h>
#endif
#include <glib.h>
#include <glib/gstdio.h>
#include <pango/pangocairo.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/types.h>
#include <unistd.h>
#include <locale.h>
#include <limits.h>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif


#include "common/dbus.h"
#include "common/utility.h"

#if defined(__SUNOS__)
#include <sys/varargs.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

darktable_t darktable;

#if !defined(_WIN32) && !defined(__APPLE__)
typedef struct _PangoFcFontMap PangoFcFontMap;
extern GType pango_fc_font_map_get_type(void);
extern void pango_fc_font_map_shutdown(PangoFcFontMap *fcfontmap);
#endif

/**
 * GLib 2.82 routes GTK/GDK diagnostics through the structured log writer, so
 * filtering them with g_log_set_handler() is not sufficient here. We drop only
 * the known harmless startup messages and forward every other record to the
 * default writer unchanged.
 */
static GLogWriterOutput _gtk_log_writer_filter(GLogLevelFlags log_level, const GLogField *fields,
                                               gsize n_fields, gpointer user_data)
{
  const gchar *message = NULL;

  for(gsize k = 0; k < n_fields; k++)
  {
    if(g_strcmp0(fields[k].key, "MESSAGE")) continue;

    message = fields[k].value;
    break;
  }

  // Silence only warnings/errors that come from default Adwaita CSS or desktop theme
  // because there is nothing we can do about those.
  // Yes, default Adwaita GTK CSS is still using deprecated GTK stuff in 2026...
  // Even those morons can't keep up with the pace of their own deprecations.
  if(message)
  {
    if(!g_strcmp0(message, "Unable to load dot from the cursor theme"))
      return G_LOG_WRITER_HANDLED;

    if(g_str_has_prefix(message, "Theme parsing error:")
       && g_str_has_suffix(message, "The :insensitive pseudo-class is deprecated. Use :disabled instead."))
      return G_LOG_WRITER_HANDLED;

    if(g_str_has_prefix(message, "Theme parsing error:")
       && g_str_has_suffix(message, "The :inconsistent pseudo-class is deprecated. Use :indeterminate instead."))
      return G_LOG_WRITER_HANDLED;
  }

  return g_log_writer_default(log_level, fields, n_fields, user_data);
}

static int usage(const char *argv0)
{
#ifdef _WIN32
  char *logfile = g_build_filename(g_get_user_cache_dir(), "ansel", "ansel-log.txt", NULL);
#endif
  // clang-format off
  printf("usage: %s [options] [IMG_1234.{RAW,..}|image_folder/]\n", argv0);
  printf("\n");
  printf("options:\n");
  printf("\n");
  printf("  --cachedir <user cache directory>\n");
  printf("  --conf <key>=<value>\n");
  printf("  --configdir <user config directory>\n");
  printf("  -d {all,cache,camctl,camsupport,colorprofile,control,demosaic,dev,gtk,history,imageio,import,\n");
  printf("      input,ioporder,lighttable,lua,masks,memory,nan,nocache_reuse,opencl,params,\n");
  printf("      perf,pipe,pipecache,print,signal,sql,shortcuts,tiling,undo,verbose}\n");
  printf("  --d-signal <signal> \n");
  printf("  --d-signal-act <all,raise,connect,disconnect");
  // clang-format on
#ifdef DT_HAVE_SIGNAL_TRACE
  printf(",print-trace");
#endif
  printf(">\n");
  printf("  --datadir <data directory>\n");
#ifdef HAVE_OPENCL
  printf("  --disable-opencl\n");
#endif
  printf("  -h, --help");
#ifdef _WIN32
  printf(", /?");
#endif
  printf("\n");
  printf("  --library <library file>\n");
  printf("  --localedir <locale directory>\n");
  printf("  --moduledir <module directory>\n");
  printf("  --noiseprofiles <noiseprofiles json file>\n");
  printf("  -t <num openmp threads>\n");
  printf("  --tmpdir <tmp directory>\n");
  printf("  --version\n");
#ifdef _WIN32
  printf("\n");
  printf("  note: debug log and output will be written to this file:\n");
  printf("        %s\n", logfile);
#endif

#ifdef _WIN32
  dt_free(logfile);
#endif

  return 1;
}

char *dt_version_major_minor()
{
  char ver[100] = { 0 };
  g_strlcpy(ver, darktable_package_string, sizeof(ver));
  int count = -1;
  char *start = ver;
  for(char *p = ver; *p; p++)
  {
    // first look for a number
    if(count == -1)
    {
      if(*p >= '0' && *p <= '9')
      {
        count++;
        start = p;
      }
    }
    // then check for <major>.<minor>
    else
    {
      if(*p == '.' || *p == '+') count++;
      if(count == 2)
      {
        *p = '\0';
        break;
      }
    }
  }
  return g_strdup(start);
}

const char *dt_session_id(void)
{
  // Random per-run UUID, generated once and shared by crash reporting and usage
  // analytics so a single session can be correlated across Sentry and PostHog.
  static gchar *id = NULL;
  static gsize init = 0;
  if(g_once_init_enter(&init))
  {
    id = g_uuid_string_random();
    g_once_init_leave(&init, 1);
  }
  return id;
}

const char *dt_install_id(void)
{
  // Anonymous, stable per-installation UUID, persisted in conf and shared by crash
  // reporting (Sentry user id) and usage analytics (PostHog distinct_id) so the
  // same user can be de-duplicated across both systems. Created lazily on first use.
  static gchar *id = NULL;
  static gsize init = 0;
  if(g_once_init_enter(&init))
  {
    gchar *stored = dt_conf_get_string("telemetry/install_id");
    if(!stored || !*stored)
    {
      g_free(stored);
      stored = g_uuid_string_random();
      dt_conf_set_string("telemetry/install_id", stored);
    }
    id = stored;
    g_once_init_leave(&init, 1);
  }
  return id;
}

gboolean dt_supported_image(const gchar *filename)
{
  char *ext = g_strrstr(filename, ".");
  if(IS_NULL_PTR(ext))
    return FALSE;
  return dt_image_ext_is_supported(ext + 1);
}

int dt_load_from_string(const gchar *input, gboolean open_image_in_dr, gboolean *single_image)
{
  int32_t id = 0;
  if(IS_NULL_PTR(input) || input[0] == '\0') return 0;

  char *filename = dt_util_normalize_path(input);

  if(IS_NULL_PTR(filename))
  {
    dt_control_log(_("found strange path `%s'"), input);
    return 0;
  }

  if(g_file_test(filename, G_FILE_TEST_IS_DIR))
  {
    // import a directory into a film roll
    id = dt_film_import(filename);
    if(id)
    {
      dt_film_open(id);
      dt_ctl_switch_mode_to("lighttable");
    }
    else
    {
      dt_control_log(_("error loading directory `%s'"), filename);
    }
    if(single_image) *single_image = FALSE;
  }
  else
  {
    // import a single image
    gchar *directory = g_path_get_dirname((const gchar *)filename);
    dt_film_t film;
    const int filmid = dt_film_new(&film, directory);
    id = dt_image_import(filmid, filename, TRUE);
    dt_free(directory);
    if(id)
    {
      dt_film_open(filmid);
      // make sure buffers are loaded (load full for testing)
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_get(&buf, id, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING, 'r');
      gboolean loaded = (!IS_NULL_PTR(buf.buf));
      dt_mipmap_cache_release(&buf);
      if(!loaded)
      {
        id = 0;
        dt_control_log(_("file `%s' has unknown format!"), filename);
      }
      else
      {
        if(open_image_in_dr)
        {
          dt_control_set_mouse_over_id(id);
          dt_ctl_switch_mode_to("darkroom");
        }
      }
    }
    else
    {
      dt_control_log(_("error loading file `%s'"), filename);
    }
    if(single_image) *single_image = TRUE;
  }
  dt_free(filename);
  return id;
}

// Returns total system memory in kiloBytes
static inline size_t _get_total_memory()
{
#if defined(__linux__)
  FILE *f = g_fopen("/proc/meminfo", "rb");
  if(IS_NULL_PTR(f)) return 0;
  size_t mem = 0;
  char *line = NULL;
  size_t len = 0;
  int first = 1, found = 0;
  // return "MemTotal" or the value from the first line
  while(!found && getline(&line, &len, f) != -1)
  {
    char *colon = strchr(line, ':');
    if(IS_NULL_PTR(colon)) continue;
    found = !strncmp(line, "MemTotal:", 9);
    if(found || first) mem = atol(colon + 1);
    first = 0;
  }
  fclose(f);
  if(len > 0)
  {
    dt_free(line);
  }
  return mem;
#elif defined(__APPLE__) || defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__)            \
    || defined(__OpenBSD__)
#if defined(__APPLE__)
  int mib[2] = { CTL_HW, HW_MEMSIZE };
#elif defined(HW_PHYSMEM64)
  int mib[2] = { CTL_HW, HW_PHYSMEM64 };
#else
  int mib[2] = { CTL_HW, HW_PHYSMEM };
#endif
  uint64_t physical_memory;
  size_t length = sizeof(uint64_t);
  sysctl(mib, 2, (void *)&physical_memory, &length, (void *)NULL, 0);
  return physical_memory / 1024;
#elif defined _WIN32
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&memInfo);
  return memInfo.ullTotalPhys / (uint64_t)1024;
#else
  // assume 2GB until we have a better solution.
  fprintf(stderr, "Unknown memory size. Assuming 2GB\n");
  return 2097152;
#endif
}

void *dt_alloc_align(size_t size)
{
  return dt_alloc_align_internal(size);
}

/* Singleton accessors: the orchestrator BINDS the application-wide instances to the
 * lower-level libs that declare these symbols (caches/pixelpipe_cache.h and
 * common/openmp.h). This keeps those libs free of darktable.h — they link
 * against two functions instead of importing the whole application struct. */

int dt_get_num_openmp_threads(void)
{
  return darktable.num_openmp_threads;
}

struct dt_gui_gtk_t *dt_gui_get_global(void)
{
  return darktable.gui;
}

struct dt_develop_t *dt_dev_get_global(void)
{
  return darktable.develop;
}

GList *dt_iop_get_modules_so(void)
{
  return darktable.iop;
}

GList *dt_ioppr_get_iop_order_rules_global(void)
{
  return darktable.iop_order_rules;
}

void dt_dev_set_global(struct dt_develop_t *dev)
{
  darktable.develop = dev;
}

GList *dt_guides_get_list(void)
{
  return darktable.guides;
}

GList **dt_guides_get_list_ref(void)
{
  return &darktable.guides;
}

GList *dt_gui_get_themes(void)
{
  return darktable.themes;
}

void dt_gui_set_themes(GList *themes)
{
  g_list_free_full(darktable.themes, dt_free_gpointer);
  darktable.themes = themes;
}

/* The string is written by pipeline worker threads and read by the GUI thread painting the
 * banner, so it carries its own lock rather than borrowing control->log_mutex: the borrowed
 * one was taken by the single writer at its CALL SITE, which left the invariant unenforceable
 * -- and left the reader, dt_control_draw_busy_msg(), holding no lock at all while a worker
 * dt_free()d the very pointer it had handed to pango. That fired once per module per frame
 * whenever the darkroom was rendering.
 *
 * A private lock also keeps this independent of dt_control_t's lifetime, which is freed at
 * darktable.c's teardown while this string is not. */
/* Statically initialised on purpose, and correct in the _DEBUG build too, where
 * dt_pthread_mutex_t carries ~1.8 kB of instrumentation after the pthread_mutex_t. A
 * brace-enclosed initialiser with fewer initialisers than members zero-fills the remainder
 * (C11 6.7.9p21), and this object has static storage duration anyway, so it lives in .bss --
 * measured: 0 of the 1864 trailing bytes non-zero before first use. That is the same state
 * dt_pthread_mutex_init() leaves (it memsets), minus only the `name' field, which
 * dt_pthread_mutex_lock() snprintf()s over before the one place it reads it.
 *
 * Static rather than initialised in dt_init() because this string is written from worker
 * threads: a lock that is valid from program start has no window in which it is not. */
static dt_pthread_mutex_t _main_message_lock = { PTHREAD_MUTEX_INITIALIZER };

char *dt_get_main_message_copy(void)
{
  dt_pthread_mutex_lock(&_main_message_lock);
  char *const copy = !IS_NULL_PTR(darktable.main_message) ? g_strdup(darktable.main_message) : NULL;
  dt_pthread_mutex_unlock(&_main_message_lock);
  return copy;
}

void dt_set_main_message(char *message)
{
  dt_pthread_mutex_lock(&_main_message_lock);
  dt_free(darktable.main_message);
  darktable.main_message = message;
  dt_pthread_mutex_unlock(&_main_message_lock);
}

struct dt_view_manager_t *dt_view_manager_get_global(void)
{
  return darktable.view_manager;
}

dt_pthread_mutex_t *dt_plugin_threadsafe_mutex(void)
{
  return &darktable.plugin_threadsafe;
}

dt_pthread_mutex_t *dt_pipeline_threadsafe_mutex(void)
{
  return &darktable.pipeline_threadsafe;
}

dt_pthread_mutex_t *dt_exiv2_threadsafe_mutex(void)
{
  return &darktable.exiv2_threadsafe;
}

dt_pthread_mutex_t *dt_readfile_mutex(void)
{
  return &darktable.readFile_mutex;
}



struct dt_selection_t *dt_selection_get_global(void)
{
  return darktable.selection;
}

struct dt_undo_t *dt_undo_get_global(void)
{
  return darktable.undo;
}

struct dt_collection_t *dt_collection_get_global(void)
{
  return darktable.collection;
}

struct dt_control_signal_t *dt_control_signal_get_global(void)
{
  return darktable.signals;
}

struct dt_lib_t *dt_lib_get_global(void)
{
  return darktable.lib;
}

struct dt_imageio_t *dt_imageio_get_global(void)
{
  return darktable.imageio;
}

struct dt_points_t *dt_points_get_global(void)
{
  return darktable.points;
}

struct dt_l10n_t *dt_l10n_get_global(void)
{
  return darktable.l10n;
}

struct dt_dbus_t *dt_dbus_get_global(void)
{
  return darktable.dbus;
}

JsonParser *dt_noiseprofile_get_parser_global(void)
{
  return darktable.noiseprofile_parser;
}

struct dt_bauhaus_t *dt_bauhaus_get_global(void)
{
  return darktable.bauhaus;
}

struct dt_control_t *dt_control_get_global(void)
{
  return darktable.control;
}


/* --- pixelpipe cache handlers ------------------------------------------------
 * See dt_dev_pixelpipe_cache_set_handlers(). These are the application's answers to the
 * cache's three announcements; the cache itself names none of these subsystems. */
static void _pixelpipe_cache_warn(const char *message)
{
  dt_control_log("%s", message);
}

static void _pixelpipe_cache_ready(uint64_t hash, uint64_t producer_node_key)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CACHELINE_READY, hash,
                                producer_node_key);
}

static const dt_pixelpipe_cache_observer_t _pixelpipe_cache_observer = {
  .active = dt_supervisor_active,
  .cacheline_read = dt_supervisor_cacheline_read,
  .cacheline_delete = dt_supervisor_cacheline_delete,
  .rekey = dt_supervisor_rekey,
};


/* The mipmap cache's user-facing settings live in conf; the cache does not read conf. The
 * application owns that translation, here and in the preference-change handler below, which is
 * what makes the lifecycle of these four visible: read at startup, re-read when the user
 * changes one, never anywhere else. */
static dt_mipmap_cache_settings_t _mipmap_settings_from_conf(void)
{
  dt_mipmap_cache_settings_t s = { 0 };
  s.max_memory = darktable.dtresources.mipmap_memory;
  s.disk_backend = dt_conf_get_bool("cache_disk_backend");
  s.embedded_jpg = dt_conf_get_int("lighttable/embedded_jpg");
  s.cache_quality = dt_conf_get_int("database_cache_quality");
  return s;
}

/* Same arrangement for the database's maintenance and snapshot policy. These were read
 * with dt_conf_* from five places inside database.c, several of them deep in a decision
 * the user never sees. */
static void _database_settings_from_conf(void)
{
  dt_database_settings_t s = { 0 };
  s.maintenance_check = dt_conf_get_string("database/maintenance_check");
  s.maintenance_freepage_ratio = dt_conf_get_int("database/maintenance_freepage_ratio");
  s.create_snapshot = dt_conf_get_string("database/create_snapshot");
  s.keep_snapshots = dt_conf_get_int("database/keep_snapshots");
  dt_database_set_settings(&s);
  dt_free(s.maintenance_check);
  dt_free(s.create_snapshot);
}

/* The database moved the legacy pre-XDG library file and settled on a new name for it.
 * Persisting that is ours: the module does not write conf. */
static void _database_renamed(const char *new_library_name)
{
  dt_conf_set_string("database", new_library_name);
}

static void _metadata_tags_changed(void)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_TAG_CHANGED);
}

/* src/metadata states what it did; where that appears is ours to decide. Installed
 * unconditionally: dt_control_log()/dt_toast_log() are themselves no-ops without a GUI, so
 * this keeps the headless behaviour the direct calls had. */
static void _metadata_notify(const dt_metadata_notice_t kind, const char *message)
{
  if(kind == DT_METADATA_NOTICE_TOAST)
    dt_toast_log("%s", message);
  else
    dt_control_log("%s", message);
}

/* The pipeline worker names what it is chewing on; the banner and its repaint are ours.
 * Called from worker threads, as the calls it replaces were -- dt_set_main_message()
 * under log_mutex and the centre redraw are both worker-safe. */
static void _pipeline_busy(const char *message_or_null)
{
  // dt_set_main_message() takes the string's own lock. Do NOT reintroduce an outer
  // control->log_mutex here: it guards the message LOG, not this, and it bought nothing.
  dt_set_main_message(message_or_null ? g_strdup(message_or_null) : NULL);
  dt_control_queue_redraw_center();
}

static void _pipeline_message(const char *message)
{
  dt_control_log("%s", message);
}

/* The geotag list is copied here, at the raise site, exactly where the old call sites
 * copied it: the signal takes ownership of what it is given. */
static void _metadata_geotags_changed(const GList *imgs)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_GEOTAG_CHANGED,
                                g_list_copy((GList *)imgs), 0);
}

static void _image_imported(const int32_t imgid)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_IMAGE_IMPORT, imgid);
}

/* History, styles and presets state what happened; turning that into a signal is ours. */
static void _history_notify(const char *message)
{
  dt_control_log("%s", message);
}

static void _history_toast(const char *message)
{
  dt_toast_log("%s", message);
}

static void _history_changed(const dt_history_change_t what)
{
  switch(what)
  {
    case DT_HISTORY_CHANGE_TAGS:
      DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_TAG_CHANGED);
      break;
    case DT_HISTORY_CHANGE_STYLES:
      DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_STYLE_CHANGED);
      break;
    case DT_HISTORY_CHANGE_DEVELOP:
      DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
      break;
  }
}

/* The copy is made here, at the raise site, exactly where the call site used to make it:
 * the signal takes ownership of the list it is given, and the caller keeps its own. */
static void _history_images_changed(const GList *imgs)
{
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_IMAGE_INFO_CHANGED,
                                g_list_copy((GList *)imgs));
}

/* Only the side that loaded the modules knows what an operation is called. */
static const char *_history_operation_name(const char *operation)
{
  return dt_iop_get_localized_name(operation);
}

/* Only the side of the application that owns the panels can answer this. */
static gboolean _presets_can_autoapply(const gchar *operation)
{
  for(const GList *lib_modules = dt_lib_get_global()->plugins; lib_modules;
      lib_modules = g_list_next(lib_modules))
  {
    dt_lib_module_t *lib_module = (dt_lib_module_t *)lib_modules->data;
    if(!strcmp(lib_module->plugin_name, operation)) return dt_lib_presets_can_autoapply(lib_module);
  }
  return TRUE;
}

/* The two parameters are the GTK signal signature, not ours; neither carries anything we
 * need, since the settings are read from conf either way. */
static void _preferences_changed(gpointer instance, gpointer user_data)
{
  (void)instance;
  (void)user_data;

  const dt_mipmap_cache_settings_t s = _mipmap_settings_from_conf();
  dt_mipmap_cache_set_settings(&s);
  _database_settings_from_conf();
}

/* Same "read at startup, re-read on change, never anywhere else" lifecycle as the mipmap cache
 * settings above, for the "write_sidecar_files" preference: dt_image_get_xmp_mode() is on the
 * per-image hot path and reads a cache instead of conf directly. */
static void _xmp_mode_preferences_changed(gpointer instance, gpointer user_data)
{
  (void)instance;
  (void)user_data;

  dt_image_xmp_mode_refresh_from_conf();
}

int dt_init(int argc, char *argv[], const gboolean init_gui, const gboolean load_data)
{
  double start_wtime = dt_get_wtime();

#ifndef _WIN32
  if(getuid() == 0 || geteuid() == 0)
    printf(
        "WARNING: either your user id or the effective user id are 0. are you running darktable as root?\n");
#endif

  dt_fp_init(DT_FP_MODE_FAST);

  dt_set_signal_handlers();

#ifdef M_MMAP_THRESHOLD
  mallopt(M_MMAP_THRESHOLD, 128 * 1024); /* use mmap() for large allocations */
#endif

  // make sure that stack/frame limits are good (musl)
  dt_set_rlimits();

  // init all pointers to 0:
  memset(&darktable, 0, sizeof(darktable_t));

  darktable.start_wtime = start_wtime;

  darktable.progname = argv[0];

  darktable.main_message = NULL;

  // FIXME: move there into dt_database_t
  pthread_mutexattr_t recursive_locking;
  pthread_mutexattr_init(&recursive_locking);
  pthread_mutexattr_settype(&recursive_locking, PTHREAD_MUTEX_RECURSIVE);
  dt_pthread_mutex_init(&(darktable.plugin_threadsafe), NULL);
  dt_pthread_mutex_init(&(darktable.capabilities_threadsafe), NULL);
  // exiv2 and the Adobe XMP toolkit keep process-global state and are not thread-safe; this mutex
  // serializes all exiv2 access (src/common/exif.cc). Make it recursive so a public exif function can
  // hold it across its whole critical section while inner helpers (read_metadata_threadsafe) or
  // re-entrant calls (e.g. variable expansion that reads metadata) re-lock it without deadlocking.
  dt_pthread_mutex_init(&(darktable.exiv2_threadsafe), &recursive_locking);
  dt_pthread_mutex_init(&(darktable.readFile_mutex), NULL);
  dt_pthread_mutex_init(&(darktable.pipeline_threadsafe), NULL);

  darktable.control = (dt_control_t *)calloc(1, sizeof(dt_control_t));

  // database
  char *dbfilename_from_command = NULL;
  char *noiseprofiles_from_command = NULL;
  char *datadir_from_command = NULL;
  char *moduledir_from_command = NULL;
  char *localedir_from_command = NULL;
  char *tmpdir_from_command = NULL;
  char *configdir_from_command = NULL;
  char *cachedir_from_command = NULL;
  char *kerneldir_from_command = NULL;

#ifdef HAVE_OPENCL
  gboolean exclude_opencl = FALSE;
  gboolean print_statistics = (strstr(argv[0], "ansel-cltest") == NULL);
#endif

  darktable.num_openmp_threads = 1;
#ifdef _OPENMP
  darktable.num_openmp_threads = omp_get_max_threads();
#endif

  darktable.unmuted = 0;
  gboolean cpu_threads_from_cli = FALSE;

  GSList *config_override = NULL;
  for(int k = 1; k < argc; k++)
  {
#ifdef _WIN32
    if(!strcmp(argv[k], "/?"))
    {
      return usage(argv[0]);
    }
#endif
    if(argv[k][0] == '-')
    {
      if(!strcmp(argv[k], "--help") || !strcmp(argv[k], "-h"))
      {
        return usage(argv[0]);
      }
      else if(!strcmp(argv[k], "--version"))
      {
        printf("this is %s\ncopyright (c) 2009-2022 Johannes Hanika, (c) 2022-%s Aurélien Pierre\n" PACKAGE_BUGREPORT "\n\ncompile options:\n"
               "  bit depth is %" G_GSIZE_FORMAT " bit\n"
#ifdef _DEBUG
               "  debug build\n"
#else
               "  normal build\n"
#endif
#ifdef _OPENMP
               "  OpenMP support enabled\n"
#else
               "  OpenMP support disabled\n"
#endif

#ifdef HAVE_OPENCL
               "  OpenCL support enabled\n"
#else
               "  OpenCL support disabled\n"
#endif

#ifdef USE_COLORDGTK
               "  Colord support enabled\n"
#else
               "  Colord support disabled\n"
#endif

#ifdef HAVE_OPENEXR
               "  OpenEXR support enabled\n"
#else
               "  OpenEXR support disabled\n"
#endif
               ,
               darktable_package_string,
               darktable_last_commit_year,
               CHAR_BIT * sizeof(void *)
               );
        return 1;
      }
      else if(!strcmp(argv[k], "--library") && argc > k + 1)
      {
        dbfilename_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--datadir") && argc > k + 1)
      {
        datadir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--moduledir") && argc > k + 1)
      {
        moduledir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--tmpdir") && argc > k + 1)
      {
        tmpdir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--configdir") && argc > k + 1)
      {
        configdir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--cachedir") && argc > k + 1)
      {
        cachedir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--localedir") && argc > k + 1)
      {
        localedir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--kerneldir") && argc > k + 1)
      {
        kerneldir_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(argv[k][1] == 'd' && argc > k + 1)
      {
        if(!strcmp(argv[k + 1], "all"))
          darktable.unmuted = 0xffffffff & ~DT_DEBUG_VERBOSE; // enable all debug information except verbose
        else if(!strcmp(argv[k + 1], "cache"))
          darktable.unmuted |= DT_DEBUG_CACHE; // enable debugging for lib/film/cache module
        else if(!strcmp(argv[k + 1], "control"))
          darktable.unmuted |= DT_DEBUG_CONTROL; // enable debugging for scheduler module
        else if(!strcmp(argv[k + 1], "dev"))
          darktable.unmuted |= DT_DEBUG_DEV; // develop module
        else if(!strcmp(argv[k + 1], "gtk"))
          darktable.unmuted |= DT_DEBUG_GTK; // GTK widgets and display setup
        else if(!strcmp(argv[k + 1], "input"))
          darktable.unmuted |= DT_DEBUG_INPUT; // input devices
        else if(!strcmp(argv[k + 1], "pipecache"))
          darktable.unmuted |= DT_DEBUG_PIPECACHE; // pipeline cache
        else if(!strcmp(argv[k + 1], "perf"))
          darktable.unmuted |= DT_DEBUG_PERF; // performance measurements
        else if(!strcmp(argv[k + 1], "opencl"))
          darktable.unmuted |= DT_DEBUG_OPENCL; // gpu accel via opencl
        else if(!strcmp(argv[k + 1], "sql"))
          darktable.unmuted |= DT_DEBUG_SQL; // SQLite3 queries
        else if(!strcmp(argv[k + 1], "memory"))
          darktable.unmuted |= DT_DEBUG_MEMORY; // some stats on mem usage now and then.
        else if(!strcmp(argv[k + 1], "lighttable"))
          darktable.unmuted |= DT_DEBUG_LIGHTTABLE; // lighttable related stuff.
        else if(!strcmp(argv[k + 1], "nan"))
          darktable.unmuted |= DT_DEBUG_NAN; // check for NANs when processing the pipe.
        else if(!strcmp(argv[k + 1], "masks"))
          darktable.unmuted |= DT_DEBUG_MASKS; // masks related stuff.
        else if(!strcmp(argv[k + 1], "lua"))
          darktable.unmuted |= DT_DEBUG_LUA; // lua errors are reported on console
        else if(!strcmp(argv[k + 1], "print"))
          darktable.unmuted |= DT_DEBUG_PRINT; // print errors are reported on console
        else if(!strcmp(argv[k + 1], "camsupport"))
          darktable.unmuted |= DT_DEBUG_CAMERA_SUPPORT; // camera support warnings are reported on console
        else if(!strcmp(argv[k + 1], "colorprofile"))
          darktable.unmuted |= DT_DEBUG_COLORPROFILE; // color profile handling
        else if(!strcmp(argv[k + 1], "nocache_reuse"))
          darktable.unmuted |= DT_DEBUG_NOCACHE_REUSE; // disable reusable pixelpipe cache buffers
        else if(!strcmp(argv[k + 1], "ioporder"))
          darktable.unmuted |= DT_DEBUG_IOPORDER; // iop order information are reported on console
        else if(!strcmp(argv[k + 1], "imageio"))
          darktable.unmuted |= DT_DEBUG_IMAGEIO; // image importing or exporting messages on console
        else if(!strcmp(argv[k + 1], "undo"))
          darktable.unmuted |= DT_DEBUG_UNDO; // undo/redo
        else if(!strcmp(argv[k + 1], "signal"))
          darktable.unmuted |= DT_DEBUG_SIGNAL; // signal information on console
        else if(!strcmp(argv[k + 1], "params"))
          darktable.unmuted |= DT_DEBUG_PARAMS; // iop module params checks on console
        else if(!strcmp(argv[k + 1], "demosaic"))
          darktable.unmuted |= DT_DEBUG_DEMOSAIC;
        else if(!strcmp(argv[k + 1], "shortcuts"))
          darktable.unmuted |= DT_DEBUG_SHORTCUTS;
        else if(!strcmp(argv[k + 1], "tiling"))
          darktable.unmuted |= DT_DEBUG_TILING;
        else if(!strcmp(argv[k + 1], "verbose"))
          darktable.unmuted |= DT_DEBUG_VERBOSE;
        else if(!strcmp(argv[k + 1], "pipe"))
          darktable.unmuted |= DT_DEBUG_PIPE;
        else if(!strcmp(argv[k + 1], "history"))
          darktable.unmuted |= DT_DEBUG_HISTORY;
        else if(!strcmp(argv[k + 1], "import"))
          darktable.unmuted |= DT_DEBUG_IMPORT;
        else if(!strcmp(argv[k + 1], "supervisor"))
          darktable.unmuted |= DT_DEBUG_SUPERVISOR; // high-level event supervisor (NDJSON)
        else
          return usage(argv[0]);
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--d-signal-act") && argc > k + 1)
      {
        if(!strcmp(argv[k + 1], "all"))
          darktable.unmuted_signal_dbg_acts = 0xffffffff; // enable all signal debug information
        else if(!strcmp(argv[k + 1], "raise"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_RAISE; // enable debugging for signal raising
        else if(!strcmp(argv[k + 1], "connect"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_CONNECT; // enable debugging for signal connection
        else if(!strcmp(argv[k + 1], "disconnect"))
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_DISCONNECT; // enable debugging for signal disconnection
        else if(!strcmp(argv[k + 1], "print-trace"))
        {
#ifdef DT_HAVE_SIGNAL_TRACE
          darktable.unmuted_signal_dbg_acts |= DT_DEBUG_SIGNAL_ACT_PRINT_TRACE; // enable printing of signal tracing
#else
          fprintf(stderr, "[signal] print-trace not available, skipping\n");
#endif
        }
        else
          return usage(argv[0]);
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--d-signal") && argc > k + 1)
      {
        gchar *str = g_ascii_strup(argv[k+1], -1);

        #define CHKSIGDBG(sig) else if(!g_strcmp0(str, #sig)) do {darktable.unmuted_signal_dbg[sig] = TRUE;} while (0)
        if(!g_strcmp0(str, "ALL"))
        {
          for(int sig=0; sig<DT_SIGNAL_COUNT; sig++)
            darktable.unmuted_signal_dbg[sig] = TRUE;
        }
        CHKSIGDBG(DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE);
        CHKSIGDBG(DT_SIGNAL_ACTIVE_IMAGES_CHANGE);
        CHKSIGDBG(DT_SIGNAL_CONTROL_REDRAW_ALL);
        CHKSIGDBG(DT_SIGNAL_CONTROL_REDRAW_CENTER);
        CHKSIGDBG(DT_SIGNAL_VIEWMANAGER_VIEW_CHANGED);
        CHKSIGDBG(DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE);
        CHKSIGDBG(DT_SIGNAL_VIEWMANAGER_FILMSTRIP_ACTIVATE);
        CHKSIGDBG(DT_SIGNAL_VIEWMANAGER_FILMSTRIP_DRAG_BEGIN);
        CHKSIGDBG(DT_SIGNAL_COLLECTION_CHANGED);
        CHKSIGDBG(DT_SIGNAL_SELECTION_CHANGED);
        CHKSIGDBG(DT_SIGNAL_TAG_CHANGED);
        CHKSIGDBG(DT_SIGNAL_METADATA_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGE_INFO_CHANGED);
        CHKSIGDBG(DT_SIGNAL_STYLE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGES_ORDER_CHANGE);
        CHKSIGDBG(DT_SIGNAL_FILMROLLS_CHANGED);
        CHKSIGDBG(DT_SIGNAL_FILMROLLS_REMOVED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_INITIALIZE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_UI_PIPE_FINISHED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MODULEGROUPS_SET);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_HISTORY_WILL_CHANGE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_HISTORY_CHANGE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MODULE_REMOVE);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_MODULE_MOVED);
        CHKSIGDBG(DT_SIGNAL_DEVELOP_IMAGE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_DARKROOM_UI_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGE_LOADED);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PROFILE_CHANGED);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED);
        CHKSIGDBG(DT_SIGNAL_IMAGE_IMPORT);
        CHKSIGDBG(DT_SIGNAL_IMAGE_EXPORT_TMPFILE);
        CHKSIGDBG(DT_SIGNAL_IMAGEIO_STORAGE_CHANGE);
        CHKSIGDBG(DT_SIGNAL_PREFERENCES_CHANGE);
        CHKSIGDBG(DT_SIGNAL_CONTROL_NAVIGATION_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_LOG_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_TOAST_REDRAW);
        CHKSIGDBG(DT_SIGNAL_CONTROL_PICKERDATA_READY);
        CHKSIGDBG(DT_SIGNAL_METADATA_UPDATE);
        CHKSIGDBG(DT_SIGNAL_MASK_CHANGED);
        CHKSIGDBG(DT_SIGNAL_FOLDER_SURVEY_CHANGED);

        else
        {
          fprintf(stderr, "unknown signal name: '%s'. use 'ALL' to enable debug for all or use full signal name\n", str);
          return usage(argv[0]);
        }
        dt_free(str);
        #undef CHKSIGDBG
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(argv[k][1] == 't' && argc > k + 1)
      {
        darktable.num_openmp_threads = CLAMP(atol(argv[k + 1]), 1, 100);
        printf("[dt_init] using %d threads for openmp parallel sections\n", darktable.num_openmp_threads);
        k++;
        argv[k-1] = NULL;
        argv[k] = NULL;
        cpu_threads_from_cli = TRUE;
      }
      else if(!strcmp(argv[k], "--conf") && argc > k + 1)
      {
        gchar *keyval = g_strdup(argv[++k]), *c = keyval;
        argv[k-1] = NULL;
        argv[k] = NULL;
        gchar *end = keyval + strlen(keyval);
        while(*c != '=' && c < end) c++;
        if(*c == '=' && *(c + 1) != '\0')
        {
          *c++ = '\0';
          dt_conf_string_entry_t *entry = (dt_conf_string_entry_t *)g_malloc(sizeof(dt_conf_string_entry_t));
          entry->key = g_strdup(keyval);
          entry->value = g_strdup(c);
          config_override = g_slist_append(config_override, entry);
        }
        dt_free(keyval);
      }
      else if(!strcmp(argv[k], "--noiseprofiles") && argc > k + 1)
      {
        noiseprofiles_from_command = argv[++k];
        argv[k-1] = NULL;
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--disable-opencl"))
      {
#ifdef HAVE_OPENCL
        exclude_opencl = TRUE;
#endif
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--debug"))
      {
        argv[k] = NULL;
      }
      else if(!strcmp(argv[k], "--"))
      {
        // "--" confuses the argument parser of glib/gtk. remove it.
        argv[k] = NULL;
        break;
      }
#ifdef __APPLE__
      else if(!strncmp(argv[k], "-psn_", 5))
      {
        // "-psn_*" argument is added automatically by macOS and should be ignored
        argv[k] = NULL;
      }
#endif
      else
        return usage(argv[0]); // fail on unrecognized options
    }
  }

  // remove the NULLs to not confuse gtk_init() later.
  for(int i = 1; i < argc; i++)
  {
    int k;
    for(k = i; k < argc; k++)
      if(!IS_NULL_PTR(argv[k])) break;

    if(k > i)
    {
      k -= i;
      for(int j = i + k; j < argc; j++)
      {
        argv[j-k] = argv[j];
        argv[j] = NULL;
      }
      argc -= k;
    }
  }

  // get valid directories
  dt_loc_init(datadir_from_command, moduledir_from_command, localedir_from_command, configdir_from_command, cachedir_from_command, tmpdir_from_command, kerneldir_from_command);

  fprintf(stdout, "[build] version: %s\n", darktable_package_string);
  fprintf(stdout, "[build] type: %s | cpu mode: %s\n", DT_BUILD_TYPE, DT_BUILD_CPU_MODE);
  fprintf(stdout, "[build] c compiler: %s\n", DT_BUILD_C_COMPILER);
  fprintf(stdout, "[build] c flags: %s\n", DT_BUILD_C_FLAGS);
  fprintf(stdout, "[build] c++ compiler: %s\n", DT_BUILD_CXX_COMPILER);
  fprintf(stdout, "[build] c++ flags: %s\n", DT_BUILD_CXX_FLAGS);

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] at startup\n");
    dt_print_mem_usage();
  }

  char sharedir[DT_PATH_MAX] = { 0 };
  dt_loc_get_sharedir(sharedir, sizeof(sharedir));

  // we have to have our share dir in XDG_DATA_DIRS,
  // otherwise GTK+ won't find our logo for the about screen (and maybe other things)
  {
    const gchar *xdg_data_dirs = g_getenv("XDG_DATA_DIRS");
    gchar *new_xdg_data_dirs = NULL;
    gboolean set_env = TRUE;
    if(!IS_NULL_PTR(xdg_data_dirs) && *xdg_data_dirs != '\0')
    {
      // check if sharedir is already in there
      gboolean found = FALSE;
      gchar **tokens = g_strsplit(xdg_data_dirs, G_SEARCHPATH_SEPARATOR_S, 0);
      // xdg_data_dirs is neither NULL nor empty => !IS_NULL_PTR(tokens)
      for(char **iter = tokens; !IS_NULL_PTR(*iter); iter++)
        if(!strcmp(sharedir, *iter))
        {
          found = TRUE;
          break;
        }
      g_strfreev(tokens);
      if(found)
        set_env = FALSE;
      else
        new_xdg_data_dirs = g_strjoin(G_SEARCHPATH_SEPARATOR_S, sharedir, xdg_data_dirs, NULL);
    }
    else
    {
#ifndef _WIN32
      // see http://standards.freedesktop.org/basedir-spec/latest/ar01s03.html for a reason to use those as a
      // default
      if(!g_strcmp0(sharedir, "/usr/local/share")
         || !g_strcmp0(sharedir, "/usr/local/share/")
         || !g_strcmp0(sharedir, "/usr/share") || !g_strcmp0(sharedir, "/usr/share/"))
        new_xdg_data_dirs = g_strdup("/usr/local/share/" G_SEARCHPATH_SEPARATOR_S "/usr/share/");
      else
        new_xdg_data_dirs = g_strdup_printf("%s" G_SEARCHPATH_SEPARATOR_S "/usr/local/share/" G_SEARCHPATH_SEPARATOR_S
                                            "/usr/share/", sharedir);
#else
      set_env = FALSE;
#endif
    }

    if(set_env) g_setenv("XDG_DATA_DIRS", new_xdg_data_dirs, 1);
    dt_print(DT_DEBUG_DEV, "new_xdg_data_dirs: %s\n", new_xdg_data_dirs);
    dt_free(new_xdg_data_dirs);
  }

  setlocale(LC_ALL, "");
  char localedir[DT_PATH_MAX] = { 0 };
  dt_loc_get_localedir(localedir, sizeof(localedir));
  bindtextdomain(GETTEXT_PACKAGE, localedir);
  bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
  textdomain(GETTEXT_PACKAGE);

  if(init_gui)
  {
    // I doubt that connecting to dbus for ansel-cli makes sense
    darktable.dbus = NULL; //dt_dbus_init();

    // make sure that we have no stale global progress bar visible. thus it's run as early as possible
    dt_control_progress_init(darktable.control);
  }

  // thread-safe init:
  dt_exif_init();
  char datadir[DT_PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(datadir, sizeof(datadir));
  char anselrc[DT_PATH_MAX] = { 0 };
  dt_concat_path_file(anselrc, datadir, "anselrc");

  // initialize the config backend. this needs to be done first...
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, anselrc, config_override);
  g_slist_free_full(config_override, dt_free_gpointer);

  // set the interface language and prepare selection for prefs
  darktable.l10n = dt_l10n_init(init_gui);

  dt_confgen_init();
  // The throttle's persisted state and timeout preference are configuration, so they are
  // supplied here rather than read inside it.
  dt_gui_throttle_init(dt_conf_get_int("processing/gui_throttle_runtime_us"));
  dt_gui_throttle_set_timeout_ms((guint)MAX(dt_conf_get_int("processing/timeout"), 0));

  // Needs to run after dt_confgen_init()
  // Don't override cli argument if any
  if(!cpu_threads_from_cli)
  {
    const int user_threads = dt_conf_get_int("cpu_threads");
    if(user_threads > 0) darktable.num_openmp_threads = user_threads;
  }

#ifdef _OPENMP
  omp_set_num_threads(darktable.num_openmp_threads);
#endif

  // we need this REALLY early so that error messages can be shown, however after gtk_disable_setlocale
  if(init_gui)
  {
    g_log_set_writer_func(_gtk_log_writer_filter, NULL, NULL);
    gtk_init(&argc, &argv);

    darktable.themes = NULL;
  }

  // build the colour-profile module's own list; it owns it, we do not hold it
  dt_colorprofiles_init();

  // initialize datetime data
  dt_datetime_init();

  // initialize the database
  //
  // Every user preference the database acts on is read here and handed over, so that the
  // SQL layer never reads conf itself and "when does this take effect" has one answer.
  _database_settings_from_conf();
  dt_database_set_renamed_handler(_database_renamed);
  dt_metadata_set_notify_handler(_metadata_notify);
  dt_history_set_message_handler(_history_notify);
  dt_history_set_toast_handler(_history_toast);
  dt_presets_set_autoapply_resolver(_presets_can_autoapply);
  dt_history_set_operation_name_resolver(_history_operation_name);

  gchar *configured_library = dt_conf_get_string("database");
  const dt_database_params_t db_params = { .alternative = dbfilename_from_command,
                                           .library = configured_library,
                                           .load_data = load_data,
                                           .has_gui = init_gui,
                                           .verbose = (dt_get_debug_flags() & DT_DEBUG_SQL) != 0 };

  gboolean recheck_needed = TRUE;
  while (recheck_needed)
  {
    // Before, not after: dt_database_open() can hit a read-only or corrupt database and needs
    // to ask the user what to do about it, and dt_gui_gtk_init() -- where every other backend
    // handler is registered -- only runs much further down. This is the only place that knows
    // this early whether there will be anybody to ask.
    if(init_gui) dt_database_gui_register_handlers();

    const dt_database_open_result_t opened = dt_database_open(&db_params);
    if(opened == DT_DATABASE_OPEN_FAILED)
    {
      printf("ERROR : cannot open database\n");
      dt_free(configured_library);
      dt_gui_splash_close();
      return 1;
    }
    else if(opened == DT_DATABASE_OPEN_LOCKED)
    {
      gboolean error = FALSE;

      if (init_gui)
      {
        gboolean image_loaded_elsewhere = FALSE;
#ifndef MAC_INTEGRATION
        // send the images to the other instance via dbus
        fprintf(stderr, "trying to open the images in the running instance\n");

        if(darktable.dbus && darktable.dbus->dbus_connection)
        {
          GDBusConnection *connection = NULL;
          for(int i = 1; i < argc; i++)
          {
            // make the filename absolute ...
            if(argv[i] == NULL || *argv[i] == '\0') continue;
            gchar *filename = dt_util_normalize_path(argv[i]);
            if(IS_NULL_PTR(filename)) continue;
            if(IS_NULL_PTR(connection)) connection = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
            // ... and send it to the running instance of darktable
            image_loaded_elsewhere = g_dbus_connection_call_sync(connection, "org.darktable.service", "/darktable",
                                                                "org.darktable.service.Remote", "Open",
                                                                g_variant_new("(s)", filename), NULL,
                                                                G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL) != NULL;
            dt_free(filename);
          }
          if(connection) g_object_unref(connection);
        }
#endif
        if(!image_loaded_elsewhere)
          // Reporting CONSUMES the pending error.
          error = dt_database_show_error();
      }
      if(error)
      {
        fprintf(stderr, "ERROR: can't acquire database lock, aborting.\n");
        dt_free(configured_library);
        dt_gui_splash_close();
        return error;
      }
      else
      {
        // Close before retrying. The loop used to jump straight back to
        // dt_database_init(), abandoning the half-open database it had just been handed
        // -- its two filename strings and, when only one of the two lock files had been
        // taken, that lock file too. dt_database_open() refuses to open over an open
        // connection, so the leak is now a compile-time-visible step instead.
        dt_database_close();
        continue;
      }
    }
    recheck_needed = FALSE;
  }

  dt_free(configured_library);

  //db maintenance on startup (if configured to do so)
  if(dt_database_maybe_maintenance(FALSE))
  {
    dt_database_perform_maintenance();
  }

  // init darktable tags table
  dt_set_darktable_tags();

  // Initialize the signal system
  darktable.signals = dt_control_signal_init();

  /* src/metadata reports that the tag vocabulary changed; turning that into the GTK signal
   * its consumers already listen for is ours. Installed HERE, not with the other handlers
   * further up: the signal system does not exist until the line above, and nothing can
   * edit a tag before it does. */
  dt_metadata_set_tags_changed_handler(_metadata_tags_changed);
  dt_dev_history_gui_init();
  dt_gui_import_init_handlers();
  dt_metadata_set_geotags_changed_handler(_metadata_geotags_changed);
  dt_image_notify_set_imported_handler(_image_imported);
  dt_pipeline_set_message_handler(_pipeline_message);
  dt_pipeline_set_busy_handler(_pipeline_busy);
  // Same reason for these two: they raise signals, so they wait for the signal system.
  dt_history_set_changed_handler(_history_changed);
  dt_history_set_images_changed_handler(_history_images_changed);
  // Critical: ensure image cache gets refreshed BEFORE any other IMAGE_INFO_CHANGED handlers.
  // This handler reloads dt_image_t from DB so all downstream callbacks see fresh metadata.
  dt_image_cache_connect_info_changed_first(darktable.signals);

  // Make sure that the database and xmp files are in sync
  // We need conf and db to be up and running for that which is the case here.
  // FIXME: is this also useful in non-gui mode?
  GList *changed_xmp_files = NULL;
  if(init_gui && dt_conf_get_bool("run_crawler_on_start"))
  {
    changed_xmp_files = dt_control_crawler_run();
  }

  if(init_gui)
  {
    dt_control_init(darktable.control);
  }
  else
  {
    if(dbfilename_from_command && !strcmp(dbfilename_from_command, ":memory:"))
      dt_gui_presets_init(); // init preset db schema.
    darktable.control->running = 0;
    dt_pthread_mutex_init(&darktable.control->run_mutex, NULL);
    dt_pthread_mutex_init(&darktable.control->log_mutex, NULL);
  }

  // we initialize grouping early because it's needed for collection init
  // idem for folder reachability
  if(init_gui)
  {
    darktable.gui = (dt_gui_gtk_t *)calloc(1, sizeof(dt_gui_gtk_t));
    memset(darktable.gui->scroll_to, 0, sizeof(darktable.gui->scroll_to));
    dt_film_set_folder_status();
  }

  // initialize collection query
  darktable.collection = dt_collection_new();

  /* initialize selection */
  darktable.selection = dt_selection_new();

  /* capabilities set to NULL */
  darktable.capabilities = NULL;

  darktable.guides = dt_guides_init();

  // Re-assert our handlers once the third-party libraries above are up. This used to compensate
  // for GraphicsMagick's InitializeMagick(), which stole all of them; that library is gone, but
  // the call is cheap and idempotent (system_signal_handling.c counts its invocations), and it
  // keeps the guarantee that whatever init ran before this point cannot leave us without a
  // SIGSEGV handler. Crash reporting is initialized after it, deliberately.
  dt_set_signal_handlers();

#ifdef _OPENMP
  // Re-assert our thread count last, after every library above has initialised.
  //
  // omp_set_num_threads() writes only the CALLING thread's ICV, and a library that sizes its own
  // pool from omp_get_num_procs() publishes that by calling it -- a process-wide side effect on a
  // library-local decision. GraphicsMagick's InitializeMagick() did exactly that from here, which
  // silently overrode `-t N` and the "CPU cores" preference for every parallel region entered from
  // this thread afterwards: invisible in the GUI, where pixel work runs on control worker threads
  // that set their own count in dt_control_work(), and total in ansel-cli, where the export
  // pipeline runs on this very thread.
  //
  // Both of the libraries known to have done it -- GraphicsMagick and G'MIC -- have since been
  // removed, so nothing in the current dependency set is known to clobber the count. This stays
  // anyway, and deliberately unconditional: it costs one call at startup, the failure mode is
  // silent, and it took an LD_PRELOAD shim to find the last one. Keep it after every library init.
  omp_set_num_threads(darktable.num_openmp_threads);
#endif

  darktable.noiseprofile_parser = dt_noiseprofile_init(noiseprofiles_from_command);

  // The GUI must be initialized before the views, because the init()
  // functions of the views depend on darktable.control->accels_* to register
  // their keyboard accelerators

  // TODO : Make a single call to unified GUI API initializing everything graphical at once.
  // The current tangled mess is a nightmare to maintain.

  if(init_gui)
  {
    if(dt_gui_gtk_init(darktable.gui))
    {
      fprintf(stderr, "ERROR: can't init gui, aborting.\n");
      dt_gui_splash_close();
      return 1;
    }
    darktable.bauhaus = dt_bauhaus_init();
  }
  else
    darktable.gui = NULL;

  // This needs to run after gui init because we init cache lines size with window size
  // but before image cache init and pipeline cache init (aka dev init aka darkroom init aka viewmanager init)
  // because we init its size here
  dt_configure_runtime_performance(&darktable.dtresources, init_gui);

  darktable.view_manager = (dt_view_manager_t *)calloc(1, sizeof(dt_view_manager_t));
  dt_view_manager_init(darktable.view_manager);

  // check whether we were able to load darkroom view. if we failed, we'll crash everywhere later on.
  if(IS_NULL_PTR(darktable.develop))
  {
    fprintf(stderr, "ERROR: can't init develop system, aborting.\n");
    dt_gui_splash_close();
    return 1;
  }

  // The arena is only a virtual-memory reservation (physical pages get committed on
  // use), so init only fails on exhausted address space or a pathological requested
  // size. Rather than aborting outright, retry smaller: a shrunken cache degrades
  // performance, an abort loses the session.
  size_t pipecache_size = darktable.dtresources.pixelpipe_memory;
  dt_dev_pixelpipe_cache_init(pipecache_size,
                              (dt_get_debug_flags() & DT_DEBUG_PIPECACHE) != 0,
                              (dt_get_debug_flags() & DT_DEBUG_VERBOSE) != 0);
  while(!dt_dev_pixelpipe_cache_is_ready() && pipecache_size / 2 >= (size_t)512 * 1024 * 1024)
  {
    pipecache_size /= 2;
    fprintf(stderr,
            "WARNING: can't reserve %" G_GSIZE_FORMAT " MiB of virtual memory for the pixelpipe cache, "
            "retrying with %" G_GSIZE_FORMAT " MiB. Check your memory settings.\n",
            2 * pipecache_size / (1024 * 1024), pipecache_size / (1024 * 1024));
    dt_dev_pixelpipe_cache_init(pipecache_size,
                              (dt_get_debug_flags() & DT_DEBUG_PIPECACHE) != 0,
                              (dt_get_debug_flags() & DT_DEBUG_VERBOSE) != 0);
  }
  darktable.dtresources.pixelpipe_memory = pipecache_size;

  /* The cache announces three things -- it is full, a cacheline became readable, and the
   * supervisor's bookkeeping -- and used to do it by calling dt_control_log(), raising
   * DT_SIGNAL_CACHELINE_READY and calling dt_supervisor_*() itself. That put control/ and
   * develop/ inside a module that is otherwise pure storage. The orchestrator wires them
   * here instead: the cache says what happened, the application decides who hears it. */
  dt_dev_pixelpipe_cache_set_handlers(_pixelpipe_cache_warn, _pixelpipe_cache_ready,
                                      &_pixelpipe_cache_observer);
  if(!dt_dev_pixelpipe_cache_is_ready())
  {
    fprintf(stderr, "ERROR: can't init pixelpipe cache, aborting.\n");
    dt_gui_splash_close();
    return 1;
  }

  // High-level event supervisor registry (active only under -d supervisor).
  dt_supervisor_init();

  darktable.points = (dt_points_t *)calloc(1, sizeof(dt_points_t));
  dt_points_init(darktable.points, darktable.num_openmp_threads);

  // must come before mipmap_cache, because that one will need to access
  // image dimensions stored in here:
  dt_image_cache_init((dt_get_debug_flags() & DT_DEBUG_CACHE) != 0);

  const dt_mipmap_cache_settings_t mipmap_settings = _mipmap_settings_from_conf();
  dt_mipmap_cache_init(&mipmap_settings, (dt_get_debug_flags() & DT_DEBUG_CACHE) != 0);

  /* Re-tell the cache whenever the user changes one of its four settings. */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                            G_CALLBACK(_preferences_changed), NULL);

  dt_image_xmp_mode_refresh_from_conf();
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_PREFERENCES_CHANGE,
                            G_CALLBACK(_xmp_mode_preferences_changed), NULL);

#ifdef HAVE_OPENCL
  dt_opencl_init(exclude_opencl, print_statistics);
  // Show the splash only while compiling OpenCL kernels (triggered from opencl.c),
  // then close it immediately so the rest of the startup stays splash-free.
  dt_gui_splash_close();
#endif

  darktable.imageio = (dt_imageio_t *)calloc(1, sizeof(dt_imageio_t));
  dt_imageio_init(darktable.imageio);

  // load default iop order
  darktable.iop_order_list = dt_ioppr_get_iop_order_list(0, FALSE);
  // load iop order rules
  darktable.iop_order_rules = dt_ioppr_get_iop_order_rules();
  // load the darkroom mode plugins once:
  dt_iop_load_modules_so();
  // check if all modules have a iop order assigned
  if(dt_ioppr_check_so_iop_order(darktable.iop, darktable.iop_order_list))
  {
    fprintf(stderr, "ERROR: iop order looks bad, aborting.\n");
    dt_gui_splash_close();
    return 1;
  }

  // set up memory.darktable_iop_names table
  dt_iop_set_darktable_iop_table();

  // set up the list of exiv2 metadata
  dt_exif_set_exiv2_taglist();

  // init metadata flags
  dt_metadata_init();

  if(init_gui)
  {
    darktable.lib = (dt_lib_t *)calloc(1, sizeof(dt_lib_t));
    dt_lib_init(darktable.lib);

    // prevent bauhaus widgets from sending value-changed signals
    // because some of them expect user interactions.
    dt_gui_freeze_begin();

    // init the gui part of views
    dt_view_manager_gui_init(darktable.view_manager);

    dt_gui_freeze_end();

    // initialize undo struct
    darktable.undo = dt_undo_init();

    // Global menu inherits many parts of the GUI,
    // so it should be inited last
    dt_ui_init_global_menu(darktable.gui->ui);
  }

  if(darktable.unmuted & DT_DEBUG_MEMORY)
  {
    fprintf(stderr, "[memory] after successful startup\n");
    dt_print_mem_usage();
  }

  if(init_gui)
  {
    // we have to call dt_ctl_switch_mode_to() here already to not run into a lua deadlock.
    // having another call later is ok
    dt_ctl_switch_mode_to("lighttable");

#ifndef MAC_INTEGRATION
    // load image(s) specified on cmdline.
    // this has to happen after lua is initialized as image import can run lua code
    if (argc == 2)
    {
      // If only one image is listed, attempt to load it in darkroom
      (void)dt_load_from_string(argv[1], TRUE, NULL);
    }
    else if (argc > 2)
    {
      // when multiple names are given, fire up a background job to import them
      dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, dt_pathlist_import_create(argc,argv));
    }
#endif
  }

  // last but not least construct the popup that asks the user about images whose xmp files are newer than the
  // db entry
  if(init_gui && changed_xmp_files)
  {
    dt_control_crawler_show_image_list(changed_xmp_files);
  }

  if(init_gui)
  {
    // The user config is already loaded from gui/application.c, right after dt_accels_init()
    // and before any widget/menu is built -- loading it again here would re-read the
    // still-on-disk (unsaved) file and clobber any normalization dt_accels_connect_accels()
    // already applied to the live GtkAccelMap in the meantime (e.g. an old Ctrl-flavored
    // save recognized as today's platform-native default and resynced accordingly).
    dt_accels_connect_accels(darktable.gui->accels);
    //gtk_window_add_accel_group(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), darktable.gui->accels->global_accels);

    // Studio capture folder survey: restore the persisted comparison state and,
    // once the main loop runs and the window is mapped, propose to resume a
    // session that was monitoring when the application was last closed.
    dt_folder_survey_init();
    if(dt_folder_survey_session_was_active())
      g_idle_add((GSourceFunc)dt_folder_survey_propose_resume, NULL);
  }

  dt_gui_splash_close();

  // On first launch, ask once for consent to the opt-in data flows (crash reports
  // and usage analytics) in a single dialog, before initializing either module so
  // their enabled flags are set when they read them.
  dt_privacy_ask_consent(init_gui);

  // Initialize crash reporting last, after the final dt_set_signal_handlers() so
  // sentry's handler sits on top and chains down into our gdb/drmingw fallback.
  dt_sentry_init(init_gui);

  // Opt-in usage analytics (PostHog) - separate toggle from crash reporting.
  dt_telemetry_init(init_gui);

  dt_print(DT_DEBUG_CONTROL, "[init] startup took %f seconds\n", dt_get_wtime() - start_wtime);

  return 0;
}

static void _dt_drain_main_context(const int max_iters)
{
  if(max_iters <= 0) return;
  GMainContext *ctx = g_main_context_default();
  for(int i = 0; i < max_iters && g_main_context_pending(ctx); i++)
    g_main_context_iteration(ctx, FALSE);
}

void dt_cleanup()
{
  const int init_gui = (!IS_NULL_PTR(darktable.gui));

  // Flush crash reporting and mark this session as a clean exit. Done early so
  // events are sent while the rest of the app is still up; the clean-session
  // counter it writes is persisted later by dt_conf_cleanup().
  dt_sentry_shutdown();

  // Flush and stop usage analytics.
  dt_telemetry_shutdown();

  // Restore selection if exiting on culling mode to be sure it's saved in DB
  if(darktable.gui && darktable.gui->culling_mode)
    dt_culling_mode_to_selection();

  // Restore auto-computed zoom level to user-defined
  dt_conf_set_int("plugins/lighttable/images_in_row", dt_conf_get_int("plugins/lighttable/images_in_row_backup"));

  // last chance to ask user for any input...

  const gboolean perform_maintenance = dt_database_maybe_maintenance(TRUE);
  const gboolean perform_snapshot = dt_database_maybe_snapshot();
  gchar **snaps_to_remove = NULL;
  if(perform_snapshot)
  {
    snaps_to_remove = dt_database_snaps_to_remove();
  }

#ifdef HAVE_PRINT
  dt_printers_abort_discovery();
#endif

  // anything that asks user for input should be placed before this line

  if(init_gui)
  {
    if(!IS_NULL_PTR(darktable.gui->ui))
      dt_ui_cleanup_titlebar(darktable.gui->ui);

    if(darktable.gui->surface)
    {
      cairo_surface_destroy(darktable.gui->surface);
      darktable.gui->surface = NULL;
    }

    // hide main window and do rest of the cleanup in the background
    gtk_widget_hide(dt_ui_main_window(darktable.gui->ui));

    dt_ctl_switch_mode_to("");
    //dt_dbus_destroy(darktable.dbus);

    // Stop control workers before unloading views and libs. They can still be
    // processing lighttable-side jobs while shutdown is tearing down modules.
    dt_folder_survey_stop();
    dt_control_shutdown(darktable.control);
    dt_folder_survey_cleanup();

    _dt_drain_main_context(256);

    dt_lib_cleanup(darktable.lib);
    dt_free(darktable.lib);
  }

  dt_dev_pixelpipe_cache_wait_dump_pending("app-cleanup-before-view-manager");
  dt_view_manager_cleanup(darktable.view_manager);
  dt_free(darktable.view_manager);

  if(init_gui)
  {
    dt_imageio_cleanup(darktable.imageio);
    dt_free(darktable.imageio);

    dt_gui_presets_cleanup();

    if(!IS_NULL_PTR(darktable.gui->ui))
      dt_ui_cleanup_main_table(darktable.gui->ui);

    /* Force GTK to teardown the toplevel widget tree now, while the main
     * context still exists. This helps release style/cairo resources that
     * would otherwise stay alive until process exit. */
    GtkWidget *main_window = dt_ui_main_window(darktable.gui->ui);
    if(GTK_IS_WIDGET(main_window))
      gtk_widget_destroy(main_window);

    dt_gui_gtk_t *gui = darktable.gui;
    darktable.gui = NULL;
    dt_accels_cleanup(gui->accels);
    dt_free(gui->ui);
    dt_free(gui);
  }

  dt_colorlabels_cleanup();
  dt_history_repository_cleanup();
  dt_dev_history_cleanup();
  dt_metadata_cleanup();
  dt_tags_cleanup();
  dt_styles_cleanup();

  dt_collection_free(darktable.collection);
  dt_selection_free(darktable.selection);

  // Mipmap cleanup may still consult the image cache for paths.
  dt_mipmap_cache_cleanup();
  dt_image_cache_cleanup();

  dt_colorprofiles_cleanup();
  dt_conf_set_int("processing/gui_throttle_runtime_us", dt_gui_throttle_get_runtime_us());
  dt_gui_throttle_cleanup();
  dt_conf_cleanup(darktable.conf);
  dt_free(darktable.conf);
  dt_points_cleanup(darktable.points);
  dt_free(darktable.points);
  dt_iop_unload_modules_so();
  g_list_free_full(darktable.iop_order_list, dt_free_gpointer);
  darktable.iop_order_list = NULL;
  g_list_free_full(darktable.iop_order_rules, dt_free_gpointer);
  darktable.iop_order_rules = NULL;

#ifdef HAVE_OPENCL
  if(dt_opencl_is_inited() && dt_dev_pixelpipe_cache_is_ready())
  {
    for(int i = 0; i < dt_opencl_get_num_devices(); i++)
      dt_opencl_finish(i);
  }
#endif

  dt_dev_pixelpipe_cache_cleanup();
  dt_supervisor_cleanup();

  dt_opencl_cleanup();

  dt_guides_cleanup(darktable.guides);

  if(perform_maintenance)
  {
    dt_database_cleanup_busy_statements();
    dt_database_perform_maintenance();
  }

  dt_database_optimize();
  if(perform_snapshot)
  {
    if(dt_database_snapshot() && snaps_to_remove)
    {
      int i = 0;
      while(snaps_to_remove[i])
      {
        // make file to remove writable, mostly problem on windows.
        g_chmod(snaps_to_remove[i], S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

        dt_print(DT_DEBUG_SQL, "[db backup] removing old snap: %s... ", snaps_to_remove[i]);
        const int retunlink = g_remove(snaps_to_remove[i++]);
        dt_print(DT_DEBUG_SQL, "%s\n", retunlink == 0 ? "success" : "failed!");
      }
    }
  }
  if(snaps_to_remove)
  {
    g_strfreev(snaps_to_remove);
  }
  dt_database_close();

  if(init_gui)
  {
    dt_bauhaus_cleanup(darktable.bauhaus);
  }

  if (darktable.noiseprofile_parser)
  {
    g_object_unref(darktable.noiseprofile_parser);
    darktable.noiseprofile_parser = NULL;
  }

  if(init_gui)
  {
    dt_control_cleanup(darktable.control);
    dt_undo_cleanup(darktable.undo);
  }
  else
  {
    dt_pthread_mutex_destroy(&darktable.control->log_mutex);
    dt_pthread_mutex_destroy(&darktable.control->run_mutex);
  }
  dt_free(darktable.control);

  dt_control_signal_cleanup(darktable.signals);
  darktable.signals = NULL;

  dt_capabilities_cleanup();

  dt_pthread_mutex_destroy(&(darktable.plugin_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.capabilities_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.exiv2_threadsafe));
  dt_pthread_mutex_destroy(&(darktable.readFile_mutex));
  dt_pthread_mutex_destroy(&(darktable.pipeline_threadsafe));

  dt_exif_cleanup();

  /* Stop GLib pooled workers first, then release the current thread default
   * PangoCairo font map before finalizing Fontconfig caches. */
  if(init_gui)
  {
    g_thread_pool_stop_unused_threads();
#if !defined(_WIN32) && !defined(__APPLE__)
    PangoFontMap *fontmap = pango_cairo_font_map_get_default();
    gboolean use_fontconfig_backend = FALSE;

    if(fontmap && PANGO_IS_CAIRO_FONT_MAP(fontmap))
    {
      const cairo_font_type_t font_backend
          = pango_cairo_font_map_get_font_type(PANGO_CAIRO_FONT_MAP(fontmap));
      use_fontconfig_backend = (font_backend == CAIRO_FONT_TYPE_FT);
    }

    if(use_fontconfig_backend && fontmap && g_type_is_a(G_OBJECT_TYPE(fontmap), pango_fc_font_map_get_type()))
      pango_fc_font_map_shutdown((PangoFcFontMap *)fontmap);
    if(use_fontconfig_backend) FcFini();
#endif
    pango_cairo_font_map_set_default(NULL);
  }
}

double dt_get_start_wtime(void)
{
  return darktable.start_wtime;
}

int32_t dt_get_debug_flags(void)
{
  return darktable.unmuted;
}

int32_t dt_get_signal_debug_acts(void)
{
  return darktable.unmuted_signal_dbg_acts;
}

gboolean dt_get_signal_debug(const int signal)
{
  return darktable.unmuted_signal_dbg[signal];
}

void dt_print(dt_debug_thread_t thread, const char *msg, ...)
{
  if(thread == DT_DEBUG_ALWAYS || (darktable.unmuted & thread))
  {
    printf("%f ", dt_get_wtime() - darktable.start_wtime);
    va_list ap;
    va_start(ap, msg);
    g_vprintf(msg, ap);
    va_end(ap);
    fflush(stdout);
  }
}

void dt_print_nts(dt_debug_thread_t thread, const char *msg, ...)
{
  if(thread == DT_DEBUG_ALWAYS || (darktable.unmuted & thread))
  {
    va_list ap;
    va_start(ap, msg);
    g_vprintf(msg, ap);
    va_end(ap);
    fflush(stdout);
  }
}

void dt_vprint(dt_debug_thread_t thread, const char *msg, ...)
{
  if(thread == DT_DEBUG_ALWAYS || ((darktable.unmuted & DT_DEBUG_VERBOSE) && (darktable.unmuted & thread)))
  {
    printf("%f ", dt_get_wtime() - darktable.start_wtime);
    va_list ap;
    va_start(ap, msg);
    g_vprintf(msg, ap);
    va_end(ap);
    fflush(stdout);
  }
}

void dt_show_times(const dt_times_t *start, const char *prefix)
{
  /* Skip all the calculations an everything if -d perf isn't on */
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end;
    dt_get_times(&end);
    char buf[140]; /* Arbitrary size, should be lots big enough for everything used in DT */
    snprintf(buf, sizeof(buf), "%s took %.3f secs (%.3f CPU)", prefix, end.clock - start->clock,
             end.user - start->user);
    dt_print(DT_DEBUG_PERF, "%s\n", buf);
  }
}

void dt_show_times_f(const dt_times_t *start, const char *prefix, const char *suffix, ...)
{
  /* Skip all the calculations an everything if -d perf isn't on */
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_times_t end;
    dt_get_times(&end);
    char buf[160]; /* Arbitrary size, should be lots big enough for everything used in DT */
    const int n = snprintf(buf, sizeof(buf), "%s took %.3f secs (%.3f CPU) ", prefix, end.clock - start->clock,
                           end.user - start->user);
    if(n < sizeof(buf) - 1)
    {
      va_list ap;
      va_start(ap, suffix);
      vsnprintf(buf + n, sizeof(buf) - n, suffix, ap);
      va_end(ap);
    }
    dt_print(DT_DEBUG_PERF, "%s\n", buf);
  }
}

#if defined(_WIN32)
#include <windows.h>

static size_t _probe_system_available_mem(void)
{
  MEMORYSTATUSEX status;
  status.dwLength = sizeof(status);
  if(GlobalMemoryStatusEx(&status))
    return (size_t)status.ullAvailPhys; // Includes reclaimable
  else
    return 0;
}

#elif defined(__APPLE__)
#include <mach/mach.h>

static size_t _probe_system_available_mem(void)
{
  mach_port_t host = mach_host_self();
  vm_statistics64_data_t vmstat;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if(host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vmstat, &count) != KERN_SUCCESS) return 0;

  size_t page_size;
  host_page_size(host, &page_size);

  // Free + inactive (reclaimable)
  return (vmstat.free_count + vmstat.inactive_count) * page_size;
}

#elif defined(__linux__)
#include <string.h>

// A cgroup v2 memory.max limit (container, Flatpak sandbox, systemd slice) caps us
// before the system-wide MemAvailable does: the kernel enforces it with a per-cgroup
// OOM kill while /proc/meminfo still reports plenty of available RAM. Walk our own
// cgroup path up to the root and report the tightest limit (out_limit) and the
// tightest remaining slack including reclaimable pages (out_available), each
// SIZE_MAX when no level sets a limit.
static void _probe_cgroup_v2(size_t *out_limit, size_t *out_available)
{
  size_t tightest = SIZE_MAX;
  size_t tightest_limit = SIZE_MAX;
  *out_limit = SIZE_MAX;
  *out_available = SIZE_MAX;

  FILE *f = g_fopen("/proc/self/cgroup", "r");
  if(IS_NULL_PTR(f)) return;

  char line[512];
  char path[512] = "";
  while(fgets(line, sizeof(line), f))
  {
    // cgroup v2 unified hierarchy entry: "0::/user.slice/..."
    if(!strncmp(line, "0::", 3))
    {
      g_strlcpy(path, line + 3, sizeof(path));
      char *newline = strchr(path, '\n');
      if(newline) *newline = '\0';
      break;
    }
  }
  fclose(f);
  if(path[0] == '\0') return;

  while(TRUE)
  {
    char file[1024];
    snprintf(file, sizeof(file), "/sys/fs/cgroup%s/memory.max", path);
    FILE *fmax = g_fopen(file, "r");
    if(fmax)
    {
      // The file contains either a byte count or the literal "max" (no limit),
      // which simply fails the numeric scan.
      unsigned long long max_bytes = 0;
      if(fscanf(fmax, "%llu", &max_bytes) == 1 && max_bytes > 0)
      {
        unsigned long long cur_bytes = max_bytes;
        snprintf(file, sizeof(file), "/sys/fs/cgroup%s/memory.current", path);
        FILE *fcur = g_fopen(file, "r");
        if(fcur)
        {
          if(fscanf(fcur, "%llu", &cur_bytes) != 1) cur_bytes = max_bytes;
          fclose(fcur);
        }

        // memory.current still charges pages the kernel could drop at will: the page
        // cache and — critically for us — our own MADV_FREE'd arena pages, which land
        // on the cgroup's inactive file LRU. Count that list as available, the same
        // way system-wide MemAvailable does, or the slack of a busy cgroup would read
        // ~0 even though our own freed cache is fully reclaimable.
        unsigned long long inactive_file = 0;
        snprintf(file, sizeof(file), "/sys/fs/cgroup%s/memory.stat", path);
        FILE *fstat = g_fopen(file, "r");
        if(fstat)
        {
          char stat_line[256];
          while(fgets(stat_line, sizeof(stat_line), fstat))
            if(sscanf(stat_line, "inactive_file %llu", &inactive_file) == 1) break;
          fclose(fstat);
        }

        const size_t slack = (max_bytes > cur_bytes) ? (size_t)(max_bytes - cur_bytes) : 0;
        const size_t available = slack + (size_t)inactive_file;
        if(available < tightest) tightest = available;
        if((size_t)max_bytes < tightest_limit) tightest_limit = (size_t)max_bytes;
      }
      fclose(fmax);
    }

    char *slash = strrchr(path, '/');
    if(IS_NULL_PTR(slash) || slash == path) break;
    *slash = '\0';
  }

  *out_limit = tightest_limit;
  *out_available = tightest;
}

static size_t _probe_system_available_mem(void)
{
  FILE *f = g_fopen("/proc/meminfo", "r");
  if(IS_NULL_PTR(f)) return 0;

  char line[256];
  size_t available_kb = 0;
  size_t available = 0;

  while(fgets(line, sizeof(line), f))
  {
    if(sscanf(line, "MemAvailable: %" G_GSIZE_FORMAT " kB", &available_kb) == 1)
    {
      available = available_kb * 1024; // kB to bytes
      break;
    }
  }
  fclose(f);

  size_t cgroup_limit = SIZE_MAX;
  size_t cgroup_available = SIZE_MAX;
  _probe_cgroup_v2(&cgroup_limit, &cgroup_available);
  if(cgroup_available != SIZE_MAX) available = MIN(available, cgroup_available);

  return available;
}

#else
static size_t _probe_system_available_mem(void)
{
  return 0; // Unsupported platform: 0 = "no information", not "out of memory"
}
#endif

/* Short-lived cache of the system probe. Measured at 61 µs per probe (16 µs for
 * /proc/meminfo, 45 µs walking the cgroup tree), which the tiling planners alone
 * would pay ~30 times per pipeline run — 1.8 ms of a 16 ms realtime frame. The
 * cached value is at most DT_SYS_MEM_PROBE_PERIOD_US old; callers that just changed
 * the situation themselves (the pixelpipe pressure valve, right after shedding
 * cache) call dt_invalidate_system_available_mem() to force the next read to be
 * ground truth. */
#define DT_SYS_MEM_PROBE_PERIOD_US 50000 // 50 ms

static GMutex _sys_mem_probe_lock;
static size_t _sys_mem_probe_value = 0;
static gint64 _sys_mem_probe_time_us = 0;

size_t dt_get_system_available_mem(void)
{
  g_mutex_lock(&_sys_mem_probe_lock);

  const gint64 now = g_get_monotonic_time();
  if(_sys_mem_probe_time_us != 0 && now - _sys_mem_probe_time_us < DT_SYS_MEM_PROBE_PERIOD_US)
  {
    const size_t cached = _sys_mem_probe_value;
    g_mutex_unlock(&_sys_mem_probe_lock);
    return cached;
  }

  const size_t available = _probe_system_available_mem();
  _sys_mem_probe_value = available;
  _sys_mem_probe_time_us = now;
  g_mutex_unlock(&_sys_mem_probe_lock);
  return available;
}

void dt_invalidate_system_available_mem(void)
{
  g_mutex_lock(&_sys_mem_probe_lock);
  _sys_mem_probe_time_us = 0;
  g_mutex_unlock(&_sys_mem_probe_lock);
}

// Tightest container/cgroup memory limit this process runs under, SIZE_MAX when
// uncontained. This is the envelope the kernel actually OOM-enforces on us,
// regardless of how much RAM the machine has.
static size_t _get_container_mem_limit(void)
{
#if defined(__linux__)
  size_t cgroup_limit = SIZE_MAX;
  size_t cgroup_available = SIZE_MAX;
  _probe_cgroup_v2(&cgroup_limit, &cgroup_available);
  return cgroup_limit;
#else
  return SIZE_MAX;
#endif
}


int dt_worker_threads()
{
  return dt_conf_get_int("worker_threads");
}

size_t dt_get_available_mem()
{
  size_t cache_used = 0;
  size_t cache_max = 0;
  dt_dev_pixelpipe_cache_get_usage(&cache_used, &cache_max);
  const size_t budget_left = cache_max - cache_used;

  // The budget is only a startup-time plan: cap it by what the system can actually
  // back right now without dropping under the pressure floor (issue #1083), so
  // tiled modules plan tile sizes that will survive the allocation-time pressure
  // valve in pixelpipe_cache.c. Half of our own current cache usage counts as
  // room too: it is LRU-evictable, and the valve will evict it on demand — our own
  // cache must never be what forces a module into tiling.
  const size_t sys_available = dt_get_system_available_mem();
  if(sys_available == 0) return budget_left; // no information on this platform

  const size_t pressure_floor = dt_get_memory_pressure_floor();
  const size_t sys_room = ((sys_available > pressure_floor) ? sys_available - pressure_floor : 0)
                          + cache_used / 2;
  return MIN(budget_left, sys_room);
}

size_t dt_get_mipmap_mem()
{
  return darktable.dtresources.mipmap_memory;
}

size_t dt_get_total_mem(void)
{
  return darktable.dtresources.total_memory;
}

size_t dt_get_memory_pressure_floor(void)
{
  return darktable.dtresources.pressure_floor_memory;
}

void dt_configure_runtime_performance(dt_sys_resources_t *resources, gboolean init_gui)
{
  size_t physical_memory = _get_total_memory() * 1000;

  // A container/cgroup memory limit (Flatpak sandbox, systemd slice, docker...) is
  // the envelope the kernel actually OOM-enforces on us: inside it, the machine's
  // physical RAM is irrelevant, so it IS our total memory for budgeting purposes.
  const size_t container_limit = _get_container_mem_limit();
  if(container_limit != SIZE_MAX && (physical_memory == 0 || container_limit < physical_memory))
  {
    dt_print(DT_DEBUG_MEMORY | DT_DEBUG_CACHE,
             "[MEMORY CONFIGURATION] container/cgroup memory limit detected: %" G_GSIZE_FORMAT
             " MiB — using it as the total RAM\n", container_limit / (1024 * 1024));
    physical_memory = container_limit;
  }

  resources->total_memory = physical_memory;

  const size_t threads = darktable.num_openmp_threads;
  const size_t mem = resources->total_memory / (1024 * 1024);
  const size_t bits = CHAR_BIT * sizeof(void *);
  const gboolean sufficient = (mem >= 4096 && threads >= 2);

  dt_print(DT_DEBUG_MEMORY, "[MEMORY CONFIGURATION] found a %s %" G_GSIZE_FORMAT "-bit system with %" G_GSIZE_FORMAT " cores\n",
    (sufficient) ? "sufficient" : "low performance", bits, threads);

  // Override RAM detection with user config
  if(dt_conf_get_int64("host_memory_limit") > 0)
    resources->total_memory = dt_conf_get_int64("host_memory_limit") * 1024 * 1024;

  // A host_memory_limit above the physical RAM would make us plan caches the system
  // can never back: the OS OOM-killer, not us, would then enforce the difference by
  // killing the app without a message (issue #1083). The user limit can only shrink
  // the detected RAM, never grow it.
  if(physical_memory > 0 && resources->total_memory > physical_memory)
  {
    fprintf(stderr,
            "MEMORY WARNING: host_memory_limit (%" G_GSIZE_FORMAT " MiB) exceeds the physical RAM "
            "(%" G_GSIZE_FORMAT " MiB) and was clamped to it.\n",
            resources->total_memory / (1024 * 1024), physical_memory / (1024 * 1024));
    resources->total_memory = physical_memory;
  }

  // Keep OS headroom between 1 GB and a third of the system RAM
  resources->headroom_memory = dt_conf_get_int64("memory_os_headroom") * 1024 * 1024;
  resources->headroom_memory
      = CLAMP(resources->headroom_memory, 1024 * 1024 * 1024, resources->total_memory / 3);

  // Runtime memory-pressure floor: while running, whenever the SYSTEM-wide available
  // RAM drops below this — whether we or another application consumed it — the
  // pixelpipe cache sheds entries and returns their pages to the OS instead of
  // letting the system OOM-killer pick a victim (issue #1083). This is a live
  // safety net on top of the static budgets below, which are only plans.
  // Conf default 0 = auto: half the OS headroom.
  // The bounds scale with the envelope: on a normal machine the floor stays within
  // [512 MB, total/4], but inside a small container keeping 512 MB free would eat
  // the whole allotment, so the lower bound relaxes to total/8 there.
  // An ABSOLUTE reserve, not a fraction of anything. What the kernel and other
  // applications need in order to keep breathing does not scale with how much RAM the
  // machine has: 200 MiB is about as useful on a 4 GB laptop as on a 128 GB workstation.
  //
  // Deriving it from the headroom (previously half of it) made the floor grow with the
  // machine, which is backwards -- a 31 GB system ended up reserving 5175 MiB, so once
  // free RAM dipped under that the pixelpipe cache refused EVERY allocation, including
  // ones of a few kilobytes, and the application could not even start. Reserving a large
  // share of a large machine also wastes precisely the memory the user bought it for.
  int64_t pressure_floor = dt_conf_get_int64("memory_pressure_floor") * 1024 * 1024;
  if(pressure_floor <= 0) pressure_floor = DT_MEMORY_PRESSURE_FLOOR_DEFAULT;
  // Only a sanity clamp for hand-set values: never negative, and never so large on a
  // small machine that the floor swallows the whole envelope.
  resources->pressure_floor_memory = CLAMP(pressure_floor, 0, (int64_t)resources->total_memory / 4);

  // Keep mipmap cache between 256 MB and a sixth of the system RAM
  resources->mipmap_memory = dt_conf_get_int64("memory_mipmap_cache") * 1024 * 1024;
  resources->mipmap_memory
      = CLAMP(resources->mipmap_memory, 256 * 1024 * 1024, resources->total_memory / 6);

  // 6 temp copies of 24 Mpx RGBA float32 at full res
  const size_t min_pipecache_memory = 6 * 6000 * 4000 * 4 * sizeof(float); 

  // Pipeline cache gets the rest. Need to cast as int otherwise, negative values saturate the uint64 to MAX_UINT or something
  resources->pixelpipe_memory = MAX((int64_t)resources->total_memory 
                                    - (int64_t)resources->mipmap_memory 
                                    - (int64_t)resources->headroom_memory, 
                                    (int64_t)min_pipecache_memory);

  if(resources->pixelpipe_memory == min_pipecache_memory)
  {
    fprintf(stderr,
            "MEMORY WARNING: your pixelpipe cache allocated RAM is too small for your typical raw size.\n"
            "MEMORY WARNING: reduce your OS/apps headroom, or your thumbnail cache size.\n"
            "MEMORY WARNING: you may also simply need more RAM or need to reset the config key host_memory.\n"
            "MEMORY WARNING: we shrank the thumbnails cache to the bare minimum to leave enough space for pixelpipe cache.\n");
    resources->mipmap_memory = MAX((int64_t)resources->total_memory 
                                   - (int64_t)resources->headroom_memory 
                                   - (int64_t)resources->pixelpipe_memory, 
                                   (int64_t)128 * 1024 * 1024);
  }

  // Print
  dt_print(DT_DEBUG_MEMORY | DT_DEBUG_CACHE, _("[MEMORY CONFIGURATION] Total system RAM: %" G_GSIZE_FORMAT " MiB\n"),
           resources->total_memory / (1024 * 1024));

  dt_print(DT_DEBUG_MEMORY | DT_DEBUG_CACHE, _("[MEMORY CONFIGURATION] OS & Apps RAM headroom: %" G_GSIZE_FORMAT " MiB\n"),
           resources->headroom_memory / (1024 * 1024));

  dt_print(DT_DEBUG_MEMORY | DT_DEBUG_CACHE, _("[MEMORY CONFIGURATION] Lightable thumbnails cache size: %" G_GSIZE_FORMAT " MiB\n"),
           resources->mipmap_memory / (1024 * 1024));

  dt_print(DT_DEBUG_MEMORY | DT_DEBUG_CACHE, _("[MEMORY CONFIGURATION] Pixelpipe cache size: %" G_GSIZE_FORMAT " MiB\n"),
           resources->pixelpipe_memory / (1024 * 1024));

  dt_print(DT_DEBUG_MEMORY | DT_DEBUG_CACHE, _("[MEMORY CONFIGURATION] System memory pressure floor: %" G_GSIZE_FORMAT " MiB\n"),
           resources->pressure_floor_memory / (1024 * 1024));

  dt_print(DT_DEBUG_MEMORY | DT_DEBUG_CACHE, _("[MEMORY CONFIGURATION] Worker threads: %i\n"), dt_worker_threads());

  if(resources->total_memory < resources->headroom_memory + resources->mipmap_memory + resources->pixelpipe_memory)
    dt_control_log(_("CRITICAL WARNING: Ansel will not be able to use the RAM you allocated it.\n"
                     "Review your memory settings or add more RAM to your system."));
}

int dt_capabilities_check(char *capability)
{
  for(GList *capabilities = darktable.capabilities; capabilities; capabilities = g_list_next(capabilities))
  {
    if(!strcmp(capabilities->data, capability))
    {
      return TRUE;
    }
  }
  return FALSE;
}


void dt_capabilities_add(char *capability)
{
  dt_pthread_mutex_lock(&darktable.capabilities_threadsafe);

  if(!dt_capabilities_check(capability))
    darktable.capabilities = g_list_append(darktable.capabilities, capability);

  dt_pthread_mutex_unlock(&darktable.capabilities_threadsafe);
}


void dt_capabilities_remove(char *capability)
{
  dt_pthread_mutex_lock(&darktable.capabilities_threadsafe);

  darktable.capabilities = g_list_remove(darktable.capabilities, capability);

  dt_pthread_mutex_unlock(&darktable.capabilities_threadsafe);
}


void dt_capabilities_cleanup()
{
  while(darktable.capabilities)
    darktable.capabilities = g_list_delete_link(darktable.capabilities, darktable.capabilities);
}


void dt_print_mem_usage()
{
  fprintf(stdout, "[memory] Currently-free and reclaimable memory detected: %lu MiB\n", dt_get_system_available_mem() / (1024 * 1024));

#if defined(__linux__)
  char *line = NULL;
  size_t len = 128;
  char vmsize[64];
  char vmpeak[64];
  char vmrss[64];
  char vmhwm[64];
  FILE *f;

  char pidstatus[128];
  snprintf(pidstatus, sizeof(pidstatus), "/proc/%u/status", (uint32_t)getpid());

  f = g_fopen(pidstatus, "r");
  if(IS_NULL_PTR(f)) return;

  /* read memory size data from /proc/pid/status */
  while(getline(&line, &len, f) != -1)
  {
    if(!strncmp(line, "VmPeak:", 7))
      g_strlcpy(vmpeak, line + 8, sizeof(vmpeak));
    else if(!strncmp(line, "VmSize:", 7))
      g_strlcpy(vmsize, line + 8, sizeof(vmsize));
    else if(!strncmp(line, "VmRSS:", 6))
      g_strlcpy(vmrss, line + 8, sizeof(vmrss));
    else if(!strncmp(line, "VmHWM:", 6))
      g_strlcpy(vmhwm, line + 8, sizeof(vmhwm));
  }
  dt_free(line);
  fclose(f);

  fprintf(stderr, "[memory] max address space (vmpeak): %15s"
                  "[memory] cur address space (vmsize): %15s"
                  "[memory] max used memory   (vmhwm ): %15s"
                  "[memory] cur used memory   (vmrss ): %15s",
          vmpeak, vmsize, vmhwm, vmrss);

#elif defined(__APPLE__)
  struct task_basic_info t_info;
  mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

  if(KERN_SUCCESS != task_info(mach_task_self(), TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count))
  {
    fprintf(stderr, "[memory] task memory info unknown.\n");
    return;
  }

  // Report in kB, to match output of /proc on Linux.
  fprintf(stderr, "[memory] max address space (vmpeak): %15s\n"
                  "[memory] cur address space (vmsize): %12llu kB\n"
                  "[memory] max used memory   (vmhwm ): %15s\n"
                  "[memory] cur used memory   (vmrss ): %12llu kB\n",
          "unknown", (uint64_t)t_info.virtual_size / 1024, "unknown", (uint64_t)t_info.resident_size / 1024);
#elif defined (_WIN32)
  //Based on: http://stackoverflow.com/questions/63166/how-to-determine-cpu-and-memory-consumption-from-inside-a-process
  MEMORYSTATUSEX memInfo;
  memInfo.dwLength = sizeof(MEMORYSTATUSEX);
  GlobalMemoryStatusEx(&memInfo);
  // DWORDLONG totalVirtualMem = memInfo.ullTotalPageFile;

  // Virtual Memory currently used by current process:
  PROCESS_MEMORY_COUNTERS_EX pmc;
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc));
  size_t virtualMemUsedByMe = pmc.PagefileUsage;
  size_t virtualMemUsedByMeMax = pmc.PeakPagefileUsage;

  // Max Physical Memory currently used by current process
  size_t physMemUsedByMeMax = pmc.PeakWorkingSetSize;

  // Physical Memory currently used by current process
  size_t physMemUsedByMe = pmc.WorkingSetSize;


  fprintf(stderr, "[memory] max address space (vmpeak): %12llu kB\n"
                  "[memory] cur address space (vmsize): %12llu kB\n"
                  "[memory] max used memory   (vmhwm ): %12llu kB\n"
                  "[memory] cur used memory   (vmrss ): %12llu Kb\n",
          virtualMemUsedByMeMax / 1024, virtualMemUsedByMe / 1024, physMemUsedByMeMax / 1024,
          physMemUsedByMe / 1024);

#else
  fprintf(stderr, "dt_print_mem_usage() currently unsupported on this platform\n");
#endif
}

void dt_concat_path_file(char destination[DT_PATH_MAX], const char path[DT_PATH_MAX], const char *const file)
{
  g_strlcpy(destination, path, sizeof(char) * DT_PATH_MAX);
  g_strlcat(destination, G_DIR_SEPARATOR_S, sizeof(char) * DT_PATH_MAX);
  g_strlcat(destination, file, sizeof(char) * DT_PATH_MAX);  
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
