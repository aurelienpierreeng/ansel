/*
    This file is part of darktable,
    Copyright (C) 2010 calca.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010-2012 johannes hanika.
    Copyright (C) 2010-2017 Tobias Ellinghaus.
    Copyright (C) 2012-2013, 2019-2022 Aldric Renaudin.
    Copyright (C) 2012 Frédéric Grollier.
    Copyright (C) 2012-2013 Jérémy Rosen.
    Copyright (C) 2012-2015, 2017, 2019-2022 Pascal Obry.
    Copyright (C) 2012 Richard Levitte.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012-2013, 2015 Ulrich Pegelow.
    Copyright (C) 2013 Pascal de Bruijn.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2014, 2016 Roman Lebedev.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019, 2021 Hanno Schwalm.
    Copyright (C) 2019-2020 Heiko Bauke.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020-2021 darkelectron.
    Copyright (C) 2020 EdgarLux.
    Copyright (C) 2020-2022 Hubert Kowalski.
    Copyright (C) 2020 JP Verrue.
    Copyright (C) 2021-2022 Diederik Ter Rahe.
    Copyright (C) 2021 luzpaz.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2026 Guillaume Stutin.
    
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

#include "common/utility.h"
#include "common/history_actions.h"
#include "common/conf.h"
#include <glib/gstdio.h>
#include "database/style_repository.h"
#include "common/styles.h"
#include "history/notify.h"
#include "system/macros.h"
#include "system/mem_alloc.h"
#include "common/logging.h"
#include "common/paths.h"
#include "common/image.h"
#include "metadata/exif.h"
#include "common/file_location.h"
#include "history/history.h"
#include "history/history_snapshot.h"
#include "develop/iop_order.h"
#include "metadata/tags.h"

#include "develop/develop.h"
#include "develop/dev_history.h"
#include "develop/masks/masks_history.h"

#include <libxml/encoding.h>
#include <libxml/xmlwriter.h>

#include <glib.h>
#include <stdio.h>
#include <string.h>

#define DT_IOP_ORDER_INFO (dt_get_debug_flags() & DT_DEBUG_IOPORDER)

typedef struct
{
  GString *name;
  GString *description;
  GList *iop_list;
} StyleInfoData;

typedef struct
{
  int num;
  int module;
  GString *operation;
  GString *op_params;
  GString *blendop_params;
  int blendop_version;
  int multi_priority;
  GString *multi_name;
  int enabled;
  double iop_order;
  int apply_shapes;
} StylePluginData;

/* one <shape>: a row of data.style_shapes */
typedef struct
{
  int id;
  int type;
  int version;
  int points_count;
  gchar *name;
  unsigned char *points;
  int points_len;
  unsigned char *source;
  int source_len;
  gboolean valid;
} StyleShapeData;

typedef struct
{
  StyleInfoData *info;
  GList *plugins;
  GList *shapes;
  gboolean in_plugin;
} StyleData;

void dt_style_free(gpointer data)
{
  dt_style_t *style = (dt_style_t *)data;
  dt_free(style->name);
  dt_free(style->description);
  style->name = NULL;
  style->description = NULL;
  dt_free(style);
}

void dt_style_item_free(gpointer data)
{
  dt_style_item_t *item = (dt_style_item_t *)data;
  dt_free(item->name);
  dt_free(item->operation);
  dt_free(item->multi_name);
  dt_free(item->params);
  dt_free(item->blendop_params);
  item->name = NULL;
  item->operation = NULL;
  item->multi_name = NULL;
  item->params = NULL;
  item->blendop_params = NULL;
  dt_free(item);
}

int32_t dt_styles_get_id_by_name(const char *name);

gboolean dt_styles_exists(const char *name)
{
  if(name)
    return (dt_styles_get_id_by_name(name)) != 0 ? TRUE : FALSE;
  return FALSE;
}

gboolean dt_styles_has_module_order(const char *name)
{
  char *iop_list_txt = dt_style_repository_get_iop_list(name);
  const gboolean has_iop_list = (iop_list_txt != NULL);
  dt_free(iop_list_txt);
  return has_iop_list;
}

GList *dt_styles_module_order_list(const char *name)
{
  GList *iop_list = NULL;
  char *iop_list_txt = dt_style_repository_get_iop_list(name);
  if(iop_list_txt)
  {
    iop_list = dt_ioppr_deserialize_text_iop_order_list(iop_list_txt);
    // style saved before newer modules existed: place them so applying it
    // does not mis-order those modules
    if(iop_list) iop_list = dt_ioppr_insert_missing_modules(iop_list);
    dt_free(iop_list_txt);
  }
  return iop_list;
}

static gboolean dt_styles_create_style_header(const char *name, const char *description, GList *iop_list)
{
  if(dt_styles_get_id_by_name(name) != 0)
  {
    dt_history_message(_("style with name '%s' already exists"), name);
    return FALSE;
  }

  char *iop_list_txt = iop_list ? dt_ioppr_serialize_text_iop_order_list(iop_list) : NULL;
  dt_style_repository_insert_header(name, description, iop_list_txt);
  dt_free(iop_list_txt);
  return TRUE;
}

/* `from_image` gets, for every item taken from the image, its num in the style and its num in the
 * image's history. */
static void _dt_style_update_from_image(int id, int32_t imgid, GList *filter, GList *update,
                                        GHashTable *from_image)
{
  if(update && imgid != UNKNOWN_IMAGE)
  {
    GList *list = filter;
    GList *upd = update;

    do
    {
      const int history_num = GPOINTER_TO_INT(upd->data);
      const int item_num = GPOINTER_TO_INT(list->data);

      // included and update set, we then need to update the corresponding style item
      if(history_num != -1 && item_num != -1)
      {
        dt_style_repository_update_item_from_history(id, item_num, imgid, history_num);
        g_hash_table_insert(from_image, GINT_TO_POINTER(item_num), GINT_TO_POINTER(history_num));
      }
      // update only, so we want to insert the new style item
      else if(history_num != -1)
      {
        dt_style_repository_append_item_from_history(id, imgid, history_num);
        g_hash_table_insert(from_image, GINT_TO_POINTER(dt_style_repository_get_last_num(id)),
                            GINT_TO_POINTER(history_num));
      }

      list = g_list_next(list);
      upd = g_list_next(upd);
    } while(list);
  }
}

static GList *_dt_styles_get_apply_items(const int style_id);

/* The group an item's blend params name, 0 for none, -1 when its params are not of the current blend
 * version and cannot be read here. */
static int _styles_item_mask_id(const dt_style_item_t *item)
{
  if(IS_NULL_PTR(item->blendop_params)) return 0;
  if(item->blendop_version != dt_develop_blend_version()
     || item->blendop_params_size != sizeof(dt_develop_blend_params_t))
    return -1;
  return item->blendop_params->mask_id;
}

/* Whether a module draws its own shapes (retouch): applying a style always brings those, whatever the
 * style says of its drawn masks. */
static gboolean _styles_draws_own_shapes(const char *operation)
{
  return (dt_iop_get_module_flags(operation) & IOP_FLAGS_INTERNAL_MASKS) != 0;
}

/* Whether `item`'s module instance comes without the shapes of its drawn mask: the choice is the
 * instance's, so one item of it among `items` set so (num in `without`) decides for all of them, an
 * item an edit later adds to the instance included. */
