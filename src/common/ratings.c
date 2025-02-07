/*
    This file is part of darktable,
    Copyright (C) 2011-2021 darktable developers.

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
#include "common/darktable.h"
#include "common/debug.h"
#include "common/image_cache.h"
#include "common/ratings.h"
#include "common/undo.h"
#include "common/grouping.h"
#include "views/view.h"
#include "control/conf.h"
#include "control/control.h"
#include "gui/gtk.h"


#define DT_RATINGS_UPGRADE -1
#define DT_RATINGS_DOWNGRADE -2
#define DT_RATINGS_REJECT -3
#define DT_RATINGS_UNREJECT -4

typedef struct dt_undo_ratings_t
{
  int imgid;
  int before;
  int after;
} dt_undo_ratings_t;

char *dt_ratings_get_name(const int rating)
{
  switch(rating)
  {
    case 0:
      return _("empty");
    case 1:
      return _("1 star");
    case 2:
      return _("2 stars");
    case 3:
      return _("3 stars");
    case 4:
      return _("4 stars");
    case 5:
      return _("5 stars");
    case 6:
      return _("rejected");
    default:
      return _("unknown/invalid");
  }
}

int dt_ratings_get(const int imgid)
{
  int stars = 0;
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'r');
  if(image)
  {
    if(image->flags & DT_IMAGE_REJECTED)
      stars = DT_VIEW_REJECT;
    else
      stars = DT_VIEW_RATINGS_MASK & image->flags;
    dt_image_cache_read_release(darktable.image_cache, image);
  }
  return stars;
}

static void _ratings_apply_to_image(const int imgid, const int rating)
{
  int new_rating = rating;
  dt_image_t *image = dt_image_cache_get(darktable.image_cache, imgid, 'w');

  if(image)
  {
    // apply or remove rejection
    if(new_rating == DT_RATINGS_REJECT)
      image->flags |= DT_IMAGE_REJECTED;
    else if(new_rating == DT_RATINGS_UNREJECT)
      image->flags &= ~DT_IMAGE_REJECTED;
    else
    {
      image->flags = (image->flags & ~(DT_IMAGE_REJECTED | DT_VIEW_RATINGS_MASK))
        | (DT_VIEW_RATINGS_MASK & new_rating);
    }
    // synch through:
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
  }
  else
  {
    dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_RELAXED);
  }
}

static void _pop_undo(gpointer user_data, dt_undo_type_t type, dt_undo_data_t data, dt_undo_action_t action, GList **imgs)
{
  if(type == DT_UNDO_RATINGS)
  {
    for(GList *list = (GList *)data; list; list = g_list_next(list))
    {
      dt_undo_ratings_t *ratings = (dt_undo_ratings_t *)list->data;
      _ratings_apply_to_image(ratings->imgid, (action == DT_ACTION_UNDO) ? ratings->before : ratings->after);
      *imgs = g_list_prepend(*imgs, GINT_TO_POINTER(ratings->imgid));
    }
    dt_collection_hint_message(darktable.collection);
  }
}

static void _ratings_undo_data_free(gpointer data)
{
  GList *l = (GList *)data;
  g_list_free(l);
}

// wrapper that does some precalculation to deal with toggle effects and rating increase/decrease
static void _ratings_apply(GList *imgs, const int rating, GList **undo, const gboolean undo_on)
{
  // REJECTION and SINGLE_STAR rating can have a toggle effect
  // but we only toggle off if ALL images have that rating
  // so we need to check every image first
  gboolean toggle = FALSE;

  if(rating == DT_VIEW_REJECT)
  {
    toggle = TRUE;
    for(const GList *images = g_list_first(imgs); images; images = g_list_next(images))
    {
      if(dt_ratings_get(GPOINTER_TO_INT(images->data)) != DT_VIEW_REJECT)
      {
        toggle = FALSE;
        break;
      }
    }
  }

  for(const GList *images = g_list_first(imgs); images; images = g_list_next(images))
  {
    const int image_id = GPOINTER_TO_INT(images->data);
    const int old_rating = dt_ratings_get(image_id);
    if(undo_on)
    {
      dt_undo_ratings_t *undoratings = (dt_undo_ratings_t *)malloc(sizeof(dt_undo_ratings_t));
      undoratings->imgid = image_id;
      undoratings->before = old_rating;
      undoratings->after = rating;
      *undo = g_list_append(*undo, undoratings);
    }

    int new_rating = rating;
    // do not 'DT_RATINGS_UPGRADE' or 'DT_RATINGS_UPGRADE' if image was rejected
    if(old_rating == DT_VIEW_REJECT && rating < DT_VIEW_DESERT)
      new_rating = DT_VIEW_REJECT;
    else if(rating == DT_RATINGS_UPGRADE)
      new_rating = MIN(DT_VIEW_STAR_5, old_rating + 1);
    else if(rating == DT_RATINGS_DOWNGRADE)
      new_rating = MAX(DT_VIEW_DESERT, old_rating - 1);
    else if(rating == DT_VIEW_STAR_1 && toggle)
      new_rating = DT_VIEW_DESERT;
    else if(rating == DT_VIEW_REJECT && toggle)
      new_rating = DT_RATINGS_UNREJECT;
    else if(rating == DT_VIEW_REJECT && !toggle)
      new_rating = DT_RATINGS_REJECT;

    _ratings_apply_to_image(image_id, new_rating);
  }
}

void dt_ratings_apply_on_list(GList *img, const int rating, const gboolean undo_on)
{
  if(img)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);

    _ratings_apply(img, rating, &undo, undo_on);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_RATINGS, undo, _pop_undo, _ratings_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    dt_collection_hint_message(darktable.collection);
    dt_toast_log(_("Rating set to %s"), dt_ratings_get_name(rating));
  }
}

void dt_ratings_apply_on_image(const int imgid, const int rating, const gboolean single_star_toggle,
                               const gboolean undo_on, const gboolean group_on)
{
  GList *imgs = NULL;
  int new_rating = rating;

  if(imgid > 0) imgs = g_list_prepend(imgs, GINT_TO_POINTER(imgid));

  if(imgs)
  {
    GList *undo = NULL;
    if(undo_on) dt_undo_start_group(darktable.undo, DT_UNDO_RATINGS);
    if(group_on) dt_grouping_add_grouped_images(&imgs);

    if(!g_list_shorter_than(imgs, 2)) // pop up a toast if rating multiple images at once
    {
      const guint count = g_list_length(imgs);
      if(new_rating == DT_VIEW_REJECT)
        dt_control_log(ngettext("rejecting %d image", "rejecting %d images", count), count);
      else
        dt_control_log(ngettext("applying rating %d to %d image", "applying rating %d to %d images", count),
                       new_rating, count);
    }

    _ratings_apply(imgs, new_rating, &undo, undo_on);

    if(undo_on)
    {
      dt_undo_record(darktable.undo, NULL, DT_UNDO_RATINGS, undo, _pop_undo, _ratings_undo_data_free);
      dt_undo_end_group(darktable.undo);
    }
    g_list_free(imgs);
  }
  else
    dt_control_log(_("no images selected to apply rating"));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
