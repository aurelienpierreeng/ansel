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
#include "metadata/colorlabels.h"
#include "common/datetime.h"
#include "metadata/map_locations.h"
#include "common/image.h"
#include "common/utility.h"
#include "system/dtpthread.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

// The one collection. dt_collection_new() has a single call site (dt_collection_init_global(),
// reached from the GUI bootstrap only), so there is no
// handle to pass around -- an argument no caller chooses is not a parameter.
//
// The composed SQL never leaves this file. Callers describe what they want with
// dt_collection_query_set_rules() and read results back as ids and counts.
static dt_collection_params_t _params;
static gchar **_where_ext = NULL;   // composed from the rules below, never handed in
static uint32_t _tagid = 0;
static gchar *_query = NULL;
static uint32_t _count = 0;
static uint64_t _generation = 0;
static dt_collection_query_order_resolver_t _order_resolver = NULL;
static const char *const *_order_names = NULL;
static int _order_names_count = 0;


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

void dt_collection_query_set_order_resolver(dt_collection_query_order_resolver_t fn)
{
  _order_resolver = fn;
}

void dt_collection_query_set_order_names(const char *const *names, const int count)
{
  // Borrowed, not copied: these are gettext's, and gettext outlives us.
  _order_names = names;
  _order_names_count = count;
}

static int _store(gchar *query)
{
  /* The generation advances only when the composed text actually changes. Consumers hash it
   * in place of the text (gui/dtgtk/thumbtable.c), and every
   * DT_COLLECTION_CHANGE_RELOAD recomposes an IDENTICAL query -- the enum is defined by it.
   * An unconditional bump would turn each of those reloads (a rating, a tag, an import
   * batch) into a full "collection changed" reset downstream. */
  const gboolean changed = (g_strcmp0(_query, query) != 0);
  dt_free(_query);
  _query = g_strdup(query);
  if(changed) _generation++;
  return 1;
}

/** The WHERE built from the rules the caller handed in, as "(1=1<rule><rule>...)".
 *
 *  The original took an `exclude` index and, for exclude >= 0, read
 *  "plugins/lighttable/collect/mode<N>" from conf to decide whether to honour it. That branch
 *  serves dt_collection_get_images_for_rule() and stays in common/collection.c with the conf it
 *  needs; only the plain join is composition. */
