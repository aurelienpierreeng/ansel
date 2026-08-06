/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Guillaume STUTIN.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.
*/

#include "views/dev_toolbox.h"

#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/file_location.h"
#include "common/usermanual_url.h"
#include "control/conf.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "develop/dev_pixelpipe.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

#include <limits.h>

#define DT_DEV_TOOLBOX_BUTTON_TYPE_KEY "dt-dev-toolbox-button-type"

void dt_dev_toolbox_apply_iso_12646_size(dt_develop_t *dev)
{
  if(dev->iso_12646.enabled)
  {
    // For ISO 12646, we want portraits and landscapes to cover roughly the same surface
    // no matter the size of the widget. Meaning we force them to fit a square
    // of length matching the smaller widget dimension. The goal is to leave
    // a consistent perceptual impression between pictures, independent from orientation.
    const int main_dim = MIN(dev->roi.orig_width, dev->roi.orig_height);
    dev->roi.border_size = 0.125 * main_dim;
  }
  else
  {
    dev->roi.border_size = DT_PIXEL_APPLY_DPI(dt_conf_get_int("plugins/darkroom/ui/border_size"));
  }

  dt_dev_configure(dev, dev->roi.orig_width - 2 * dev->roi.border_size, dev->roi.orig_height - 2 * dev->roi.border_size);
}

/*
 * Popover show/anchor plumbing, generic: reusable for any button+popover
 * pair, including ones outside the button set below (darkroom's guides and
 * auto-set popovers use this too).
 */

#define DT_DEV_TOOLBOX_PRESHOW_FN_KEY "dt-dev-toolbox-popover-preshow-fn"
#define DT_DEV_TOOLBOX_PRESHOW_DATA_KEY "dt-dev-toolbox-popover-preshow-data"

void dt_dev_toolbox_popover_set_preshow(GtkWidget *popover, void (*preshow)(gpointer user_data), gpointer user_data)
{
  g_object_set_data(G_OBJECT(popover), DT_DEV_TOOLBOX_PRESHOW_FN_KEY, (gpointer)preshow);
  g_object_set_data(G_OBJECT(popover), DT_DEV_TOOLBOX_PRESHOW_DATA_KEY, user_data);
}

gboolean dt_dev_toolbox_show_popup(gpointer user_data)
{
  GtkPopover *popover = GTK_POPOVER(user_data);

  GtkWidget *button = gtk_popover_get_relative_to(popover);
  GdkRectangle button_rect = { 0 };
  GtkWidget *anchor = dt_gui_get_popup_relative_widget(button, &button_rect);
  GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));

  int x, y;
  GdkWindow *pointer_window = gdk_device_get_window_at_position(pointer, &x, &y);
  gpointer pointer_widget = NULL;
  if(pointer_window) gdk_window_get_user_data(pointer_window, &pointer_widget);

  gtk_popover_set_relative_to(popover, anchor ? anchor : button);

  GdkRectangle rect = { button_rect.x + button_rect.width / 2, button_rect.y, 1, 1 };

  if(pointer_widget == anchor)
  {
    rect.x = x;
    rect.y = y;
  }
  else if(pointer_widget && anchor && pointer_widget != anchor)
  {
    gtk_widget_translate_coordinates(pointer_widget, anchor, x, y, &rect.x, &rect.y);
  }

  gtk_popover_set_pointing_to(popover, &rect);

  void (*preshow)(gpointer) = (void (*)(gpointer))g_object_get_data(G_OBJECT(popover), DT_DEV_TOOLBOX_PRESHOW_FN_KEY);
  if(preshow) preshow(g_object_get_data(G_OBJECT(popover), DT_DEV_TOOLBOX_PRESHOW_DATA_KEY));

  gtk_widget_show_all(GTK_WIDGET(popover));

  // cancel glib timeout if invoked by long button press
  return FALSE;
}

static gboolean _quickbutton_press_release(GtkWidget *button, GdkEventButton *event, GtkWidget *popover)
{
  static guint start_time = 0;

  int delay = 0;
  g_object_get(gtk_settings_get_default(), "gtk-long-press-time", &delay, NULL);

  if((event->type == GDK_BUTTON_PRESS && event->button == 3)
     || (event->type == GDK_BUTTON_RELEASE && event->time - start_time > delay))
  {
    gtk_popover_set_relative_to(GTK_POPOVER(popover), button);
    g_object_set(G_OBJECT(popover), "transitions-enabled", FALSE, NULL);

    dt_dev_toolbox_show_popup(popover);
    return TRUE;
  }
  else
  {
    start_time = event->time;
    return FALSE;
  }
}

