/*
    This file is part of darktable,
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010-2011, 2013 johannes hanika.
    Copyright (C) 2012 José Carlos García Sogo.
    Copyright (C) 2012, 2018, 2020 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Simon Spannagel.
    Copyright (C) 2013 Gaspard Jankowiak.
    Copyright (C) 2013-2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2013 Ulrich Pegelow.
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2015-2016 Jérémy Rosen.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2016, 2020-2022 Aldric Renaudin.
    Copyright (C) 2016 itinerarium.
    Copyright (C) 2017 luzpaz.
    Copyright (C) 2018 Mario Lueder.
    Copyright (C) 2018 Rick Yorgason.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2018 Sam Smith.
    Copyright (C) 2018 Simon Legner.
    Copyright (C) 2019 rrd1.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2020-2021 Philippe Weyland.
    Copyright (C) 2021 Arnaud TANGUY.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022-2023, 2025 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    
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

#ifndef DT_COMMON_COLLECTION_H
#define DT_COMMON_COLLECTION_H

#include <glib.h>
#include <glib/gi18n.h>
#include <inttypes.h>
#include "metadata/metadata.h"

#define NUM_LAST_COLLECTIONS 10

/* Recording the N most recently used collections is a GUI feature: the list exists to fill a
 * menu, and nothing in the collection backend reads it back. It lived here only because this
 * is where a collection change is noticed.
 *
 * It must run BEFORE DT_SIGNAL_COLLECTION_CHANGED is raised -- some listeners read the recents
 * list -- so it cannot simply become another listener. The backend therefore calls the handler
 * at exactly the point the old code did. */
typedef void (*dt_collection_recents_handler_t)(void);

/** Install the handler that records the current collection as recently used. NULL removes it. */
void dt_collection_set_recents_handler(dt_collection_recents_handler_t handler);

typedef enum dt_collection_query_flags_t
{
  COLLECTION_QUERY_SIMPLE             = 0,      // a query with only select and where statement
  COLLECTION_QUERY_USE_SORT           = 1 << 0, // if query should include order by statement
  COLLECTION_QUERY_USE_LIMIT          = 1 << 1, // if query should include "limit ?1,?2" part
  COLLECTION_QUERY_USE_WHERE_EXT      = 1 << 2, // if query should include extended where part
  COLLECTION_QUERY_USE_ONLY_WHERE_EXT = 1 << 3  // if query should only use extended where part
} dt_collection_query_flags_t;
#define COLLECTION_QUERY_FULL (COLLECTION_QUERY_USE_SORT | COLLECTION_QUERY_USE_LIMIT)

typedef enum dt_collection_filter_flag_t
{
  COLLECTION_FILTER_NONE            = 0,
  COLLECTION_FILTER_ALTERED         = 1 << 0, // altered images
  COLLECTION_FILTER_UNALTERED       = 1 << 1, // unaltered images
  COLLECTION_FILTER_REJECTED        = 1 << 2, // rejected images
  COLLECTION_FILTER_0_STAR          = 1 << 3,
  COLLECTION_FILTER_1_STAR          = 1 << 4,
  COLLECTION_FILTER_2_STAR          = 1 << 5,
  COLLECTION_FILTER_3_STAR          = 1 << 6,
  COLLECTION_FILTER_4_STAR          = 1 << 7,
  COLLECTION_FILTER_5_STAR          = 1 << 8,
  COLLECTION_FILTER_RED             = 1 << 9,
  COLLECTION_FILTER_YELLOW          = 1 << 10,
  COLLECTION_FILTER_GREEN           = 1 << 11,
  COLLECTION_FILTER_BLUE            = 1 << 12,
  COLLECTION_FILTER_MAGENTA         = 1 << 13,
  COLLECTION_FILTER_WHITE           = 1 << 14, // white means "no color label"
  COLLECTION_FILTER_ALL             = (1 << 15) - 1, // all 15 defined flags
} dt_collection_filter_flag_t;

typedef enum dt_collection_sort_t
{
  DT_COLLECTION_SORT_NONE     = -1,
  DT_COLLECTION_SORT_FILENAME = 0,
  DT_COLLECTION_SORT_DATETIME,
  DT_COLLECTION_SORT_IMPORT_TIMESTAMP,
  DT_COLLECTION_SORT_CHANGE_TIMESTAMP,
  DT_COLLECTION_SORT_EXPORT_TIMESTAMP,
  DT_COLLECTION_SORT_PRINT_TIMESTAMP,
  DT_COLLECTION_SORT_RATING,
  DT_COLLECTION_SORT_ID,
  DT_COLLECTION_SORT_COLOR,
  DT_COLLECTION_SORT_GROUP,
  DT_COLLECTION_SORT_PATH,
  DT_COLLECTION_SORT_TITLE
} dt_collection_sort_t;

