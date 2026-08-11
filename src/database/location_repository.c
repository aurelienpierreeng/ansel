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

#include "database/location_repository.h"

#include "database/database.h"
#include "database/sql_debug.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <sqlite3.h>

void dt_location_repository_delete(const guint locid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "DELETE FROM data.locations WHERE tagid=?1", -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

GList *dt_location_repository_get_in_bbox(const dt_map_box_t *const bbox)
{
  sqlite3_stmt *stmt = NULL;
  /* Columns named rather than `SELECT *`. The old query read columns 0..6 positionally,
   * which is only correct while the schema keeps `ratio` seventh -- and `ratio` was added
   * by an ALTER TABLE, so its position is a migration artefact rather than a decision. */
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT tagid, type, longitude, latitude, delta1, delta2, ratio"
                              "  FROM data.locations AS t"
                              "  WHERE latitude IS NOT NULL"
                              "    AND (latitude + delta2) > ?2"
                              "    AND (latitude - delta2) < ?1"
                              "    AND (longitude + delta1) > ?3"
                              "    AND (longitude - delta1) < ?4",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 1, bbox->lat1);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 2, bbox->lat2);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 3, bbox->lon1);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 4, bbox->lon2);

  GList *locs = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_location_draw_t *t = g_malloc0(sizeof(dt_location_draw_t));
    if(t)
    {
      t->id = sqlite3_column_int(stmt, 0);
      t->data.shape = sqlite3_column_int(stmt, 1);
      t->data.lon = sqlite3_column_double(stmt, 2);
      t->data.lat = sqlite3_column_double(stmt, 3);
      t->data.delta1 = sqlite3_column_double(stmt, 4);
      t->data.delta2 = sqlite3_column_double(stmt, 5);
      t->data.ratio = sqlite3_column_double(stmt, 6);
      locs = g_list_prepend(locs, t);
    }
  }
  sqlite3_finalize(stmt);

  return locs; // not reversed: the caller has never depended on the order
}

dt_map_location_data_t *dt_location_repository_get_data(const guint locid, const char *name_prefix)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT type, longitude, latitude, delta1, delta2, ratio"
                              "  FROM data.locations"
                              "  JOIN data.tags ON id = tagid"
                              "  WHERE tagid = ?1 AND longitude IS NOT NULL"
                              "    AND SUBSTR(name, 1, LENGTH(?2)) = ?2",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, name_prefix, -1, SQLITE_STATIC);

  dt_map_location_data_t *g = NULL;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    g = (dt_map_location_data_t *)g_malloc0(sizeof(dt_map_location_data_t));
    g->shape = sqlite3_column_int(stmt, 0);
    g->lon = sqlite3_column_double(stmt, 1);
    g->lat = sqlite3_column_double(stmt, 2);
    g->delta1 = sqlite3_column_double(stmt, 3);
    g->delta2 = sqlite3_column_double(stmt, 4);
    g->ratio = sqlite3_column_double(stmt, 5);
  }
  sqlite3_finalize(stmt);
  return g;
}

void dt_location_repository_set_data(const guint locid, const dt_map_location_data_t *g)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT OR REPLACE INTO data.locations"
                              "  (tagid, type, longitude, latitude, delta1, delta2, ratio, polygons)"
                              "  VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, g->shape);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 3, g->lon);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 4, g->lat);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 5, g->delta1);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 6, g->delta2);
  DT_DEBUG_SQLITE3_BIND_DOUBLE(stmt, 7, g->ratio);
  if(g->shape != MAP_LOCATION_SHAPE_POLYGONS)
  {
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, NULL, 0, SQLITE_STATIC);
  }
  else
  {
    DT_DEBUG_SQLITE3_BIND_BLOB(stmt, 8, g->polygons->data,
                               g->plg_pts * (int)sizeof(dt_geo_map_display_point_t), SQLITE_STATIC);
  }
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

