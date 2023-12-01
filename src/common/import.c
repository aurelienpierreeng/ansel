/*
    This file is part of Ansel.
    Copyright (C) 2011-2022 darktable developers.
    Copyright (C) 2023 Aurélien Pierre.

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

#include "bauhaus/bauhaus.h"
#include "common/collection.h"
#include "common/darktable.h"
#include "common/file_location.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/import.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/metadata.h"
#include "common/datetime.h"
#include "common/selection.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/draw.h"
#include "gui/import_metadata.h"
#include "gui/preferences.h"
#include "gui/gtkentry.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#ifdef _WIN32
//MSVCRT does not have strptime implemented
#include "win/strptime.h"
#endif
#include <strings.h>
#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

static GSList *_get_subfolder(GFile *folder);
static void _do_select_all(dt_lib_import_t *d);
static void _do_select_none(dt_lib_import_t *d);
static void _do_select_new(dt_lib_import_t *d);

static GdkPixbuf *_import_get_thumbnail(const gchar *filename, const int width, const int height)
{
  GdkPixbuf *pixbuf = NULL;
  gboolean no_preview_fallback = FALSE;

  if(!filename || !g_file_test(filename, G_FILE_TEST_IS_REGULAR))
    no_preview_fallback = TRUE;

  // Step 1: try to check whether the picture contains embedded thumbnail
  // In case it has, we'll use that thumbnail to show on the dialog
  if(!no_preview_fallback)
  {
    uint8_t *buffer = NULL;
    size_t size = 0;
    char *mime_type = NULL;
    if(!dt_exif_get_thumbnail(filename, &buffer, &size, &mime_type))
    {
      // Scale the image to the correct size
      GdkPixbuf *tmp;
      GdkPixbufLoader *loader = gdk_pixbuf_loader_new();

      // Calling gdk_pixbuf_loader_close forces the data to be parsed by the
      // loader. We must do this before calling gdk_pixbuf_loader_get_pixbuf.
      if (!gdk_pixbuf_loader_write(loader, buffer, size, NULL)) goto cleanup;
      if(!gdk_pixbuf_loader_close(loader, NULL)) goto cleanup;
      if (!(tmp = gdk_pixbuf_loader_get_pixbuf(loader))) goto cleanup;

      const float ratio = ((float)gdk_pixbuf_get_height(tmp)) / ((float)gdk_pixbuf_get_width(tmp));
      pixbuf = gdk_pixbuf_scale_simple(tmp, width / ratio, height, GDK_INTERP_BILINEAR);

    cleanup:
      gdk_pixbuf_loader_close(loader, NULL);
      free(mime_type);
      free(buffer);
      g_object_unref(loader); // This should clean up tmp as well
    }
    else
    {
      // Fallback to whatever Gtk found in the file
      pixbuf = gdk_pixbuf_new_from_file_at_size(filename, width, height, NULL);
    }
  }

  if(!no_preview_fallback && pixbuf != NULL)
  {
    // get image orientation
    dt_image_t img = { 0 };
    (void)dt_exif_read(&img, filename);

    // Rotate the image to the correct orientation
    GdkPixbuf *tmp = pixbuf;

    if(img.orientation == ORIENTATION_ROTATE_CCW_90_DEG)
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
    else if(img.orientation == ORIENTATION_ROTATE_CW_90_DEG)
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);
    else if(img.orientation == ORIENTATION_ROTATE_180_DEG)
      tmp = gdk_pixbuf_rotate_simple(pixbuf, GDK_PIXBUF_ROTATE_UPSIDEDOWN);

    if(pixbuf != tmp)
    {
      g_object_unref(pixbuf);
      pixbuf = tmp;
    }
  }

  return pixbuf;
}

static void _do_select_all_clicked(GtkWidget *widget, dt_lib_import_t *d)
{
  _do_select_all(d);
}

static void _do_select_none_clicked(GtkWidget *widget, dt_lib_import_t *d)
{
  _do_select_none(d);
}

static void _do_select_new_clicked(GtkWidget *widget, dt_lib_import_t *d)
{
  _do_select_new(d);
}


static void _resize_dialog(GtkWidget *widget)
{
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  dt_conf_set_int("ui_last/import_dialog_width", allocation.width);
  dt_conf_set_int("ui_last/import_dialog_height", allocation.height);
}

/* Add file extension patterns for file chooser filters
* Bloody GTK doesn't support regex patterns so we need to unroll
* every combination separately, for lowercase and uppercase.
*/
static void _file_filters(GtkWidget *file_chooser)
{
  GtkFileFilter *filter;

  const char *raster[] = {
    "jpg", "jpeg", "j2c", "jp2", "tif", "tiff", "png", "exr",
    "dng", "heif", "heic", "avi", "avif", "webp", NULL };

  const char *raw[] = {
    "3fr", "ari", "arw", "bay", "bmq", "cap", "cine", "cr2",
    "cr3", "crw", "cs1", "dc2", "dcr", "dng", "gpr", "erf",
    "fff", "hdr",  "ia", "iiq", "k25", "kc2", "kdc", "mdc",
    "mef", "mos", "mrw", "nef", "nrw", "orf", "ori", "pef",
    "pfm", "pnm", "pxn", "qtk", "raf", "raw", "rdc", "rw2",
    "rwl", "sr2", "srf", "srw", "sti", "x3f",  NULL };

  /* ALL IMAGES */
  filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("All image files"));
  //TODO: use dt_supported_extensions list ?
  for(int i = 0; i < 46; i++)
  {
    gtk_file_filter_add_pattern(filter, g_strdup_printf("*.%s", raw[i]));
    gtk_file_filter_add_pattern(filter, g_utf8_strup(g_strdup_printf("*.%s", raw[i]), -1));
  }
  for(int i = 0; i < 14; i++)
  {
    gtk_file_filter_add_pattern(filter, g_strdup_printf("*.%s", raster[i]));
    gtk_file_filter_add_pattern(filter, g_utf8_strup(g_strdup_printf("*.%s", raster[i]), -1));
  }
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), filter);

  /* RAW ONLY */
  filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("Raw image files"));
  for(int i = 0; i < 46; i++)
  {
    gtk_file_filter_add_pattern(filter, g_strdup_printf("*.%s", raw[i]));
    gtk_file_filter_add_pattern(filter, g_utf8_strup(g_strdup_printf("*.%s", raw[i]), -1));
  }
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), filter);

  // Set RAW ONLY as default
  gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(file_chooser), filter);

  /* RASTER ONLY */
  filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, _("Raster image files"));
  for(int i = 0; i < 14; i++)
  {
    gtk_file_filter_add_pattern(filter, g_strdup_printf("*.%s", raster[i]));
    gtk_file_filter_add_pattern(filter, g_utf8_strup(g_strdup_printf("*.%s", raster[i]), -1));
  }
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(file_chooser), filter);
}

