/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2012 johannes hanika.
    Copyright (C) 2011 Simon Spannagel.
    Copyright (C) 2011-2014, 2016-2017 Tobias Ellinghaus.
    Copyright (C) 2012 Ivan Tarozzi.
    Copyright (C) 2012 James C. McPherson.
    Copyright (C) 2012 José Carlos García Sogo.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Dennis Gnad.
    Copyright (C) 2013 Jérémy Rosen.
    Copyright (C) 2013-2015, 2019-2021 Pascal Obry.
    Copyright (C) 2013-2014, 2016 Roman Lebedev.
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2016 piterdias.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019-2022 Philippe Weyland.
    Copyright (C) 2020-2021 Aldric Renaudin.
    Copyright (C) 2020 David-Tillmann Schaefer.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    
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
#include <glib/gstdio.h>
#include "common/act_on.h"
#include "metadata/notify.h"
#include "common/metadata_export.h"
#include "common/utility.h"
#include "metadata/tags.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "common/image.h"
#include "database/tag_repository.h"
#include "common/grouping.h"
#include "common/selection.h"
#include "common/undo.h"
#include "common/conf.h"
#include <glib.h>
#if defined (_WIN32)
#include "win/getdelim.h"
#endif // defined (_WIN32)


typedef struct dt_undo_tags_t
{
  int32_t imgid;
  GList *before; // list of tagid before
  GList *after; // list of tagid after
} dt_undo_tags_t;

static gchar *_get_tb_removed_tag_string_values(GList *before, GList *after)
{
  GList *a = after;
  gchar *tag_list = NULL;
  for(GList *b = before; b; b = g_list_next(b))
  {
    if(!g_list_find(a, b->data))
    {
      tag_list = dt_util_dstrcat(tag_list, "%d,", GPOINTER_TO_INT(b->data));
    }
  }
  if(tag_list) tag_list[strlen(tag_list) - 1] = '\0';
  return tag_list;
}

static gchar *_get_tb_added_tag_string_values(const int img, GList *before, GList *after)
{
  GList *b = before;
  gchar *tag_list = NULL;
  for(GList *a = after; a; a = g_list_next(a))
  {
    if(!g_list_find(b, a->data))
    {
      // clang-format off
      tag_list = dt_util_dstrcat(tag_list,
                                 "(%d,%d,"
                                 "  (SELECT (IFNULL(MAX(position),0) & 0xFFFFFFFF00000000) + (1 << 32)"
                                 "    FROM main.tagged_images)"
                                 "),",
                                 GPOINTER_TO_INT(img),
                                 GPOINTER_TO_INT(a->data));
      // clang-format on
    }
  }
  if(tag_list) tag_list[strlen(tag_list) - 1] = '\0';
  return tag_list;
}

static void _bulk_remove_tags(const int img, const gchar *tag_list)
{
  dt_tag_repository_detach_batch(img, tag_list);
}

static void _bulk_add_tags(const gchar *tag_list)
{
  dt_tag_repository_attach_batch(tag_list);
}

static void _pop_undo_execute(const int32_t imgid, GList *before, GList *after)
{
  gchar *tobe_removed_list = _get_tb_removed_tag_string_values(before, after);
  gchar *tobe_added_list = _get_tb_added_tag_string_values(imgid, before, after);

  _bulk_remove_tags(imgid, tobe_removed_list);
  _bulk_add_tags(tobe_added_list);

  dt_free(tobe_removed_list);
  dt_free(tobe_added_list);
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_TAGS)
  {
    for(GList *list = (GList *)data; list; list = g_list_next(list))
    {
      dt_undo_tags_t *undotags = (dt_undo_tags_t *)list->data;

      GList *before = (action == DT_ACTION_UNDO) ? undotags->after : undotags->before;
      GList *after = (action == DT_ACTION_UNDO) ? undotags->before : undotags->after;
      _pop_undo_execute(undotags->imgid, before, after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undotags->imgid));
    }

    dt_metadata_tags_changed();
  }
}

static void _undo_tags_free(gpointer data)
{
  dt_undo_tags_t *undotags = (dt_undo_tags_t *)data;
  g_list_free(undotags->before);
  undotags->before = NULL;
  g_list_free(undotags->after);
  undotags->after = NULL;
  dt_free(undotags);
}

static void _tags_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free_full(l, _undo_tags_free);
  l = NULL;
}

gboolean dt_tag_new(const char *name, guint *tagid)
{
  if(IS_NULL_PTR(name) || name[0] == '\0') return FALSE; // no tagid name.

  guint id = dt_tag_repository_find_by_name(name);
  if(id)
  {
    // tagid already exists.
    if(!IS_NULL_PTR(tagid)) *tagid = id;
    return TRUE;
  }

  id = dt_tag_repository_insert(name);

  if(id && g_strstr_len(name, -1, "darktable|") == name)
    dt_tag_repository_mark_internal(id);

  if(!IS_NULL_PTR(tagid))
    *tagid = id;

  return TRUE;
}