void dt_dev_toolbox_connect_popover(GtkWidget *button, GtkWidget *popover)
{
  g_signal_connect(button, "button-press-event", G_CALLBACK(_quickbutton_press_release), popover);
  g_signal_connect(button, "button-release-event", G_CALLBACK(_quickbutton_press_release), popover);
}

/*
 * Accelerators: forward keyboard activation to the existing Gtk buttons so
 * the keyboard path reuses the exact same callbacks, state changes and
 * popover anchoring as the pointer path. Generic — reused for guides and
 * auto-set (outside the button set below) by whichever view owns them.
 */

gboolean dt_dev_toolbox_activate_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                       GdkModifierType modifier, gpointer data)
{
  GtkWidget *button = GTK_WIDGET(data);
  if(IS_NULL_PTR(button) || !gtk_widget_is_visible(button) || !gtk_widget_is_sensitive(button)) return FALSE;

  gtk_button_clicked(GTK_BUTTON(button));
  return TRUE;
}

gboolean dt_dev_toolbox_focus_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                    GdkModifierType modifier, gpointer data)
{
  GtkWidget *button = GTK_WIDGET(data);
  if(IS_NULL_PTR(button) || !gtk_widget_is_visible(button) || !gtk_widget_is_sensitive(button)) return FALSE;

  GtkWidget *popover = g_object_get_data(G_OBJECT(button), DT_DEV_TOOLBOX_POPOVER_KEY);
  if(IS_NULL_PTR(popover) || !gtk_widget_is_sensitive(popover)) return FALSE;

  gtk_widget_grab_focus(button);
  gtk_popover_set_relative_to(GTK_POPOVER(popover), button);
  dt_dev_toolbox_show_popup(popover);
  return TRUE;
}

void dt_dev_toolbox_add_accels(dt_develop_t *dev, GtkAccelGroup *accel_group, const char *category,
                               const dt_dev_toolbox_button_t *buttons, size_t n_buttons)
{
  for(size_t i = 0; i < n_buttons; i++)
  {
    GtkWidget *button = NULL;
    const char *activate_label = NULL;
    const char *focus_label = NULL;

    switch(buttons[i])
    {
      case DT_DEV_TOOLBOX_ISO_12646:
        button = dev->iso_12646.button;
        activate_label = N_("Toggle ISO 12646 color assessment conditions");
        break;

      case DT_DEV_TOOLBOX_OVEREXPOSED:
        button = dev->overexposed.button;
        activate_label = N_("Toggle clipping indication");
        focus_label = N_("Focus clipping indication options");
        break;

      case DT_DEV_TOOLBOX_RAWOVEREXPOSED:
        button = dev->rawoverexposed.button;
        activate_label = N_("Toggle raw over exposed indication");
        focus_label = N_("Focus raw over exposed indication options");
        break;

      case DT_DEV_TOOLBOX_SOFTPROOF:
        button = dev->profile.softproof_button;
        activate_label = N_("Toggle softproofing");
        focus_label = N_("Focus softproof options");
        break;

      case DT_DEV_TOOLBOX_GAMUT:
        button = dev->profile.gamut_button;
        activate_label = N_("Toggle gamut checking");
        focus_label = N_("Focus gamut checking options");
        break;

      case DT_DEV_TOOLBOX_DISPLAY:
        button = dev->display.button;
        focus_label = N_("Focus picture display options");
        break;
    }

    if(activate_label)
      dt_accels_new_action_shortcut(dt_gui_get_accels(), dt_dev_toolbox_activate_accel, button, accel_group,
                                    category, activate_label, 0, 0, FALSE, _("Triggers the action"));
    if(focus_label)
      dt_accels_new_action_shortcut(dt_gui_get_accels(), dt_dev_toolbox_focus_accel, button, accel_group,
                                    category, focus_label, 0, 0, FALSE, _("Shows the options popover"));
  }
}

/*
 * Single click dispatcher for every button below: which one fired is read
 * back from a tag set at creation time, instead of one callback per button.
 */

static void _button_clicked(GtkWidget *w, gpointer user_data);

/* Keep both buttons' active state in sync with dt_colorspaces_get_global()->mode,
   since enabling one implicitly disables the other. */
