/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010 johannes hanika.
    Copyright (C) 2011-2012 Edouard Gomez.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2017 Tobias Ellinghaus.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020, 2022 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
    Copyright (C) 2025-2026 Aurélien PIERRE.
    Copyright (C) 2025 Guillaume Stutin.
    
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

#ifndef DT_DATABASE_DATABASE_H
#define DT_DATABASE_DATABASE_H

/* DT_DEBUG_TRACE_WRAPPER, used by the transaction macros at the bottom of this file. It
 * used to arrive the other way round -- common/debug.h included this header, so anything
 * that reached here through it already had the wrapper. */
#include "common/debug.h"

#include <glib.h>
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------------------
 *  Lifecycle
 *
 *  There is exactly one connection, and the module owns it. `dt_database_t` used to be an
 *  opaque struct the caller held and passed back into all 20 functions below -- but every
 *  caller filled that argument with `dt_database_get_global()`, the module's own accessor,
 *  so it was never a parameter anybody chose. It is gone, along with the accessor and the
 *  entry on `darktable_t`.
 * ------------------------------------------------------------------------------------- */

/** What identifies a connection. Fixed for as long as one is open; a different workspace
 *  means closing and opening again with a different set. */
typedef struct dt_database_params_t
{
  /** Library file from the command line (`--library`), or NULL to use @ref library. */
  const char *alternative;
  /** Library file name as configured, e.g. "library.db". Relative names resolve against
   *  the user config directory; ":memory:" opens a throwaway in-memory database. */
  const char *library;
  /** Load the shipped presets and styles into data.db when creating it. */
  gboolean load_data;
  /** There is a user to put a prompt to. Without it every prompt answers CLOSE. */
  gboolean has_gui;
  /** Trace every statement and every maintenance decision (`-d sql`). Read once, here:
   *  the module does not consult the debug flags at runtime. */
  gboolean verbose;
} dt_database_params_t;

typedef enum dt_database_open_result_t
{
  /** Nothing usable came back. The caller should abort startup. */
  DT_DATABASE_OPEN_FAILED = 0,
  /** Open, and the lock files are ours. */
  DT_DATABASE_OPEN_OK,
  /** The files opened but another process holds the lock. Take the error with
   *  dt_database_take_error() to find out which, then decide whether to break the lock
   *  and call dt_database_open() again. */
  DT_DATABASE_OPEN_LOCKED
} dt_database_open_result_t;

/** Open the one connection. Fails if one is already open -- call dt_database_close() first. */
dt_database_open_result_t dt_database_open(const dt_database_params_t *params);

/** Close the connection and release the lock files. Safe to call when nothing is open.
 *
 *  @warning Every statement must be finalised before this returns, which is why the
 *  repositories under `src/database` expose a `*_cleanup()` each. A connection cannot be
 *  closed out from under a `sqlite3_stmt` still held elsewhere -- and *that* is the reason
 *  this pair is not yet a `dt_database_swap()`. See `src/database/README.md`. */
void dt_database_close(void);

/** TRUE between a successful dt_database_open() and dt_database_close(). */
gboolean dt_database_is_open(void);

/** Absolute path of the library file currently open, or NULL. Valid until the next
 *  dt_database_close(); copy it if you need to outlive that. */
const gchar *dt_database_get_path(void);

/** The connection itself.
 *
 *  @warning **This is the escape hatch, and it is counted.** Every caller of this holds a
 *  raw `sqlite3 *` that the module cannot account for, cannot serialise against a close,
 *  and cannot trace. `tools/check_module_boundaries.sh` ratchets the number of call sites
 *  downwards and it is deleted at zero -- at which point the private lock below actually
 *  guarantees something and swapping workspaces at runtime becomes implementable.
 *
 *  Do not call it from new code: put the query in a repository under `src/database` and
 *  give it a name. See `src/database/README.md`. */
sqlite3 *dt_database_get_sqlite3_global(void);

/** The message for the most recent failed call on the connection.
 *
 *  Exists so that reporting an error does not require the handle. Valid until the next call
 *  into the database from any thread -- copy it if you keep it. */
const char *dt_database_get_last_error(void);

