/*
    This file is part of darktable,
    Copyright (C) 2019-2021 darktable developers.
    Copyright (C) 2022-2025 Aurélien PIERRE.

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
/** a class to manage a table of thumbnail for lighttable and filmstrip.  */

#include "dtgtk/thumbtable.h"
#include "dtgtk/thumbnail.h"
#include "common/collection.h"
#include "common/colorlabels.h"
#include "common/datetime.h"
#include "common/history.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/grouping.h"
#include "common/ratings.h"
#include "common/selection.h"
#include "common/undo.h"
#include "control/control.h"
#include "control/jobs/import_jobs.h"

#include "gui/drag_and_drop.h"
#include "views/view.h"
#include "bauhaus/bauhaus.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <glib-object.h>
#include <math.h>

static void _thumbtable_info_from_image(dt_thumbnail_image_info_t *info, const dt_image_t *img,
                                        const uint32_t history_items, const uint32_t group_members);

#ifndef NDEBUG
static gboolean _thumbtable_float_equal(const float a, const float b)
{
  return (isnan(a) && isnan(b)) || a == b;
}

static gboolean _thumbtable_double_equal(const double a, const double b)
{
  return (isnan(a) && isnan(b)) || a == b;
}

static void _thumbtable_debug_assert_info_matches_cache(const dt_thumbnail_image_info_t *sql_info,
                                                        const uint32_t history_items, const uint32_t group_members)
{
  if(!sql_info || sql_info->imgid <= 0) return;

  const dt_image_t *img = dt_image_cache_get(darktable.image_cache, sql_info->imgid, 'r');
  if(!img) return;

  dt_thumbnail_image_info_t cache_info = {0};
  _thumbtable_info_from_image(&cache_info, img, history_items, group_members);
  dt_image_cache_read_release(darktable.image_cache, img);

  g_assert_cmpint(sql_info->imgid, ==, cache_info.imgid);
  g_assert_cmpint(sql_info->film_id, ==, cache_info.film_id);
  g_assert_cmpint(sql_info->groupid, ==, cache_info.groupid);
  g_assert_cmpint(sql_info->version, ==, cache_info.version);
  g_assert_cmpint(sql_info->width, ==, cache_info.width);
  g_assert_cmpint(sql_info->height, ==, cache_info.height);
  g_assert_cmpint(sql_info->orientation, ==, cache_info.orientation);
  g_assert_cmpint(sql_info->p_width, ==, cache_info.p_width);
  g_assert_cmpint(sql_info->p_height, ==, cache_info.p_height);
  g_assert_cmpint(sql_info->flags, ==, cache_info.flags);
  g_assert_cmpint(sql_info->loader, ==, cache_info.loader);
  g_assert_cmpint(sql_info->rating, ==, cache_info.rating);
  g_assert_cmpint(sql_info->colorlabels, ==, cache_info.colorlabels);
  g_assert(sql_info->has_localcopy == cache_info.has_localcopy);
  g_assert(sql_info->has_audio == cache_info.has_audio);
  g_assert(sql_info->is_bw == cache_info.is_bw);
  g_assert(sql_info->is_bw_flow == cache_info.is_bw_flow);
  g_assert(sql_info->is_hdr == cache_info.is_hdr);
  g_assert(sql_info->is_altered == cache_info.is_altered);
  g_assert(sql_info->is_grouped == cache_info.is_grouped);
  g_assert((int64_t)sql_info->import_timestamp == (int64_t)cache_info.import_timestamp);
  g_assert((int64_t)sql_info->change_timestamp == (int64_t)cache_info.change_timestamp);
  g_assert((int64_t)sql_info->export_timestamp == (int64_t)cache_info.export_timestamp);
  g_assert((int64_t)sql_info->print_timestamp == (int64_t)cache_info.print_timestamp);
  g_assert(_thumbtable_float_equal(sql_info->exif_exposure, cache_info.exif_exposure));
  g_assert(_thumbtable_float_equal(sql_info->exif_exposure_bias, cache_info.exif_exposure_bias));
  g_assert(_thumbtable_float_equal(sql_info->exif_aperture, cache_info.exif_aperture));
  g_assert(_thumbtable_float_equal(sql_info->exif_iso, cache_info.exif_iso));
  g_assert(_thumbtable_float_equal(sql_info->exif_focal_length, cache_info.exif_focal_length));
  g_assert(_thumbtable_float_equal(sql_info->exif_focus_distance, cache_info.exif_focus_distance));
  g_assert((int64_t)sql_info->exif_datetime_taken == (int64_t)cache_info.exif_datetime_taken);
  g_assert(_thumbtable_double_equal(sql_info->geoloc_latitude, cache_info.geoloc_latitude));
  g_assert(_thumbtable_double_equal(sql_info->geoloc_longitude, cache_info.geoloc_longitude));
  g_assert(_thumbtable_double_equal(sql_info->geoloc_elevation, cache_info.geoloc_elevation));
  g_assert_cmpstr(sql_info->filename, ==, cache_info.filename);
  g_assert_cmpstr(sql_info->fullpath, ==, cache_info.fullpath);
  g_assert_cmpstr(sql_info->filmroll, ==, cache_info.filmroll);
  g_assert_cmpstr(sql_info->folder, ==, cache_info.folder);
  g_assert_cmpstr(sql_info->datetime, ==, cache_info.datetime);
  g_assert_cmpstr(sql_info->camera, ==, cache_info.camera);
  g_assert_cmpstr(sql_info->exif_maker, ==, cache_info.exif_maker);
  g_assert_cmpstr(sql_info->exif_model, ==, cache_info.exif_model);
  g_assert_cmpstr(sql_info->exif_lens, ==, cache_info.exif_lens);
}
#endif

static void _thumbtable_info_finalize(dt_thumbnail_image_info_t *info)
{
  if(!info) return;

  // Compute derived UI fields once per collection build or refresh.
  // This avoids repeated per-thumb image-cache reads just to format UI strings/flags.
  dt_image_t tmp = {0};
  tmp.flags = info->flags;
  g_strlcpy(tmp.filename, info->filename, sizeof(tmp.filename));

  info->has_localcopy = (info->flags & DT_IMAGE_LOCAL_COPY);
  info->has_audio = (info->flags & DT_IMAGE_HAS_WAV);
  info->rating = (info->flags & DT_IMAGE_REJECTED) ? DT_VIEW_REJECT : (info->flags & DT_VIEW_RATINGS_MASK);
  info->is_bw = dt_image_monochrome_flags(&tmp);
  info->is_bw_flow = dt_image_use_monochrome_workflow(&tmp);
  info->is_hdr = dt_image_is_hdr(&tmp);

  if(!info->camera[0])
  {
    const char *maker = info->exif_maker;
    const char *model = info->exif_model;
    char mk[64] = { 0 };
    char md[64] = { 0 };
    char al[64] = { 0 };

    if((info->exif_maker[0] || info->exif_model[0])
       && dt_imageio_lookup_makermodel(info->exif_maker, info->exif_model,
                                       mk, sizeof(mk), md, sizeof(md), al, sizeof(al)))
    {
      if(mk[0]) maker = mk;
      if(md[0]) model = md;
    }

    g_strlcpy(info->camera, maker, sizeof(info->camera));
    const size_t len = strlen(maker);
    if(len < sizeof(info->camera) - 1)
      info->camera[len] = ' ';
    g_strlcpy(info->camera + len + 1, model, sizeof(info->camera) - len - 1);
  }

  dt_datetime_gtimespan_to_local(info->datetime, sizeof(info->datetime), info->exif_datetime_taken, FALSE, FALSE);

  if(info->film_id < 0 || !info->folder[0])
  {
    g_strlcpy(info->filmroll, _("orphaned image"), sizeof(info->filmroll));
  }
  else
  {
    g_strlcpy(info->filmroll, dt_image_film_roll_name(info->folder), sizeof(info->filmroll));
  }

  if(info->folder[0] && info->filename[0])
    g_snprintf(info->fullpath, sizeof(info->fullpath), "%s" G_DIR_SEPARATOR_S "%s", info->folder, info->filename);
}

static void _thumbtable_info_from_image(dt_thumbnail_image_info_t *info, const dt_image_t *img,
                                        const uint32_t history_items, const uint32_t group_members)
{
  if(!info || !img) return;

  // Refresh a single LUT entry from the image cache for write-driven updates
  // (ratings, color labels, etc. are updated through the cache APIs).
  memset(info, 0, sizeof(*info));

  info->imgid = img->id;
  info->film_id = img->film_id;
  info->groupid = img->group_id;
  info->version = img->version;
  info->width = img->width;
  info->height = img->height;
  info->orientation = img->orientation;
  info->p_width = img->p_width;
  info->p_height = img->p_height;
  info->flags = img->flags;
  info->loader = img->loader;
  info->import_timestamp = img->import_timestamp;
  info->change_timestamp = img->change_timestamp;
  info->export_timestamp = img->export_timestamp;
  info->print_timestamp = img->print_timestamp;
  info->exif_exposure = img->exif_exposure;
  info->exif_exposure_bias = img->exif_exposure_bias;
  info->exif_aperture = img->exif_aperture;
  info->exif_iso = img->exif_iso;
  info->exif_focal_length = img->exif_focal_length;
  info->exif_focus_distance = img->exif_focus_distance;
  info->exif_datetime_taken = img->exif_datetime_taken;
  info->geoloc_latitude = img->geoloc.latitude;
  info->geoloc_longitude = img->geoloc.longitude;
  info->geoloc_elevation = img->geoloc.elevation;
  g_strlcpy(info->filename, img->filename, sizeof(info->filename));
  g_strlcpy(info->exif_maker, img->exif_maker, sizeof(info->exif_maker));
  g_strlcpy(info->exif_model, img->exif_model, sizeof(info->exif_model));
  g_strlcpy(info->exif_lens, img->exif_lens, sizeof(info->exif_lens));
  g_strlcpy(info->camera, img->camera_makermodel, sizeof(info->camera));
  dt_image_film_roll_directory(img, info->folder, sizeof(info->folder));
  info->colorlabels = img->color_labels;
  info->is_grouped = (group_members > 1);
  info->is_altered = (history_items > 0);

  _thumbtable_info_finalize(info);
}


