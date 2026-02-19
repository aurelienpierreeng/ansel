/*
    This file is part of the Ansel project.
    Copyright (C) 2023-2026 Aurélien PIERRE.
    
    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "gui/actions/menu.h"
#include "control/crawler.h"
#include "common/collection.h"
#include "common/selection.h"
#include "control/jobs.h"

static gboolean clear_caches_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_dev_pixelpipe_reset_all(darktable.develop);
  dt_dev_pixelpipe_refresh_all(darktable.develop, TRUE);
  return TRUE;
}

static gboolean optimize_database_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_database_perform_maintenance(darktable.db);
  return TRUE;
}

static gboolean backup_database_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_database_snapshot(darktable.db);
  return TRUE;
}

static gboolean crawl_xmp_changes(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  GList *changed_xmp_files = dt_control_crawler_run();
  dt_control_crawler_show_image_list(changed_xmp_files);
  return TRUE;
}

static int32_t preload_image_cache(dt_job_t *job)
{
  // Load the mipmap cache sizes 0 to 4 of the current selection
  GList *selection = dt_selection_get_list(darktable.selection);
  int i = 0;
  float imgs = (float)dt_selection_get_length(darktable.selection) * DT_MIPMAP_F;
  GList *img = g_list_first(selection);

  while(img && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    const int32_t imgid = GPOINTER_TO_INT(img->data);

    // Note : we generate from large scale to small,
    // because the mipmap code has a mechanism that downscales
    // higher resolution thumbnails if present, rather
    // than recomputing a pipe from scratch.
    for(int k = DT_MIPMAP_F - 1; k >= DT_MIPMAP_0 && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED; k--)
    {
      char filename[PATH_MAX] = { 0 };
      snprintf(filename, sizeof(filename), "%s.d/%d/%d.jpg", darktable.mipmap_cache->cachedir, k, imgid);

      // if a valid thumbnail file is already on disc - do nothing
      if(dt_util_test_image_file(filename)) continue;

      // else, generate thumbnail and store in mipmap cache.
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, k, DT_MIPMAP_BLOCKING, 'r');
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);

      i++;
      dt_control_job_set_progress(job, (float)i / imgs);
    }

    // and immediately write thumbs to disc and remove from mipmap cache.
    dt_mimap_cache_evict(darktable.mipmap_cache, imgid);

    img = g_list_next(img);
  }

  g_list_free(selection);
  return 0;
}

static gboolean preload_image_cache_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_job_t *job = dt_control_job_create(&preload_image_cache, "preload");
  dt_control_job_add_progress(job, _("Preloading cache for current collection"), TRUE);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, job);
  return TRUE;
}

static gboolean clear_image_cache(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  GList *selection = dt_selection_get_list(darktable.selection);

  for(GList *img = g_list_first(selection); img; img = g_list_next(img))
  {
    const int32_t imgid = GPOINTER_TO_INT(img->data);
    dt_mipmap_cache_remove(darktable.mipmap_cache, imgid, TRUE);
  }

  g_list_free(selection);

  // Redraw thumbnails
  dt_thumbtable_refresh_thumbnail(darktable.gui->ui->thumbtable_lighttable, UNKNOWN_IMAGE, TRUE);
  return TRUE;
}

MAKE_ACCEL_WRAPPER(dt_control_write_sidecar_files)
MAKE_ACCEL_WRAPPER(dt_image_local_copy_synch)

void append_run(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Clear darkroom pipeline caches"), index, NULL, clear_caches_callback, NULL, NULL, NULL, 0, 0);
  add_sub_menu_entry(menus, lists, _("Preload selected thumbnails in cache"), index, NULL, preload_image_cache_callback, NULL, NULL, has_active_images, 0, 0);
  add_sub_menu_entry(menus, lists, _("Purge selected thumbnails from cache"), index, NULL, clear_image_cache, NULL, NULL, has_active_images, 0, 0);
  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("Defragment the library"), index, NULL, optimize_database_callback, NULL, NULL, NULL, 0, 0);
  add_sub_menu_entry(menus, lists, _("Backup the library"), index, NULL, backup_database_callback, NULL, NULL, NULL, 0, 0);
  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("Resynchronize library and XMP"), index, NULL, crawl_xmp_changes, NULL, NULL, NULL, 0, 0);
  add_sub_menu_entry(menus, lists, _("Save selected developments to XMP"), index, NULL, GET_ACCEL_WRAPPER(dt_control_write_sidecar_files), NULL, NULL, has_active_images, 0, 0);
  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("Resynchronize database with distant XMP for local copies"), index, NULL, GET_ACCEL_WRAPPER(dt_image_local_copy_synch), NULL, NULL, NULL, 0, 0);
}