gboolean dt_tag_new_from_gui(const char *name, guint *tagid)
{
  const gboolean ret = dt_tag_new(name, tagid);
  /* if everything went fine, raise signal of tags change to refresh keywords module in GUI */
  if(ret) dt_metadata_tags_changed();
  return ret;
}

guint dt_tag_remove(const guint tagid, gboolean final)
{
  const int count = dt_tag_repository_count_attachments(tagid);

  if(final == TRUE)
    dt_tag_repository_delete(tagid);

  return count;
}

void dt_tag_delete_tag_batch(const char *flatlist)
{
  dt_tag_repository_delete_batch(flatlist);
  dt_set_darktable_tags();
}

guint dt_tag_remove_list(GList *tag_list)
{
  if (!tag_list) return 0;

  char *flatlist = NULL;
  guint count = 0;
  guint tcount = 0;
  for (GList *taglist = tag_list; taglist ; taglist = g_list_next(taglist))
  {
    const guint tagid = ((dt_tag_t *)taglist->data)->id;
    flatlist = dt_util_dstrcat(flatlist, "%u,", tagid);
    count++;
    if(flatlist && count > 1000)
    {
      flatlist[strlen(flatlist)-1] = '\0';
      dt_tag_delete_tag_batch(flatlist);
      dt_free(flatlist);
      tcount = tcount + count;
      count = 0;
    }
  }
  if(flatlist)
  {
    flatlist[strlen(flatlist)-1] = '\0';
    dt_tag_delete_tag_batch(flatlist);
    dt_free(flatlist);
    tcount = tcount + count;
  }
  return tcount;
}

gchar *dt_tag_get_name(const guint tagid)
{
  return dt_tag_repository_get_name(tagid);
}

void dt_tag_rename(const guint tagid, const gchar *new_tagname)
{
  if(IS_NULL_PTR(new_tagname) || !new_tagname[0]) return;
  if(dt_tag_exists(new_tagname, NULL)) return;

  dt_tag_repository_rename(tagid, new_tagname);
}

gboolean dt_tag_exists(const char *name, guint *tagid)
{
  const guint id = dt_tag_repository_find_by_name(name);

  if(id)
  {
    if(!IS_NULL_PTR(tagid)) *tagid = id;
    return TRUE;
  }

  if(!IS_NULL_PTR(tagid)) *tagid = -1;
  return FALSE;
}

static gboolean _tag_add_tags_to_list(GList **list, const GList *tags)
{
  gboolean res = FALSE;
  for(const GList *t = tags; t; t = g_list_next(t))
  {
    if(!g_list_find(*list, t->data))
    {
      *list = g_list_prepend(*list, t->data);
      res = TRUE;
    }
  }
  return res;
}

static gboolean _tag_remove_tags_from_list(GList **list, const GList *tags)
{
  const int nb_ini = g_list_length(*list);
  for(const GList *t = tags; t; t = g_list_next(t))
  {
    *list = g_list_remove(*list, t->data);
  }
  return (g_list_length(*list) != nb_ini);
}

typedef enum dt_tag_type_t
{
  DT_TAG_TYPE_DT,
  DT_TAG_TYPE_USER,
  DT_TAG_TYPE_ALL,
} dt_tag_type_t;

typedef enum dt_tag_actions_t
{
  DT_TA_ATTACH = 0,
  DT_TA_DETACH,
  DT_TA_SET,
  DT_TA_SET_ALL,
} dt_tag_actions_t;

static GList *_tag_get_tags(const int32_t imgid, const dt_tag_type_t type);

static gboolean _tag_execute(const GList *tags, const GList *imgs, GList **undo, const gboolean undo_on,
                             const gint action)
{
  gboolean res = FALSE;
  for(const GList *images = imgs; images; images = g_list_next(images))
  {
    const int32_t image_id = GPOINTER_TO_INT(images->data);
    dt_undo_tags_t *undotags = (dt_undo_tags_t *)malloc(sizeof(dt_undo_tags_t));
    undotags->imgid = image_id;
    undotags->before = _tag_get_tags(image_id, DT_TAG_TYPE_ALL);
    switch(action)
    {
      case DT_TA_ATTACH:
        undotags->after = g_list_copy(undotags->before);
        if(_tag_add_tags_to_list(&undotags->after, tags)) res = TRUE;
        break;
      case DT_TA_DETACH:
        undotags->after = g_list_copy(undotags->before);
        if(_tag_remove_tags_from_list(&undotags->after, tags)) res = TRUE;
        break;
      case DT_TA_SET:
        undotags->after = g_list_copy((GList *)tags);
        // preserve dt tags
        GList *dttags = _tag_get_tags(image_id, DT_TAG_TYPE_DT);
        if(dttags) undotags->after = g_list_concat(undotags->after, dttags);
        res = TRUE;
        break;
      case DT_TA_SET_ALL:
        undotags->after = g_list_copy((GList *)tags);
        res = TRUE;
        break;
      default:
        undotags->after = g_list_copy(undotags->before);
        res = FALSE;
        break;
    }
    _pop_undo_execute(image_id, undotags->before, undotags->after);
    if(undo_on)
      *undo = g_list_append(*undo, undotags);
    else
      _undo_tags_free(undotags);
  }
  return res;
}