static void _update_softproof_gamut_checking(dt_develop_t *dev)
{
  g_signal_handlers_block_by_func(dev->profile.softproof_button, _button_clicked, dev);
  g_signal_handlers_block_by_func(dev->profile.gamut_button, _button_clicked, dev);

  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->profile.softproof_button),
                               dt_colorspaces_get_global()->mode == DT_PROFILE_SOFTPROOF);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dev->profile.gamut_button),
                               dt_colorspaces_get_global()->mode == DT_PROFILE_GAMUTCHECK);

  g_signal_handlers_unblock_by_func(dev->profile.softproof_button, _button_clicked, dev);
  g_signal_handlers_unblock_by_func(dev->profile.gamut_button, _button_clicked, dev);
}

static void _button_clicked(GtkWidget *w, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  const dt_dev_toolbox_button_t type
      = (dt_dev_toolbox_button_t)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), DT_DEV_TOOLBOX_BUTTON_TYPE_KEY));

  switch(type)
  {
    case DT_DEV_TOOLBOX_ISO_12646:
      if(!dev->gui_attached) return;
      dev->iso_12646.enabled = !dev->iso_12646.enabled;
      dt_dev_toolbox_apply_iso_12646_size(dev);
      // This is already called in apply_iso_12646_size but it's not enough
      dt_control_queue_redraw_center();
      break;

    case DT_DEV_TOOLBOX_OVEREXPOSED:
      dev->overexposed.enabled = !dev->overexposed.enabled;
      dt_dev_pixelpipe_resync_history_main(dev);
      break;

    case DT_DEV_TOOLBOX_RAWOVEREXPOSED:
      dev->rawoverexposed.enabled = !dev->rawoverexposed.enabled;
      dt_dev_pixelpipe_resync_history_main(dev);
      break;

    case DT_DEV_TOOLBOX_SOFTPROOF:
      dt_colorspaces_get_global()->mode
          = (dt_colorspaces_get_global()->mode == DT_PROFILE_SOFTPROOF) ? DT_PROFILE_NORMAL : DT_PROFILE_SOFTPROOF;
      _update_softproof_gamut_checking(dev);
      dt_dev_pixelpipe_resync_history_main(dev);
      break;

    case DT_DEV_TOOLBOX_GAMUT:
      dt_colorspaces_get_global()->mode
          = (dt_colorspaces_get_global()->mode == DT_PROFILE_GAMUTCHECK) ? DT_PROFILE_NORMAL : DT_PROFILE_GAMUTCHECK;
      _update_softproof_gamut_checking(dev);
      dt_dev_pixelpipe_resync_history_main(dev);
      break;

    case DT_DEV_TOOLBOX_DISPLAY:
      // No toggle state: this button only anchors an options popover, attached by the caller.
      break;
  }
}

/*
 * Popover content: generic controls only. View-specific extras (darkroom's
 * rendering-size and mask-preview-checkerboard additions to Picture
 * display) are packed into the returned box by the caller afterward.
 */

static void _overexposed_mode_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dev->overexposed.mode = dt_bauhaus_combobox_get(slider);
  if(dev->overexposed.enabled == FALSE) gtk_button_clicked(GTK_BUTTON(dev->overexposed.button));
  dt_dev_pixelpipe_update_history_main(dev);
}

static void _overexposed_colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dev->overexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(dev->overexposed.enabled == FALSE) gtk_button_clicked(GTK_BUTTON(dev->overexposed.button));
  dt_dev_pixelpipe_resync_history_main(dev);
}

static void _overexposed_lower_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dev->overexposed.lower = dt_bauhaus_slider_get(slider);
  if(dev->overexposed.enabled == FALSE) gtk_button_clicked(GTK_BUTTON(dev->overexposed.button));
  dt_dev_pixelpipe_resync_history_main(dev);
}

static void _overexposed_upper_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dev->overexposed.upper = dt_bauhaus_slider_get(slider);
  if(dev->overexposed.enabled == FALSE) gtk_button_clicked(GTK_BUTTON(dev->overexposed.button));
  dt_dev_pixelpipe_resync_history_main(dev);
}

