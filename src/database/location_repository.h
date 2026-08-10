/*
    This file is part of darktable,
    Copyright (C) 2026 Aurélien PIERRE.

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

/** @file database/location_repository.h
 *
 * @brief `data.locations`: the geometry of a geotagging location.
 *
 * @details A location is a **tag** under `darktable|locations|` plus one row here giving
 * its shape and extent, keyed by that tag's id. So `common/map_locations.c` deals with
 * two repositories, this one and `tag_repository`, and the tag half is the identity: the
 * name, the rename, the attach-to-image.
 *
 * @note Several queries here join `main.images` on longitude/latitude to answer "which
 * images fall inside this shape". They return **candidates**, not answers: a polygon
 * location is bounded by its box in SQL and the point-in-polygon test is done by the
 * caller, exactly as before. The test is geometry, not storage.
 */

#ifndef DT_DATABASE_LOCATION_REPOSITORY_H
#define DT_DATABASE_LOCATION_REPOSITORY_H

#include "common/map_locations.h"

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/**
 * @brief One row of a geo query: an id with the position that matched.
 *
 * @details Used for both directions. Looking for an image's locations, ::id is a location
 * (tag) id; looking for a location's images, it is an image id. ::lon and ::lat are the
 * IMAGE's coordinates either way, because they are what a point-in-polygon test needs.
 */
typedef struct dt_location_candidate_t
{
  int id;
  int shape; /**< the location's `dt_map_location_shape_t`; 0 when not applicable */
  double lon;
  double lat;
} dt_location_candidate_t;

/** @brief Drop the geometry row of @p locid. The tag itself is the tag repository's. */
void dt_location_repository_delete(const guint locid);

/**
 * @brief Locations whose extent overlaps @p bbox.
 * @return a `GList` of newly allocated `dt_location_draw_t`, without their polygons --
 *         fetch those with dt_location_repository_get_polygon(). Free with
 *         `g_list_free_full(l, g_free)`.
 */
GList *dt_location_repository_get_in_bbox(const dt_map_box_t *const bbox);

/**
 * @brief The geometry of @p locid, if it is a real location.
 *
 * @param name_prefix the `darktable|locations|` prefix a location's tag name must have.
 *        Passed in rather than known here: which tags count as locations is
 *        `common/map_locations.c`'s definition, not the database's.
 * @return newly allocated, or NULL when there is no such location, its longitude is NULL,
 *         or its tag is not under @p name_prefix.
 */
dt_map_location_data_t *dt_location_repository_get_data(const guint locid, const char *name_prefix);

/** @brief Insert or replace the geometry of @p locid, polygon blob included. */
void dt_location_repository_set_data(const guint locid, const dt_map_location_data_t *g);

/**
 * @brief The raw polygon blob of @p locid.
 *
 * @param blob receives a newly allocated copy, or NULL. Free with dt_free().
 * @param bytes receives its size in bytes.
 * @return TRUE when a blob was found. The caller divides by
 *         `sizeof(dt_geo_map_display_point_t)` to get the point count -- this function
 *         does not, because it hands back bytes.
 */
gboolean dt_location_repository_get_polygon(const guint locid, void **blob, gint *bytes);

/**
 * @brief Locations whose shape contains @p imgid's coordinates.
 *
 * @return `dt_location_candidate_t` list; polygon entries are box-bounded candidates the
 *         caller must still test. Free with `g_list_free_full(l, g_free)`.
 */
GList *dt_location_repository_find_locations_for_image(const int32_t imgid);

/**
 * @brief Images whose coordinates fall inside location @p locid.
 *
 * @param shape the location's shape, which selects the query. `MAP_LOCATION_SHAPE_POLYGONS`
 *        returns box-bounded candidates with their coordinates filled in.
 */
GList *dt_location_repository_find_images_for_location(const guint locid, const int shape);

/** @brief Location (tag) ids currently attached to @p imgid. `GINT_TO_POINTER` list. */
GList *dt_location_repository_get_image_locations(const int32_t imgid);

G_END_DECLS

#endif // DT_DATABASE_LOCATION_REPOSITORY_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