/**
 * @file thumbtable.c
 *
 * We keep a double reference of thumbnail objects for the current collection:
 *  - as a linked list of variable length, in table->list
 *  - as an array of fixed length, in table->lut.
 *
 * The linked list is used to keep track of allocated objects to update, redraw and free.
 * Its length is limited to 840 elements or whatever is visible inside viewport
 * at current scroll level. It's garbage-collected.
 *
 * The LUT is used to speed up lookups for thumbnails at known, bounded positions in sequential
 * order (position in collection = (rowid - 1) in SQLite result = order in GUI = index in the LUT).
 * This LUT prevents us from re-querying the collection in SQLite all the time using:
 * "SELECT rowid, imgid FROM main.collected_images WHERE rowid > min_row AND rowid <= max_row ORDER BY rowid ASC".
 * Note though that SQLite starts indexing at 1, so there is an unit offset.
 * It also keeps a reference to the thumbnail objects, but objets should never be freed from there.
 * Given that collections set on root folders contain all the images from their children,
 * the number of elements in a LUT can be anything from 1 to several 100k images.
 *
 * It is expected that thumbnails alloc/free always happen using table->list,
 * and that table->lut only updates its references accordingly, because table->list
 * will typically lead to fewer loop incrementations.
 *
 * Keep that in mind if/when extending features.
 *
 * For image collections having up to 1000 items,
 * we could just statically reset/init the list of thumbnails once when the collection changes, then only resize
 * thumbnails at runtime. But for collections of thousands of images, while adding child widgets is fairly fast,
 * considering, detaching those widgets from the parent takes ages (several orders of magnitude more than
 * attaching). So we have no choice here but to attach and detach dynamically, as to keep the number of children
 * reasonable. "Reasonable" is we populate the current viewport page (at current scrolling position),
 * the previous and the next ones, as to ensure smooth scrolling.
 *
 * The dimensions of the full collection grid are only ever virtual, but we need to make them real for the scrollbars to behave properly
 * through dynamic loading and unloading of thumbnails.
 * So we set the grid area to what it would be if we loaded all thumbnails.
 **/

void _dt_thumbtable_empty_list(dt_thumbtable_t *table);

static int _grab_focus(dt_thumbtable_t *table)
{
  // Ensure all previous Gtk events are finished,
  // aka the scrolled window widget inits or else the scroll
  // will have no effect
  while(gtk_events_pending())
    gtk_main_iteration();

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    // Grab focus here otherwise, on first click over the grid,
    // scrolled window gets scrolled all the way to the top and it's annoying.
    // This can work only if the grid is mapped and realized, which we ensure
    // by wrapping that in a g_idle() method.
    gtk_widget_grab_focus(table->grid);
  }
  dt_thumbtable_scroll_to_selection(table);
  return 0;
}

// We can't trust the mouse enter/leave events on thumnbails to properly
// update active thumbnail styling, so we need to catch the signal here and update the whole list.
void _mouse_over_image_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(!table->lut || table->collection_count == 0) return;

  const int32_t imgid = dt_control_get_mouse_over_id();

  dt_pthread_mutex_lock(&table->lock);

  int32_t group_id = UNKNOWN_IMAGE;

  // We iterate and update only over the visible range of thumbs + 2 rows as a safety margin
  int32_t row_start = CLAMP(table->min_row_id - 2 * table->thumbs_per_row, 0, table->collection_count - 1);
  int32_t row_end = CLAMP(table->max_row_id + 2 * table->thumbs_per_row, 0, table->collection_count - 1);
  for(int rowid = row_start; rowid <= row_end; rowid++)
  {
    dt_thumbnail_t *thumb = table->lut[rowid].thumb;
    if(!thumb) continue; // thumb object not inited

    const gboolean mouse_over = thumb->mouse_over;
    dt_thumbnail_set_mouseover(thumb, thumb->imgid == imgid);

    if(thumb->imgid == imgid && table->lut[rowid].group_members > 1)
      group_id = thumb->groupid;

    if(thumb->mouse_over != mouse_over)
      gtk_widget_queue_draw(thumb->widget);
  }

  // Now, we update all the thumbs of the same image group
  if(table->draw_group_borders)
  {
    for(int rowid = row_start; rowid <= row_end; rowid++)
    {
      dt_thumbnail_t *thumb = table->lut[rowid].thumb;
      if(!thumb) continue; // thumb object not inited

      // In CSS:
      // images borders from non-grouped images are transparent (default),
      // images borders from non-hovered groups, when there is none, are dark orange (base border classes)
      // images borders from the hovered group, when there is one, are bright orange (overwrite base border classes)
      // images borders from non-hovered groups, when there is one, are transparent (overwrite base border classes width default)
      // Here we dispatch the additional CSS classes alloying to overwrite
      // the base border classes.
      if(thumb->groupid == group_id)
      {
        dt_gui_add_class(thumb->widget, "hovered-group");
        dt_gui_remove_class(thumb->widget, "non-hovered-group");
      }
      else if(group_id == UNKNOWN_IMAGE)
      {
        dt_gui_remove_class(thumb->widget, "hovered-group");
        dt_gui_remove_class(thumb->widget, "non-hovered-group");
      }
      else
      {
        dt_gui_remove_class(thumb->widget, "hovered-group");
        dt_gui_add_class(thumb->widget, "non-hovered-group");
      }
    }
  }

  dt_pthread_mutex_unlock(&table->lock);
}


static void _rowid_to_position(dt_thumbtable_t *table, int rowid, int *x, int *y)
{
  if(table->thumbs_per_row < 1) return;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    int row = rowid / table->thumbs_per_row;  // euclidean division
    int col = rowid % table->thumbs_per_row;
    *x = col * table->thumb_width;
    *y = row * table->thumb_height;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    *x = rowid * table->thumb_width;
    *y = 0;
  }
}

// Needs updated table->x_position and table->y_position
static int _position_to_rowid(dt_thumbtable_t *table, const double x, const double y)
{
  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    // Attempt to get the image rowid sitting in the center of the middle row
    return (y + table->view_height / 2) / table->thumb_height * table->thumbs_per_row
      + table->thumbs_per_row / 2 - 1;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    return x + (table->view_width / 2.) / table->thumb_width;
  }
  return UNKNOWN_IMAGE;
}

// Find the x, y coordinates of any given thumbnail
// Return TRUE if a position could be computed
// thumb->rowid and table->thumbs_per_row need to have been inited before calling this
static gboolean _set_thumb_position(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  if(table->thumbs_per_row < 1) return FALSE;
  _rowid_to_position(table, thumb->rowid, &thumb->x, &thumb->y);
  return TRUE;
}

// Updates table->x_position and table->y_position
void dt_thumbtable_get_scroll_position(dt_thumbtable_t *table, double *x, double *y)
{
  *y = gtk_adjustment_get_value(table->v_scrollbar);
  *x = gtk_adjustment_get_value(table->h_scrollbar);
}

void dt_thumbtable_set_active_rowid(dt_thumbtable_t *table)
{
  double x = 0.;
  double y = 0.;
  dt_thumbtable_get_scroll_position(table, &x, &y);
  table->rowid = _position_to_rowid(table, x, y);
}

static int dt_thumbtable_scroll_to_position(dt_thumbtable_t *table, const double x, const double y)
{
  gtk_adjustment_set_value(table->v_scrollbar, y);
  gtk_adjustment_set_value(table->h_scrollbar, x);
  return 0;
}

static void dt_thumbtable_scroll_to_rowid(dt_thumbtable_t *table, int rowid)
{
  // Find (x, y) of the current thumbnail (north-west corner)
  int x = 0, y = 0;
  _rowid_to_position(table, rowid, &x, &y);

  // Put the image always in the center of the view, if possible,
  // aka move from north-west corner to center of the thumb
  x += table->thumb_width / 2;
  y += table->thumb_height / 2;

  // Scroll viewport there
  const double x_scroll = (double)x - (double)table->view_width / 2.;
  const double y_scroll = (double)y - (double)table->view_height / 2.;
  dt_thumbtable_scroll_to_position(table, x_scroll, y_scroll);
}

static int _find_rowid_from_imgid(dt_thumbtable_t *table, const int32_t imgid)
{
  for(int i = 0; i < table->collection_count; i++)
    if(table->lut[i].imgid == imgid)
      return i;

  return UNKNOWN_IMAGE;
}

int dt_thumbtable_scroll_to_imgid(dt_thumbtable_t *table, int32_t imgid)
{
  if(!table->collection_inited) return 1;
  int rowid = UNKNOWN_IMAGE;
  if(imgid > UNKNOWN_IMAGE)
  {
    dt_pthread_mutex_lock(&table->lock);
    rowid = _find_rowid_from_imgid(table, imgid);
    dt_pthread_mutex_unlock(&table->lock);
  }
  else
    rowid = table->rowid;

  if(rowid == UNKNOWN_IMAGE) return 1;

  dt_thumbtable_scroll_to_rowid(table, rowid);

  return 0;
}


int dt_thumbtable_scroll_to_active_rowid(dt_thumbtable_t *table)
{
  if(table->rowid > UNKNOWN_IMAGE)
    dt_thumbtable_scroll_to_rowid(table, table->rowid);
  else
    dt_thumbtable_scroll_to_selection(table);
  return 0;
}


int dt_thumbtable_scroll_to_selection(dt_thumbtable_t *table)
{
  int id = dt_selection_get_first_id(darktable.selection);
  if(id < 0) id = dt_control_get_keyboard_over_id();
  if(id < 0) id = dt_control_get_mouse_over_id();
  //fprintf(stdout, "scrolling to %i\n", id);
  dt_thumbtable_scroll_to_imgid(table, id);
  return 0;
}

// Find the row ids (as in SQLite indices) of the images contained within viewport at current scrolling stage
static gboolean _get_row_ids(dt_thumbtable_t *table, int *rowid_min, int *rowid_max)
{
  if(!table->configured || !table->v_scrollbar || !table->h_scrollbar) return FALSE;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    // Pixel coordinates of the viewport:
    float page_size = gtk_adjustment_get_page_size(table->v_scrollbar);
    float position = gtk_adjustment_get_value(table->v_scrollbar);

    // what is currently visible lies between position and position + page_size.
    // don't preload next/previous rows because, when in 1 thumb/column,
    // that can be quite slow
    int row_min = floorf(position / (float)table->thumb_height);
    int row_max = ceilf((position + page_size) / (float)table->thumb_height);

    // rowid is the positional ID of the image in the SQLite collection, indexed from 0.
    // SQLite indexes from 1 but then be use our own array to cache results.
    *rowid_min = row_min * table->thumbs_per_row;
    *rowid_max = row_max * table->thumbs_per_row;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    float page_size = gtk_adjustment_get_page_size(table->h_scrollbar);
    float position = gtk_adjustment_get_value(table->h_scrollbar);

    // Preload the previous and next pages too because thumbnails are typically small
    int row_min = (position - page_size) / table->thumb_width;
    int row_max = (position + 2.f * page_size) / table->thumb_width;

    *rowid_min = row_min * table->thumbs_per_row;
    *rowid_max = row_max * table->thumbs_per_row;
  }

  return TRUE;
}

