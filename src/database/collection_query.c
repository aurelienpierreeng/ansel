/*
    This file is part of darktable,
    Copyright (C) 2009-2011 johannes hanika.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2011-2016 Tobias Ellinghaus.
    Copyright (C) 2012, 2019-2022 Pascal Obry.
    Copyright (C) 2025-2026 Aurelien PIERRE.

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

#include <string.h>

#include "database/collection_query.h"
#include "database/database.h"
#include "database/sql_debug.h"
#include "common/colorlabels.h"
#include "common/image.h"
#include "common/utility.h"
#include "system/dtpthread.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

// The one collection. dt_collection_new() has a single call site (darktable.c), so there is no
// handle to pass around -- an argument no caller chooses is not a parameter.
//
// The composed SQL never leaves this file. Callers describe what they want with
// dt_collection_query_set_rules() and read results back as ids and counts.
static dt_collection_params_t _params;
static gchar **_where_ext = NULL;
static uint32_t _tagid = 0;
static gchar *_query = NULL;
static uint32_t _count = 0;
static uint64_t _generation = 0;

static sqlite3_stmt *_collection_count_stmt = NULL;
static sqlite3_stmt *_collection_get_limit_stmt = NULL;
static sqlite3_stmt *_collection_get_stmt = NULL;
static sqlite3_stmt *_collection_image_offset_stmt = NULL;

#define LIMIT_QUERY "LIMIT ?1, ?2"

// for term should be an int initialized to and_operator_initial()
// before use.
#define and_operator_initial() (0)
static char * and_operator(int *term)
{
  assert(!IS_NULL_PTR(term));
  if(*term == 0)
  {
    *term = 1;
    return "";
  }
  else
  {
    return " AND ";
  }

  assert(0); // Not reached.
}

#define or_operator_initial() (0)
static char * or_operator(int *term)
{
  assert(!IS_NULL_PTR(term));
  if(*term == 0)
  {
    *term = 1;
    return "";
  }
  else
  {
    return " OR ";
  }

  assert(0); // Not reached.
}

static int _store(gchar *query)
{
  dt_free(_query);
  _query = g_strdup(query);
  _generation++;
  return 1;
}

/** The WHERE built from the rules the caller handed in, as "(1=1<rule><rule>...)".
 *
 *  The original took an `exclude` index and, for exclude >= 0, read
 *  "plugins/lighttable/collect/mode<N>" from conf to decide whether to honour it. That branch
 *  serves dt_collection_get_images_for_rule() and stays in common/collection.c with the conf it
 *  needs; only the plain join is composition. */
static gchar *_extended_where(void)
{
  gchar *complete_string = g_strjoinv(NULL, _where_ext);
  gchar *where_ext = g_strdup_printf("(1=1%s)", complete_string);
  dt_free(complete_string);
  return where_ext;
}

static void _set_selq_pre_sort(char **selq_pre){
  const uint32_t tagid = _tagid;
  char tag[16] = { 0 };
  snprintf(tag, sizeof(tag), "%u", tagid);

  // clang-format off
  *selq_pre = dt_util_dstrcat(*selq_pre,
                              "SELECT DISTINCT mi.id FROM (SELECT"
                              "  id, group_id, film_id, filename, datetime_taken, "
                              "  flags, version, aspect_ratio,"
                              "  maker, model, lens, aperture, exposure, focal_length,"
                              "  iso, import_timestamp, change_timestamp,"
                              "  export_timestamp, print_timestamp"
                              "  FROM main.images AS mi %s%s WHERE ",
                              tagid ? " LEFT JOIN main.tagged_images AS ti"
                                      " ON ti.imgid = mi.id AND ti.tagid = " : "",
                              tagid ? tag : "");
  // clang-format on
}

