/*
    This file is part of darktable,
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2014-2016, 2020 Tobias Ellinghaus.
    Copyright (C) 2017 parafin.
    Copyright (C) 2018 Peter Budai.
    Copyright (C) 2019, 2021-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2020 esq4.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2022 Pascal Obry.
    Copyright (C) 2020 Philippe Weyland.
    Copyright (C) 2021 Hanno Schwalm.
    Copyright (C) 2021 Marco.
    Copyright (C) 2021 Marco Carrarini.
    Copyright (C) 2021 Miloš Komarčević.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
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

#include <glib.h>
#include "common/paths.h"   // DT_PATH_MAX
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "common/logging.h"
#include "common/history_actions.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "caches/image_cache.h"
#include "control/control.h"
#include "control/jobs.h"
#include "database/image_repository.h"
#include "common/image.h"
#include "common/utility.h"
#include "crawler.h"
#include "gui/application.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif


typedef enum dt_control_crawler_cols_t
{
  DT_CONTROL_CRAWLER_COL_ID = 0,
  DT_CONTROL_CRAWLER_COL_IMAGE_PATH,
  DT_CONTROL_CRAWLER_COL_XMP_PATH,
  DT_CONTROL_CRAWLER_COL_TS_XMP,
  DT_CONTROL_CRAWLER_COL_TS_DB,
  DT_CONTROL_CRAWLER_COL_TS_XMP_INT, // new timestamp to db
  DT_CONTROL_CRAWLER_COL_TS_DB_INT,
  DT_CONTROL_CRAWLER_COL_REPORT,
  DT_CONTROL_CRAWLER_COL_TIME_DELTA,
  DT_CONTROL_CRAWLER_NUM_COLS
} dt_control_crawler_cols_t;

typedef struct dt_control_crawler_result_t
{
  int id;
  time_t timestamp_xmp;
  time_t timestamp_db;
  char *image_path, *xmp_path;
} dt_control_crawler_result_t;

static void _free_crawler_result(dt_control_crawler_result_t *entry)
{
  dt_free(entry->image_path);
  dt_free(entry->xmp_path);
  entry->image_path = entry->xmp_path = NULL;
}

static void _free_crawler_results(GList *results)
{
  for(GList *l = results; !IS_NULL_PTR(l); l = g_list_next(l))
    _free_crawler_result((dt_control_crawler_result_t *)l->data);
  g_list_free_full(results, dt_free_gpointer);
}

static void _set_modification_time(char *filename,
                                   const time_t timestamp)
{
  GFile *gfile = g_file_new_for_path(filename);

  GFileInfo *info = g_file_query_info(
    gfile,
    G_FILE_ATTRIBUTE_TIME_MODIFIED "," G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC,
    G_FILE_QUERY_INFO_NONE,
    NULL,
    NULL);

  // For reference, we could use the following lines but for some
  // reasons there is a deprecated message raised even though this
  // routine is not marked as deprecated in the documentation.
  //
  // GDateTime *datetime = g_date_time_new_from_unix_local(timestamp);
  // g_file_info_set_modification_date_time(info, datetime);

  if(info)
  {
    g_file_info_set_attribute_uint64
      (info,
       G_FILE_ATTRIBUTE_TIME_MODIFIED,
       timestamp);

    g_file_set_attributes_from_info(
      gfile,
      info,
      G_FILE_QUERY_INFO_NONE,
      NULL,
      NULL);
  }

  g_object_unref(gfile);
  if(info) g_clear_object(&info);
}

/* A folder's contents as one lookup table: basename -> modification time.
 *
 * The crawler asks up to six questions about every image -- does the image still exist, does
 * its XMP exist and when was it last written, is there a .txt/.TXT/.wav/.WAV beside it -- and
 * used to answer each one with its own stat(). On a network filesystem the round-trip, not the
 * work, is the entire cost: measured at 8.1 ms per stat() on a GVFS/SMB share, a 1969-image
 * library spent 102 s in dt_control_crawler_run() before the main window was ever built.
 *
 * One directory listing answers all six questions for every image in that folder, and carries
 * the modification times with it -- SMB returns them in the listing itself, so the XMP
 * timestamp costs nothing beyond the listing. Same library, same share: 1.1 s for 3967 entries
 * across 18 folders.
 *
 * Do NOT "improve" this by parallelising the per-file lookups instead. That was measured on
 * the same share and does not work: gvfsd-fuse multiplexes every FUSE request through a single
 * daemon, so 4 threads gained 4% (inside the noise) and 64 threads ran twice as slow as one.
 * What this path needs is fewer round-trips, not overlapping ones.
 */
/* Windows and macOS resolve a filename without regard to case, and so does an SMB server:
 * stat() used to find `IMG.NEF.XMP' when asked for `IMG.NEF.xmp', and the exact hash lookup
 * that replaced it does not. So a folder carries a second, casefolded index, consulted only
 * when the exact name misses and built on that first miss -- a library whose names all agree
 * with the database never pays for it. Two names differing only in case cannot coexist on the
 * filesystems this rescues, and on one where they can (ext4) the exact lookup already
 * answered, so the fallback only ever adds tolerance it cannot take away. */
