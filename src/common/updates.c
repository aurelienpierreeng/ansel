/*
    This file is part of the Ansel project.
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

#include "config.h" // DT_BUILD_CHANNEL, darktable_commit_hash, darktable_package_version

#include "common/updates.h"

#include "common/conf.h"
#include "common/logging.h"

#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

#define DT_UPDATES_ENABLED_KEY "updates/enabled"
#define DT_UPDATES_LAST_CHECK_KEY "updates/last_check"
#define DT_UPDATES_MANIFEST_KEY "updates/manifest_url"
#define DT_UPDATES_INTERVAL (24 * 60 * 60)
// A manifest is a few kilobytes; anything past this is not the manifest.
#define DT_UPDATES_MAX_BODY (1 << 20)

static GThread *_worker = NULL;
static gint _shutting_down = 0;
static dt_updates_notify_fn _notify = NULL;
static char *_download_url = NULL;
static char *_available_version = NULL;

const char *dt_updates_runtime_format(void)
{
  // The AppImage runtime exports the path of the image it mounted.
  if(g_getenv("APPIMAGE")) return "appimage";
  // Every Flatpak sandbox carries its metadata at the root.
  if(g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS)) return "flatpak";
#if defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
  return "dmg-arm64";
#else
  return "dmg-i386";
#endif
#elif defined(_WIN32)
  return "exe";
#else
  return NULL;
#endif
}

const char *dt_updates_get_download_url(void)
{
  return _download_url;
}

const char *dt_updates_get_available_version(void)
{
  return _available_version;
}

static size_t _write_cb(char *data, size_t size, size_t nmemb, void *user)
{
  GString *body = (GString *)user;
  const size_t n = size * nmemb;
  if(body->len + n > DT_UPDATES_MAX_BODY) return 0; // aborts the transfer
  g_string_append_len(body, data, n);
  return n;
}

typedef struct _found_t
{
  char *url;
  char *version;
} _found_t;

// GUI thread: publish the result and tell the user once.
static gboolean _announce(gpointer data)
{
  _found_t *found = (_found_t *)data;
  if(!g_atomic_int_get(&_shutting_down))
  {
    g_free(_download_url);
    g_free(_available_version);
    _download_url = found->url;
    _available_version = found->version;
    found->url = found->version = NULL;
    if(_notify) _notify(_available_version, _download_url);
  }
  g_free(found->url);
  g_free(found->version);
  g_free(found);
  return G_SOURCE_REMOVE;
}

// Parse the manifest and decide. Returns a heap _found_t when a newer build exists.
static _found_t *_evaluate(const char *body, gsize len)
{
  JsonParser *parser = json_parser_new();
  _found_t *found = NULL;
  if(json_parser_load_from_data(parser, body, len, NULL))
  {
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *top = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    JsonObject *formats = (top && json_object_has_member(top, "formats")) ? json_object_get_object_member(top, "formats") : NULL;
    const char *format = dt_updates_runtime_format();
    JsonObject *entry = (formats && format && json_object_has_member(formats, format))
                            ? json_object_get_object_member(formats, format)
                            : NULL;
    if(entry)
    {
      const char *commit = json_object_has_member(entry, "commit") ? json_object_get_string_member(entry, "commit") : NULL;
      const char *version = json_object_has_member(entry, "version") ? json_object_get_string_member(entry, "version") : NULL;
      const char *url = json_object_has_member(entry, "url") ? json_object_get_string_member(entry, "url") : NULL;
      // The nightly channel is monotonic: a build whose commit is not ours is newer.
      // The manifest carries the full SHA and so does darktable_commit_hash; a short
      // hash in the manifest (commit resolution failed upstream) is compared as a
      // prefix rather than ignored.
      const gboolean differs = commit && *commit && strncmp(commit, darktable_commit_hash, strlen(commit)) != 0;
      dt_print(DT_DEBUG_CONTROL, "[updates] running %s %.10s, manifest %s %.10s -> %s\n", format,
               darktable_commit_hash, version ? version : "?", commit ? commit : "?",
               differs ? "newer available" : "up to date");
      if(differs && url)
      {
        found = g_new0(_found_t, 1);
        found->url = g_strdup(url);
        found->version = g_strdup(version ? version : "unknown");
      }
    }
    else
      dt_print(DT_DEBUG_CONTROL, "[updates] manifest has no entry for format '%s'\n", format ? format : "(unknown)");
  }
  else
    dt_print(DT_DEBUG_CONTROL, "[updates] manifest is not valid JSON\n");
  g_object_unref(parser);
  return found;
}

static gpointer _updates_worker(gpointer data)
{
  const char *url = dt_conf_get_string_const(DT_UPDATES_MANIFEST_KEY);
  if(!url || !*url) return NULL;

  GString *body = g_string_sized_new(8192);
  char agent[128];
  snprintf(agent, sizeof(agent), "Ansel/%s (%s)", darktable_package_version, DT_BUILD_CHANNEL);

  CURL *curl = curl_easy_init();
  if(curl)
  {
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
#if defined(_WIN32) && defined(CURLSSLOPT_NATIVE_CA)
    // Same reason as telemetry.c: the packaged libcurl has no CA bundle on disk.
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if(res == CURLE_OK && status == 200)
    {
      _found_t *found = _evaluate(body->str, body->len);
      if(found && !g_atomic_int_get(&_shutting_down))
        g_main_context_invoke(NULL, _announce, found);
      else if(found)
      {
        g_free(found->url);
        g_free(found->version);
        g_free(found);
      }
    }
    else
      dt_print(DT_DEBUG_CONTROL, "[updates] fetch of %s failed: %s (HTTP %ld)\n", url,
               res == CURLE_OK ? "unexpected status" : curl_easy_strerror(res), status);
  }
  g_string_free(body, TRUE);
  return NULL;
}

void dt_updates_init(const gboolean have_gui, dt_updates_notify_fn notify)
{
  _notify = notify;
  // Only a nightly knows what "newer" means; a self-build or a distribution package
  // is updated by whoever built it.
  if(!have_gui) return;
  if(strcmp(DT_BUILD_CHANNEL, "nightly") != 0) return;
  if(!dt_conf_get_bool(DT_UPDATES_ENABLED_KEY)) return;

  const gint64 now = (gint64)time(NULL);
  const gint64 last = dt_conf_get_int64(DT_UPDATES_LAST_CHECK_KEY);
  if(now - last < DT_UPDATES_INTERVAL)
  {
    dt_print(DT_DEBUG_CONTROL, "[updates] last check %" G_GINT64_FORMAT " s ago, not due\n", now - last);
    return;
  }
  dt_conf_set_int64(DT_UPDATES_LAST_CHECK_KEY, now);

  g_atomic_int_set(&_shutting_down, 0);
  _worker = g_thread_new("updates", _updates_worker, NULL);
  dt_print(DT_DEBUG_CONTROL, "[updates] checking for a newer nightly (%s)\n",
           dt_updates_runtime_format() ? dt_updates_runtime_format() : "unknown format");
}

void dt_updates_shutdown(void)
{
  g_atomic_int_set(&_shutting_down, 1);
  if(_worker)
  {
    g_thread_join(_worker); // bounded by the curl timeouts above
    _worker = NULL;
  }
  g_free(_download_url);
  g_free(_available_version);
  _download_url = _available_version = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