gboolean dt_tag_attach_images(const guint tagid, const GList *img, const gboolean undo_on)
{
  if(IS_NULL_PTR(img)) return FALSE;
  GList *undo = NULL;
  GList *tags = NULL;

  tags = g_list_prepend(tags, GINT_TO_POINTER(tagid));
  if(undo_on) dt_undo_start_group(dt_undo_get_global(), DT_UNDO_TAGS);

  const gboolean res = _tag_execute(tags, img, &undo, undo_on, DT_TA_ATTACH);

  g_list_free(tags);
  tags = NULL;
  if(undo_on)
  {
    dt_undo_record(dt_undo_get_global(), NULL, DT_UNDO_TAGS, undo, _pop_undo, _tags_undo_data_free);
    dt_undo_end_group(dt_undo_get_global());
  }

  return res;
}

gboolean dt_tag_attach(const guint tagid, const int32_t imgid, const gboolean undo_on, const gboolean group_on)
{
  gboolean res = FALSE;
  if(imgid == UNKNOWN_IMAGE)
  {
    GList *imgs = dt_act_on_get_images();
    res = dt_tag_attach_images(tagid, imgs, undo_on);
    g_list_free(imgs);
    imgs = NULL;
  }
  else
  {
    if(dt_is_tag_attached(tagid, imgid)) return FALSE;
    GList *imgs = g_list_append(NULL, GINT_TO_POINTER(imgid));
    res = dt_tag_attach_images(tagid, imgs, undo_on);
    g_list_free(imgs);
    imgs = NULL;
  }
  return res;
}

gboolean dt_tag_set_tags(const GList *tags, const GList *img, const gboolean ignore_dt_tags,
                         const gboolean clear_on, const gboolean undo_on)
{
  if(img)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(dt_undo_get_global(), DT_UNDO_TAGS);

    const gboolean res = _tag_execute(tags, img, &undo, undo_on,
                                      clear_on ? ignore_dt_tags ? DT_TA_SET : DT_TA_SET_ALL : DT_TA_ATTACH);
    if(undo_on)
    {
      dt_undo_record(dt_undo_get_global(), NULL, DT_UNDO_TAGS, undo, _pop_undo, _tags_undo_data_free);
      dt_undo_end_group(dt_undo_get_global());
    }
    return res;
  }
  return FALSE;
}

gboolean dt_tag_attach_string_list(const gchar *tags, const GList *img, const gboolean undo_on)
{
  // tags may not exist yet
  // undo only undoes the tags attachments. it doesn't remove created tags.
  gchar **tokens = g_strsplit(tags, ",", 0);
  gboolean res = FALSE;
  if(tokens)
  {
    // tag(s) creation
    GList *tagl = NULL;
    gchar **entry = tokens;
    while(*entry)
    {
      char *e = g_strstrip(*entry);
      if(*e)
      {
        guint tagid = 0;
        dt_tag_new(e, &tagid);
        tagl = g_list_prepend(tagl, GINT_TO_POINTER(tagid));
      }
      entry++;
    }

    // attach newly created tags
    if(img)
    {
      GList *undo = NULL;
      if(undo_on) dt_undo_start_group(dt_undo_get_global(), DT_UNDO_TAGS);

      res = _tag_execute(tagl, img, &undo, undo_on, DT_TA_ATTACH);

      if(undo_on)
      {
        dt_undo_record(dt_undo_get_global(), NULL, DT_UNDO_TAGS, undo, _pop_undo, _tags_undo_data_free);
        dt_undo_end_group(dt_undo_get_global());
      }
    }
    g_list_free(tagl);
    tagl = NULL;
  }
  g_strfreev(tokens);
  return res;
}

gboolean dt_tag_detach_images(const guint tagid, const GList *img, const gboolean undo_on)
{
  if(img)
  {
    GList *tags = NULL;
    tags = g_list_prepend(tags, GINT_TO_POINTER(tagid));
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(dt_undo_get_global(), DT_UNDO_TAGS);

    const gboolean res = _tag_execute(tags, img, &undo, undo_on, DT_TA_DETACH);

    g_list_free(tags);
    tags = NULL;
    if(undo_on)
    {
      dt_undo_record(dt_undo_get_global(), NULL, DT_UNDO_TAGS, undo, _pop_undo, _tags_undo_data_free);
      dt_undo_end_group(dt_undo_get_global());
    }
    return res;
  }
  return FALSE;
}

gboolean dt_tag_detach(const guint tagid, const int32_t imgid, const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  if(imgid == UNKNOWN_IMAGE)
    imgs = dt_act_on_get_images();
  else
    imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));
  if(group_on) dt_grouping_add_grouped_images(&imgs);

  const gboolean res = dt_tag_detach_images(tagid, imgs, undo_on);
  g_list_free(imgs);
  imgs = NULL;
  return res;
}

gboolean dt_tag_detach_by_string(const char *name, const int32_t imgid, const gboolean undo_on,
                                 const gboolean group_on)
{
  if(IS_NULL_PTR(name) || !name[0]) return FALSE;
  guint tagid = 0;
  if(!dt_tag_exists(name, &tagid)) return FALSE;

  return dt_tag_detach(tagid, imgid, undo_on, group_on);
}