// Find out if a given row id is visible at current scroll step
gboolean _is_rowid_visible(dt_thumbtable_t *table, int rowid)
{
  if(!table->configured || !table->v_scrollbar || !table->h_scrollbar) return FALSE;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {    // Pixel coordinates of the viewport:
    int page_size = gtk_adjustment_get_page_size(table->v_scrollbar);
    int position = gtk_adjustment_get_value(table->v_scrollbar);
    int page_bottom = page_size + position;

    int img_top = (rowid / table->thumbs_per_row) * table->thumb_height;
    int img_bottom = img_top + table->thumb_height;
    return img_top >= position && img_bottom <= page_bottom;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    int page_size = gtk_adjustment_get_page_size(table->h_scrollbar);
    int position = gtk_adjustment_get_value(table->h_scrollbar);
    int page_right = page_size + position;

    int img_left = rowid * table->thumb_height;
    int img_right = img_left + table->thumb_width;
    return img_left >= position && img_right <= page_right;
  }

  return FALSE;
}

// Returns TRUE if visible row ids have changed since last check
gboolean _update_row_ids(dt_thumbtable_t *table)
{
  int rowid_min = 0;
  int rowid_max = 0;
  _get_row_ids(table, &rowid_min, &rowid_max);
  if(rowid_min != table->min_row_id || rowid_max != table->max_row_id)
  {
    table->min_row_id = rowid_min;
    table->max_row_id = rowid_max;
    table->thumbs_inited = FALSE;
    return TRUE;
  }
  return FALSE;
}

void _update_grid_area(dt_thumbtable_t *table)
{
  if(!table->configured || !table->collection_inited) return;

  double main_dimension = 0.;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    double height = main_dimension = ceilf((float)table->collection_count / (float)table->thumbs_per_row) * table->thumb_height;
    gtk_widget_set_size_request(table->grid, -1, height);
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    double width = main_dimension = table->collection_count * table->thumb_height;
    gtk_widget_set_size_request(table->grid, width, -1);
  }
  else
    gtk_widget_set_size_request(table->grid, -1, -1);

  dt_print(DT_DEBUG_LIGHTTABLE, "Configuring grid size main dimension: %.f\n", main_dimension);
}


void _grid_configure(dt_thumbtable_t *table, int width, int height, int cols)
{
  if(width < 32 || height < 32) return;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    table->thumbs_per_row = cols;
    table->view_width = width;
    table->view_height = height;
    table->thumb_width = (int)floorf((float)width / (float)table->thumbs_per_row);
    table->thumb_height = (table->thumbs_per_row == 1) ? height : table->thumb_width;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    table->thumbs_per_row = 1;
    table->view_width = width;
    table->view_height = height;
    table->thumb_height = height;
    table->thumb_width = height;
  }

  table->configured = TRUE;

  dt_print(DT_DEBUG_LIGHTTABLE, "Configuring thumbtable w=%i h=%i thumbs/row=%i thumb_width=%i\n",
           table->view_width, table->view_height, table->thumbs_per_row, table->thumb_width);
}

// Track size changes of the container or number of thumbs per row
// and recomputed the size of individual thumbnails accordingly
void dt_thumbtable_configure(dt_thumbtable_t *table)
{
  if(!gtk_widget_is_visible(table->scroll_window)) return;

  int cols = 1;
  int new_width = 0;
  int new_height = 0;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    new_width = gtk_widget_get_allocated_width(table->parent_overlay);
    new_height = gtk_widget_get_allocated_height(table->parent_overlay);
    GtkWidget *v_scroll = gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(table->scroll_window));
    new_width -= gtk_widget_get_allocated_width(v_scroll) + 1;
    cols = dt_conf_get_int("plugins/lighttable/images_in_row");
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    new_width = gtk_widget_get_allocated_width(table->parent_overlay);
    new_height = gtk_widget_get_allocated_height(table->parent_overlay);
    GtkWidget *h_scroll = gtk_scrolled_window_get_hscrollbar(GTK_SCROLLED_WINDOW(table->scroll_window));
    new_height -= gtk_widget_get_allocated_height(h_scroll) + 1;
  }

  if((new_width > 0 && new_width != table->view_width) ||
     (new_height > 0 && new_height != table->view_height) ||
     (cols != table->thumbs_per_row))
  {
    // new sizes: update everything
    table->thumbs_inited = FALSE;
    _grid_configure(table, new_width, new_height, cols);
    _update_grid_area(table);
  }
  else if(new_width < 32 || new_height < 32)
  {
    // Parent is not allocated or something went wrong:
    // ensure to reset everything so no further code will run
    table->thumbs_inited = FALSE;
    table->configured = FALSE;
    table->thumbs_per_row = 0;
    table->thumb_height = 0;
    table->thumb_width = 0;
  }
}

dt_thumbnail_t *_find_thumb_by_imgid(dt_thumbtable_t *table, const int32_t imgid)
{
  for(GList *item = g_list_first(table->list); item; item = g_list_next(item))
  {
    dt_thumbnail_t *th = (dt_thumbnail_t *)item->data;
    if(th->imgid == imgid)
      return th;
  }

  return NULL;
}

gboolean dt_thumbtable_get_thumbnail_info(dt_thumbtable_t *table, int32_t imgid, dt_thumbnail_image_info_t *out)
{
  if(!table || !out || imgid <= 0) return FALSE;

  // Prefer LUT-backed metadata to avoid touching the image cache for read-only UI needs.
  gboolean found = FALSE;
  dt_pthread_mutex_lock(&table->lock);
  if(table->lut)
  {
    for(int rowid = 0; rowid < table->collection_count; rowid++)
    {
      if(table->lut[rowid].imgid == imgid && table->lut[rowid].info_valid)
      {
        *out = table->lut[rowid].info;
        found = TRUE;
        break;
      }
    }
  }
  if(!found)
  {
    dt_thumbnail_t *thumb = _find_thumb_by_imgid(table, imgid);
    if(thumb && thumb->info_valid)
    {
      *out = thumb->info;
      found = TRUE;
    }
  }
  dt_pthread_mutex_unlock(&table->lock);

  return found;
}

#define CLAMP_ROW(rowid) CLAMP(rowid, 0, table->collection_count - 1)
#define IS_COLLECTION_EDGE(rowid) (rowid < 0 || rowid >= table->collection_count)

void _add_thumbnail_group_borders(dt_thumbtable_t *table, dt_thumbnail_t *thumb)
{
  // Reset all CSS classes
  dt_thumbnail_border_t borders = 0;
  dt_thumbnail_set_group_border(thumb, borders);

  const int32_t rowid = thumb->rowid;
  const int32_t groupid = thumb->groupid;

  // Ungrouped image: abort
  if(table->lut[rowid].group_members < 2 || !table->draw_group_borders) return;

  if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    if(table->lut[CLAMP_ROW(rowid - table->thumbs_per_row)].groupid != groupid
      || IS_COLLECTION_EDGE(rowid - table->thumbs_per_row))
      borders |= DT_THUMBNAIL_BORDER_TOP;

    if(table->lut[CLAMP_ROW(rowid + table->thumbs_per_row)].groupid != groupid
      || IS_COLLECTION_EDGE(rowid + table->thumbs_per_row))
      borders |= DT_THUMBNAIL_BORDER_BOTTOM;

    if(table->lut[CLAMP_ROW(rowid - 1)].groupid != groupid
      || IS_COLLECTION_EDGE(rowid - 1))
      borders |= DT_THUMBNAIL_BORDER_LEFT;

    if(table->lut[CLAMP_ROW(rowid + 1)].groupid != groupid
      || IS_COLLECTION_EDGE(rowid + 1))
      borders |= DT_THUMBNAIL_BORDER_RIGHT;

    // If the group spans over more than a full row,
    // close the row ends. Otherwise, we leave orphans opened at the row ends.
    if(table->lut[rowid].group_members > table->thumbs_per_row)
    {
      if(rowid % table->thumbs_per_row == 0)
        borders |= DT_THUMBNAIL_BORDER_LEFT;
      if(rowid % table->thumbs_per_row == table->thumbs_per_row - 1)
        borders |= DT_THUMBNAIL_BORDER_RIGHT;
    }
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    borders |= DT_THUMBNAIL_BORDER_BOTTOM | DT_THUMBNAIL_BORDER_TOP;

    if(table->lut[CLAMP_ROW(rowid - 1)].groupid != groupid
      || IS_COLLECTION_EDGE(rowid - 1))
      borders |= DT_THUMBNAIL_BORDER_LEFT;

    if(table->lut[CLAMP_ROW(rowid + 1)].groupid != groupid
      || IS_COLLECTION_EDGE(rowid + 1))
      borders |= DT_THUMBNAIL_BORDER_RIGHT;
  }

  dt_thumbnail_set_group_border(thumb, borders);
}

