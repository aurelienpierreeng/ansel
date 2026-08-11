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
    Copyright (C) 2026 Guillaume STUTIN.

    
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
#include "database/image_repository.h"
#include "widgets/button.h"
#include "control/jobs/control_jobs.h"
#include "system/macros.h"
#include "common/module_versioning.h"
#include "common/metadata.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/dev_snapshot.h"
#include "gui/dtgtk/thumbnail.h"

#include "libs/lib.h"

#include <sqlite3.h>
#include "gui/window_manager.h"
#include "widgets/accelerators.h"
#include "widgets/container.h"
#include "widgets/scroll_wrap.h"
#include "widgets/widget_settings.h"
#include "widgets/widget_style.h"

DT_MODULE(1)


typedef struct dt_lib_duplicate_t
{
  GtkWidget *duplicate_box;
  int32_t imgid;                 // duplicate currently held under mouse press, UNKNOWN_IMAGE if none
  dt_dev_snapshot_t preview;     // hold-to-preview render, ROI-scoped and recomputed on pan/zoom, see develop/dev_snapshot.h
  int32_t preview_cached_imgid;  // which imgid `preview` currently holds, UNKNOWN_IMAGE if none

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

  if(imgid == dt_dev_get_global()->image_storage.id)
  {
    // we find the duplicate image to show now
    for(GList *l = d->thumbs; l; l = g_list_next(l))
    {
      dt_thumbnail_t *thumb = (dt_thumbnail_t *)l->data;
      if(thumb->info.id == imgid)
      {
        GList *l2 = g_list_next(l);
        if(IS_NULL_PTR(l2)) l2 = g_list_previous(l);
        if(l2)
        {
          dt_thumbnail_t *th2 = (dt_thumbnail_t *)l2->data;
          DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_VIEWMANAGER_THUMBTABLE_ACTIVATE,
                                        th2->info.id);
          break;
        }
      }
    }
  }

  // and we remove the image
  dt_control_delete_image(imgid);
  dt_collection_update_query(dt_collection_get_global(), DT_COLLECTION_CHANGE_RELOAD, DT_COLLECTION_PROP_UNDEF,
                             g_list_prepend(NULL, GINT_TO_POINTER(imgid)));
}

static gboolean _lib_duplicate_thumb_press_callback(GtkWidget *widget, GdkEventButton *event, dt_lib_module_t *self)
{
  if(event->button == 1 && event->type == GDK_BUTTON_PRESS)
  {
    dt_develop_t *dev = dt_dev_get_global();
    if(IS_NULL_PTR(dev)) return FALSE;

    dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
    const int32_t imgid = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "imgid"));
    if(imgid <= 0) return FALSE;

    // Render once per duplicate and keep it around for the panel's lifetime -- re-pressing the
    // same thumbnail is then instant. The first press on a given duplicate still pays for a full
    // pipeline run (same cost as the "take snapshot" button in libs/snapshots.c), so show the
    // same busy-cursor feedback while it's in flight.
    if(d->preview_cached_imgid != imgid)
    {
      dt_control_change_cursor_by_name_and_flush("progress");
      const gboolean ok = dt_dev_snapshot_capture(&d->preview, dev, imgid, NULL, NULL, -1);
      dt_control_commit_cursor();
      d->preview_cached_imgid = ok ? imgid : UNKNOWN_IMAGE;
    }

    d->imgid = (d->preview_cached_imgid == imgid) ? imgid : UNKNOWN_IMAGE;
    dt_control_queue_redraw_center();
    return TRUE;
  }
  return FALSE;
}

