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

#ifndef DT_COMMON_DATABASE_H
#define DT_COMMON_DATABASE_H

#include <glib.h>
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dt_database_t;

/** allocates and initializes database */
struct dt_database_t *dt_database_init(const char *alternative, const gboolean load_data, const gboolean has_gui);
/** closes down database and frees memory */
void dt_database_destroy(const struct dt_database_t *);
/** get handle */
sqlite3 *dt_database_get(const struct dt_database_t *);

// Interim accessors (Strategy B, doc/globals-migration.md): implemented by the orchestrator; long-term the handle should be carried on the job/view context (Strategy C).
// The sqlite3 variant exists because nearly every consumer wants the connection, not the wrapper.
struct dt_database_t *dt_database_get_global(void);
sqlite3 *dt_database_get_sqlite3_global(void);
/** Returns database path */
const gchar *dt_database_get_path(const struct dt_database_t *db);
/** test if database was already locked by another instance */
gboolean dt_database_get_lock_acquired(const struct dt_database_t *db);
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

/** Move the pending error out of `db` into `error`, clearing it on the database.
 *  Consumes it: a second call reports no error. */
void dt_database_take_error(struct dt_database_t *db, dt_database_error_t *error);

/** Release the strings owned by `error`. */
void dt_database_error_free(dt_database_error_t *error);

/* Questions dt_database_init() must ask before it can continue.
 *
 * These are NOT the dt_database_error_t path below. That one records what went wrong for
 * whoever can report it afterwards; these three happen mid-init and the answer decides
 * whether init aborts, restores from a snapshot, or starts over -- there is no "afterwards"
 * to report to. So the backend states the question and takes back a value, and every trace
 * of how it is put to the user lives in gui/common/database_gui.c.
 */
typedef enum dt_database_prompt_t
{
  /** The library database cannot be written to. Informational: nothing to decide. */
  DT_DATABASE_PROMPT_READONLY = 0,
  /** The database is corrupt. Offer to close, restore a snapshot, or delete and start over. */
  DT_DATABASE_PROMPT_CORRUPTED
} dt_database_prompt_t;

typedef enum dt_database_response_t
{
  /** Give up and abort startup. Also what a caller with no handler gets. */
  DT_DATABASE_RESPONSE_CLOSE = 0,
  /** Delete the corrupt file and restore the most recent snapshot over it. */
  DT_DATABASE_RESPONSE_RESTORE,
  /** Delete the corrupt file and start with a fresh database. */
  DT_DATABASE_RESPONSE_DELETE
} dt_database_response_t;

/** Put @p prompt to the user and return their answer.
 *
 *  @param dbfilename          the database file the question is about.
 *  @param quick_check         sqlite's quick_check output, or NULL. Never NULL-terminated markup:
 *                             the handler escapes it, since only the handler knows the format.
 *  @param snapshot_available  TRUE when a snapshot exists, i.e. RESTORE is worth offering. */
typedef dt_database_response_t (*dt_database_prompt_handler_t)(dt_database_prompt_t prompt,
                                                               const char *dbfilename,
                                                               const char *quick_check,
                                                               gboolean snapshot_available);

/** Install the handler dt_database_init() asks through. NULL removes it.
 *
 *  Must be registered BEFORE dt_database_init() runs -- which is before dt_gui_gtk_init(),
 *  so this one cannot go where the film/collection/folder-survey handlers go. darktable.c
 *  registers it, being the only thing that knows this early whether there will be a GUI.
 *
 *  With no handler, every prompt answers CLOSE: a corrupt database is not deleted or
 *  restored on the strength of a question nobody was asked. That is also what makes a
 *  headless run safe -- these dialogs used to be built unconditionally, with no has_gui
 *  guard, on a GTK that ansel-cli never initialises. */
void dt_database_set_prompt_handler(dt_database_prompt_handler_t handler);

/** Delete data.db.lock and library.db.lock beside `dbfilename`. Returns 0 when every lock
 *  file that existed was removed. Call only once the user has agreed. */
int dt_database_delete_lock_files(const char *dbfilename);
/** perform pre-db-close optimizations (always call when quiting darktable) */
void dt_database_optimize(const struct dt_database_t *);
/** conditionally perfrom db maintenance */
gboolean dt_database_maybe_maintenance(const struct dt_database_t *db, const gboolean has_gui, const gboolean closing_time);
void dt_database_perform_maintenance(const struct dt_database_t *db);
/** cleanup busy statements on closing dt, just before performing maintenance */
void dt_database_cleanup_busy_statements(const struct dt_database_t *db);
/** simply create db snapshot of both library and data */
gboolean dt_database_snapshot(const struct dt_database_t *db);
/** check if creating database snapshot is recommended */
gboolean dt_database_maybe_snapshot(const struct dt_database_t *db);
/** get list of snapshot files to remove after successful snapshot */
char **dt_database_snaps_to_remove(const struct dt_database_t *db);
/** get possibly the freshest snapshot to restore */
gchar *dt_database_get_most_recent_snap(const char* db_filename);


// nested transactions support
void dt_database_start_transaction_debug(const struct dt_database_t *db);
void dt_database_release_transaction_debug(const struct dt_database_t *db);
void dt_database_rollback_transaction(const struct dt_database_t *db);
void dt_database_begin_transaction_batch(const struct dt_database_t *db);
void dt_database_end_transaction_batch(const struct dt_database_t *db);

#define dt_database_start_transaction(db) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_SQL, dt_database_start_transaction_debug, (db))
#define dt_database_release_transaction(db) DT_DEBUG_TRACE_WRAPPER(DT_DEBUG_SQL, dt_database_release_transaction_debug, (db))

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_DATABASE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