static void _build_overexposed_popover(dt_develop_t *dev)
{
  dt_gui_add_help_link(dev->overexposed.button, dt_get_help_url("overexposed"));

  dev->overexposed.floating_window = gtk_popover_new(dev->overexposed.button);
  dt_dev_toolbox_connect_popover(dev->overexposed.button, dev->overexposed.floating_window);
  g_object_set_data(G_OBJECT(dev->overexposed.button), DT_DEV_TOOLBOX_POPOVER_KEY, dev->overexposed.floating_window);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(dev->overexposed.floating_window), vbox);

  GtkWidget *mode;
  DT_BAUHAUS_COMBOBOX_NEW_FULL(dt_bauhaus_get_global(), mode, NULL, N_("clipping preview mode"),
                               _("select the metric you want to preview\nfull gamut is the combination of all other modes"),
                               dev->overexposed.mode, _overexposed_mode_callback, dev,
                               N_("full gamut"), N_("any RGB channel"), N_("luminance only"), N_("saturation only"));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

  GtkWidget *colorscheme;
  DT_BAUHAUS_COMBOBOX_NEW_FULL(dt_bauhaus_get_global(), colorscheme, NULL, N_("color scheme"),
                               _("select colors to indicate clipping"), dev->overexposed.colorscheme,
                               _overexposed_colorscheme_callback, dev, N_("black & white"), N_("red & blue"),
                               N_("purple & green"));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);

  GtkWidget *lower = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), -32., -4., 1., -12.69, 2);
  dt_bauhaus_slider_set(lower, dev->overexposed.lower);
  dt_bauhaus_slider_set_format(lower, _(" EV"));
  dt_bauhaus_widget_set_label(lower, N_("lower threshold"));
  gtk_widget_set_tooltip_text(lower, _("clipping threshold for the black point,\n"
                                       "in EV, relatively to white (0 EV).\n"
                                       "8 bits sRGB clips blacks at -12.69 EV,\n"
                                       "8 bits Adobe RGB clips blacks at -19.79 EV,\n"
                                       "16 bits sRGB clips blacks at -20.69 EV,\n"
                                       "typical fine-art mat prints produce black at -5.30 EV,\n"
                                       "typical color glossy prints produce black at -8.00 EV,\n"
                                       "typical B&W glossy prints produce black at -9.00 EV."));
  g_signal_connect(G_OBJECT(lower), "value-changed", G_CALLBACK(_overexposed_lower_callback), dev);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(lower), TRUE, TRUE, 0);

  GtkWidget *upper = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), 0.0, 100.0, 0.1, 99.99, 2);
  dt_bauhaus_slider_set(upper, dev->overexposed.upper);
  dt_bauhaus_slider_set_format(upper, "%");
  dt_bauhaus_widget_set_label(upper, N_("upper threshold"));
  /* xgettext:no-c-format */
  gtk_widget_set_tooltip_text(upper, _("clipping threshold for the white point.\n100% is peak medium luminance."));
  g_signal_connect(G_OBJECT(upper), "value-changed", G_CALLBACK(_overexposed_upper_callback), dev);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(upper), TRUE, TRUE, 0);

  gtk_widget_show_all(vbox);
}

static void _rawoverexposed_mode_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dev->rawoverexposed.mode = dt_bauhaus_combobox_get(combo);
  if(dev->rawoverexposed.enabled == FALSE) gtk_button_clicked(GTK_BUTTON(dev->rawoverexposed.button));
  dt_dev_pixelpipe_resync_history_main(dev);
}

static void _rawoverexposed_colorscheme_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dev->rawoverexposed.colorscheme = dt_bauhaus_combobox_get(combo);
  if(dev->rawoverexposed.enabled == FALSE) gtk_button_clicked(GTK_BUTTON(dev->rawoverexposed.button));
  dt_dev_pixelpipe_resync_history_main(dev);
}

static void _rawoverexposed_threshold_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dev->rawoverexposed.threshold = dt_bauhaus_slider_get(slider);
  if(dev->rawoverexposed.enabled == FALSE) gtk_button_clicked(GTK_BUTTON(dev->rawoverexposed.button));
  dt_dev_pixelpipe_resync_history_main(dev);
}

