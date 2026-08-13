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

#ifndef DT_VIEWS_DEV_TOOLBOX_H
#define DT_VIEWS_DEV_TOOLBOX_H

#include <stddef.h>

#include "views/view.h"

struct dt_develop_t;

/** Bottom-toolbar quick-access buttons shared by any view owning its own
 * dt_develop_t (darkroom, Studio Capture...). */
typedef enum dt_dev_toolbox_button_t
{
  DT_DEV_TOOLBOX_ISO_12646,
  DT_DEV_TOOLBOX_OVEREXPOSED,
  DT_DEV_TOOLBOX_RAWOVEREXPOSED,
  DT_DEV_TOOLBOX_SOFTPROOF,
  DT_DEV_TOOLBOX_GAMUT,
  DT_DEV_TOOLBOX_DISPLAY,
} dt_dev_toolbox_button_t;

/** Create every button listed in `buttons` for `dev`, register each into the
 * module toolbox for the given view flag(s), and wire every one of them to
 * a single shared click handler that dispatches by which button fired the
 * event (a g_object_set_data tag, not one callback per button). Each button
 * is stored in its matching dt_develop_t field as it always was
 * (dev->iso_12646.button, dev->overexposed.button, dev->profile.softproof_button...).
 *
 * Buttons with an options popover (everything except ISO 12646) also get one
 * built here, anchored via dt_dev_toolbox_connect_popover() and filled with
 * the controls generic enough to make sense for any view (thresholds,
 * mode/colorscheme, profile picker, background brightness/margins). The
 * popover's content box is a plain GtkBox, retrievable with
 * gtk_bin_get_child(GTK_BIN(dev->X.floating_window)): a caller wanting extra,
 * view-specific controls (e.g. darkroom's rendering-size and
 * mask-preview-checkerboard additions to Picture display's popover) packs
 * them into that same box and calls gtk_widget_show_all() itself once done —
 * this function does not call it, so callers control when the popover is
 * considered fully assembled. Softproof and gamut still share one popover,
 * built only once both are present in the same call.
 *
 * Accelerators are NOT wired here — see dt_dev_toolbox_add_accels() — so
 * callers needing them call it separately, after this one. */
void dt_dev_toolbox_create(struct dt_develop_t *dev, dt_view_type_flags_t views,
                           const dt_dev_toolbox_button_t *buttons, size_t n_buttons);

/** Add the same "activate this button" (and, for buttons with a popover,
 * "focus its popover") keyboard accelerators for every button in `buttons`,
 * on `accel_group`/`category`. The action names ("Toggle clipping
 * indication", "Focus softproof options"...) are the same regardless of
 * which view/accel group they're bound to, since the buttons themselves are
 * shared — only the accelerator group and category differ per caller (e.g.
 * darkroom passes dt_gui_get_accels()->darkroom_accels and
 * N_("Darkroom/Toolbox"); Studio Capture passes
 * dt_gui_get_accels()->lighttable_accels, the group it actually connects,
 * and its own category). Call after dt_dev_toolbox_create() so the buttons
 * (and popovers) already exist. */
void dt_dev_toolbox_add_accels(struct dt_develop_t *dev, GtkAccelGroup *accel_group, const char *category,
                               const dt_dev_toolbox_button_t *buttons, size_t n_buttons);

/** Generic accelerator callbacks: reused by dt_dev_toolbox_add_accels(), and
 * directly usable by any view wiring its own popover-anchored button outside
 * this button set (darkroom's guides and auto-set buttons, for instance).
 * "Activate" simulates a click on `data` (the button widget); "focus" grabs
 * it and shows its popover (looked up via DT_DEV_TOOLBOX_POPOVER_KEY). */
gboolean dt_dev_toolbox_activate_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                       GdkModifierType modifier, gpointer data);
gboolean dt_dev_toolbox_focus_accel(GtkAccelGroup *accel_group, GObject *accelerable, guint keyval,
                                    GdkModifierType modifier, gpointer data);

/** Re-apply dt_dev_viewport_border_size(dev) from dev->iso_12646.enabled and the
 * "plugins/darkroom/ui/border_size" conf key, then dt_dev_configure() the
 * result. Called by the ISO 12646 toggle, and by any view that needs to
 * resize on iso_12646 changes from its own configure()/resize handling. */
void dt_dev_toolbox_apply_iso_12646_size(struct dt_develop_t *dev);

/** Anchor `popover` to `button`: right-click, or releasing after a long
 * press, shows it near the pointer. Generic — used for every popover built
 * by dt_dev_toolbox_create(), and reusable for view-owned popovers outside
 * this button set (e.g. darkroom's guides and auto-set popovers). */
void dt_dev_toolbox_connect_popover(GtkWidget *button, GtkWidget *popover);

/** Position and show `popover` (passed as a bare GtkWidget* so this can be
 * used directly as a glib timeout/idle callback). Exposed so a caller can
 * trigger the same show/anchor path from an accelerator (keyboard-activated
 * "focus this popover" actions, for instance). */
gboolean dt_dev_toolbox_show_popup(gpointer popover);

/** Run `preshow` (with `user_data`) right before dt_dev_toolbox_show_popup()
 * shows `popover` — e.g. to refresh the popover's content from state that
 * changed since it was built. Optional; a popover with none just shows
 * as-is. */
void dt_dev_toolbox_popover_set_preshow(GtkWidget *popover, void (*preshow)(gpointer user_data),
                                       gpointer user_data);

/** The g_object_data key dt_dev_toolbox_connect_popover() and
 * dt_dev_toolbox_create() tag a button with, pointing to its popover — use
 * it to look a button's popover back up (e.g. a "focus this popover"
 * accelerator). */
#define DT_DEV_TOOLBOX_POPOVER_KEY "dt-dev-toolbox-popover"
#endif // DT_VIEWS_DEV_TOOLBOX_H