static gboolean _styles_instance_without_shapes(GList *items, GList *without, const dt_style_item_t *item)
{
  const char *multi_name = item->multi_name ? item->multi_name : "";
  for(GList *l = items; l; l = g_list_next(l))
  {
    const dt_style_item_t *other = (const dt_style_item_t *)l->data;
    if(!g_strcmp0(other->operation, item->operation)
       && !g_strcmp0(other->multi_name ? other->multi_name : "", multi_name)
       && g_list_find(without, GINT_TO_POINTER(other->num)))
      return TRUE;
  }
  return FALSE;
}

/* Store in the style the shapes its items use, as copy and paste would carry them. An item named in
 * `from_image` (num in the style -> num in the history of `imgid`) takes them from the image's
 * history at that item; any other one from `old_shapes`, those the style stored before. */
static void _styles_write_shapes(const int id, const int32_t imgid, GHashTable *from_image,
                                 GList *old_shapes)
{
  GList *items = _dt_styles_get_apply_items(id);
  GList *shapes = NULL;
  dt_develop_t dev_img;
  gboolean dev_img_loaded = FALSE;

  // Later items first: two items naming one form keep the later one's state, the one the image ends on.
  for(GList *l = g_list_last(items); l; l = g_list_previous(l))
  {
    const dt_style_item_t *item = (const dt_style_item_t *)l->data;
    const int mask_id = _styles_item_mask_id(item);
    if(mask_id == 0) continue;

    gpointer history_num = NULL;
    if(!g_hash_table_lookup_extended(from_image, GINT_TO_POINTER(item->num), NULL, &history_num))
    {
      // the style stores shapes since the current blend version: an older item has none
      if(mask_id > 0 && dt_masks_forms_hold(old_shapes, mask_id))
        dt_masks_collect_shapes(&shapes, old_shapes, mask_id);
      continue;
    }

    // From the image, the group is read from its history, where older blend params are converted.
    if(!dev_img_loaded)
    {
      dt_dev_init(&dev_img, FALSE);
      dt_dev_reload_history_items(&dev_img, imgid);
      dev_img_loaded = TRUE;
    }

    for(GList *h = dev_img.history; h; h = g_list_next(h))
    {
      const dt_dev_history_item_t *hist = (const dt_dev_history_item_t *)h->data;
      if(hist->num != GPOINTER_TO_INT(history_num)) continue;
      if(hist->blend_params && hist->blend_params->mask_id > 0)
        dt_masks_collect_shapes(&shapes, dt_masks_history_forms_at(dev_img.history, hist),
                                hist->blend_params->mask_id);
      break;
    }
  }

  dt_masks_write_style_shapes(id, shapes);

  dt_masks_forms_snapshot_release(&shapes);
  if(dev_img_loaded) dt_dev_cleanup(&dev_img);
  g_list_free_full(items, dt_style_item_free);
}

void dt_style_shapes_module_free(gpointer data)
{
  dt_style_shapes_module_t *module = (dt_style_shapes_module_t *)data;
  dt_free(module->operation);
  dt_free(module->multi_name);
  dt_free(module->label);
  dt_free(module);
}

GList *dt_styles_get_shapes_modules(const char *name)
{
  const int id = dt_styles_get_id_by_name(name);
  if(id == 0) return NULL;

  GList *items = _dt_styles_get_apply_items(id);
  GList *without = dt_style_repository_get_items_without_shapes(id);
  GList *modules = NULL;
  for(GList *l = items; l; l = g_list_next(l))
  {
    const dt_style_item_t *item = (const dt_style_item_t *)l->data;
    const int mask_id = _styles_item_mask_id(item);
    if(mask_id <= 0 || _styles_draws_own_shapes(item->operation)
       || !dt_style_repository_holds_shape(id, mask_id))
      continue;

    // one entry per module instance, whatever number of items it has
    const char *multi_name = item->multi_name ? item->multi_name : "";
    dt_style_shapes_module_t *module = NULL;
    for(GList *m = modules; m && IS_NULL_PTR(module); m = g_list_next(m))
    {
      dt_style_shapes_module_t *candidate = (dt_style_shapes_module_t *)m->data;
      if(!g_strcmp0(candidate->operation, item->operation) && !g_strcmp0(candidate->multi_name, multi_name))
        module = candidate;
    }
    if(IS_NULL_PTR(module))
    {
      module = g_new0(dt_style_shapes_module_t, 1);
      module->operation = g_strdup(item->operation);
      module->multi_name = g_strdup(multi_name);
      const gchar *localized = dt_iop_get_localized_name(item->operation);
      module->label = (*multi_name && strcmp(multi_name, "0") != 0)
                          ? g_strdup_printf("%s %s", localized, multi_name)
                          : g_strdup(localized);
      module->apply = !_styles_instance_without_shapes(items, without, item);
      modules = g_list_prepend(modules, module);
    }
  }
  g_list_free(without);
  g_list_free_full(items, dt_style_item_free);
  return g_list_reverse(modules);
}

void dt_styles_set_module_shapes(const char *name, const char *operation, const char *multi_name,
                                 const gboolean apply)
{
  const int id = dt_styles_get_id_by_name(name);
  if(id == 0) return;

  // backup style to disk, as an edit does
  if(dt_style_repository_set_apply_shapes(id, operation, multi_name, apply))
    dt_styles_save_to_file(name, NULL, TRUE);
}

static void  _dt_style_update_iop_order(const gchar *name, const int id, const int32_t imgid,
                                        const gboolean copy_iop_order, const gboolean update_iop_order)
{
  GList *iop_list = dt_styles_module_order_list(name);

  // if we update or if the style does not contains an order then the
  // copy must be done using the imgid iop-order.

  if(update_iop_order || IS_NULL_PTR(iop_list))
    iop_list = dt_ioppr_get_iop_order_list(imgid, FALSE);

  gchar *iop_list_txt = dt_ioppr_serialize_text_iop_order_list(iop_list);

  // copy from style name to style id, or clear the order outright
  dt_style_repository_set_iop_list(id, (copy_iop_order || update_iop_order) ? iop_list_txt : NULL);

  g_list_free_full(iop_list, dt_free_gpointer);
  iop_list = NULL;
  dt_free(iop_list_txt);
}

void dt_styles_update(const char *name, const char *newname, const char *newdescription, GList *filter,
                      const int32_t imgid, GList *update,
                      const gboolean copy_iop_order, const gboolean update_iop_order)
{
  const int id = dt_styles_get_id_by_name(name);
  if(id == 0) return;

  gchar *desc = dt_styles_get_description(name);

  if((g_strcmp0(name, newname)) || (g_strcmp0(desc, newdescription)))
    dt_style_repository_update_header(id, newname, newdescription);

  if(filter)
    dt_style_repository_delete_items_except(id, filter);

  GHashTable *from_image = g_hash_table_new(g_direct_hash, g_direct_equal);
  _dt_style_update_from_image(id, imgid, filter, update, from_image);

  GList *old_shapes = dt_masks_read_style_shapes(id);
  _styles_write_shapes(id, imgid, from_image, old_shapes);
  dt_masks_forms_snapshot_release(&old_shapes);
  g_hash_table_destroy(from_image);

  _dt_style_update_iop_order(name, id, imgid, copy_iop_order, update_iop_order);

  if(!copy_iop_order && !update_iop_order)
  {
    // Without a stored module order, style items are self-contained and can use compact priorities.
    dt_style_repository_normalize_multi_priority(id);
  }

  /* backup style to disk */
  dt_styles_save_to_file(newname, NULL, TRUE);

  dt_history_changed(DT_HISTORY_CHANGE_STYLES);

  dt_free(desc);
}

