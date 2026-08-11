/*
    This file is part of darktable,
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010-2013 johannes hanika.
    Copyright (C) 2010-2017 Tobias Ellinghaus.
    Copyright (C) 2011-2012 José Carlos García Sogo.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2012, 2018-2022 Pascal Obry.
    Copyright (C) 2012 Petr Styblo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2013 Simon Spannagel.
    Copyright (C) 2013 Gaspard Jankowiak.
    Copyright (C) 2013 hal.
    Copyright (C) 2013 Ulrich Pegelow.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015-2016 Jérémy Rosen.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2016, 2020-2022 Aldric Renaudin.
    Copyright (C) 2016 itinerarium.
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2016 Petr Synek.
    Copyright (C) 2017 Dominik Markiewicz.
    Copyright (C) 2017, 2019 Liran Vaknin.
    Copyright (C) 2017, 2019 luzpaz.
    Copyright (C) 2018 August Schwerdfeger.
    Copyright (C) 2018 Mario Lueder.
    Copyright (C) 2018 Rick Yorgason.
    Copyright (C) 2018 Rikard Öxler.
    Copyright (C) 2018, 2020 Sam Smith.
    Copyright (C) 2018 Simon Legner.
    Copyright (C) 2019 Bill Ferguson.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019 Mark Feit.
    Copyright (C) 2019 rrd1.
    Copyright (C) 2020 codingdave@gmail.com.
    Copyright (C) 2020 David-Tillmann Schaefer.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2020 jpverrue.
    Copyright (C) 2020-2022 Philippe Weyland.
    Copyright (C) 2020 Tino Mettler.
    Copyright (C) 2020 U-DESKTOP-HQME86J\marco.
    Copyright (C) 2021 Arnaud TANGUY.
    Copyright (C) 2021 Chris Elston.
    Copyright (C) 2021 Daniel Vogelbacher.
    Copyright (C) 2021 HansBull.
    Copyright (C) 2021 Harald.
    Copyright (C) 2021 quovadit.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2021 Stefan Boxleitner.
    Copyright (C) 2022-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2023 André Doherty.
    Copyright (C) 2024-2025 Guillaume Stutin.
    
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

#include "common/collection.h"
#include "control/settings.h"
#include "database/collection_query.h"
#include "database/sql_debug.h"
#include "common/colorlabels.h"
#include "common/image.h"
#include "imageio/imageio_core.h"
#include "develop/iop_order.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "common/map_locations.h"
#include "common/datetime.h"
#include "common/selection.h"
#include "common/conf.h"
#include "control/control.h"
#include "views/view.h"

#include <assert.h>
#include <glib.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "gui/application.h"


#ifdef _WIN32
//MSVCRT does not have strptime implemented
#endif


#define SELECT_QUERY "SELECT DISTINCT * FROM %s"
#define LIMIT_QUERY "LIMIT ?1, ?2"

static sqlite3_stmt *_collection_get_makermodels_stmt = NULL;

/* Stores the collection query, returns 1 if changed.. */
/* Counts the number of images in the current collection */

/* determine image offset of specified imgid for the given collection */
static int dt_collection_image_offset_with_collection(const dt_collection_t *collection, int32_t imgid);

dt_collection_t *dt_collection_new()
{
  dt_collection_t *collection = g_malloc0(sizeof(dt_collection_t));
  dt_collection_reset(collection);
  return collection;
}

void dt_collection_free(const dt_collection_t *collection)
{
  dt_free(collection->params.text_filter);
  g_strfreev(collection->where_ext);

  // The composed query, its cached statements and the copy of the rules belong to the database
  // module now.
  dt_collection_query_cleanup();
  if(_collection_get_makermodels_stmt)
  {
    sqlite3_finalize(_collection_get_makermodels_stmt);
    _collection_get_makermodels_stmt = NULL;
  }
  dt_free(collection);
}

const dt_collection_params_t *dt_collection_params(const dt_collection_t *collection)
{
  return &collection->params;
}


// Return a pointer to a static string for an "AND" operator if the
// number of terms processed so far requires it.  The variable used
// for term should be an int initialized to and_operator_initial()
// before use.


