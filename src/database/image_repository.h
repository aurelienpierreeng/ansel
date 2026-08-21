/*
    This file is part of darktable,
    Copyright (C) 2025 Aurélien PIERRE.

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

/** @file database/image_repository.h
 *
 * @brief Reading and writing one ::dt_image_t to and from the library database. The SQL half
 * of what used to be `common/image_cache.c`.
 *
 * @details The image cache was two things wearing one name: an LRU of `dt_image_t` structs
 * with refcounting and per-entry locking, and the only code in the tree that knows the shape
 * of the `main.images` row. The second is not a cache, it is a repository -- 107 of the file's
 * lines were SQL -- and keeping them together meant every consumer of the cache also had a
 * translation unit that could reach the database.
 *
 * They are separate now. `caches/image_cache.c` holds the LRU and calls in here whenever it
 * needs a row read or written; this file holds every `sqlite3_*` call and the prepared
 * statements behind them, and knows nothing about caching, refcounting or eviction.
 *
 * @warning The statement cache below is process-wide and guarded by one mutex, so the calls
 * here serialise against each other. That is inherited behaviour, not a new constraint: the
 * cache took the same mutex around the same statements before the split.
 */

#ifndef DT_DATABASE_IMAGE_REPOSITORY_H
#define DT_DATABASE_IMAGE_REPOSITORY_H

#include "common/image.h"

#include <sqlite3.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

G_BEGIN_DECLS

/**
 * @brief Fill @p img from the `main.images` row for @p imgid.
 *
 * @param imgid image to read.
 * @param img destination. On failure its `id` is set to -1, which is what
 *        dt_image_invalid() tests, and the rest is left as the caller had it.
 * @return TRUE when a row was found and read.
 */
gboolean dt_image_repository_load(const int32_t imgid, dt_image_t *img);

/**
 * @brief Write @p img back to `main.images`, plus its colour labels and history hash.
 *
 * @details Three tables carry one image's state: the row itself, `main.color_labels`
 * (through dt_colorlabel_repository_set()/dt_colorlabel_repository_remove(), one call per
 * colour) and `main.history_hash`. A caller that wrote only the first would leave an image
 * whose labels and hash describe its previous contents.
 *
 * @param img image to persist. Ignored when NULL or when its `id` is not positive.
 */
void dt_image_repository_store(const dt_image_t *img);

/** @brief One image of the current collection. @p info is borrowed: it lives until the
 *  callback returns. */
typedef void (*dt_image_repository_collected_cb)(void *user_data, const dt_image_t *info);

/**
 * @brief Every image of the current collection, in collection order, already mapped.
 *
 * @details One bulk query instead of one image-cache miss per thumbnail, which is what keeps
 * scrolling a large collection predictable. It selects the same columns
 * dt_image_repository_load() does and maps them with the same private mapper, so the column
 * list and the mapper cannot drift apart -- they used to sit in two files, with the caller
 * running its own copy of the query and calling the mapper on each row.
 *
 * The statement is prepared and finalised around the walk rather than cached, and no internal
 * lock is held while the callback runs. Callbacks here reach the selection and the image
 * cache, either of which can raise a signal that lands back in this same function; a single
 * shared statement stepped re-entrantly would have its cursor pulled out from under the outer
 * walk. One prepare per collection reload is the price, and a collection reload is not a
 * per-frame event.
 */
void dt_image_repository_foreach_collected(dt_image_repository_collected_cb cb, void *user_data);

/* ---------------------------------------------------------------------------------------
 *  Grouping
 *
 *  `main.images.group_id`. Every image belongs to exactly one group, whose id is the id of
 *  one of its members; a lone image is its own group. These return and take plain image
 *  ids -- `common/grouping.c` keeps the rules (who leads a group, what happens to the rest
 *  when the leader leaves) and the image-cache updates that go with them.
 * ------------------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------------------
 *  Duplicating and copying a row, with everything that hangs off it
 *
 *  An image is one `main.images` row plus satellites in `main.color_labels`,
 *  `main.meta_data` and `main.tagged_images`. Copying one means copying all four, in one
 *  place, or the copy is silently partial -- which is why these are single calls rather
 *  than a set of per-table ones the caller would have to remember to make.
 *  (`main.module_order` is deliberately NOT among them: it travels with the history, which
 *  the caller copies next via dt_history_copy_and_paste_on_image().)
 * ------------------------------------------------------------------------------------- */