/* ---------------------------------------------------------------------------------------
 *  Maintenance and snapshot policy
 *
 *  User preferences, read from conf by the orchestrator and told to the module -- the same
 *  arrangement as `dt_mipmap_cache_settings_t`. They used to be read with `dt_conf_*` from
 *  five places inside the maintenance and snapshot paths, which put `common/conf.h` in the
 *  SQL layer and made "when does this take effect" a question you answered by reading
 *  call sites.
 * ------------------------------------------------------------------------------------- */

typedef struct dt_database_settings_t
{
  /** When to offer a VACUUM: "never" | "on startup" | "on close" | "on both". */
  char *maintenance_check;
  /** Only offer it once this percentage of the file is free pages. */
  int maintenance_freepage_ratio;
  /** When to snapshot: "never" | "once a day" | "once a week" | "once a month" | "on close". */
  char *create_snapshot;
  /** How many snapshots to keep beside the library. */
  int keep_snapshots;
} dt_database_settings_t;

/** Replace the policy. Strings are copied; the caller keeps ownership of what it passed. */
void dt_database_set_settings(const dt_database_settings_t *settings);

/** Take a copy of the policy. Strings are newly allocated -- release with
 *  dt_database_settings_free(). Read as one snapshot under the module's lock: the GUI
 *  thread can replace these while a maintenance decision is being made, and four fields
 *  read one at a time can be a mix of old and new. */
void dt_database_get_settings(dt_database_settings_t *settings);

/** Release the strings owned by @p settings (not @p settings itself). */
void dt_database_settings_free(dt_database_settings_t *settings);

/** The module settled on a different library file than the params named, because the
 *  legacy `~/.darktablerc` database was migrated into the XDG directory. The orchestrator
 *  persists the new name; the module does not write conf. NULL removes the handler. */
typedef void (*dt_database_renamed_handler_t)(const char *new_library_name);
void dt_database_set_renamed_handler(dt_database_renamed_handler_t handler);

/* Why the database would not open, handed to whoever can report it.
 *
 * Reporting used to live here as dt_database_show_error(), a stack of modal dialogs inside
 * a SQL file. The backend now only says what went wrong; the dialogs, and the decision to
 * retry or delete lock files, are gui/common/database_gui.c's business. */
typedef struct dt_database_error_t
{
  gboolean lock_acquired; /**< TRUE when the database opened fine and there is nothing to report */
  int other_pid;          /**< the process that holds the lock, 0 if unknown */
  char *message;          /**< owned by the caller after the take; free with dt_database_error_free() */
  char *dbfilename;       /**< owned by the caller after the take */
} dt_database_error_t;

/** Move the pending error out of the module into `error`, clearing it.
 *  Consumes it: a second call reports no error. */
void dt_database_take_error(dt_database_error_t *error);

/** Release the strings owned by `error`. */
void dt_database_error_free(dt_database_error_t *error);

/* Questions the module must ask before it can continue.
 *
 * These are NOT the dt_database_error_t path above. That one records what went wrong for
 * whoever can report it afterwards; these happen mid-operation and the answer decides
 * whether init aborts, restores from a snapshot, starts over, or spends a minute
 * vacuuming -- there is no "afterwards" to report to. So the backend states the question
 * and takes back a value, and every trace of how it is put to the user lives in
 * gui/common/database_gui.c.
 *
 * The module passes FACTS, never prose. Composing a sentence -- and translating it, and
 * escaping it into markup -- is the handler's business, because only the handler knows
 * what it will be rendered into.
 */
typedef enum dt_database_prompt_t
{
  /** The library database cannot be written to. Informational: nothing to decide. */
  DT_DATABASE_PROMPT_READONLY = 0,
  /** The database is corrupt. Offer to close, restore a snapshot, or delete and start over. */
  DT_DATABASE_PROMPT_CORRUPTED,
  /** The schema is older than this build and must be migrated before anything else can
   *  happen. PROCEED migrates, CLOSE aborts startup so the user can back up first. */
  DT_DATABASE_PROMPT_UPGRADE,
  /** Enough of the file is free pages to be worth a VACUUM. PROCEED runs it now, CLOSE
   *  defers; ::ask_on_startup / ::ask_on_close say when the user would be asked again. */
  DT_DATABASE_PROMPT_MAINTENANCE
} dt_database_prompt_t;

