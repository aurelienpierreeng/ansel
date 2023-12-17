#include "gui/actions/menu.h"
#include "control/crawler.h"
#include "common/collection.h"
#include "control/jobs.h"

static void clear_caches_callback(GtkWidget *widget)
{
  dt_dev_reprocess_all(darktable.develop);
  dt_control_queue_redraw();
  dt_dev_refresh_ui_images(darktable.develop);
}

static void optimize_database_callback(GtkWidget *widget)
{
  dt_database_perform_maintenance(darktable.db);
}

static void backup_database_callback(GtkWidget *widget)
{
  dt_database_snapshot(darktable.db);
}

static void crawl_xmp_changes(GtkWidget *widget)
{
  GList *changed_xmp_files = dt_control_crawler_run();
  dt_control_crawler_show_image_list(changed_xmp_files);
}

static int32_t preload_image_cache(dt_job_t *job)
{
  // Load the mipmap cache sizes 0 to 4 of the current selection
  GList *collection = dt_collection_get_all(darktable.collection, -1);
  collection = g_list_first(collection);
  float i = 0.f;
  float imgs = (float)g_list_length(collection);

  while(collection && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    const int imgid = GPOINTER_TO_INT(collection->data);

    for(int k = 4; k >= 0; k--)
    {
      dt_mipmap_buffer_t buf;
      dt_mipmap_cache_get(darktable.mipmap_cache, &buf, imgid, k, DT_MIPMAP_BLOCKING, 'r');
      dt_mipmap_cache_release(darktable.mipmap_cache, &buf);
      fprintf(stdout, "Processing mipmap %i for image %i\n", k, imgid);
    }

    dt_history_hash_set_mipmap(imgid);
    i += 1.f;
    dt_control_job_set_progress(job, i / imgs);
    collection = g_list_next(collection);
  }
  return 0;
}

static void preload_image_cache_callback(GtkWidget *widget)
{
  dt_job_t *job = dt_control_job_create(&preload_image_cache, "preload");
  dt_control_job_add_progress(job, _("Preloading cache for current collection"), TRUE);
  dt_control_add_job(darktable.control, DT_JOB_QUEUE_USER_BG, job);
}

static void write_XMP()
{
  dt_control_write_sidecar_files();
}

void append_run(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Clear all pipeline caches"), index, NULL, clear_caches_callback, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("Preload collection thumbnails"), index, NULL, preload_image_cache_callback, NULL, NULL, NULL);
  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("Defragment the library"), index, NULL, optimize_database_callback, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("Backup the library"), index, NULL, backup_database_callback, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("Resynchronize library and XMP"), index, NULL, crawl_xmp_changes, NULL, NULL, NULL);
  add_sub_menu_entry(menus, lists, _("Save all developments to XMP"), index, NULL, write_XMP, NULL, NULL, NULL);
}
