/*
    This file is part of darktable,
    Copyright (C) 2010-2011, 2014 Henrik Andersson.
    Copyright (C) 2010-2013 johannes hanika.
    Copyright (C) 2010, 2012-2017 Tobias Ellinghaus.
    Copyright (C) 2012 Christian Tellefsen.
    Copyright (C) 2012 Jérémy Rosen.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Simon Spannagel.
    Copyright (C) 2012, 2014 Ulrich Pegelow.
    Copyright (C) 2013 Jean-Sébastien Pédron.
    Copyright (C) 2013 Jesper Pedersen.
    Copyright (C) 2014 parafin.
    Copyright (C) 2014, 2020-2021 Pascal Obry.
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2016-2018 Peter Budai.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 Philippe Weyland.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2021 Mark-64.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Hanno Schwalm.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
    
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

#ifndef DT_COMMON_CONF_H
#define DT_COMMON_CONF_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "system/dtpthread.h"
#include "common/paths.h"   // DT_PATH_MAX

#include <glib.h>
#include <gtk/gtk.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum dt_confgen_type_t
{
  DT_INT,
  DT_INT64,
  DT_FLOAT,
  DT_BOOL,
  DT_PATH,
  DT_STRING,
  DT_ENUM
} dt_confgen_type_t;

typedef struct dt_confgen_value_t
{
  dt_confgen_type_t type;
  char *def;
  char *min;
  char *max;
  char *enum_values;
  char *shortdesc;
  char *longdesc;
} dt_confgen_value_t;

/**
 * @brief The whole configuration state. One instance lives at darktable.conf.
 *
 * @details Every dt_conf_*() call below reaches it through that global and dereferences it
 * WITHOUT checking, so dt_conf_init() must have run before any of them. There is no
 * "conf not ready" return value; calling early is a NULL dereference.
 *
 * The two hash tables have opposite lifetimes, and that difference decides which returned
 * pointers are safe to hold:
 *
 *   - @ref table is LIVE. Any thread may replace a value through dt_conf_set_*(), and
 *     g_hash_table_insert() frees the value it replaces. A pointer into it is therefore
 *     only valid while @ref mutex is held.
 *   - @ref x_confgen is built once by dt_conf_init() from the generated XML and only read
 *     afterwards, until dt_conf_cleanup(). Pointers into it stay valid for the process
 *     lifetime, which is why the dt_confgen_*() readers take no lock at all.
 */
typedef struct dt_conf_t
{
  /** Guards @ref table and @ref override_entries. Not needed for @ref x_confgen. */
  dt_pthread_mutex_t mutex;
  /** Absolute path of the anselrc this instance loads from and saves to. */
  char filename[DT_PATH_MAX];
  /** Live key -> value strings, user-writable. Values are replaced and freed at runtime. */
  GHashTable *table;
  /** Defaults, bounds and descriptions from the generated XML. Immutable after init. */
  GHashTable *x_confgen;
  /** Keys forced on the command line; they win over @ref table on read. Written once by
   * dt_conf_init() and read-only afterwards, so pointers into it are stable. */
  GHashTable *override_entries;
  /** Values displaced by a later write, kept alive until dt_conf_cleanup().
   * dt_conf_get_string_const() borrows pointers into @ref table and its readers hold them
   * without the lock, so a replaced value cannot be freed at the moment it is replaced.
   * Grows only when a key's value actually changes. */
  GPtrArray *retired_values;
} dt_conf_t;

/**
 * @brief One key/value pair handed back by dt_conf_all_string_entries().
 *
 * @details Both members are private copies owned by the entry, not pointers into the conf
 * table -- which is what makes the returned list safe to hold after the lock is released.
 * Release an entry with dt_conf_string_entry_free().
 */
typedef struct dt_conf_string_entry_t
{
  /** Key name, relative to the directory that was asked for. Owned by this entry. */
  char *key;
  /** Value as a string. Owned by this entry. */
  char *value;
} dt_conf_string_entry_t;

typedef enum dt_confgen_value_kind_t
{
  DT_DEFAULT,
  DT_MIN,
  DT_MAX,
  DT_VALUES
} dt_confgen_value_kind_t;