static GtkWidget * _attach_aligned_grid_item(GtkWidget *grid, const int row, const int column,
                                      const char *label, const GtkAlign align, const gboolean fixed_width,
                                      const gboolean full_width)
{
  GtkWidget *w = gtk_label_new(label);
  if(fixed_width)
    gtk_label_set_max_width_chars(GTK_LABEL(w), 25);

  gtk_label_set_ellipsize(GTK_LABEL(w), PANGO_ELLIPSIZE_END);
  gtk_grid_attach(GTK_GRID(grid), w, column, row, full_width ? 2 : 1, 1);
  gtk_label_set_xalign(GTK_LABEL(w), align);
  gtk_widget_set_halign(w, align);
  gtk_label_set_line_wrap(GTK_LABEL(w), TRUE);
  return w;
}

static GtkWidget * _attach_grid_separator(GtkWidget *grid, const int row, const int length)
{
  GtkWidget *w = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_grid_attach(GTK_GRID(grid), w, 0, row, length, 1);
  dt_gui_add_class(w, "grid-separator");
  return w;
}

static int _is_in_library_by_path(const gchar *folder, const char *filename)
{
  int32_t filmroll_id = dt_film_get_id(folder);
  int32_t image_id = dt_image_get_id(filmroll_id, filename);
  if(filmroll_id > -1 && image_id > -1)
    return image_id;
  else
    return -1;
}

static int _is_in_library_by_metadata(GFile *file)
{
  GFileInfo *info = g_file_query_info(file,
                            G_FILE_ATTRIBUTE_STANDARD_NAME ","
                            G_FILE_ATTRIBUTE_TIME_MODIFIED,
                            G_FILE_QUERY_INFO_NONE, NULL, NULL);

  const guint64 datetime = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
  char dtid[DT_DATETIME_EXIF_LENGTH];
  dt_datetime_unix_to_exif(dtid, sizeof(dtid), (const time_t *)&datetime);
  const int res = dt_metadata_already_imported(g_file_info_get_name(info), dtid);
  g_object_unref(info);
  return res;
}

