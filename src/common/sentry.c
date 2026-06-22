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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/sentry.h"
#include "common/darktable.h"

#ifdef HAVE_SENTRY

#include "common/file_location.h"
#include "common/image.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "gui/gtk.h"

#include <sentry.h>

#include <signal.h> // for sig_atomic_t
#include <string.h> // for strrchr, strcmp

#if defined(__linux__)
#include <fcntl.h>      // for open flags
#include <sys/prctl.h>  // for PR_SET_PTRACER
#include <sys/wait.h>   // for waitpid
#include <unistd.h>     // for fork, getpid
#endif

#ifndef SENTRY_DSN
#define SENTRY_DSN ""
#endif

// Conf keys. "sentry/enabled" is a confgen key (shown in Preferences); the two
// below are intentionally NOT in confgen so dt_conf_key_exists() reflects whether
// the user has actually decided / how many clean sessions were recorded.
#define DT_SENTRY_ENABLED_KEY "sentry/enabled"
#define DT_SENTRY_ASKED_KEY "sentry/consent_asked"
#define DT_SENTRY_CLEAN_SESSIONS_KEY "sentry/clean_sessions"
#define DT_SENTRY_LAST_SESSION_KEY "sentry/last_session_seconds"
#define DT_SENTRY_TOTAL_SESSION_KEY "sentry/total_session_seconds"

static gboolean _sentry_inited = FALSE;

// Set once sentry's on_crash hook has captured a gdb backtrace, so the local
// signal handler can skip running gdb a second time for the same crash.
static volatile sig_atomic_t _sentry_backtrace_captured = 0;

// Per-session module usage counts ("category/name" -> count). Mutated only from
// the GUI thread; mirrored into the sentry scope on each change so the crash
// handler never has to read this table (which would be unsafe in a signal context).
static GHashTable *_module_usage = NULL;

// Dedup state for the currently-processed image. Pipelines run on worker threads
// (darkroom full/preview, export), so this is guarded by a mutex.
static GMutex _processed_image_lock;
static int32_t _processed_imgid = -1;
static char _processed_pipeline[32] = { 0 };
// Count of distinct image+pipeline runs this session, stamped on crash events so
// the website can report "images processed before a crash".
static volatile int _processed_image_count = 0;

// Length of the running session, in seconds. darktable.start_wtime is stamped at
// the very start of dt_init().
static double _sentry_session_seconds(void)
{
  const double dur = dt_get_wtime() - darktable.start_wtime;
  return (dur > 0.0) ? dur : 0.0;
}

// Stamp the event with the current session length, in seconds. For crashes this
// runs inside the crashing process, so the value is the exact time-to-crash.
static void _sentry_stamp_session_length(sentry_value_t event)
{
  const double dur = _sentry_session_seconds();

  // Numeric values under "extra" for inspection.
  sentry_value_t extra = sentry_value_get_by_key(event, "extra");
  if(sentry_value_is_null(extra))
  {
    extra = sentry_value_new_object();
    sentry_value_set_by_key(event, "extra", extra);
  }
  sentry_value_set_by_key(extra, "session_seconds", sentry_value_new_double(dur));
  sentry_value_set_by_key(extra, "images_processed", sentry_value_new_int32(_processed_image_count));

  // String tags so events are searchable/groupable by session length and by how
  // many images had been processed when the crash happened.
  char buf[32];
  snprintf(buf, sizeof(buf), "%.0f", dur);
  char ibuf[32];
  snprintf(ibuf, sizeof(ibuf), "%d", _processed_image_count);
  sentry_value_t tags = sentry_value_get_by_key(event, "tags");
  if(sentry_value_is_null(tags))
  {
    tags = sentry_value_new_object();
    sentry_value_set_by_key(event, "tags", tags);
  }
  sentry_value_set_by_key(tags, "session_seconds", sentry_value_new_string(buf));
  sentry_value_set_by_key(tags, "images_processed", sentry_value_new_string(ibuf));
}

// before_send handles NON-crash events (on_crash takes over for crashes). Stamp
// the session length so every event carries it.
static sentry_value_t _sentry_before_send(sentry_value_t event, void *hint, void *user_data)
{
  _sentry_stamp_session_length(event);
  return event;
}