void dt_set_darktable_tags()
{
  dt_tag_repository_rebuild_internal();
}

uint32_t dt_tag_get_attached(const int32_t imgid, GList **result, const gboolean ignore_dt_tags)
{
  uint32_t nb_selected = 0;
  if(imgid > 0)
  {
    nb_selected = 1;
  }
  else
  {
    nb_selected = dt_selection_get_length(dt_selection_get_global());
  }

  uint32_t count = 0;
  if(imgid > 0 || nb_selected > 0)
  {
    GList *tags = dt_tag_repository_get_attached(imgid, ignore_dt_tags);

    // Create result
    *result = NULL;
    for(GList *l = tags; l; l = g_list_next(l))
    {
      dt_tag_t *t = (dt_tag_t *)l->data;
      t->leave = g_strrstr(t->tag, "|");
      t->leave = t->leave ? t->leave + 1 : t->tag;
      const uint32_t imgnb = t->count;
      t->select = (nb_selected == 0) ? DT_TS_NO_IMAGE :
                  (imgnb == nb_selected) ? DT_TS_ALL_IMAGES :
                  (imgnb == 0) ? DT_TS_NO_IMAGE : DT_TS_SOME_IMAGES;
      *result = g_list_append(*result, t);
      count++;
    }
    g_list_free(tags); // the elements moved into *result
  }
  return count;
}

static uint32_t _tag_get_attached_export(const int32_t imgid, GList **result)
{
  if(!(imgid > 0)) return 0;

  GList *tags = dt_tag_repository_get_attached_for_export(imgid);

  // Create result
  uint32_t count = 0;
  for(GList *l = tags; l; l = g_list_next(l))
  {
    dt_tag_t *t = (dt_tag_t *)l->data;
    t->leave = g_strrstr(t->tag, "|");
    t->leave = t->leave ? t->leave + 1 : t->tag;
    *result = g_list_append(*result, t);
    count++;
  }
  g_list_free(tags); // the elements moved into *result

  return count;
}

static gint sort_tag_by_path(gconstpointer a, gconstpointer b)
{
  const dt_tag_t *tuple_a = (const dt_tag_t *)a;
  const dt_tag_t *tuple_b = (const dt_tag_t *)b;

  return g_strcmp0(tuple_a->tag, tuple_b->tag);
}

static gint sort_tag_by_leave(gconstpointer a, gconstpointer b)
{
  const dt_tag_t *tuple_a = (const dt_tag_t *)a;
  const dt_tag_t *tuple_b = (const dt_tag_t *)b;

  return g_strcmp0(tuple_a->leave, tuple_b->leave);
}

static gint sort_tag_by_count(gconstpointer a, gconstpointer b)
{
  const dt_tag_t *tuple_a = (const dt_tag_t *)a;
  const dt_tag_t *tuple_b = (const dt_tag_t *)b;

  return (tuple_b->count - tuple_a->count);
}
// sort_type 0 = path, 1 = leave other = count
GList *dt_sort_tag(GList *tags, gint sort_type)
{
  GList *sorted_tags;
  if (sort_type <= 1)
  {
    for(GList *taglist = tags; taglist; taglist = g_list_next(taglist))
    {
      // order such that sub tags are coming directly behind their parent
      gchar *tag = ((dt_tag_t *)taglist->data)->tag;
      for(char *letter = tag; *letter; letter++)
        if(*letter == '|') *letter = '\1';
    }
    sorted_tags = g_list_sort(tags, !sort_type ? sort_tag_by_path : sort_tag_by_leave);
    for(GList *taglist = sorted_tags; taglist; taglist = g_list_next(taglist))
    {
      gchar *tag = ((dt_tag_t *)taglist->data)->tag;
      for(char *letter = tag; *letter; letter++)
        if(*letter == '\1') *letter = '|';
    }
  }
  else
  {
    sorted_tags = g_list_sort(tags, sort_tag_by_count);
  }
  return sorted_tags;
}

GList *dt_tag_get_list(int32_t imgid)
{
  GList *taglist = NULL;
  GList *tags = NULL;

  gboolean omit_tag_hierarchy = dt_conf_get_bool("omit_tag_hierarchy");

  uint32_t count = dt_tag_get_attached(imgid, &taglist, TRUE);

  if(count < 1) return NULL;

  for(GList *tag_iter = taglist; tag_iter; tag_iter = g_list_next(tag_iter))
  {
    dt_tag_t *t = (dt_tag_t *)tag_iter->data;
    gchar *value = t->tag;

    gchar **pch = g_strsplit(value, "|", -1);

    if(!IS_NULL_PTR(pch))
    {
      if(omit_tag_hierarchy)
      {
        char **iter = pch;
        for(; *iter && *(iter + 1); iter++);
        if(*iter) tags = g_list_prepend(tags, g_strdup(*iter));
      }
      else
      {
        size_t j = 0;
        while(!IS_NULL_PTR(pch[j]))
        {
          tags = g_list_prepend(tags, g_strdup(pch[j]));
          j++;
        }
      }
      g_strfreev(pch);
    }
  }

  dt_tag_free_result(&taglist);

  return dt_util_glist_uniq(tags);
}