static void
update_preview_cb (GtkFileChooser *file_chooser, gpointer userdata)
{
  dt_lib_import_t *d = (dt_lib_import_t *)userdata;
  char *filename = gtk_file_chooser_get_preview_filename(file_chooser);
  gboolean have_file = (filename != NULL) && filename[0] && g_file_test(filename, G_FILE_TEST_IS_REGULAR);

  /* Get the thumbnail */
  GdkPixbuf *pixbuf = _import_get_thumbnail(filename, DT_PIXEL_APPLY_DPI(180), DT_PIXEL_APPLY_DPI(180));
  gtk_image_set_from_pixbuf(GTK_IMAGE(d->preview), pixbuf);
  gtk_widget_show_all(d->preview);
  if(pixbuf) g_object_unref(pixbuf);

  // Reset everything
  gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_DATETIME_FIELD]), "");
  gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_MODEL_FIELD]), "");
  gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_MAKER_FIELD]), "");
  gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_LENS_FIELD]), "");
  gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_FOCAL_LENS_FIELD]), "");
  gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_EXPOSURE_FIELD]), "");
  gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_INLIB_FIELD]), _("No"));
  gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_PATH_FIELD]), "");

  if(!have_file) return; // Nothing more we can do

  /* Do we already have this picture in library ? */
  gchar *folder = gtk_file_chooser_get_current_folder(file_chooser);
  const int is_path_in_lib = _is_in_library_by_path(folder, filename);
  const int is_metadata_in_lib = _is_in_library_by_metadata(gtk_file_chooser_get_file(file_chooser));
  const gboolean is_in_lib = (is_path_in_lib > -1) || (is_metadata_in_lib > -1);
  g_free(folder);

  /* If alread imported, find out where */
  int imgid = -1;
  if(is_path_in_lib > -1)
    imgid = is_path_in_lib;
  else if(is_metadata_in_lib > -1)
    imgid = is_metadata_in_lib;

  char path[512];
  if(imgid > -1)
  {
    const dt_image_t *img = dt_image_cache_get(darktable.image_cache, imgid, 'r');
    if(img)
    {
      dt_image_film_roll_directory(img, path, sizeof(path));
      dt_image_cache_read_release(darktable.image_cache, img);
    }
  }

  /* Get EXIF info */
  dt_image_t img = { 0 };
  if(!dt_exif_read(&img, filename))
  {
    char datetime[200];
    const gboolean valid = dt_datetime_img_to_local(datetime, sizeof(datetime), &img, FALSE);
    const gchar *exposure_field = g_strdup_printf("%.0f ISO – f/%.1f – %s", img.exif_iso, img.exif_aperture,
                                                  dt_util_format_exposure(img.exif_exposure));
    gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_DATETIME_FIELD]), (valid) ? g_strdup(datetime) : _("N/A"));
    gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_MODEL_FIELD]), g_strdup(img.exif_model));
    gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_MAKER_FIELD]), g_strdup(img.exif_maker));
    gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_LENS_FIELD]), g_strdup(img.exif_lens));
    gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_FOCAL_LENS_FIELD]), g_strdup_printf("%0.f mm", img.exif_focal_length));
    gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_EXPOSURE_FIELD]), exposure_field);
    gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_INLIB_FIELD]), (is_in_lib) ? g_strdup_printf(_("Yes (ID %i), in"), imgid) : _("No"));

    if(is_in_lib)
      gtk_label_set_text(GTK_LABEL(d->exif_info[EXIF_PATH_FIELD]), g_strdup_printf(_("%s"), path));
  }

  g_free(filename);
  gtk_file_chooser_set_preview_widget_active(file_chooser, have_file);
}

static void _update_directory(GtkWidget *file_chooser, dt_lib_import_t *d)
{
  char *path = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(file_chooser));
  dt_conf_set_string("ui_last/import_last_directory", path);
}

static void _copy_toggled_callback(GtkWidget *toggle, GtkGrid *grid)
{
  gboolean state = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
  dt_conf_set_bool("ui_last/import_copy", state);
  gtk_widget_set_visible(GTK_WIDGET(grid), state);
}

static void _base_dir_changed(GtkFileChooserButton* self)
{
  dt_conf_set_string("session/base_directory_pattern", gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(self)));
}

static void _project_dir_changed(GtkWidget *widget, gpointer data)
{
  gchar *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
  dt_conf_set_string("session/sub_directory_pattern", g_strstrip(text));
  g_free(text);
}

static void _filename_changed(GtkWidget *widget, gpointer data)
{
  gchar *text = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
  dt_conf_set_string("session/filename_pattern", g_strstrip(text));
  g_free(text);
}