// Cancels the hold-to-preview, whether triggered by releasing the button or by the pointer
// leaving the thumbnail while still held -- see the two signals this is connected to below.
static gboolean _lib_duplicate_thumb_revert_callback(GtkWidget *widget, GdkEvent *event, dt_lib_module_t *self)
{
  // thumb->widget (the event box this is connected to) is not a single window: it contains
  // several unconditionally-shown child event boxes/drawing areas of its own (w_image,
  // w_top_eb, w_bottom_eb in dtgtk/thumbnail.c), each with their own GdkWindow. Moving the
  // pointer between those, while still inside the thumbnail's visible bounds, crosses a child
  // window boundary and fires a leave-notify on the parent too (detail == GDK_NOTIFY_INFERIOR).
  // Only a crossing to a window that is NOT a descendant is an actual "left the thumbnail".
  if(event->type == GDK_LEAVE_NOTIFY && event->crossing.detail == GDK_NOTIFY_INFERIOR) return FALSE;

  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  d->imgid = UNKNOWN_IMAGE;
  dt_control_queue_redraw_center();

  return FALSE;
}

/* while a duplicate thumbnail is held, show it full-frame in the center view instead of the
   image currently being edited, matching the live pan/zoom -- released on button-up. */
void gui_post_expose(dt_lib_module_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  if(IS_NULL_PTR(d) || d->imgid <= 0 || d->preview_cached_imgid != d->imgid) return;

  dt_develop_t *dev = dt_dev_get_global();
  float image_box[4] = { 0.0f };
  dt_dev_get_image_box_in_widget(dev, width, height, image_box);
  if(image_box[2] <= 0.0f || image_box[3] <= 0.0f) return;

  dt_dev_snapshot_draw(&d->preview, cri, dev, width, height, image_box[0], image_box[1], image_box[2],
                       image_box[3]);
}

void view_leave(struct dt_lib_module_t *self, struct dt_view_t *old_view, struct dt_view_t *new_view)
{
  // we leave the view. Let's destroy the cached preview if any
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;
  dt_dev_snapshot_clear(&d->preview);
  d->preview_cached_imgid = UNKNOWN_IMAGE;
}

static void _thumb_remove(gpointer user_data)
{
  dt_thumbnail_t *thumb = (dt_thumbnail_t *)user_data;
  if(IS_NULL_PTR(thumb)) return;

  GtkWidget *parent = NULL;
  if(!IS_NULL_PTR(thumb->w_main)) parent = gtk_widget_get_parent(thumb->w_main);
  if(!IS_NULL_PTR(parent) && GTK_IS_CONTAINER(parent) && !IS_NULL_PTR(thumb->w_main))
    gtk_container_remove(GTK_CONTAINER(parent), thumb->w_main);

  dt_thumbnail_destroy(thumb);
}