GList *dt_tag_get_hierarchical(int32_t imgid)
{
  GList *taglist = NULL;
  GList *tags = NULL;

  int count = dt_tag_get_attached(imgid, &taglist, TRUE);

  if(count < 1) return NULL;

  for(GList *tag_iter = taglist; tag_iter; tag_iter = g_list_next(tag_iter))
  {
    dt_tag_t *t = (dt_tag_t *)tag_iter->data;
    tags = g_list_prepend(tags, g_strdup(t->tag));
  }

  dt_tag_free_result(&taglist);

  tags = g_list_reverse(tags);	// list was built in reverse order, so un-reverse it
  return tags;
}

static GList *_tag_get_tags(const int32_t imgid, const dt_tag_type_t type)
{
  char *images = NULL;
  if(imgid > 0)
    images = g_strdup_printf("%d", imgid);
  else
  {
    // we get the query used to retrieve the list of select images
    images = dt_selection_ids_to_string(dt_selection_get_global());
  }

  const dt_tag_kind_t kind = (type == DT_TAG_TYPE_ALL) ? DT_TAG_KIND_ANY
                           : (type == DT_TAG_TYPE_DT)  ? DT_TAG_KIND_INTERNAL
                                                       : DT_TAG_KIND_USER;
  GList *tags = dt_tag_repository_get_ids_for_images(images, kind);
  dt_free(images);
  return tags;
}

GList *dt_tag_get_tags(const int32_t imgid, const gboolean ignore_dt_tags)
{
  return _tag_get_tags(imgid, ignore_dt_tags ? DT_TAG_TYPE_USER : DT_TAG_TYPE_ALL);
}

static gint _is_not_exportable_tag(gconstpointer a, gconstpointer b)
{
  dt_tag_t *ta = (dt_tag_t *)a;
  dt_tag_t *tb = (dt_tag_t *)b;
  return ((g_strcmp0(ta->tag, tb->tag) == 0) &&
          ((ta->flags) & (DT_TF_CATEGORY | DT_TF_PRIVATE))) ? 0 : -1;
}

GList *dt_tag_get_list_export(int32_t imgid, int32_t flags)
{
  GList *taglist = NULL;
  GList *tags = NULL;

  gboolean omit_tag_hierarchy = flags & DT_META_OMIT_HIERARCHY;
  gboolean export_private_tags = flags & DT_META_PRIVATE_TAG;
  gboolean export_tag_synonyms = flags & DT_META_SYNONYMS_TAG;

  uint32_t count = _tag_get_attached_export(imgid, &taglist);

  if(count < 1) return NULL;
  GList *sorted_tags = dt_sort_tag(taglist, 0);
  sorted_tags = g_list_reverse(sorted_tags);

  // reset private if export private
  if(export_private_tags)
  {
    for(GList *tagt = sorted_tags; tagt; tagt = g_list_next(tagt))
    {
      dt_tag_t *t = (dt_tag_t *)tagt->data;
      t->flags &= ~DT_TF_PRIVATE;
    }
  }
  for(GList *sorted_iter = sorted_tags; sorted_iter; sorted_iter = g_list_next(sorted_iter))
  {
    dt_tag_t *t = (dt_tag_t *)sorted_iter->data;
    if ((export_private_tags || !(t->flags & DT_TF_PRIVATE))
        && !(t->flags & DT_TF_CATEGORY))
    {
      gchar *tagname = t->leave;
      tags = g_list_prepend(tags, g_strdup(tagname));

      // if not "omit tag hierarchy" the path elements are added
      // unless otherwise stated (defined as category or private)
      if(!omit_tag_hierarchy)
      {
        GList *next = g_list_next(sorted_iter);
        gchar *end = g_strrstr(t->tag, "|");
        while (end)
        {
          end[0] = '\0';
          end = g_strrstr(t->tag, "|");
          if (IS_NULL_PTR(next) ||
              !g_list_find_custom(next, t, (GCompareFunc)_is_not_exportable_tag))
          {
            const gchar *tag = end ? end + 1 : t->tag;
            tags = g_list_prepend(tags, g_strdup(tag));
          }
        }
      }

      // add synonyms as necessary
      if (export_tag_synonyms)
      {
        gchar *synonyms = t->synonym;
        if (synonyms && synonyms[0])
          {
          gchar **tokens = g_strsplit(synonyms, ",", 0);
          if(tokens)
          {
            gchar **entry = tokens;
            while(*entry)
            {
              char *e = *entry;
              if (*e == ' ') e++;
              tags = g_list_append(tags, g_strdup(e));
              entry++;
            }
          }
          g_strfreev(tokens);
        }
      }
    }
  }
  dt_tag_free_result(&sorted_tags);

  return dt_util_glist_uniq(tags);
}

