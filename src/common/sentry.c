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
#include "common/opencl.h"
#include "control/conf.h"
#include "gui/gtk.h"

#include <sentry.h>

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

// Length of the running session, in seconds. darktable.start_wtime is stamped at
// the very start of dt_init().
static double _sentry_session_seconds(void)
{
  const double dur = dt_get_wtime() - darktable.start_wtime;
  return (dur > 0.0) ? dur : 0.0;
}

// Stamp every outgoing event with the current session length. For inproc crashes
// this runs inside the crashing process (before the event is serialized to disk),
// so the crash event carries the exact time-to-crash. Also covers non-crash events.
static sentry_value_t _sentry_before_send(sentry_value_t event, void *hint, void *user_data)
{
  const double dur = _sentry_session_seconds();

  // Numeric value under "extra" for inspection.
  sentry_value_t extra = sentry_value_get_by_key(event, "extra");
  if(sentry_value_is_null(extra))
  {
    extra = sentry_value_new_object();
    sentry_value_set_by_key(extra, "session_seconds", sentry_value_new_double(dur));
    sentry_value_set_by_key(event, "extra", extra);
  }
  else
  {
    sentry_value_set_by_key(extra, "session_seconds", sentry_value_new_double(dur));
  }

  // String tag so events are searchable/groupable by session length.
  char buf[32];
  snprintf(buf, sizeof(buf), "%.0f", dur);
  sentry_value_t tags = sentry_value_get_by_key(event, "tags");
  if(sentry_value_is_null(tags))
  {
    tags = sentry_value_new_object();
    sentry_value_set_by_key(tags, "session_seconds", sentry_value_new_string(buf));
    sentry_value_set_by_key(event, "tags", tags);
  }
  else
  {
    sentry_value_set_by_key(tags, "session_seconds", sentry_value_new_string(buf));
  }

  return event;
}

// Ask the user, once, whether they agree to send anonymous crash reports.
// Returns TRUE if they opted in. Records the decision so we never ask again.
static gboolean _sentry_ask_consent(void)
{
  GtkWidget *parent = (darktable.gui && darktable.gui->ui) ? dt_ui_main_window(darktable.gui->ui) : NULL;

  GtkWidget *dialog = gtk_message_dialog_new(
      GTK_WINDOW(parent), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE, "%s", _("Help us make Ansel more stable?"));

  gtk_message_dialog_format_secondary_markup(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      _("Ansel can automatically report crashes to its developers so they can be fixed.\n\n"
        "If you agree, we send:\n"
        "  • the crash backtrace (where the program failed) when Ansel crashes,\n"
        "  • your operating system, and hardware specs (CPU, RAM, GPU),\n"
        "  • the Ansel version, and crash-free session statistics.\n\n"
        "We never send your images, file names, or any personal data.\n\n"
        "Data is collected by sentry.io and stored in Europe. You can review their policy at https://sentry.io/privacy/.\n\n"
        "This is entirely optional and you can change your choice at any time in\n"
        "<i>Preferences ▸ Storage ▸ Privacy</i>."));

  gtk_dialog_add_button(GTK_DIALOG(dialog), _("No, thanks"), GTK_RESPONSE_NO);
  gtk_dialog_add_button(GTK_DIALOG(dialog), _("Yes, send crash reports"), GTK_RESPONSE_YES);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES);

  const gint response = gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  return (response == GTK_RESPONSE_YES);
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
  sentry_set_context("device", device);

  // Build / runtime info as searchable extras
  sentry_set_extra("build_type", sentry_value_new_string(DT_BUILD_TYPE));
  sentry_set_tag("opencl", cl_enabled ? "yes" : "no");

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
  // First-run consent. "sentry/consent_asked" is not a confgen key, so its
  // presence means the user has actually made a choice before.
  if(!dt_conf_key_exists(DT_SENTRY_ASKED_KEY))
  {
    if(!have_gui)
      return; // can't ask without a GUI; stay disabled until the user is prompted

    const gboolean agreed = _sentry_ask_consent();
    dt_conf_set_bool(DT_SENTRY_ENABLED_KEY, agreed);
    dt_conf_set_bool(DT_SENTRY_ASKED_KEY, TRUE);
  }

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

  char *release = g_strdup_printf("ansel@%s", darktable_package_version);
  sentry_options_set_release(options, release);
  g_free(release);

  sentry_options_set_environment(options, DT_BUILD_TYPE);
  sentry_options_set_debug(options, (darktable.unmuted & DT_DEBUG_CONTROL) ? 1 : 0);

  // Stamp every event - including crash events, computed at crash time - with the
  // session length.
  sentry_options_set_before_send(options, _sentry_before_send, NULL);

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
}

#else // !HAVE_SENTRY

void dt_sentry_init(const gboolean have_gui)
{
}

void dt_sentry_shutdown(void)
{
}

#endif // HAVE_SENTRY

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