/**
 * @brief Duplicate @p imgid as a new version, returning the new image id (-1 on failure).
 *
 * @param newversion the version to give the duplicate, or -1 for "one past the current max".
 *        If that version already exists for this file, its id is returned and NOTHING is
 *        written -- the call is idempotent per version.
 *
 * @warning History is NOT copied. Callers that then paste history onto the duplicate must use
 *          dt_image_duplicate_no_reload() and issue their own collection reload once, after
 *          the paste -- otherwise the lighttable can render the duplicate in its momentary
 *          historyless state and cache that render.
 */
int32_t dt_image_repository_duplicate(const int32_t imgid, const int32_t newversion);

/** @brief One version of a file: which duplicate it is, its id, and the name the user gave it. */
typedef struct dt_image_version_t
{
  int version;
  int32_t imgid;
  char *version_name;   /**< NULL when the image has none */
} dt_image_version_t;

/** @brief Release one dt_image_version_t. Use with g_list_free_full(). */
void dt_image_version_free(gpointer data);

/**
 * @brief Every version of the file (@p film_id, @p filename), in version order, each with the
 *        name stored under metadata key @p name_keyid.
 *
 * @details Which key holds the version name is the caller's vocabulary, so it is passed in.
 * The whole list is materialised: the one caller builds a GTK widget per row and would
 * otherwise re-enter the database from inside its own open cursor.
 */
GList *dt_image_repository_get_versions(const int32_t film_id, const char *filename,
                                        const int name_keyid);

/**
 * @brief Every image id sharing @p imgid's film roll and filename, itself included, in row
 *        order -- an image and its duplicates.
 */
GList *dt_image_repository_get_duplicate_ids(const int32_t imgid);

/**
 * @brief Copy @p imgid into film roll @p filmid under @p new_filename, returning the new id.
 *
 * @details The database half of "copy an image to another folder": the row, its three satellite
 * tables, the version and max_version of every duplicate sharing the destination name, and the
 * group the copy joins. Moving the file and copying its history are the caller's.
 *
 * @param old_filename @p imgid's current filename, needed to tell the freshly inserted row
 *        apart from any pre-existing image of the same name in the destination roll.
 */
int32_t dt_image_repository_copy_to_film(const int32_t imgid, const int32_t filmid,
                                         const char *new_filename, const char *old_filename);

/* ---------------------------------------------------------------------------------------
 *  The two reads that write a sidecar
 *
 *  Both distinguish a NULL column from a zero one, which is why they carry `has_*` flags
 *  rather than a sentinel: 0 is a legitimate longitude and a legitimate timestamp.
 * ------------------------------------------------------------------------------------- */

/** @brief The `main.images` fields an XMP sidecar carries. */
typedef struct dt_image_xmp_row_t
{
  char *filename;              /**< copied out; release with dt_image_repository_xmp_row_cleanup() */
  int flags;
  int raw_parameters;
  int history_end;
  double longitude, latitude, altitude;
  gboolean has_longitude, has_latitude, has_altitude;
  int64_t datetime_taken;
} dt_image_xmp_row_t;

/** @brief Read @p imgid's sidecar fields. FALSE when there is no such image. */
gboolean dt_image_repository_get_xmp_row(const int32_t imgid, dt_image_xmp_row_t *row);

/** @brief Release what dt_image_repository_get_xmp_row() allocated. */
void dt_image_repository_xmp_row_cleanup(dt_image_xmp_row_t *row);

/** @brief The four timestamps, each with whether it is set at all. */
typedef struct dt_image_timestamps_t
{
  int64_t import_timestamp, change_timestamp, export_timestamp, print_timestamp;
  gboolean has_import, has_change, has_export, has_print;
} dt_image_timestamps_t;

/** @brief Read @p imgid's four timestamps. FALSE when there is no such image. */
gboolean dt_image_repository_get_timestamps(const int32_t imgid, dt_image_timestamps_t *ts);

/* ---------------------------------------------------------------------------------------
 *  Identity lookups
 *
 *  Finding the row for a file on disk. Both of these answer -1 for "no such image", which is
 *  what dt_image_invalid() tests.
 * ------------------------------------------------------------------------------------- */

/**
 * @brief The image id for @p filename inside film roll @p film_id, or -1.
 */
int32_t dt_image_repository_find_by_film_and_filename(const int32_t film_id, const char *filename);

/**
 * @brief The image id for a full path, split into its @p folder and @p filename, or -1.
 *
 * @details Joins `main.film_rolls` on its folder rather than taking a film id, because the
 * caller has a path and not a roll.
 */
