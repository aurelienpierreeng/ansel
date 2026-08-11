/*
    This file is part of darktable,
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2012, 2014 johannes hanika.
    Copyright (C) 2010-2016, 2019 Tobias Ellinghaus.
    Copyright (C) 2012-2014, 2019-2022 Aldric Renaudin.
    Copyright (C) 2012 Frédéric Grollier.
    Copyright (C) 2012-2015, 2018-2022 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Ulrich Pegelow.
    Copyright (C) 2013 José Carlos García Sogo.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2015 Jan Kundrát.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Alexander Blinne.
    Copyright (C) 2019, 2022 Hanno Schwalm.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Luca Zulberti.
    
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

#include "database/history_repository.h"
#include "common/thumbnail_notify.h"
#include "common/history.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/history_snapshot.h"
#include "caches/image_cache.h"
#include "caches/mipmap_cache.h"
#include "common/tags.h"
#include "common/undo.h"
#include "common/utility.h"
#include "develop/masks.h"
#include "widgets/label.h"

#define DT_IOP_ORDER_INFO (dt_get_debug_flags() & DT_DEBUG_IOPORDER)

void dt_history_item_free(gpointer data)
{
  dt_history_item_t *item = (dt_history_item_t *)data;
  dt_free(item->op);
  dt_free(item->name);
  item->op = NULL;
  item->name = NULL;
  dt_free(item);
}

static void _remove_preset_flag(const int32_t imgid)
{
  dt_image_t *image = dt_image_cache_get(imgid, 'w');
  if(IS_NULL_PTR(image)) return;

  // clear flag
  image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

  // write through to sql+xmp
  dt_image_cache_write_release(image, DT_IMAGE_CACHE_SAFE);
}

void dt_history_delete_on_image_ext(int32_t imgid, gboolean undo)
{
  dt_undo_lt_history_t *hist = undo ? dt_history_snapshot_item_init() : NULL;

  if(undo)
  {
    hist->imgid = imgid;
    dt_history_snapshot_undo_create(hist->imgid, &hist->before, &hist->before_history_end);
  }

  dt_history_repository_delete_all_for_image(imgid);

  _remove_preset_flag(imgid);

  /* make sure mipmaps are recomputed */
  dt_mipmap_cache_remove(imgid, TRUE);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style|%", imgid, FALSE, FALSE);
  dt_tag_detach_by_string("darktable|changed", imgid, FALSE, FALSE);

  // signal that the mipmap need to be updated
  dt_thumbnail_notify_image_changed(imgid, TRUE);

  if(undo)
  {
    dt_history_snapshot_undo_create(hist->imgid, &hist->after, &hist->after_history_end);

    dt_undo_start_group(dt_undo_get_global(), DT_UNDO_LT_HISTORY);
    dt_undo_record(dt_undo_get_global(), NULL, DT_UNDO_LT_HISTORY, (dt_undo_data_t)hist,
                   dt_history_snapshot_undo_pop, dt_history_snapshot_undo_lt_history_data_free);
    dt_undo_end_group(dt_undo_get_global());
  }
}

void dt_history_delete_on_image(int32_t imgid)
{
  dt_history_delete_on_image_ext(imgid, TRUE);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_TAG_CHANGED);
}

char *dt_history_item_as_string(const char *name, gboolean enabled)
{
  return g_strconcat(enabled ? "\342\227\217" : "\342\227\213", "  ", name, NULL);
}

static void _collect_item(void *user_data, const int num, const char *operation,
                          const gboolean enabled, const char *multi_name)
{
  GList **result = (GList **)user_data;

  if(strcmp(operation, "mask_manager") == 0) return;

  char name[512] = { 0 };
  dt_history_item_t *item = g_malloc(sizeof(dt_history_item_t));
  item->num = num;
  item->enabled = enabled;

  if(strcmp(multi_name, "0") == 0)
    g_snprintf(name, sizeof(name), "%s", dt_iop_get_localized_name(operation));
  else
    g_snprintf(name, sizeof(name), "%s %s", dt_iop_get_localized_name(operation), multi_name);

  item->name = g_strdup(name);
  item->op = g_strdup(operation);
  *result = g_list_prepend(*result, item);
}

GList *dt_history_get_items(const int32_t imgid, gboolean enabled)
{
  GList *result = NULL;
  dt_history_repository_foreach_last_item(imgid, enabled, _collect_item, &result);
  return g_list_reverse(result);   // list was built in reverse order, so un-reverse it
}

static void _collect_item_string(void *user_data, const int num, const char *operation,
                                 const gboolean enabled, const char *multi_name)
{
  GList **items = (GList **)user_data;
  char *decorated = NULL;

  if(multi_name && *multi_name && g_strcmp0(multi_name, " ") != 0 && g_strcmp0(multi_name, "0") != 0)
    decorated = g_strconcat(" ", multi_name, NULL);

  char *iname = dt_history_item_as_string(dt_iop_get_localized_name(operation), enabled);
  char *name = g_strconcat(iname, decorated ? decorated : "", NULL);
  *items = g_list_prepend(*items, delete_underscore(name));

  dt_free(iname);
  dt_free(name);
  dt_free(decorated);
}

char *dt_history_get_items_as_string(const int32_t imgid)
{
  GList *items = NULL;
  dt_history_repository_foreach_item(imgid, _collect_item_string, &items);
  items = g_list_reverse(items); // list was built in reverse order, so un-reverse it
  char *result = dt_util_glist_to_str("\n", items);
  g_list_free_full(items, dt_free_gpointer);
  return result;
}


#undef DT_IOP_ORDER_INFO
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