static void _lib_duplicate_init_callback(gpointer instance, dt_lib_module_t *self)
{
  //block signals to avoid concurrent calls
  dt_control_signal_block_by_func(dt_control_signal_get_global(), G_CALLBACK(_lib_duplicate_init_callback), self);

  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  d->imgid = UNKNOWN_IMAGE;
  // we drop the cached preview if any -- it belongs to a thumb the list rebuild below is about
  // to tear down, and stays keyed to a specific imgid that may no longer even be a duplicate of
  // whatever image this panel now describes
  dt_dev_snapshot_clear(&d->preview);
  d->preview_cached_imgid = UNKNOWN_IMAGE;
  // we drop all the thumbs
  g_list_free_full(d->thumbs, _thumb_remove);
  d->thumbs = NULL;
  // and the other widgets too
  dt_gui_container_destroy_children(GTK_CONTAINER(d->duplicate_box));
  // retrieve all the versions of the image
  dt_develop_t *dev = dt_dev_get_global();

  int count = 0;

  // we get a summarize of all versions of the image
  // clang-format off
  // Materialised first: the loop below builds a thumbnail and several widgets per row, which
  // re-enter the image cache and the database -- it used to do that from inside its own cursor.
  GList *versions = dt_image_repository_get_versions(dev->image_storage.film_id,
                                                     dev->image_storage.filename,
                                                     DT_METADATA_XMP_VERSION_NAME);

  GtkWidget *bt = NULL;

  for(GList *l = versions; l; l = g_list_next(l))
  {
    const dt_image_version_t *v = (const dt_image_version_t *)l->data;
    GtkWidget *hb = gtk_grid_new();
    const int32_t imgid = v->imgid;
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
      g_object_set_data(G_OBJECT(thumb->widget), "imgid", GINT_TO_POINTER(imgid));
      g_signal_connect(G_OBJECT(thumb->widget), "button-press-event",
                       G_CALLBACK(_lib_duplicate_thumb_press_callback), self);
      g_signal_connect(G_OBJECT(thumb->widget), "button-release-event",
                       G_CALLBACK(_lib_duplicate_thumb_revert_callback), self);
      // GTK keeps delivering crossing events to the widget that holds the implicit pointer
      // grab, so dragging off the thumbnail without releasing is caught here the same way.
      gtk_widget_add_events(thumb->widget, GDK_LEAVE_NOTIFY_MASK);
      g_signal_connect(G_OBJECT(thumb->widget), "leave-notify-event",
                       G_CALLBACK(_lib_duplicate_thumb_revert_callback), self);
    }

    gchar chl[256];
    const gchar *path = v->version_name;
    g_snprintf(chl, sizeof(chl), "%d", v->version);

    GtkWidget *tb = gtk_entry_new();
    dt_accels_disconnect_on_text_input(tb);
    if(path) gtk_entry_set_text(GTK_ENTRY(tb), path);
    gtk_entry_set_width_chars(GTK_ENTRY(tb), 0);
    gtk_widget_set_hexpand(tb, TRUE);
    g_object_set_data (G_OBJECT(tb), "imgid", GINT_TO_POINTER(imgid));
    gtk_widget_add_events(tb, GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(G_OBJECT(tb), "focus-out-event", G_CALLBACK(_lib_duplicate_caption_out_callback), self);
    GtkWidget *lb = gtk_label_new(chl);
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
  g_list_free_full(versions, dt_image_version_free);

  gtk_widget_show(d->duplicate_box);

  // we have a single image, do not allow it to be removed so hide last bt
  if(count==1)
  {
    gtk_widget_set_sensitive(bt, FALSE);
    gtk_widget_set_visible(bt, FALSE);
  }

  dt_control_signal_unblock_by_func(dt_control_signal_get_global(), G_CALLBACK(_lib_duplicate_init_callback), self); //unblock signals
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
  gtk_widget_queue_draw (d->duplicate_box);
  dt_control_queue_redraw_center();
}


void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)g_malloc0(sizeof(dt_lib_duplicate_t));
  self->data = (void *)d;

  d->imgid = UNKNOWN_IMAGE;
  d->preview_cached_imgid = UNKNOWN_IMAGE;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  dt_gui_add_class(self->widget, "dt_duplicate_ui");

  d->duplicate_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);

  GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);

  /* add duplicate list and buttonbox to widget */
  gtk_box_pack_start(GTK_BOX(self->widget),
                     dt_ui_scroll_wrap(d->duplicate_box, 1, "plugins/darkroom/duplicate/windowheight",
                                       DT_UI_RESIZE_DYNAMIC), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hb, TRUE, TRUE, 0);

  gtk_widget_show_all(self->widget);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_IMAGE_CHANGED, G_CALLBACK(_lib_duplicate_init_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_INITIALIZE, G_CALLBACK(_lib_duplicate_init_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_duplicate_collection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED,
                            G_CALLBACK(_lib_duplicate_preview_updated_callback), self);
}

void gui_cleanup(dt_lib_module_t *self)
{
  if(IS_NULL_PTR(self->data)) return;
  dt_lib_duplicate_t *d = (dt_lib_duplicate_t *)self->data;

  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_lib_duplicate_init_callback), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_lib_duplicate_collection_changed), self);
  DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_lib_duplicate_preview_updated_callback), self);

  if(!IS_NULL_PTR(d))
  {
    dt_dev_snapshot_clear(&d->preview);

    g_list_free_full(d->thumbs, _thumb_remove);
    d->thumbs = NULL;
    dt_gui_container_destroy_children(GTK_CONTAINER(d->duplicate_box));
  }


  dt_free(self->data);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