#if defined(__linux__)
// Run gdb against the crashing process and capture its backtrace into a freshly
// allocated buffer (NUL-terminated; *len excludes the terminator). Returns NULL
// on failure. This mirrors the local gdb fallback in system_signal_handling.c but
// returns the text so it can be attached to the Sentry crash report.
static char *_sentry_capture_gdb_backtrace(gsize *len)
{
  gchar *name = NULL;
  const int fd = g_file_open_tmp("ansel_sentry_bt_XXXXXX.txt", &name, NULL);
  if(fd == -1) return NULL;
  close(fd);

  gchar *pid_arg = g_strdup_printf("%d", (int)getpid());
  gchar *exe_arg = g_strdup_printf("/proc/%s/exe", pid_arg);
  gchar *log_file_arg = g_strdup_printf("set logging file %s", name);

  char *contents = NULL;
  const pid_t pid = fork();
  if(pid == 0)
  {
    // child: gdb attaches to the parent and dumps all threads' backtraces
    execlp("gdb", "gdb", exe_arg, pid_arg, "-batch",
           "-ex", "set pagination off",
           "-ex", "set confirm off",
           "-ex", log_file_arg,
           "-ex", "set logging overwrite on",
           "-ex", "set logging redirect on",
           "-ex", "set logging enabled on",
           "-ex", "thread apply all bt full",
           NULL);
    _exit(127); // execlp only returns on failure
  }
  else if(pid > 0)
  {
    prctl(PR_SET_PTRACER, pid, 0, 0, 0); // let the child ptrace us (Yama)
    waitpid(pid, NULL, 0);

    gsize n = 0;
    if(g_file_get_contents(name, &contents, &n, NULL))
    {
      if(len) *len = n;
    }
    else
    {
      contents = NULL;
    }
  }

  g_unlink(name);
  g_free(name);
  g_free(pid_arg);
  g_free(exe_arg);
  g_free(log_file_arg);
  return contents;
}
#endif // __linux__

// on_crash replaces before_send for crash events (inproc). It runs before the
// crash event/attachments are serialized, so this is where we both stamp the
// session length and attach a full gdb backtrace to the report.
static sentry_value_t _sentry_on_crash(const sentry_ucontext_t *uctx, sentry_value_t event, void *user_data)
{
  _sentry_stamp_session_length(event);

#if defined(__linux__)
  gsize bt_len = 0;
  char *bt = _sentry_capture_gdb_backtrace(&bt_len);
  if(bt && bt_len > 0)
  {
    // Registered on the scope, this is picked up when the crash envelope is
    // assembled (right after this hook returns). sentry copies the bytes.
    sentry_attach_bytes(bt, bt_len, "gdb-backtrace.txt");

    // Tell the local signal handler (which runs next in the chain) not to run
    // gdb again for this same crash.
    _sentry_backtrace_captured = 1;
  }
  g_free(bt);
#endif

  return event;
}

gboolean dt_sentry_backtrace_captured(void)
{
  return _sentry_backtrace_captured != 0;
}

void dt_sentry_record_module_usage(const char *category, const char *name)
{
  if(!_sentry_inited || !category || !name || !*name) return;

  if(!_module_usage)
    _module_usage = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

  // g_hash_table_insert frees the duplicate key when the entry already exists,
  // so the table keeps a single owned copy of each key.
  char *key = g_strdup_printf("%s/%s", category, name);
  const int count = GPOINTER_TO_INT(g_hash_table_lookup(_module_usage, key)) + 1;
  g_hash_table_insert(_module_usage, key, GINT_TO_POINTER(count));

  // Push the whole map into the scope as the "module_usage" context. Cheap at
  // human interaction rates, and keeps the crash path free of table iteration.
  sentry_value_t obj = sentry_value_new_object();
  GHashTableIter iter;
  gpointer k, v;
  g_hash_table_iter_init(&iter, _module_usage);
  while(g_hash_table_iter_next(&iter, &k, &v))
    sentry_value_set_by_key(obj, (const char *)k, sentry_value_new_int32(GPOINTER_TO_INT(v)));
  sentry_set_context("module_usage", obj);
}