void dt_styles_create_from_style(const char *name, const char *newname, const char *description,
                                 GList *filter, const int32_t imgid, GList *update,
                                 const gboolean copy_iop_order, const gboolean update_iop_order)
{
  int id = 0;

  const int oldid = dt_styles_get_id_by_name(name);
  if(oldid == 0) return;

  /* create the style header */
  if(!dt_styles_create_style_header(newname, description, NULL)) return;

  if((id = dt_styles_get_id_by_name(newname)) != 0)
  {
    dt_style_repository_copy_items_from_style(id, oldid, filter);

    /* insert items from imgid if defined */

    GHashTable *from_image = g_hash_table_new(g_direct_hash, g_direct_equal);
    _dt_style_update_from_image(id, imgid, filter, update, from_image);

    GList *old_shapes = dt_masks_read_style_shapes(oldid);
    _styles_write_shapes(id, imgid, from_image, old_shapes);
    dt_masks_forms_snapshot_release(&old_shapes);
    g_hash_table_destroy(from_image);

    _dt_style_update_iop_order(name, id, imgid, copy_iop_order, update_iop_order);

    if(!copy_iop_order && !update_iop_order)
    {
      // Without a stored module order, style items are self-contained and can use compact priorities.
      dt_style_repository_normalize_multi_priority(id);
    }

    /* backup style to disk */
    dt_styles_save_to_file(newname, NULL, FALSE);

    dt_history_message(_("style named '%s' successfully created"), newname);
    dt_history_changed(DT_HISTORY_CHANGE_STYLES);
  }
}

gboolean dt_styles_create_from_image(const char *name, const char *description,
                                     const int32_t imgid, GList *filter, gboolean copy_iop_order)
{
  int id = 0;

  GList *iop_list = NULL;
  if(copy_iop_order)
  {
    iop_list = dt_ioppr_get_iop_order_list(imgid, FALSE);
  }

  /* first create the style header */
  if(!dt_styles_create_style_header(name, description, iop_list)) return FALSE;

  g_list_free_full(iop_list, dt_free_gpointer);
  iop_list = NULL;

  if((id = dt_styles_get_id_by_name(name)) != 0)
  {
    /* create the style_items from source image history stack */
    dt_style_repository_copy_items_from_history(id, imgid, filter);

    // every item keeps the num it has in the image's history
    GHashTable *from_image = g_hash_table_new(g_direct_hash, g_direct_equal);
    GList *items = _dt_styles_get_apply_items(id);
    for(GList *l = items; l; l = g_list_next(l))
    {
      const int num = ((dt_style_item_t *)l->data)->num;
      g_hash_table_insert(from_image, GINT_TO_POINTER(num), GINT_TO_POINTER(num));
    }
    g_list_free_full(items, dt_style_item_free);
    _styles_write_shapes(id, imgid, from_image, NULL);
    g_hash_table_destroy(from_image);

    if(!copy_iop_order)
    {
      // A style without module order cannot use sparse image priorities, so compact them locally.
      dt_style_repository_normalize_multi_priority(id);
    }

    /* backup style to disk */
    dt_styles_save_to_file(name, NULL, FALSE);

    dt_history_changed(DT_HISTORY_CHANGE_STYLES);
    return TRUE;
  }
  return FALSE;
}

void dt_multiple_styles_apply_to_list(GList *styles, const GList *list, gboolean duplicate)
{
  /* write current history changes so nothing gets lost,
     do that only in the darkroom as there is nothing to be saved
     when in the lighttable (and it would write over current history stack) */

  if(IS_NULL_PTR(styles) && !list)
  {
    dt_history_message(_("no images nor styles selected!"));
    return;
  }
  else if(!styles)
  {
    dt_history_message(_("no styles selected!"));
    return;
  }
  else if(!list)
  {
    dt_history_message(_("no image selected!"));
    return;
  }

  /* for each selected image apply style */
  dt_undo_start_group(dt_undo_get_global(), DT_UNDO_LT_HISTORY);
  for(const GList *l = list; l; l = g_list_next(l))
  {
    const int32_t imgid = GPOINTER_TO_INT(l->data);
    for(GList *style = styles; style; style = g_list_next(style))
    {
      dt_history_style_on_image(imgid, (char *)style->data, duplicate);
    }
  }
  dt_undo_end_group(dt_undo_get_global());

  const guint styles_cnt = g_list_length(styles);
  dt_history_message(ngettext("style successfully applied!", "styles successfully applied!", styles_cnt));
}

static const char *_dt_styles_normalize_multi_name(const char *multi_name)
{
  if(IS_NULL_PTR(multi_name) || !*multi_name || !strcmp(multi_name, "0")) return "";
  return multi_name;
}