void _add_thumbnail_at_rowid(dt_thumbtable_t *table, const size_t rowid, const int32_t mouse_over)
{
  const int32_t imgid = table->lut[rowid].imgid;
  const int32_t groupid = table->lut[rowid].groupid;

  dt_thumbnail_t *thumb = NULL;
  gboolean new_item = TRUE;
  gboolean new_position = TRUE;

  // Do we already have a thumbnail at the correct postion for the correct imgid ?
  if(table->lut[rowid].thumb && table->lut[rowid].thumb->imgid == imgid)
  {
    // YES : reuse it
    thumb = table->lut[rowid].thumb;
    new_position = FALSE;
  }
  else
  {
    // NO : Try to find an existing thumbnail widget by imgid in table->list
    // That will be faster if we only changed the sorting order but are still in the same collection.
    // NOTE: the thumb widget position in grid will be wrong
    thumb = _find_thumb_by_imgid(table, imgid);
  }

  if(thumb)
  {
    // Ensure everything is up-to-date
    thumb->rowid = rowid;
    thumb->groupid = groupid;
    new_item = FALSE;
  }
  else
  {
    thumb = dt_thumbnail_new(imgid, rowid, groupid, table->overlays, table);
    table->list = g_list_prepend(table->list, thumb);
    table->thumb_nb += 1;
  }

  table->lut[rowid].thumb = thumb;

  if(!thumb) return; // not sure why that would happen

  // Resize
  gboolean size_changed = (table->thumb_height != thumb->height || table->thumb_width != thumb->width);
  if(new_item || size_changed || table->overlays != thumb->over)
  {
    dt_thumbnail_set_overlay(thumb, table->overlays);
    dt_thumbnail_resize(thumb, table->thumb_width, table->thumb_height);
  }

  // Actually moving the widgets in the grid is more expensive, do it only if necessary
  if(new_item)
  {
    _set_thumb_position(table, thumb);
    gtk_fixed_put(GTK_FIXED(table->grid), thumb->widget, thumb->x, thumb->y);
    //fprintf(stdout, "adding new thumb at #%lu: %i, %i\n", rowid, thumb->x, thumb->y);
  }
  else if(new_position || size_changed)
  {
    _set_thumb_position(table, thumb);
    gtk_fixed_move(GTK_FIXED(table->grid), thumb->widget, thumb->x, thumb->y);
    //fprintf(stdout, "moving new thumb at #%lu: %i, %i\n", rowid, thumb->x, thumb->y);
  }

  // Update visual states and flags. Mouse over is not connected to a signal and cheap to update
  dt_thumbnail_set_mouseover(thumb, (mouse_over == thumb->imgid));
  dt_thumbnail_alternative_mode(thumb, table->alternate_mode);

  if(table->mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    dt_thumbnail_update_selection(thumb, dt_view_active_images_has_imgid(thumb->imgid));
    thumb->disable_actions = TRUE;
  }
  else if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    dt_thumbnail_update_selection(thumb, dt_selection_is_id_selected(darktable.selection, thumb->imgid));
    thumb->disable_actions = FALSE;
  }

  _add_thumbnail_group_borders(table, thumb);
  gtk_widget_show(thumb->widget);
}


// Add and/or resize thumbnails within visible viewort at current scroll level
void _populate_thumbnails(dt_thumbtable_t *table)
{
  const int32_t mouse_over = dt_control_get_mouse_over_id();

  // for(size_t rowid = 0; rowid < table->collection_count; rowid++)
  for(size_t rowid = MAX(table->min_row_id, 0); rowid < MIN(table->max_row_id, table->collection_count); rowid++)
    _add_thumbnail_at_rowid(table, rowid, mouse_over);
}

// Resize the thumbnails that are still existing but outside of visible viewport at current scroll level
void _resize_thumbnails(dt_thumbtable_t *table)
{
  if(!table->configured) return;

  for(GList *link = g_list_first(table->list); link; link = g_list_next(link))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)link->data;
    gboolean size_changed = (table->thumb_height != thumb->height || table->thumb_width != thumb->width);

    if(size_changed || table->overlays != thumb->over)
    {
      // Overlay modes may change the height of the image
      // to accommodate buttons. We need to resize on overlay changes.
      dt_thumbnail_set_overlay(thumb, table->overlays);
      dt_thumbnail_resize(thumb, table->thumb_width, table->thumb_height);
      if(size_changed)
      {
        _set_thumb_position(table, thumb);
        gtk_fixed_move(GTK_FIXED(table->grid), thumb->widget, thumb->x, thumb->y);
      }
      dt_thumbnail_alternative_mode(thumb, table->alternate_mode);
    }

    dt_thumbnail_update_partial_infos(thumb);
    _add_thumbnail_group_borders(table, thumb);
    gtk_widget_queue_draw(thumb->widget);
  }
}


void dt_thumbtable_update(dt_thumbtable_t *table)
{
  _update_row_ids(table);

  if(!gtk_widget_is_visible(table->scroll_window) || !table->lut || !table->configured || !table->collection_inited
     || table->thumbs_inited || table->collection_count == 0)
    return;

  if(table->reset_collection)
  {
    _dt_thumbtable_empty_list(table);
    table->reset_collection = FALSE;
  }

  const double start = dt_get_wtime();

  dt_pthread_mutex_lock(&table->lock);

  gboolean empty_list = (table->list == NULL);

  _populate_thumbnails(table);

  if(!empty_list && table->list)
    _resize_thumbnails(table);

  table->thumbs_inited = TRUE;

  dt_pthread_mutex_unlock(&table->lock);

  dt_print(DT_DEBUG_LIGHTTABLE, "Populated %d thumbs between %i and %i in %0.04f sec \n", table->thumb_nb,
           table->min_row_id, table->max_row_id, dt_get_wtime() - start);
}


static void _dt_profile_change_callback(gpointer instance, int type, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  dt_thumbtable_refresh_thumbnail(table, UNKNOWN_IMAGE, TRUE);
}

static void _dt_selection_changed_callback(gpointer instance, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  gboolean first = TRUE;

  dt_pthread_mutex_lock(&table->lock);
  for(const GList *l = g_list_first(table->list); l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    const gboolean selected = thumb->selected;
    dt_thumbnail_update_selection(thumb, dt_selection_is_id_selected(darktable.selection, thumb->imgid));

    if(thumb->selected && first)
    {
      dt_view_image_info_update(thumb->imgid);

      // Sync the table active row id with the first thumb in selection
      table->rowid = thumb->rowid;
      first = FALSE;
    }

    if(thumb->selected != selected)
      gtk_widget_queue_draw(thumb->widget);
  }
  dt_pthread_mutex_unlock(&table->lock);
}

void dt_thumbtable_set_zoom(dt_thumbtable_t *table, dt_thumbtable_zoom_t level)
{
  table->zoom = level;
  dt_thumbtable_set_active_rowid(table);
  dt_thumbtable_refresh_thumbnail(table, UNKNOWN_IMAGE, TRUE);
  g_idle_add((GSourceFunc)_grab_focus, table);
}

dt_thumbtable_zoom_t dt_thumbtable_get_zoom(dt_thumbtable_t *table)
{
  return table->zoom;
}

void dt_thumbtable_offset_zoom(dt_thumbtable_t *table, const double delta_x, const double delta_y)
{
  dt_pthread_mutex_lock(&table->lock);
  for(const GList *l = g_list_first(table->list); l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    thumb->zoomx += delta_x;
    thumb->zoomy += delta_y;
    gtk_widget_queue_draw(thumb->w_image);
  }
  dt_pthread_mutex_unlock(&table->lock);
}


void dt_thumbtable_set_focus_regions(dt_thumbtable_t *table, gboolean enable)
{
  table->focus_regions = enable;
  dt_thumbtable_refresh_thumbnail(table, UNKNOWN_IMAGE, TRUE);
}

gboolean dt_thumbtable_get_focus_regions(dt_thumbtable_t *table)
{
  return table->focus_regions;
}

void dt_thumbtable_set_focus_peaking(dt_thumbtable_t *table, gboolean enable)
{
  table->focus_peaking = enable;
  dt_thumbtable_refresh_thumbnail(table, UNKNOWN_IMAGE, TRUE);
}

gboolean dt_thumbtable_get_focus_peaking(dt_thumbtable_t *table)
{
  return table->focus_peaking;
}

void dt_thumbtable_set_draw_group_borders(dt_thumbtable_t *table, gboolean enable)
{
  table->draw_group_borders = enable;
  dt_pthread_mutex_lock(&table->lock);
  _resize_thumbnails(table);
  dt_pthread_mutex_unlock(&table->lock);
}

gboolean dt_thumbtable_get_draw_group_borders(dt_thumbtable_t *table)
{
  return table->draw_group_borders;
}

// can be called with imgid = -1, in that case we reload all mipmaps
// reinit = FALSE should be called when the mipmap is ready to redraw,
// reinit = TRUE should be called when a refreshed mipmap has been requested but we have nothing yet to draw
void dt_thumbtable_refresh_thumbnail_real(dt_thumbtable_t *table, int32_t imgid, gboolean reinit)
{
  dt_pthread_mutex_lock(&table->lock);
  for(GList *l = g_list_first(table->list); l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    if(thumb->imgid == imgid)
    {
      dt_thumbnail_image_refresh(thumb);
      break;
    }
    else if(imgid == UNKNOWN_IMAGE)
    {
      dt_thumbnail_image_refresh(thumb);
    }
  }
  dt_pthread_mutex_unlock(&table->lock);
}


// this is called each time the images info change
static void _dt_image_info_changed_callback(gpointer instance, gpointer imgs, gpointer user_data)
{
  if(!user_data || !imgs) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  // Batch-fetch lightweight update data to avoid per-image SQL from dt_image_altered().
  GArray *imgids = g_array_new(FALSE, FALSE, sizeof(int32_t));
  for(GList *l = g_list_first(imgs); l; l = g_list_next(l))
  {
    const int32_t imgid_to_update = GPOINTER_TO_INT(l->data);
    g_array_append_val(imgids, imgid_to_update);
  }

  typedef struct dt_thumbtable_update_info_t
  {
    int32_t groupid;
    uint32_t history_items;
  } dt_thumbtable_update_info_t;

  GHashTable *updates = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
  if(imgids->len > 0)
  {
    GString *placeholders = g_string_new("");
    for(guint i = 0; i < imgids->len; i++)
    {
      if(i) g_string_append(placeholders, ",");
      g_string_append(placeholders, "?");
    }

    gchar *query = g_strdup_printf(
        "SELECT im.id, im.group_id, COALESCE(h.cnt, 0) "
        "FROM main.images AS im "
        "LEFT JOIN (SELECT imgid, COUNT(imgid) AS cnt FROM main.history WHERE imgid IN (%s) GROUP BY imgid) AS h "
        "ON h.imgid = im.id "
        "WHERE im.id IN (%s)",
        placeholders->str, placeholders->str);
    g_string_free(placeholders, TRUE);

    sqlite3_stmt *stmt = NULL;
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1, &stmt, NULL);
    for(guint i = 0; i < imgids->len; i++)
      DT_DEBUG_SQLITE3_BIND_INT(stmt, (int)i + 1, g_array_index(imgids, int32_t, i));
    for(guint i = 0; i < imgids->len; i++)
      DT_DEBUG_SQLITE3_BIND_INT(stmt, (int)i + 1 + (int)imgids->len, g_array_index(imgids, int32_t, i));

    while(sqlite3_step(stmt) == SQLITE_ROW)
    {
      const int32_t imgid = sqlite3_column_int(stmt, 0);
      dt_thumbtable_update_info_t *info = g_new0(dt_thumbtable_update_info_t, 1);
      info->groupid = sqlite3_column_int(stmt, 1);
      info->history_items = sqlite3_column_int(stmt, 2);
      g_hash_table_insert(updates, GINT_TO_POINTER(imgid), info);
    }

    sqlite3_finalize(stmt);
    g_free(query);
  }

  dt_pthread_mutex_lock(&table->lock);

  // Update the LUT infos ahead, because group borders need their neighbours inited first
  for(int rowid = 0; rowid < table->collection_count; rowid++)
  {
    for(GList *l = g_list_first(imgs); l; l = g_list_next(l))
    {
      const int32_t imgid_to_update = GPOINTER_TO_INT(l->data);
      if(table->lut[rowid].imgid == imgid_to_update)
      {
        dt_thumbtable_update_info_t *info = g_hash_table_lookup(updates, GINT_TO_POINTER(imgid_to_update));
        if(info)
        {
          // Update infos reads the content of the LUT, for performance at init time,
          // but then we need to keep it updated during the lifetime of the thumbnail.
          table->lut[rowid].history_items = info->history_items;

          // This is overkill in grouping/ungrouping since grouping operations necessarily refresh the collection
          // and we already refresh the image group as part of collection_changed signal handler.
          // Needed when changing image representative though.
          table->lut[rowid].groupid = info->groupid;
        }

        // Refresh the cached LUT info from the image cache for write-driven updates
        // (ratings, color labels, etc.) while still keeping read paths cache-free.
        const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid_to_update, 'r');
        if(img)
        {
          _thumbtable_info_from_image(&table->lut[rowid].info, img,
                                      table->lut[rowid].history_items, table->lut[rowid].group_members);
          table->lut[rowid].info_valid = TRUE;
          dt_image_cache_read_release(darktable.image_cache, img);
        }
        break;
      }
    }
  }

  for(GList *i = g_list_first(imgs); i; i = g_list_next(i))
  {
    const int32_t imgid_to_update = GPOINTER_TO_INT(i->data);
    for(GList *l = g_list_first(table->list); l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      if(thumb->imgid == imgid_to_update)
      {
        dt_thumbnail_update_infos(thumb);
        _add_thumbnail_group_borders(table, thumb);
        gtk_widget_queue_draw(thumb->widget);

        if(darktable.view_manager->image_info_id == imgid_to_update)
          dt_view_image_info_update(imgid_to_update);

        break;
      }
    }
  }
  dt_pthread_mutex_unlock(&table->lock);

  g_hash_table_destroy(updates);
  g_array_free(imgids, TRUE);
}