void dt_sentry_set_processed_image(const struct dt_image_t *img, const char *pipeline)
{
  if(!_sentry_inited || !img) return;
  const char *pl = pipeline ? pipeline : "";

  // Skip the (frequent) case where the same image keeps being reprocessed by the
  // same pipeline; only push a new context when something actually changed.
  g_mutex_lock(&_processed_image_lock);
  if(img->id == _processed_imgid && !strcmp(pl, _processed_pipeline))
  {
    g_mutex_unlock(&_processed_image_lock);
    return;
  }
  _processed_imgid = img->id;
  g_strlcpy(_processed_pipeline, pl, sizeof(_processed_pipeline));
  _processed_image_count++;
  g_mutex_unlock(&_processed_image_lock);

  // Extension and type flags only - never the file name or path.
  const char *dot = strrchr(img->filename, '.');

  sentry_value_t o = sentry_value_new_object();
  sentry_value_set_by_key(o, "extension", sentry_value_new_string(dot ? dot + 1 : ""));
  sentry_value_set_by_key(o, "pipeline", sentry_value_new_string(pl));
  sentry_value_set_by_key(o, "raw", sentry_value_new_bool(dt_image_is_raw(img)));
  sentry_value_set_by_key(o, "ldr", sentry_value_new_bool(dt_image_is_ldr(img)));
  sentry_value_set_by_key(o, "hdr", sentry_value_new_bool(dt_image_is_hdr(img)));
  sentry_value_set_by_key(o, "monochrome", sentry_value_new_bool(dt_image_is_monochrome(img)));
  // dsc.filters != 0 means the buffer still carries a CFA mosaic, i.e. it has not
  // been demosaiced yet.
  sentry_value_set_by_key(o, "needs_demosaic", sentry_value_new_bool(img->dsc.filters != 0));
  sentry_value_set_by_key(o, "width", sentry_value_new_int32(img->width));
  sentry_value_set_by_key(o, "height", sentry_value_new_int32(img->height));
  sentry_set_context("processed_image", o);
}