static gchar *get_query_string(const dt_collection_properties_t property, const gchar *text,
                               const gboolean recursive)
{
  char *escaped_text = sqlite3_mprintf("%q", text);
  const unsigned int escaped_length = strlen(escaped_text);
  gchar *query = NULL;

  switch(property)
  {
    case DT_COLLECTION_PROP_QUERY: // raw user-provided SQL WHERE expression (advanced)
      // Intentionally NOT escaped: this is a power-user escape hatch that injects a raw
      // read-only WHERE clause against the local library. A malformed expression makes the
      // prepared statement fail gracefully (empty collection), it does not crash.
      if(text && *text)
        query = g_strdup_printf("(%s)", text);
      else
        query = g_strdup("1=1");
      break;

    case DT_COLLECTION_PROP_FILMROLL: // film roll
      if(!(escaped_text && *escaped_text))
        // clang-format off
        query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s%%'))",
                                escaped_text);
        // clang-format on
      else
        // clang-format off
        query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s'))",
                                escaped_text);
        // clang-format on
      break;

    case DT_COLLECTION_PROP_FOLDERS: // folders
      {
        // Recursion is normally the explicit `recursive` flag; a still-present trailing '*' is
        // only recognized as a fallback for collections/presets saved before that flag existed,
        // and for the Queries tab's raw rule editor, which has no checkbox and still relies on
        // typing '*' by hand -- so this is permanent, not a transitional shim.
        const gboolean has_star = (escaped_length > 0) && (escaped_text[escaped_length-1] == '*');
        if(recursive || has_star)
        {
          if(has_star) escaped_text[escaped_length-1] = '\0';
          // clang-format off
          query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s' OR folder LIKE '%s"
                                  G_DIR_SEPARATOR_S "%%'))",
                                  escaped_text, escaped_text);
          // clang-format on
        }
        // replace |% at the end with /% to only show subfolders
        else if ((escaped_length > 1) && (strcmp(escaped_text+escaped_length-2, "|%") == 0 ))
        {
          escaped_text[escaped_length-2] = '\0';
          // clang-format off
          query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s"
                                  G_DIR_SEPARATOR_S "%%'))",
                                  escaped_text);
          // clang-format on
        }
        else
        {
          // clang-format off
          query = g_strdup_printf("(film_id IN (SELECT id FROM main.film_rolls WHERE folder LIKE '%s'))",
                                  escaped_text);
          // clang-format on
        }
      }
      break;

    case DT_COLLECTION_PROP_COLORLABEL: // colorlabel
    {
      if(!(escaped_text && *escaped_text) || strcmp(escaped_text, "%") == 0)
        // clang-format off
        query = g_strdup_printf("(id IN (SELECT imgid FROM main.color_labels WHERE color IS NOT NULL))");
        // clang-format on
      else
      {
        int color = 0;
        if(strcmp(escaped_text, _("red")) == 0)
          color = 0;
        else if(strcmp(escaped_text, _("yellow")) == 0)
          color = 1;
        else if(strcmp(escaped_text, _("green")) == 0)
          color = 2;
        else if(strcmp(escaped_text, _("blue")) == 0)
          color = 3;
        else if(strcmp(escaped_text, _("purple")) == 0)
          color = 4;
        // clang-format off
        query = g_strdup_printf("(id IN (SELECT imgid FROM main.color_labels WHERE color=%d))", color);
        // clang-format on
      }
    }
    break;

    case DT_COLLECTION_PROP_HISTORY: // history
      {
        if(strcmp(escaped_text, _("altered")) == 0)
        {
          query = g_strdup("EXISTS (SELECT 1 FROM main.history h WHERE h.imgid = id)");
        }
        else if(strcmp(escaped_text, _("unaltered")) == 0)
        {
          query = g_strdup("NOT EXISTS (SELECT 1 FROM main.history h WHERE h.imgid = id)");
        }
        else
        {
          query = g_strdup("1");
        }
      }
      break;

    case DT_COLLECTION_PROP_GEOTAGGING: // geotagging
      {
        const gboolean not_tagged = strcmp(escaped_text, _("not tagged")) == 0;
        const gboolean no_location = strcmp(escaped_text, _("tagged")) == 0;
        const gboolean all_tagged = strcmp(escaped_text, _("tagged*")) == 0;
        char *escaped_text2 = g_strstr_len(escaped_text, -1, "|");
        char *name_clause = g_strdup_printf("t.name LIKE \'%s\' || \'%s\'",
            dt_map_location_data_tag_root(), escaped_text2 ? escaped_text2 : "%");

        if (escaped_text2 && (escaped_text2[strlen(escaped_text2)-1] == '*'))
        {
          escaped_text2[strlen(escaped_text2)-1] = '\0';
          name_clause = g_strdup_printf("(t.name LIKE \'%s\' || \'%s\' OR t.name LIKE \'%s\' || \'%s|%%\')",
          dt_map_location_data_tag_root(), escaped_text2 , dt_map_location_data_tag_root(), escaped_text2);
        }

        if(not_tagged || all_tagged)
          // clang-format off
          query = g_strdup_printf("(id %s IN (SELECT id AS imgid FROM main.images "
                                  "WHERE (longitude IS NOT NULL AND latitude IS NOT NULL))) ",
                                  all_tagged ? "" : "not");
          // clang-format on
        else
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT id AS imgid FROM main.images "
                                         "WHERE (longitude IS NOT NULL AND latitude IS NOT NULL))"
                                         "AND id %s IN (SELECT imgid FROM main.tagged_images AS ti"
                                         "  JOIN data.tags AS t"
                                         "  ON t.id = ti.tagid"
                                         "     AND %s)) ",
                                  no_location ? "not" : "",
                                  name_clause);
          // clang-format on
      }
      break;

    case DT_COLLECTION_PROP_LOCAL_COPY: // local copy
      // clang-format off
      query = g_strdup_printf("(id %s IN (SELECT id AS imgid FROM main.images WHERE (flags & %d))) ",
                              (strcmp(escaped_text, _("not copied locally")) == 0) ? "not" : "",
                              DT_IMAGE_LOCAL_COPY);
      // clang-format on
      break;

    case DT_COLLECTION_PROP_CAMERA: // camera
      // Start query with a false statement to avoid special casing the first condition
      query = g_strdup_printf("((1=0)");
      GList *lists = NULL;
      dt_collection_get_makermodels(text, NULL, &lists);
      for(GList *element = lists; element; element = g_list_next(element))
      {
        GList *tuple = element->data;
        char *clause = sqlite3_mprintf(" OR (maker = '%q' AND model = '%q')", tuple->data, tuple->next->data);
        query = dt_util_dstrcat(query, "%s", clause);
        sqlite3_free(clause);
        dt_free(tuple->data);
        dt_free(tuple->next->data);
        g_list_free(tuple);
        tuple = NULL;
      }
      g_list_free(lists);
      lists = NULL;
      query = dt_util_dstrcat(query, ")");
      break;

    case DT_COLLECTION_PROP_TAG: // tag
    {
      if(!strcmp(escaped_text, _("not tagged")))
      {
        // clang-format off
        query = g_strdup_printf("(id NOT IN (SELECT DISTINCT imgid FROM main.tagged_images "
                                            "WHERE tagid NOT IN memory.darktable_tags))");
        // clang-format on
      }
      else
      {
        if ((escaped_length > 0) && (escaped_text[escaped_length-1] == '*'))
        {
          // shift-click adds an asterix * to include items in and under this hierarchy
          // without using a wildcard % which also would include similar named items
          escaped_text[escaped_length-1] = '\0';
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.tagged_images WHERE tagid IN "
                                         "(SELECT id FROM data.tags "
                                         "WHERE LOWER(name) = LOWER('%s')"
                                         "  OR SUBSTR(LOWER(name), 1, LENGTH('%s') + 1) = LOWER('%s|'))))",
                                  escaped_text, escaped_text, escaped_text);
          // clang-format on
        }
        else if ((escaped_length > 0) && (escaped_text[escaped_length-1] == '%'))
        {
          // ends with % or |%
          escaped_text[escaped_length-1] = '\0';
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.tagged_images WHERE tagid IN "
                                         "(SELECT id FROM data.tags WHERE SUBSTR(LOWER(name), 1, LENGTH('%s')) = LOWER('%s'))))",
                                  escaped_text, escaped_text);
          // clang-format on
        }
        else
        {
          // default
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.tagged_images WHERE tagid IN "
                                       "(SELECT id FROM data.tags WHERE LOWER(name) = LOWER('%s'))))",
                                  escaped_text);
          // clang-format on
        }
      }
    }
    break;

    case DT_COLLECTION_PROP_LENS: // lens
      query = g_strdup_printf("(lens LIKE '%%%s%%')", escaped_text);
      break;

    case DT_COLLECTION_PROP_FOCAL_LENGTH: // focal length
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = g_strdup_printf("((focal_length >= %s) AND (focal_length <= %s))", number1, number2);
      }
      else if(operator && number1)
        query = g_strdup_printf("(focal_length %s %s)", operator, number1);
      else if(number1)
        // clang-format off
        query = g_strdup_printf("(CAST(focal_length AS INTEGER) = CAST(%s AS INTEGER))", number1);
        // clang-format on
      else
        query = g_strdup_printf("(focal_length LIKE '%%%s%%')", escaped_text);

      dt_free(operator);
      dt_free(number1);
      dt_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_ISO: // iso
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = g_strdup_printf("((iso >= %s) AND (iso <= %s))", number1, number2);
      }
      else if(operator && number1)
        query = g_strdup_printf("(iso %s %s)", operator, number1);
      else if(number1)
        query = g_strdup_printf("(iso = %s)", number1);
      else
        query = g_strdup_printf("(iso LIKE '%%%s%%')", escaped_text);

      dt_free(operator);
      dt_free(number1);
      dt_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_APERTURE: // aperture
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          // clang-format off
          query = g_strdup_printf("((ROUND(aperture,1) >= %s) AND (ROUND(aperture,1) <= %s))", number1,
                                  number2);
          // clang-format on
      }
      else if(operator && number1)
        query = g_strdup_printf("(ROUND(aperture,1) %s %s)", operator, number1);
      else if(number1)
        query = g_strdup_printf("(ROUND(aperture,1) = %s)", number1);
      else
        query = g_strdup_printf("(ROUND(aperture,1) LIKE '%%%s%%')", escaped_text);

      dt_free(operator);
      dt_free(number1);
      dt_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_EXPOSURE: // exposure
    {
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_exposure(escaped_text, &number1, &number2, &operator);

      if(operator && strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          // clang-format off
          query = g_strdup_printf("((exposure >= %s  - 1.0/100000) AND (exposure <= %s  + 1.0/100000))", number1,
                                  number2);
          // clang-format on
      }
      else if(operator && number1)
        query = g_strdup_printf("(exposure %s %s)", operator, number1);
      else if(number1)
        // clang-format off
        query = g_strdup_printf("(CASE WHEN exposure < 0.4 THEN ((exposure >= %s - 1.0/100000) AND  (exposure <= %s + 1.0/100000)) "
                                "ELSE (ROUND(exposure,2) >= %s - 1.0/100000) AND (ROUND(exposure,2) <= %s + 1.0/100000) END)",
                                number1, number1, number1, number1);
        // clang-format on
      else
        query = g_strdup_printf("(exposure LIKE '%%%s%%')", escaped_text);

      dt_free(operator);
      dt_free(number1);
      dt_free(number2);
    }
    break;

    case DT_COLLECTION_PROP_FILENAME: // filename
    {
      GList *list = dt_util_str_to_glist(",", escaped_text);

      for (GList *l = list; l; l = g_list_next(l))
      {
        char *name = (char*)l->data;	// remember the original content of this list node
        l->data = g_strdup_printf("(filename LIKE '%%%s%%')", name);
        dt_free(name);			// free the original filename
      }

      char *subquery = dt_util_glist_to_str(" OR ", list);
      query = g_strdup_printf("(%s)", subquery);
      dt_free(subquery);
      g_list_free_full(list, dt_free_gpointer);	// free the SQL clauses as well as the list
      list = NULL;

      break;
    }
    case DT_COLLECTION_PROP_DAY:
    case DT_COLLECTION_PROP_TIME:
    case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
    case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
    {
      const int local_property = property;
      char *colname = NULL;

      switch(local_property)
      {
        case DT_COLLECTION_PROP_DAY: colname = "datetime_taken" ; break ;
        case DT_COLLECTION_PROP_TIME: colname = "datetime_taken" ; break ;
        case DT_COLLECTION_PROP_IMPORT_TIMESTAMP: colname = "import_timestamp" ; break ;
        case DT_COLLECTION_PROP_CHANGE_TIMESTAMP: colname = "change_timestamp" ; break ;
        case DT_COLLECTION_PROP_EXPORT_TIMESTAMP: colname = "export_timestamp" ; break ;
        case DT_COLLECTION_PROP_PRINT_TIMESTAMP: colname = "print_timestamp" ; break ;
      }
      gchar *operator, *number1, *number2;
      dt_collection_split_operator_datetime(escaped_text, &number1, &number2, &operator);
      if(number1 && number1[strlen(number1) - 1] == '%')
        number1[strlen(number1) - 1] = '\0';
      GTimeSpan nb1 = number1 ? dt_datetime_exif_to_gtimespan(number1) : 0;
      GTimeSpan nb2 = number2 ? dt_datetime_exif_to_gtimespan(number2) : 0;

      if(strcmp(operator, "[]") == 0)
      {
        if(number1 && number2)
          query = g_strdup_printf("((%s >= %" G_GINT64_FORMAT ") AND (%s <= %" G_GINT64_FORMAT "))", colname, nb1, colname, nb2);
      }
      else if((strcmp(operator, "=") == 0 || strcmp(operator, "") == 0) && number1 && number2)
        query = g_strdup_printf("((%s >= %" G_GINT64_FORMAT ") AND (%s <= %" G_GINT64_FORMAT "))", colname, nb1, colname, nb2);
      else if(strcmp(operator, "<>") == 0 && number1 && number2)
        // a date/period spans the range [nb1;nb2]; "not equal" means anything OUTSIDE it
        // (before its start OR after its end). AND here would be unsatisfiable (nb1 < nb2).
        query = g_strdup_printf("((%s < %" G_GINT64_FORMAT ") OR (%s > %" G_GINT64_FORMAT "))", colname, nb1, colname, nb2);
      else if(number1)
        query = g_strdup_printf("(%s %s %" G_GINT64_FORMAT ")", colname, operator, nb1);
      else
        query = g_strdup("1 = 1");

      dt_free(operator);
      dt_free(number1);
      dt_free(number2);
      break;
    }

    case DT_COLLECTION_PROP_GROUPING: // grouping
      query = g_strdup_printf("(id %s group_id)", (strcmp(escaped_text, _("group leaders")) == 0) ? "=" : "!=");
      break;

    case DT_COLLECTION_PROP_MODULE: // dev module
      {
        // clang-format off
        query = g_strdup_printf("(id IN (SELECT imgid AS id FROM main.history AS h "
                                "JOIN memory.darktable_iop_names AS m ON m.operation = h.operation "
                                "WHERE h.enabled = 1 AND m.name LIKE '%s'))", escaped_text);
        // clang-format on
      }
      break;

    case DT_COLLECTION_PROP_ORDER: // module order
      {
        // The text here is a LOCALISED module-order name, and turning one back into an id is
        // presentation: this module cannot see translations. The caller installs the resolver.
        const int i = _order_resolver ? _order_resolver(escaped_text) : -1;
        if(i >= 0)
          // clang-format off
          query = g_strdup_printf("(id IN (SELECT imgid FROM main.module_order WHERE version = %d))", i);
          // clang-format on
        else
          // clang-format off
          query = g_strdup_printf("(id NOT IN (SELECT imgid FROM main.module_order))");
          // clang-format on
      }
      break;

    case DT_COLLECTION_PROP_RATING: // image rating
      {
        gchar *operator, *number1, *number2;
        dt_collection_split_operator_number(escaped_text, &number1, &number2, &operator);

        if(operator && strcmp(operator, "[]") == 0)
        {
          if(number1 && number2)
          {
            if(atoi(number1) == -1)
            { // rejected + star rating
              // clang-format off
              query = g_strdup_printf("(flags & 7 >= %s AND flags & 7 <= %s)", number1, number2);
              // clang-format on
            }
            else
            { // non-rejected + star rating
              // clang-format off
              query = g_strdup_printf("((flags & 8 == 0) AND (flags & 7 >= %s AND flags & 7 <= %s))", number1, number2);
              // clang-format on
            }
          }
        }
        else if(operator && number1)
        {
          if(g_strcmp0(operator, "<=") == 0 || g_strcmp0(operator, "<") == 0)
          { // all below rating + rejected
            // clang-format off
            query = g_strdup_printf("(flags & 8 == 8 OR flags & 7 %s %s)", operator, number1);
            // clang-format on
          }
          else if(g_strcmp0(operator, ">=") == 0 || g_strcmp0(operator, ">") == 0)
          {
            if(atoi(number1) >= 0)
            { // non rejected above rating
              // clang-format off
              query = g_strdup_printf("(flags & 8 == 0 AND flags & 7 %s %s)", operator, number1);
              // clang-format on
            }
            // otherwise no filter (rejected + all ratings)
          }
          else
          { // <> exclusion operator
            if(atoi(number1) == -1)
            { // all except rejected
              query = g_strdup_printf("(flags & 8 == 0)");
            }
            else
            { // all except star rating (including rejected)
              query = g_strdup_printf("(flags & 8 == 8 OR flags & 7 %s %s)", operator, number1);
            }
          }
        }
        else if(number1)
        {
          if(atoi(number1) == -1)
          { // rejected only
            query = g_strdup_printf("(flags & 8 == 8)");
          }
          else
          { // non-rejected + star rating
            query = g_strdup_printf("(flags & 8 == 0 AND flags & 7 == %s)", number1);
          }
        }

        dt_free(operator);
        dt_free(number1);
        dt_free(number2);
      }
      break;

    default:
      {
        if(property >= DT_COLLECTION_PROP_METADATA
           && property < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)
        {
          const int keyid = dt_metadata_get_keyid_by_display_order(property - DT_COLLECTION_PROP_METADATA);
          if(strcmp(escaped_text, _("not defined")) != 0)
            // clang-format off
            query = g_strdup_printf("(id IN (SELECT id FROM main.meta_data WHERE key = %d AND value "
                                           "LIKE '%%%s%%'))", keyid, escaped_text);
            // clang-format on
          else
            // clang-format off
            query = g_strdup_printf("(id NOT IN (SELECT id FROM main.meta_data WHERE key = %d))",
                                           keyid);
            // clang-format off
        }
      }
      break;
  }
  sqlite3_free(escaped_text);

  if(IS_NULL_PTR(query)) // We've screwed up and not done a query string, send a placeholder
    query = g_strdup_printf("(1=1)");

  return query;
}