typedef struct dt_crawler_folder_t
{
  GHashTable *exact;  // basename -> guint64 *mtime, owns both
  GHashTable *folded; // casefolded basename -> the SAME guint64 *, borrowed; NULL until needed
} dt_crawler_folder_t;

static void _free_folder(gpointer p)
{
  dt_crawler_folder_t *folder = (dt_crawler_folder_t *)p;
  if(folder->folded) g_hash_table_destroy(folder->folded);
  g_hash_table_destroy(folder->exact);
  dt_free(folder);
}

typedef struct dt_crawler_walk_t
{
  GList **result;
  GHashTable *folders; // dirname -> dt_crawler_folder_t *
  dt_job_t *job;       // NULL for a crawl the user asked for from the menu: it runs to its end
} dt_crawler_walk_t;

/* A crawl run as a job stops when the job is cancelled, and when Ansel quits. Nothing cancels a
 * running job on the way out -- dt_control_quit() and dt_control_shutdown() only clear
 * `running`, then join the workers -- so `running` is the flag that says so, and a crawl that
 * ignored it would hold the quit up until the last image: the full mount timeout per folder
 * on a share that has gone away. */
static gboolean _job_cancelled(dt_job_t *job)
{
  return !IS_NULL_PTR(job)
         && (dt_control_job_get_state(job) == DT_JOB_STATE_CANCELLED || !dt_control_running());
}

/* The walk visits folders in film-roll order -- the query orders by f.id -- so one folder is
 * live at a time and this cache is a window onto the library, not a copy of it. The cap keeps
 * it a window even if that order ever changes: past it the cache is dropped wholesale rather
 * than growing with the collection, which costs a re-listing at worst and never a wrong
 * answer. */
#define DT_CRAWLER_FOLDER_CACHE_MAX 32