void dt_conf_set_int(const char *name, int val);
void dt_conf_set_int64(const char *name, int64_t val);
void dt_conf_set_float(const char *name, float val);
void dt_conf_set_bool(const char *name, int val);
void dt_conf_set_string(const char *name, const char *val);
void dt_conf_set_folder_from_file_chooser(const char *name, GtkFileChooser *chooser);
/* The _fast readers return the STORED value. The plain readers return the same value
 * CLAMPED to the min/max declared for that key in the generated XML, at the cost of two
 * extra confgen lookups. "fast" therefore means "unclamped": for any key that declares
 * bounds, the two can disagree, and a value out of range is exactly the case where it
 * matters. Prefer the plain reader unless the key has no bounds or the caller clamps.
 *
 * All of them accept an arithmetic expression as the stored text -- it goes through the
 * calculator -- and fall back to the XML default, then to 0, when it does not parse. */

/** @brief Stored integer for @p name, NOT clamped to its declared bounds. */
int dt_conf_get_int_fast(const char *name);
/** @brief Integer for @p name, clamped to the bounds declared in the XML. */
int dt_conf_get_int(const char *name);
/** @brief Stored 64-bit integer for @p name, NOT clamped. */
int64_t dt_conf_get_int64_fast(const char *name);
/** @brief 64-bit integer for @p name, clamped to its declared bounds. */
int64_t dt_conf_get_int64(const char *name);
/** @brief Stored float for @p name, NOT clamped. */
float dt_conf_get_float_fast(const char *name);
/** @brief Float for @p name, clamped to its declared bounds. */
float dt_conf_get_float(const char *name);
int dt_conf_get_and_sanitize_int(const char *name, int min, int max);
int64_t dt_conf_get_and_sanitize_int64(const char *name, int64_t min, int64_t max);
float dt_conf_get_and_sanitize_float(const char *name, float min, float max);
int dt_conf_get_bool(const char *name);
/**
 * @brief Borrow the stored string for @p name without copying it.
 *
 * @return a pointer into the conf table. Do not free it. **Valid for the lifetime of the
 * process**, and never NULL.
 *
 * @warning It can go STALE. It is a snapshot: a later dt_conf_set_*() on the same key
 * installs a new string and leaves this one behind, so a caller holding the pointer keeps
 * reading the value as it was. Re-read the key when currency matters, or take a private
 * copy with dt_conf_get_string() when the value must be carried somewhere.
 *
 * @note This used to be a use-after-free rather than a staleness question: the value was
 * freed the moment another thread replaced it, while the reader still held the pointer --
 * the mechanism behind a "write_sidecar_files silently flips to FALSE" report.
 * dt_conf_set_if_not_overridden() now retires displaced values instead of freeing them,
 * which fixes every caller at once rather than asking 98 call sites to be audited
 * individually.
 */
const char *dt_conf_get_string_const(const char *name);

/**
 * @brief Read the stored string for @p name as a private copy.
 *
 * @details The copy is taken while the lock is held, so it cannot race a concurrent
 * dt_conf_set_*() on the same key.
 *
 * @return a newly-allocated string. **The caller owns it and must g_free() it.** Never
 * NULL: an unknown key is created on demand and yields the XML default, or an empty value.
 */
gchar *dt_conf_get_string(const char *name);
gboolean dt_conf_get_folder_to_file_chooser(const char *name, GtkFileChooser *chooser);
gboolean dt_conf_is_equal(const char *name, const char *value);
/**
 * @brief Populate @p cf from @p filename and from the generated defaults.
 *
 * @details Must run before ANY other dt_conf_*() call: the rest of this API reaches
 * darktable.conf unconditionally.
 *
 * @param cf storage to initialise, normally darktable.conf.
 * @param filename anselrc to read; created from defaults when absent.
 * @param override_entries command-line overrides, as dt_conf_string_entry_t. They win over
 * stored values on read and are not written back on save.
 */
void dt_conf_init(dt_conf_t *cf, const char *filename, GSList *override_entries);

/** @brief Free everything @p cf owns. No dt_conf_*() call is valid afterwards. */
void dt_conf_cleanup(dt_conf_t *cf);

/** @brief Write @p cf back to its file. Overridden keys keep their stored value. */
void dt_conf_save(dt_conf_t *cf);