static dt_collection_name_value_t *_name_value_new(char *name, int id, int count, int status)
{
  dt_collection_name_value_t *v = g_malloc0(sizeof(dt_collection_name_value_t));
  v->name = name;
  v->id = id;
  v->count = count;
  v->status = status;
  return v;
}

/** The WHERE for every rule except @p exclude -- or NO restriction at all when
 *  @p apply_exclude is FALSE. The caller decides whether the exclusion applies (that used
 *  to be a conf read of "plugins/lighttable/collect/mode<N>", and conf is not this module's
 *  to read), and FALSE means the rule being edited is an OR rule: an OR rule does not limit
 *  the collection, so nothing may limit its value list either. The original spelled this
 *  "don't limit the collection for OR" and appended no rule whatsoever. */
static gchar *_extended_where_excluding(const int exclude, const gboolean apply_exclude)
{
  gchar *complete_string = g_strdup("");
  if(_where_ext && apply_exclude)
  {
    for(int i = 0; !IS_NULL_PTR(_where_ext[i]); i++)
    {
      if(i == exclude) continue;
      complete_string = dt_util_dstrcat(complete_string, "%s", _where_ext[i]);
    }
  }
  gchar *where_ext = g_strdup_printf("(1=1%s)", complete_string);
  dt_free(complete_string);
  return where_ext;
}

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
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT COUNT(DISTINCT imgid) from memory.collected_images",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return count;
  if(sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  _count = count;
  return count;
}


