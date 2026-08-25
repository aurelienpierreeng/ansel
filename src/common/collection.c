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
    Copyright (C) 2024-2026 Guillaume Stutin.
    
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
#include "common/paths.h"   // DT_PATH_MAX
#include "control/settings.h"
#include "database/collection_query.h"
#include "metadata/colorlabels.h"
#include "common/image.h"
#include "imageio/imageio_core.h"
#include "develop/iop_order.h"
#include "metadata/metadata.h"
#include "common/utility.h"
#include "metadata/map_locations.h"
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


/* Stores the collection query, returns 1 if changed.. */
/* Counts the number of images in the current collection */

/* determine image offset of specified imgid for the given collection */
static int dt_collection_image_offset_with_collection(const dt_collection_t *collection, int32_t imgid);

static int _resolve_iop_order_name(const char *text)
{
  for(int i = 0; i < DT_IOP_ORDER_LAST; i++)
    if(strcmp(text, _(dt_iop_order_string(i))) == 0) return i;
  return -1;
}

/** The module-order names, in the order their versions are stored. Built once: gettext owns the
 *  strings, so the array can be handed over and borrowed. */
static const char *_order_names[DT_IOP_ORDER_LAST];

/* The application-wide collection.
 *
 * Owned HERE, not by darktable_t, and created by the GUI bootstrap (dt_gui_gtk_init())
 * rather than by dt_init(): the collection is the lighttable's query state, and ansel-cli
 * has no lighttable. Instantiating it in dt_init() made every CLI run pay for the user's
 * collection query against their whole library before exporting a single pixel.
 *
 * In a GUI-less process the accessor therefore returns NULL, and every public entry point
 * of this module treats a NULL collection as "the module is not up": a query update is a
 * no-op, a count is 0. The guard lives at this module boundary ONCE, so callers shared
 * between GUI and CLI (film import, image removal) stay free of if(darktable.gui) tests.
 */
static dt_collection_t *_collection_global = NULL;

dt_collection_t *dt_collection_get_global(void)
{
  return _collection_global;
}

void dt_collection_init_global(void)
{
  if(IS_NULL_PTR(_collection_global)) _collection_global = dt_collection_new();
}

void dt_collection_cleanup_global(void)
{
  dt_collection_free(_collection_global);
  _collection_global = NULL;
}

dt_collection_t *dt_collection_new()
{
  dt_collection_t *collection = g_malloc0(sizeof(dt_collection_t));

  // The database module composes the collection query but cannot translate, so give it the two
  // things it needs from this layer: how to read a module-order name, and what they are called.
  for(int i = 0; i < DT_IOP_ORDER_LAST; i++) _order_names[i] = _(dt_iop_order_string(i));
  dt_collection_query_set_order_names(_order_names, DT_IOP_ORDER_LAST);
  dt_collection_query_set_order_resolver(_resolve_iop_order_name);

  dt_collection_reset(collection);
  return collection;
}

void dt_collection_free(const dt_collection_t *collection)
{
  if(IS_NULL_PTR(collection)) return;
  dt_free(collection->params.text_filter);
  for(int i = 0; i < collection->n_rules; i++)
  {
    gchar *t = (gchar *)collection->rules[i].text;
    dt_free(t);
  }
  dt_free(collection->rules);

  // The composed query, its cached statements and the copy of the rules belong to the database
  // module now.
  dt_collection_query_cleanup();
  dt_free(collection);
}

const dt_collection_params_t *dt_collection_params(const dt_collection_t *collection)
{
  if(IS_NULL_PTR(collection)) return NULL;
  return &collection->params;
}


// Return a pointer to a static string for an "AND" operator if the
// number of terms processed so far requires it.  The variable used
// for term should be an int initialized to and_operator_initial()
// before use.


int dt_collection_update(const dt_collection_t *collection)
{
  if(IS_NULL_PTR(collection)) return 0;
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
  return dt_collection_query_set_rules(&collection->params, collection->rules, collection->n_rules,
                                      collection->tagid);
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
  if(IS_NULL_PTR(collection)) return;
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
  if(IS_NULL_PTR(collection)) return COLLECTION_FILTER_ALL;
  return collection->params.filter_flags;
}