static void _dt_collection_lut(dt_thumbtable_t *table)
{
  if(table->lut) free(table->lut);
  table->lut = NULL;

  // In-memory collected images don't store group_id, so we need to fetch it again from DB
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
    // Batch-fetch thumbnail metadata in one SQL query to avoid one query per image
    // through the image cache. This keeps scrolling lightweight and predictable.
    "SELECT im.id, im.group_id, c.rowid, "
    "(SELECT COUNT(id) FROM main.images WHERE group_id=im.group_id), "
    "(SELECT COUNT(imgid) FROM main.history WHERE imgid=c.imgid), "
    "im.film_id, im.version, im.width, im.height, im.orientation, "
    "im.flags, "
    "im.import_timestamp, im.change_timestamp, im.export_timestamp, im.print_timestamp, "
    "im.exposure, im.exposure_bias, im.aperture, im.iso, im.focal_length, im.focus_distance, "
    "im.datetime_taken, "
    "im.longitude, im.latitude, im.altitude, "
    "im.filename, im.maker, im.model, im.lens, fr.folder, "
    "COALESCE((SELECT SUM(1 << color) FROM main.color_labels WHERE imgid=im.id), 0) "
    "FROM main.images AS im "
    "JOIN memory.collected_images AS c ON im.id = c.imgid "
    "LEFT JOIN main.film_rolls AS fr ON fr.id = im.film_id "
    "ORDER BY c.rowid ASC",
    -1, &stmt, NULL);

  // NOTE: non-grouped images have group_id equal to their own id
  // grouped images have group_id equal to the id of the "group leader".
  // In old database versions, it's possible that group_id may have been set to -1 for non-grouped images.
  // That would actually make group detection much easier...

  // Convert SQL imgids into C objects we can work with
  GArray *collection = g_array_new(FALSE, FALSE, sizeof(dt_thumbtable_cache_t));
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int32_t imgid = sqlite3_column_int(stmt, 0);
    int32_t groupid = sqlite3_column_int(stmt, 1);
    int32_t group_items = sqlite3_column_int(stmt, 3);
    int32_t history_items = sqlite3_column_int(stmt, 4);

    if(table->collapse_groups && imgid != groupid)
    {
      // if user requested to collapse image groups in GUI,
      // only the group leader is shown. But we need to make sure
      // there is no dangling selection pointing to hidden group members
      // because it's unexpected that unvisible items might be selected,
      // and selection sanitization only deals with imgids outside of current collection,
      // but group members are always within the collection.
      dt_selection_deselect(darktable.selection, imgid);
      continue;
    }

    dt_thumbtable_cache_t entry = {0};
    entry.imgid = imgid;
    entry.groupid = groupid;
    entry.group_members = group_items;
    entry.history_items = history_items;
    entry.thumb = NULL;

    dt_thumbnail_image_info_t *info = &entry.info;
    info->imgid = imgid;
    info->film_id = sqlite3_column_int(stmt, 5);
    info->groupid = groupid;
    info->version = sqlite3_column_int(stmt, 6);
    info->width = sqlite3_column_int(stmt, 7);
    info->height = sqlite3_column_int(stmt, 8);
    info->orientation = sqlite3_column_int(stmt, 9);
    info->p_width = 0;
    info->p_height = 0;
    info->flags = sqlite3_column_int(stmt, 10);
    info->loader = LOADER_UNKNOWN;
    info->import_timestamp = sqlite3_column_int64(stmt, 11);
    info->change_timestamp = sqlite3_column_int64(stmt, 12);
    info->export_timestamp = sqlite3_column_int64(stmt, 13);
    info->print_timestamp = sqlite3_column_int64(stmt, 14);
    info->exif_exposure = sqlite3_column_double(stmt, 15);
    if(sqlite3_column_type(stmt, 16) == SQLITE_FLOAT)
      info->exif_exposure_bias = sqlite3_column_double(stmt, 16);
    else
      info->exif_exposure_bias = NAN;
    info->exif_aperture = sqlite3_column_double(stmt, 17);
    info->exif_iso = sqlite3_column_double(stmt, 18);
    info->exif_focal_length = sqlite3_column_double(stmt, 19);
    info->exif_focus_distance = sqlite3_column_double(stmt, 20);
    info->exif_datetime_taken = sqlite3_column_int64(stmt, 21);
    if(sqlite3_column_type(stmt, 22) == SQLITE_FLOAT)
      info->geoloc_longitude = sqlite3_column_double(stmt, 22);
    else
      info->geoloc_longitude = NAN;
    if(sqlite3_column_type(stmt, 23) == SQLITE_FLOAT)
      info->geoloc_latitude = sqlite3_column_double(stmt, 23);
    else
      info->geoloc_latitude = NAN;
    if(sqlite3_column_type(stmt, 24) == SQLITE_FLOAT)
      info->geoloc_elevation = sqlite3_column_double(stmt, 24);
    else
      info->geoloc_elevation = NAN;

    const char *filename = (const char *)sqlite3_column_text(stmt, 25);
    if(filename) g_strlcpy(info->filename, filename, sizeof(info->filename));
    const char *maker = (const char *)sqlite3_column_text(stmt, 26);
    if(maker) g_strlcpy(info->exif_maker, maker, sizeof(info->exif_maker));
    const char *model = (const char *)sqlite3_column_text(stmt, 27);
    if(model) g_strlcpy(info->exif_model, model, sizeof(info->exif_model));
    const char *lens = (const char *)sqlite3_column_text(stmt, 28);
    if(lens) g_strlcpy(info->exif_lens, lens, sizeof(info->exif_lens));
    const char *folder = (const char *)sqlite3_column_text(stmt, 29);
    if(folder) g_strlcpy(info->folder, folder, sizeof(info->folder));

    info->colorlabels = sqlite3_column_int(stmt, 30);
    info->is_grouped = (group_items > 1);
    info->is_altered = (history_items > 0);
    _thumbtable_info_finalize(info);
#ifndef NDEBUG
    _thumbtable_debug_assert_info_matches_cache(info, history_items, group_items);
#endif
    entry.info_valid = TRUE;

    g_array_append_val(collection, entry);
  }
  sqlite3_finalize(stmt);

  if(!collection || collection->len == 0)
  {
    table->collection_count = 0;
    if(collection) g_array_free(collection, TRUE);
    return;
  }

  table->collection_count = collection->len;

  dt_pthread_mutex_lock(&table->lock);

  // Build the collection LUT, aka a fixed-sized array of image objects
  // where the position of an image in the collection is directly the index in the LUT/array.
  // This makes for very efficient position -> imgid/thumbnail accesses directly in C,
  // especially from GUI code. The downside is we need to fully clear and recreate the LUT
  // everytime a collection changes (meaning filters OR sorting changed).
  table->lut = malloc(table->collection_count * sizeof(dt_thumbtable_cache_t));

  if(!table->lut)
  {
    g_array_free(collection, TRUE);
    dt_pthread_mutex_unlock(&table->lock);
    return;
  }

  memcpy(table->lut, collection->data, collection->len * sizeof(dt_thumbtable_cache_t));

  table->collection_inited = TRUE;

  dt_pthread_mutex_unlock(&table->lock);

  g_array_free(collection, TRUE);
}

static gboolean _dt_collection_get_hash(dt_thumbtable_t *table)
{
  // Hash the collection query string
  const char *const query = dt_collection_get_query(darktable.collection);
  size_t len = strlen(query);
  uint64_t hash = dt_hash(5384, query, len);

  // Factor in the number of images in the collection result
  uint32_t num_pics = dt_collection_get_count(darktable.collection);
  hash = dt_hash(hash, (char *)&num_pics, sizeof(uint32_t));

  if(hash != table->collection_hash || table->reset_collection)
  {
    // Collection changed: reset everything
    table->collection_hash = hash;
    table->collection_count = num_pics;
    table->collection_inited = FALSE;
    return TRUE;
  }
  return FALSE;
}