static dt_crawler_folder_t *_crawler_folder(dt_crawler_walk_t *walk, const char *dirname)
{
  GHashTable *folders = walk->folders;
  dt_crawler_folder_t *folder = (dt_crawler_folder_t *)g_hash_table_lookup(folders, dirname);
  if(folder) return folder;

  if(g_hash_table_size(folders) >= DT_CRAWLER_FOLDER_CACHE_MAX)
    g_hash_table_remove_all(folders);

  folder = (dt_crawler_folder_t *)g_malloc0(sizeof(dt_crawler_folder_t));
  folder->exact = g_hash_table_new_full(g_str_hash, g_str_equal,
                                        dt_free_gpointer, dt_free_gpointer);

  /* GIO rather than readdir()/stat(): it is the one spelling that works on all three
   * platforms, and on Windows it takes the UTF-8 path this database stores and does the
   * UTF-16 conversion itself -- which is exactly what the hand-rolled _wstati64() branch
   * removed from _crawl_image() was there to do. */
  GFile *dir = g_file_new_for_path(dirname);
  GFileEnumerator *entries = g_file_enumerate_children(dir,
                                                       G_FILE_ATTRIBUTE_STANDARD_NAME ","
                                                       G_FILE_ATTRIBUTE_TIME_MODIFIED,
                                                       G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if(entries)
  {
    GFileInfo *info = NULL;
    while(!_job_cancelled(walk->job) && (info = g_file_enumerator_next_file(entries, NULL, NULL)))
    {
      const char *name = g_file_info_get_name(info);
      if(name)
      {
        guint64 *mtime = (guint64 *)g_malloc(sizeof(guint64));
        *mtime = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        g_hash_table_insert(folder->exact, g_strdup(name), mtime);
      }
      g_object_unref(info);
    }
    g_object_unref(entries);
  }
  else
    dt_print(DT_DEBUG_CONTROL, "[crawler] cannot list `%s'.\n", dirname);

  g_object_unref(dir);

  /* A folder we could not read memoises as an EMPTY listing, not as "not looked at yet":
   * every image in it then reads as missing, which is exactly what the per-file stat()
   * answered for an unreachable folder -- and we do not ask again once per image. That is
   * the unplugged external drive and the offline share, at one failed call per folder
   * instead of six per image. */
  g_hash_table_insert(folders, g_strdup(dirname), folder);
  return folder;
}

/* TRUE if the folder holds `name`; its modification time goes to `mtime` when one is wanted. */
static gboolean _folder_holds(dt_crawler_folder_t *folder, const char *name, time_t *mtime)
{
  const guint64 *found = (const guint64 *)g_hash_table_lookup(folder->exact, name);

  if(!found)
  {
    if(!folder->folded)
    {
      folder->folded = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
      GHashTableIter iter;
      gpointer key, value;
      g_hash_table_iter_init(&iter, folder->exact);
      while(g_hash_table_iter_next(&iter, &key, &value))
        g_hash_table_insert(folder->folded, g_utf8_casefold((const char *)key, -1), value);
    }

    gchar *folded = g_utf8_casefold(name, -1);
    found = (const guint64 *)g_hash_table_lookup(folder->folded, folded);
    dt_free(folded);
  }

  if(!found) return FALSE;
  if(mtime) *mtime = (time_t)*found;
  return TRUE;
}

/* `name` with its extension replaced by the three characters `ext` -- the sibling-file
 * spelling the per-file lookups used to build by hand, kept byte-for-byte compatible with
 * them (a name carrying no '.' at all keeps its first character, as it always did). */
static gchar *_sibling_name(const char *name, const char *ext)
{
  size_t len = strlen(name);
  const char *c = name + len;
  while((c > name) && (*c != '.')) c--;
  len = c - name + 1;

  // g_strndup always allocates n + 1 bytes and NUL-pads, so writing [len .. len + 2] is in
  // bounds even when `name`'s own extension is shorter than three characters.
  gchar *sibling = g_strndup(name, len + 3);
  memcpy(sibling + len, ext, 3);
  return sibling;
}

/* One row of the library walk: everything below used to be the body of a cursor loop over
 * main.images joined to main.film_rolls, with a second statement writing the flags back. */
static gboolean _crawl_image(const int32_t id,
                             const int64_t timestamp,
                             const int version,
                             const char *image_path,
                             const int flags,
                             void *user_data)
{
  dt_crawler_walk_t *walk = (dt_crawler_walk_t *)user_data;
  if(_job_cancelled(walk->job)) return FALSE;

  gboolean go_on = TRUE;
  gchar *dirname = g_path_get_dirname(image_path);
  gchar *filename = g_path_get_basename(image_path);
  dt_crawler_folder_t *folder = _crawler_folder(walk, dirname);

  /* Checked again after the listing, which is where the time goes: a cancel landing during it
   * leaves a listing cut short, and a name missing from it would read as a file missing from
   * disk -- clearing the companion flags of every image whose .txt was not listed yet. */
  if(_job_cancelled(walk->job))
  {
    go_on = FALSE;
    goto done;
  }

  // if the image is missing we ignore it.
  if(!_folder_holds(folder, filename, NULL))
  {
    dt_print(DT_DEBUG_CONTROL, "[crawler] `%s' (id: %d) is missing.\n", image_path, id);
    goto done;
  }

  {
    // construct the xmp filename for this image
    gchar xmp_name[DT_PATH_MAX] = { 0 };
    g_strlcpy(xmp_name, filename, sizeof(xmp_name));
    dt_image_path_append_version_no_db(version, xmp_name, sizeof(xmp_name));
    g_strlcat(xmp_name, ".xmp", sizeof(xmp_name));

    time_t xmp_timestamp = 0;
    if(!_folder_holds(folder, xmp_name, &xmp_timestamp))
      goto done; // TODO: shall we report these?

    // step 1: check if the xmp is newer than our db entry
    // FIXME: allow for a few seconds difference?
    if(timestamp < xmp_timestamp)
    {
      dt_control_crawler_result_t *item
          = (dt_control_crawler_result_t *)malloc(sizeof(dt_control_crawler_result_t));
      item->id = id;
      item->timestamp_xmp = xmp_timestamp;
      item->timestamp_db = timestamp;
      item->image_path = g_strdup(image_path);
      item->xmp_path = g_build_filename(dirname, xmp_name, NULL);

      *walk->result = g_list_prepend(*walk->result, item);
      dt_print(DT_DEBUG_CONTROL,
                "[crawler] `%s' (id: %d) is a newer XMP file.\n", item->xmp_path, id);
    }
    // older timestamps are the case for all images after the db
    // upgrade. better not report these
  }

  {
    // step 2: check if the image has associated files (.txt, .wav)
    // Both spellings of each, in the order the per-file lookups tried them.
    gchar *txt_lower = _sibling_name(filename, "txt");
    gchar *txt_upper = _sibling_name(filename, "TXT");
    const gboolean has_txt = _folder_holds(folder, txt_lower, NULL)
                          || _folder_holds(folder, txt_upper, NULL);
    dt_free(txt_lower);
    dt_free(txt_upper);

    gchar *wav_lower = _sibling_name(filename, "wav");
    gchar *wav_upper = _sibling_name(filename, "WAV");
    const gboolean has_wav = _folder_holds(folder, wav_lower, NULL)
                          || _folder_holds(folder, wav_upper, NULL);
    dt_free(wav_lower);
    dt_free(wav_upper);

    // TODO: decide if we want to remove the flag for images that lost
    // their extra file. currently we do (the else cases)
    const int mask = DT_IMAGE_HAS_TXT | DT_IMAGE_HAS_WAV;
    int value = 0;
    if(has_txt) value |= DT_IMAGE_HAS_TXT;
    if(has_wav) value |= DT_IMAGE_HAS_WAV;

    /* The row is not the only copy of this word. An image the user has looked at also has an
     * image-cache entry holding its own dt_image_t, and releasing that entry writes the whole
     * struct back -- which is how a rating or a colour label reaches the database at all
     * (metadata/ratings.c's _ratings_apply_to_image()). So when the image has an entry, the
     * entry is what we compare against AND what we edit: it owns the struct, and its write lock
     * is the one _ratings_apply_to_image() takes, which is what serialises the two. A rating set
     * meanwhile either lands before us and is read here, or lands after us and sees our bits.
     *
     * Compare against the entry, not the row. The two can disagree on these very bits -- a row
     * written behind the entry earlier, say -- and a guard reading the row would then find
     * nothing to do, leave the entry stale, and let its next release write the stale bits back.
     *
     * get_existing(), not testget(). testget() returns NULL for an entry someone holds this
     * instant as well as for no entry, and writing the row in the first case writes behind a
     * live entry whose release then reverts it. get_existing() waits for that entry instead --
     * and, like testget(), never creates one, so a crawl over the whole library does not pull
     * the whole library into the cache on its way past.
     *
     * RELAXED rather than SAFE: the release writes the row, but must not queue an XMP write
     * from the one job whose entire purpose is to find out whether the sidecars are in sync.
     * MINIMAL when nothing changed: that gives the lock back without writing anything. */
    dt_image_t *cached = dt_image_cache_get_existing(id, 'w');
    if(!IS_NULL_PTR(cached))
    {
      if((cached->flags & mask) != value)
      {
        cached->flags = (cached->flags & ~mask) | value;
        dt_image_cache_write_release(cached, DT_IMAGE_CACHE_RELAXED);
      }
      else
        dt_image_cache_write_release(cached, DT_IMAGE_CACHE_MINIMAL);
    }
    /* No entry, so the row is the only copy and comparing against it is sound. `flags` was read
     * from it before this folder was listed -- a filesystem round-trip, up to a second on a
     * network share -- so the write is masked: a rating set in that window lives in the same
     * word and must survive. The two bits compared are written only by the crawl and by the
     * import, so the stale read costs at worst one redundant UPDATE. */
    else if((flags & mask) != value)
      dt_image_repository_set_flags_masked(id, mask, value);
  }

done:
  dt_free(dirname);
  dt_free(filename);
  return go_on;
}

static GList *_crawler_run(dt_job_t *job)
{
  GList *result = NULL;
  dt_crawler_walk_t walk
      = { .result = &result,
          .folders = g_hash_table_new_full(g_str_hash, g_str_equal,
                                           dt_free_gpointer, _free_folder),
          .job = job };

  /* NO transaction around this walk, deliberately -- it used to carry one, inherited from the
   * days when the crawl ran before the main window existed and nothing else could touch the
   * database. It cannot stay now that this runs as a background job:
   *
   *  - dt_database_start_transaction() takes the module-wide _db_lock as a WRITER and holds it
   *    until the matching release, so every other thread that opens a transaction -- the GUI
   *    thread does so constantly -- would block for the whole crawl;
   *  - the module owns ONE sqlite3 connection, and a transaction belongs to the connection
   *    rather than to the thread, so any statement the GUI thread issues outside a transaction
   *    of its own would silently execute inside OURS: not durable until we commit, and gone if
   *    anything rolled us back;
   *  - and what it spanned is not database work at all. _crawler_folder() lists a directory
   *    from inside the callback, so the lock would be held across every filesystem round-trip
   *    -- 1.1 s on the measured SMB share, and the full mount timeout when a share is gone.
   *
   * Nothing is lost by dropping it. The walk is a read; the only writes are the rare
   * dt_image_repository_set_flags_masked() calls for an image whose .txt/.wav sibling appeared or
   * disappeared, and the database runs `synchronous = OFF` with `journal_mode = MEMORY`
   * (dt_database_open), so a commit costs no disk sync and batching them buys nothing.
   */
  dt_image_repository_foreach_with_path(_crawl_image, &walk);

  g_hash_table_destroy(walk.folders);

  return g_list_reverse(result); // list was built in reverse order, so un-reverse it
}

GList *dt_control_crawler_run(void)
{
  return _crawler_run(NULL);
}

/* The crawl is I/O-latency bound and its cost scales with the library, so it does not belong
 * on the startup path at all: it used to run to completion before dt_control_init(), i.e.
 * before the main window was built. It runs as a background job instead, and posts its popup
 * to the GUI thread if and when it finds anything.
 */
static gboolean _crawler_show_results(gpointer user_data)
{
  // takes ownership of the list and frees it
  dt_control_crawler_show_image_list((GList *)user_data);
  return G_SOURCE_REMOVE;
}

static int32_t _crawler_job_run(dt_job_t *job)
{
  GList *changed_xmp_files = _crawler_run(job);

  /* A cancelled walk stopped part-way, so its list is a fraction of the answer -- and after a
   * quit, the main loop that would show it is on its way out. Neither is worth a popup. */
  if(_job_cancelled(job))
  {
    _free_crawler_results(changed_xmp_files);
    return 0;
  }

  // the popup is GTK and this runs on a worker thread
  if(!IS_NULL_PTR(changed_xmp_files))
    g_main_context_invoke(NULL, _crawler_show_results, changed_xmp_files);

  return 0;
}

void dt_control_crawler_run_in_background(void)
{
  /* dt_control_add_job() runs a job synchronously on the calling thread when the scheduler is
   * not up, and reports that as success -- which is the one outcome this function exists to
   * avoid. No crawl at all is the right answer then: it is a consistency check between the
   * database and the sidecars, not something a session depends on. dt_init() calls this well
   * after dt_control_init() starts the workers, so it cannot fire today; this keeps that
   * ordering from being a silent precondition. */
  if(!dt_control_running()) return;

  dt_job_t *job = dt_control_job_create(&_crawler_job_run, "crawl XMP files");
  if(IS_NULL_PTR(job)) return;

  // SYSTEM_BG, not SYSTEM_FG: the queue a job may not be pushed back out of. Dropping the
  // crawl would leave the database out of sync with the sidecars with nothing said about it.
  dt_control_add_job(dt_control_get_global(), DT_JOB_QUEUE_SYSTEM_BG, job);
}


/********************* the gui stuff *********************/

typedef struct dt_control_crawler_gui_t
{
  GtkTreeView *tree;
  GtkTreeModel *model;
  GtkWidget *log;
  GtkWidget *spinner;
  GList *rows_to_remove;
} dt_control_crawler_gui_t;

// close the window and clean up
static void dt_control_crawler_response_callback(GtkWidget *dialog,
                                                 const gint response_id,
                                                 gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  g_object_unref(G_OBJECT(gui->model));
  gtk_widget_destroy(dialog);
  dt_free(gui);
}


static void _delete_selected_rows(dt_control_crawler_gui_t *gui)
{
  GList *rr_list = gui->rows_to_remove;
  GtkTreeModel *model = gui->model;

  // Remove TreeView rows from rr_list. It needs to be populated before
  for(GList *node = rr_list; !IS_NULL_PTR(node); node = g_list_next(node))
  {
    GtkTreePath *path = gtk_tree_row_reference_get_path((GtkTreeRowReference*)node->data);

    if(path)
    {
      GtkTreeIter  iter;
      if(gtk_tree_model_get_iter(model, &iter, path))
        gtk_list_store_remove(GTK_LIST_STORE(model), &iter);
    }
  }

  // Cleanup the list of rows
  g_list_foreach(rr_list, (GFunc) gtk_tree_row_reference_free, NULL);
  g_list_free(rr_list);
  rr_list = NULL;
}


static void _select_all_callback(GtkButton *button,
                                 gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gtk_tree_selection_select_all(selection);
}


static void _select_none_callback(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gtk_tree_selection_unselect_all(selection);
}


static void _select_invert_callback(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);

  GtkTreeIter iter;
  gboolean valid = gtk_tree_model_get_iter_first(gui->model, &iter);
  while(valid)
  {
    if(gtk_tree_selection_iter_is_selected(selection, &iter))
      gtk_tree_selection_unselect_iter(selection, &iter);
    else
      gtk_tree_selection_select_iter(selection, &iter);

    valid = gtk_tree_model_iter_next(gui->model, &iter);
  }
}