static gchar *_sort_query(void){
  gchar *sq = NULL;
  const gchar *order = (_params.descending) ? "DESC" : "ASC";

  switch(_params.sort)
  {
    case DT_COLLECTION_SORT_DATETIME:
    case DT_COLLECTION_SORT_IMPORT_TIMESTAMP:
    case DT_COLLECTION_SORT_CHANGE_TIMESTAMP:
    case DT_COLLECTION_SORT_EXPORT_TIMESTAMP:
    case DT_COLLECTION_SORT_PRINT_TIMESTAMP:
    {
      const int local_order = _params.sort;
      char *colname;

      switch(local_order)
      {
        case DT_COLLECTION_SORT_DATETIME:         colname = "datetime_taken" ; break ;
        case DT_COLLECTION_SORT_IMPORT_TIMESTAMP: colname = "import_timestamp" ; break ;
        case DT_COLLECTION_SORT_CHANGE_TIMESTAMP: colname = "change_timestamp" ; break ;
        case DT_COLLECTION_SORT_EXPORT_TIMESTAMP: colname = "export_timestamp" ; break ;
        case DT_COLLECTION_SORT_PRINT_TIMESTAMP:  colname = "print_timestamp" ; break ;
        default: colname = "";
      }
      // clang-format off
      sq = g_strdup_printf("ORDER BY %s %s", colname, order);
      // clang-format on
      break;
    }

    case DT_COLLECTION_SORT_RATING:
      // clang-format off
      sq = g_strdup_printf("ORDER BY CASE WHEN flags & 8 = 8 THEN -1 ELSE flags & 7 END %s", order);
      // clang-format on
      break;

    case DT_COLLECTION_SORT_FILENAME:
      // clang-format off
      sq = g_strdup_printf("ORDER BY filename %s", order);
      // clang-format on
      break;

    case DT_COLLECTION_SORT_ID:
      // clang-format off
      sq = g_strdup_printf("ORDER BY mi.id %s", order);
      // clang-format on
      break;

    case DT_COLLECTION_SORT_COLOR:
      // clang-format off
      sq = g_strdup_printf("ORDER BY color %s", order);
      // clang-format on
      break;

    case DT_COLLECTION_SORT_GROUP:
      // clang-format off
      sq = g_strdup_printf("ORDER BY group_id %s, mi.id-group_id != 0", order);
      // clang-format on
      break;

    case DT_COLLECTION_SORT_PATH:
      // clang-format off
      sq = g_strdup_printf("ORDER BY folder %s", order);
      // clang-format on
      break;

    case DT_COLLECTION_SORT_TITLE:
      // clang-format off
      sq = g_strdup_printf("ORDER BY m.value %s", order);
      // clang-format on
      break;

    case DT_COLLECTION_SORT_NONE:
    default:/*fall through for default*/
      // shouldn't happen
      // clang-format off
      sq = g_strdup_printf("ORDER BY mi.id %s", order);
      // clang-format on
      break;
  }

  // Finish with unique IDs in case we have aliasing
  // try to keep grouped images next to each other, then similar files
  sq = dt_util_dstrcat(sq, ", group_id ASC, mi.id-group_id != 0, filename ASC, version ASC, mi.id ASC");

  return sq;
}