static gboolean _dt_styles_apply_item_to_module(dt_iop_module_t *module, const dt_style_item_t *style_item)
{
  module->enabled = style_item->enabled;

  const char *multi_name = _dt_styles_normalize_multi_name(style_item->multi_name);
  if(*multi_name)
    g_strlcpy(module->multi_name, multi_name, sizeof(module->multi_name));
  else
    module->multi_name[0] = '\0';

  if(style_item->blendop_params && (style_item->blendop_version == dt_develop_blend_version())
     && (style_item->blendop_params_size == sizeof(dt_develop_blend_params_t)))
  {
    memcpy(module->blend_params, style_item->blendop_params, sizeof(dt_develop_blend_params_t));
  }
  else if(style_item->blendop_params
          && dt_develop_blend_legacy_params(module, style_item->blendop_params, style_item->blendop_version,
                                            module->blend_params, dt_develop_blend_version(),
                                            style_item->blendop_params_size)
                 == 0)
  {
    // do nothing
  }
  else if(module->default_blendop_params)
  {
    memcpy(module->blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
  }

  gboolean ok = TRUE;
  if(module->version() != style_item->module_version || module->params_size != style_item->params_size
     || strcmp(style_item->operation, module->op))
  {
    if(!module->legacy_params
       || module->legacy_params(module, style_item->params, labs(style_item->module_version), module->params,
                                labs(module->version())))
    {
      fprintf(stderr, "[dt_styles_apply] module `%s' version mismatch: history is %d, dt %d.\n", module->op,
              style_item->module_version, module->version());
      dt_history_message(_("module `%s' version mismatch: %d != %d"), module->op, module->version(),
                     style_item->module_version);
      ok = FALSE;
    }
  }
  else
  {
    memcpy(module->params, style_item->params, module->params_size);
  }

  if(ok && !strcmp(module->op, "flip") && module->enabled == 0 && labs(style_item->module_version) == 1)
  {
    memcpy(module->params, module->default_params, module->params_size);
    module->enabled = 1;
  }

  return ok;
}

static dt_iop_module_t *_dt_styles_get_or_create_module_instance(dt_develop_t *dev, const dt_style_item_t *style_item)
{
  const char *multi_name = _dt_styles_normalize_multi_name(style_item->multi_name);
  dt_iop_module_t *module = dt_dev_get_module_instance(dev, style_item->operation, multi_name,
                                                       style_item->multi_priority);
  if(module) return module;

  module = dt_dev_create_module_instance(dev, style_item->operation, multi_name, style_item->multi_priority, FALSE);
  if(module) module->iop_order = style_item->iop_order;
  return module;
}

static dt_iop_module_t *_dt_styles_tmp_module_from_style_item(dt_develop_t *dev, const dt_style_item_t *style_item)
{
  dt_iop_module_t *mod_src = dt_iop_get_module_by_op_priority(dev->iop, style_item->operation, -1);
  if(IS_NULL_PTR(mod_src)) return NULL;

  dt_iop_module_t *module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
  if(dt_iop_load_module(module, mod_src->so, dev))
  {
    fprintf(stderr, "[dt_styles_apply] can't load module %s %s\n", style_item->operation,
            style_item->multi_name ? style_item->multi_name : "(null)");
    dt_free(module);
    return NULL;
  }

  module->instance = mod_src->instance;
  module->multi_priority = style_item->multi_priority;
  module->iop_order = style_item->iop_order;

  if(!_dt_styles_apply_item_to_module(module, style_item))
  {
    dt_iop_cleanup_module(module);
    dt_free(module);
    return NULL;
  }

  return module;
}

static void _prepend_apply_item(void *user_data, const int num, const int multi_priority,
                                const int module_version, const char *operation, const int enabled,
                                const void *params, const int32_t params_size,
                                const void *blendop_params, const int32_t blendop_params_size,
                                const int blendop_version, const char *multi_name,
                                const int selimg_num, const double iop_order)
{
  GList **si_list = (GList **)user_data;
  dt_style_item_t *style_item = (dt_style_item_t *)malloc(sizeof(dt_style_item_t));

  style_item->num = num;
  style_item->selimg_num = selimg_num;
  style_item->enabled = enabled;
  style_item->multi_priority = multi_priority;
  style_item->name = NULL;
  style_item->operation = g_strdup(operation);
  style_item->multi_name = g_strdup(multi_name);
  style_item->module_version = module_version;
  style_item->blendop_version = blendop_version;
  style_item->params_size = params_size;
  style_item->params = (void *)malloc(params_size);
  memcpy(style_item->params, params, params_size);
  style_item->blendop_params_size = blendop_params_size;
  style_item->blendop_params = (void *)malloc(blendop_params_size);
  memcpy(style_item->blendop_params, blendop_params, blendop_params_size);
  style_item->iop_order = iop_order;

  *si_list = g_list_prepend(*si_list, style_item);
}

static GList *_dt_styles_get_apply_items(const int style_id)
{
  GList *si_list = NULL;
  dt_style_repository_foreach_apply_item(style_id, _prepend_apply_item, &si_list);
  return g_list_reverse(si_list);  // list was built in reverse order, so un-reverse it
}

static GList *_dt_styles_build_mod_list_from_history(dt_develop_t *dev, GHashTable *style_ids)
{
  GList *mod_list = NULL;
  for(GList *h = g_list_first(dev->history); h; h = g_list_next(h))
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)h->data;
    if(!hist || !hist->module) continue;
    char *id_str = g_strdup_printf("%s|%s", hist->op_name, hist->multi_name);
    const gboolean keep = g_hash_table_contains(style_ids, id_str);
    dt_free(id_str);
    if(!keep) continue;

    if(!g_list_find(mod_list, hist->module))
      mod_list = g_list_append(mod_list, hist->module);
  }
  return mod_list;
}

static void _dt_styles_tmp_module_free(dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return;
  dt_iop_cleanup_module(module);
  dt_free(module);
}

static int _styles_init_source_dev(dt_develop_t *dev_src, const char *name, const int32_t imgid,
                                   gboolean *has_iop_list)
{
  dt_dev_init(dev_src, FALSE);
  if(dt_dev_ensure_image_storage(dev_src, imgid)) return 1;

  *has_iop_list = FALSE;
  dt_ioppr_set_default_iop_order(dev_src, imgid);
  dt_dev_init_default_history(dev_src, imgid, FALSE);

  // If the style has a stored iop-order list, apply it to the temporary pipeline
  GList *iop_list = dt_styles_module_order_list(name);
  if(iop_list)
  {
    g_list_free_full(dev_src->iop_order_list, dt_free_gpointer);
    dev_src->iop_order_list = iop_list;
    *has_iop_list = TRUE;
  }

  return 0;
}

static GList *_styles_collect_applied_items(dt_develop_t *dev_src, GList *si_list, GHashTable *style_ids)
{
  GList *applied_items = NULL;

  for(GList *l = si_list; l; l = g_list_next(l))
  {
    dt_style_item_t *style_item = (dt_style_item_t *)l->data;
    dt_iop_module_t *module = _dt_styles_get_or_create_module_instance(dev_src, style_item);
    if(IS_NULL_PTR(module)) continue;

    const char *multi_name = _dt_styles_normalize_multi_name(style_item->multi_name);
    module->multi_priority = style_item->multi_priority;
    g_strlcpy(module->multi_name, multi_name, sizeof(module->multi_name));

    if(!_dt_styles_apply_item_to_module(module, style_item)) continue;

    applied_items = g_list_append(applied_items, style_item);
    g_hash_table_add(style_ids, g_strdup_printf("%s|%s", style_item->operation, multi_name));
  }

  return applied_items;
}

static void _styles_sync_pipeline_from_items(dt_develop_t *dev_src, GList *applied_items,
                                             const gboolean has_iop_list)
{
  if(!has_iop_list) dt_ioppr_update_for_style_items(dev_src, applied_items, FALSE);

  for(GList *l = applied_items; l; l = g_list_next(l))
  {
    dt_style_item_t *style_item = (dt_style_item_t *)l->data;
    const char *multi_name = _dt_styles_normalize_multi_name(style_item->multi_name);
    dt_iop_module_t *module
        = dt_dev_get_module_instance(dev_src, style_item->operation, multi_name, style_item->multi_priority);
    if(!IS_NULL_PTR(module))
    {
      if(has_iop_list)
        style_item->iop_order = dt_ioppr_get_iop_order(dev_src->iop_order_list, style_item->operation,
                                                       style_item->multi_priority);
      module->multi_priority = style_item->multi_priority;
      module->iop_order = style_item->iop_order;
    }
  }

  dt_ioppr_resync_pipeline(dev_src, 0, NULL, FALSE);
}

static int _styles_rebuild_history_from_items(dt_develop_t *dev_src, GList *applied_items)
{
  dt_dev_history_free_history(dev_src);

  // dev_src->forms holds the style's shapes: an item naming a group carries them, as an item
  // committed with a drawn mask does, so they survive the pop below and reach the image.

  for(GList *l = applied_items; l; l = g_list_next(l))
  {
    dt_style_item_t *style_item = (dt_style_item_t *)l->data;
    const char *multi_name = _dt_styles_normalize_multi_name(style_item->multi_name);
    dt_iop_module_t *module
        = dt_dev_get_module_instance(dev_src, style_item->operation, multi_name, style_item->multi_priority);
    if(IS_NULL_PTR(module)) continue;

    dt_dev_history_item_t *hist = dt_dev_history_item_create();
    if(IS_NULL_PTR(hist)) return 1;

    dev_src->history = g_list_append(dev_src->history, hist);
    GList *forms = (module->blend_params && module->blend_params->mask_id > 0)
                       ? dt_masks_snapshot_current_forms(dev_src, FALSE)
                       : NULL;
    if(!dt_dev_history_item_update_from_params(dev_src, hist, module, module->enabled, NULL, 0, NULL, forms))
    {
      dt_dev_history_free_history(dev_src);
      return 1;
    }
  }

  dt_dev_set_history_end_ext(dev_src, g_list_length(dev_src->history));

  dt_dev_history_compress_ext(dev_src, FALSE);
  dt_dev_pop_history_items_ext(dev_src);

  return 0;
}