int dt_collection_update(const dt_collection_t *collection)
{
  /* store flags to conf */
  if(collection == dt_collection_get_global())
  {
    dt_conf_set_int("plugins/collection/query_flags", collection->params.query_flags);
    dt_conf_set_int("plugins/collection/filter_flags", collection->params.filter_flags);
    dt_conf_set_string("plugins/collection/text_filter", collection->params.text_filter ? collection->params.text_filter : "");
    dt_conf_set_int("plugins/collection/sort", collection->params.sort);
    dt_conf_set_bool("plugins/collection/descending", collection->params.descending);
  }

  // Hand the rules over; composing the SQL from them is the database module's.
  return dt_collection_query_set_rules(&collection->params, collection->where_ext, collection->tagid);
}

void dt_collection_memory_update()
{
  // Handle culling mode across re-queryings : re-restrict collection to selection
  if(dt_gui_get_global() && dt_gui_get_global()->culling_mode)
    dt_culling_mode_to_selection();

  dt_collection_query_refresh_memory_table();

  // Handle culling mode across re-queryings : re-restrict collection to selection
  if(dt_gui_get_global() && dt_gui_get_global()->culling_mode)
    dt_selection_to_culling_mode();

  dt_collection_hint_message(dt_collection_get_global());
}

GList *dt_collection_get(const dt_collection_t *collection, const uint32_t limit)
{
  return dt_collection_query_get_images(limit);
}

int32_t dt_collection_get_nth(const dt_collection_t *collection, const int nth)
{
  return dt_collection_query_get_nth(nth);
}

static int dt_collection_image_offset_with_collection(const dt_collection_t *collection, int32_t imgid)
{
  return dt_collection_query_image_offset(imgid);
}

void dt_pop_collection()
{
  dt_collection_query_pop();
}

void dt_push_collection()
{
  dt_collection_query_push();
}

void dt_selection_to_culling_mode()
{
  // Culling mode restricts the collection to the selection

  // Remove non-selected from collected images, aka culling mode
  dt_push_collection();
  dt_collection_query_restrict_to_selection();

  // Backup and reset current selection
  dt_selection_push(dt_selection_get_global());
  dt_selection_clear(dt_selection_get_global());
}

uint32_t dt_collection_get_count(const dt_collection_t *collection)
{
  return dt_collection_query_count();
}

void dt_collection_reset(const dt_collection_t *collection)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;

  /* setup defaults */
  params->query_flags = COLLECTION_QUERY_FULL;

  // enable all filters, aka filter in everything
  params->filter_flags = COLLECTION_FILTER_ALL;

  /* apply stored query parameters from previous darktable session */
  int flags = dt_conf_get_int("plugins/collection/filter_flags");
  params->filter_flags = (flags < 0) ? COLLECTION_FILTER_ALL : flags;

  dt_free(params->text_filter);
  params->text_filter = dt_conf_get_string("plugins/collection/text_filter");
  params->sort = dt_conf_get_int("plugins/collection/sort");
  params->descending = dt_conf_get_bool("plugins/collection/descending");
  dt_collection_update_query(collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

dt_collection_filter_flag_t dt_collection_get_filter_flags(const dt_collection_t *collection)
{
  return collection->params.filter_flags;
}

void dt_collection_set_filter_flags(const dt_collection_t *collection, dt_collection_filter_flag_t flags)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->filter_flags = flags;
}

char *dt_collection_get_text_filter(const dt_collection_t *collection)
{
  return collection->params.text_filter;
}

void dt_collection_set_text_filter(const dt_collection_t *collection, char *text_filter)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  dt_free(params->text_filter);
  params->text_filter = text_filter;
}

dt_collection_query_flags_t dt_collection_get_query_flags(const dt_collection_t *collection)
{
  return collection->params.query_flags;
}

void dt_collection_set_query_flags(const dt_collection_t *collection, dt_collection_query_flags_t flags)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->query_flags = flags;
}

gchar *dt_collection_get_extended_where(const dt_collection_t *collection, int exclude)
{
  gchar *complete_string = NULL;

  if (exclude >= 0)
  {
    complete_string = g_strdup("");
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", exclude);
    const int mode = dt_conf_get_int(confname);
    if (mode != 1) // don't limit the collection for OR
    {
      for(int i = 0; !IS_NULL_PTR(collection->where_ext[i]); i++)
      {
        // exclude the one rule from extended where
        if (i != exclude)
          complete_string = dt_util_dstrcat(complete_string, "%s", collection->where_ext[i]);
      }
    }
  }
  else
    complete_string = g_strjoinv(complete_string, ((dt_collection_t *)collection)->where_ext);

  gchar *where_ext = g_strdup_printf("(1=1%s)", complete_string);
  dt_free(complete_string);

  return where_ext;
}

