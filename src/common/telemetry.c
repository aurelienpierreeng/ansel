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

#include "common/telemetry.h"
#include "common/darktable.h"

#ifdef HAVE_TELEMETRY

#include "common/image.h"
#include "common/opencl.h"
#include "control/conf.h"
#include "gui/gtk.h"

#include <curl/curl.h>
#include <string.h>

#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

#define POSTHOG_API_KEY "phc_uLtshRLGnot4cMieYFebh4gxkszztKLcfHgEYSZF3Cu6"

#ifndef POSTHOG_HOST
#define POSTHOG_HOST "https://eu.i.posthog.com"
#endif

// "telemetry/enabled" is a confgen key (shown in Preferences). The other two are
// intentionally NOT confgen so dt_conf_key_exists() reflects real user state.
#define DT_TELEMETRY_ENABLED_KEY "telemetry/enabled"
#define DT_TELEMETRY_ASKED_KEY "telemetry/consent_asked"
#define DT_TELEMETRY_INSTALL_ID_KEY "telemetry/install_id"

static gboolean _running = FALSE;
static GThread *_worker = NULL;
static GAsyncQueue *_queue = NULL;     // queue of malloc'd JSON body strings
static char *_distinct_id = NULL;      // anonymous per-installation id
static char _stop_sentinel;            // queue marker meaning "stop"

// Per-session aggregation, sent once in "session_end" at shutdown. Touched from
// the GUI thread (module usage) and pipeline worker threads (file types), so all
// access is guarded by _stats_lock.
static GMutex _stats_lock;
static GHashTable *_module_usage = NULL; // "category/name" -> count (GINT)
static GHashTable *_file_types = NULL;   // "ext" -> count (GINT)
static int _raw_images = 0;              // distinct images that were raw
static int _nonraw_images = 0;           // distinct images that were not raw
static int _mosaiced_images = 0;         // distinct images still needing demosaic
static int _processed_images = 0;        // distinct image+pipeline combinations
// Dedup state, mirroring the crash-context dedup so a reprocessed image counts once.
static int32_t _last_imgid = -1;
static char _last_pipeline[32] = { 0 };

// Discard HTTP response bodies; we only care that the POST went out.
static size_t _discard_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
  return size * nmemb;
}

// Background sender: pops serialized JSON bodies and POSTs them to PostHog.
static gpointer _telemetry_worker(gpointer data)
{
  CURL *curl = curl_easy_init();
  struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
  char url[512];
  snprintf(url, sizeof(url), "%s/capture/", POSTHOG_HOST);

  while(TRUE)
  {
    char *body = (char *)g_async_queue_pop(_queue); // blocks
    if(body == &_stop_sentinel) break;

    if(curl)
    {
      curl_easy_setopt(curl, CURLOPT_URL, url);
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _discard_cb);
      curl_easy_perform(curl); // best-effort: ignore network errors
    }
    g_free(body);
  }

  if(headers) curl_slist_free_all(headers);
  if(curl) curl_easy_cleanup(curl);
  return NULL;
}

void dt_telemetry_capture(const char *event, JsonObject *properties)
{
  if(!_running || !event)
  {
    if(properties) json_object_unref(properties);
    return;
  }

  JsonObject *root = json_object_new();
  json_object_set_string_member(root, "api_key", POSTHOG_API_KEY);
  json_object_set_string_member(root, "event", event);
  json_object_set_string_member(root, "distinct_id", _distinct_id ? _distinct_id : "unknown");

  GDateTime *now = g_date_time_new_now_utc();
  gchar *ts = g_date_time_format_iso8601(now);
  if(ts) json_object_set_string_member(root, "timestamp", ts);
  g_free(ts);
  g_date_time_unref(now);

  // set_object_member takes ownership of the properties object.
  json_object_set_object_member(root, "properties", properties ? properties : json_object_new());

  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, root);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, node);
  gchar *body = json_generator_to_data(gen, NULL);
  g_object_unref(gen);
  json_node_free(node); // frees root and, transitively, properties

  if(body) g_async_queue_push(_queue, body);
}