#define DT_COLLECTION_ORDER_FLAG 0x8000

/* NOTE: any reordeing in this module require a legacy_preset entry in src/libs/collect.c */
typedef enum dt_collection_properties_t
{
  DT_COLLECTION_PROP_FILMROLL = 0,
  DT_COLLECTION_PROP_FOLDERS,
  DT_COLLECTION_PROP_FILENAME,

  DT_COLLECTION_PROP_CAMERA,
  DT_COLLECTION_PROP_LENS,
  DT_COLLECTION_PROP_APERTURE,
  DT_COLLECTION_PROP_EXPOSURE,
  DT_COLLECTION_PROP_FOCAL_LENGTH,
  DT_COLLECTION_PROP_ISO,

  DT_COLLECTION_PROP_DAY,
  DT_COLLECTION_PROP_TIME,
  DT_COLLECTION_PROP_IMPORT_TIMESTAMP,
  DT_COLLECTION_PROP_CHANGE_TIMESTAMP,
  DT_COLLECTION_PROP_EXPORT_TIMESTAMP,
  DT_COLLECTION_PROP_PRINT_TIMESTAMP,

  DT_COLLECTION_PROP_GEOTAGGING,
  DT_COLLECTION_PROP_TAG,
  DT_COLLECTION_PROP_COLORLABEL,
  DT_COLLECTION_PROP_METADATA,
  DT_COLLECTION_PROP_GROUPING = DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER,
  DT_COLLECTION_PROP_LOCAL_COPY,

  DT_COLLECTION_PROP_HISTORY,
  DT_COLLECTION_PROP_MODULE,
  DT_COLLECTION_PROP_ORDER,
  DT_COLLECTION_PROP_RATING,

  DT_COLLECTION_PROP_QUERY, // raw user-provided SQL WHERE expression (advanced)

  DT_COLLECTION_PROP_LAST,

  DT_COLLECTION_PROP_UNDEF,
  DT_COLLECTION_PROP_SORT
} dt_collection_properties_t;

typedef enum dt_collection_change_t
{
  DT_COLLECTION_CHANGE_NONE            = 0,
  DT_COLLECTION_CHANGE_NEW_QUERY       = 1, // a completly different query
  DT_COLLECTION_CHANGE_FILTER          = 2, // base query has been finetuned (filter, ...)
  DT_COLLECTION_CHANGE_RELOAD          = 3, // we have just reload the collection after images changes (query is identical)
  // Something changed elsewhere (e.g. an import landing outside the browsed folder) that
  // listeners with their own independent state may care about, but that is NOT a navigational
  // event: nothing about what the user is currently looking at changed. Listeners driven by user
  // navigation (scroll/focus/zoom resets) should treat this the same as NONE and do nothing;
  // listeners that keep their own counts (e.g. the Collect module's tag/camera/lens lists) should
  // still refresh. Deliberately its own value rather than reusing NONE: NONE already means "no
  // value" in non-signal contexts (see gui/actions/file.c's one-time init call), and overloading
  // it with this second, signal-specific meaning would make every future listener re-derive which
  // sense of NONE it is looking at.
  DT_COLLECTION_CHANGE_BACKGROUND_SYNC = 4
} dt_collection_change_t;

/** One rule of a collection: "images whose <property> <mode> matches <text>".
 *
 *  This is what crosses into the database module. The module turns rules into SQL; reading them
 *  out of the user's configuration is common/collection.c's. */
typedef struct dt_collection_rule_t
{
  dt_collection_properties_t property;
  int mode;              /**< 0 = AND, 1 = OR, 2 = AND NOT */
  const char *text;      /**< empty or NULL means "match everything" */
  gboolean recursive;    /**< for the path-like properties: match below the value too */
} dt_collection_rule_t;

typedef struct dt_collection_params_t
{
  /** flags for which query parts to use, see COLLECTION_QUERY_x defines... */
  dt_collection_query_flags_t query_flags;

  /** flags for which filters to use, see COLLECTION_FILTER_x defines... */
  dt_collection_filter_flag_t filter_flags;

  /** text filter */
  char *text_filter;

  /** sorting **/
  dt_collection_sort_t sort; // Has to be changed to a dt_collection_sort struct
  gint descending;

} dt_collection_params_t;