GList *dt_tag_get_hierarchical_export(int32_t imgid, int32_t flags)
{
  GList *taglist = NULL;
  GList *tags = NULL;

  const int count = dt_tag_get_attached(imgid, &taglist, TRUE);

  if(count < 1) return NULL;
  const gboolean export_private_tags = flags & DT_META_PRIVATE_TAG;

  for(GList *tag_iter = taglist; tag_iter; tag_iter = g_list_next(tag_iter))
  {
    dt_tag_t *t = (dt_tag_t *)tag_iter->data;
    if (export_private_tags || !(t->flags & DT_TF_PRIVATE))
    {
      tags = g_list_prepend(tags, g_strdup(t->tag));
    }
  }

  dt_tag_free_result(&taglist);

  return g_list_reverse(tags);  // list was built in reverse order, so un-reverse it
}

gboolean dt_is_tag_attached(const guint tagid, const int32_t imgid)
{
  return dt_tag_repository_is_attached(tagid, imgid);
}

GList *dt_tag_get_images(const gint tagid)
{
  return dt_tag_repository_get_images(tagid);  // in row order, as before
}

GList *dt_tag_get_images_from_list(const GList *img, const gint tagid)
{
  char *images = NULL;
  for(GList *imgs = (GList *)img; imgs; imgs = g_list_next(imgs))
  {
    images = dt_util_dstrcat(images, "%d,",GPOINTER_TO_INT(imgs->data));
  }

  GList *result = NULL;
  if(images)
  {
    images[strlen(images) - 1] = '\0';
    result = dt_tag_repository_get_images_in_list(tagid, images);
    dt_free(images);
  }
  return result;  // the repository returns them in row order
}

uint32_t dt_tag_get_suggestions(GList **result)
{
  const uint32_t nb_selected = dt_selection_get_length(dt_selection_get_global());
  const int nb_recent = dt_conf_get_int("plugins/lighttable/tagging/nb_recent_tags");
  const uint32_t confidence = dt_conf_get_int("plugins/lighttable/tagging/confidence");
  const char *slist = dt_conf_get_string_const("plugins/lighttable/tagging/recent_tags");

  GList *tags = dt_tag_repository_get_suggestions(nb_selected, confidence, slist, nb_recent);

  uint32_t count = 0;
  for(GList *l = tags; l; l = g_list_next(l))
  {
    dt_tag_t *t = (dt_tag_t *)l->data;
    t->leave = g_strrstr(t->tag, "|");
    t->leave = t->leave ? t->leave + 1 : t->tag;
    *result = g_list_append(*result, t);
    count++;
  }
  g_list_free(tags); // the elements moved into *result

  return count;
}

void dt_tag_count_tags_images(const gchar *keyword, int *tag_count, int *img_count)
{
  dt_tag_repository_count_similar(keyword, tag_count, img_count);
}

void dt_tag_get_tags_images(const gchar *keyword, GList **tag_list, GList **img_list)
{
  dt_tag_repository_get_similar(keyword, tag_list, img_list);
}

uint32_t dt_tag_images_count(gint tagid)
{
  return dt_tag_repository_count_distinct_images(tagid);
}

uint32_t dt_tag_get_with_usage(GList **result)
{
  const uint32_t nb_selected = dt_selection_get_length(dt_selection_get_global());
  GList *tags = dt_tag_repository_get_with_usage(nb_selected);

  /* ... and create the result list to send upwards */
  uint32_t count = 0;
  for(GList *l = tags; l; l = g_list_next(l))
  {
    dt_tag_t *t = (dt_tag_t *)l->data;
    t->leave = g_strrstr(t->tag, "|");
    t->leave = t->leave ? t->leave + 1 : t->tag;
    *result = g_list_append(*result, t);
    count++;
  }
  g_list_free(tags); // the elements moved into *result

  return count;
}

uint32_t dt_tag_get_collection_tags(GList **result)
{
  GList *tags = dt_tag_repository_get_collection_tags();

  uint32_t count = 0;
  for(GList *l = tags; l; l = g_list_next(l))
  {
    dt_tag_t *t = (dt_tag_t *)l->data;
    t->leave = g_strrstr(t->tag, "|");
    t->leave = t->leave ? t->leave + 1 : t->tag;
    *result = g_list_append(*result, t);
    count++;
  }
  g_list_free(tags); // the elements moved into *result

  return count;
}

static gchar *dt_cleanup_synonyms(gchar *synonyms_entry)
{
  gchar *synonyms = NULL;
  for(char *letter = synonyms_entry; *letter; letter++)
  {
    if(*letter == ';' || *letter == '\n') *letter = ',';
    if(*letter == '\r') *letter = ' ';
  }
  gchar **tokens = g_strsplit(synonyms_entry, ",", 0);
  if(tokens)
  {
    gchar **entry = tokens;
    while (*entry)
    {
      char *e = g_strstrip(*entry);
      if(*e)
      {
        synonyms = dt_util_dstrcat(synonyms, "%s, ", e);
      }
      entry++;
    }
    if (synonyms)
      synonyms[strlen(synonyms) - 2] = '\0';
  }
  g_strfreev(tokens);
  return synonyms;
}

gchar *dt_tag_get_synonyms(gint tagid)
{
  return dt_tag_repository_get_synonyms(tagid);
}