static int _recompose(void){
  uint32_t result;
  gchar *wq, *sq, *selq_pre, *selq_post, *query;
  wq = sq = selq_pre = selq_post = query = NULL;

  /* build where part */
  gchar *where_ext = _extended_where();
  if(_params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT)
  {
    wq = g_strdup(where_ext);
  }
  else if(_params.filter_flags > COLLECTION_FILTER_NONE)
  {
    char *rejected_check = g_strdup_printf("((flags & %d) = %d)", DT_IMAGE_REJECTED, DT_IMAGE_REJECTED);
    int and_term = 1; // that effectively makes the use of and_operator() useless

    // DON'T SELECT IMAGES MARKED TO BE DELETED.
    wq = g_strdup_printf(" ((flags & %d) != %d) ", DT_IMAGE_REMOVE, DT_IMAGE_REMOVE);

    /* From there, the other arguments are OR so we need parentheses if any rating filter is used */
    gboolean got_rating_filter
        = _params.filter_flags
          & (COLLECTION_FILTER_REJECTED | COLLECTION_FILTER_0_STAR | COLLECTION_FILTER_1_STAR
             | COLLECTION_FILTER_2_STAR | COLLECTION_FILTER_3_STAR | COLLECTION_FILTER_4_STAR
             | COLLECTION_FILTER_5_STAR);

    if(got_rating_filter)
      wq = dt_util_dstrcat(wq, " %s (", and_operator(&and_term));

    int or_term = or_operator_initial();
    /* Rejected was a mutually-exclusive rating in initial design, but got converted to
      a toggle state circa 2019, aka images can now have a rating AND be rejected.
      Which sucks because users will not expect rejected images to show when they target n stars ratings.
      Aka we collect images that are rejected OR (have rating == n AND are not rejected).
      Also, because rating flags are bitmasks but not octal, we can't build a single bitmask to
      turn into a single SQL request
    */
    if(_params.filter_flags & COLLECTION_FILTER_REJECTED)
      wq = dt_util_dstrcat(wq, " %s %s ", or_operator(&or_term), rejected_check);

    if(_params.filter_flags & COLLECTION_FILTER_0_STAR)
      wq = dt_util_dstrcat(wq, " %s ((flags & 7) = %i AND NOT %s) ", or_operator(&or_term),
                           DT_VIEW_DESERT, rejected_check);

    if(_params.filter_flags & COLLECTION_FILTER_1_STAR)
      wq = dt_util_dstrcat(wq, " %s ((flags & 7) = %i AND NOT %s) ", or_operator(&or_term),
                          DT_VIEW_STAR_1, rejected_check);

    if(_params.filter_flags & COLLECTION_FILTER_2_STAR)
      wq = dt_util_dstrcat(wq, " %s ((flags & 7) = %i AND NOT %s) ", or_operator(&or_term),
                          DT_VIEW_STAR_2, rejected_check);

    if(_params.filter_flags & COLLECTION_FILTER_3_STAR)
      wq = dt_util_dstrcat(wq, " %s ((flags & 7) = %i AND NOT %s) ", or_operator(&or_term),
                          DT_VIEW_STAR_3, rejected_check);

    if(_params.filter_flags & COLLECTION_FILTER_4_STAR)
      wq = dt_util_dstrcat(wq, " %s ((flags & 7) = %i AND NOT %s) ", or_operator(&or_term),
                          DT_VIEW_STAR_4, rejected_check);

    if(_params.filter_flags & COLLECTION_FILTER_5_STAR)
      wq = dt_util_dstrcat(wq, " %s ((flags & 7) = %i AND NOT %s) ", or_operator(&or_term),
                          DT_VIEW_STAR_5, rejected_check);

    /* Closing the OR parentheses */
    if(got_rating_filter)
      wq = dt_util_dstrcat(wq, ") ");

    gboolean got_altered_filter
        = _params.filter_flags & (COLLECTION_FILTER_ALTERED | COLLECTION_FILTER_UNALTERED);

    if(got_altered_filter)
      wq = dt_util_dstrcat(wq, " %s (", and_operator(&and_term));

    or_term = or_operator_initial();
    if(_params.filter_flags & COLLECTION_FILTER_ALTERED)
      // clang-format off
      wq = dt_util_dstrcat(wq, " %s id IN (SELECT imgid FROM main.history)",
                           or_operator(&or_term));
      // clang-format on

    if(_params.filter_flags & COLLECTION_FILTER_UNALTERED)
      // clang-format off
      wq = dt_util_dstrcat(wq, " %s id NOT IN (SELECT imgid FROM main.history) ",
                           or_operator(&or_term));
      // clang-format on

    if(got_altered_filter)
      wq = dt_util_dstrcat(wq, ") ");

    /* add text filter if any */
    if(_params.text_filter && _params.text_filter[0])
    {
      // clang-format off
      wq = dt_util_dstrcat(wq, " %s id IN (SELECT id FROM main.meta_data WHERE value LIKE '%s'"
                                          " UNION SELECT imgid AS id FROM main.tagged_images AS ti, data.tags AS t"
                                          "   WHERE t.id=ti.tagid AND (t.name LIKE '%s' OR t.synonyms LIKE '%s')"
                                          " UNION SELECT id FROM main.images"
                                          "   WHERE filename LIKE '%s'"
                                          " UNION SELECT i.id FROM main.images AS i, main.film_rolls AS fr"
                                          "   WHERE fr.id=i.film_id AND fr.folder LIKE '%s')",
                           and_operator(&and_term), _params.text_filter,
                                                    _params.text_filter,
                                                    _params.text_filter,
                                                    _params.text_filter,
                                                    _params.text_filter);
      // clang-format on
    }

    /* add colorlabel filter if any */
    gboolean got_color_filter = _params.filter_flags
                                & (COLLECTION_FILTER_BLUE | COLLECTION_FILTER_GREEN | COLLECTION_FILTER_MAGENTA
                                   | COLLECTION_FILTER_RED | COLLECTION_FILTER_YELLOW | COLLECTION_FILTER_WHITE);

    if(got_color_filter)
    {
      int color_mask = 0;
      if(_params.filter_flags & COLLECTION_FILTER_RED)
        color_mask |= 1 << DT_COLORLABELS_RED;
      if(_params.filter_flags & COLLECTION_FILTER_YELLOW)
        color_mask |= 1 << DT_COLORLABELS_YELLOW;
      if(_params.filter_flags & COLLECTION_FILTER_GREEN)
        color_mask |= 1 << DT_COLORLABELS_GREEN;
      if(_params.filter_flags & COLLECTION_FILTER_BLUE)
        color_mask |= 1 << DT_COLORLABELS_BLUE;
      if(_params.filter_flags & COLLECTION_FILTER_MAGENTA)
        color_mask |= 1 << DT_COLORLABELS_PURPLE;

      // color_mask = 31 when all flags are on
      wq = dt_util_dstrcat(wq, " %s (", and_operator(&and_term));

      or_term = or_operator_initial();

      // clang-format off
      if(color_mask > 0)
        wq = dt_util_dstrcat(wq, " %s id IN (SELECT id FROM"
                                 " (SELECT imgid AS id, SUM(1 << color) AS mask FROM main.color_labels GROUP BY imgid)"
                                 " WHERE ((mask & %i) > 0))",
                                 or_operator(&or_term), color_mask);

      if((_params.filter_flags & COLLECTION_FILTER_WHITE))
        wq = dt_util_dstrcat(wq, " %s id NOT IN (SELECT id FROM"
                                 " (SELECT imgid AS id, SUM(1 << color) AS mask FROM main.color_labels GROUP BY imgid)"
                                 " WHERE ((mask & 31) > 0))",
                                 or_operator(&or_term));

      // clang-format on
      wq = dt_util_dstrcat(wq, ")");
    }

    /* add where ext if wanted */
    if((_params.query_flags & COLLECTION_QUERY_USE_WHERE_EXT))
      wq = dt_util_dstrcat(wq, " %s %s", and_operator(&and_term), where_ext);

    dt_free(rejected_check);
  }
  else
  {
    // No filter set: no collection, because filters are toggle in.
    // Just setup some bullshit condition impossible to match.
    wq = g_strdup(" id=0");
  }

  dt_free(where_ext);

  /* build select part includes where */
  /* only COLOR */
  if((_params.sort == DT_COLLECTION_SORT_COLOR)
     && (_params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _set_selq_pre_sort(&selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat(selq_post, ") AS mi LEFT OUTER JOIN main.color_labels AS b ON mi.id = b.imgid");
    // clang-format on
  }
  /* only PATH */
  else if((_params.sort == DT_COLLECTION_SORT_PATH)
          && (_params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _set_selq_pre_sort(&selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat
      (selq_post,
       ") AS mi JOIN (SELECT id AS film_rolls_id, folder FROM main.film_rolls) ON film_id = film_rolls_id");
    // clang-format on
  }
  /* only TITLE */
  else if((_params.sort == DT_COLLECTION_SORT_TITLE)
          && (_params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    _set_selq_pre_sort(&selq_pre);
    // clang-format off
    selq_post = dt_util_dstrcat(selq_post, ") AS mi LEFT OUTER JOIN main.meta_data AS m ON mi.id = m.id AND m.key = %d ",
                                DT_METADATA_XMP_DC_TITLE);
    // clang-format on
  }
  else if(_params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT)
  {
    const uint32_t tagid = _tagid;
    char tag[16] = { 0 };
    snprintf(tag, sizeof(tag), "%u", tagid);
    // clang-format off
    selq_pre = dt_util_dstrcat(selq_pre,
                               "SELECT DISTINCT mi.id FROM (SELECT"
                               "  id, group_id, film_id, filename, datetime_taken, "
                               "  flags, version, %s position, aspect_ratio,"
                               "  maker, model, lens, aperture, exposure, focal_length,"
                               "  iso, import_timestamp, change_timestamp,"
                               "  export_timestamp, print_timestamp"
                               "  FROM main.images AS mi %s%s ) AS mi ",
                               tagid ? "CASE WHEN ti.position IS NULL THEN 0 ELSE ti.position END AS" : "",
                               tagid ? " LEFT JOIN main.tagged_images AS ti"
                                       " ON ti.imgid = mi.id AND ti.tagid = " : "",
                               tagid ? tag : "");
    // clang-format on
  }
  else
  {
    const uint32_t tagid = _tagid;
    char tag[16] = { 0 };
    snprintf(tag, sizeof(tag), "%u", tagid);
    // clang-format off
    selq_pre = dt_util_dstrcat(selq_pre,
                               "SELECT DISTINCT mi.id FROM (SELECT"
                               "  id, group_id, film_id, filename, datetime_taken, "
                               "  flags, version, %s position, aspect_ratio,"
                               "  maker, model, lens, aperture, exposure, focal_length,"
                               "  iso, import_timestamp, change_timestamp,"
                               "  export_timestamp, print_timestamp"
                               "  FROM main.images AS mi %s%s ) AS mi WHERE ",
                               tagid ? "CASE WHEN ti.position IS NULL THEN 0 ELSE ti.position END AS" : "",
                               tagid ? " LEFT JOIN main.tagged_images AS ti"
                                       " ON ti.imgid = mi.id AND ti.tagid = " : "",
                               tagid ? tag : "");
    // clang-format on
  }


  /* build sort order part */
  if(!(_params.query_flags & COLLECTION_QUERY_USE_ONLY_WHERE_EXT)
     && (_params.query_flags & COLLECTION_QUERY_USE_SORT))
  {
    sq = _sort_query();
  }

  /* store the new query */
  query
      = dt_util_dstrcat(query, "%s%s%s %s%s", selq_pre, wq, selq_post ? selq_post : "", sq ? sq : "",
                        (_params.query_flags & COLLECTION_QUERY_USE_LIMIT) ? " " LIMIT_QUERY : "");

  result = _store(query);

  /* free memory used */
  dt_free(sq);
  dt_free(wq);
  dt_free(selq_pre);
  dt_free(selq_post);
  dt_free(query);

  return result;
}

static uint32_t _compute_count(void){
  uint32_t count = 1;
  if(IS_NULL_PTR(_collection_count_stmt))
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT COUNT(DISTINCT imgid) from memory.collected_images",
                                -1, &_collection_count_stmt, NULL);
  }
  sqlite3_stmt *stmt = _collection_count_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  _count = count;
  return count;
}


static const gchar *_ensure_query(void)
{
  if(IS_NULL_PTR(_query)) _recompose();
  return _query;
}

int dt_collection_query_set_rules(const dt_collection_params_t *params, gchar **where_ext,
                                  const uint32_t tagid)
{
  if(IS_NULL_PTR(params)) return 0;

  // Copy the rules in: the caller owns its own and may change them under us.
  dt_free(_params.text_filter);
  _params = *params;
  _params.text_filter = params->text_filter ? g_strdup(params->text_filter) : NULL;

  g_strfreev(_where_ext);
  _where_ext = where_ext ? g_strdupv(where_ext) : NULL;
  _tagid = tagid;

  return _recompose();
}

int dt_collection_query_recompose(void)
{
  return _recompose();
}

uint32_t dt_collection_query_count(void)
{
  return _count;
}

uint64_t dt_collection_query_get_generation(void)
{
  // Bumped by every accepted recomposition. Callers that used to hash the query text to notice a
  // collection change compare this instead: one number that cannot go stale field by field.
  return _generation;
}

GList *dt_collection_query_get_group_members(const int32_t group_id, const int32_t exclude_imgid)
{
  const gchar *collection_query = _ensure_query();
  if(IS_NULL_PTR(collection_query)) return NULL;

  sqlite3_stmt *stmt = NULL;
  // clang-format off
  gchar *query = g_strdup_printf("SELECT id"
                                 "  FROM main.images"
                                 "  WHERE group_id = %d AND id IN (%s)",
                                 group_id, collection_query);
  // clang-format on
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  dt_free(query);

  GList *ids = NULL;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int32_t id = sqlite3_column_int(stmt, 0);
    if(id != exclude_imgid) ids = g_list_prepend(ids, GINT_TO_POINTER(id));
  }
  sqlite3_finalize(stmt);
  return g_list_reverse(ids);
}

void dt_collection_query_cleanup(void)
{
  if(_collection_count_stmt)
  {
    sqlite3_finalize(_collection_count_stmt);
    _collection_count_stmt = NULL;
  }
  if(_collection_get_stmt)
  {
    sqlite3_finalize(_collection_get_stmt);
    _collection_get_stmt = NULL;
  }
  if(_collection_image_offset_stmt)
  {
    sqlite3_finalize(_collection_image_offset_stmt);
    _collection_image_offset_stmt = NULL;
  }
  if(_collection_get_limit_stmt)
  {
    sqlite3_finalize(_collection_get_limit_stmt);
    _collection_get_limit_stmt = NULL;
  }
  dt_free(_query);
  _query = NULL;
  dt_free(_params.text_filter);
  _params.text_filter = NULL;
  g_strfreev(_where_ext);
  _where_ext = NULL;
}

void dt_collection_query_refresh_memory_table(void){
  if(IS_NULL_PTR(dt_collection_get_global()) || !dt_database_is_open()) return;
  sqlite3_stmt *stmt;

  /* check if we can get a query from collection */
  gchar *query = g_strdup(_ensure_query());
  if(IS_NULL_PTR(query)) return;

  // The caller re-restricts the collection to the selection first when the GUI is in culling
  // mode: that is a decision about the interface, and this module cannot see one.

  // 1. drop previous data

  // clang-format off
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "DELETE FROM memory.collected_images",
                        NULL, NULL, NULL);
  // reset autoincrement. need in star_key_accel_callback
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "DELETE FROM memory.sqlite_sequence"
                        " WHERE name='collected_images'",
                        NULL, NULL, NULL);
  // clang-format on

  // 2. insert collected images into the temporary table
  gchar *ins_query = g_strdup_printf("INSERT INTO memory.collected_images (imgid) %s", query);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), ins_query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, 0);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, -1);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  dt_free(query);
  dt_free(ins_query);

  // Re-restricting to the culling selection, and telling the user what just happened, are both
  // the caller's: this module rebuilds the table and counts what landed in it.
  _compute_count();
}