/* The shapes applying the style brings: all it stores but the groups of the module instances set to
 * come without the shapes of their drawn mask (apply_shapes off); those of a module that draws its own
 * always come.
 * Such a drawn mask arrives with its blend params unchanged, naming a group the image lacks: the pipe
 * fills a drawn mask whose group is missing, so the module applies as if it had no drawn mask until
 * shapes are drawn on the image. */
static GList *_styles_applied_shapes(const int style_id, GList *items)
{
  GList *shapes = dt_masks_read_style_shapes(style_id);
  GList *without = dt_style_repository_get_items_without_shapes(style_id);
  if(IS_NULL_PTR(without)) return shapes;

  GList *applied = NULL;
  for(GList *l = items; l; l = g_list_next(l))
  {
    const dt_style_item_t *item = (const dt_style_item_t *)l->data;
    const int mask_id = _styles_item_mask_id(item);
    if(mask_id <= 0 || !dt_masks_forms_hold(shapes, mask_id)) continue;
    if(_styles_draws_own_shapes(item->operation) || !_styles_instance_without_shapes(items, without, item))
      dt_masks_collect_shapes(&applied, shapes, mask_id);
  }
  g_list_free(without);
  dt_masks_forms_snapshot_release(&shapes);
  return applied;
}

static int _styles_prepare_source_dev(dt_develop_t *dev_src, const char *name, const int style_id,
                                      const int32_t imgid, GList **out_si_list, GHashTable **out_style_ids,
                                      GList **out_mod_list)
{
  gboolean has_iop_list = FALSE;
  if(_styles_init_source_dev(dev_src, name, imgid, &has_iop_list)) return 1;

  GList *si_list = _dt_styles_get_apply_items(style_id);

  GList *shapes = _styles_applied_shapes(style_id, si_list);
  dt_masks_replace_current_forms(dev_src, shapes);
  dt_masks_forms_snapshot_release(&shapes);
  if(!has_iop_list)
  {
    // Without a stored iop_list, add order entries before creating the style modules;
    // otherwise update_for_style_items() sees the new modules as already present.
    dt_ioppr_update_for_style_items(dev_src, si_list, FALSE);
  }

  GHashTable *style_ids = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  GList *applied_items = _styles_collect_applied_items(dev_src, si_list, style_ids);

  if(IS_NULL_PTR(applied_items))
  {
    g_hash_table_destroy(style_ids);
    g_list_free_full(si_list, dt_style_item_free);
    si_list = NULL;
    return 1;
  }

  _styles_sync_pipeline_from_items(dev_src, applied_items, has_iop_list);
  if(_styles_rebuild_history_from_items(dev_src, applied_items))
  {
    g_list_free(applied_items);
    applied_items = NULL;
    g_hash_table_destroy(style_ids);
    g_list_free_full(si_list, dt_style_item_free);
    si_list = NULL;
    return 1;
  }

  // Build module list from the compressed history to guarantee a matching history entry
  GList *mod_list = _dt_styles_build_mod_list_from_history(dev_src, style_ids);
  g_list_free(applied_items);
  applied_items = NULL;

  if(!mod_list)
  {
    g_hash_table_destroy(style_ids);
    g_list_free_full(si_list, dt_style_item_free);
    si_list = NULL;
    return 1;
  }

  *out_si_list = si_list;
  *out_style_ids = style_ids;
  *out_mod_list = mod_list;
  return 0;
}

int dt_styles_apply_to_image_merge(const char *name, const int style_id, const int32_t newimgid,
                                   const dt_history_merge_strategy_t mode, dt_hm_batch_state_t *batch)
{
  int ret_val = 1;

  // Init source history + pipeline (style content)
  dt_develop_t dev_src = { 0 };

  GList *si_list = NULL;
  GHashTable *style_ids = NULL;
  GList *mod_list = NULL;

  if(_styles_prepare_source_dev(&dev_src, name, style_id, newimgid, &si_list, &style_ids, &mod_list))
  {
    dt_dev_cleanup(&dev_src);
    return 1;
  }

  if (DT_IOP_ORDER_INFO) fprintf(stderr,"\n^^^^^ Apply style on image %i, history size %i\n",newimgid, dt_dev_get_history_end_ext(&dev_src));

  if(mode == DT_HISTORY_MERGE_REPLACE)
  {
    ret_val = dt_dev_replace_history_on_image(&dev_src, newimgid, TRUE, "_styles_apply_to_image_merge");
  }
  else
  {
    ret_val = dt_dev_merge_history_into_image(&dev_src, newimgid, mod_list,
                                              dt_conf_get_bool("history/style/copy_iop_order"), mode,
                                              dt_conf_get_bool("history/paste_instances"), name, batch);
  }

  g_list_free(mod_list);
  mod_list = NULL;
  g_hash_table_destroy(style_ids);
  g_list_free_full(si_list, dt_style_item_free);
  si_list = NULL;
  dt_dev_cleanup(&dev_src);

  return ret_val;
}

static void _styles_apply_style_item(dt_develop_t *dev, dt_develop_t *dev_shapes, dt_style_item_t *style_item)
{
  dt_pthread_rwlock_wrlock(&dev->history_mutex);

  dt_iop_module_t *module = _dt_styles_tmp_module_from_style_item(dev, style_item);
  if(module)
  {
    dt_history_merge_module_into_history(dev, dev_shapes, module);
    dt_ioppr_resync_pipeline(dev, 0, NULL, FALSE);
    dt_dev_pop_history_items_ext(dev);
    _dt_styles_tmp_module_free(module);
  }

  dt_pthread_rwlock_unlock(&dev->history_mutex);
}

void dt_styles_apply_style_items(dt_develop_t *dev, const char *name, GList *style_items)
{
  // A merged module takes its shapes from a dev's forms: the style's travel in a dev of their own.
  dt_develop_t dev_shapes;
  dt_dev_init(&dev_shapes, FALSE);
  GList *shapes = _styles_applied_shapes(dt_styles_get_id_by_name(name), style_items);
  dt_masks_replace_current_forms(&dev_shapes, shapes);
  dt_masks_forms_snapshot_release(&shapes);

  for(GList *l = style_items; l; l = g_list_next(l))
    _styles_apply_style_item(dev, &dev_shapes, (dt_style_item_t *)l->data);

  dt_dev_cleanup(&dev_shapes);
}

void dt_styles_apply_to_image(const char *name, const gboolean duplicate, const int32_t imgid)
{
  dt_undo_start_group(dt_undo_get_global(), DT_UNDO_LT_HISTORY);
  dt_history_style_on_image(imgid, name, duplicate);
  dt_undo_end_group(dt_undo_get_global());
}

void dt_styles_delete_by_name_adv(const char *name, const gboolean raise)
{
  int id = 0;
  if((id = dt_styles_get_id_by_name(name)) != 0)
  {
    dt_style_repository_delete(id);

    if(raise)
      dt_history_changed(DT_HISTORY_CHANGE_STYLES);
  }
}

void dt_styles_delete_by_name(const char *name)
{
  dt_styles_delete_by_name_adv(name, TRUE);
}

typedef struct _item_list_ctx_t
{
  GList *result;
  gboolean params;
  int32_t imgid;
} _item_list_ctx_t;