// this is called each time collected images change
static void _dt_collection_changed_callback(gpointer instance, dt_collection_change_t query_change,
                                            dt_collection_properties_t changed_property, gpointer imgs,
                                            const int next, gpointer user_data)
{
  if(!user_data) return;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  gboolean collapse_groups = dt_conf_get_bool("ui_last/grouping");
  gboolean collapsing_changed = (table->collapse_groups != collapse_groups);

  // Remember where the scrolling is at to possibly get the same visible images
  // despite collection changes (provided they are still there).
  dt_thumbtable_set_active_rowid(table);

  // See if the collection changed
  gboolean grouping_changed = changed_property == DT_COLLECTION_PROP_GROUPING;
  gboolean changed = _dt_collection_get_hash(table) || collapsing_changed || grouping_changed;
  if(changed)
  {
    // If groups are collapsed, we add only the group leader image to the collection
    // It needs to be set before running _dt_collection_lut()
    table->collapse_groups = collapse_groups;
    _dt_collection_lut(table);

    table->thumbs_inited = FALSE;
    _dt_thumbtable_empty_list(table);

    if(table->collection_count == 0)
    {
      dt_control_log(_(
          "The current filtered collection contains no image. Relax your filters or fetch a non-empty collection"));
    }

    // Ensure we have something to scroll
    dt_thumbtable_configure(table);

    // Number of images may have changed, size of grid too:
    _update_grid_area(table);

    // Update without waiting for the next redraw event,
    // improves subjective responsiveness and makes auto-scrolling to
    // selected image more reliable.
    dt_thumbtable_update(table);

    g_idle_add((GSourceFunc)_grab_focus, table);
  }
}

// get the class name associated with the overlays mode
static gchar *_thumbs_get_overlays_class(dt_thumbnail_overlay_t over)
{
  switch(over)
  {
    case DT_THUMBNAIL_OVERLAYS_NONE:
      return g_strdup("dt_overlays_none");
    case DT_THUMBNAIL_OVERLAYS_ALWAYS_NORMAL:
      return g_strdup("dt_overlays_always");
    case DT_THUMBNAIL_OVERLAYS_HOVER_NORMAL:
    default:
      return g_strdup("dt_overlays_hover");
  }
}

// update thumbtable class and overlays mode, depending on size category
static void _thumbs_update_overlays_mode(dt_thumbtable_t *table)
{
  // we change the overlay mode
  gchar *txt = g_strdup("plugins/lighttable/overlays/global");
  dt_thumbnail_overlay_t over = sanitize_overlays(dt_conf_get_int(txt));
  g_free(txt);

  dt_thumbtable_set_overlays_mode(table, over);
}

// change the type of overlays that should be shown
void dt_thumbtable_set_overlays_mode(dt_thumbtable_t *table, dt_thumbnail_overlay_t over)
{
  if(!table) return;
  if(over == table->overlays) return;

  // Cleanup old Darktable stupid modes
  dt_conf_set_int("plugins/lighttable/overlays/global", sanitize_overlays(over));

  gchar *cl0 = _thumbs_get_overlays_class(table->overlays);
  gchar *cl1 = _thumbs_get_overlays_class(over);
  dt_gui_remove_class(table->grid, cl0);
  dt_gui_add_class(table->grid, cl1);
  g_free(cl0);
  g_free(cl1);

  table->thumbs_inited = FALSE;
  table->overlays = over;

  dt_pthread_mutex_lock(&table->lock);
  _resize_thumbnails(table);
  dt_pthread_mutex_unlock(&table->lock);
}

static void _event_dnd_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection_data,
                           const guint target_type, const guint time, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  g_assert(selection_data != NULL);

  switch(target_type)
  {
    case DND_TARGET_IMGID:
    {
      const int imgs_nb = g_list_length(table->drag_list);
      if(imgs_nb)
      {
        uint32_t *imgs = malloc(sizeof(uint32_t) * imgs_nb);
        GList *l = table->drag_list;
        for(int i = 0; i < imgs_nb; i++)
        {
          imgs[i] = GPOINTER_TO_INT(l->data);
          l = g_list_next(l);
        }
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                               _DWORD, (guchar *)imgs, imgs_nb * sizeof(uint32_t));
        free(imgs);
      }
      break;
    }
    default: // return the location of the file as a last resort
    case DND_TARGET_URI:
    {
      GList *l = table->drag_list;
      if(g_list_is_singleton(l))
      {
        gchar pathname[PATH_MAX] = { 0 };
        gboolean from_cache = TRUE;
        const int id = GPOINTER_TO_INT(l->data);
        dt_image_full_path(id,  pathname,  sizeof(pathname),  &from_cache, __FUNCTION__);
        gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data),
                               _BYTE, (guchar *)uri, strlen(uri));
        g_free(uri);
      }
      else
      {
        GList *images = NULL;
        for(; l; l = g_list_next(l))
        {
          const int id = GPOINTER_TO_INT(l->data);
          gchar pathname[PATH_MAX] = { 0 };
          gboolean from_cache = TRUE;
          dt_image_full_path(id,  pathname,  sizeof(pathname),  &from_cache, __FUNCTION__);
          gchar *uri = g_strdup_printf("file://%s", pathname); // TODO: should we add the host?
          images = g_list_prepend(images, uri);
        }
        images = g_list_reverse(images); // list was built in reverse order, so un-reverse it
        gchar *uri_list = dt_util_glist_to_str("\r\n", images);
        g_list_free_full(images, g_free);
        gtk_selection_data_set(selection_data, gtk_selection_data_get_target(selection_data), _BYTE,
                               (guchar *)uri_list, strlen(uri_list));
        g_free(uri_list);
      }
      break;
    }
  }
}

static void _event_dnd_begin(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  table->drag_list = dt_act_on_get_images();

#ifdef HAVE_MAP
  dt_view_manager_t *vm = darktable.view_manager;
  dt_view_t *view = vm->current_view;
  if(!strcmp(view->module_name, "map"))
  {
    if(table->drag_list)
      dt_view_map_drag_set_icon(darktable.view_manager, context,
                                GPOINTER_TO_INT(table->drag_list->data),
                                g_list_length(table->drag_list));
  }
#endif
}

GList *_thumbtable_dnd_import_check(GList *files, const char *pathname, int *elements)
{
  if (!pathname || pathname == NULL)
  {
    fprintf(stdout,"DND check: no pathname.\n");
    return files;
  }
  fprintf(stdout,"DND check pathname: %s\n", pathname);

  if(g_file_test(pathname, G_FILE_TEST_IS_REGULAR))
  {
    if (dt_supported_image(pathname))
    {
      files = g_list_prepend(files, g_strdup(pathname));
      (*elements)++;
    }
    else
      fprintf(stderr, "`%s`: Unkonwn format.", pathname);
  }
  else if(g_file_test(pathname, G_FILE_TEST_IS_DIR))
  {
    fprintf(stderr, "DND check: Folders are not allowed");
    dt_control_log(_("'%s': Please use 'File > Import' to import a folder."), pathname);
  }
  else
  {
    fprintf(stderr, "DND check: `%s` not a file or folder.\n", pathname);
  }

  return files;
}

static gboolean _thumbtable_dnd_import(GtkSelectionData *selection_data)
{
  gchar **uris = gtk_selection_data_get_uris(selection_data);
  int elements = 0;
  GList *files = NULL;

  if(uris)
  {
    GVfs *vfs = g_vfs_get_default();
    for (int i = 0; uris[i] != NULL; i++)
    {
      GFile *filepath = g_vfs_get_file_for_uri(vfs, uris[i]);
      const gchar *pathname = g_strdup(g_file_get_path(filepath));
      files = _thumbtable_dnd_import_check(files, pathname, &elements);
      g_object_unref(filepath);
    }

    if(elements > 0)
    {
      // WARNING: we copy a Glist of pathes as char*
      // The references to the char* still belong to the original.
      // We will free them in the import job.
      dt_control_import_t data = {.imgs = g_list_copy(files),
                                  .datetime = g_date_time_new_now_local(),
                                  .copy = FALSE, // we only import in place.
                                  .jobcode = dt_conf_get_string("ui_last/import_jobcode"),
                                  .base_folder = dt_conf_get_string("session/base_directory_pattern"),
                                  .target_subfolder_pattern = dt_conf_get_string("session/sub_directory_pattern"),
                                  .target_file_pattern = dt_conf_get_string("session/filename_pattern"),
                                  .target_dir = NULL,
                                  .elements = elements,
                                  .discarded = NULL
                                  };

      dt_control_import(data);
    }
    else fprintf(stderr,"No files to import. Check your selection or use 'File > Import'.");
  }

  g_strfreev(uris);
  g_list_free(files);
  return elements >= 0 ? TRUE : FALSE;
}

void dt_thumbtable_event_dnd_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                                GtkSelectionData *selection_data, guint target_type, guint time,
                                gpointer user_data)
{
  gboolean success = FALSE;

  if((target_type == DND_TARGET_URI) && (selection_data != NULL)
     && (gtk_selection_data_get_length(selection_data) >= 0))
  {
    success = _thumbtable_dnd_import(selection_data);
  }

  gtk_drag_finish(context, success, FALSE, time);
}

static void _event_dnd_end(GtkWidget *widget, GdkDragContext *context, gpointer user_data)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(table->drag_list)
  {
    g_list_free(table->drag_list);
    table->drag_list = NULL;
  }
  // in any case, with reset the reordering class if any
  dt_gui_remove_class(table->grid, "dt_thumbtable_reorder");
}

int _imgid_to_rowid(dt_thumbtable_t *table, int32_t imgid)
{
  if(!table->lut) return UNKNOWN_IMAGE;

  int rowid = UNKNOWN_IMAGE;

  dt_pthread_mutex_lock(&table->lock);
  for(int i = 0; i < table->collection_count; i++)
  {
    if(table->lut[i].imgid == imgid)
    {
      rowid = i;
      break;
    }
  }
  dt_pthread_mutex_unlock(&table->lock);

  return rowid;
}

typedef enum dt_thumbtable_direction_t
{
  DT_TT_MOVE_UP,
  DT_TT_MOVE_DOWN,
  DT_TT_MOVE_LEFT,
  DT_TT_MOVE_RIGHT,
  DT_TT_MOVE_PREVIOUS_PAGE,
  DT_TT_MOVE_NEXT_PAGE,
  DT_TT_MOVE_START,
  DT_TT_MOVE_END
} dt_thumbtable_direction_t;