GList *dt_collection_query_get_images(const uint32_t limit){
  GList *list = NULL;
  const gchar *query = _ensure_query();
  if(query)
  {
    if(_params.query_flags & COLLECTION_QUERY_USE_LIMIT)
    {
      if(IS_NULL_PTR(_collection_get_limit_stmt))
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                    "SELECT imgid FROM memory.collected_images LIMIT -1, ?1",
                                    -1, &_collection_get_limit_stmt, NULL);
      }
      sqlite3_stmt *stmt = _collection_get_limit_stmt;
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);
      DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, limit);

      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int32_t imgid = sqlite3_column_int(stmt, 0);
        list = g_list_prepend(list, GINT_TO_POINTER(imgid));
      }
    }
    else
    {
      if(IS_NULL_PTR(_collection_get_stmt))
      {
        DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                    "SELECT imgid FROM memory.collected_images",
                                    -1, &_collection_get_stmt, NULL);
      }
      sqlite3_stmt *stmt = _collection_get_stmt;
      sqlite3_reset(stmt);
      sqlite3_clear_bindings(stmt);

      while(sqlite3_step(stmt) == SQLITE_ROW)
      {
        const int32_t imgid = sqlite3_column_int(stmt, 0);
        list = g_list_prepend(list, GINT_TO_POINTER(imgid));
      }
    }
  }

  return g_list_reverse(list);  // list built in reverse order, so un-reverse it
}