void dt_collection_set_extended_where(const dt_collection_t *collection, gchar **extended_where)
{
  /* free extended where if already exists */
  g_strfreev(collection->where_ext);

  /* set new from parameter */
  ((dt_collection_t *)collection)->where_ext = g_strdupv(extended_where);
}

void dt_collection_set_tag_id(dt_collection_t *collection, const uint32_t tagid)
{
  collection->tagid = tagid;
}

void dt_collection_set_sort(const dt_collection_t *collection, dt_collection_sort_t sort, gboolean reverse)
{
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;

  if(sort != DT_COLLECTION_SORT_NONE)
    params->sort = sort;

  if(reverse != -1) params->descending = reverse;
}

dt_collection_sort_t dt_collection_get_sort_field(const dt_collection_t *collection)
{
  return collection->params.sort;
}

gboolean dt_collection_get_sort_descending(const dt_collection_t *collection)
{
  return collection->params.descending;
}

const char *dt_collection_name(dt_collection_properties_t prop)
{
  char *col_name = NULL;
  switch(prop)
  {
    case DT_COLLECTION_PROP_FILMROLL:         return _("film roll");
    case DT_COLLECTION_PROP_FOLDERS:          return _("folder");
    case DT_COLLECTION_PROP_CAMERA:           return _("camera");
    case DT_COLLECTION_PROP_TAG:              return _("tag");
    case DT_COLLECTION_PROP_DAY:              return _("date taken");
    case DT_COLLECTION_PROP_TIME:             return _("date-time taken");
    case DT_COLLECTION_PROP_IMPORT_TIMESTAMP: return _("import timestamp");
    case DT_COLLECTION_PROP_CHANGE_TIMESTAMP: return _("change timestamp");
    case DT_COLLECTION_PROP_EXPORT_TIMESTAMP: return _("export timestamp");
    case DT_COLLECTION_PROP_PRINT_TIMESTAMP:  return _("print timestamp");
    case DT_COLLECTION_PROP_HISTORY:          return _("history");
    case DT_COLLECTION_PROP_COLORLABEL:       return _("color label");
    case DT_COLLECTION_PROP_LENS:             return _("lens");
    case DT_COLLECTION_PROP_FOCAL_LENGTH:     return _("focal length");
    case DT_COLLECTION_PROP_ISO:              return _("ISO");
    case DT_COLLECTION_PROP_APERTURE:         return _("aperture");
    case DT_COLLECTION_PROP_EXPOSURE:         return _("exposure");
    case DT_COLLECTION_PROP_FILENAME:         return _("filename");
    case DT_COLLECTION_PROP_GEOTAGGING:       return _("geotagging");
    case DT_COLLECTION_PROP_GROUPING:         return _("grouping");
    case DT_COLLECTION_PROP_LOCAL_COPY:       return _("local copy");
    case DT_COLLECTION_PROP_MODULE:           return _("module");
    case DT_COLLECTION_PROP_ORDER:            return _("module order");
    case DT_COLLECTION_PROP_RATING:           return _("rating");
    case DT_COLLECTION_PROP_QUERY:            return _("custom query");
    case DT_COLLECTION_PROP_LAST:             return NULL;
    default:
    {
      if(prop >= DT_COLLECTION_PROP_METADATA
         && prop < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)
      {
        const int i = prop - DT_COLLECTION_PROP_METADATA;
        const int type = dt_metadata_get_type_by_display_order(i);
        if(type != DT_METADATA_TYPE_INTERNAL)
        {
          const char *name = (gchar *)dt_metadata_get_name_by_display_order(i);
          char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
          const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
          dt_free(setting);
          if(!hidden) col_name = _(name);
        }
      }
    }
  }
  return col_name;
}

GList *dt_collection_get_all(const dt_collection_t *collection, int limit)
{
  return dt_collection_get(collection, limit);
}