void dt_collection_set_filter_flags(const dt_collection_t *collection, dt_collection_filter_flag_t flags)
{
  if(IS_NULL_PTR(collection)) return;
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->filter_flags = flags;
}

char *dt_collection_get_text_filter(const dt_collection_t *collection)
{
  if(IS_NULL_PTR(collection)) return NULL;
  return collection->params.text_filter;
}

void dt_collection_set_text_filter(const dt_collection_t *collection, char *text_filter)
{
  if(IS_NULL_PTR(collection)) { dt_free(text_filter); return; } // takes ownership even when down
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  dt_free(params->text_filter);
  params->text_filter = text_filter;
}

dt_collection_query_flags_t dt_collection_get_query_flags(const dt_collection_t *collection)
{
  if(IS_NULL_PTR(collection)) return COLLECTION_QUERY_FULL;
  return collection->params.query_flags;
}

void dt_collection_set_query_flags(const dt_collection_t *collection, dt_collection_query_flags_t flags)
{
  if(IS_NULL_PTR(collection)) return;
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;
  params->query_flags = flags;
}

void dt_collection_set_rules(const dt_collection_t *collection, const dt_collection_rule_t *rules,
                             const int n_rules)
{
  if(IS_NULL_PTR(collection)) return;
  dt_collection_t *c = (dt_collection_t *)collection;

  for(int i = 0; i < c->n_rules; i++)
  {
    gchar *t = (gchar *)c->rules[i].text;
    dt_free(t);
  }
  dt_free(c->rules);

  c->rules = g_malloc0_n(MAX(n_rules, 1), sizeof(dt_collection_rule_t));
  c->n_rules = n_rules;
  for(int i = 0; i < n_rules; i++)
  {
    c->rules[i] = rules[i];
    c->rules[i].text = rules[i].text ? g_strdup(rules[i].text) : NULL;
  }

  dt_collection_update(collection);
}

void dt_collection_set_tag_id(dt_collection_t *collection, const uint32_t tagid)
{
  if(IS_NULL_PTR(collection)) return;
  collection->tagid = tagid;
}

void dt_collection_set_sort(const dt_collection_t *collection, dt_collection_sort_t sort, gboolean reverse)
{
  if(IS_NULL_PTR(collection)) return;
  dt_collection_params_t *params = (dt_collection_params_t *)&collection->params;

  if(sort != DT_COLLECTION_SORT_NONE)
    params->sort = sort;

  if(reverse != -1) params->descending = reverse;
}

dt_collection_sort_t dt_collection_get_sort_field(const dt_collection_t *collection)
{
  if(IS_NULL_PTR(collection)) return DT_COLLECTION_SORT_NONE;
  return collection->params.sort;
}