static void _prepend_item(void *user_data, const int num, const int multi_priority,
                          const int module_version, const char *operation, const int enabled,
                          const void *params, const int32_t params_size,
                          const void *blendop_params, const int32_t blendop_params_size,
                          const int blendop_version, const char *multi_name,
                          const int selimg_num, const double iop_order)
{
  _item_list_ctx_t *ctx = (_item_list_ctx_t *)user_data;

  if(strcmp(operation, "mask_manager") == 0) return;

  // name of current item of style
  char iname[512] = { 0 };
  dt_style_item_t *item = calloc(1, sizeof(dt_style_item_t));

  item->num = num;
  item->multi_priority = multi_priority;
  item->selimg_num = -1;
  item->module_version = module_version;
  item->enabled = enabled;

  const gboolean has_multi_name = multi_name && *multi_name && (strcmp(multi_name, "0") != 0);

  if(ctx->params)
  {
    // when we get the parameters we do not want to get the operation localized as this
    // is used to compare against the internal module name.

    if(has_multi_name)
      g_snprintf(iname, sizeof(iname), "%s %s", operation, multi_name);
    else
      g_snprintf(iname, sizeof(iname), "%s", operation);

    item->params = malloc(params_size);
    item->params_size = params_size;
    memcpy(item->params, params, params_size);

    item->blendop_params = malloc(blendop_params_size);
    item->blendop_params_size = blendop_params_size;
    item->blendop_version = blendop_version;
    memcpy(item->blendop_params, blendop_params, blendop_params_size);
  }
  else
  {
    const gchar *itname = dt_iop_get_localized_name(operation);

    if(has_multi_name)
      g_snprintf(iname, sizeof(iname), "%s %s", itname, multi_name);
    else
      g_snprintf(iname, sizeof(iname), "%s", itname);

    item->params = NULL;
    item->blendop_params = NULL;
    item->params_size = 0;
    item->blendop_params_size = 0;
    item->blendop_version = 0;
    if(ctx->imgid != UNKNOWN_IMAGE) item->selimg_num = selimg_num;
  }
  item->name = g_strdup(iname);
  item->operation = g_strdup(operation);
  item->multi_name = g_strdup(multi_name);
  item->iop_order = iop_order;
  ctx->result = g_list_prepend(ctx->result, item);
}

GList *dt_styles_get_item_list(const char *name, gboolean params, int32_t imgid)
{
  _item_list_ctx_t ctx = { .result = NULL, .params = params, .imgid = imgid };
  int id = 0;
  if((id = dt_styles_get_id_by_name(name)) != 0)
  {
    if(params)
      dt_style_repository_foreach_item_with_params(id, _prepend_item, &ctx);
    else if(imgid != UNKNOWN_IMAGE)
      dt_style_repository_foreach_item_against_image(id, imgid, _prepend_item, &ctx);
    else
      dt_style_repository_foreach_item(id, _prepend_item, &ctx);
  }
  return g_list_reverse(ctx.result);   // list was built in reverse order, so un-reverse it
}

char *dt_styles_get_item_list_as_string(const char *name)
{
  GList *items = dt_styles_get_item_list(name, FALSE, -1);
  if(IS_NULL_PTR(items)) return NULL;

  GList *names = NULL;
  for(GList *items_iter = items; items_iter; items_iter = g_list_next(items_iter))
  {
    dt_style_item_t *item = (dt_style_item_t *)items_iter->data;
    names = g_list_prepend(names, g_strdup(item->name));
  }
  names = g_list_reverse(names);  // list was built in reverse order, so un-reverse it

  char *result = dt_util_glist_to_str("\n", names);
  g_list_free_full(names, dt_free_gpointer);
  names = NULL;
  g_list_free_full(items, dt_style_item_free);
  items = NULL;
  return result;
}

static void _prepend_style(void *user_data, const char *name, const char *description)
{
  GList **result = (GList **)user_data;
  dt_style_t *s = g_malloc(sizeof(dt_style_t));
  s->name = g_strdup(name);
  s->description = g_strdup(description);
  *result = g_list_prepend(*result, s);
}

GList *dt_styles_get_list(const char *filter)
{
  GList *result = NULL;
  dt_style_repository_foreach_style(filter, _prepend_style, &result);
  return g_list_reverse(result);  // list was built in reverse order, so un-reverse it
}

typedef struct _write_items_ctx_t
{
  xmlTextWriterPtr writer;
  GList *without_shapes; // nums of the items whose drawn mask comes without its shapes
} _write_items_ctx_t;

static void _write_item(void *user_data, const int num, const int multi_priority,
                        const int module_version, const char *operation, const int enabled,
                        const void *params, const int32_t params_size,
                        const void *blendop_params, const int32_t blendop_params_size,
                        const int blendop_version, const char *multi_name,
                        const int selimg_num, const double iop_order)
{
  _write_items_ctx_t *ctx = (_write_items_ctx_t *)user_data;
  xmlTextWriterPtr writer = ctx->writer;
  char *op_params_txt = dt_exif_xmp_encode((const unsigned char *)params, params_size, NULL);
  char *blendop_params_txt = dt_exif_xmp_encode((const unsigned char *)blendop_params, blendop_params_size, NULL);

  xmlTextWriterStartElement(writer, BAD_CAST "plugin");
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "num", "%d", num);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "module", "%d", module_version);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "operation", "%s", operation);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "op_params", "%s", op_params_txt);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "enabled", "%d", enabled);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "blendop_params", "%s", blendop_params_txt);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "blendop_version", "%d", blendop_version);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_priority", "%d", multi_priority);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "multi_name", "%s", multi_name);
  // written only when off, so a style that brings every shape keeps the file it always had
  if(g_list_find(ctx->without_shapes, GINT_TO_POINTER(num)))
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "apply_shapes", "%d", 0);
  xmlTextWriterEndElement(writer);

  // dt_style_encode() used to leak both of these on every item of every style ever saved
  dt_free(op_params_txt);
  dt_free(blendop_params_txt);
}

typedef struct _write_shapes_ctx_t
{
  xmlTextWriterPtr writer;
  gboolean open;
} _write_shapes_ctx_t;

/* A shape is written as attributes only: a parser that predates shapes reads the text of any element
 * named like one of its own (name, description...) wherever it sits, and ignores attributes. */
static void _write_shape(void *user_data, const int formid, const int form, const char *name,
                         const int version, const void *points, const int points_len,
                         const int points_count, const void *source, const int source_len)
{
  _write_shapes_ctx_t *ctx = (_write_shapes_ctx_t *)user_data;
  if(!ctx->open)
  {
    xmlTextWriterStartElement(ctx->writer, BAD_CAST "shapes");
    ctx->open = TRUE;
  }

  char *points_txt = dt_exif_xmp_encode((const unsigned char *)points, points_len, NULL);
  char *source_txt = dt_exif_xmp_encode((const unsigned char *)source, source_len, NULL);

  xmlTextWriterStartElement(ctx->writer, BAD_CAST "shape");
  xmlTextWriterWriteFormatAttribute(ctx->writer, BAD_CAST "formid", "%d", formid);
  xmlTextWriterWriteFormatAttribute(ctx->writer, BAD_CAST "type", "%d", form);
  xmlTextWriterWriteAttribute(ctx->writer, BAD_CAST "name", BAD_CAST(name ? name : ""));
  xmlTextWriterWriteFormatAttribute(ctx->writer, BAD_CAST "version", "%d", version);
  xmlTextWriterWriteFormatAttribute(ctx->writer, BAD_CAST "points_count", "%d", points_count);
  xmlTextWriterWriteAttribute(ctx->writer, BAD_CAST "points", BAD_CAST(points_txt ? points_txt : ""));
  xmlTextWriterWriteAttribute(ctx->writer, BAD_CAST "source", BAD_CAST(source_txt ? source_txt : ""));
  xmlTextWriterEndElement(ctx->writer);

  dt_free(points_txt);
  dt_free(source_txt);
}