static const gchar *_ensure_query(void)
{
  if(IS_NULL_PTR(_query)) _recompose();
  return _query;
}

/** Turn the rules into the array of WHERE fragments the query is built from.
 *
 *  One fragment per rule, each prefixed by its conjunction, exactly as the caller used to
 *  assemble them before handing the array over. An empty rule contributes " OR 1=1" in OR mode
 *  and nothing otherwise, which is what makes a blank OR row mean "everything". */
static gchar **_compose_where_ext(const dt_collection_rule_t *rules, const int n_rules)
{
  static const char *const conj[] = { "AND", "OR", "AND NOT" };

  gchar **parts = g_malloc0_n(n_rules + 1, sizeof(gchar *));
  for(int i = 0; i < n_rules; i++)
  {
    const dt_collection_rule_t *r = &rules[i];
    const int mode = CLAMP(r->mode, 0, 2);

    if(IS_NULL_PTR(r->text) || r->text[0] == '\0')
    {
      parts[i] = g_strdup((mode == 1) ? " OR 1=1" : "");
    }
    else
    {
      gchar *where = get_query_string(r->property, r->text, r->recursive);
      parts[i] = g_strdup_printf(" %s %s", conj[mode], where);
      dt_free(where);
    }
  }
  return parts;
}

int dt_collection_query_set_rules(const dt_collection_params_t *params,
                                  const dt_collection_rule_t *rules, const int n_rules,
                                  const uint32_t tagid)
{
  if(IS_NULL_PTR(params)) return 0;

  // Copy the rules in: the caller owns its own and may change them under us.
  dt_free(_params.text_filter);
  _params = *params;
  _params.text_filter = params->text_filter ? g_strdup(params->text_filter) : NULL;

  g_strfreev(_where_ext);
  _where_ext = (rules && n_rules > 0) ? _compose_where_ext(rules, n_rules) : NULL;
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

void dt_collection_query_set_iop_names(const dt_iop_name_row_t *rows, const size_t count)
{
  if(IS_NULL_PTR(rows) || count == 0) return;

  // Faster than building a huge VALUES string: reuse a prepared statement and bind per module.
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "INSERT INTO memory.darktable_iop_names (operation, name) VALUES (?1, ?2)",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return;

  dt_database_start_transaction();
  for(size_t i = 0; i < count; i++)
  {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, rows[i].operation, -1, SQLITE_TRANSIENT);
    DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, rows[i].name, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
  }
  dt_database_release_transaction();
  sqlite3_finalize(stmt);
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

GList *dt_collection_query_get_property_values(const dt_collection_values_request_t *req)
{
  if(IS_NULL_PTR(req)) return NULL;
  const dt_collection_properties_t property = req->property;

  GList *out = NULL;
  gchar *where_ext = _extended_where_excluding(req->exclude_rule, req->apply_exclude);

  // Camera is special: it groups on two text columns and combines them into a display name.
  if(property == DT_COLLECTION_PROP_CAMERA)
  {
    gchar *q = g_strdup_printf("SELECT maker, model, COUNT(*) AS count FROM main.images AS mi"
                               " WHERE %s GROUP BY maker, model", where_ext);
    g_free(where_ext);
    sqlite3_stmt *stmt = NULL;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), q, -1, &stmt, NULL);
    int index = 0;
    while(stmt && sqlite3_step(stmt) == SQLITE_ROW)
    {
      const char *maker = (const char *)sqlite3_column_text(stmt, 0);
      const char *model = (const char *)sqlite3_column_text(stmt, 1);
      gchar *name = dt_collection_get_makermodel(maker, model);
      out = g_list_prepend(out, _name_value_new(name, index++, sqlite3_column_int(stmt, 2), -1));
    }
    if(stmt) sqlite3_finalize(stmt);
    g_free(q);
    return g_list_reverse(out);
  }

  const gboolean is_date = property == DT_COLLECTION_PROP_DAY || property == DT_COLLECTION_PROP_TIME
                           || property == DT_COLLECTION_PROP_IMPORT_TIMESTAMP
                           || property == DT_COLLECTION_PROP_CHANGE_TIMESTAMP
                           || property == DT_COLLECTION_PROP_EXPORT_TIMESTAMP
                           || property == DT_COLLECTION_PROP_PRINT_TIMESTAMP;
  const gboolean has_status
      = (property == DT_COLLECTION_PROP_FOLDERS || property == DT_COLLECTION_PROP_FILMROLL);
  gchar *query = NULL;

  switch(property)
  {
    case DT_COLLECTION_PROP_FOLDERS:
      query = g_strdup_printf("SELECT folder, film_rolls_id, COUNT(*) AS count, status"
                              " FROM main.images AS mi"
                              " JOIN (SELECT fr.id AS film_rolls_id, folder, status"
                              "       FROM main.film_rolls AS fr"
                              "       JOIN memory.film_folder AS ff ON fr.id = ff.id)"
                              "   ON film_id = film_rolls_id"
                              " WHERE %s GROUP BY folder, film_rolls_id", where_ext);
      break;

    case DT_COLLECTION_PROP_TAG:
      query = g_strdup_printf("SELECT name, 1 AS tagid, SUM(count) AS count"
                              " FROM (SELECT tagid, COUNT(*) as count"
                              "   FROM main.images AS mi JOIN main.tagged_images ON id = imgid"
                              "   WHERE %s GROUP BY tagid)"
                              " JOIN (SELECT name, id AS tag_id FROM data.tags)"
                              "   ON tagid = tag_id GROUP BY name", where_ext);
      query = dt_util_dstrcat(query, " UNION ALL "
                                     "SELECT '%s' AS name, 0 as id, COUNT(*) AS count "
                                     "FROM main.images AS mi WHERE mi.id NOT IN"
                                     "  (SELECT DISTINCT imgid FROM main.tagged_images AS ti"
                                     "   WHERE ti.tagid NOT IN memory.darktable_tags)",
                              _("not tagged"));
      break;

    case DT_COLLECTION_PROP_GEOTAGGING:
      query = g_strdup_printf("SELECT CASE WHEN mi.longitude IS NULL OR mi.latitude IS null THEN '%s'"
                              "      ELSE CASE WHEN ta.imgid IS NULL THEN '%s' ELSE '%s' || ta.tagname END"
                              "      END AS name, ta.tagid AS tag_id, COUNT(*) AS count"
                              " FROM main.images AS mi"
                              " LEFT JOIN (SELECT imgid, t.id AS tagid, SUBSTR(t.name, %d) AS tagname"
                              "   FROM main.tagged_images AS ti JOIN data.tags AS t ON ti.tagid = t.id"
                              "   JOIN data.locations AS l ON l.tagid = t.id) AS ta ON ta.imgid = mi.id"
                              " WHERE %s GROUP BY name, tag_id",
                              _("not tagged"), _("tagged"), _("tagged"),
                              (int)strlen(dt_map_location_data_tag_root()) + 1, where_ext);
      break;

    case DT_COLLECTION_PROP_DAY:
      query = g_strdup_printf("SELECT (datetime_taken / 86400000000) * 86400000000 AS date, 1, COUNT(*) AS count"
                              " FROM main.images AS mi"
                              " WHERE datetime_taken IS NOT NULL AND datetime_taken <> 0 AND %s"
                              " GROUP BY date", where_ext);
      break;

    case DT_COLLECTION_PROP_TIME:
    case DT_COLLECTION_PROP_IMPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_CHANGE_TIMESTAMP:
    case DT_COLLECTION_PROP_EXPORT_TIMESTAMP:
    case DT_COLLECTION_PROP_PRINT_TIMESTAMP:
    {
      char *colname = NULL;
      switch(property)
      {
        case DT_COLLECTION_PROP_TIME: colname = "datetime_taken"; break;
        case DT_COLLECTION_PROP_IMPORT_TIMESTAMP: colname = "import_timestamp"; break;
        case DT_COLLECTION_PROP_CHANGE_TIMESTAMP: colname = "change_timestamp"; break;
        case DT_COLLECTION_PROP_EXPORT_TIMESTAMP: colname = "export_timestamp"; break;
        case DT_COLLECTION_PROP_PRINT_TIMESTAMP: colname = "print_timestamp"; break;
        default: break; // unreachable: outer switch already restricts to the timestamp cases
      }
      query = g_strdup_printf("SELECT %s AS date, 1, COUNT(*) AS count FROM main.images AS mi"
                              " WHERE %s IS NOT NULL AND %s <> 0 AND %s GROUP BY date",
                              colname, colname, colname, where_ext);
      break;
    }

    case DT_COLLECTION_PROP_HISTORY:
      query = g_strdup_printf("SELECT CASE WHEN EXISTS (SELECT 1 FROM main.history h WHERE h.imgid = mi.id)"
                              "       THEN '%s' ELSE '%s' END as altered, 1, COUNT(*) AS count"
                              " FROM main.images AS mi WHERE %s GROUP BY altered ORDER BY altered ASC",
                              _("altered"), _("unaltered"), where_ext);
      break;

    case DT_COLLECTION_PROP_LOCAL_COPY:
      query = g_strdup_printf("SELECT CASE WHEN (flags & %d) THEN '%s' ELSE '%s' END as lcp, 1, COUNT(*) AS count"
                              " FROM main.images AS mi WHERE %s GROUP BY lcp ORDER BY lcp ASC",
                              DT_IMAGE_LOCAL_COPY, _("copied locally"), _("not copied locally"), where_ext);
      break;

    case DT_COLLECTION_PROP_COLORLABEL:
      query = g_strdup_printf("SELECT CASE color WHEN 0 THEN '%s' WHEN 1 THEN '%s' WHEN 2 THEN '%s'"
                              "         WHEN 3 THEN '%s' WHEN 4 THEN '%s' ELSE '' END, color, COUNT(*) AS count"
                              " FROM main.images AS mi"
                              " JOIN (SELECT imgid AS color_labels_id, color FROM main.color_labels)"
                              "   ON id = color_labels_id WHERE %s GROUP BY color ORDER BY color DESC",
                              _("red"), _("yellow"), _("green"), _("blue"), _("purple"), where_ext);
      break;

    case DT_COLLECTION_PROP_LENS:
      query = g_strdup_printf("SELECT lens, 1, COUNT(*) AS count FROM main.images AS mi WHERE %s"
                              " GROUP BY lens ORDER BY lens", where_ext);
      break;

    case DT_COLLECTION_PROP_FOCAL_LENGTH:
      query = g_strdup_printf("SELECT CAST(focal_length AS INTEGER) AS focal_length, 1, COUNT(*) AS count"
                              " FROM main.images AS mi WHERE %s GROUP BY CAST(focal_length AS INTEGER)"
                              " ORDER BY CAST(focal_length AS INTEGER)", where_ext);
      break;

    case DT_COLLECTION_PROP_ISO:
      query = g_strdup_printf("SELECT CAST(iso AS INTEGER) AS iso, 1, COUNT(*) AS count"
                              " FROM main.images AS mi WHERE %s GROUP BY iso ORDER BY iso", where_ext);
      break;

    case DT_COLLECTION_PROP_APERTURE:
      query = g_strdup_printf("SELECT ROUND(aperture,1) AS aperture, 1, COUNT(*) AS count"
                              " FROM main.images AS mi WHERE %s GROUP BY aperture ORDER BY aperture", where_ext);
      break;

    case DT_COLLECTION_PROP_EXPOSURE:
      query = g_strdup_printf("SELECT CASE WHEN (exposure < 0.4) THEN '1/' || CAST(1/exposure + 0.9 AS INTEGER)"
                              "         ELSE ROUND(exposure,2) || '\"' END as _exposure, 1, COUNT(*) AS count"
                              " FROM main.images AS mi WHERE %s GROUP BY _exposure ORDER BY exposure", where_ext);
      break;

    case DT_COLLECTION_PROP_FILENAME:
      query = g_strdup_printf("SELECT filename, 1, COUNT(*) AS count FROM main.images AS mi WHERE %s"
                              " GROUP BY filename ORDER BY filename", where_ext);
      break;

    case DT_COLLECTION_PROP_GROUPING:
      query = g_strdup_printf("SELECT CASE WHEN id = group_id THEN '%s' ELSE '%s' END as group_leader, 1,"
                              " COUNT(*) AS count FROM main.images AS mi WHERE %s"
                              " GROUP BY group_leader ORDER BY group_leader ASC",
                              _("group leaders"), _("group followers"), where_ext);
      break;

    case DT_COLLECTION_PROP_MODULE:
      query = g_strdup_printf("SELECT m.name AS module_name, 1, COUNT(*) AS count FROM main.images AS mi"
                              " JOIN (SELECT DISTINCT imgid, operation FROM main.history WHERE enabled = 1) AS h"
                              "  ON h.imgid = mi.id JOIN memory.darktable_iop_names AS m"
                              "  ON m.operation = h.operation WHERE %s GROUP BY module_name ORDER BY module_name",
                              where_ext);
      break;

    case DT_COLLECTION_PROP_ORDER:
    {
      char *orders = NULL;
      for(int i = 0; i < _order_names_count; i++)
        orders = dt_util_dstrcat(orders, "WHEN mo.version = %d THEN '%s' ", i, _order_names[i]);
      orders = dt_util_dstrcat(orders, "ELSE '%s' ", _("none"));
      query = g_strdup_printf("SELECT CASE %s END as ver, 1, COUNT(*) AS count FROM main.images AS mi"
                              " LEFT JOIN (SELECT imgid, version FROM main.module_order) mo ON mo.imgid = mi.id"
                              " WHERE %s GROUP BY ver ORDER BY ver", orders, where_ext);
      g_free(orders);
      break;
    }

    case DT_COLLECTION_PROP_RATING:
      query = g_strdup_printf("SELECT CASE WHEN (flags & 8) == 8 THEN -1 ELSE (flags & 7) END AS rating, 1,"
                              " COUNT(*) AS count FROM main.images AS mi WHERE %s GROUP BY rating ORDER BY rating",
                              where_ext);
      break;

    default:
      if(property >= DT_COLLECTION_PROP_METADATA && property < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)
      {
        const int keyid = dt_metadata_get_keyid_by_display_order(property - DT_COLLECTION_PROP_METADATA);
        // whether this metadata field is hidden is a display preference the caller resolved
        if(!req->metadata_hidden)
          query = g_strdup_printf("SELECT CASE WHEN value IS NULL THEN '%s' ELSE value END AS value, 1,"
                                  " COUNT(*) AS count, CASE WHEN value IS NULL THEN 0 ELSE 1 END AS force_order"
                                  " FROM main.images AS mi"
                                  " LEFT JOIN (SELECT id AS meta_data_id, value FROM main.meta_data WHERE key = %d)"
                                  "  ON id = meta_data_id WHERE %s GROUP BY value ORDER BY force_order, value",
                                  _("not defined"), keyid, where_ext);
      }
      else // film roll
      {
        // likewise the film-roll ordering: a preference, resolved by the caller
        const char *order_by = req->filmroll_order_by;
        query = g_strdup_printf("SELECT folder, film_rolls_id, COUNT(*) AS count, status FROM main.images AS mi"
                                " JOIN (SELECT fr.id AS film_rolls_id, folder, status FROM main.film_rolls AS fr"
                                "        JOIN memory.film_folder AS ff ON ff.id = fr.id) ON film_id = film_rolls_id"
                                " WHERE %s GROUP BY folder ORDER BY %s", where_ext, order_by);
      }
      break;
  }
  g_free(where_ext);
  if(!query) return NULL;

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  while(stmt && sqlite3_step(stmt) == SQLITE_ROW)
  {
    char *name;
    if(is_date)
    {
      char sdt[DT_DATETIME_EXIF_LENGTH] = { 0 };
      dt_datetime_gtimespan_to_exif(sdt, sizeof(sdt), sqlite3_column_int64(stmt, 0));
      if(property == DT_COLLECTION_PROP_DAY) sdt[10] = '\0';
      name = g_strdup(sdt);
    }
    else
    {
      const char *txt = (const char *)sqlite3_column_text(stmt, 0);
      name = txt ? g_strdup(txt) : g_strdup("");
    }
    const int id = sqlite3_column_int(stmt, 1);
    const int count = sqlite3_column_int(stmt, 2);
    const int status = has_status ? sqlite3_column_int(stmt, 3) : -1;
    out = g_list_prepend(out, _name_value_new(name, id, count, status));
  }
  if(stmt) sqlite3_finalize(stmt);
  g_free(query);
  return g_list_reverse(out);
}