int32_t dt_image_repository_find_by_folder_and_filename(const char *folder, const char *filename);


/* ---------------------------------------------------------------------------------------
 *  Versions, flags and the write timestamp
 * ------------------------------------------------------------------------------------- */

/** @brief The duplicate version number of @p imgid, or 0. */
int dt_image_repository_get_version(const int32_t imgid);

/** @brief Set both `version` and `max_version` of @p imgid to @p version. */
gboolean dt_image_repository_set_version(const int32_t imgid, const int version);

/**
 * @brief How many OTHER images share this image's film roll and filename and carry @p flag.
 *
 * @details Used to decide whether removing one local copy would orphan the others. Counts
 * rows other than @p imgid itself; answers 1 when the query cannot be read, which is the
 * conservative "assume someone else has one" the caller relied on.
 */
int dt_image_repository_count_others_with_flag(const int32_t imgid, const int flag);

/**
 * @brief Of the images in @p imgids, those whose `flags` carry @p flag, in row order.
 *
 * @details The id set is built into the statement as integers. Callers used to pass a
 * comma-joined STRING bound to a single `IN (?)` parameter, which SQLite reads as ONE value:
 * it matched a lone id and matched NOTHING for two or more.
 */
GList *dt_image_repository_get_ids_with_flag_among(GList *imgids, const int flag);

/** @brief Set @p flag on every image in @p imgids. */
gboolean dt_image_repository_set_flag_among(GList *imgids, const int flag);

/** @brief "<folder>/<filename>" for every image in @p imgids, distinct, in row order.
 *  Caller owns the strings. */
GList *dt_image_repository_get_full_paths(GList *imgids);

/** @brief Every image id whose `flags` carry @p flag, in row order. */
GList *dt_image_repository_get_ids_with_flag(const int flag);

/** @brief Replace @p imgid's whole `flags` word. */
gboolean dt_image_repository_set_flags(const int32_t imgid, const int flags);

/** @brief One row of dt_image_repository_foreach_with_path(). @p image_path is borrowed:
 *  it lives until the callback returns. */
typedef void (*dt_image_repository_path_row_cb)(const int32_t imgid,
                                                const int64_t write_timestamp,
                                                const int version,
                                                const char *image_path,
                                                const int flags,
                                                void *user_data);

/**
 * @brief Walk every image in the library, film roll by film roll, filename within each.
 *
 * @details Hands the callback what is needed to find an image's files on disk and compare
 * them against the row: its path, its version (for the sidecar's `_NN` suffix), the
 * `write_timestamp` a sidecar's mtime is checked against, and the `flags` word recording
 * which companion files it had last time.
 *
 * The statement is prepared and finalised around the walk rather than cached, and no
 * internal lock is held while the callback runs: the callback is expected to write back
 * through this same repository (dt_image_repository_set_flags()), and a cached statement
 * under the shared statement mutex would deadlock on that re-entry.
 */
void dt_image_repository_foreach_with_path(dt_image_repository_path_row_cb cb, void *user_data);

/* ---------------------------------------------------------------------------------------
 *  Geolocation
 *
 *  `main.images.latitude`/`longitude`, restricted to the current collection. The map view
 *  asks two questions of it: where to centre, and which images fall inside the viewport.
 * ------------------------------------------------------------------------------------- */

/** @brief The extent of the collection's geotagged images. */
typedef struct dt_image_geo_bounds_t
{
  double min_latitude, max_latitude;
  double min_longitude, max_longitude;
  int count;   /**< how many geotagged images the extent was computed from; 0 means none */
} dt_image_geo_bounds_t;

/** @brief Bounding box of every collected image carrying coordinates.
 *
 *  @return FALSE when the query cannot be read. @p bounds is seeded with an empty extent
 *  (mins at +INFINITY, maxes at -INFINITY, count 0) -- the caller's original locals -- so
 *  the numbers mean something ONLY when `count` is positive. A `count` of 0 means the
 *  collection has nothing geotagged, and the extent fields must not be read. */
gboolean dt_image_repository_get_collected_geo_bounds(dt_image_geo_bounds_t *bounds);

/** @brief One geotagged image, in degrees as stored. */
typedef struct dt_image_geo_point_t
{
  int32_t imgid;
  double longitude;
  double latitude;
} dt_image_geo_point_t;

