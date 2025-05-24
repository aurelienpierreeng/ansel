/*
    This file is part of darktable,
    Copyright (C) 2010-2020 darktable developers.

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

#pragma once

#include <glib.h>
#include <glib/gi18n.h>
#include <inttypes.h>
#include "common/metadata.h"

#define NUM_LAST_COLLECTIONS 10

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

  DT_COLLECTION_PROP_LAST,

  DT_COLLECTION_PROP_UNDEF,
  DT_COLLECTION_PROP_SORT
} dt_collection_properties_t;

typedef enum dt_collection_change_t
{
  DT_COLLECTION_CHANGE_NONE      = 0,
  DT_COLLECTION_CHANGE_NEW_QUERY = 1, // a completly different query
  DT_COLLECTION_CHANGE_FILTER    = 2, // base query has been finetuned (filter, ...)
  DT_COLLECTION_CHANGE_RELOAD    = 3  // we have just reload the collection after images changes (query is identical)
} dt_collection_change_t;

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
  gchar *query;
  gchar **where_ext;
  unsigned int count;
  unsigned int tagid;
  dt_collection_params_t params;
  dt_collection_params_t store;
} dt_collection_t;

/* returns the name for the given collection property */
const char *dt_collection_name(dt_collection_properties_t prop);

/** instantiates a collection context */
dt_collection_t *dt_collection_new();
/** frees a collection context. */
void dt_collection_free(const dt_collection_t *collection);
/** fetch params for collection for storing. */
const dt_collection_params_t *dt_collection_params(const dt_collection_t *collection);
/** get the filtered map between sanitized makermodel and exif maker/model **/
void dt_collection_get_makermodels(const gchar *filter, GList **sanitized, GList **exif);
/** get the sanitized makermodel for exif maker/model **/
gchar *dt_collection_get_makermodel(const char *exif_maker, const char *exif_model);
/** get the generated query for collection */
const gchar *dt_collection_get_query(const dt_collection_t *collection);
/** updates sql query for a collection. @return 1 if query changed. */
int dt_collection_update(const dt_collection_t *collection);
/** reset collection to default dummy selection */
void dt_collection_reset(const dt_collection_t *collection);
/** gets an extended where part */
gchar *dt_collection_get_extended_where(const dt_collection_t *collection, int exclude);
/** sets an extended where part */
void dt_collection_set_extended_where(const dt_collection_t *collection, gchar **extended_where);

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

/** load a filmroll-based collection from an imgid */
void dt_collection_load_filmroll(dt_collection_t *collection, const int32_t imgid, gboolean open_single_image);

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

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
