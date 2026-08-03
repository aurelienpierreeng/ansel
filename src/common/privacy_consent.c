/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/privacy_consent.h"
#include "common/darktable.h"

#if defined(HAVE_SENTRY) || defined(HAVE_TELEMETRY)

#include "control/conf.h"
#include "gui/gtk.h"

// Non-confgen sentinel: its presence means the user has already been asked. The
// per-feature toggles (sentry/enabled, telemetry/enabled) are confgen keys shown
// in Preferences ▸ Storage ▸ Privacy, so they can be changed later.
#define DT_PRIVACY_ASKED_KEY "privacy/consent_asked"

// User-facing documentation describing exactly what is collected, where it goes
// and why. Linked from the consent dialog so the decision is informed.
#define DT_PRIVACY_DOC_URL "https://ansel.photos/en/doc/data-privacy/"

void dt_privacy_ask_consent(const gboolean have_gui)
{
  // Already decided once: never ask again (toggles live in Preferences).
  if(dt_conf_key_exists(DT_PRIVACY_ASKED_KEY)) return;

  // Without a GUI we cannot prompt; leave both features at their (off) defaults
  // until the user is shown the dialog on a future GUI launch.
  if(!have_gui) return;

  GtkWidget *parent = (darktable.gui && darktable.gui->ui) ? dt_ui_main_window(darktable.gui->ui) : NULL;

  GtkWidget *dialog = gtk_dialog_new_with_buttons(
      _("Help us improve Ansel"), GTK_WINDOW(parent),
      GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
      _("Confirm choices"), GTK_RESPONSE_ACCEPT, NULL);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

  GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
  gtk_box_set_spacing(GTK_BOX(content), DT_PIXEL_APPLY_DPI(8));
  gtk_container_set_border_width(GTK_CONTAINER(content), DT_PIXEL_APPLY_DPI(12));

  GtkWidget *intro = gtk_label_new(
      _("Ansel can share anonymous data with its developers to help fix bugs and "
        "decide what to work on. This is entirely optional, separate for each purpose, "
        "and can be changed any time in Preferences ▸ Storage ▸ Privacy.\n\n"
        "We never send your images, file names or any personal data."));
  gtk_label_set_line_wrap(GTK_LABEL(intro), TRUE);
  gtk_label_set_xalign(GTK_LABEL(intro), 0.0);
  gtk_label_set_max_width_chars(GTK_LABEL(intro), 64);
  gtk_box_pack_start(GTK_BOX(content), intro, FALSE, FALSE, 0);

#ifdef HAVE_SENTRY
  GtkWidget *crash_check = gtk_check_button_new_with_label(
      _("Send crash reports (backtrace, OS and hardware specs, app version)"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(crash_check), TRUE);
  gtk_box_pack_start(GTK_BOX(content), crash_check, FALSE, FALSE, 0);
#endif

#ifdef HAVE_TELEMETRY
  GtkWidget *usage_check = gtk_check_button_new_with_label(
      _("Share anonymous usage statistics (features used, file types, OS and hardware)"));
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(usage_check), TRUE);
  gtk_box_pack_start(GTK_BOX(content), usage_check, FALSE, FALSE, 0);
#endif

  GtkWidget *link = gtk_link_button_new_with_label(
      DT_PRIVACY_DOC_URL, _("Read what is collected, where it goes and why"));
  gtk_widget_set_halign(link, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(content), link, FALSE, FALSE, 0);

  gtk_widget_show_all(dialog);
  gtk_dialog_run(GTK_DIALOG(dialog));

  // Whatever the close action, honor the current checkbox state (both default to
  // on). Features not built in stay disabled.
#ifdef HAVE_SENTRY
  dt_conf_set_bool("sentry/enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(crash_check)));
#endif
#ifdef HAVE_TELEMETRY
  dt_conf_set_bool("telemetry/enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(usage_check)));
#endif

  gtk_widget_destroy(dialog);

  // Record that the user has decided so the dialog never shows again.
  dt_conf_set_bool(DT_PRIVACY_ASKED_KEY, TRUE);
}

#else // neither crash reporting nor analytics built in

void dt_privacy_ask_consent(const gboolean have_gui)
{
}

#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