/* splits an input string into a number part and an optional operator part.
   number can be a decimal integer or rational numerical item.
   operator can be any of "=", "<", ">", "<=", ">=" and "<>".
   range notation [x;y] can also be used

   number and operator are returned as pointers to null terminated strings in g_mallocated
   memory (to be g_free'd after use) - or NULL if no match is found.
*/
void dt_collection_split_operator_number(const gchar *input, char **number1, char **number2, char **operator)
{
  GRegex *regex;
  GMatchInfo *match_info;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  regex = g_regex_new("^\\s*\\[\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*;\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  int match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    *number1 = g_match_info_fetch(match_info, 1);
    *number2 = g_match_info_fetch(match_info, 2);
    *operator= g_strdup("[]");
    g_match_info_free(match_info);
    g_regex_unref(regex);
    return;
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  // and we test the classic comparison operators
  regex = g_regex_new("^\\s*(=|<|>|<=|>=|<>)?\\s*([-+]?[0-9]+\\.?[0-9]*)\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    *operator= g_match_info_fetch(match_info, 1);
    *number1 = g_match_info_fetch(match_info, 2);

    if(*operator && strcmp(*operator, "") == 0)
    {
      dt_free(*operator);
    }
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);
}

static char *_dt_collection_compute_datetime(const char *operator, const char *input)
{
  if(strlen(input) < 4) return NULL;

  char bound[DT_DATETIME_LENGTH];
  gboolean res;
  if(strcmp(operator, ">") == 0 || strcmp(operator, "<=") == 0)
    res = dt_datetime_entry_to_exif_upper_bound(bound, sizeof(bound), input);
  else
    res = dt_datetime_entry_to_exif(bound, sizeof(bound), input);
  if(res)
    return g_strdup(bound);
  else return NULL;
}
/* splits an input string into a date-time part and an optional operator part.
   operator can be any of "=", "<", ">", "<=", ">=" and "<>".
   range notation [x;y] can also be used
   datetime values should follow the pattern YYYY:MM:DD hh:mm:ss.sss
   but only year part is mandatory

   datetime and operator are returned as pointers to null terminated strings in g_mallocated
   memory (to be g_free'd after use) - or NULL if no match is found.
*/
void dt_collection_split_operator_datetime(const gchar *input, char **number1, char **number2, char **operator)
{
  GRegex *regex;
  GMatchInfo *match_info;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  // 2 elements : date-time1 and  date-time2
  regex = g_regex_new("^\\s*\\[\\s*(\\d{4}[:.\\d\\s]*)\\s*;\\s*(\\d{4}[:.\\d\\s]*)\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  int match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    gchar *txt = g_match_info_fetch(match_info, 1);
    gchar *txt2 = g_match_info_fetch(match_info, 2);

    *number1 = _dt_collection_compute_datetime(">=", txt);
    *number2 = _dt_collection_compute_datetime("<=", txt2);
    *operator= g_strdup("[]");

    dt_free(txt);
    dt_free(txt2);
    g_match_info_free(match_info);
    g_regex_unref(regex);
    return;
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  // and we test the classic comparison operators
  // 2 elements : operator and date-time
  regex = g_regex_new("^\\s*(=|<|>|<=|>=|<>)?\\s*(\\d{4}[:.\\d\\s]*)?\\s*%?\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);

  if(match_count == 3)
  {
    *operator= g_match_info_fetch(match_info, 1);
    gchar *txt = g_match_info_fetch(match_info, 2);

    if(strcmp(*operator, "") == 0 || strcmp(*operator, "=") == 0 || strcmp(*operator, "<>") == 0)
    {
      *number1 = dt_util_dstrcat(*number1, "%s%%", txt);
      *number2 = _dt_collection_compute_datetime(">", txt);
    }
    else
      *number1 = _dt_collection_compute_datetime(*operator, txt);

    dt_free(txt);
  }

  // ensure operator is not null
  if(IS_NULL_PTR(*operator)) *operator= g_strdup("");

  g_match_info_free(match_info);
  g_regex_unref(regex);
}

void dt_collection_split_operator_exposure(const gchar *input, char **number1, char **number2, char **operator)
{
  GRegex *regex;
  GMatchInfo *match_info;

  *number1 = *number2 = *operator= NULL;

  // we test the range expression first
  regex = g_regex_new("^\\s*\\[\\s*(1/)?([0-9]+\\.?[0-9]*)(\")?\\s*;\\s*(1/)?([0-9]+\\.?[0-9]*)(\")?\\s*\\]\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  int match_count = g_match_info_get_match_count(match_info);

  if(match_count == 6 || match_count == 7)
  {
    gchar *n1 = g_match_info_fetch(match_info, 2);

    if(strstr(g_match_info_fetch(match_info, 1), "1/") != NULL)
      *number1 = g_strdup_printf("1.0/%s", n1);
    else
      *number1 = n1;

    gchar *n2 = g_match_info_fetch(match_info, 5);

    if(strstr(g_match_info_fetch(match_info, 4), "1/") != NULL)
      *number2 = g_strdup_printf("1.0/%s", n2);
    else
      *number2 = n2;

    *operator= g_strdup("[]");
    g_match_info_free(match_info);
    g_regex_unref(regex);
    return;
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);

  // and we test the classic comparison operators
  regex = g_regex_new("^\\s*(=|<|>|<=|>=|<>)?\\s*(1/)?([0-9]+\\.?[0-9]*)(\")?\\s*$", 0, 0, NULL);
  g_regex_match_full(regex, input, -1, 0, 0, &match_info, NULL);
  match_count = g_match_info_get_match_count(match_info);
  if(match_count == 4 || match_count == 5)
  {
    *operator= g_match_info_fetch(match_info, 1);

    gchar *n1 = g_match_info_fetch(match_info, 3);

    if(strstr(g_match_info_fetch(match_info, 2), "1/") != NULL)
      *number1 = g_strdup_printf("1.0/%s", n1);
    else
      *number1 = n1;

    if(*operator && strcmp(*operator, "") == 0)
    {
      dt_free(*operator);
    }
  }

  g_match_info_free(match_info);
  g_regex_unref(regex);
}

void dt_collection_get_makermodels(const gchar *filter, GList **sanitized, GList **exif)
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

  if(IS_NULL_PTR(_collection_get_makermodels_stmt))
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get_sqlite3_global(),
                                "SELECT maker, model FROM main.images GROUP BY maker, model",
                                -1, &_collection_get_makermodels_stmt, NULL);
  }
  sqlite3_stmt *stmt = _collection_get_makermodels_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
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
  dt_free(needle);

  if(sanitized)
  {
    *sanitized = g_list_sort(g_hash_table_get_keys(names), (GCompareFunc) strcmp);
    g_hash_table_destroy(names);
  }
}

gchar *dt_collection_get_makermodel(const char *exif_maker, const char *exif_model)
{
  char maker[64];
  char model[64];
  char alias[64];
  maker[0] = model[0] = alias[0] = '\0';
  dt_imageio_lookup_makermodel(exif_maker, exif_model,
                               maker, sizeof(maker),
                               model, sizeof(model),
                               alias, sizeof(alias));

  // Create the makermodel by concatenation
  gchar *makermodel = g_strdup_printf("%s %s", maker, model);
  return makermodel;
}

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
        int i = 0;
        for(i = 0; i < DT_IOP_ORDER_LAST; i++)
        {
          if(strcmp(escaped_text, _(dt_iop_order_string(i))) == 0) break;
        }
        if(i < DT_IOP_ORDER_LAST)
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

GList *dt_collection_get_images_for_rule(const dt_collection_properties_t property, const char *text,
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

void dt_collection_name_value_free(gpointer value)
{
  dt_collection_name_value_t *v = (dt_collection_name_value_t *)value;
  if(!v) return;
  g_free(v->name);
  g_free(v);
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

GList *dt_collection_get_property_values(const dt_collection_properties_t property, const int rule)
{
  GList *out = NULL;
  gchar *where_ext = dt_collection_get_extended_where(dt_collection_get_global(), rule);

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
      for(int i = 0; i < DT_IOP_ORDER_LAST; i++)
        orders = dt_util_dstrcat(orders, "WHEN mo.version = %d THEN '%s' ", i, _(dt_iop_order_string(i)));
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
        const char *name = (const char *)dt_metadata_get_name(keyid);
        char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
        const gboolean hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
        g_free(setting);
        if(!hidden)
          query = g_strdup_printf("SELECT CASE WHEN value IS NULL THEN '%s' ELSE value END AS value, 1,"
                                  " COUNT(*) AS count, CASE WHEN value IS NULL THEN 0 ELSE 1 END AS force_order"
                                  " FROM main.images AS mi"
                                  " LEFT JOIN (SELECT id AS meta_data_id, value FROM main.meta_data WHERE key = %d)"
                                  "  ON id = meta_data_id WHERE %s GROUP BY value ORDER BY force_order, value",
                                  _("not defined"), keyid, where_ext);
      }
      else // film roll
      {
        gchar *order_by = NULL;
        const char *filmroll_sort = dt_conf_get_string_const("plugins/collect/filmroll_sort");
        if(strcmp(filmroll_sort, "id") == 0)
          order_by = g_strdup("film_rolls_id DESC");
        else
          order_by = dt_conf_get_bool("plugins/collect/descending") ? g_strdup("folder DESC") : g_strdup("folder");
        query = g_strdup_printf("SELECT folder, film_rolls_id, COUNT(*) AS count, status FROM main.images AS mi"
                                " JOIN (SELECT fr.id AS film_rolls_id, folder, status FROM main.film_rolls AS fr"
                                "        JOIN memory.film_folder AS ff ON ff.id = fr.id) ON film_id = film_rolls_id"
                                " WHERE %s GROUP BY folder ORDER BY %s", where_ext, order_by);
        g_free(order_by);
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

int dt_collection_serialize(char *buf, int bufsize)
{
  char confname[200];
  int c;
  const int num_rules = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  c = snprintf(buf, bufsize, "%d:", num_rules);
  buf += c;
  bufsize -= c;
  for(int k = 0; k < num_rules; k++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", k);
    const int mode = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", mode);
    buf += c;
    bufsize -= c;
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", k);
    const int item = dt_conf_get_int(confname);
    c = snprintf(buf, bufsize, "%d:", item);
    buf += c;
    bufsize -= c;
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", k);
    const char *str = dt_conf_get_string_const(confname);
    // Fold the recursive flag back into the trailing '*' this wire format has always used, so
    // dt_collection_deserialize() (and any consumer reading this string directly) sees exactly
    // what get_query_string() expects, with no separate field to carry through.
    gchar *str_recursive = NULL;
    if(item == DT_COLLECTION_PROP_FOLDERS && str && str[0] != '\0' && str[strlen(str) - 1] != '*')
    {
      snprintf(confname, sizeof(confname), "plugins/lighttable/collect/recursive%1d", k);
      if(dt_conf_get_bool(confname)) str_recursive = g_strconcat(str, "*", NULL);
    }
    const char *emit = str_recursive ? str_recursive : str;
    if(emit && (emit[0] != '\0'))
      c = snprintf(buf, bufsize, "%s$", emit);
    else
      c = snprintf(buf, bufsize, "%%$");
    g_free(str_recursive);
    buf += c;
    bufsize -= c;
  }
  return 0;
}

void dt_collection_deserialize(const char *buf)
{
  int num_rules = 0;
  sscanf(buf, "%d", &num_rules);
  if(num_rules == 0)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
    dt_conf_set_int("plugins/lighttable/collect/mode0", 0);
    dt_conf_set_int("plugins/lighttable/collect/item0", 0);
    dt_conf_set_string("plugins/lighttable/collect/string0", "%");
    dt_conf_set_bool("plugins/lighttable/collect/recursive0", FALSE);
  }
  else
  {
    int mode = 0, item = 0;
    dt_conf_set_int("plugins/lighttable/collect/num_rules", num_rules);
    while(buf[0] != '\0' && buf[0] != ':') buf++;
    if(buf[0] == ':') buf++;
    char str[400], confname[200];
    for(int k = 0; k < num_rules; k++)
    {
      const int n = sscanf(buf, "%d:%d:%399[^$]", &mode, &item, str);
      if(n == 3)
      {
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", k);
        dt_conf_set_int(confname, mode);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", k);
        dt_conf_set_int(confname, item);
        // A FOLDERS rule's trailing '*' is the recursion marker (see dt_collection_serialize):
        // pull it back out into its own flag here, setting AND clearing so a rule slot never
        // inherits a stale flag left over from whatever collection previously occupied it.
        if(item == DT_COLLECTION_PROP_FOLDERS)
        {
          const size_t len = strlen(str);
          const gboolean recursive = len > 0 && str[len - 1] == '*';
          if(recursive) str[len - 1] = '\0';
          snprintf(confname, sizeof(confname), "plugins/lighttable/collect/recursive%1d", k);
          dt_conf_set_bool(confname, recursive);
        }
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", k);
        dt_conf_set_string(confname, str);
      }
      else if(num_rules == 1)
      {
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", k);
        dt_conf_set_int(confname, 0);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", k);
        dt_conf_set_int(confname, 0);
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", k);
        dt_conf_set_string(confname, "%");
        snprintf(confname, sizeof(confname), "plugins/lighttable/collect/recursive%1d", k);
        dt_conf_set_bool(confname, FALSE);
        break;
      }
      else
      {
        dt_conf_set_int("plugins/lighttable/collect/num_rules", k);
        break;
      }
      while(buf[0] != '$' && buf[0] != '\0') buf++;
      if(buf[0] == '$') buf++;
    }
  }
  dt_collection_update_query(dt_collection_get_global(), DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF, NULL);
}

static dt_collection_recents_handler_t _recents_handler = NULL;

void dt_collection_set_recents_handler(dt_collection_recents_handler_t handler)
{
  _recents_handler = handler;
}


void dt_collection_update_query(const dt_collection_t *collection, dt_collection_change_t query_change,
                                dt_collection_properties_t changed_property, GList *list)
{
  int next = -1;
  if(list)
  {
    // for changing offsets, thumbtable needs to know the first untouched imageid after the list
    // we do this here

    // 1. create a string with all the imgids of the list to be used inside IN sql query
    gchar *txt = NULL;
    int i = 0;
    for(GList *l = list; l; l = g_list_next(l))
    {
      const int id = GPOINTER_TO_INT(l->data);
      if(i == 0)
        txt = dt_util_dstrcat(txt, "%d", id);
      else
        txt = dt_util_dstrcat(txt, ",%d", id);
      i++;
    }
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
  }

  char confname[200];

  const int _n_r = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int num_rules = CLAMP(_n_r, 1, 10);
  char *conj[] = { "AND", "OR", "AND NOT" };

  gchar **query_parts = g_new (gchar*, num_rules + 1);
  query_parts[num_rules] =  NULL;

  for(int i = 0; i < num_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    const int property = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    gchar *text = dt_conf_get_string(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    const int mode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/recursive%1d", i);
    const gboolean recursive = dt_conf_get_bool(confname);

    if(IS_NULL_PTR(text) || text[0] == '\0')
    {
      if (mode == 1) // for OR show all
        query_parts[i] = g_strdup(" OR 1=1");
      else
        query_parts[i] = g_strdup("");
    }
    else
    {
      gchar *query = get_query_string(property, text, recursive);

      query_parts[i] =  g_strdup_printf(" %s %s", conj[mode], query);

      dt_free(query);
    }
    dt_free(text);
  }

  /* set the extended where and the use of it in the query */
  dt_collection_set_extended_where(collection, query_parts);
  g_strfreev(query_parts);
  dt_collection_set_query_flags(collection,
                                (dt_collection_get_query_flags(collection) | COLLECTION_QUERY_USE_WHERE_EXT));

  /* update query and at last the visual */
  dt_collection_update(collection);

  /* Update recent collections history before we raise the signal,
  *  since some signal listeners will need it */
  if(_recents_handler) _recents_handler();

  /* raise signal of collection change, only if this is an original */
  dt_collection_memory_update();
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_COLLECTION_CHANGED, query_change, changed_property,
                                list, next);
}

void dt_culling_mode_to_selection()
{
  // Restore everything as before
  dt_selection_pop(dt_selection_get_global());
  dt_pop_collection();
}


gboolean dt_collection_hint_message_internal(void *message)
{
  dt_control_hinter_message(dt_control_get_global(), message);
  dt_free(message);
  return FALSE;
}

void dt_collection_hint_message(const dt_collection_t *collection)
{
  /* collection hinting */
  gchar *message;

  const int c = dt_collection_get_count(collection);
  const int cs = dt_selection_get_length(dt_selection_get_global());

  if(cs == 1)
  {
    /* determine offset of the single selected image */
    GList *selected_imgids = dt_selection_get_list(dt_selection_get_global());
    int selected = -1;

    if(selected_imgids)
    {
      selected = GPOINTER_TO_INT(selected_imgids->data);
      selected = dt_collection_image_offset_with_collection(collection, selected);
      selected++;
    }
    g_list_free(selected_imgids);
    selected_imgids = NULL;
    message = g_strdup_printf(_("%d image of %d (#%d) in current collection is selected"), cs, c, selected);
  }
  else
  {
    message = g_strdup_printf(
      ngettext(
        "%d image of %d in current collection is selected",
        "%d images of %d in current collection are selected",
        cs),
      cs, c);
  }

  g_idle_add(dt_collection_hint_message_internal, message);
}

static inline void _dt_collection_change_view_after_import(const dt_view_t *current_view, gboolean open_single_image)
{
  // Studio Capture already shows every newly-imported image itself (see
  // _studio_image_imported_callback() in views/studio_capture.c), without leaving the atelier:
  // forcing a switch to darkroom/lighttable here on every auto-imported capture would fight that
  // and kick the user out of a live shooting session.
  if(!g_strcmp0(current_view->module_name, "studio_capture")) return;

  if(open_single_image)
  {
    if(!g_strcmp0(current_view->module_name, "darkroom")) // if current view IS "darkroom".
      dt_ctl_reload_view("darkroom");
    else
      dt_ctl_switch_mode_to("darkroom");
  }
  else if(g_strcmp0(current_view->module_name, "lighttable")) // if current view IS NOT "lighttable".
    dt_ctl_switch_mode_to("lighttable");
}

static inline gboolean _collection_can_switch_folder(const int32_t imgid, const dt_view_t *current_atelier)
{
  // Go out if the image is unknown.
  gboolean result = imgid == UNKNOWN_IMAGE;

  // Go out if we are not in lighttable or Studio Capture: those are the only two ateliers whose
  // filmstrip/grid should follow newly-imported images into their folder. Studio Capture's own
  // filmstrip is driven by the same dt_collection_get_global() query as lighttable's grid, so without
  // this it never picks up an auto-imported capture that lands outside the currently browsed
  // folder.
  result |= current_atelier && g_strcmp0(current_atelier->module_name, "lighttable")
            && g_strcmp0(current_atelier->module_name, "studio_capture");

  // Go out if the Collection module is not showing the "Folders" tab. Only applies to
  // lighttable, the only atelier with a Collect module UI exposing that tab: Studio Capture has
  // no such module, so this persisted, lighttable-specific tab selection must not gate it too.
  const gboolean is_lighttable = current_atelier && !g_strcmp0(current_atelier->module_name, "lighttable");
  if(is_lighttable)
    result |= dt_conf_get_int("plugins/lighttable/collect/tab") != 0;

  return result;
}

void dt_collection_load_filmroll(dt_collection_t *collection, const int32_t imgid, gboolean open_single_image)
{
  const dt_view_t *current_atelier = dt_view_manager_get_current_view(dt_view_manager_get_global());

  // Go out if conditions are not reunited
  if(_collection_can_switch_folder(imgid, current_atelier))
    return;

  gchar first_directory[PATH_MAX] = { 0 };
  dt_get_dirname_from_imgid(first_directory, imgid);

  const gboolean copy = dt_conf_get_bool("ui_last/import_copy");
  const dt_collection_properties_t Collection_view = dt_conf_get_int("plugins/lighttable/collect/item0");
  gchar dir[PATH_MAX] = { 0 };

  // - If user imports images in place and View mode is on "Tree":
  // - if the user selecter 1 folder in Import:
  //    - the lighttable displays the contents of that folder.
  //    - else, the lighttable displays the contents of the folder
  //        showing in the file explorer in Import.
  //
  // - In all other cases, the lighttable displays the first
  //    imported image's folder.

  if (Collection_view == DT_COLLECTION_PROP_FOLDERS && !copy)
  {
    int nb = dt_conf_get_int("ui_last/import_selection_nb");
    const gchar *first_selection = dt_conf_get_string_const("ui_last/import_first_selected_str");

    if(nb ==1 && dt_util_dir_exist(first_selection))
    {
      fprintf(stdout,"Collection: one folder.\n");
      g_strlcpy(dir, g_strdup(first_selection), sizeof(dir));
    }
    else
    {
      fprintf(stdout,"Collection: files and folders.\n");
      const gchar *import_last_dir = dt_conf_get_string("ui_last/import_last_directory");
      if(dt_util_dir_exist(import_last_dir))
        g_strlcpy(dir, g_strdup(import_last_dir), sizeof(dir));
    }
  }
  else // in List view or we copy
  {
    fprintf(stdout,"Collection: copy or in List view.\n");

    gchar first_img_path[PATH_MAX] = { 0 };
    dt_get_dirname_from_imgid(first_img_path, imgid);

    if(dt_util_dir_exist(first_img_path))
    {
      g_strlcpy(dir, first_img_path, sizeof(dir));
      fprintf(stdout,"Collection: ID %d, last img path %s.\n", imgid, first_img_path);
    }
  }

  // Don't append "*": it's the legacy encoding for "recursive" and would silently
  // override the user's current recursive/sub-folders setting on every import.
  dt_conf_set_string("plugins/lighttable/collect/string0", dir);
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);

  // Reload the collection with the current filmroll
  dt_collection_update_query(collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_FILMROLL, NULL);

  // Necessary to directly open in darkroom if we want to.
  dt_control_set_mouse_over_id(imgid);

  // To scroll the lighttable automatically to this image,
  // it needs to be selected.
  dt_selection_select(dt_selection_get_global(), imgid);

  // New images are untagged, that may need an update of the collection module for untagged count
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_TAG_CHANGED);

  if(current_atelier) _dt_collection_change_view_after_import(current_atelier, open_single_image);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