static void _update_date(GtkCalendar *calendar, GtkWidget *entry)
{
  guint year, month, day;
  gtk_calendar_get_date(calendar, &year, &month, &day);
  GTimeZone *tz = g_time_zone_new_local();

  // Again, GDateTime counts months from 1 but GtkCalendar from 0. Stupid.
  GDateTime *datetime = g_date_time_new(tz, year, month + 1, day, 0, 0, 0.);
  g_time_zone_unref(tz);
  gtk_entry_set_text(GTK_ENTRY(entry), g_date_time_format(datetime, "%F"));
  g_date_time_unref(datetime);
}

/* Validate user input, aka check if date format respects ISO 8601*/
static void _datetime_changed_callback(GtkEntry *entry, dt_lib_import_t *d)
{
  const char *date = gtk_entry_get_text(entry);
  if(date[0])
  {
    char filtered[DT_DATETIME_LENGTH] = { 0 };
    gboolean valid = dt_datetime_entry_to_exif(filtered, sizeof(filtered), date);
    if(!valid)
    {
      gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_SECONDARY, "dialog-error");
      gtk_entry_set_icon_tooltip_text(entry, GTK_ENTRY_ICON_SECONDARY,
                                      _("Date should follow the ISO 8601 format, like :\n"
                                        "YYYY-MM-DD\n"
                                        "YYYY-MM-DD HH:mm\n"
                                        "YYYY-MM-DD HH:mm:ss\n"
                                        "YYYY-MM-DDTHH:mm:ss"));
      return;
    }
    else
    {
      gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_SECONDARY, "");
      gtk_entry_set_icon_tooltip_text(entry, GTK_ENTRY_ICON_SECONDARY, "");
    }
  }
  gtk_entry_set_icon_from_icon_name(entry, GTK_ENTRY_ICON_PRIMARY, NULL);
}

static void _file_activated(GtkFileChooser *chooser, GtkDialog *dialog)
{
  // If we double-click on image and we are not asking to duplicate files, let the filechooser
  // behave as a replacement of lighttable and directly open the image in darkroom.
  if(g_file_test(gtk_file_chooser_get_filename(chooser), G_FILE_TEST_IS_REGULAR)
     && !dt_conf_get_bool("ui_last/import_copy"))
  {
    gtk_dialog_response(dialog, GTK_RESPONSE_ACCEPT);
  }
}