int32_t dt_collection_query_get_nth(const int nth){
  if(nth < 0 || nth >= dt_collection_query_count())
    return -1;
  const gchar *query = _ensure_query();
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, nth);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, 1);

  int result = -1;
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    result  = sqlite3_column_int(stmt, 0);
  }

  sqlite3_finalize(stmt);

  return result;

}

int dt_collection_query_image_offset(const int32_t imgid){
  if(imgid == UNKNOWN_IMAGE) return 0;
  int offset = 0;
  if(IS_NULL_PTR(_collection_image_offset_stmt))
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT imgid FROM memory.collected_images",
                                -1, &_collection_image_offset_stmt, NULL);
  }
  sqlite3_stmt *stmt = _collection_image_offset_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);

  gboolean found = FALSE;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const int id = sqlite3_column_int(stmt, 0);
    if(imgid == id)
    {
      found = TRUE;
      break;
    }
    offset++;
  }

  if(!found) offset = 0;

  return offset;
}

void dt_collection_query_pop(void){
  // Restore previous collection
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM memory.collected_images", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "INSERT INTO memory.collected_images"
                        " SELECT * FROM memory.collected_backup",
                        NULL, NULL, NULL);
}

void dt_collection_query_push(void){
  // Backup current collection
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(), "DELETE FROM memory.collected_backup", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "INSERT INTO memory.collected_backup"
                        " SELECT * FROM memory.collected_images",
                        NULL, NULL, NULL);
}

void dt_collection_query_restrict_to_selection(void)
{
  // Drop everything the user has not selected. Deciding that culling mode means this, backing
  // the collection up first and resetting the selection afterwards, are the caller's -- they
  // are statements about selection and about a view mode, neither of which this module knows.
  DT_DEBUG_SQLITE3_EXEC(dt_database_get_sqlite3_global(),
                        "DELETE FROM memory.collected_images"
                        "  WHERE imgid NOT IN "
                        "  (SELECT imgid FROM main.selected_images)",
                        NULL, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