// Attach OS / hardware context so reports are actionable. No images, files or
// personal data: only the runtime environment.
static void _sentry_set_context(void)
{
  // Hardware / device context
  sentry_value_t device = sentry_value_new_object();
  sentry_value_set_by_key(device, "cpu_logical_cores", sentry_value_new_int32(g_get_num_processors()));
  sentry_value_set_by_key(device, "openmp_threads", sentry_value_new_int32(darktable.num_openmp_threads));

  if(darktable.dtresources.total_memory > 0)
  {
    const double mem_gb = (double)darktable.dtresources.total_memory / (1024.0 * 1024.0 * 1024.0);
    sentry_value_set_by_key(device, "memory_gb", sentry_value_new_double(mem_gb));
  }

  const gboolean cl_enabled = dt_opencl_is_enabled();
  sentry_value_set_by_key(device, "opencl_enabled", sentry_value_new_bool(cl_enabled));

#ifdef HAVE_OPENCL
  // Device enumeration fields (num_devs/dev) only exist in HAVE_OPENCL builds.
  if(darktable.opencl && darktable.opencl->inited && darktable.opencl->num_devs > 0 && darktable.opencl->dev)
  {
    sentry_value_t gpus = sentry_value_new_list();
    for(int i = 0; i < darktable.opencl->num_devs; i++)
    {
      const char *name = darktable.opencl->dev[i].name;
      if(name) sentry_value_append(gpus, sentry_value_new_string(name));
    }
    sentry_value_set_by_key(device, "opencl_devices", gpus);

    // Tag with the first device so events are filterable by GPU.
    if(darktable.opencl->dev[0].name) sentry_set_tag("opencl_device", darktable.opencl->dev[0].name);
  }
#endif
  sentry_set_context("device", device);

#if !defined(_WIN32) && !defined(__APPLE__)
  // Linux/BSD: display server (X11 vs Wayland) and desktop environment, as
  // searchable tags. Useful since many GUI bugs are backend/DE specific.
  const char *session_type = g_getenv("XDG_SESSION_TYPE");
  if(!session_type || !*session_type)
  {
    // Fall back to the well-known display sockets if the session type is unset.
    if(g_getenv("WAYLAND_DISPLAY"))
      session_type = "wayland";
    else if(g_getenv("DISPLAY"))
      session_type = "x11";
  }
  if(session_type && *session_type) sentry_set_tag("display_server", session_type);

  const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
  if(!desktop || !*desktop) desktop = g_getenv("DESKTOP_SESSION");
  if(desktop && *desktop) sentry_set_tag("desktop_environment", desktop);

  // What GTK actually renders on (may differ from the session, e.g. an X11 app
  // under XWayland). The GObject type name ("GdkWaylandDisplay" / "GdkX11Display")
  // gives this without pulling in the gdkwayland/gdkx backend headers.
  GdkDisplay *display = gdk_display_get_default();
  if(display) sentry_set_tag("gdk_backend", G_OBJECT_TYPE_NAME(display));
#endif

  // Display scaling and main window geometry (GUI sessions only). DPI/PPD come
  // from the GUI, already computed during dt_gui_gtk_init(). The window size is
  // read from conf, which holds the restored/last geometry and is kept up to date
  // live on every resize - more reliable than the not-yet-mapped window here.
  if(darktable.gui)
  {
    sentry_value_t scr = sentry_value_new_object();
    sentry_value_set_by_key(scr, "dpi", sentry_value_new_double(darktable.gui->dpi));
    sentry_value_set_by_key(scr, "dpi_factor", sentry_value_new_double(darktable.gui->dpi_factor));
    sentry_value_set_by_key(scr, "ppd", sentry_value_new_double(darktable.gui->ppd));

    const int win_w = dt_conf_get_int("ui_last/window_width");
    const int win_h = dt_conf_get_int("ui_last/window_height");
    if(win_w > 0 && win_h > 0)
    {
      sentry_value_set_by_key(scr, "window_width", sentry_value_new_int32(win_w));
      sentry_value_set_by_key(scr, "window_height", sentry_value_new_int32(win_h));

      // Searchable tag so issues can be filtered/grouped by window size.
      char wbuf[32];
      snprintf(wbuf, sizeof(wbuf), "%dx%d", win_w, win_h);
      sentry_set_tag("window_size", wbuf);
    }

    // Monitor resolution (logical pixels) of the primary monitor.
    GdkDisplay *gdkdisp = gdk_display_get_default();
    GdkMonitor *mon = gdkdisp ? gdk_display_get_primary_monitor(gdkdisp) : NULL;
    if(!mon && gdkdisp && gdk_display_get_n_monitors(gdkdisp) > 0)
      mon = gdk_display_get_monitor(gdkdisp, 0);
    if(mon)
    {
      GdkRectangle geo;
      gdk_monitor_get_geometry(mon, &geo);
      sentry_value_set_by_key(scr, "screen_width", sentry_value_new_int32(geo.width));
      sentry_value_set_by_key(scr, "screen_height", sentry_value_new_int32(geo.height));
      sentry_value_set_by_key(scr, "monitor_scale_factor",
                              sentry_value_new_int32(gdk_monitor_get_scale_factor(mon)));

      char sbuf[32];
      snprintf(sbuf, sizeof(sbuf), "%dx%d", geo.width, geo.height);
      sentry_set_tag("screen_size", sbuf);
    }
    sentry_set_context("display", scr);
  }

  // Build / runtime info as searchable extras
  sentry_set_extra("build_type", sentry_value_new_string(DT_BUILD_TYPE));
  sentry_set_tag("opencl", cl_enabled ? "yes" : "no");

  // Distribution channel as a searchable tag (also set as the Sentry environment in
  // dt_sentry_init), so official nightly builds can be told apart from self-builds.
  sentry_set_tag("build_channel", DT_BUILD_CHANNEL);

  // Stable per-run id, shared with usage analytics (PostHog) so the same session
  // can be correlated across both systems without double-counting.
  sentry_set_tag("session_id", dt_session_id());

  // Use the same anonymous per-installation id as PostHog's distinct_id, so the
  // "users" counted by Sentry de-duplicate against usage analytics.
  sentry_value_t user = sentry_value_new_object();
  sentry_value_set_by_key(user, "id", sentry_value_new_string(dt_install_id()));
  sentry_set_user(user);

  // Human-readable version string (the release itself is the commit SHA). May be a
  // full "0.0.0+3848~ghash" or, on a shallow clone, just the abbreviated hash.
  sentry_set_tag("version", darktable_package_version);

  // Surface how many crash-free sessions preceded this one (local mirror of the
  // server-side release-health metric, useful directly on the event).
  sentry_set_extra("clean_sessions_local",
                   sentry_value_new_int32(dt_conf_get_int(DT_SENTRY_CLEAN_SESSIONS_KEY)));

  // Length of the previous clean session and cumulative usage time, so events
  // (e.g. crashes) carry the user's recent/total session history. The current
  // session's own length is stamped per-event by _sentry_before_send().
  sentry_set_extra("previous_session_seconds",
                   sentry_value_new_int32(dt_conf_get_int(DT_SENTRY_LAST_SESSION_KEY)));
  sentry_set_extra("total_session_seconds",
                   sentry_value_new_int64(dt_conf_get_int64(DT_SENTRY_TOTAL_SESSION_KEY)));
}