void dt_styles_save_to_file(const char *style_name, const char *filedir, gboolean overwrite)
{
  char stylesdir[DT_PATH_MAX] = { 0 };
  if(IS_NULL_PTR(filedir))
  {
    dt_loc_get_user_config_dir(stylesdir, sizeof(stylesdir));
    g_strlcat(stylesdir, "/styles", sizeof(stylesdir));
    g_mkdir_with_parents(stylesdir, 00755);
    filedir = stylesdir;
  }

  int rc = 0;
  char stylename[DT_PATH_MAX];

  // generate filename based on name of style
  // convert all characters to underscore which are not allowed in filenames
  char *filename = g_strdup_printf("%s.dtstyle", style_name);
  g_strdelimit(filename, "/<>:\"\\|*?[]", '_');
  dt_concat_path_file(stylename, filedir, filename);
  dt_free(filename);

  // check if file exists
  if(g_file_test(stylename, G_FILE_TEST_EXISTS) == TRUE)
  {
    if(overwrite)
    {
      if(g_unlink(stylename))
      {
        dt_history_message(_("failed to overwrite style file for %s"), style_name);
        return;
      }
    }
    else
    {
      dt_history_message(_("style file for %s exists"), style_name);
      return;
    }
  }

  if(!dt_styles_exists(style_name)) return;

  xmlTextWriterPtr writer = xmlNewTextWriterFilename(stylename, 0);
  if(IS_NULL_PTR(writer))
  {
    fprintf(stderr, "[dt_styles_save_to_file] Error creating the xml writer\n, path: %s", stylename);
    return;
  }
  rc = xmlTextWriterStartDocument(writer, NULL, "UTF-8", NULL);
  if(rc < 0)
  {
    fprintf(stderr, "[dt_styles_save_to_file]: Error on encoding setting");
    return;
  }
  xmlTextWriterStartElement(writer, BAD_CAST "darktable_style");
  xmlTextWriterWriteAttribute(writer, BAD_CAST "version", BAD_CAST "1.0");

  xmlTextWriterStartElement(writer, BAD_CAST "info");
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "name", "%s", style_name);
  gchar *style_description = dt_styles_get_description(style_name);
  xmlTextWriterWriteFormatElement(writer, BAD_CAST "description", "%s", style_description);
  dt_free(style_description);
  GList *iop_list = dt_styles_module_order_list(style_name);
  if(iop_list)
  {
    char *iop_list_text = dt_ioppr_serialize_text_iop_order_list(iop_list);
    xmlTextWriterWriteFormatElement(writer, BAD_CAST "iop_list", "%s", iop_list_text);
    dt_free(iop_list_text);
    g_list_free_full(iop_list, dt_free_gpointer);
    iop_list = NULL;
  }
  xmlTextWriterEndElement(writer);

  const int id = dt_styles_get_id_by_name(style_name);
  xmlTextWriterStartElement(writer, BAD_CAST "style");
  _write_items_ctx_t items_ctx = { .writer = writer,
                                   .without_shapes = dt_style_repository_get_items_without_shapes(id) };
  dt_style_repository_foreach_item_for_export(id, _write_item, &items_ctx);
  g_list_free(items_ctx.without_shapes);
  xmlTextWriterEndElement(writer);

  _write_shapes_ctx_t shapes_ctx = { .writer = writer, .open = FALSE };
  dt_style_repository_foreach_shape(id, _write_shape, &shapes_ctx);
  xmlTextWriterEndDocument(writer);
  xmlFreeTextWriter(writer);
}

static StyleData *dt_styles_style_data_new()
{
  StyleInfoData *info = g_new0(StyleInfoData, 1);
  info->name = g_string_new("");
  info->description = g_string_new("");

  StyleData *data = g_new0(StyleData, 1);
  data->info = info;
  data->in_plugin = FALSE;
  data->plugins = NULL;

  return data;
}

static StylePluginData *dt_styles_style_plugin_new()
{
  StylePluginData *plugin = g_new0(StylePluginData, 1);
  plugin->operation = g_string_new("");
  plugin->op_params = g_string_new("");
  plugin->blendop_params = g_string_new("");
  plugin->multi_name = g_string_new("");
  plugin->iop_order = -1.0;
  plugin->apply_shapes = 1;
  return plugin;
}

static void _style_shape_free(gpointer data)
{
  StyleShapeData *shape = (StyleShapeData *)data;
  dt_free(shape->name);
  dt_free(shape->points);
  dt_free(shape->source);
  dt_free(shape);
}

static void dt_styles_style_data_free(StyleData *style, gboolean free_segments)
{
  g_string_free(style->info->name, free_segments);
  g_string_free(style->info->description, free_segments);
  g_list_free_full(style->info->iop_list, dt_free_gpointer);
  style->info->iop_list = NULL;
  g_list_free(style->plugins);
  style->plugins = NULL;
  g_list_free_full(style->shapes, _style_shape_free);
  style->shapes = NULL;
  dt_free(style);
}

static void dt_styles_start_tag_handler(GMarkupParseContext *context, const gchar *element_name,
                                        const gchar **attribute_names, const gchar **attribute_values,
                                        gpointer user_data, GError **error)
{
  StyleData *style = user_data;
  const gchar *elt = g_markup_parse_context_get_element(context);

  // We need to append the contents of any subtags to the content field
  // for this we need to know when we are inside the note-content tag
  if(g_ascii_strcasecmp(elt, "plugin") == 0)
  {
    style->in_plugin = TRUE;
    style->plugins = g_list_prepend(style->plugins, dt_styles_style_plugin_new());
  }
  else if(g_ascii_strcasecmp(elt, "shape") == 0)
  {
    // An attribute this version does not know is skipped, so a newer file still imports.
    StyleShapeData *shape = g_new0(StyleShapeData, 1);
    shape->valid = TRUE;
    for(int k = 0; attribute_names[k]; k++)
    {
      const char *attribute = attribute_names[k];
      const char *value = attribute_values[k];
      if(!g_ascii_strcasecmp(attribute, "formid"))
        shape->id = atoi(value);
      else if(!g_ascii_strcasecmp(attribute, "type"))
        shape->type = atoi(value);
      else if(!g_ascii_strcasecmp(attribute, "version"))
        shape->version = atoi(value);
      else if(!g_ascii_strcasecmp(attribute, "points_count"))
        shape->points_count = atoi(value);
      else if(!g_ascii_strcasecmp(attribute, "name"))
      {
        dt_free(shape->name);
        shape->name = g_strdup(value);
      }
      else if(!g_ascii_strcasecmp(attribute, "points") && *value)
      {
        dt_free(shape->points);
        shape->points = dt_exif_xmp_decode(value, strlen(value), &shape->points_len);
        shape->valid &= !IS_NULL_PTR(shape->points);
      }
      else if(!g_ascii_strcasecmp(attribute, "source") && *value)
      {
        dt_free(shape->source);
        shape->source = dt_exif_xmp_decode(value, strlen(value), &shape->source_len);
        shape->valid &= !IS_NULL_PTR(shape->source);
      }
    }
    style->shapes = g_list_prepend(style->shapes, shape);
  }
}