/**
 * @brief Does @p key have a value?
 *
 * @warning TRUE for every key declared in the generated XML, on a first run with no
 * anselrc at all, because defaults are loaded at init. It answers "is this key known?",
 * NOT "has the user ever chosen?". For the latter, write a NON-confgen sentinel key when
 * the user acts and test that instead -- see the Sentry consent flow in common/sentry.c.
 */
int dt_conf_key_exists(const char *key);
gboolean dt_conf_key_not_empty(const char *key);
/**
 * @brief Every key under @p dir, as a list of copies.
 *
 * @param dir key prefix to match, e.g. "plugins/lighttable".
 * @return a newly-allocated GSList of dt_conf_string_entry_t. **The caller owns the list
 * AND its elements**; release with
 * `g_slist_free_full(list, dt_conf_string_entry_free)`. Empty (NULL) when nothing matches.
 * The entries hold copies, so they outlive any concurrent dt_conf_set_*().
 */
GSList *dt_conf_all_string_entries(const char *dir);

/** @brief Free one dt_conf_string_entry_t. Shaped as a GDestroyNotify so it can be handed
 * to g_slist_free_full(). */
void dt_conf_string_entry_free(gpointer data);

/* These three carry their own trailing semicolon, so a call site that adds one produces an
 * empty statement -- harmless alone, but it makes `if(c) DT_CONF_SET_SANITIZED_INT(...);
 * else ...` fail to compile. Brace the branch. */

/** @brief Clamp @p val to [@p min, @p max] and store it as an int. */
#define DT_CONF_SET_SANITIZED_INT(name, val, min, max) dt_conf_set_int(name, CLAMPS(val, min,max));

/** @brief Intended as the 64-bit form of DT_CONF_SET_SANITIZED_INT().
 * @warning It calls dt_conf_set_int(), not dt_conf_set_int64(), so a value beyond 32 bits
 * is truncated. Currently unused -- fix it before the first call site, and note the name
 * says 6464. */
#define DT_CONF_SET_SANITIZED_INT6464(name, val, min, max) dt_conf_set_int(name, CLAMPS(val, min,max));

/** @brief Clamp @p val to [@p min, @p max] and store it as a float. */
#define DT_CONF_SET_SANITIZED_FLOAT(name, val, min, max) dt_conf_set_float(name, CLAMPS(val, min,max));

// conf generated from darktable config XML

gboolean dt_confgen_exists(const char *name);
dt_confgen_type_t dt_confgen_type(const char *name);

gboolean dt_confgen_value_exists(const char *name, dt_confgen_value_kind_t kind);

int dt_confgen_get_int(const char *name, dt_confgen_value_kind_t kind);
int64_t dt_confgen_get_int64(const char *name, dt_confgen_value_kind_t kind);
gboolean dt_confgen_get_bool(const char *name, dt_confgen_value_kind_t kind);
float dt_confgen_get_float(const char *name, dt_confgen_value_kind_t kind);
/**
 * @brief One declared attribute of @p name from the generated XML, as text.
 *
 * @param kind which attribute: default, min, max, or the enum's value list.
 * @return a pointer into the confgen table, or NULL when @p name or @p kind is not
 * declared. Do NOT free it. Unlike dt_conf_get_string_const(), this one is safe to hold:
 * the confgen table is built once at init and never mutated, which is also why these
 * readers take no lock.
 */
const char *dt_confgen_get(const char *name, dt_confgen_value_kind_t kind);

/** @brief Translated short description of @p name, borrowed and valid for the process
 * lifetime. NULL when @p name is not declared. */
const char *dt_confgen_get_label(const char *name);

/** @brief Translated long description of @p name, borrowed and valid for the process
 * lifetime. NULL when @p name is not declared. */
const char *dt_confgen_get_tooltip(const char *name);

/** @brief Is @p name still at the value declared in the XML? FALSE for an undeclared key. */
gboolean dt_conf_is_default(const char *name);

/**
 * @brief Expand the placeholders in a default directory string.
 * @return a newly-allocated string. **The caller owns it and must g_free() it.**
 */
gchar* dt_conf_expand_default_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_CONF_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

