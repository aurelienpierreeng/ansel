
#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "gui/actions/views.h"
#include "gui/actions/file.h"
#include "gui/actions/edit.h"
#include "gui/actions/image.h"
#include "gui/actions/help.h"
#include "gui/actions/run.h"
#include "gui/actions/select.h"
#include "gui/actions/display.h"
#include "libs/lib.h"
#include "libs/lib_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

DT_MODULE(1)

typedef struct dt_lib_menu_t
{
  GtkWidget *menu_bar;
  GtkWidget *menus[DT_MENU_LAST];
  GList *item_lists[DT_MENU_LAST];
} dt_lib_menu_t;

const char *name(struct dt_lib_module_t *self)
{
  return _("Main menu");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"*", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_TOP_FIRST_ROW_LEFT;
}

int expandable(dt_lib_module_t *self)
{
  return 0;
}

int position()
{
  return 2;
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_menu_t *d = (dt_lib_menu_t *)g_malloc0(sizeof(dt_lib_menu_t));
  self->data = (void *)d;

  /* Init container widget */
  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_halign(self->widget, GTK_ALIGN_START);
  gtk_widget_set_hexpand(self->widget, FALSE);

  d->menu_bar = gtk_menu_bar_new();
  gtk_widget_set_name(d->menu_bar, "menu-bar");

  /* Init top-level menus */
  gchar *labels [DT_MENU_LAST] = { _("_File"), _("_Edit"), _("_Selection"), _("_Image"), _("_Styles"), _("_Run"), _("_Display"), _("_Ateliers"), _("_Help") };
  for(int i = 0; i < DT_MENU_LAST; i++)
  {
    d->item_lists[i] = NULL;
    add_top_menu_entry(d->menu_bar, d->menus, &d->item_lists[i], i, labels[i]);
  }

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(d->menu_bar), FALSE, FALSE, 0);
  gtk_widget_set_halign(d->menu_bar, GTK_ALIGN_START);
  gtk_widget_set_hexpand(d->menu_bar, FALSE);

  /* Populate file menu */
  append_file(d->menus, &d->item_lists[DT_MENU_FILE], DT_MENU_FILE);

  /* Populate edit menu */
  append_edit(d->menus, &d->item_lists[DT_MENU_EDIT], DT_MENU_EDIT);

  /* Populate selection menu */
  append_select(d->menus, &d->item_lists[DT_MENU_SELECTION], DT_MENU_SELECTION);

  /* Populate image menu */
  append_image(d->menus, &d->item_lists[DT_MENU_IMAGE], DT_MENU_IMAGE);

  /* Populate run menu */
  append_run(d->menus, &d->item_lists[DT_MENU_RUN], DT_MENU_RUN);

  /* Populate display menu */
  append_display(d->menus, &d->item_lists[DT_MENU_DISPLAY], DT_MENU_DISPLAY);

  /* Populate ateliers menu */
  append_views(d->menus, &d->item_lists[DT_MENU_ATELIERS], DT_MENU_ATELIERS);

  /* Populate help menu */
  append_help(d->menus, &d->item_lists[DT_MENU_HELP], DT_MENU_HELP);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_menu_t *d = (dt_lib_menu_t *)self->data;
  // Free all the dt_menu_entry_t elements from the GList that stores them
  for(int i = 0; i < DT_MENU_LAST; i++)
    g_list_free_full(d->item_lists[i], g_free);
  g_free(self->data);
  self->data = NULL;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