typedef enum dt_database_response_t
{
  /** Do not do the thing. Abort startup, or defer the maintenance. Also what a caller
   *  with no handler gets, for every prompt. */
  DT_DATABASE_RESPONSE_CLOSE = 0,
  /** Delete the corrupt file and restore the most recent snapshot over it. */
  DT_DATABASE_RESPONSE_RESTORE,
  /** Delete the corrupt file and start with a fresh database. */
  DT_DATABASE_RESPONSE_DELETE,
  /** Go ahead: migrate the schema, or run the maintenance. */
  DT_DATABASE_RESPONSE_PROCEED
} dt_database_response_t;

/** What the handler is told. Every field is a fact about the database; none of it is
 *  user-facing text. Fields not relevant to ::prompt are zero. */
typedef struct dt_database_prompt_context_t
{
  dt_database_prompt_t prompt;
  /** The database file the question is about. */
  const char *dbfilename;
  /** CORRUPTED: sqlite's `quick_check` output, or NULL. Not markup -- the handler escapes
   *  it, since only the handler knows the format. */
  const char *quick_check;
  /** CORRUPTED: TRUE when a snapshot exists, i.e. RESTORE is worth offering. */
  gboolean snapshot_available;
  /** MAINTENANCE: how many bytes a VACUUM would reclaim. */
  guint64 reclaimable_bytes;
  /** MAINTENANCE: this check is happening as Ansel closes, rather than as it starts. */
  gboolean at_close;
  /** MAINTENANCE: declining now means being asked again at the next startup. */
  gboolean ask_on_startup;
  /** MAINTENANCE: declining now means being asked again when Ansel closes -- today's
   *  close if ::at_close is FALSE, the next one if it is TRUE. */
  gboolean ask_on_close;
} dt_database_prompt_context_t;

/** Put a question to the user and return their answer. */
typedef dt_database_response_t (*dt_database_prompt_handler_t)(const dt_database_prompt_context_t *context);

/** Install the handler the module asks through. NULL removes it.
 *
 *  Must be registered BEFORE dt_database_open() runs -- which is before dt_gui_gtk_init(),
 *  so this one cannot go where the film/collection/folder-survey handlers go. darktable.c
 *  registers it, being the only thing that knows this early whether there will be a GUI.
 *
 *  With no handler, every prompt answers CLOSE: a corrupt database is not deleted or
 *  restored, and a schema is not migrated, on the strength of a question nobody was asked.
 *  That is also what makes a headless run safe -- these dialogs used to be built
 *  unconditionally, with no has_gui guard, on a GTK that ansel-cli never initialises. */
void dt_database_set_prompt_handler(dt_database_prompt_handler_t handler);

/** Delete data.db.lock and library.db.lock beside `dbfilename`. Returns 0 when every lock
 *  file that existed was removed. Call only once the user has agreed. */
int dt_database_delete_lock_files(const char *dbfilename);
/** perform pre-db-close optimizations (always call when quiting darktable) */
void dt_database_optimize(void);
/** Ask the user whether to VACUUM, if the policy and the free-page ratio both say it is
 *  worth it. @p closing_time distinguishes the startup check from the shutdown one. */
gboolean dt_database_maybe_maintenance(const gboolean closing_time);
void dt_database_perform_maintenance(void);
/** cleanup busy statements on closing dt, just before performing maintenance */
void dt_database_cleanup_busy_statements(void);
/** simply create db snapshot of both library and data */
gboolean dt_database_snapshot(void);
/** check if creating database snapshot is recommended */
gboolean dt_database_maybe_snapshot(void);
/** get list of snapshot files to remove after successful snapshot */
char **dt_database_snaps_to_remove(void);
/** get possibly the freshest snapshot to restore */
gchar *dt_database_get_most_recent_snap(const char* db_filename);


/* Nested transactions.
 *
 * These took a `const dt_database_t *` that all 50 call sites filled with
 * dt_database_get_global(). They do not any more, and that accessor is gone with them. */
void dt_database_start_transaction_debug(void);
void dt_database_release_transaction_debug(void);
void dt_database_rollback_transaction(void);
void dt_database_begin_transaction_batch(void);
void dt_database_end_transaction_batch(void);

#define dt_database_start_transaction() DT_DEBUG_TRACE_WRAPPER_VOID(DT_DEBUG_SQL, dt_database_start_transaction_debug)
#define dt_database_release_transaction() DT_DEBUG_TRACE_WRAPPER_VOID(DT_DEBUG_SQL, dt_database_release_transaction_debug)

#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_DATABASE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