static void _import_from_dialog_new(dt_lib_import_t *d)
{
  d->dialog = gtk_dialog_new_with_buttons
    ( _("Ansel — Open pictures"), NULL, GTK_DIALOG_MODAL,
      _("Cancel"), GTK_RESPONSE_CANCEL,
      _("Open"), GTK_RESPONSE_ACCEPT,
      NULL);

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(d->dialog);
#endif
  gtk_window_set_default_size(GTK_WINDOW(d->dialog),
                              dt_conf_get_int("ui_last/import_dialog_width"),
                              dt_conf_get_int("ui_last/import_dialog_height"));
  gtk_window_set_modal(GTK_WINDOW(d->dialog), FALSE);


  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(d->dialog));
  g_signal_connect(d->dialog, "check-resize", G_CALLBACK(_resize_dialog), NULL);

  /* Grid of options for copy/duplicate */
  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_column_spacing(grid, 0);
  gtk_grid_set_row_spacing(grid, 0);

  /* RIGHT PANEL */
  GtkWidget *rbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_box_pack_start(GTK_BOX(content), rbox, TRUE, TRUE, 0);

  // File browser
  d->file_chooser = gtk_file_chooser_widget_new(GTK_FILE_CHOOSER_ACTION_OPEN);
  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(d->file_chooser), TRUE);
  gtk_file_chooser_set_use_preview_label(GTK_FILE_CHOOSER(d->file_chooser), FALSE);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(d->file_chooser),
                                      dt_conf_get_string("ui_last/import_last_directory"));
  gtk_box_pack_start(GTK_BOX(rbox), d->file_chooser, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(d->file_chooser), "current-folder-changed", G_CALLBACK(_update_directory), NULL);
  g_signal_connect(G_OBJECT(d->file_chooser), "file-activated", G_CALLBACK(_file_activated), GTK_DIALOG(d->dialog));

  // file extension filters
  _file_filters(d->file_chooser);

  // File browser toolbox (extra widgets)
  GtkWidget *toolbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  GtkWidget *select_all = gtk_button_new_with_label(_("Select all"));
  gtk_box_pack_start(GTK_BOX(toolbox), select_all, FALSE, FALSE, 0);
  g_signal_connect(select_all, "clicked", G_CALLBACK(_do_select_all_clicked), d);

  GtkWidget *select_none = gtk_button_new_with_label(_("Select none"));
  gtk_box_pack_start(GTK_BOX(toolbox), select_none, FALSE, FALSE, 0);
  g_signal_connect(select_none, "clicked", G_CALLBACK(_do_select_none_clicked), d);

  GtkWidget *select_new = gtk_button_new_with_label(_("Select new"));
  gtk_box_pack_start(GTK_BOX(toolbox), select_new, FALSE, FALSE, 0);
  g_signal_connect(select_new, "clicked", G_CALLBACK(_do_select_new_clicked), d);
  gtk_widget_set_tooltip_text(select_new,
                              _("Selecting new files targets pictures that have never been added to the library. "
                                "The lookup is done by searching for the original filename and date/time. "
                                "It can detect files existing at another path, under a different name. "
                                "False-positive can arise if two pictures have been taken at the same time with the same name."));

  GtkWidget *copy = gtk_check_button_new_with_label(_("Copy files to a new destination"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(copy), dt_conf_get_bool("ui_last/import_copy"));
  g_signal_connect(G_OBJECT(copy), "toggled", G_CALLBACK(_copy_toggled_callback), (gpointer)grid);

  GtkBox *help_box_copy = attach_help_popover(
      copy,
      _("Use this option if your images are stored on a temporary drive (memory card, USB flash drive) "
        "and you want to duplicate them on a permanent storage.\n\n"
        "You will be able to rename them in batch using the patterns and variables below."));
  gtk_box_pack_start(GTK_BOX(toolbox), GTK_WIDGET(help_box_copy), FALSE, FALSE, 0);
  gtk_widget_set_name(GTK_WIDGET(help_box_copy), "import-help-box-copy");

  gtk_file_chooser_set_extra_widget(GTK_FILE_CHOOSER(d->file_chooser), toolbox);

  // File browser preview box
  // 1. Thumbnail
  GtkWidget *preview_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  d->preview = gtk_image_new();
  gtk_widget_set_size_request(d->preview, DT_PIXEL_APPLY_DPI(240), DT_PIXEL_APPLY_DPI(240));
  gtk_box_pack_start(GTK_BOX(preview_box), d->preview, TRUE, FALSE, 0);

  // 2. Exif metadata
  d->exif = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(d->exif), 1);
  _attach_aligned_grid_item(d->exif, 0, 0, _("Shot :"), GTK_ALIGN_END, FALSE, FALSE);
  _attach_grid_separator(   d->exif, 1, 2);
  _attach_aligned_grid_item(d->exif, 2, 0, _("Camera :"), GTK_ALIGN_END, FALSE, FALSE);
  _attach_aligned_grid_item(d->exif, 3, 0, _("Brand :"), GTK_ALIGN_END, FALSE, FALSE);
  _attach_aligned_grid_item(d->exif, 4, 0, _("Lens :"), GTK_ALIGN_END, FALSE, FALSE);
  _attach_aligned_grid_item(d->exif, 5, 0, _("Focal :"), GTK_ALIGN_END, FALSE, FALSE);
  _attach_grid_separator(   d->exif, 6, 2);
  // exposure trifecta
  _attach_grid_separator(   d->exif, 8, 2);

  GtkWidget *imported_label = gtk_label_new(_("Imported"));
  GtkBox *help_box_inlib = attach_help_popover(
      imported_label,
      _("Images already in the library will not be imported again, selected or not. "
        "Remove them from the library first, or use the menu "
        "`Run` → `Resynchronize library and XMP` to update the local database from distant XMP.\n\n"
        "Ansel indexes images by their filename and parent folder (full path), "
        "not by their content. Therefore, renaming or moving images on the filesystem, "
        "or changing the mounting point of their external drive will make them "
        "look like new (unknown) images.\n\n"
        "If an XMP file is present alongside images, it will be imported as well, "
        "including the metadata and settings stored in it. If it is not what you want, "
        "you can reset metadata in the lighttable."));
  gtk_widget_set_halign(imported_label, GTK_ALIGN_END);
  gtk_grid_attach(GTK_GRID(d->exif), GTK_WIDGET(help_box_inlib), 0, EXIF_INLIB_FIELD, 1, 1);
  //_attach_aligned_grid_item(d->exif, 9, 0, _("Imported :"), GTK_ALIGN_END, FALSE, FALSE);

  d->exif_info[EXIF_DATETIME_FIELD] = _attach_aligned_grid_item(d->exif, 0, 1, "", GTK_ALIGN_START, TRUE, FALSE);
  d->exif_info[EXIF_MODEL_FIELD] = _attach_aligned_grid_item(d->exif, 2, 1, "", GTK_ALIGN_START, TRUE, FALSE);
  d->exif_info[EXIF_MAKER_FIELD] = _attach_aligned_grid_item(d->exif, 3, 1, "", GTK_ALIGN_START, TRUE, FALSE);
  d->exif_info[EXIF_LENS_FIELD] = _attach_aligned_grid_item(d->exif, 4, 1, "", GTK_ALIGN_START, TRUE, FALSE);
  d->exif_info[EXIF_FOCAL_LENS_FIELD] = _attach_aligned_grid_item(d->exif, 5, 1, "", GTK_ALIGN_START, TRUE, FALSE);
  d->exif_info[EXIF_EXPOSURE_FIELD] = _attach_aligned_grid_item(d->exif, 7, 0, "", GTK_ALIGN_CENTER, TRUE, TRUE);
  d->exif_info[EXIF_INLIB_FIELD] = _attach_aligned_grid_item(d->exif, 9, 1, "", GTK_ALIGN_START, FALSE, TRUE);
  d->exif_info[EXIF_PATH_FIELD] = _attach_aligned_grid_item(d->exif, 10, 0, "", GTK_ALIGN_START, FALSE, TRUE);

  gtk_box_pack_start(GTK_BOX(preview_box), d->exif, TRUE, TRUE, 0);
  gtk_widget_show_all(d->exif);

  gtk_file_chooser_set_preview_widget(GTK_FILE_CHOOSER(d->file_chooser), preview_box);
  g_signal_connect(GTK_FILE_CHOOSER(d->file_chooser), "update-preview", G_CALLBACK(update_preview_cb), d);

  // Project date
  GtkWidget *calendar_label = gtk_label_new(_("Project date"));
  gtk_widget_set_halign(calendar_label, GTK_ALIGN_START);
  d->datetime = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(d->datetime), 20);
  g_signal_connect(G_OBJECT(d->datetime), "changed", G_CALLBACK(_datetime_changed_callback), d);

  // Date is inited as today by default
  GDateTime *now = g_date_time_new_now_local();
  gtk_entry_set_text(GTK_ENTRY(d->datetime), g_date_time_format(now, "%F"));

  // Date chooser
  GtkWidget *calendar = gtk_calendar_new();
  // GtkCalendar uses monthes in [0:11]. Glib GDateTime returns monthes in [1:12]. Stupid.
  gtk_calendar_select_month(GTK_CALENDAR(calendar), g_date_time_get_month(now) - 1, g_date_time_get_year(now));
  const guint day = g_date_time_get_day_of_month(now);
  gtk_calendar_select_day(GTK_CALENDAR(calendar), day);
  gtk_calendar_mark_day(GTK_CALENDAR(calendar), day);
  GtkBox *box_calendar = attach_popover(d->datetime, "appointment-new", calendar);
  g_signal_connect(G_OBJECT(calendar), "day-selected", G_CALLBACK(_update_date), d->datetime);

  // free date
  g_date_time_unref(now);

  // Base directory of projects
  GtkWidget *base_label = gtk_label_new(_("Base directory of all projects"));
  gtk_widget_set_halign(base_label, GTK_ALIGN_START);

  GtkWidget *dir_label = gtk_label_new(_("Project directory naming pattern"));
  gtk_widget_set_halign(dir_label, GTK_ALIGN_START);

  GtkWidget *file_label = gtk_label_new(_("File naming pattern"));
  gtk_widget_set_halign(file_label, GTK_ALIGN_START);

  GtkWidget *base_dir
      = gtk_file_chooser_button_new(_("Select a base directory"), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(base_dir), dt_conf_get_string("session/base_directory_pattern"));
  g_signal_connect(G_OBJECT(base_dir), "file-set", G_CALLBACK(_base_dir_changed), NULL);
  gtk_widget_set_hexpand(base_dir, TRUE);

  GtkWidget *sep1 = gtk_label_new("/");
  GtkWidget *sep2 = gtk_label_new("/");

  GtkWidget *project_dir = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(project_dir), dt_conf_get_string("session/sub_directory_pattern"));
  gtk_widget_set_hexpand(project_dir, TRUE);
  dt_gtkentry_setup_completion(GTK_ENTRY(project_dir), dt_gtkentry_get_default_path_compl_list());
  gtk_widget_set_tooltip_text(project_dir, _("Start typing `$(` to see available variables through auto-completion"));
  g_signal_connect(G_OBJECT(project_dir), "changed", G_CALLBACK(_project_dir_changed), d);

  GtkWidget *file = gtk_entry_new();
  gtk_entry_set_text(GTK_ENTRY(file), dt_conf_get_string("session/filename_pattern"));
  gtk_widget_set_hexpand(file, TRUE);
  dt_gtkentry_setup_completion(GTK_ENTRY(file), dt_gtkentry_get_default_path_compl_list());
  g_signal_connect(G_OBJECT(file), "changed", G_CALLBACK(_filename_changed), d);

  /* Create the grid of import params when using duplication */

  // Row 0: labels for text entries
  // Row 1: text entries
  gtk_grid_attach(grid, calendar_label, 0, 0, 1, 1);
  gtk_grid_attach(grid, GTK_WIDGET(box_calendar), 0, 1, 1, 1);

  // create text box with label and attach on grid directly
  dt_gui_preferences_string(grid, "ui_last/import_jobcode", 2, 0);

  // Row 2: separator
  _attach_grid_separator(GTK_WIDGET(grid), 2, 5);

  // Row 3: labels for text entries
  gtk_grid_attach(grid, base_label, 0, 3, 1, 1);
  gtk_grid_attach(grid, dir_label, 2, 3, 1, 1);
  gtk_grid_attach(grid, file_label, 4, 3, 1, 1);

  // Row 4: text entries
  gtk_grid_attach(grid, base_dir, 0, 4, 1, 1);
  gtk_grid_attach(grid, sep1, 1, 4, 1, 1);
  gtk_grid_attach(grid, project_dir, 2, 4, 1, 1);
  gtk_grid_attach(grid, sep2, 3, 4, 1, 1);
  gtk_grid_attach(grid, file, 4, 4, 1, 1);

  gtk_box_pack_end(GTK_BOX(rbox), GTK_WIDGET(grid), FALSE, FALSE, 0);
  gtk_widget_show_all(d->dialog);

  // Duplication parameters visible only if the option is set
  gtk_widget_set_visible(GTK_WIDGET(grid), dt_conf_get_bool("ui_last/import_copy"));
}