void dt_collection_query_get_makermodels(const gchar *filter, GList **sanitized, GList **exif)
{
  gchar *needle = NULL;
  gboolean wildcard = FALSE;

  GHashTable *names = NULL;
  if (sanitized)
    names = g_hash_table_new(g_str_hash, g_str_equal);

  if (filter && filter[0] != '\0')
  {
    needle = g_utf8_strdown(filter, -1);
    wildcard = (needle && needle[strlen(needle) - 1] == '%') ? TRUE : FALSE;
    if(wildcard)
      needle[strlen(needle) - 1] = '\0';
  }

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT maker, model FROM main.images GROUP BY maker, model",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    const char *exif_maker = (char *)sqlite3_column_text(stmt, 0);
    const char *exif_model = (char *)sqlite3_column_text(stmt, 1);

    gchar *makermodel =  dt_collection_get_makermodel(exif_maker, exif_model);

    gchar *haystack = g_utf8_strdown(makermodel, -1);
    if (IS_NULL_PTR(needle) || (wildcard && g_strrstr(haystack, needle) != NULL)
                || (!wildcard && !g_strcmp0(haystack, needle)))
    {
      if (exif)
      {
        // Append a two element list with maker and model
        GList *inner_list = NULL;
        inner_list = g_list_append(inner_list, g_strdup(exif_maker));
        inner_list = g_list_append(inner_list, g_strdup(exif_model));
        *exif = g_list_append(*exif, inner_list);
      }

      if (sanitized)
      {
        gchar *key = g_strdup(makermodel);
        g_hash_table_add(names, key);
      }
    }
    dt_free(haystack);
    dt_free(makermodel);
  }
  sqlite3_finalize(stmt);
  dt_free(needle);

  if(sanitized)
  {
    *sanitized = g_list_sort(g_hash_table_get_keys(names), (GCompareFunc) strcmp);
    g_hash_table_destroy(names);
  }
}