static void _get_crawler_entry_from_model(GtkTreeModel *model,
                                          GtkTreeIter *iter,
                                          dt_control_crawler_result_t *entry)
{
  gtk_tree_model_get(model, iter,
                     DT_CONTROL_CRAWLER_COL_IMAGE_PATH, &entry->image_path,
                     DT_CONTROL_CRAWLER_COL_ID,         &entry->id,
                     DT_CONTROL_CRAWLER_COL_XMP_PATH,   &entry->xmp_path,
                     DT_CONTROL_CRAWLER_COL_TS_DB_INT,  &entry->timestamp_db,
                     DT_CONTROL_CRAWLER_COL_TS_XMP_INT, &entry->timestamp_xmp, -1);
}


static void _append_row_to_remove(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GList **rowref_list)
{
  // append TreeModel rows to the list to remove
  GtkTreeRowReference *rowref = gtk_tree_row_reference_new(model, path);
  *rowref_list = g_list_append(*rowref_list, rowref);
}

static void _log_synchronization(dt_control_crawler_gui_t *gui,
                                 gchar *pattern,
                                 gchar *filepath)
{
  gchar *message = pattern;
  gboolean to_free = FALSE;

  if(!IS_NULL_PTR(filepath))
  {
    message = g_strdup_printf(pattern, filepath);
    to_free = TRUE;
  }

  // add a new line in the log TreeView
  GtkTreeIter iter_log;
  GtkTreeModel *model_log = gtk_tree_view_get_model(GTK_TREE_VIEW(gui->log));
  gtk_list_store_append(GTK_LIST_STORE(model_log), &iter_log);
  gtk_list_store_set(GTK_LIST_STORE(model_log), &iter_log,
                     0, message,
                     -1);

  if(to_free)
  {
    dt_free(message);
  }
}