static void _do_select_all(dt_lib_import_t *d)
{
  gtk_file_chooser_select_all(GTK_FILE_CHOOSER(d->file_chooser));
}

static void _do_select_none(dt_lib_import_t *d)
{
  gtk_file_chooser_unselect_all(GTK_FILE_CHOOSER(d->file_chooser));
}

static void _do_select_new(dt_lib_import_t *d)
{
  // Twisted Gtk doesn't let us select multiple files.
  // We need to select all then unselect what we don't want.
  gtk_file_chooser_select_all(GTK_FILE_CHOOSER(d->file_chooser));

  gchar *folder = gtk_file_chooser_get_current_folder(GTK_FILE_CHOOSER(d->file_chooser));
  GFileEnumerator *files = g_file_enumerate_children(
      g_file_new_for_path(folder), G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NONE, NULL, NULL);

  // Get the file filter in use
  GtkFileFilter *filter = gtk_file_chooser_get_filter(GTK_FILE_CHOOSER(d->file_chooser));

  GFile *file = NULL;
  while(g_file_enumerator_iterate(files, NULL, &file, NULL, NULL))
  {
    // g_file_enumerator_iterate returns FALSE only on errors, not on end of enumeration.
    // We need an ugly break here else infinite loop.
    if(!file) break;

    GtkFileFilterInfo filter_info = { gtk_file_filter_get_needed(filter),
                                      g_file_get_parse_name(file),
                                      g_file_get_uri(file),
                                      g_file_get_parse_name(file), NULL };

    // We need to act only on files passing the file filter, aka being currently displayed on screen.
    // Unselecting files not displayed in the current list freezes the UI and introduces oddities.
    if(gtk_file_filter_filter(filter, &filter_info)
       && !(g_file_test(g_file_get_path(file), G_FILE_TEST_IS_REGULAR)
            && _is_in_library_by_metadata(file) == -1))
    {
      gtk_file_chooser_unselect_file(GTK_FILE_CHOOSER(d->file_chooser), file);
    }
  }
  g_object_unref(files);
  g_free(folder);
}