typedef struct dt_collection_t
{
  dt_collection_rule_t *rules;   /**< the user's rules; SQL is composed from these downstream */
  int n_rules;
  unsigned int count;
  unsigned int tagid;
  dt_collection_params_t params;
  dt_collection_params_t store;
} dt_collection_t;

/* returns the name for the given collection property */
const char *dt_collection_name(dt_collection_properties_t prop);

/** instantiates a collection context */
// Interim accessor (Strategy B, doc/globals-migration.md): implemented by the orchestrator; long-term the handle should be carried on the job/view context (Strategy C).
dt_collection_t *dt_collection_get_global(void);

dt_collection_t *dt_collection_new();
/** frees a collection context. */
void dt_collection_free(const dt_collection_t *collection);
/** fetch params for collection for storing. */
const dt_collection_params_t *dt_collection_params(const dt_collection_t *collection);
/** get the filtered map between sanitized makermodel and exif maker/model **/
void dt_collection_get_makermodels(const gchar *filter, GList **sanitized, GList **exif);
/** get the sanitized makermodel for exif maker/model **/
gchar *dt_collection_get_makermodel(const char *exif_maker, const char *exif_model);
/** updates sql query for a collection. @return 1 if query changed. */
int dt_collection_update(const dt_collection_t *collection);
/** reset collection to default dummy selection */
void dt_collection_reset(const dt_collection_t *collection);
/** gets an extended where part */
/** sets an extended where part */
/** Replace the collection's rules and rebuild its query. */
void dt_collection_set_rules(const dt_collection_t *collection, const dt_collection_rule_t *rules,
                             const int n_rules);

/** get filter flags for collection */
dt_collection_filter_flag_t dt_collection_get_filter_flags(const dt_collection_t *collection);
/** set filter flags for collection */
void dt_collection_set_filter_flags(const dt_collection_t *collection, dt_collection_filter_flag_t flags);

/** get filter flags for collection */
dt_collection_query_flags_t dt_collection_get_query_flags(const dt_collection_t *collection);
/** set filter flags for collection */
void dt_collection_set_query_flags(const dt_collection_t *collection, dt_collection_query_flags_t flags);

/** get text filter for collection */
char *dt_collection_get_text_filter(const dt_collection_t *collection);
/** set text filter for collection */
void dt_collection_set_text_filter(const dt_collection_t *collection, char *text_filter);

/** set the tagid of collection */
void dt_collection_set_tag_id(dt_collection_t *collection, const uint32_t tagid);

/** load a filmroll-based collection from an imgid. set_mouse_over controls whether
 * dt_control_set_mouse_over_id(imgid) is (re-)applied here: pass FALSE when the caller already
 * pointed mouse_over_id at imgid earlier and does not want it forced back onto imgid here,
 * clobbering whatever the user may be hovering by the time this runs. */
void dt_collection_load_filmroll(dt_collection_t *collection, const int32_t imgid, gboolean open_single_image,
                                 gboolean set_mouse_over);

/** If lighttable/Studio Capture is currently browsing a single folder or film-roll (Collect
 * module on the "Folders" tab), copy its path into folder (up to len bytes), set *recursive to
 * whether sub-folders are included (always FALSE for a film-roll: only the Tree/FOLDERS view
 * supports recursion), and return TRUE. Otherwise leave both outputs untouched and return
 * FALSE. */
gboolean dt_collection_get_browsed_folder(gchar *folder, size_t len, gboolean *recursive);