static void sync_xmp_to_db(GtkTreeModel *model,
                           GtkTreePath *path,
                           GtkTreeIter *iter,
                           gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { 0 };
  _get_crawler_entry_from_model(model, iter, &entry);
  // the DB writing timestamp becomes the XMP file's
    dt_image_repository_set_write_timestamp(entry.id, entry.timestamp_xmp);

  const int error =
    dt_history_load_and_apply_on_image(entry.id, entry.xmp_path, 0);  // success = 0, fail = 1

  if(error)
  {
    _log_synchronization(gui, _("ERROR: %s NOT synced XMP \342\206\222 DB"), entry.image_path);
    _log_synchronization(gui, _("ERROR: cannot write the database."
                                " the destination may be full, offline or read-only."),
                         NULL);
  }
  else
  {
    _append_row_to_remove(model, path, &gui->rows_to_remove);
    _log_synchronization(gui, _("SUCCESS: %s synced XMP \342\206\222 DB"), entry.image_path);
  }

  _free_crawler_result(&entry);
}


static void sync_db_to_xmp(GtkTreeModel *model,
                           GtkTreePath *path,
                           GtkTreeIter *iter,
                           gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { 0 };
  _get_crawler_entry_from_model(model, iter, &entry);

  const dt_image_write_sidecar_result_t result = dt_image_write_sidecar_file_forced(entry.id);

  if(result == DT_IMAGE_WRITE_SIDECAR_OK)
  {
    _set_modification_time(entry.xmp_path, entry.timestamp_db);
    _append_row_to_remove(model, path, &gui->rows_to_remove);
    _log_synchronization(gui, _("SUCCESS: %s synced DB \342\206\222 XMP"), entry.image_path);
  }
  else
  {
    _log_synchronization(gui, _("ERROR: %s NOT synced DB \342\206\222 XMP"), entry.image_path);
    _log_synchronization(gui,
                         _("ERROR: cannot write %s \nthe destination may be full,"
                           " offline or read-only."), entry.xmp_path);
  }

  _free_crawler_result(&entry);
}