static void _import_set_collection(const char *dirname)
{
  if(dirname)
  {
    dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
    dt_conf_set_int("plugins/lighttable/collect/item0", 0);
    dt_conf_set_string("plugins/lighttable/collect/string0", g_strdup_printf("%s/%%", dirname));
    dt_collection_update_query(darktable.collection, DT_COLLECTION_CHANGE_NEW_QUERY, DT_COLLECTION_PROP_UNDEF,
                               NULL);
  }
}

static GSList* _get_subfolder(GFile *folder)
{
  // Get subfolders and files from current folder
  // Put them in a GSList for compatibility with the rest of the API.
  GFileEnumerator *files
      = g_file_enumerate_children(folder, G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
                                  G_FILE_QUERY_INFO_NONE, NULL, NULL);

  GFile *file = NULL;
  GSList *files_list = NULL;
  while(g_file_enumerator_iterate(files, NULL, &file, NULL, NULL))
  {
    // g_file_enumerator_iterate returns FALSE only on errors, not on end of enumeration.
    // We need an ugly break here else infinite loop.
    if(!file) break;
    // *file is destroyed with g_object_unref below, but GFile doesn't support memcpy.
    // We create a new "file" from the path of the read one.
    // Note that a GFile is the abstracted representation of an harddrive file, not the file itself.
    // Creating a new GFile doesn't create I/O on the disk.
    files_list = g_slist_append(files_list, g_file_new_for_path(g_file_get_path(file)));
  }

  g_object_unref(files);

  return files_list;
}