/**
 * @brief Collected images inside the box, **ordered by longitude ascending**.
 *
 * @details The order is not cosmetic: the caller's DBSCAN clustering sweeps the array in
 * longitude order and would cluster differently without it.
 *
 * @param count out: how many points the array holds. Never NULL.
 * @return a newly allocated array of @p count points, or NULL when there are none.
 *         Free with dt_free().
 */
dt_image_geo_point_t *dt_image_repository_get_collected_geo_points(const double lon1, const double lon2,
                                                                   const double lat1, const double lat2,
                                                                   int *count);

/* ---------------------------------------------------------------------------------------
 *  Do these images agree?
 * ------------------------------------------------------------------------------------- */

/** Fields dt_image_repository_count_distinct_fields() reports on, IN THE ORDER IT RETURNS
 *  THEM. This enumeration is the contract: its one consumer is a metadata panel whose own row
 *  enumeration runs in the same order, and it checks the length it gets back against its own
 *  before reading a single value. Append here and there, never insert. */
typedef enum dt_image_field_t
{
  DT_IMAGE_FIELD_FILM_ROLL = 0,
  DT_IMAGE_FIELD_FILM_ROLL_ID,
  DT_IMAGE_FIELD_IMGID,          /**< always reported as differing: ids are unique by definition */
  DT_IMAGE_FIELD_GROUP_ID,
  DT_IMAGE_FIELD_FILENAME,
  DT_IMAGE_FIELD_VERSION,
  DT_IMAGE_FIELD_PATH,
  DT_IMAGE_FIELD_LOCAL_COPY,
  DT_IMAGE_FIELD_IMPORT_TIMESTAMP,
  DT_IMAGE_FIELD_CHANGE_TIMESTAMP,
  DT_IMAGE_FIELD_EXPORT_TIMESTAMP,
  DT_IMAGE_FIELD_PRINT_TIMESTAMP,
  DT_IMAGE_FIELD_FLAGS,
  DT_IMAGE_FIELD_MODEL,
  DT_IMAGE_FIELD_MAKER,
  DT_IMAGE_FIELD_LENS,
  DT_IMAGE_FIELD_APERTURE,
  DT_IMAGE_FIELD_EXPOSURE,
  DT_IMAGE_FIELD_EXPOSURE_BIAS,
  DT_IMAGE_FIELD_FOCAL_LENGTH,
  DT_IMAGE_FIELD_FOCUS_DISTANCE,
  DT_IMAGE_FIELD_ISO,
  DT_IMAGE_FIELD_DATETIME,
  DT_IMAGE_FIELD_WIDTH,
  DT_IMAGE_FIELD_HEIGHT,
  DT_IMAGE_FIELD_EXPORT_WIDTH,
  DT_IMAGE_FIELD_EXPORT_HEIGHT,
  DT_IMAGE_FIELD_META_TITLE,     /**< the eight main.meta_data keys, in the panel's display order */
  DT_IMAGE_FIELD_META_DESCRIPTION,
  DT_IMAGE_FIELD_META_CREATOR,
  DT_IMAGE_FIELD_META_PUBLISHER,
  DT_IMAGE_FIELD_META_RIGHTS,
  DT_IMAGE_FIELD_META_NOTES,
  DT_IMAGE_FIELD_META_VERSION_NAME,
  DT_IMAGE_FIELD_META_IMAGE_ID,
  DT_IMAGE_FIELD_LATITUDE,
  DT_IMAGE_FIELD_LONGITUDE,
  DT_IMAGE_FIELD_ALTITUDE,
  DT_IMAGE_FIELD_COUNT
} dt_image_field_t;

/**
 * @brief How many DISTINCT values each ::dt_image_field_t takes over @p imgids.
 *
 * @details One value per field, so a caller can grey out whatever the images disagree on. A
 * count above 1 means they disagree; 0 means nothing was read.
 *
 * The id set is built into the statement as integers, never bound to an `IN (?)` -- SQLite
 * reads a bound comma-joined string as ONE value, which matches a lone id and matches nothing
 * at all for two or more.
 *
 * @return a newly allocated array of ::DT_IMAGE_FIELD_COUNT ints, or NULL. Free with dt_free().
 */
int *dt_image_repository_count_distinct_fields(GList *imgids);

/** @brief `main.images.write_timestamp` for @p imgid, or 0. */
int64_t dt_image_repository_get_write_timestamp(const int32_t imgid);

/** @brief Set `write_timestamp` of @p imgid to now. */
void dt_image_repository_touch_write_timestamp(const int32_t imgid);

/** @brief Set `write_timestamp` of @p imgid to @p timestamp (seconds since the epoch).
 *  Bound as a 64-bit integer; the caller this replaced bound it as an `int`. */