static void sync_newest_to_oldest(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GtkTreeIter *iter,
                                  gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { 0 };
  _get_crawler_entry_from_model(model, iter, &entry);

  int error = 0;

  if(entry.timestamp_xmp > entry.timestamp_db)
  {
    // WRITE XMP in DB
    // the DB writing timestamp becomes the XMP file's
    dt_image_repository_set_write_timestamp(entry.id, entry.timestamp_xmp);
    error = dt_history_load_and_apply_on_image(entry.id, entry.xmp_path, 0);
    if(error)
    {
      _log_synchronization
        (gui,
         _("ERROR: %s NOT synced new (XMP) \342\206\222 old (DB)"), entry.image_path);
      _log_synchronization
        (gui,
         _("ERROR: cannot write the database. the destination may be full,"
           " offline or read-only."), NULL);
    }
    else
    {
      _log_synchronization
        (gui,
         _("SUCCESS: %s synced new (XMP) \342\206\222 old (DB)"), entry.image_path);
    }
  }
  else if(entry.timestamp_xmp < entry.timestamp_db)
  {
    // write the XMP and make sure it get the last modified timestamp of the db
    const dt_image_write_sidecar_result_t xres = dt_image_write_sidecar_file_forced(entry.id);
    error = (xres != DT_IMAGE_WRITE_SIDECAR_OK) ? 1 : 0;
    if(!error) _set_modification_time(entry.xmp_path, entry.timestamp_db);

    if(error)
    {
      _log_synchronization
        (gui,
         _("ERROR: %s NOT synced new (DB) \342\206\222 old (XMP)"), entry.image_path);
      _log_synchronization
        (gui,
         _("ERROR: cannot write %s \nthe destination may be full, offline or read-only."),
         entry.xmp_path);
    }
    else
    {
      _log_synchronization(gui, _("SUCCESS: %s synced new (DB) \342\206\222 old (XMP)"),
                           entry.image_path);
    }
  }
  else
  {
    // we should never reach that part of the code
    // if both timestamps are equal, they should not be in this list in the first place
    error = 1;
    _log_synchronization(gui, _("EXCEPTION: %s has inconsistent timestamps"),
                         entry.image_path);
  }

  if(!error) _append_row_to_remove(model, path, &gui->rows_to_remove);

  _free_crawler_result(&entry);
}


static void sync_oldest_to_newest(GtkTreeModel *model,
                                  GtkTreePath *path,
                                  GtkTreeIter *iter,
                                  gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  dt_control_crawler_result_t entry = { 0 };
  _get_crawler_entry_from_model(model, iter, &entry);
  int error = 0;

  if(entry.timestamp_xmp < entry.timestamp_db)
  {
    // WRITE XMP in DB
    // the DB writing timestamp becomes the XMP file's
    dt_image_repository_set_write_timestamp(entry.id, entry.timestamp_xmp);
    error = dt_history_load_and_apply_on_image(entry.id, entry.xmp_path, 0);
    if(error)
    {
      _log_synchronization(gui,
                           _("ERROR: %s NOT synced old (XMP) \342\206\222 new (DB)"),
                           entry.image_path);
    _log_synchronization(gui,
                         _("ERROR: cannot write the database."
                           " the destination may be full, offline or read-only."), NULL);
    }
    else
    {
      _log_synchronization(gui,
                           _("SUCCESS: %s synced old (XMP) \342\206\222 new (DB)"),
                           entry.image_path);
    }
  }
  else if(entry.timestamp_xmp > entry.timestamp_db)
  {
    // WRITE DB in XMP
    const dt_image_write_sidecar_result_t xres = dt_image_write_sidecar_file_forced(entry.id);
    error = (xres != DT_IMAGE_WRITE_SIDECAR_OK) ? 1 : 0;
    if(!error) _set_modification_time(entry.xmp_path, entry.timestamp_db);
    if(error)
    {
      _log_synchronization(gui,
                           _("ERROR: %s NOT synced old (DB) \342\206\222 new (XMP)"),
                           entry.image_path);
      _log_synchronization(gui,
                           _("ERROR: cannot write %s \nthe destination may be full,"
                             " offline or read-only."), entry.xmp_path);
    }
    else
    {
      _log_synchronization(gui,
                           _("SUCCESS: %s synced old (DB) \342\206\222 new (XMP)"),
                           entry.image_path);
    }
  }
  else
  {
    // we should never reach that part of the code
    // if both timestamps are equal, they should not be in this list in the first place
    error = 1;
    _log_synchronization(gui,
                         _("EXCEPTION: %s has inconsistent timestamps"),
                         entry.image_path);
  }

  if(!error)
    _append_row_to_remove(model, path, &gui->rows_to_remove);

  _free_crawler_result(&entry);
}