gboolean dt_collection_get_sort_descending(const dt_collection_t *collection)
{
  if(IS_NULL_PTR(collection)) return FALSE;
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
  dt_collection_query_get_makermodels(filter, sanitized, exif);
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

GList *dt_collection_get_images_for_rule(const dt_collection_properties_t property, const char *text,
                                         gboolean recursive)
{
  return dt_collection_query_get_images_for_rule(property, text, recursive);
}

void dt_collection_name_value_free(gpointer value)
{
  dt_collection_name_value_t *v = (dt_collection_name_value_t *)value;
  if(!v) return;
  g_free(v->name);
  g_free(v);
}


GList *dt_collection_get_property_values(const dt_collection_properties_t property, const int rule)
{
  // Whether the edited rule excludes itself from the value list is a property of how that rule
  // is configured -- an OR rule does not limit the collection, so it must not limit the choices
  // either. That decision is made here, where conf is readable, and passed as a fact.
  gboolean apply_exclude = FALSE;
  if(rule >= 0)
  {
    char confname[200];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", rule);
    apply_exclude = (dt_conf_get_int(confname) != 1);
  }

  // The remaining preferences the value list depends on, resolved here where conf is readable.
  gboolean metadata_hidden = FALSE;
  if(property >= DT_COLLECTION_PROP_METADATA && property < DT_COLLECTION_PROP_METADATA + DT_METADATA_NUMBER)
  {
    const int keyid = dt_metadata_get_keyid_by_display_order(property - DT_COLLECTION_PROP_METADATA);
    const char *name = (const char *)dt_metadata_get_name(keyid);
    char *setting = g_strdup_printf("plugins/lighttable/metadata/%s_flag", name);
    metadata_hidden = dt_conf_get_int(setting) & DT_METADATA_FLAG_HIDDEN;
    g_free(setting);
  }

  const char *filmroll_sort = dt_conf_get_string_const("plugins/collect/filmroll_sort");
  const char *filmroll_order_by =
      (strcmp(filmroll_sort, "id") == 0)
          ? "film_rolls_id DESC"
          : (dt_conf_get_bool("plugins/collect/descending") ? "folder DESC" : "folder");

  const dt_collection_values_request_t req = { .property = property,
                                               .exclude_rule = rule,
                                               .apply_exclude = apply_exclude,
                                               .metadata_hidden = metadata_hidden,
                                               .filmroll_order_by = filmroll_order_by };
  return dt_collection_query_get_property_values(&req);
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
  // Import and removal paths shared with ansel-cli land here; without a GUI there is no
  // collection to re-query and nobody listening. The one guard that keeps them CLI-clean.
  if(IS_NULL_PTR(collection)) return;
  int next = -1;
  if(list)
  {
    // for changing offsets, thumbtable needs to know the first untouched imageid after the list
    // we do this here

    next = dt_collection_query_find_neighbour(list);
  }

  // Read the user's rules out of conf and hand them over as RULES. Composing SQL from them is
  // the database module's; this is the only place that knows they live in conf at all.
  char confname[200];

  const int _n_r = dt_conf_get_int("plugins/lighttable/collect/num_rules");
  const int num_rules = CLAMP(_n_r, 1, 10);

  dt_collection_rule_t *rules = g_malloc0_n(num_rules, sizeof(dt_collection_rule_t));
  gchar **texts = g_malloc0_n(num_rules, sizeof(gchar *));

  for(int i = 0; i < num_rules; i++)
  {
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/item%1d", i);
    rules[i].property = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/string%1d", i);
    texts[i] = dt_conf_get_string(confname);
    rules[i].text = texts[i];
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/mode%1d", i);
    rules[i].mode = dt_conf_get_int(confname);
    snprintf(confname, sizeof(confname), "plugins/lighttable/collect/recursive%1d", i);
    rules[i].recursive = dt_conf_get_bool(confname);
  }

  dt_collection_set_rules(collection, rules, num_rules);

  for(int i = 0; i < num_rules; i++) dt_free(texts[i]);
  dt_free(texts);
  dt_free(rules);

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
  if(IS_NULL_PTR(collection)) return;
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

// TRUE when there is no folder-browsing grid to talk about at all: neither lighttable nor
// Studio Capture is the current atelier, or (lighttable only) the Collect module is not showing
// the "Folders" tab. Factored out of _collection_can_switch_folder() so the atelier/tab
// eligibility rule has exactly one implementation.
static inline gboolean _collection_folder_ui_inactive(const dt_view_t *current_atelier)
{
  // Go out if we are not in lighttable or Studio Capture: those are the only two ateliers whose
  // filmstrip/grid should follow newly-imported images into their folder. Studio Capture's own
  // filmstrip is driven by the same dt_collection_get_global() query as lighttable's grid, so without
  // this it never picks up an auto-imported capture that lands outside the currently browsed
  // folder.
  gboolean result = current_atelier && g_strcmp0(current_atelier->module_name, "lighttable")
                    && g_strcmp0(current_atelier->module_name, "studio_capture");

  // Go out if the Collection module is not showing the "Folders" tab. Only applies to
  // lighttable, the only atelier with a Collect module UI exposing that tab: Studio Capture has
  // no such module, so this persisted, lighttable-specific tab selection must not gate it too.
  const gboolean is_lighttable = current_atelier && !g_strcmp0(current_atelier->module_name, "lighttable");
  if(is_lighttable)
    result |= dt_conf_get_int("plugins/lighttable/collect/tab") != 0;

  return result;
}

gboolean dt_collection_get_browsed_folder(gchar *folder, size_t len, gboolean *recursive)
{
  const dt_view_t *current_atelier = dt_view_manager_get_current_view(dt_view_manager_get_global());
  if(_collection_folder_ui_inactive(current_atelier))
    return FALSE;

  // tab == 0 (checked above) guarantees item0 is DT_COLLECTION_PROP_FOLDERS or _FILMROLL: the
  // Collect module's Folders tab always forces one of the two on rule 0 (libs/collect.c). Only
  // the Tree view (FOLDERS) supports recursion -- the flat List view (FILMROLL) never appends a
  // sub-folder wildcard to its query (database/collection_query.c), so recursive0 is meaningless
  // there even if it's still set from a previous Tree-view session.
  const gboolean is_folders = dt_conf_get_int("plugins/lighttable/collect/item0") == DT_COLLECTION_PROP_FOLDERS;
  *recursive = is_folders && dt_conf_get_bool("plugins/lighttable/collect/recursive0");

  gchar *string0 = dt_conf_get_string("plugins/lighttable/collect/string0");
  const gboolean has_value = string0 && string0[0];
  if(has_value) g_strlcpy(folder, string0, len);
  dt_free(string0);
  return has_value;
}

void dt_collection_notify_imported(const int32_t imgid, const gchar *known_image_folder, gint64 *last_refresh_us)
{
  // Throttled: every import job feeds this one image at a time, and a full
  // dt_collection_update_query() is expensive enough (rebuilds memory.collected_images, triggers
  // a full lighttable/thumbtable re-layout on the GUI thread) that firing it after every single
  // image kept the GUI thread permanently busy processing the backlog on a large import.
  const gint64 now = g_get_monotonic_time();
  if(now - *last_refresh_us <= 250000) return; // 250ms
  *last_refresh_us = now;

  // Read fresh on every throttle-admitted call (so at most 4/s, not per image) rather than once
  // before the import loop starts: the user can change which folder is browsed while a long
  // import is still running, and a stale snapshot would keep comparing against wherever they
  // were looking when the job began, silently going quiet on the folder they navigated to.
  gchar browsed_folder[DT_PATH_MAX] = { 0 };
  gboolean browsed_folder_recursive = FALSE;
  const gboolean has_browsed_folder
      = dt_collection_get_browsed_folder(browsed_folder, sizeof(browsed_folder), &browsed_folder_recursive);

  // Unknown scope (not browsing a single folder/film-roll): can't tell whether imgid is
  // relevant, so always do the real thing -- same as before this function existed.
  gboolean image_in_browsed_folder = TRUE;
  gchar image_folder_buf[DT_PATH_MAX] = { 0 };
  if(has_browsed_folder)
  {
    const gchar *image_folder = known_image_folder;
    if(!image_folder)
    {
      dt_get_dirname_from_imgid(image_folder_buf, imgid);
      image_folder = image_folder_buf;
    }
    const size_t browsed_len = strlen(browsed_folder);
    image_in_browsed_folder = !g_strcmp0(image_folder, browsed_folder)
      || (browsed_folder_recursive && g_str_has_prefix(image_folder, browsed_folder)
          && image_folder[browsed_len] == G_DIR_SEPARATOR);
  }

  // Both branches below label the change DT_COLLECTION_CHANGE_BACKGROUND_SYNC: a background
  // import heartbeat is never the kind of event that should steal the user's scroll/focus or
  // reset grid/zoom preferences, whether or not it actually touches what's on screen. Listeners
  // that key off query_change to guard exactly that -- gui/dtgtk/thumbtable.c's
  // _dt_collection_changed_callback() (skips dt_thumbtable_schedule_focus(), still rebuilds grid
  // content off its own hash) and libs/tools/lighttable.c's same-named callback (skips its
  // unconditional zoom-reset entirely) -- both recognize it as "not a real [navigational] change".
  // query_change never affects dt_collection_update_query()'s own re-sync work, only what it
  // hands listeners, so this is free to pick regardless of which branch runs.
  if(image_in_browsed_folder)
  {
    // imgid lands in (or, if recursive, under) the browsed folder: a real
    // dt_collection_update_query() is required so memory.collected_images actually gains the new
    // image and the grid has something new to show.
    dt_collection_update_query(dt_collection_get_global(), DT_COLLECTION_CHANGE_BACKGROUND_SYNC,
                               DT_COLLECTION_PROP_UNDEF, NULL);
    return;
  }

  // imgid does not concern the folder currently browsed: skip the real re-query (it could not
  // have shown imgid there anyway) and raise the signal directly instead of going silent. Every
  // listener that keeps its own counts independently of memory.collected_images -- the Collect
  // module's tag/camera/lens lists (libs/collect.c queries main.images directly and never reads
  // memory.collected_images) -- still refreshes. gui/dtgtk/thumbtable.c's own hash (a function of
  // the query generation and memory.collected_images's row count) is untouched by this path, so
  // it does not rebuild grid content either.
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_COLLECTION_CHANGED,
                                DT_COLLECTION_CHANGE_BACKGROUND_SYNC, DT_COLLECTION_PROP_UNDEF, NULL, -1);
}

void dt_collection_load_filmroll(dt_collection_t *collection, const int32_t imgid, gboolean open_single_image,
                                 gboolean set_mouse_over)
{
  if(IS_NULL_PTR(collection)) return;
  const dt_view_t *current_atelier = dt_view_manager_get_current_view(dt_view_manager_get_global());

  // Without a real image there is nothing to switch to or open.
  if(imgid == UNKNOWN_IMAGE)
    return;

  // _collection_folder_ui_inactive() also gates on the atelier (only lighttable/Studio Capture
  // have a folder-browsing grid worth re-pointing at the import). That gate must only cover the
  // folder-following block below, not the mouse-over/selection/view-switch block that follows
  // it: those are what actually opens the newly imported image, and must run for every atelier,
  // darkroom included -- otherwise importing a single image while already in darkroom never
  // opens it, since dt_control_set_mouse_over_id() below (which darkroom's try_enter() reads to
  // pick the target image) never runs.
  if(!_collection_folder_ui_inactive(current_atelier))
  {
    // Always the folder actually containing imgid (the first successfully imported image), for
    // every case -- copy or in-place, List or Tree view. This used to special-case in-place
    // imports in Tree view: if the user had selected exactly one top-level folder to import
    // recursively, it showed THAT folder (ui_last/import_first_selected_str) instead of drilling
    // down to wherever the first image actually landed, which is surprising and unpredictable
    // for a recursive import spanning several sub-folders (the sub-folder that ends up used
    // depended on file-chooser/last-browsed-directory state left over from a previous, unrelated
    // import, not on anything about this one).
    gchar dir[DT_PATH_MAX] = { 0 };
    dt_get_dirname_from_imgid(dir, imgid);
    if(!dt_util_dir_exist(dir)) dir[0] = 0;

    // Don't append "*": it's the legacy encoding for "recursive" and would silently
    // override the user's current recursive/sub-folders setting on every import.
    dt_conf_set_string("plugins/lighttable/collect/string0", dir);
    dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);

    // Reload the collection with the current filmroll
    dt_collection_update_query(collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_FILMROLL, NULL);
  }

  // Necessary to directly open in darkroom if we want to. Skippable: a caller that already
  // pointed mouse_over_id at imgid earlier (e.g. right when it was first known, before a long
  // import job finishes) does not want it forced back here, possibly clobbering whatever the
  // user is hovering by now.
  if(set_mouse_over) dt_control_set_mouse_over_id(imgid);

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
