#include "gui/actions/menu.h"
#include "common/l10n.h"
#include "control/control.h"


static gboolean show_about_dialog(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  GtkWidget *dialog = gtk_about_dialog_new();
  gtk_widget_set_name (dialog, "about-dialog");
#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), darktable_package_version);
  char *copyright = g_strdup_printf(_("Copyright \302\251 darktable authors 2009-2022\nCopyright \302\251 Aur\303\251lien Pierre 2022-%s"), darktable_last_commit_year);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), copyright);
  g_free(copyright);
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog),
                                _("Organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "https://ansel.photos");
  gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog), _("Website"));
  char *icon = g_strdup("ansel");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), icon);
  g_free(icon);

  const char *str = _("all those of you that made previous releases possible");

#include "tools/darktable_authors.h"

  const char *final[] = {str, NULL };
  gtk_about_dialog_add_credit_section (GTK_ABOUT_DIALOG(dialog), _("and..."), final);
  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog), _("translator-credits"));

  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);

  return TRUE;
}

static gboolean open_doc_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  // TODO: use translated URL when doc gets translated
  gtk_show_uri_on_window(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)), "https://ansel.photos/en/doc", GDK_CURRENT_TIME, NULL);
  return TRUE;
}

static gboolean open_chat_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  gtk_show_uri_on_window(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
  "https://app.element.io/#/room/#ansel:matrix.org", GDK_CURRENT_TIME, NULL);
  return TRUE;
}

static gboolean open_search_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  gtk_show_uri_on_window(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
  "https://chantal.aurelienpierre.com", GDK_CURRENT_TIME, NULL);
  return TRUE;
}

static gboolean open_forum_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  gtk_show_uri_on_window(GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)),
  "https://community.ansel.photos", GDK_CURRENT_TIME, NULL);
  return TRUE;
}

// TODO: this doesn't work for all widgets. the reason being that the GtkEventBox we put libs/iops into catches events.
static char *get_help_url(GtkWidget *widget)
{
  while(widget)
  {
    // if the widget doesn't have a help url set go up the widget hierarchy to find a parent that has an url
    gchar *help_url = g_object_get_data(G_OBJECT(widget), "dt-help-url");
    if(help_url) return help_url;
    widget = gtk_widget_get_parent(widget);
  }

  return NULL;
}

static void _restore_default_cursor()
{
  dt_control_allow_change_cursor();
  dt_control_change_cursor(GDK_LEFT_PTR);
  gdk_event_handler_set((GdkEventFunc)gtk_main_do_event, NULL, NULL);
}

static void _main_do_event_help(GdkEvent *event, gpointer data)
{
  gboolean handled = FALSE;

  switch(event->type)
  {
    case GDK_BUTTON_PRESS:
    {
      if(event->button.button == GDK_BUTTON_SECONDARY)
      {
        // On right-click : abort and reset
        _restore_default_cursor();
        handled = TRUE;
        break;
      }

      GtkWidget *event_widget = gtk_get_event_widget(event);
      if(event_widget)
      {
        // Note : help url contains the fully-formed URL adapted to current language
        gchar *help_url = get_help_url(event_widget);
        if(help_url && *help_url)
        {
          _restore_default_cursor();

          GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
          dt_print(DT_DEBUG_CONTROL, "[context help] opening '%s'\n", help_url);

          // ask the user if the website may be accessed
          GtkWidget *dialog = gtk_message_dialog_new
            (GTK_WINDOW(win), GTK_DIALOG_DESTROY_WITH_PARENT,
              GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
              _("do you want to access ansel.photos ?"));
#ifdef GDK_WINDOWING_QUARTZ
            dt_osx_disallow_fullscreen(dialog);
#endif

          gtk_window_set_title(GTK_WINDOW(dialog), _("access the online usermanual?"));
          const gint res = gtk_dialog_run(GTK_DIALOG(dialog));
          const gboolean open = (res == GTK_RESPONSE_YES) || !(res == GTK_RESPONSE_NO);
          gtk_widget_destroy(dialog);

          if(open)
          {
            GError *error = NULL;
            const gboolean uri_success = gtk_show_uri_on_window(GTK_WINDOW(win), help_url, gtk_get_current_event_time(), &error);

            if(uri_success)
            {
              dt_control_log(_("help url opened in web browser"));
            }
            else
            {
              dt_control_log(_("error while opening help url in web browser"));
              if (error != NULL) // uri_success being FALSE should guarantee that
              {
                fprintf (stderr, "unable to read file: %s\n", error->message);
                g_error_free(error);
              }
            }
          }
        }
        else
        {
          dt_control_log(_("there is no help available for this element"));
        }
      }
      handled = TRUE;
      break;
    }

    case GDK_ENTER_NOTIFY:
    case GDK_LEAVE_NOTIFY:
    {
      GtkWidget *event_widget = gtk_get_event_widget(event);
      if(event_widget)
      {
        gchar *help_url = get_help_url(event_widget);
        if(help_url)
        {
          // TODO: find a better way to tell the user that the hovered widget has a help link
          dt_cursor_t cursor = event->type == GDK_ENTER_NOTIFY ? GDK_QUESTION_ARROW : GDK_X_CURSOR;
          dt_control_allow_change_cursor();
          dt_control_change_cursor(cursor);
          dt_control_forbid_change_cursor();
        }
      }
      break;
    }
    default:
      break;
  }

  if(!handled) gtk_main_do_event(event);
}

static gboolean contextual_help_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_control_change_cursor(GDK_X_CURSOR);
  dt_control_forbid_change_cursor();
  gdk_event_handler_set(_main_do_event_help, NULL, NULL);
  return TRUE;
}

static gboolean search_accels_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data)
{
  dt_accels_search(darktable.gui->accels, GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  return TRUE;
}

void append_help(GtkWidget **menus, GList **lists, const dt_menus_t index)
{
  add_sub_menu_entry(menus, lists, _("Online documentation"), index, NULL, open_doc_callback, NULL, NULL, NULL, 0, 0);
  add_sub_menu_entry(menus, lists, _("Ask a question"), index, NULL, open_search_callback, NULL, NULL, NULL, 0, 0);
  add_sub_menu_entry(menus, lists, _("Join the support chat"), index, NULL, open_chat_callback, NULL, NULL, NULL, 0, 0);
  add_sub_menu_entry(menus, lists, _("Join the support forum"), index, NULL, open_forum_callback, NULL, NULL, NULL, 0, 0);
  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("Open contextual help"), index, NULL, contextual_help_callback, NULL, NULL, NULL, 0, 0);
  add_sub_menu_entry(menus, lists, _("Search actions..."), index, NULL, search_accels_callback, NULL, NULL,
                     NULL, GDK_KEY_p, GDK_CONTROL_MASK);
  add_menu_separator(menus[index]);
  add_sub_menu_entry(menus, lists, _("About"), index, NULL, show_about_dialog, NULL, NULL, NULL, 0, 0);
}