void dt_sentry_init(const gboolean have_gui)
{
  // Consent is gathered once at startup by dt_privacy_ask_consent() (a single
  // dialog shared with usage analytics). Here we only honor the resulting toggle.
  if(!dt_conf_get_bool(DT_SENTRY_ENABLED_KEY))
    return;

  if(SENTRY_DSN[0] == '\0')
  {
    dt_print(DT_DEBUG_CONTROL, "[sentry] no DSN configured, crash reporting disabled\n");
    return;
  }

  sentry_options_t *options = sentry_options_new();
  sentry_options_set_dsn(options, SENTRY_DSN);

  // Keep the crash database next to our other runtime caches.
  char cachedir[PATH_MAX] = { 0 };
  dt_loc_get_user_cache_dir(cachedir, sizeof(cachedir));
  char *db_path = g_build_filename(cachedir, "sentry-native", NULL);
  sentry_options_set_database_path(options, db_path);
  g_free(db_path);

  // Release is keyed on the full commit SHA: it is the only build identifier that
  // is identical across shallow and full clones, so events from official builds and
  // self-builds of the same commit group into one release (the version string's
  // abbreviated hash / commit count is not consistent across clone types). The
  // human-readable version is attached as a tag in _sentry_set_context().
  char *release = g_strdup_printf("ansel@%s", darktable_commit_hash);
  sentry_options_set_release(options, release);
  g_free(release);

  // Environment separates official nightly builds from local/self-builds in the
  // dashboard and release-health metrics. The compiler/optimization build type is
  // kept separately as the "build_type" extra.
  sentry_options_set_environment(options, DT_BUILD_CHANNEL);
  sentry_options_set_debug(options, (darktable.unmuted & DT_DEBUG_CONTROL) ? 1 : 0);

  // Stamp non-crash events with the session length...
  sentry_options_set_before_send(options, _sentry_before_send, NULL);
  // ...and for crashes, stamp the session length and attach a full gdb backtrace
  // (on_crash replaces before_send for crash events).
  sentry_options_set_on_crash(options, _sentry_on_crash, NULL);

  // Release health: starts a session now, ended healthy on dt_sentry_shutdown()
  // or marked crashed by the in-process handler. This is what produces the
  // "sessions that ended with no error" / crash-free rate metric.
  sentry_options_set_auto_session_tracking(options, 1);

  if(sentry_init(options) == 0)
  {
    _sentry_inited = TRUE;
    _sentry_set_context();
    dt_print(DT_DEBUG_CONTROL, "[sentry] crash reporting initialized\n");
  }
  else
  {
    dt_print(DT_DEBUG_ALWAYS, "[sentry] initialization failed\n");
  }
}

void dt_sentry_shutdown(void)
{
  if(!_sentry_inited) return;

  // This session ended without a crash: bump the local counter before closing
  // so the next run (and any future crash) sees the updated count.
  dt_conf_set_int(DT_SENTRY_CLEAN_SESSIONS_KEY, dt_conf_get_int(DT_SENTRY_CLEAN_SESSIONS_KEY) + 1);

  // Record this healthy session's length (sentry's release health already tracks
  // the per-session duration server-side; this keeps a local record and feeds the
  // "previous/total session seconds" context attached on the next run).
  const int dur = (int)_sentry_session_seconds();
  dt_conf_set_int(DT_SENTRY_LAST_SESSION_KEY, dur);
  dt_conf_set_int64(DT_SENTRY_TOTAL_SESSION_KEY,
                    dt_conf_get_int64(DT_SENTRY_TOTAL_SESSION_KEY) + dur);

  sentry_close();
  _sentry_inited = FALSE;

  if(_module_usage)
  {
    g_hash_table_destroy(_module_usage);
    _module_usage = NULL;
  }
}

#else // !HAVE_SENTRY

void dt_sentry_init(const gboolean have_gui)
{
}

void dt_sentry_shutdown(void)
{
}

gboolean dt_sentry_backtrace_captured(void)
{
  return FALSE;
}

void dt_sentry_record_module_usage(const char *category, const char *name)
{
}

void dt_sentry_set_processed_image(const struct dt_image_t *img, const char *pipeline)
{
}

#endif // HAVE_SENTRY

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