gboolean dt_image_repository_set_write_timestamp(const int32_t imgid, const int64_t timestamp);

/**
 * @brief Delete @p imgid from `main.images` and `main.meta_data`.
 *
 * @details Foreign keys added in schema version 33 cascade the delete to every other table
 * referencing the image, so these two statements are the whole removal. `main.meta_data` is
 * listed explicitly because it predates the constraint.
 */
gboolean dt_image_repository_delete(const int32_t imgid);

/**
 * @brief Image ids in group @p group_id.
 *
 * @param exclude_imgid an image to leave out, or -1 to leave nothing out.
 * @return a `GList` of ids as `GINT_TO_POINTER`, in database order. Free with g_list_free().
 */
GList *dt_image_repository_get_group_members(const int32_t group_id, const int32_t exclude_imgid);

/** @brief One member of a group, with what a tooltip needs to name it. */
typedef struct dt_image_group_member_t
{
  int32_t imgid;
  int version;
  char *filename;
} dt_image_group_member_t;

/** @brief Free one dt_image_group_member_t. Suits g_list_free_full(). */
void dt_image_group_member_free(gpointer data);

/** @brief Every member of @p group_id, in row order, named. Free with
 *  g_list_free_full(list, dt_image_group_member_free). */
GList *dt_image_repository_get_group_member_rows(const int32_t group_id);

/** @brief How many images have an id in [@p min_imgid, @p max_imgid]. -1 when unreadable. */
int dt_image_repository_count_in_id_range(const int32_t min_imgid, const int32_t max_imgid);

/** @brief One row of dt_image_repository_foreach_in_id_range(). @p filename is borrowed. */
typedef void (*dt_image_repository_id_filename_cb)(const int32_t imgid, const char *filename,
                                                   void *user_data);

/** @brief Walk the images whose id falls in [@p min_imgid, @p max_imgid], in row order.
 *
 *  @details Same prepare-and-finalise, no-lock-held contract as
 *  dt_image_repository_foreach_collected(): its one caller generates a thumbnail per row,
 *  which goes back through the image and mipmap caches. */
void dt_image_repository_foreach_in_id_range(const int32_t min_imgid, const int32_t max_imgid,
                                             dt_image_repository_id_filename_cb cb,
                                             void *user_data);

/**
 * @brief Insert a bare row for a file being imported, returning TRUE on success.
 *
 * @details Everything but the identity is left at its default and filled in later from EXIF
 * and the sidecar. `position` takes the next free slot at the end of the collection order.
 * The row's id is not returned: the caller finds it with
 * dt_image_repository_find_by_film_and_filename(), which is what the original did.
 */
gboolean dt_image_repository_insert_import(const int32_t film_id, const char *filename,
                                           const int flags, const int64_t import_timestamp);

/**
 * @brief The group leader of an existing image in @p film_id matching @p filename_pattern
 *        (a LIKE pattern), or -1.
 *
 * @details Used at import to decide whether the file joins an existing group of same-basename
 * images. Two questions, one per caller: @p leader_only TRUE asks for a row that IS its own
 * group leader; FALSE asks for any row's group, excluding @p exclude_imgid.
 */
int32_t dt_image_repository_find_group_for_pattern(const int32_t film_id,
                                                   const char *filename_pattern,
                                                   const gboolean leader_only,
                                                   const int32_t exclude_imgid);

/** @brief Set @p imgid's `group_id`. */
gboolean dt_image_repository_set_group(const int32_t imgid, const int32_t group_id);


/** @brief Move every member of @p from_group_id into @p to_group_id, except @p exclude_imgid. */
void dt_image_repository_reassign_group(const int32_t from_group_id, const int32_t to_group_id,
                                        const int32_t exclude_imgid);

/**
 * @brief Star ratings, as `main.images.flags` encodes them: the low three bits, minus one.
 *
 * @param imgid the image, or a negative value for every selected image -- the convention
 *        dt_metadata_get() uses, which is this function's only caller.
 * @return a `GList` of ratings as `GINT_TO_POINTER`, one per image. Free with g_list_free().
 */
GList *dt_image_repository_get_ratings(const int32_t imgid);

/**
 * @brief Finalise the prepared statements. Called at shutdown, after the last cache release
 * and before the database connection closes.
 */
void dt_image_repository_cleanup(void);

G_END_DECLS


#ifdef __cplusplus
}
#endif

#endif // DT_DATABASE_IMAGE_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