GList *dt_collection_query_get_images_for_rule(const dt_collection_properties_t property, const char *text,
                                         gboolean recursive)
{
  // Build the same WHERE clause the collection would use for this single rule, then
  // enumerate the matching image ids. Independent of the currently active collection so it
  // can feed batch/background operations (remove, attach tag, pre-render thumbnails, ...).
  GList *result = NULL;
  gchar *where = get_query_string(property, text, recursive);
  if(IS_NULL_PTR(where)) return NULL;

  gchar *query = g_strdup_printf("SELECT id FROM main.images WHERE %s", where);
  dt_free(where);

  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt, NULL);
  if(stmt)
  {
    while(sqlite3_step(stmt) == SQLITE_ROW)
      result = g_list_prepend(result, GINT_TO_POINTER(sqlite3_column_int(stmt, 0)));
    sqlite3_finalize(stmt);
  }
  dt_free(query);

  return g_list_reverse(result);
}

int32_t dt_collection_query_find_neighbour(GList *imgids)
{
  // The first image of the collection that is NOT in `imgids`, searched after the list first and
  // then before it. Used to pick what to show once the listed images are gone.
  if(IS_NULL_PTR(imgids)) return -1;

  gchar *txt = NULL;
  int i = 0;
  for(GList *l = imgids; l; l = g_list_next(l))
  {
    const int id = GPOINTER_TO_INT(l->data);
    if(i == 0)
      txt = dt_util_dstrcat(txt, "%d", id);
    else
      txt = dt_util_dstrcat(txt, ",%d", id);
    i++;
  }

  int32_t next = -1;
  // 2. search the first imgid not in the list but AFTER the list (or in a gap inside the list)
  // we need to be carefull that some images in the list may not be present on screen (collapsed groups)
  // clang-format off
  gchar *query = g_strdup_printf("SELECT imgid"
                                  " FROM memory.collected_images"
                                  " WHERE imgid NOT IN (%s)"
                                  "  AND rowid > (SELECT rowid"
                                  "              FROM memory.collected_images"
                                  "              WHERE imgid IN (%s)"
                                  "              ORDER BY rowid LIMIT 1)"
                                  " ORDER BY rowid LIMIT 1",
                                  txt, txt);
  // clang-format on
  sqlite3_stmt *stmt2;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt2, NULL);
  if(sqlite3_step(stmt2) == SQLITE_ROW)
  {
    next = sqlite3_column_int(stmt2, 0);
  }
  sqlite3_finalize(stmt2);
  dt_free(query);
  // 3. if next is still unvalid, let's try to find the first untouched image BEFORE the list
  if(next < 0)
  {
    // clang-format off
    query = g_strdup_printf("SELECT imgid"
                            " FROM memory.collected_images"
                            " WHERE imgid NOT IN (%s)"
                            "   AND rowid < (SELECT rowid"
                            "                FROM memory.collected_images"
                            "                WHERE imgid IN (%s)"
                            "                ORDER BY rowid LIMIT 1)"
                            " ORDER BY rowid DESC LIMIT 1",
                            txt, txt);
    // clang-format on
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(), query, -1, &stmt2, NULL);
    if(sqlite3_step(stmt2) == SQLITE_ROW)
    {
      next = sqlite3_column_int(stmt2, 0);
    }
    sqlite3_finalize(stmt2);
    dt_free(query);
  }
  dt_free(txt);
  return next;
}