// overwrite database with xmp
static void _reload_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_xmp_to_db, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite xmp with database
void _overwrite_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_db_to_xmp, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite the oldest with the newest
static void _newest_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_newest_to_oldest, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

// overwrite the newest with the oldest
static void _oldest_button_clicked(GtkButton *button, gpointer user_data)
{
  dt_control_crawler_gui_t *gui = (dt_control_crawler_gui_t *)user_data;
  GtkTreeSelection *selection = gtk_tree_view_get_selection(gui->tree);
  gui->rows_to_remove = NULL;
  gtk_spinner_start(GTK_SPINNER(gui->spinner));
  gtk_tree_selection_selected_foreach(selection, sync_oldest_to_newest, gui);
  _delete_selected_rows(gui);
  gtk_spinner_stop(GTK_SPINNER(gui->spinner));
}

static gchar* str_time_delta(const int time_delta)
{
  // display the time difference as a legible string
  int seconds = time_delta;

  int minutes = seconds / 60;
  seconds -= 60 * minutes;

  int hours = minutes / 60;
  minutes -= 60 * hours;

  const int days = hours / 24;
  hours -= 24 * days;

  return g_strdup_printf(_("%id %02dh %02dm %02ds"), days, hours, minutes, seconds);
}

// show a popup window with a list of updated images/xmp files and allow the user to tell dt what to do about them
void dt_control_crawler_show_image_list(GList *images)
{
  if(IS_NULL_PTR(images)) return;

  dt_control_crawler_gui_t *gui =
    (dt_control_crawler_gui_t *)malloc(sizeof(dt_control_crawler_gui_t));

  // a list with all the images
  GtkTreeViewColumn *column;
  GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
  gtk_widget_set_vexpand(scroll, TRUE);
  GtkListStore *store = gtk_list_store_new(DT_CONTROL_CRAWLER_NUM_COLS,
                                           G_TYPE_INT,    // id
                                           G_TYPE_STRING, // image path
                                           G_TYPE_STRING, // xmp path
                                           G_TYPE_STRING, // timestamp from xmp
                                           G_TYPE_STRING, // timestamp from db
                                           G_TYPE_INT,    // timestamp to db
                                           G_TYPE_INT,
                                           G_TYPE_STRING, // report: newer version
                                           G_TYPE_STRING);// time delta

  gui->model = GTK_TREE_MODEL(store);

  for(GList *list_iter = images; list_iter; list_iter = g_list_next(list_iter))
  {
    GtkTreeIter iter;
    dt_control_crawler_result_t *item = list_iter->data;
    char timestamp_db[64], timestamp_xmp[64];
    struct tm tm_stamp;
    strftime(timestamp_db, sizeof(timestamp_db),
             "%c", localtime_r(&item->timestamp_db, &tm_stamp));
    strftime(timestamp_xmp, sizeof(timestamp_xmp),
             "%c", localtime_r(&item->timestamp_xmp, &tm_stamp));

    const time_t time_delta = llabs(item->timestamp_db - item->timestamp_xmp);
    gchar *timestamp_delta = str_time_delta(time_delta);

    gtk_list_store_append(store, &iter);
    gtk_list_store_set
      (store, &iter,
       DT_CONTROL_CRAWLER_COL_ID, item->id,
       DT_CONTROL_CRAWLER_COL_IMAGE_PATH, item->image_path,
       DT_CONTROL_CRAWLER_COL_XMP_PATH, item->xmp_path,
       DT_CONTROL_CRAWLER_COL_TS_XMP, timestamp_xmp,
       DT_CONTROL_CRAWLER_COL_TS_DB, timestamp_db,
       DT_CONTROL_CRAWLER_COL_TS_XMP_INT, item->timestamp_xmp,
       DT_CONTROL_CRAWLER_COL_TS_DB_INT, item->timestamp_db,
       DT_CONTROL_CRAWLER_COL_REPORT, (item->timestamp_xmp > item->timestamp_db)
                                      ? _("XMP")
                                      : _("database"),
       DT_CONTROL_CRAWLER_COL_TIME_DELTA, timestamp_delta,
       -1);
    _free_crawler_result(item);
    dt_free(timestamp_delta);
  }
  g_list_free_full(images, dt_free_gpointer);
  images = NULL;

  GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
  GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
  gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);

  gui->tree = GTK_TREE_VIEW(tree); // FIXME: do we need to free that later ?

  GtkCellRenderer *renderer_text = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("path"), renderer_text, "text",
     DT_CONTROL_CRAWLER_COL_IMAGE_PATH, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
  gtk_tree_view_column_set_expand(column, TRUE);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_column_set_min_width(column, DT_PIXEL_APPLY_DPI(200));
  g_object_set(renderer_text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);

  column = gtk_tree_view_column_new_with_attributes
    (_("XMP timestamp"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_TS_XMP, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes
    (_("database timestamp"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_TS_DB, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  column = gtk_tree_view_column_new_with_attributes
    (_("newest"), gtk_cell_renderer_text_new(), "text",
     DT_CONTROL_CRAWLER_COL_REPORT, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  GtkCellRenderer *renderer_date = gtk_cell_renderer_text_new();
  column = gtk_tree_view_column_new_with_attributes
    (_("time difference"), renderer_date, "text",
     DT_CONTROL_CRAWLER_COL_TIME_DELTA, NULL);
  g_object_set(renderer_date, "xalign", 1., NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);

  dt_gui_add_class(scroll, "dt_recessed_scroll");
  gtk_container_add(GTK_CONTAINER(scroll), tree);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  // build a dialog window that contains the list of images
  GtkWidget *win = dt_gui_main_window();
  GtkWidget *dialog = gtk_dialog_new_with_buttons
    (_("updated XMP sidecar files found"), GTK_WINDOW(win),
     GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, _("_close"),
     GTK_RESPONSE_CLOSE, NULL);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_widget_set_size_request(dialog, -1, DT_PIXEL_APPLY_DPI(400));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(win));
  GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));

  GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(content_area), content_box);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(content_box), box, FALSE, FALSE, 0);
  GtkWidget *select_all = gtk_button_new_with_label(_("select all"));
  GtkWidget *select_none = gtk_button_new_with_label(_("select none"));
  GtkWidget *select_invert = gtk_button_new_with_label(_("invert selection"));
  gtk_box_pack_start(GTK_BOX(box), select_all, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), select_none, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), select_invert, FALSE, FALSE, 0);
  g_signal_connect(select_all, "clicked", G_CALLBACK(_select_all_callback), gui);
  g_signal_connect(select_none, "clicked", G_CALLBACK(_select_none_callback), gui);
  g_signal_connect(select_invert, "clicked", G_CALLBACK(_select_invert_callback), gui);

  gtk_box_pack_start(GTK_BOX(content_box), scroll, TRUE, TRUE, 0);

  box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(content_box), box, FALSE, FALSE, 1);
  GtkWidget *label = gtk_label_new_with_mnemonic(_("on the selection:"));
  GtkWidget *reload_button = gtk_button_new_with_label(_("keep the XMP edit"));
  GtkWidget *overwrite_button = gtk_button_new_with_label(_("keep the database edit"));
  GtkWidget *newest_button = gtk_button_new_with_label(_("keep the newest edit"));
  GtkWidget *oldest_button = gtk_button_new_with_label(_("keep the oldest edit"));
  gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), reload_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), overwrite_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), newest_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(box), oldest_button, FALSE, FALSE, 0);
  g_signal_connect(reload_button, "clicked", G_CALLBACK(_reload_button_clicked), gui);
  g_signal_connect(overwrite_button, "clicked", G_CALLBACK(_overwrite_button_clicked), gui);
  g_signal_connect(newest_button, "clicked", G_CALLBACK(_newest_button_clicked), gui);
  g_signal_connect(oldest_button, "clicked", G_CALLBACK(_oldest_button_clicked), gui);

  /* Feedback spinner in case synch happens over network and stales */
  gui->spinner = gtk_spinner_new();
  gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(gui->spinner), FALSE, FALSE, 0);

  /* Log report */
  scroll = gtk_scrolled_window_new(NULL, NULL);
  gui->log = gtk_tree_view_new();
  gtk_box_pack_start(GTK_BOX(content_box), scroll, TRUE, TRUE, 0);
  dt_gui_add_class(scroll, "dt_recessed_scroll");
  gtk_container_add(GTK_CONTAINER(scroll), gui->log);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

  gtk_tree_view_insert_column_with_attributes
    (GTK_TREE_VIEW(gui->log), -1,
     _("synchronization log"), renderer_text,
     "text", 0, NULL);

  GtkListStore *store_log = gtk_list_store_new (1, G_TYPE_STRING);
  GtkTreeModel *model_log = GTK_TREE_MODEL(store_log);
  gtk_tree_view_set_model(GTK_TREE_VIEW(gui->log), model_log);
  g_object_unref(model_log);

  gtk_widget_show_all(dialog);

  g_signal_connect(dialog, "response",
                   G_CALLBACK(dt_control_crawler_response_callback), gui);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