void dt_telemetry_record_module_usage(const char *category, const char *name)
{
  if(!_running || !category || !name || !*name) return;

  char *key = g_strdup_printf("%s/%s", category, name);

  g_mutex_lock(&_stats_lock);
  if(!_module_usage)
    _module_usage = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  const int count = GPOINTER_TO_INT(g_hash_table_lookup(_module_usage, key)) + 1;
  // insert frees the duplicate key when the entry already exists.
  g_hash_table_insert(_module_usage, key, GINT_TO_POINTER(count));
  const gboolean first_use = (count == 1);
  g_mutex_unlock(&_stats_lock);

  // Send a discrete event the first time each module is used this session. Unlike
  // the session_end aggregate (only sent on a clean exit), this reaches PostHog
  // immediately, so usage is still recorded if the session later crashes. One
  // event per distinct module per session keeps the volume low; PostHog can then
  // count/break-down "module_used" by category and name.
  if(first_use)
  {
    JsonObject *props = json_object_new();
    json_object_set_string_member(props, "category", category);
    json_object_set_string_member(props, "name", name);
    dt_telemetry_capture("module_used", props);
  }
}

void dt_telemetry_record_file_type(const struct dt_image_t *img, const char *pipeline)
{
  if(!_running || !img) return;
  const char *pl = pipeline ? pipeline : "";

  g_mutex_lock(&_stats_lock);
  // Count each image+pipeline once, even though pipelines reprocess constantly.
  if(img->id == _last_imgid && !strcmp(pl, _last_pipeline))
  {
    g_mutex_unlock(&_stats_lock);
    return;
  }
  _last_imgid = img->id;
  g_strlcpy(_last_pipeline, pl, sizeof(_last_pipeline));

  // Extension only - never the file name or path.
  const char *dot = strrchr(img->filename, '.');
  char *ext = g_ascii_strdown(dot ? dot + 1 : "none", -1);

  if(!_file_types)
    _file_types = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  const int count = GPOINTER_TO_INT(g_hash_table_lookup(_file_types, ext)) + 1;
  // g_hash_table_insert takes ownership of ext (frees the dup key on replace).
  g_hash_table_insert(_file_types, ext, GINT_TO_POINTER(count));
  const gboolean first_ext = (count == 1);

  const gboolean is_raw = dt_image_is_raw(img);
  const gboolean is_ldr = dt_image_is_ldr(img);
  const gboolean is_hdr = dt_image_is_hdr(img);
  const gboolean is_mono = dt_image_is_monochrome(img);
  const gboolean needs_demosaic = (img->dsc.filters != 0);

  if(is_raw) _raw_images++; else _nonraw_images++;
  if(needs_demosaic) _mosaiced_images++;
  _processed_images++;
  g_mutex_unlock(&_stats_lock);

  // First time we see a given extension this session, send a discrete event so
  // the kind of files processed reaches PostHog even if the session later
  // crashes. "ext" is owned by the hash table now, so re-derive it for the event.
  if(first_ext)
  {
    gchar *ext_lc = g_ascii_strdown(dot ? dot + 1 : "none", -1);
    JsonObject *props = json_object_new();
    json_object_set_string_member(props, "extension", ext_lc);
    g_free(ext_lc);
    json_object_set_boolean_member(props, "raw", is_raw);
    json_object_set_boolean_member(props, "ldr", is_ldr);
    json_object_set_boolean_member(props, "hdr", is_hdr);
    json_object_set_boolean_member(props, "monochrome", is_mono);
    json_object_set_boolean_member(props, "needs_demosaic", needs_demosaic);
    json_object_set_string_member(props, "pipeline", pl);
    dt_telemetry_capture("file_opened", props);
  }
}

// Flatten a "name -> count" hashtable into top-level numeric properties named
// "<prefix><sanitized-name>". PostHog only lets you filter/break-down/aggregate
// on top-level scalar properties: nested objects are ingested but invisible in
// insights, so module usage and file types must be flat to be usable in reports.
// Caller must hold _stats_lock.
static void _flatten_counts(JsonObject *p, const char *prefix, GHashTable *table)
{
  if(!table) return;

  GHashTableIter it;
  gpointer k, v;
  g_hash_table_iter_init(&it, table);
  while(g_hash_table_iter_next(&it, &k, &v))
  {
    // Build a PostHog-safe property name: prefix + key with every character
    // outside [A-Za-z0-9_] replaced by '_' (e.g. "view/lighttable" -> "lighttable",
    // prefixed -> "mod_view_lighttable").
    gchar *safe = g_strdup_printf("%s%s", prefix, (const char *)k);
    for(char *c = safe; *c; c++)
      if(!g_ascii_isalnum(*c) && *c != '_') *c = '_';
    json_object_set_int_member(p, safe, GPOINTER_TO_INT(v));
    g_free(safe);
  }
}