/** Shared notifier for import jobs that process images one at a time, throttled to at most once
 * every 250ms via *last_refresh_us (caller-owned state, zero-initialized before the import loop
 * starts). Re-reads dt_collection_get_browsed_folder() itself on every throttle-admitted call
 * (so at most 4/s, not once per whole import job) rather than trusting a snapshot the caller took
 * before the loop started: the user can change which folder is browsed while a long import is
 * still running, and a stale snapshot would keep comparing against wherever they were looking
 * when the job began.
 *
 * known_image_folder is an optional optimization: pass imgid's folder if the caller already has
 * it at hand (e.g. film_jobs.c's import loop, which never copies files, already knows it from
 * the film-roll it just inserted into) to skip re-deriving it here. Pass NULL to have it resolved
 * fresh via dt_get_dirname_from_imgid() -- necessary whenever the caller can't otherwise know the
 * final path, e.g. import_jobs.c's copy mode, which writes to a pattern-generated destination.
 *
 * If imgid's folder matches the browsed folder exactly, or falls under it and sub-folders are
 * included, this does a real dt_collection_update_query(), so the grid gains the new image -- or
 * no folder is being browsed at all, meaning relevance can't be determined, which takes the same
 * real-update path. Otherwise it raises DT_SIGNAL_COLLECTION_CHANGED directly, without
 * re-querying/rebuilding memory.collected_images: listeners that keep their own counts
 * independently of it (e.g. the Collect module's tag/camera/lens lists) still refresh, while the
 * center view -- keyed off the query generation and memory.collected_images's row count, both
 * left untouched on this path -- does not needlessly reload thumbnails.
 *
 * Either way the signal is labeled DT_COLLECTION_CHANGE_BACKGROUND_SYNC: a background import
 * heartbeat should never steal scroll/focus or reset grid/zoom preferences, whether or not it
 * touches what's on screen. Listeners that guard exactly that off query_change (thumbtable.c's
 * own collection-changed callback, libs/tools/lighttable.c's) recognize it as "not a real
 * [navigational] change" and skip just that side effect, not the parts driven by their own
 * hash/state. */
void dt_collection_notify_imported(const int32_t imgid, const gchar *known_image_folder, gint64 *last_refresh_us);

/** set the sort fields and flags used to show the collection **/
void dt_collection_set_sort(const dt_collection_t *collection, dt_collection_sort_t sort, gint reverse);
/** get the sort field used **/
dt_collection_sort_t dt_collection_get_sort_field(const dt_collection_t *collection);
/** get if the collection must be shown in descending order **/
gboolean dt_collection_get_sort_descending(const dt_collection_t *collection);
/** get the part of the query for sorting the collection **/
gchar *dt_collection_get_sort_query(const dt_collection_t *collection);

/** get the count of query */
uint32_t dt_collection_get_count(const dt_collection_t *collection);
/** get the nth image in the query */
int dt_collection_get_nth(const dt_collection_t *collection, int nth);
/** get all image ids order as current selection. no more than limit many images are returned, <0 ==
 * unlimited */
GList *dt_collection_get_all(const dt_collection_t *collection, int limit);

/** get the list of image ids matching a single (property, text, recursive) rule, independently of
 * the currently active collection. `recursive` only matters for DT_COLLECTION_PROP_FOLDERS.
 * Returns a GList of imgids (GINT_TO_POINTER), caller frees with g_list_free. Used by the library
 * module to feed batch/background operations. */
GList *dt_collection_get_images_for_rule(const dt_collection_properties_t property, const char *text,
                                         gboolean recursive);

/** One distinct value of a collection property, for the library module's value lists. */
typedef struct dt_collection_name_value_t
{
  char *name;  // raw/display value: folder path, tag name, formatted number/date, ...
  int id;      // film_roll id / tag id / running index, or 0
  int count;   // number of matching images
  int status;  // folder reachability (1 = reachable) for folders/film-rolls, else -1
} dt_collection_name_value_t;

/** Enumerate the distinct values of a collection property for the library module, honouring
 * all the *other* active rules through the extended-where of rule `rule`. The library module
 * then turns these into a flat list or a hierarchical tree. Returns a GList of
 * dt_collection_name_value_t*, free with g_list_free_full(list, dt_collection_name_value_free). */
GList *dt_collection_get_property_values(const dt_collection_properties_t property, const int rule);
void dt_collection_name_value_free(gpointer value);

/** update query by conf vars */
void dt_collection_update_query(const dt_collection_t *collection, dt_collection_change_t query_change,
                                dt_collection_properties_t changed_property, GList *list);

/** updates the hint message for collection */
void dt_collection_hint_message(const dt_collection_t *collection);

/* serialize and deserialize into a string. */
void dt_collection_deserialize(const char *buf);
int dt_collection_serialize(char *buf, int bufsize);

/* splits an input string into a number part and an optional operator part */
void dt_collection_split_operator_number(const gchar *input, char **number1, char **number2, char **op);
void dt_collection_split_operator_datetime(const gchar *input, char **number1, char **number2, char **op);
void dt_collection_split_operator_exposure(const gchar *input, char **number1, char **number2, char **op);

/* initialize memory table */
void dt_collection_memory_update();

/** restrict the collection to selected pictures */
void dt_selection_to_culling_mode();
/** restore initial collection and selection when exiting culling mode */
void dt_culling_mode_to_selection();

#endif // DT_COMMON_COLLECTION_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