void dt_collection_query_cleanup(void)
{
  /* No cached statements any more: everything in this file is prepared and finalised per
   * call, so there is nothing to finalise ahead of the connection closing. */
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
    const gboolean use_limit = (_params.query_flags & COLLECTION_QUERY_USE_LIMIT) != 0;
    sqlite3_stmt *stmt = NULL;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                use_limit ? "SELECT imgid FROM memory.collected_images LIMIT -1, ?1"
                                          : "SELECT imgid FROM memory.collected_images",
                                -1, &stmt, NULL);
    if(IS_NULL_PTR(stmt)) return NULL;
    if(use_limit) DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, limit);

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int32_t imgid = sqlite3_column_int(stmt, 0);
      list = g_list_prepend(list, GINT_TO_POINTER(imgid));
    }
    sqlite3_finalize(stmt);
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
  sqlite3_stmt *stmt = NULL;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                              "SELECT imgid FROM memory.collected_images",
                              -1, &stmt, NULL);
  if(IS_NULL_PTR(stmt)) return 0;

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

  sqlite3_finalize(stmt);

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

  /* The published count follows every mutation of memory.collected_images this module
   * makes. The refresh already counted, but the caller runs this restriction AFTER the
   * refresh -- the original computed its count after both, and dt_collection_get_count()
   * in culling mode must report the culled subset, not the full collection. */
  _compute_count();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