// Build the "session_end" payload: session length plus the per-session usage
// aggregates. System properties are carried by "session_start", so we keep this
// focused on what happened during the session.
static JsonObject *_telemetry_session_end_properties(void)
{
  JsonObject *p = json_object_new();

  const double dur = dt_get_wtime() - darktable.start_wtime;
  json_object_set_double_member(p, "session_seconds", (dur > 0.0) ? dur : 0.0);

  // Stamp the release on session_end too (not only session_start), so average
  // session length can be grouped by release and cross-checked against Sentry,
  // keyed on the same full commit SHA.
  json_object_set_string_member(p, "commit", darktable_commit_hash);
  json_object_set_string_member(p, "app_version", darktable_package_version);
  json_object_set_string_member(p, "build_channel", DT_BUILD_CHANNEL);
  // Same per-run id as the Sentry session_id tag, to correlate without double count.
  json_object_set_string_member(p, "session_id", dt_session_id());

  g_mutex_lock(&_stats_lock);
  // Flat numeric properties so they show up and can be aggregated in PostHog:
  //   mod_view_<name>, mod_lib_<plugin>, mod_iop_<op>, ext_<extension>.
  _flatten_counts(p, "mod_", _module_usage);
  _flatten_counts(p, "ext_", _file_types);
  json_object_set_int_member(p, "images_processed", _processed_images);
  json_object_set_int_member(p, "raw_images", _raw_images);
  json_object_set_int_member(p, "nonraw_images", _nonraw_images);
  json_object_set_int_member(p, "mosaiced_images", _mosaiced_images);
  g_mutex_unlock(&_stats_lock);

  return p;
}

// Build the common "what machine is this" properties shared by analytics events.
static JsonObject *_telemetry_system_properties(void)
{
  JsonObject *p = json_object_new();

  // Explicitly forbid capturing IP and GeoIP
  /*
   "$geoip_disable": true,  
   "$ip": "0.0.0.0"  
  */
  json_object_set_boolean_member(p, "$geoip_disable", TRUE);
  json_object_set_string_member(p, "$ip", "0.0.0.0");

  json_object_set_string_member(p, "app_version", darktable_package_version);
  // Full commit SHA: consistent release id across shallow/full clones (see sentry.c).
  json_object_set_string_member(p, "commit", darktable_commit_hash);
  // Same per-run id as the Sentry session_id tag, to correlate without double count.
  json_object_set_string_member(p, "session_id", dt_session_id());
  json_object_set_string_member(p, "build_type", DT_BUILD_TYPE);
  // Full C compiler flags baked in at configure time (includes -DNDEBUG, -O3, -g, etc.)
  json_object_set_string_member(p, "build_cflags", DT_BUILD_C_FLAGS);
  // "nightly" for official builds, "self-build" otherwise - lets analytics exclude
  // local/development builds from population stats.
  json_object_set_string_member(p, "build_channel", DT_BUILD_CHANNEL);

  gchar *os = g_get_os_info(G_OS_INFO_KEY_PRETTY_NAME);
#ifdef __APPLE__
  // macOS has no /etc/os-release, so g_get_os_info() returns NULL there. Build a
  // pretty name from the product version (e.g. "macOS 15.1") via sysctl.
  if(!os)
  {
    char ver[256] = { 0 };
    size_t len = sizeof(ver);
    if(sysctlbyname("kern.osproductversion", ver, &len, NULL, 0) == 0 && ver[0])
      os = g_strdup_printf("macOS %s", ver);
    else
      os = g_strdup("macOS");
  }
#endif
  if(os)
  {
    json_object_set_string_member(p, "os", os);
    g_free(os);
  }

  json_object_set_int_member(p, "cpu_cores", g_get_num_processors());
  if(darktable.dtresources.total_memory > 0)
    json_object_set_double_member(p, "ram_gb",
                                  (double)darktable.dtresources.total_memory / (1024.0 * 1024.0 * 1024.0));

  const gboolean cl = dt_opencl_is_enabled();
  json_object_set_boolean_member(p, "opencl", cl);
#ifdef HAVE_OPENCL
  // Device enumeration fields (num_devs/dev) only exist in HAVE_OPENCL builds.
  if(darktable.opencl && darktable.opencl->inited && darktable.opencl->num_devs > 0 && darktable.opencl->dev
     && darktable.opencl->dev[0].name)
    json_object_set_string_member(p, "gpu", darktable.opencl->dev[0].name);
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
  const char *session_type = g_getenv("XDG_SESSION_TYPE");
  if(session_type && *session_type) json_object_set_string_member(p, "display_server", session_type);
  const char *desktop = g_getenv("XDG_CURRENT_DESKTOP");
  if(desktop && *desktop) json_object_set_string_member(p, "desktop_environment", desktop);
#endif

  if(darktable.gui)
  {
    json_object_set_double_member(p, "dpi", darktable.gui->dpi);
    json_object_set_double_member(p, "ppd", darktable.gui->ppd);
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *mon = display ? gdk_display_get_primary_monitor(display) : NULL;
    if(!mon && display && gdk_display_get_n_monitors(display) > 0) mon = gdk_display_get_monitor(display, 0);
    if(mon)
    {
      GdkRectangle geo;
      gdk_monitor_get_geometry(mon, &geo);
      json_object_set_int_member(p, "screen_width", geo.width);
      json_object_set_int_member(p, "screen_height", geo.height);
    }
  }

  return p;
}