static void _build_rawoverexposed_popover(dt_develop_t *dev)
{
  dt_gui_add_help_link(dev->rawoverexposed.button, dt_get_help_url("rawoverexposed"));

  dev->rawoverexposed.floating_window = gtk_popover_new(dev->rawoverexposed.button);
  dt_dev_toolbox_connect_popover(dev->rawoverexposed.button, dev->rawoverexposed.floating_window);
  g_object_set_data(G_OBJECT(dev->rawoverexposed.button), DT_DEV_TOOLBOX_POPOVER_KEY,
                    dev->rawoverexposed.floating_window);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(dev->rawoverexposed.floating_window), vbox);

  GtkWidget *mode;
  DT_BAUHAUS_COMBOBOX_NEW_FULL(dt_bauhaus_get_global(), mode, NULL, N_("mode"), _("select how to mark the clipped pixels"),
                               dev->rawoverexposed.mode, _rawoverexposed_mode_callback, dev,
                               N_("mark with CFA color"), N_("mark with solid color"), N_("false color"));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(mode), TRUE, TRUE, 0);

  // FIXME can't use DT_BAUHAUS_COMBOBOX_NEW_FULL because of (unnecessary?) translation context
  GtkWidget *colorscheme = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(colorscheme, N_("color scheme"));
  dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "red"));
  dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "green"));
  dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "blue"));
  dt_bauhaus_combobox_add(colorscheme, C_("solidcolor", "black"));
  dt_bauhaus_combobox_set(colorscheme, dev->rawoverexposed.colorscheme);
  gtk_widget_set_tooltip_text(
      colorscheme, _("select the solid color to indicate over exposure.\nwill only be used if mode = mark with solid color"));
  g_signal_connect(G_OBJECT(colorscheme), "value-changed", G_CALLBACK(_rawoverexposed_colorscheme_callback), dev);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(colorscheme), TRUE, TRUE, 0);

  GtkWidget *threshold = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), 0.0, 2.0, 0.01, 1.0, 3);
  dt_bauhaus_slider_set(threshold, dev->rawoverexposed.threshold);
  dt_bauhaus_widget_set_label(threshold, N_("clipping threshold"));
  gtk_widget_set_tooltip_text(threshold,
                              _("threshold of what shall be considered overexposed\n1.0 - white level\n0.0 - black level"));
  g_signal_connect(G_OBJECT(threshold), "value-changed", G_CALLBACK(_rawoverexposed_threshold_callback), dev);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(threshold), TRUE, TRUE, 0);

  gtk_widget_show_all(vbox);
}

static void _softproof_profile_callback(GtkWidget *combo, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  gboolean profile_changed = FALSE;
  const int pos = dt_bauhaus_combobox_get(combo);
  for(GList *profiles = dt_colorspaces_get_global()->profiles; profiles; profiles = g_list_next(profiles))
  {
    dt_colorspaces_color_profile_t *pp = (dt_colorspaces_color_profile_t *)profiles->data;
    if(pp->out_pos == pos)
    {
      if(dt_colorspaces_get_global()->softproof_type != pp->type
        || (dt_colorspaces_get_global()->softproof_type == DT_COLORSPACE_FILE
            && strcmp(dt_colorspaces_get_global()->softproof_filename, pp->filename)))
      {
        dt_colorspaces_get_global()->softproof_type = pp->type;
        g_strlcpy(dt_colorspaces_get_global()->softproof_filename, pp->filename,
                 sizeof(dt_colorspaces_get_global()->softproof_filename));
        profile_changed = TRUE;
      }
      goto end;
    }
  }

  // profile not found, fall back to sRGB. shouldn't happen
  fprintf(stderr, "can't find softproof profile `%s', using sRGB instead\n", dt_bauhaus_combobox_get_text(combo));
  profile_changed = dt_colorspaces_get_global()->softproof_type != DT_COLORSPACE_SRGB;
  dt_colorspaces_get_global()->softproof_type = DT_COLORSPACE_SRGB;
  dt_colorspaces_get_global()->softproof_filename[0] = '\0';

end:
  if(profile_changed)
  {
    DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                                  DT_COLORSPACES_PROFILE_TYPE_SOFTPROOF);
    dt_dev_pixelpipe_resync_history_main(dev);
  }
}