void dt_tag_set_synonyms(gint tagid, gchar *synonyms_entry)
{
  if (!synonyms_entry) return;
  char *synonyms = dt_cleanup_synonyms(synonyms_entry);

  dt_tag_repository_set_synonyms(tagid, synonyms);
  dt_free(synonyms);
}

gint dt_tag_get_flags(gint tagid)
{
  return dt_tag_repository_get_flags(tagid);
}

void dt_tag_set_flags(gint tagid, gint flags)
{
  dt_tag_repository_set_flags(tagid, flags);
}

void dt_tag_add_synonym(gint tagid, gchar *synonym)
{
  char *synonyms = dt_tag_get_synonyms(tagid);
  if (synonyms)
  {
    synonyms = dt_util_dstrcat(synonyms, ", %s", synonym);
  }
  else
  {
    synonyms = g_strdup(synonym);
  }
  dt_tag_repository_set_synonyms(tagid, synonyms);
  dt_free(synonyms);
}

static void _free_result_item(gpointer data)
{
  dt_tag_t *t = (dt_tag_t*)data;
  dt_free(t->tag);
  dt_free(t->synonym);
  dt_free(t);
}

void dt_tag_free_result(GList **result)
{
  if(result && *result)
  {
    g_list_free_full(*result, _free_result_item);
    *result = NULL;
  }
}

uint32_t dt_tag_get_recent_used(GList **result)
{
  return 0;
}

/*
  TODO
  the file format allows to specify {synonyms} that are one hierarchy level deeper than the parent. those are not
  to be shown in the gui but can be searched. when the parent or a synonym is attached then ALSO the rest of the
  bunch is to be added.
  there is also a ~ prefix for tags that indicate that the tag order has to be kept instead of sorting them. that's
  also not possible at the moment.
*/
uint32_t dt_tag_import(const char *filename)
{
  FILE *fd = g_fopen(filename, "r");
  if(IS_NULL_PTR(fd)) return -1;

  GList * hierarchy = NULL;
  char *line = NULL;
  size_t len = 0;
  uint32_t count = 0;
  guint tagid = 0;
  guint previous_category_depth = 0;
  gboolean previous_category = FALSE;
  gboolean previous_synonym = FALSE;

  while(getline(&line, &len, fd) != -1)
  {
    // remove newlines and set start past the initial tabs
    char *start = line;
    while(*start == '\t' || *start == ' ' || *start == ',' || *start == ';') start++;
    const int depth = start - line;

    char *end = line + strlen(line) - 1;
    while((*end == '\n' || *end == '\r' || *end == ',' || *end == ';') && end >= start)
    {
      *end = '\0';
      end--;
    }

    // remove control characters from the string
    // if no associated synonym the previous category node can be reused
    gboolean skip = FALSE;
    gboolean category = FALSE;
    gboolean synonym = FALSE;
    if (*start == '[' && *end == ']') // categories
    {
      category = TRUE;
      start++;
      *end-- = '\0';
    }
    else if (*start == '{' && *end == '}')  // synonyms
    {
      synonym = TRUE;
      start++;
      *end-- = '\0';
    }
    if(*start == '~') // fixed order. TODO not possible with our db
    {
      skip = TRUE;
      start++;
    }

    if (synonym)
    {
      // associate the synonym to last tag
      if (tagid)
      {
        char *tagname = g_strdup(start);
        // clear synonyms before importing the new ones => allows export, modification and back import
        if (!previous_synonym) dt_tag_set_synonyms(tagid, "");
        dt_tag_add_synonym(tagid, tagname);
        dt_free(tagname);
      }
    }
    else
    {
      // remove everything past the current prefix from hierarchy
      GList *iter = g_list_nth(hierarchy, depth);
      while(iter)
      {
        GList *current = iter;
        iter = g_list_next(iter);
        hierarchy = g_list_delete_link(hierarchy, current);
      }

      // add the current level
      hierarchy = g_list_append(hierarchy, g_strdup(start));

      // add tag to db iff it's not something to be ignored
      if(!skip)
      {
        char *tag = dt_util_glist_to_str("|", hierarchy);
        if (previous_category && (depth > previous_category_depth + 1))
        {
          // reuse previous tag
          dt_tag_rename(tagid, tag);
          if (!category)
            dt_tag_set_flags(tagid, 0);
        }
        else
        {
          // create a new tag
          count++;
          tagid = 1;  // if 0, dt_tag_new creates a new one even if  the tag already exists
          dt_tag_new(tag, &tagid);
          if (category)
            dt_tag_set_flags(tagid, DT_TF_CATEGORY);
        }
        dt_free(tag);
      }
    }
    previous_category_depth = category ? depth : 0;
    previous_category = category;
    previous_synonym = synonym;
  }

  dt_free(line);
  g_list_free_full(hierarchy, dt_free_gpointer);
  hierarchy = NULL;
  fclose(fd);

  dt_metadata_tags_changed();

  return count;
}