static GList* _recurse_folder(GSList *files, GList *items, GtkFileFilter *filter)
{
  for(GSList *file = files; file; file = g_slist_next(file))
  {
    GFile *document = (GFile *)file->data;
    gchar *pathname = g_file_get_path(document);
    GtkFileFilterInfo filter_info = { gtk_file_filter_get_needed(filter),
                                      g_file_get_parse_name(document),
                                      g_file_get_uri(document),
                                      g_file_get_parse_name(document), NULL };

    // Check that document is a real file (not directory) and it passes the type check defined by user in GUI filters.
    // gtk_file_chooser_get_files() applies the filters on the first level of recursivity,
    // so this test is only useful for the next levels if folders are selected at the first level.
    if(g_file_test(pathname, G_FILE_TEST_IS_REGULAR) && gtk_file_filter_filter(filter, &filter_info))
    {
      items = g_list_prepend(items, pathname);
      // prepend is more efficient than append. Import control reorders alphabetically anyway.
    }
    else if(g_file_test(pathname, G_FILE_TEST_IS_DIR))
    {
      GSList *children = _get_subfolder(document);
      items = _recurse_folder(children, items, filter);
      g_slist_free_full(children, g_object_unref);
    }
  }
  return items;
}


static void _import_from_dialog_run(dt_lib_import_t *d)
{
  // Import params
  const gboolean duplicate = dt_conf_get_bool("ui_last/import_copy");
  char datetime_override[DT_DATETIME_LENGTH] = { 0 };

  if(duplicate)
  {
    // Abort early if date format is wrong
    const char *entry = gtk_entry_get_text(GTK_ENTRY(d->datetime));
    if(entry[0] && !dt_datetime_entry_to_exif(datetime_override, sizeof(datetime_override), entry))
    {
      dt_control_log(_("invalid date/time format for import"));
      return;
    }
  }

  // Get the file filter in use
  GtkFileFilter *filter = gtk_file_chooser_get_filter(GTK_FILE_CHOOSER(d->file_chooser));

  // Get selected files and subfolders
  GSList *files = gtk_file_chooser_get_files(GTK_FILE_CHOOSER(d->file_chooser));
  if(g_slist_length(files) == 0) return;
  GList *items = NULL;
  items = _recurse_folder(files, items, filter);
  g_slist_free(files);

  if(items)
  {
    const int num_elem = g_list_length(items);

    // WARNING: the GList items is freed in control import,
    // everything needs to be accessed before.
    dt_control_import(items, datetime_override, !duplicate);

    if(!duplicate) _import_set_collection(dt_conf_get_string("ui_last/import_last_directory"));
    // else : collection set by import job.

    dt_view_filter_reset(darktable.view_manager, TRUE);

    const int imgid = dt_conf_get_int("ui_last/import_last_image");
    if(num_elem == 1 && imgid != -1)
    {
      dt_control_set_mouse_over_id(imgid);
      dt_selection_select_single(darktable.selection, imgid);
      dt_ctl_switch_mode_to("darkroom");
    }
  }
  else
  {
    dt_control_log(_("No files to import. Check your selection."));
  }
}

void dt_images_import()
{
  dt_lib_import_t *d = malloc(sizeof(dt_lib_import_t));
  _import_from_dialog_new(d);
  switch(gtk_dialog_run(GTK_DIALOG(d->dialog)))
  {
    case GTK_RESPONSE_ACCEPT:
      _import_from_dialog_run(d);
      break;
    default:
      break;
  }
  gtk_widget_destroy(d->dialog);
  free(d);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