static void dt_styles_end_tag_handler(GMarkupParseContext *context, const gchar *element_name,
                                      gpointer user_data, GError **error)
{
  StyleData *style = user_data;
  const gchar *elt = g_markup_parse_context_get_element(context);

  // We need to append the contents of any subtags to the content field
  // for this we need to know when we are inside the note-content tag
  if(g_ascii_strcasecmp(elt, "plugin") == 0)
  {
    style->in_plugin = FALSE;
  }
}

static void dt_styles_style_text_handler(GMarkupParseContext *context, const gchar *text, gsize text_len,
                                         gpointer user_data, GError **error)
{
  StyleData *style = user_data;
  const gchar *elt = g_markup_parse_context_get_element(context);

  if(g_ascii_strcasecmp(elt, "name") == 0)
  {
    g_string_append_len(style->info->name, text, text_len);
  }
  else if(g_ascii_strcasecmp(elt, "description") == 0)
  {
    g_string_append_len(style->info->description, text, text_len);
  }
  else if(g_ascii_strcasecmp(elt, "iop_list") == 0)
  {
    style->info->iop_list = dt_ioppr_deserialize_text_iop_order_list(text);
    if(style->info->iop_list)
      style->info->iop_list = dt_ioppr_insert_missing_modules(style->info->iop_list);
  }
  else if(style->in_plugin)
  {
    StylePluginData *plug = style->plugins->data;
    if(g_ascii_strcasecmp(elt, "operation") == 0)
    {
      g_string_append_len(plug->operation, text, text_len);
    }
    else if(g_ascii_strcasecmp(elt, "op_params") == 0)
    {
      g_string_append_len(plug->op_params, text, text_len);
    }
    else if(g_ascii_strcasecmp(elt, "blendop_params") == 0)
    {
      g_string_append_len(plug->blendop_params, text, text_len);
    }
    else if(g_ascii_strcasecmp(elt, "blendop_version") == 0)
    {
      plug->blendop_version = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "multi_priority") == 0)
    {
      plug->multi_priority = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "multi_name") == 0)
    {
      g_string_append_len(plug->multi_name, text, text_len);
    }
    else if(g_ascii_strcasecmp(elt, "num") == 0)
    {
      plug->num = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "module") == 0)
    {
      plug->module = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "enabled") == 0)
    {
      plug->enabled = atoi(text);
    }
    else if(g_ascii_strcasecmp(elt, "iop_order") == 0)
    {
      plug->iop_order = atof(text);
    }
    else if(g_ascii_strcasecmp(elt, "apply_shapes") == 0)
    {
      plug->apply_shapes = atoi(text);
    }
  }
}

static GMarkupParser dt_style_parser = {
  dt_styles_start_tag_handler,  // Start element handler
  dt_styles_end_tag_handler,    // End element handler
  dt_styles_style_text_handler, // Text element handler
  NULL,                         // Passthrough handler
  NULL                          // Error handler
};

static void dt_style_plugin_save(StylePluginData *plugin, gpointer styleId)
{
  int id = GPOINTER_TO_INT(styleId);

  const char *param_c = plugin->op_params->str;
  const int param_c_len = strlen(param_c);
  int params_len = 0;
  unsigned char *params = dt_exif_xmp_decode(param_c, param_c_len, &params_len);

  /* decode and store blendop params */
  int blendop_params_len = 0;
  unsigned char *blendop_params = dt_exif_xmp_decode(
      plugin->blendop_params->str, strlen(plugin->blendop_params->str), &blendop_params_len);

  dt_style_repository_insert_item(id, plugin->num, plugin->module, plugin->operation->str, params,
                                  params_len, plugin->enabled, blendop_params, blendop_params_len,
                                  plugin->blendop_version, plugin->multi_priority,
                                  plugin->multi_name->str, plugin->apply_shapes);

  dt_free(params);
}

static void _style_shape_save(StyleShapeData *shape, gpointer styleId)
{
  // what the points and source say is checked when the shape is read back
  if(!shape->valid)
  {
    fprintf(stderr, "[dt_styles_import_from_file] shape %d cannot be decoded, skipped\n", shape->id);
    return;
  }
  dt_style_repository_insert_shape(GPOINTER_TO_INT(styleId), shape->id, shape->type,
                                   shape->name ? shape->name : "", shape->version, shape->points,
                                   shape->points_len, shape->points_count, shape->source,
                                   shape->source_len);
}

static void dt_style_save(StyleData *style)
{
  int id = 0;
  if(IS_NULL_PTR(style)) return;

  /* first create the style header */
  if(!dt_styles_create_style_header(style->info->name->str, style->info->description->str, style->info->iop_list)) return;

  if((id = dt_styles_get_id_by_name(style->info->name->str)) != 0)
  {
    g_list_foreach(style->plugins, (GFunc)dt_style_plugin_save, GINT_TO_POINTER(id));
    style->shapes = g_list_reverse(style->shapes);
    g_list_foreach(style->shapes, (GFunc)_style_shape_save, GINT_TO_POINTER(id));
    dt_history_message(_("style %s was successfully imported"), style->info->name->str);
  }
}

void dt_styles_import_from_file(const char *style_path)
{
  FILE *style_file;
  StyleData *style;
  GMarkupParseContext *parser;
  gchar buf[1024];

  style = dt_styles_style_data_new();
  parser = g_markup_parse_context_new(&dt_style_parser, 0, style, NULL);

  if((style_file = g_fopen(style_path, "r")))
  {

    while(!feof(style_file))
    {
      const size_t num_read = fread(buf, sizeof(gchar), sizeof(buf), style_file);

      if(num_read == 0)
      {
        break;
      }
      else if(num_read == -1)
      {
        // FIXME: ferror?
        // ERROR !
        break;
      }

      if(!g_markup_parse_context_parse(parser, buf, num_read, NULL))
      {
        g_markup_parse_context_free(parser);
        dt_styles_style_data_free(style, TRUE);
        fclose(style_file);
        return;
      }
    }
  }
  else
  {
    // Failed to open file, clean up.
    dt_history_message(_("could not read file `%s'"), style_path);
    g_markup_parse_context_free(parser);
    dt_styles_style_data_free(style, TRUE);
    return;
  }

  if(!g_markup_parse_context_end_parse(parser, NULL))
  {
    g_markup_parse_context_free(parser);
    dt_styles_style_data_free(style, TRUE);
    fclose(style_file);
    return;
  }
  g_markup_parse_context_free(parser);
  // save data
  dt_style_save(style);
  //
  dt_styles_style_data_free(style, TRUE);
  fclose(style_file);

  dt_history_changed(DT_HISTORY_CHANGE_STYLES);
}

gchar *dt_styles_get_description(const char *name)
{
  const int id = dt_styles_get_id_by_name(name);
  if(id == 0) return NULL;
  return dt_style_repository_get_description(id);
}

int32_t dt_styles_get_id_by_name(const char *name)
{
  return dt_style_repository_get_id_by_name(name);
}

static void _collect_style(void *user_data, const char *name, const char *description)
{
  dt_style_t **out = (dt_style_t **)user_data;
  dt_style_t *s = g_malloc(sizeof(dt_style_t));
  s->name = g_strdup(name);
  s->description = g_strdup(description);
  *out = s;
}

dt_style_t *dt_styles_get_by_name(const char *name)
{
  dt_style_t *s = NULL;
  dt_style_repository_get_style(name, _collect_style, &s);
  return s;
}

void dt_styles_cleanup(void)
{
  dt_style_repository_cleanup();
}

#undef DT_IOP_ORDER_INFO
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