static void _build_softproof_gamut_popover(dt_develop_t *dev)
{
  dt_gui_add_help_link(dev->profile.softproof_button, dt_get_help_url("softproof"));
  dt_gui_add_help_link(dev->profile.gamut_button, dt_get_help_url("gamut"));

  // the popup window, shared between the two profile buttons
  dev->profile.floating_window = gtk_popover_new(NULL);
  dt_dev_toolbox_connect_popover(dev->profile.softproof_button, dev->profile.floating_window);
  dt_dev_toolbox_connect_popover(dev->profile.gamut_button, dev->profile.floating_window);
  g_object_set_data(G_OBJECT(dev->profile.softproof_button), DT_DEV_TOOLBOX_POPOVER_KEY, dev->profile.floating_window);
  g_object_set_data(G_OBJECT(dev->profile.gamut_button), DT_DEV_TOOLBOX_POPOVER_KEY, dev->profile.floating_window);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_container_add(GTK_CONTAINER(dev->profile.floating_window), vbox);

  char datadir[PATH_MAX] = { 0 };
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  dt_loc_get_datadir(datadir, sizeof(datadir));

  GtkWidget *softproof_profile = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL));
  dt_bauhaus_widget_set_label(softproof_profile, N_("softproof profile"));
  dt_bauhaus_combobox_set_entries_ellipsis(softproof_profile, PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_pack_start(GTK_BOX(vbox), softproof_profile, TRUE, TRUE, 0);

  for(const GList *l = dt_colorspaces_get_global()->profiles; l; l = g_list_next(l))
  {
    dt_colorspaces_color_profile_t *prof = (dt_colorspaces_color_profile_t *)l->data;
    // the system display profile is only suitable for display purposes
    if(prof->out_pos > -1)
    {
      dt_bauhaus_combobox_add(softproof_profile, prof->name);
      if(prof->type == dt_colorspaces_get_global()->softproof_type
        && (prof->type != DT_COLORSPACE_FILE || !strcmp(prof->filename, dt_colorspaces_get_global()->softproof_filename)))
        dt_bauhaus_combobox_set(softproof_profile, prof->out_pos);
    }
  }

  char *system_profile_dir = g_build_filename(datadir, "color", "out", NULL);
  char *user_profile_dir = g_build_filename(confdir, "color", "out", NULL);
  char *tooltip = g_strdup_printf(_("softproof ICC profiles in %s or %s"), user_profile_dir, system_profile_dir);
  gtk_widget_set_tooltip_text(softproof_profile, tooltip);
  dt_free(tooltip);
  dt_free(system_profile_dir);
  dt_free(user_profile_dir);

  g_signal_connect(G_OBJECT(softproof_profile), "value-changed", G_CALLBACK(_softproof_profile_callback), dev);

  gtk_widget_show_all(vbox);
}

static void _display_brightness_callback(GtkWidget *slider, gpointer user_data)
{
  dt_conf_set_int("display/brightness", (int)(dt_bauhaus_slider_get(slider)));
  dt_control_queue_redraw_center();
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_DARKROOM_UI_CHANGED);
}

static void _display_borders_callback(GtkWidget *slider, gpointer user_data)
{
  dt_develop_t *dev = (dt_develop_t *)user_data;
  dt_conf_set_int("plugins/darkroom/ui/border_size", (int)dt_bauhaus_slider_get(slider));
  dt_dev_toolbox_apply_iso_12646_size(dev);
  dt_dev_pixelpipe_change_zoom_main(dev);
  DT_DEBUG_CONTROL_SIGNAL_RAISE(dt_control_signal_get_global(), DT_SIGNAL_DARKROOM_UI_CHANGED);
}

static void _build_display_popover(dt_develop_t *dev)
{
  dev->display.floating_window = gtk_popover_new(dev->display.button);
  dt_dev_toolbox_connect_popover(dev->display.button, dev->display.floating_window);
  g_object_set_data(G_OBJECT(dev->display.button), DT_DEV_TOOLBOX_POPOVER_KEY, dev->display.floating_window);

  GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
  gtk_widget_set_margin_bottom(vbox, DT_PIXEL_APPLY_DPI(DT_GUI_BOX_SPACING));
  gtk_container_add(GTK_CONTAINER(dev->display.floating_window), vbox);

  GtkWidget *brightness = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), 0, 100, 5, 50, 0);
  dt_bauhaus_slider_set(brightness, (int)dt_conf_get_int("display/brightness"));
  dt_bauhaus_widget_set_label(brightness, N_("Background brightness"));
  dt_bauhaus_slider_set_format(brightness, "%");
  g_signal_connect(G_OBJECT(brightness), "value-changed", G_CALLBACK(_display_brightness_callback), dev);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(brightness), TRUE, TRUE, 0);

  GtkWidget *borders = dt_bauhaus_slider_new_with_range(dt_bauhaus_get_global(), DT_GUI_MODULE(NULL), 0, 250, 5, 10, 0);
  dt_bauhaus_slider_set(borders, dt_conf_get_int("plugins/darkroom/ui/border_size"));
  dt_bauhaus_widget_set_label(borders, N_("Picture margins"));
  dt_bauhaus_slider_set_format(borders, "px");
  g_signal_connect(G_OBJECT(borders), "value-changed", G_CALLBACK(_display_borders_callback), dev);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(borders), TRUE, TRUE, 0);

  // Left un-shown: callers wanting view-specific extras (darkroom's rendering
  // size and mask preview checkerboard) pack them into this same vbox and
  // call gtk_widget_show_all() themselves once done.
}