void _move_in_grid(dt_thumbtable_t *table, GdkEventKey *event, dt_thumbtable_direction_t direction, int origin_imgid)
{
  if(!table->lut) return;
  if(!gtk_widget_is_visible(table->scroll_window)) return;

  int current_rowid = _imgid_to_rowid(table, origin_imgid);
  int offset = 0;

  switch(direction)
  {
    case DT_TT_MOVE_UP:
      offset = - table->thumbs_per_row;
      break;
    case DT_TT_MOVE_DOWN:
      offset = + table->thumbs_per_row;
      break;
    case DT_TT_MOVE_LEFT:
      offset = - 1;
      break;
    case DT_TT_MOVE_RIGHT:
      offset = + 1;
      break;
    case DT_TT_MOVE_PREVIOUS_PAGE:
      offset = - table->view_height / table->thumb_height * table->thumbs_per_row;
      break;
    case DT_TT_MOVE_NEXT_PAGE:
      offset = + table->view_height / table->thumb_height * table->thumbs_per_row;
      break;
    case DT_TT_MOVE_START:
      offset = - origin_imgid;
      break;
    case DT_TT_MOVE_END:
      offset = +table->collection_count; // will be clamped below, don't care
      break;
  }

  int new_rowid = CLAMP(current_rowid + offset, 0, table->collection_count - 1);

  dt_pthread_mutex_lock(&table->lock);
  int new_imgid = table->lut[new_rowid].imgid;
  dt_pthread_mutex_unlock(&table->lock);

  dt_thumbtable_dispatch_over(table, event->type, new_imgid);

  if(!_is_rowid_visible(table, new_rowid))
  {
    // GUI update will be handled through the value-changed event of the GtkAdjustment
    dt_thumbtable_scroll_to_imgid(table, new_imgid);
  }
  else
  {
    // We still need to update all visible thumbs to keep mouse_over states in sync
    table->thumbs_inited = FALSE;
    dt_thumbtable_update(table);
  }
}

void _alternative_mode(dt_thumbtable_t *table, gboolean enable)
{
  if(table->alternate_mode == enable) return;
  table->alternate_mode = enable;

  dt_pthread_mutex_lock(&table->lock);
  for(GList *l = g_list_first(table->list); l; l = g_list_next(l))
  {
    dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
    dt_thumbnail_alternative_mode(thumb, enable);
  }
  dt_pthread_mutex_unlock(&table->lock);
}


gboolean dt_thumbtable_key_pressed_grid(GtkWidget *self, GdkEventKey *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;
  if(!user_data) return FALSE;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;
  if(!table->lut) return FALSE;

  // Find out the current image
  // NOTE: when moving into the grid from key arrow events,
  // if the cursor is still overlaying the grid when scrolling, it can collide
  // with key event and set the mouse_over focus elsewhere.
  // For this reason, we use our own private keyboard_over event,
  // and use the mouse_over as a fall-back only.
  // Key events are "knobby", therefore more reliale than "hover",
  // so they always take precedence.
  int32_t imgid = dt_control_get_keyboard_over_id();
  if(imgid < 0) imgid = dt_control_get_mouse_over_id();
  if(imgid < 0) imgid = dt_selection_get_first_id(darktable.selection);
  if(imgid < 0 && table->lut)
  {
    dt_pthread_mutex_lock(&table->lock);
    imgid = table->lut[0].imgid;
    dt_pthread_mutex_unlock(&table->lock);
  }

  //fprintf(stdout, "%s\n", gtk_accelerator_name(event->keyval, event->state));

  // Exit alternative mode on any keystroke other than alt
  if(event->keyval != GDK_KEY_Alt_L && event->keyval != GDK_KEY_Alt_R)
    _alternative_mode(table, FALSE);

  switch(event->keyval)
  {
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
    {
      if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      {
        _move_in_grid(table, event, DT_TT_MOVE_UP, imgid);
        return TRUE;
      }
      break;
    }
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
    {
      if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      {
        _move_in_grid(table, event, DT_TT_MOVE_DOWN, imgid);
        return TRUE;
      }
      break;
    }
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
    {
      _move_in_grid(table, event, DT_TT_MOVE_LEFT, imgid);
      return TRUE;
    }
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
    {
      _move_in_grid(table, event, DT_TT_MOVE_RIGHT, imgid);
      return TRUE;
    }
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
    {
      _move_in_grid(table, event, DT_TT_MOVE_PREVIOUS_PAGE, imgid);
      return TRUE;
    }
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
    {
      _move_in_grid(table, event, DT_TT_MOVE_NEXT_PAGE, imgid);
      return TRUE;
    }
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
    {
      _move_in_grid(table, event, DT_TT_MOVE_START, imgid);
      return TRUE;
    }
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
    {
      _move_in_grid(table, event, DT_TT_MOVE_END, imgid);
      return TRUE;
    }
    case GDK_KEY_space:
    {
      if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      {
        if(dt_modifier_is(event->state, GDK_SHIFT_MASK))
        {
          dt_pthread_mutex_lock(&table->lock);
          int rowid = _find_rowid_from_imgid(table, imgid);
          dt_pthread_mutex_unlock(&table->lock);
          dt_thumbtable_select_range(table, rowid);
        }
        else if(dt_modifier_is(event->state, GDK_CONTROL_MASK))
          dt_selection_toggle(darktable.selection, imgid);
        else
          dt_selection_select_single(darktable.selection, imgid);
        return TRUE;
      }
      break;
    }
    case GDK_KEY_nobreakspace:
    {
      // Shift + space is decoded as nobreakspace on BÉPO keyboards
      if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
      {
        dt_pthread_mutex_lock(&table->lock);
        int rowid = _find_rowid_from_imgid(table, imgid);
        dt_pthread_mutex_unlock(&table->lock);
        dt_thumbtable_select_range(table, rowid);
        return TRUE;
      }
      break;
    }
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    {
      // This is only to be consistent with mouse events:
      // opening to darkroom happens with double click (aka ACTIVATE event),
      // but the first click always select the clicked thumbnail before.
      // So we do the same here, even though it's not required and actually slightly annoying.
      if(table->mode == DT_THUMBTABLE_MODE_FILEMANAGER)
        dt_selection_select_single(darktable.selection, imgid);

      DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE, imgid);
      return TRUE;
    }
    case GDK_KEY_Alt_L:
    case GDK_KEY_Alt_R:
    {
      _alternative_mode(table, TRUE);
      return TRUE;
    }
    case GDK_KEY_Delete:
    {
      dt_control_remove_images();
      return TRUE;
    }
  }
  return FALSE;
}

gboolean dt_thumbtable_key_released_grid(GtkWidget *self, GdkEventKey *event, gpointer user_data)
{
  if(!gtk_window_is_active(GTK_WINDOW(darktable.gui->ui->main_window))) return FALSE;

  if(!user_data) return FALSE;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  //fprintf(stdout, "%s\n", gtk_accelerator_name(event->keyval, event->state));
  _alternative_mode(table, FALSE);
  return FALSE;
}


static gboolean _draw_callback(GtkWidget *widget, cairo_t *cr, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_thumbtable_t *table = (dt_thumbtable_t *)user_data;

  dt_print(DT_DEBUG_LIGHTTABLE, "[lighttable] Redrawing thumbtable container\n");

  // Ensure the background color is painted
  GtkStyleContext *context = gtk_widget_get_style_context(widget);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  gtk_render_background(context, cr, 0, 0, allocation.width, allocation.height);
  gtk_render_frame(context, cr, 0, 0, allocation.width, allocation.height);

  // The draw callback catches all parent resizing events that need an update of the grid layout.
  // Some are already captured upstream, but the rest need to be handled here.
  dt_thumbtable_configure(table);
  dt_thumbtable_update(table);

  return FALSE;
}

void dt_thumbtable_reset_collection(dt_thumbtable_t *table)
{
  table->reset_collection = TRUE;
}

gboolean _event_main_leave(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
  if(!user_data) return TRUE;
  dt_control_set_mouse_over_id(UNKNOWN_IMAGE);
  return TRUE;
}