/*
  TODO: there is one corner case where i am not sure if we are doing the correct thing. some examples i found
  on the internet agreed with this version, some used an alternative:
  consider two tags like "foo|bar" and "foo|bar|baz". the "foo|bar" part is both a regular tag (from the 1st tag)
  and also a category (from the 2nd tag). the two way to output are

  [foo]
      bar
          baz

  and

  [foo]
      bar
      [bar]
          baz

  we are using the first (mostly because it was easier to implement ;)). if this poses problems with other programs
  supporting these files then we should fix that.
*/
uint32_t dt_tag_export(const char *filename)
{
  FILE *fd = g_fopen(filename, "w");

  if(IS_NULL_PTR(fd)) return -1;

  GList *tags = NULL;
  gint count = 0;
  dt_tag_get_with_usage(&tags);
  GList *sorted_tags = dt_sort_tag(tags, 0);

  gchar **hierarchy = NULL;
  for(GList *tag_elt = sorted_tags; tag_elt; tag_elt = g_list_next(tag_elt))
  {
    const gchar *tag = ((dt_tag_t *)tag_elt->data)->tag;
    const char *synonyms = ((dt_tag_t *)tag_elt->data)->synonym;
    const guint flags = ((dt_tag_t *)tag_elt->data)->flags;
    gchar **tokens = g_strsplit(tag, "|", -1);

    // find how many common levels are shared with the last tag
    int common_start;
    for(common_start = 0; hierarchy && hierarchy[common_start] && tokens && tokens[common_start]; common_start++)
    {
      if(g_strcmp0(hierarchy[common_start], tokens[common_start])) break;
    }

    g_strfreev(hierarchy);
    hierarchy = tokens;

    int tabs = common_start;
    for(size_t i = common_start; tokens && tokens[i]; i++, tabs++)
    {
      for(int j = 0; j < tabs; j++) fputc('\t', fd);
      if(!tokens[i + 1])
      {
        count++;
        if (flags & DT_TF_CATEGORY)
          fprintf(fd, "[%s]\n", tokens[i]);
        else
          fprintf(fd, "%s\n", tokens[i]);
        if (synonyms && synonyms[0])
        {
          gchar **tokens2 = g_strsplit(synonyms, ",", 0);
          if(tokens2)
          {
            gchar **entry = tokens2;
            while(*entry)
            {
              char *e = *entry;
              if (*e == ' ') e++;
              for(int j = 0; j < tabs+1; j++) fputc('\t', fd);
              fprintf(fd, "{%s}\n", e);
              entry++;
            }
          }
          g_strfreev(tokens2);
        }
      }
      else
        fprintf(fd, "%s\n", tokens[i]);
    }
  }

  g_strfreev(hierarchy);

  dt_tag_free_result(&tags);

  fclose(fd);

  return count;
}

char *dt_tag_get_subtags(const int32_t imgid, const char *category, const int level)
{
  if (IS_NULL_PTR(category)) return NULL;
  const guint rootnb = dt_util_string_count_char(category, '|');
  char *tags = NULL;

  GList *names = dt_tag_repository_get_names_under(imgid, category);
  for(GList *l = names; l; l = g_list_next(l))
  {
    const char *tag = (const char *)l->data;
    const guint tagnb = dt_util_string_count_char(tag, '|');
    if (tagnb >= rootnb + level)
    {
      gchar **pch = g_strsplit(tag, "|", -1);
      char *subtag = pch[rootnb + level];
      gboolean valid = TRUE;
      // check we have not yet this subtag in the list
      if(tags && strlen(tags) >= strlen(subtag) + 1)
      {
        gchar *found = g_strstr_len(tags, strlen(tags), subtag);
        if(found && found[strlen(subtag)] == ',')
          valid = FALSE;
      }
      if(valid)
        tags = dt_util_dstrcat(tags, "%s,", subtag);
      g_strfreev(pch);
    }
  }
  g_list_free_full(names, g_free);

  if(tags) tags[strlen(tags) - 1] = '\0'; // remove the last comma
  return tags;
}

gboolean dt_tag_get_tag_order_by_id(const uint32_t tagid, uint32_t *sort,
                                          gboolean *descending)
{
  gboolean res = FALSE;
  if(IS_NULL_PTR(sort)  || !descending) return res;

  const uint32_t flags = dt_tag_repository_get_flags(tagid);
  if((flags & (DT_TF_ORDER_SET)) == (DT_TF_ORDER_SET))
  {
    *sort = (flags & ~DT_TF_DESCENDING) >> 16;
    *descending = flags & DT_TF_DESCENDING;
    res = TRUE;
  }
  return res;
}

uint32_t dt_tag_get_tag_id_by_name(const char * const name)
{
  if(IS_NULL_PTR(name)) return 0;
  return dt_tag_repository_find_by_name_nocase(name);
}

void dt_tag_set_tag_order_by_id(const uint32_t tagid, const uint32_t sort,
                                const gboolean descending)
{
  const uint32_t flags = sort << 16 | (descending ? DT_TF_DESCENDING : 0)
                                    | DT_TF_ORDER_SET;
  dt_tag_repository_update_flags(tagid, flags, DT_TF_ALL);
}

void dt_tags_cleanup(void)
{
  /* The four attached-tag statements this used to finalise belong to
   * database/tag_repository.c now, and are released with the rest of its cache. */
  dt_tag_repository_cleanup();
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