/*
 * Button creation.
 */

static GtkWidget *_create_one_button(dt_develop_t *dev, dt_view_type_flags_t views, dt_dev_toolbox_button_t type)
{
  GtkWidget *button = NULL;
  const char *tooltip = NULL;

  switch(type)
  {
    case DT_DEV_TOOLBOX_ISO_12646:
      button = dtgtk_togglebutton_new(dtgtk_cairo_paint_bulb, 0, NULL);
      tooltip = _("toggle ISO 12646 color assessment conditions");
      dev->iso_12646.button = button;
      break;

    case DT_DEV_TOOLBOX_OVEREXPOSED:
      button = dtgtk_togglebutton_new(dtgtk_cairo_paint_overexposed, 0, NULL);
      tooltip = _("toggle clipping indication\nright click for options");
      dev->overexposed.button = button;
      break;

    case DT_DEV_TOOLBOX_RAWOVEREXPOSED:
      button = dtgtk_togglebutton_new(dtgtk_cairo_paint_rawoverexposed, 0, NULL);
      tooltip = _("toggle raw over exposed indication\nright click for options");
      dev->rawoverexposed.button = button;
      break;

    case DT_DEV_TOOLBOX_SOFTPROOF:
      button = dtgtk_togglebutton_new(dtgtk_cairo_paint_softproof, 0, NULL);
      tooltip = _("toggle softproofing\nright click for profile options");
      dev->profile.softproof_button = button;
      break;

    case DT_DEV_TOOLBOX_GAMUT:
      button = dtgtk_togglebutton_new(dtgtk_cairo_paint_gamut_check, 0, NULL);
      tooltip = _("toggle gamut checking\nright click for profile options");
      dev->profile.gamut_button = button;
      break;

    case DT_DEV_TOOLBOX_DISPLAY:
      button = dtgtk_button_new(dtgtk_cairo_paint_display, 0, NULL);
      tooltip = _("Picture display options");
      dev->display.button = button;
      break;
  }

  g_object_set_data(G_OBJECT(button), DT_DEV_TOOLBOX_BUTTON_TYPE_KEY, GINT_TO_POINTER(type));
  gtk_widget_set_tooltip_text(button, tooltip);
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_button_clicked), dev);
  dt_view_manager_module_toolbox_add(dt_view_manager_get_global(), button, views);
  return button;
}

void dt_dev_toolbox_create(dt_develop_t *dev, dt_view_type_flags_t views, const dt_dev_toolbox_button_t *buttons,
                           size_t n_buttons)
{
  gboolean has_softproof = FALSE;
  gboolean has_gamut = FALSE;

  for(size_t i = 0; i < n_buttons; i++)
  {
    _create_one_button(dev, views, buttons[i]);
    // Popover options
    switch(buttons[i])
    {
      case DT_DEV_TOOLBOX_ISO_12646:
        // No options popover for this one: it's a plain toggle.
        break;
      case DT_DEV_TOOLBOX_SOFTPROOF:
        has_softproof = TRUE;
        break;
      case DT_DEV_TOOLBOX_GAMUT:
        has_gamut = TRUE;
        break;
      case DT_DEV_TOOLBOX_OVEREXPOSED:
        _build_overexposed_popover(dev);
        break;
      case DT_DEV_TOOLBOX_RAWOVEREXPOSED:
        _build_rawoverexposed_popover(dev);
        break;
      case DT_DEV_TOOLBOX_DISPLAY:
        _build_display_popover(dev);
        break;
    }
  }

  // Both buttons exist only once both are requested in the same call.
  if(has_softproof && has_gamut)
  {
    _update_softproof_gamut_checking(dev);
    _build_softproof_gamut_popover(dev);
  }
}