void dt_telemetry_init(const gboolean have_gui)
{
  // Consent is gathered once at startup by dt_privacy_ask_consent() (a single
  // dialog shared with crash reporting). Here we only honor the resulting toggle.
  if(!dt_conf_get_bool(DT_TELEMETRY_ENABLED_KEY)) return;

  if(POSTHOG_API_KEY[0] == '\0')
  {
    dt_print(DT_DEBUG_CONTROL, "[telemetry] no PostHog API key configured, analytics disabled\n");
    return;
  }

  // Anonymous, stable-per-installation id, shared with Sentry (dt_install_id) so
  // the same user de-duplicates across both systems.
  _distinct_id = g_strdup(dt_install_id()); // kept; freed at shutdown

  _queue = g_async_queue_new();
  _worker = g_thread_new("telemetry", _telemetry_worker, NULL);
  _running = TRUE;

  dt_print(DT_DEBUG_CONTROL, "[telemetry] usage analytics initialized\n");

  // One event per launch carries the system info, so every session (healthy or
  // not) is represented for population stats.
  dt_telemetry_capture("session_start", _telemetry_system_properties());
}

void dt_telemetry_shutdown(void)
{
  if(!_running) return;

  // Emit the per-session usage summary while we are still running (capture is a
  // no-op once _running is cleared), then stop accepting new events.
  dt_telemetry_capture("session_end", _telemetry_session_end_properties());

  _running = FALSE;

  // Tell the worker to drain and stop, then wait for the in-flight POST.
  g_async_queue_push(_queue, &_stop_sentinel);
  if(_worker)
  {
    g_thread_join(_worker);
    _worker = NULL;
  }
  if(_queue)
  {
    g_async_queue_unref(_queue);
    _queue = NULL;
  }
  g_free(_distinct_id);
  _distinct_id = NULL;

  g_mutex_lock(&_stats_lock);
  if(_module_usage)
  {
    g_hash_table_destroy(_module_usage);
    _module_usage = NULL;
  }
  if(_file_types)
  {
    g_hash_table_destroy(_file_types);
    _file_types = NULL;
  }
  g_mutex_unlock(&_stats_lock);
}

#else // !HAVE_TELEMETRY

void dt_telemetry_init(const gboolean have_gui)
{
}

void dt_telemetry_shutdown(void)
{
}

void dt_telemetry_capture(const char *event, JsonObject *properties)
{
  if(properties) json_object_unref(properties);
}

void dt_telemetry_record_module_usage(const char *category, const char *name)
{
}

void dt_telemetry_record_file_type(const struct dt_image_t *img, const char *pipeline)
{
}

#endif // HAVE_TELEMETRY

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