dt_thumbtable_t *dt_thumbtable_new(dt_thumbtable_mode_t mode)
{
  dt_thumbtable_t *table = (dt_thumbtable_t *)calloc(1, sizeof(dt_thumbtable_t));

  table->scroll_window = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(table->scroll_window), FALSE);
  gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(table->scroll_window), GTK_SHADOW_ETCHED_IN);

  table->v_scrollbar = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(table->scroll_window));
  table->h_scrollbar = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(table->scroll_window));

  table->grid = gtk_fixed_new();
  dt_gui_add_class(table->grid, "dt_thumbtable");
  gtk_container_add(GTK_CONTAINER(table->scroll_window), table->grid);
  gtk_widget_set_can_focus(table->grid, TRUE);
  gtk_widget_set_focus_on_click(table->grid, TRUE);
  gtk_widget_add_events(table->grid, GDK_LEAVE_NOTIFY_MASK);
  gtk_widget_set_app_paintable(table->grid, TRUE);
  g_signal_connect(G_OBJECT(table->grid), "leave-notify-event", G_CALLBACK(_event_main_leave), table);

  // Disable auto re-scrolling to beginning when a child of scrolled window gets the focus
  // Doesn't seem to work...
  // https://stackoverflow.com/questions/26693042/gtkscrolledwindow-disable-scroll-to-focused-child
  GtkAdjustment *dummy = gtk_adjustment_new(0., 0., 1., 1., 1., 1.);
  gtk_container_set_focus_hadjustment(GTK_CONTAINER(table->scroll_window), dummy);
  gtk_container_set_focus_vadjustment(GTK_CONTAINER(table->scroll_window), dummy);
  gtk_container_set_focus_hadjustment(GTK_CONTAINER(table->grid), dummy);
  gtk_container_set_focus_vadjustment(GTK_CONTAINER(table->grid), dummy);

  // drag and drop : used for reordering, interactions with maps, exporting uri to external apps, importing images
  // in filmroll...
  gtk_drag_source_set(table->grid, GDK_BUTTON1_MASK, target_list_all, n_targets_all, GDK_ACTION_MOVE);
  gtk_drag_dest_set(table->grid, GTK_DEST_DEFAULT_ALL, target_list_all, n_targets_all, GDK_ACTION_MOVE);
  g_signal_connect_after(table->grid, "drag-begin", G_CALLBACK(_event_dnd_begin), table);
  g_signal_connect_after(table->grid, "drag-end", G_CALLBACK(_event_dnd_end), table);
  g_signal_connect(table->grid, "drag-data-get", G_CALLBACK(_event_dnd_get), table);
  g_signal_connect(table->grid, "drag-data-received", G_CALLBACK(dt_thumbtable_event_dnd_received), table);

  gtk_widget_add_events(table->grid, GDK_STRUCTURE_MASK | GDK_EXPOSURE_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
  g_signal_connect(table->grid, "draw", G_CALLBACK(_draw_callback), table);
  g_signal_connect(table->grid, "key-press-event", G_CALLBACK(dt_thumbtable_key_pressed_grid), table);
  g_signal_connect(table->grid, "key-release-event", G_CALLBACK(dt_thumbtable_key_released_grid), table);
  gtk_widget_show(table->grid);

  table->thumb_nb = 0;
  table->grid_cols = 0;
  table->collection_inited = FALSE;
  table->configured = FALSE;
  table->thumbs_inited = FALSE;
  table->collection_hash = -1;
  table->list = NULL;
  table->collection_count = 0;
  table->min_row_id = 0;
  table->max_row_id = 0;
  table->lut = NULL;
  table->reset_collection = FALSE;
  table->x_position = 0.;
  table->y_position = 0.;
  table->alternate_mode = FALSE;
  table->rowid = -1;
  table->collapse_groups = dt_conf_get_bool("ui_last/grouping");
  table->draw_group_borders = dt_conf_get_bool("plugins/lighttable/group_borders");

  dt_pthread_mutex_init(&table->lock, NULL);

  dt_gui_add_help_link(table->grid, dt_get_help_url("lighttable_filemanager"));

  // set css name and class
  gtk_widget_set_name(table->grid, "thumbtable-filemanager");

  // overlays mode
  _thumbs_update_overlays_mode(table);

  // we register globals signals
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_dt_collection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_SELECTION_CHANGED,
                            G_CALLBACK(_dt_selection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                            G_CALLBACK(_dt_profile_change_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_IMAGE_INFO_CHANGED,
                            G_CALLBACK(_dt_image_info_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_MOUSE_OVER_IMAGE_CHANGE,
                            G_CALLBACK(_mouse_over_image_callback), table);

  dt_thumbtable_set_parent(table, mode);

  // Keys used by key-pressed event handler when the grid has the focus
  gchar *path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Move up"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Up, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Move down"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Down, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Move left"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Left, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Move right"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Right, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Go to previous page"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Page_Up, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Go to next page"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Page_Down, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Go to the start"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Home, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Go to the end"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_End, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Select the current thumbnail"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_space, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Toogle the current thumbnail from selection"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_space, GDK_CONTROL_MASK);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Expand the current selection up to the hovered thumbnail"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_space, GDK_SHIFT_MASK);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Open the current thumbnail in darkroom"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Return, 0);
  g_free(path);
  path = dt_accels_build_path(_("Lighttable/Thumbtable"), _("Enable thumbnail transient alternative view"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Alt_L, 0);
  g_free(path);
  path = dt_accels_build_path(_("Global/Menu/File"), _("Remove image from library"));
  dt_accels_new_virtual_shortcut(darktable.gui->accels, darktable.gui->accels->lighttable_accels,
                                 path, table->grid, GDK_KEY_Delete, 0);
  g_free(path);
  return table;
}


void _dt_thumbtable_empty_list(dt_thumbtable_t *table)
{
  // Cleanup existing stuff
  const double start = dt_get_wtime();

  dt_pthread_mutex_lock(&table->lock);
  if(table->list)
  {
    // WARNING: we need to detach children from parent starting from the last
    // otherwise, Gtk updates the index of all the next children in sequence
    // and that takes forever when thumb_nb > 1000
    for(GList *l = g_list_first(table->list); l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      gtk_widget_hide(thumb->widget);
      g_idle_add((GSourceFunc)dt_thumbnail_destroy, thumb);
    }

    g_list_free(g_steal_pointer(&table->list));
  }
  dt_pthread_mutex_unlock(&table->lock);

  dt_print(DT_DEBUG_LIGHTTABLE, "Cleaning the list of %i elements in %0.04f sec\n", table->thumb_nb,
           dt_get_wtime() - start);

  table->list = NULL;
  table->thumb_nb = 0;
  table->thumbs_inited = FALSE;
}


void dt_thumbtable_cleanup(dt_thumbtable_t *table)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_collection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_selection_changed_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_profile_change_callback), table);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_dt_image_info_changed_callback), table);

  _dt_thumbtable_empty_list(table);

  dt_pthread_mutex_destroy(&table->lock);

  if(table->lut) free(table->lut);

  free(table);
  table = NULL;
}

void dt_thumbtable_update_parent(dt_thumbtable_t *table)
{
  g_idle_add((GSourceFunc)_grab_focus, table);
}

void dt_thumbtable_set_parent(dt_thumbtable_t *table, dt_thumbtable_mode_t mode)
{
  table->mode = mode;
  table->parent_overlay = gtk_overlay_new();
  gtk_overlay_add_overlay(GTK_OVERLAY(table->parent_overlay), table->scroll_window);

  if(mode == DT_THUMBTABLE_MODE_FILEMANAGER)
  {
    gtk_widget_set_name(table->grid, "thumbtable-filemanager");
    dt_gui_add_help_link(table->grid, dt_get_help_url("lighttable_filemanager"));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table->scroll_window), GTK_POLICY_NEVER, GTK_POLICY_ALWAYS);
  }
  else if(mode == DT_THUMBTABLE_MODE_FILMSTRIP)
  {
    gtk_widget_set_name(table->grid, "thumbtable-filmstrip");
    dt_gui_add_help_link(table->grid, dt_get_help_url("filmstrip"));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table->scroll_window), GTK_POLICY_ALWAYS, GTK_POLICY_NEVER);
  }
}

void dt_thumbtable_select_all(dt_thumbtable_t *table)
{
  if(!table->collection_inited || table->collection_count == 0) return;

  if(table->collapse_groups)
    dt_control_log(_("Image groups are collapsed in view.\n"
                     "Selecting all images will only target visible members of image groups.\n"
                     "Uncollapse groups to select all their members"));

  GList *img = NULL;

  dt_pthread_mutex_lock(&table->lock);
  for(size_t i = 0; i < table->collection_count; i++)
    img = g_list_prepend(img, GINT_TO_POINTER(table->lut[i].imgid));
  dt_pthread_mutex_unlock(&table->lock);

  if(img)
  {
    dt_selection_select_list(darktable.selection, img);
    g_list_free(img);
  }
}

void dt_thumbtable_select_range(dt_thumbtable_t *table, const int rowid)
{
  if(!table->collection_inited || table->collection_count == 0) return;
  if(rowid < 0 || rowid > table->collection_count - 1) return;

  if(table->collapse_groups)
    dt_control_log(_("Image groups are collapsed in view.\n"
                     "Selecting a range of images will only target visible members of image groups.\n"
                     "Uncollapse groups to select all their members"));

  dt_pthread_mutex_lock(&table->lock);

  // Find the bounds of the current selection
  size_t rowid_end = 0;
  size_t rowid_start = table->collection_count - 1;
  GList *selected = dt_selection_get_list(darktable.selection);

  if(selected)
  {
    for(GList *s = g_list_first(selected); s; s = g_list_next(s))
    {
      int32_t imgid = GPOINTER_TO_INT(s->data);
      int row = _find_rowid_from_imgid(table, imgid);
      if(row < 0) continue; // not found - should not happen
      if(row < rowid_start) rowid_start = row;
      if(row > rowid_end) rowid_end = row;
    }
    g_list_free(selected);
  }
  else
  {
    // range selection always has to start from a previous selection
    dt_pthread_mutex_unlock(&table->lock);
    return;
  }

  if(rowid_start > rowid_end)
  {
     // the start is strictly after the end, we have a deep problem
     dt_pthread_mutex_unlock(&table->lock);
    return;
  }

  // Find the extra imgids to select
  GList *img = NULL;
  if(rowid > rowid_end && rowid_end < table->collection_count - 1)
  {
    // select after
    for(int i = rowid_end + 1; i <= rowid; i++)
      img = g_list_prepend(img, GINT_TO_POINTER(table->lut[i].imgid));
  }
  else if(rowid < rowid_start && rowid_start > 0)
  {
    // select before
    for(int i = rowid_start - 1; i >= rowid; i--)
      img = g_list_prepend(img, GINT_TO_POINTER(table->lut[i].imgid));
  }
  // else: select within. What should that yield ??? Deselect ?

  dt_pthread_mutex_unlock(&table->lock);

  if(img)
  {
    dt_selection_select_list(darktable.selection, img);
    g_list_free(img);
  }
}

void dt_thumbtable_invert_selection(dt_thumbtable_t *table)
{
  if(!table->collection_inited || table->collection_count == 0) return;

  // Record initial selection, select all, then deselect initial selection
  GList *to_deselect = dt_selection_get_list(darktable.selection);
  if(to_deselect)
  {
    dt_thumbtable_select_all(table);
    dt_selection_deselect_list(darktable.selection, to_deselect);
    g_list_free(to_deselect);
  }
}

static gint64 next_over_time = 0;

void dt_thumbtable_dispatch_over(dt_thumbtable_t *table, GdkEventType type, int32_t imgid)
{
  if(!gtk_widget_is_visible(table->scroll_window)) return;

  gint64 current_time = g_get_real_time(); // microseconds
  if(type == GDK_KEY_PRESS || type == GDK_KEY_RELEASE)
  {
    // allow the mouse to capture the next hover events
    // in more than 100 ms:
    next_over_time = current_time + 100000;

    dt_control_set_mouse_over_id(imgid);
    dt_control_set_keyboard_over_id(imgid);
  }
  else if(type == GDK_ENTER_NOTIFY || type == GDK_LEAVE_NOTIFY)
  {
    // When navigating the grid with arrow keys, the view will get scrolled.
    // If the mouse pointer is over the grid, it will enter a new thumbnail
    // which will trigger leave/enter events.
    // But we don't want that when interacting from the keyboard, so disallow
    // recording enter/leave events in the next 100 ms after keyboard interaction.
    if(current_time > next_over_time)
      dt_control_set_mouse_over_id(imgid);
    else
      return;
  }
  else if(type == GDK_MOTION_NOTIFY	|| type == GDK_BUTTON_PRESS || type == GDK_2BUTTON_PRESS)
  {
    // Active mouse pointer interactions: accept unconditionnaly
    dt_control_set_mouse_over_id(imgid);
  }
  else
  {
    fprintf(stderr, "[dt_thumbtable_dispatch_over] unsupported event type: %i\n", type);
    return;
  }

  dt_pthread_mutex_lock(&table->lock);
  table->rowid = _find_rowid_from_imgid(table, imgid);
  dt_pthread_mutex_unlock(&table->lock);

  // Attempt to re-grab focus on every interaction to restore keyboard navigation,
  // for example after a combobox grabbed it on click.
  if(!gtk_widget_has_focus(table->grid))
  {
    // But giving focus to the grid scrolls it back to top, so we have to re-scroll it after
    double x = 0.;
    double y = 0.;
    dt_thumbtable_get_scroll_position(table, &x, &y);
    gtk_widget_grab_focus(table->grid);
    dt_thumbtable_scroll_to_position(table, x, y);
  }
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