gboolean dt_location_repository_get_polygon(const guint locid, void **blob, gint *bytes)
{
  if(IS_NULL_PTR(blob) || IS_NULL_PTR(bytes)) return FALSE;
  *blob = NULL;
  *bytes = 0;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT polygons FROM data.locations WHERE tagid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);

  gboolean found = FALSE;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int n = sqlite3_column_bytes(stmt, 0);
    const void *src = sqlite3_column_blob(stmt, 0);
    if(n > 0 && src)
    {
      /* Copied out: sqlite owns the column memory only until the statement is stepped or
       * finalised, and the caller keeps this well past both.
       *
       * g_malloc, not malloc: the caller releases it with dt_free(), which is g_free().
       * The original paired malloc() with dt_free() -- harmless on glibc, where g_free is
       * free, and undefined the moment GLib is built with a custom allocator. */
      *blob = g_malloc(n);
      if(*blob)
      {
        memcpy(*blob, src, n);
        *bytes = n;
        found = TRUE;
      }
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

GList *dt_location_repository_find_locations_for_image(const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT l.tagid, l.type, i.longitude, i.latitude FROM main.images AS i"
                              "  JOIN data.locations AS l"
                              "  ON (l.type = ?2"
                              "      AND ((((i.longitude-l.longitude)*(i.longitude-l.longitude))/"
                                            "(delta1*delta1) +"
                              "            ((i.latitude-l.latitude)*(i.latitude-l.latitude))/"
                                            "(delta2*delta2)) <= 1)"
                              "    OR ((l.type = ?3 OR l.type = ?4)"
                              "        AND i.longitude>=(l.longitude-delta1)"
                              "        AND i.longitude<=(l.longitude+delta1)"
                              "        AND i.latitude>=(l.latitude-delta2)"
                              "        AND i.latitude<=(l.latitude+delta2)))"
                              " WHERE i.id = ?1 "
                              "       AND i.latitude IS NOT NULL AND i.longitude IS NOT NULL",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, MAP_LOCATION_SHAPE_ELLIPSE);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, MAP_LOCATION_SHAPE_RECTANGLE);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 4, MAP_LOCATION_SHAPE_POLYGONS);

  GList *candidates = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_location_candidate_t *c = g_malloc0(sizeof(dt_location_candidate_t));
    if(c)
    {
      c->id = sqlite3_column_int(stmt, 0);
      c->shape = sqlite3_column_int(stmt, 1);
      c->lon = sqlite3_column_double(stmt, 2);
      c->lat = sqlite3_column_double(stmt, 3);
      candidates = g_list_prepend(candidates, c);
    }
  }
  sqlite3_finalize(stmt);

  return g_list_reverse(candidates);
}

GList *dt_location_repository_find_images_for_location(const guint locid, const int shape)
{
  sqlite3_stmt *stmt = NULL;

  if(shape == MAP_LOCATION_SHAPE_ELLIPSE)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT i.id, i.longitude, i.latitude FROM main.images AS i"
                                "  JOIN data.locations AS l"
                                "  ON (l.type = ?2"
                                "      AND ((((i.longitude-l.longitude)*(i.longitude-l.longitude))/"
                                              "(delta1*delta1) +"
                                "            ((i.latitude-l.latitude)*(i.latitude-l.latitude))/"
                                              "(delta2*delta2)) <= 1))"
                                "  WHERE l.tagid = ?1 ",
                                -1, &stmt, NULL);
    // clang-format on
  }
  else if(shape == MAP_LOCATION_SHAPE_RECTANGLE)
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT i.id, i.longitude, i.latitude FROM main.images AS i"
                                "  JOIN data.locations AS l"
                                "  ON (l.type = ?2"
                                "       AND i.longitude>=(l.longitude-delta1)"
                                "       AND i.longitude<=(l.longitude+delta1)"
                                "       AND i.latitude>=(l.latitude-delta2)"
                                "       AND i.latitude<=(l.latitude+delta2))"
                                "  WHERE l.tagid = ?1 ",
                                -1, &stmt, NULL);
    // clang-format on
  }
  else // MAP_LOCATION_SHAPE_POLYGONS
  {
    // clang-format off
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT i.id, i.longitude, i.latitude FROM main.images AS i"
                                "  JOIN data.locations AS l"
                                "  ON (l.type = ?2"
                                "       AND i.longitude>=(l.longitude-delta1)"
                                "       AND i.longitude<=(l.longitude+delta1)"
                                "       AND i.latitude>=(l.latitude-delta2)"
                                "       AND i.latitude<=(l.latitude+delta2))"
                                "  WHERE l.tagid = ?1 ",
                                -1, &stmt, NULL);
    // clang-format on
  }

  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, locid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, shape);

  GList *candidates = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_location_candidate_t *c = g_malloc0(sizeof(dt_location_candidate_t));
    if(c)
    {
      c->id = sqlite3_column_int(stmt, 0);
      c->shape = shape;
      c->lon = sqlite3_column_double(stmt, 1);
      c->lat = sqlite3_column_double(stmt, 2);
      candidates = g_list_prepend(candidates, c);
    }
  }
  sqlite3_finalize(stmt);

  // Row order, NOT reverse-row: _map_location_find_images() filters this list and prepends into
  // its own, and that second prepend is what reproduces the original's single prepend off the
  // cursor. Returning reverse-row here would flip the imgid list.
  return g_list_reverse(candidates);
}

GList *dt_location_repository_get_image_locations(const int32_t imgid)
{
  sqlite3_stmt *stmt = NULL;
  // clang-format off
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT t.id FROM main.tagged_images ti"
                              "  JOIN data.tags AS t ON t.id = ti.tagid"
                              "  JOIN data.locations AS l ON l.tagid = t.id"
                              "  WHERE imgid = ?1",
                              -1, &stmt, NULL);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  GList *ids = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
    ids = g_list_prepend(ids, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
  sqlite3_finalize(stmt);

  return ids; // not reversed: matches the previous prepend-only order
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
