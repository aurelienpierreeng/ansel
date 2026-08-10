/*
    This file is part of darktable,
    Copyright (C) 2010 Alexandre Prokoudine.
    Copyright (C) 2010-2011 Henrik Andersson.
    Copyright (C) 2010-2011 johannes hanika.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 José Carlos García Sogo.
    Copyright (C) 2013 Jérémy Rosen.
    Copyright (C) 2014 Ulrich Pegelow.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019-2022 Pascal Obry.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020-2021 Aldric Renaudin.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2021 Diederik Ter Rahe.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2021 solarer.
    Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.
    
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
#include "common/colorlabels.h"
#include "common/collection.h"
#include "system/macros.h"
#include "database/colorlabel_repository.h"
#include "caches/image_cache.h"
#include "common/undo.h"
#include "control/control.h"

#include <gdk/gdkkeysyms.h>

const char *dt_colorlabels_name[] = {
  "red", "yellow", "green", "blue", "purple",
  NULL // termination
};

char * dt_colorlabels_get_name(const int label)
{
  switch(label)
  {
    case 0:
      return _("red");
    case 1:
      return _("yellow");
    case 2:
      return _("green");
    case 3:
      return _("blue");
    case 4:
      return _("purple");
    case 5:
      return _("empty");
    default:
      return _("unknown/invalid");
  }
}

typedef struct dt_undo_colorlabels_t
{
  int32_t imgid;
  int before;
  int after;
} dt_undo_colorlabels_t;

int dt_colorlabels_get_labels(const int32_t imgid)
{
  return dt_colorlabel_repository_get(imgid);
}

void dt_colorlabels_set_labels(const int32_t imgid, const int colors)
{
  for(int color = 0; color < 5; color++)
  {
    if(colors & (1 << color))
      dt_colorlabels_set_label(imgid, color);
    else
      dt_colorlabels_remove_label(imgid, color);
  }
}

static void _pop_undo_execute(const int32_t imgid, const int before, const int after)
{
  dt_image_t *image = dt_image_cache_get(imgid, 'w');
  if(IS_NULL_PTR(image)) return;

  // Write to image
  for(int color = 0; color < 5; color++)
  {
    if(after & (1 << color))
    {
      if (!(before & (1 << color)))
        image->color_labels |= (1 << color);
    }
    else if (before & (1 << color))
      image->color_labels &= ~(1 << color);
  }

  // Update image cache object and write to DB in _write_release
  image->color_labels = dt_colorlabels_get_labels(imgid);
  dt_image_cache_write_release(image, DT_IMAGE_CACHE_SAFE);
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_COLORLABELS)
  {
    for(GList *list = (GList *)data; list; list = g_list_next(list))
    {
      dt_undo_colorlabels_t *undocolorlabels = (dt_undo_colorlabels_t *)list->data;

      const int before = (action == DT_ACTION_UNDO) ? undocolorlabels->after : undocolorlabels->before;
      const int after = (action == DT_ACTION_UNDO) ? undocolorlabels->before : undocolorlabels->after;
      _pop_undo_execute(undocolorlabels->imgid, before, after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(undocolorlabels->imgid));
    }
    dt_collection_hint_message(dt_collection_get_global());
  }
}

static void _colorlabels_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free(l);
  l = NULL;
}

void dt_colorlabels_remove_labels(const int32_t imgid)
{
  dt_colorlabel_repository_remove_all(imgid);
}

void dt_colorlabels_set_label(const int32_t imgid, const int color)
{
  dt_colorlabel_repository_set(imgid, color);
}

void dt_colorlabels_remove_label(const int32_t imgid, const int color)
{
  dt_colorlabel_repository_remove(imgid, color);
}

void dt_colorlabels_cleanup(void)
{
  dt_colorlabel_repository_cleanup();
}

typedef enum dt_colorlabels_actions_t
{
  DT_CA_SET = 0,
  DT_CA_ADD,
  DT_CA_TOGGLE
} dt_colorlabels_actions_t;


static void _colorlabels_execute(GList *imgs, const int labels, GList **undo, const gboolean undo_on, int action)
{
  if(action == DT_CA_TOGGLE)
  {
    // if we are supposed to toggle color labels, first check if all images have that label
    for(const GList *image = g_list_first(imgs); image; image = g_list_next((GList *)image))
    {
      const int32_t image_id = GPOINTER_TO_INT(image->data);

      dt_image_t *img = dt_image_cache_get(image_id, 'r');
      if(IS_NULL_PTR(img)) continue;

      const int before = img->color_labels;
      dt_image_cache_read_release(img);

      // as long as a single image does not have the label we do not toggle the label for all images
      // but add the label to all unlabeled images first
      if(!(before & labels))
      {
        action = DT_CA_ADD;
        break;
      }
    }
  }

  for(GList *image = g_list_first(imgs); image; image = g_list_next((GList *)image))
  {
    const int32_t image_id = GPOINTER_TO_INT(image->data);

    dt_image_t *img = dt_image_cache_get(image_id, 'w');
    if(IS_NULL_PTR(img)) continue;

    const int before = img->color_labels;
    int after = 0;
    switch(action)
    {
      case DT_CA_SET:
        after = labels;
        break;
      case DT_CA_ADD:
        after = before | labels;
        break;
      case DT_CA_TOGGLE:
        after = (before & labels) ? before & (~labels) : before | labels;
        break;
      default:
        after = before;
        break;
    }

    img->color_labels = after;
    dt_image_cache_write_release(img, DT_IMAGE_CACHE_SAFE);

    if(undo_on)
    {
      dt_undo_colorlabels_t *undocolorlabels = (dt_undo_colorlabels_t *)malloc(sizeof(dt_undo_colorlabels_t));
      undocolorlabels->imgid = image_id;
      undocolorlabels->before = before;
      undocolorlabels->after = after;
      *undo = g_list_append(*undo, undocolorlabels);
    }
  }
}

void dt_colorlabels_toggle_label_on_list(GList *list, const int color, const gboolean undo_on)
{
  const int label = 1<<color;
  GList *undo = NULL;
  if(undo_on) dt_undo_start_group(dt_undo_get_global(), DT_UNDO_COLORLABELS);

  if(color == 5)
  {
    _colorlabels_execute(list, 0, &undo, undo_on, DT_CA_SET);
  }
  else
  {
    _colorlabels_execute(list, label, &undo, undo_on, DT_CA_TOGGLE);
  }

  if(undo_on)
  {
    dt_undo_record(dt_undo_get_global(), NULL, DT_UNDO_COLORLABELS, undo, _pop_undo, _colorlabels_undo_data_free);
    dt_undo_end_group(dt_undo_get_global());
  }
  dt_collection_hint_message(dt_collection_get_global());
  dt_toast_log(_("Color label set to %s for %i image(s)"), dt_colorlabels_get_name(color), g_list_length(list));
}

int dt_colorlabels_check_label(const int32_t imgid, const int color)
{
  return dt_colorlabel_repository_has(imgid, color) ? 1 : 0;
}

// FIXME: XMP uses Red, Green, ... while we use red, green, ... What should this function return?
const char *dt_colorlabels_to_string(int label)
{
  if(label < 0 || label >= DT_COLORLABELS_LAST) return ""; // shouldn't happen
  return dt_colorlabels_name[label];
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
