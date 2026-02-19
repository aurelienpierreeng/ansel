/*
    This file is part of darktable,
    Copyright (C) 2015-2016, 2019-2022 Aldric Renaudin.
    Copyright (C) 2018-2019 Matthieu Moy.
    Copyright (C) 2018-2021 Pascal Obry.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2020, 2022-2023, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2019 Edgardo Hoszowski.
    Copyright (C) 2019-2020 Philippe Weyland.
    Copyright (C) 2020 Chris Elston.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020 Marco.
    Copyright (C) 2020 Mark-64.
    Copyright (C) 2020, 2022 Nicolas Auffray.
    Copyright (C) 2021 Fabio Heer.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023 Alynx Zhou.
    
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
#include "common/history.h"
#include "common/metadata.h"
#include "common/mipmap_cache.h"
#include "common/selection.h"
#include "common/styles.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "dtgtk/thumbnail.h"

#include "gui/gtk.h"
#include "gui/styles.h"
#include "libs/lib.h"

#define DUPLICATE_COMPARE_SIZE 40

DT_MODULE(1)

static sqlite3_stmt *_duplicate_versions_stmt = NULL;

typedef struct dt_lib_duplicate_t
{
  GtkWidget *duplicate_box;
  int32_t imgid;
  gboolean busy;
  int cur_final_width;
  int cur_final_height;
  int32_t preview_width;
  int32_t preview_height;
  gboolean allow_zoom;

  cairo_surface_t *preview_surf;
  float preview_zoom;
  int preview_id;

  GList *thumbs;
} dt_lib_duplicate_t;

const char *name(struct dt_lib_module_t *self)
{
  return _("Duplicates");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 850;
}

static void _lib_duplicate_init_callback(gpointer instance, dt_lib_module_t *self);

static gboolean _lib_duplicate_caption_out_callback(GtkWidget *widget, GdkEvent *event, dt_lib_module_t *self)
{
  const int32_t imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget),"imgid"));

  // we write the content of the textbox to the caption field
  dt_metadata_set(imgid, "Xmp.darktable.version_name", gtk_entry_get_text(GTK_ENTRY(widget)), FALSE);
  dt_control_save_xmp(imgid);

  return FALSE;
}

static void _lib_duplicate_delete(GtkButton *button, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  const int32_t imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "imgid"));

  if(imgid == darktable.develop->image_storage.id)
  {
    // we find the duplicate image to show now
    for(GList *l = d->thumbs; l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      if(thumb->info.id == imgid)
      {
        GList *l2 = g_list_next(l);
        if(!l2) l2 = g_list_previous(l);
        if(l2)
        {
          dt_thumbnail_t *th2 = (dt_thumbnail_t *)l2->data;
          DT_DEBUG_CONTROL_SIGNAL_RAISE(darktable.signals, DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                                        th2->info.id);
          break;
        }
      }
    }
  }

  // and we remove the image
  dt_control_delete_image(imgid);
  dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_prepend(NULL, GINT_TO_POINTER(imgid)));
}

static gboolean _lib_duplicate_thumb_press_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(event->button == 1)
  {
    if(event->type == GDK_BUTTON_PRESS)
    {
      dt_develop_t *dev = darktable.develop;
      if(!dev) return FALSE;
      return TRUE;
    }
  }
  return FALSE;
}

static gboolean _lib_duplicate_thumb_release_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  d->imgid = UNKNOWN_IMAGE;
  if(d->busy)
  {
    dt_control_log_busy_leave();
    dt_control_toast_busy_leave();
  }
  d->busy = FALSE;
  dt_control_queue_redraw_center();

  return FALSE;
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  // we leave the view. Let's destroy preview surf if any
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  if(d->preview_surf)
  {
    cairo_surface_destroy(d->preview_surf);
    d->preview_surf = NULL;
  }
}

static void _thumb_remove(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(thumb->w_main)), thumb->w_main);
  dt_thumbnail_destroy(thumb);
}

static void _lib_duplicate_init_callback(gpointer instance, dt_lib_module_t *self)
{
  //block signals to avoid concurrent calls
  dt_control_signal_block_by_func(darktable.signals, G_CALLBACK(_lib_duplicate_init_callback), self);

  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  d->imgid = UNKNOWN_IMAGE;
  // we drop the preview if any
  if(d->preview_surf)
  {
    cairo_surface_destroy(d->preview_surf);
    d->preview_surf = NULL;
  }
  // we drop all the thumbs
  g_list_free_full(d->thumbs, _thumb_remove);
  d->thumbs = NULL;
  // and the other widgets too
  dt_gui_container_destroy_children(GTK_CONTAINER(d->duplicate_box));
  // retrieve all the versions of the image
  sqlite3_stmt *stmt;
  dt_develop_t *dev = darktable.develop;

  int count = 0;

  // we get a summarize of all versions of the image
  // clang-format off
  if(!_duplicate_versions_stmt)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT i.version, i.id, m.value"
                                " FROM images AS i"
                                " LEFT JOIN meta_data AS m ON m.id = i.id AND m.key = ?3"
                                " WHERE film_id = ?1 AND filename = ?2"
                                " ORDER BY i.version",
                                -1, &_duplicate_versions_stmt, NULL);
  }
  stmt = _duplicate_versions_stmt;
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  // clang-format on
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dev->image_storage.film_id);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 2, dev->image_storage.filename, -1, SQLITE_TRANSIENT);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, DT_METADATA_XMP_VERSION_NAME);

  GtkWidget *bt = NULL;

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    GtkWidget *hb = gtk_grid_new();
    const int32_t imgid = sqlite3_column_int(stmt, 1);
    dt_image_t info = { 0 };
    info.id = imgid;
    dt_thumbnail_t *thumb = dt_thumbnail_new(0, DT_THUMBNAIL_OVERLAYS_NONE, NULL, &info);

    thumb->disable_mouseover = TRUE;
    thumb->disable_actions = TRUE;
    dt_thumbnail_resize(thumb, DT_PIXEL_APPLY_DPI(92), DT_PIXEL_APPLY_DPI(92));
    dt_thumbnail_set_mouseover(thumb, imgid == dev->image_storage.id);
    dt_thumbnail_update_selection(thumb, imgid == dev->image_storage.id);
    gtk_widget_queue_draw(thumb->widget);

    if(imgid != dev->image_storage.id)
    {
      g_signal_connect(G_OBJECT(thumb->widget), "button-press-event",
                       G_CALLBACK(_lib_duplicate_thumb_press_callback), self);
      g_signal_connect(G_OBJECT(thumb->widget), "button-release-event",
                       G_CALLBACK(_lib_duplicate_thumb_release_callback), self);
    }

    gchar chl[256];
    gchar *path = (gchar *)sqlite3_column_text(stmt, 2);
    g_snprintf(chl, sizeof(chl), "%d", sqlite3_column_int(stmt, 0));

    GtkWidget *tb = gtk_entry_new();
    dt_accels_disconnect_on_text_input(tb);
    if(path) gtk_entry_set_text(GTK_ENTRY(tb), path);
    gtk_entry_set_width_chars(GTK_ENTRY(tb), 0);
    gtk_widget_set_hexpand(tb, TRUE);
    g_object_set_data (G_OBJECT(tb), "imgid", GINT_TO_POINTER(imgid));
    gtk_widget_add_events(tb, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(G_OBJECT(tb), "focus-out-event", G_CALLBACK(_lib_duplicate_caption_out_callback), self);
    GtkWidget *lb = gtk_label_new (g_strdup(chl));
    gtk_widget_set_hexpand(lb, TRUE);
    bt = dtgtk_button_new(dtgtk_cairo_paint_remove, 0, NULL);
    //    gtk_widget_set_halign(bt, GTK_ALIGN_END);
    g_object_set_data(G_OBJECT(bt), "imgid", GINT_TO_POINTER(imgid));
    g_signal_connect(G_OBJECT(bt), "clicked", G_CALLBACK(_lib_duplicate_delete), self);

    gtk_grid_attach(GTK_GRID(hb), thumb->widget, 0, 0, 1, 2);
    gtk_grid_attach(GTK_GRID(hb), bt, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(hb), lb, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(hb), tb, 1, 1, 2, 1);

    // Can't use gtk_widget_show_all here or the buttons of the thumbnail will show too
    gtk_widget_show(thumb->widget);
    gtk_widget_show(hb);
    gtk_widget_show(lb);
    gtk_widget_show(tb);
    gtk_box_pack_start(GTK_BOX(d->duplicate_box), hb, FALSE, FALSE, 0);
    d->thumbs = g_list_append(d->thumbs, thumb);
    count++;
  }
  sqlite3_reset(stmt);

  gtk_widget_show(d->duplicate_box);

  // we have a single image, do not allow it to be removed so hide last bt
  if(count==1)
  {
    gtk_widget_set_sensitive(bt, FALSE);
    gtk_widget_set_visible(bt, FALSE);
  }

  // and reset the final size of the current image
  if(dev->image_storage.id >= 0)
  {
    d->cur_final_width = 0;
    d->cur_final_height = 0;
  }

  dt_control_signal_unblock_by_func(darktable.signals, G_CALLBACK(_lib_duplicate_init_callback), self); //unblock signals
}

static void _lib_duplicate_collection_changed(gpointer instance, dt_collection_change_t query_change,
                                              dt_collection_properties_t changed_property, gpointer imgs, int next,
                                              dt_lib_module_t *self)
{
  _lib_duplicate_init_callback(instance, self);
}

static void _lib_duplicate_preview_updated_callback(gpointer instance, dt_lib_module_t *self)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  // we reset the final size of the current image
  if(darktable.develop->image_storage.id >= 0)
  {
    d->cur_final_width = 0;
    d->cur_final_height = 0;
  }

  gtk_widget_queue_draw (d->duplicate_box);
  dt_control_queue_redraw_center();
}


void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)g_malloc0(sizeof(dt_lib_duplicate_t));
  self->data = (void *)d;

  d->imgid = UNKNOWN_IMAGE;
  d->preview_surf = NULL;
  d->preview_zoom = 1.0;
  d->preview_width = 0;
  d->preview_height = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  dt_gui_add_class(self->widget, "dt_duplicate_ui");

  d->duplicate_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

  /* add duplicate list and buttonbox to widget */
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->duplicate_box, 1, "plugins/darkroom/duplicate/windowheight"), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hb, TRUE, TRUE, 0);

  gtk_widget_show_all(self->widget);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_IMAGE_CHANGED, G_CALLBACK(_lib_duplicate_init_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_INITIALIZE, G_CALLBACK(_lib_duplicate_init_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_duplicate_collection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(darktable.signals, DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_lib_duplicate_preview_updated_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_duplicate_init_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(darktable.signals, G_CALLBACK(_lib_duplicate_preview_updated_callback), self);

  if(_duplicate_versions_stmt)
  {
    sqlite3_finalize(_duplicate_versions_stmt);
    _duplicate_versions_stmt = NULL;
  }

  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
