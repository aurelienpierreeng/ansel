#include "accelerators.h"
#include "common/darktable.h" // lots of garbage to include, only to get debug prints & flags
#include "gui/gdkkeys.h"

#include <assert.h>
#include <glib.h>


static void _clean_shortcut(gpointer data)
{
  dt_shortcut_t *shortcut = (dt_shortcut_t *)data;
  g_free(shortcut->path);
  g_free(shortcut);
}


dt_accels_t * dt_accels_init(char *config_file, GtkAccelFlags flags)
{
  dt_accels_t *accels = malloc(sizeof(dt_accels_t));
  accels->config_file = g_strdup(config_file);
  accels->global_accels = gtk_accel_group_new();
  accels->darkroom_accels = gtk_accel_group_new();
  accels->lighttable_accels = gtk_accel_group_new();
  accels->acceleratables = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _clean_shortcut);
  accels->active_group = NULL;
  accels->reset = 1;
  accels->keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  accels->default_mod_mask = gtk_accelerator_get_default_mod_mask();
  accels->init = !g_file_test(accels->config_file, G_FILE_TEST_EXISTS);
  accels->active_key.accel_flags = 0;
  accels->active_key.accel_key = 0;
  accels->active_key.accel_mods = 0;
  accels->scroll.callback = NULL;
  accels->scroll.data = NULL;
  accels->disable_accels = FALSE;
  accels->flags = flags;
  return accels;
}


void dt_accels_cleanup(dt_accels_t *accels)
{
  gtk_accel_map_save(accels->config_file);

  accels->active_group = NULL;

  g_object_unref(accels->global_accels);
  g_object_unref(accels->darkroom_accels);
  g_object_unref(accels->lighttable_accels);
  accels->global_accels = NULL;
  accels->darkroom_accels = NULL;
  accels->lighttable_accels = NULL;

  g_hash_table_unref(accels->acceleratables);

  g_free(accels->config_file);
  g_free(accels);
}


void dt_accels_connect_active_group(dt_accels_t *accels, const gchar *group)
{
  if(!accels) return;

  if(!g_strcmp0(group, "lighttable") && accels->lighttable_accels)
  {
    accels->reset--;
    accels->active_group = accels->lighttable_accels;
  }
  else if(!g_strcmp0(group, "darkroom") && accels->darkroom_accels)
  {
    accels->reset--;
    accels->active_group = accels->darkroom_accels;
  }
  else
  {
    fprintf(stderr, "[dt_accels_connect_active_group] INFO: unknown value: `%s'\n", group);
  }
}


void dt_accels_disconnect_active_group(dt_accels_t *accels)
{
  if(!accels) return;
  accels->active_group = NULL;
  accels->reset++;
}


gboolean _update_shortcut_state(dt_shortcut_t *shortcut, GtkAccelKey *key, gboolean init)
{
  gboolean changed = FALSE;
  if(shortcut->type == DT_SHORTCUT_UNSET)
  {
    // accel_map table is initially populated with shortcut->type = DT_SHORTCUT_UNSET
    // so that means the entry is new
    if(init || shortcut->locked)
    {
      // We have no user config file, or the shortcut is locked by the app.
      // Both ways, init shortcuts with defaults,
      // then a brand new config will be saved on exiting the app.
      // Note: they might still be zero, not all shortcuts are assigned.
      key->accel_key = shortcut->key;
      key->accel_mods = shortcut->mods;
      gtk_accel_map_change_entry(shortcut->path, shortcut->key, shortcut->mods, TRUE);
      shortcut->type = DT_SHORTCUT_DEFAULT;
    }
    else if(key->accel_key == shortcut->key && key->accel_mods == shortcut->mods)
    {
      // We loaded user config file and found our defaults in it. Nothing to do.
      shortcut->type = DT_SHORTCUT_DEFAULT;
    }
    else
    {
      // We loaded user config file, and user made changes in there.
      // We will need to update our "defaults", which now become rather a memory of previous state
      shortcut->key = key->accel_key;
      shortcut->mods = key->accel_mods;
      shortcut->type = DT_SHORTCUT_USER;
    }

    // UNSET state always needs update, it means it's the first time we connect accels
    changed = TRUE;
  }
  else if(shortcut->locked && (key->accel_key != shortcut->key || key->accel_mods != shortcut->mods))
  {
    // Something changed a locked shortcut. Revert to defaults.
    key->accel_key = shortcut->key;
    key->accel_mods = shortcut->mods;
    gtk_accel_map_change_entry(shortcut->path, shortcut->key, shortcut->mods, TRUE);
    shortcut->type = DT_SHORTCUT_DEFAULT;
    changed = TRUE;
  }
  else if(key->accel_key != shortcut->key || key->accel_mods != shortcut->mods)
  {
    shortcut->key = key->accel_key;
    shortcut->mods = key->accel_mods;
    shortcut->type = DT_SHORTCUT_USER;
    changed = TRUE;
  }

  return changed;
}

/** For native Gtk widget accels, we create alternatives for numpad keys, in case we fail to decode
 * them ourselves and defer to native Gtk. Otherwise, numpad keys are also converted at input event
 * handling.
*/

void _add_widget_accel(dt_shortcut_t *shortcut, const GtkAccelKey *key, GtkAccelFlags flags)
{
  gtk_widget_add_accelerator(shortcut->widget, shortcut->signal, shortcut->accel_group, key->accel_key,
                              key->accel_mods, flags);

  // Numpad numbers register as different keys. Find the numpad equivalent key here, if any.
  guint alt_char = dt_keys_numpad_alternatives(key->accel_key);
  if(key->accel_key != alt_char)
    gtk_widget_add_accelerator(shortcut->widget, shortcut->signal, shortcut->accel_group, alt_char,
                                key->accel_mods, flags);
}


void _remove_widget_accel(dt_shortcut_t *shortcut, const GtkAccelKey *key)
{
  gtk_widget_remove_accelerator(shortcut->widget, shortcut->accel_group, key->accel_key, key->accel_mods);

  // Numpad numbers register as different keys. Find the numpad equivalent key here, if any.
  guint alt_char = dt_keys_numpad_alternatives(key->accel_key);
  if(key->accel_key != alt_char)
    gtk_widget_remove_accelerator(shortcut->widget, shortcut->accel_group, alt_char, key->accel_mods);
}


void _remove_generic_accel(dt_shortcut_t *shortcut)
{
  gtk_accel_group_disconnect(shortcut->accel_group, shortcut->closure);
}

void _add_generic_accel(dt_shortcut_t *shortcut, GtkAccelKey *key, GtkAccelFlags flags)
{
  gtk_accel_group_connect(shortcut->accel_group, key->accel_key, key->accel_mods, flags,
                          shortcut->closure);
}


void _insert_accel(dt_accels_t *accels, dt_shortcut_t *shortcut)
{
  // init an accel_map entry with no keys so Gtk collects them from user config later.
  gtk_accel_map_add_entry(shortcut->path, 0, 0);
  g_hash_table_insert(accels->acceleratables, shortcut->path, shortcut);
}


void dt_accels_new_widget_shortcut(dt_accels_t *accels, GtkWidget *widget, const gchar *signal,
                                   GtkAccelGroup *accel_group, const gchar *accel_path, guint key_val,
                                   GdkModifierType accel_mods, const gboolean lock)
{
  // Our own circuitery to keep track of things after user-defined shortcuts are updated
  dt_shortcut_t *shortcut = (dt_shortcut_t *)g_hash_table_lookup(accels->acceleratables, accel_path);
  if(shortcut && shortcut->widget == widget)
  {
    // reference is still up-to-date. Nothing to do.
    return;
  }
  else if(shortcut && shortcut->type != DT_SHORTCUT_UNSET)
  {
    // If we already have a shortcut object wired to Gtk for this accel path, just update it
    GtkAccelKey key = { .accel_key = shortcut->key, .accel_mods = shortcut->mods, .accel_flags = 0 };
    if(shortcut->key > 0) _remove_widget_accel(shortcut, &key);
    shortcut->widget = widget;
    if(shortcut->key > 0) _add_widget_accel(shortcut, &key, accels->flags);
  }
  // else if shortcut && shortcut->type == DT_SHORTCUT_UNSET, we need to wait for the next call to dt_accels_connect_accels()
  else if(!shortcut)
  {
    shortcut = malloc(sizeof(dt_shortcut_t));
    shortcut->accel_group = accel_group;
    shortcut->widget = widget;
    shortcut->closure = NULL;
    shortcut->path = g_strdup(accel_path);
    shortcut->signal = signal;
    shortcut->key = key_val;
    shortcut->mods = accel_mods;
    shortcut->type = DT_SHORTCUT_UNSET;
    shortcut->locked = lock;
    _insert_accel(accels, shortcut);
    // accel is inited with empty keys so user config may set it.
    // dt_accels_load_config needs to run next
    // then dt_accels_connect_accels will update keys and possibly wire the widgets in Gtk
  }
}


const dt_shortcut_t *dt_accels_new_action_shortcut(dt_accels_t *accels, void(*action_callback), gpointer data,
                                   GtkAccelGroup *accel_group, const gchar *action_scope, const gchar *action_name,
                                   guint key_val, GdkModifierType accel_mods, const gboolean lock)
{
  // Our own circuitery to keep track of things after user-defined shortcuts are updated
  gchar *accel_path = dt_accels_build_path(action_scope, action_name);

  dt_shortcut_t *shortcut = (dt_shortcut_t *)g_hash_table_lookup(accels->acceleratables, accel_path);
  if(shortcut && shortcut->closure->data == data)
  {
    // reference is still up-to-date: nothing to do.
    return shortcut;
  }
  else if(shortcut && shortcut->type != DT_SHORTCUT_UNSET)
  {
    // If we already have a shortcut object wired to Gtk for this accel path, just update it
    GtkAccelKey key = { .accel_key = shortcut->key, .accel_mods = shortcut->mods, .accel_flags = 0 };
    if(shortcut->key > 0) _remove_generic_accel(shortcut);
    shortcut->closure = g_cclosure_new(G_CALLBACK(action_callback), data, NULL);
    if(shortcut->key > 0) _add_generic_accel(shortcut, &key, accels->flags);
  }
  // else if shortcut && shortcut->type == DT_SHORTCUT_UNSET, we need to wait for the next call to dt_accels_connect_accels()
  else if(!shortcut)
  {
    // Create a new object.
    shortcut = malloc(sizeof(dt_shortcut_t));
    shortcut->accel_group = accel_group;
    shortcut->widget = NULL;
    shortcut->closure = g_cclosure_new(G_CALLBACK(action_callback), data, NULL);
    shortcut->path = g_strdup(accel_path);
    shortcut->signal = "";
    shortcut->key = key_val;
    shortcut->mods = accel_mods;
    shortcut->type = DT_SHORTCUT_UNSET;
    shortcut->locked = lock;
    _insert_accel(accels, shortcut);
    // accel is inited with empty keys so user config may set it.
    // dt_accels_load_config needs to run next
    // then dt_accels_connect_accels will update keys and possibly wire the widgets in Gtk
  }

  g_free(accel_path);
  return shortcut;
}


void dt_accels_load_user_config(dt_accels_t *accels)
{
  gtk_accel_map_load(accels->config_file);
}


void _connect_accel(gpointer _key, gpointer value, gpointer user_data)
{
  dt_shortcut_t *shortcut = (dt_shortcut_t *)value;
  dt_accels_t *accels = (dt_accels_t *)user_data;

  GtkAccelKey key = { 0 };

  // All shortcuts should be known, they are added to accel_map at init time.
  const gboolean is_known = gtk_accel_map_lookup_entry(shortcut->path, &key);
  if(!is_known) return;

  const GtkAccelKey oldkey = { .accel_key = shortcut->key, .accel_mods = shortcut->mods, .accel_flags = 0 };
  const dt_shortcut_type_t oldtype = shortcut->type;
  const gboolean changed = _update_shortcut_state(shortcut, &key, accels->init);

  // if old_key was non zero, we already had an accel on the stack.
  // then, if the new shortcut is different, that means we need to remove the old accel.
  const gboolean needs_cleanup = changed && oldkey.accel_key > 0 && oldtype != DT_SHORTCUT_UNSET;

  // if key is non zero and new, or updated, we need to add a new accel
  const gboolean needs_init = changed && key.accel_key > 0;

  // Adding shortcuts without defined keys makes Gtk issue warnings, so avoid it.
  if(shortcut->widget)
  {
    if(needs_cleanup) _remove_widget_accel(shortcut, &oldkey);
    if(needs_init) _add_widget_accel(shortcut, &key, accels->flags);
  }
  else if(shortcut->closure)
  {
    if(needs_cleanup)
    {
      // Need to increase the number of references to avoid loosing the closure just yet.
      g_closure_ref(shortcut->closure);
      g_closure_sink(shortcut->closure);
      _remove_generic_accel(shortcut);
    }

    if(needs_init)
      _add_generic_accel(shortcut, &key, accels->flags);
    // closures can be connected only at one accel at a time, so we don't handle keypad duplicates
  }
  else
  {
    fprintf(stderr, "Invalid shortcut definition for path %s: no widget and no closure given\n", shortcut->path);
  }
}


void dt_accels_connect_accels(dt_accels_t *accels)
{
  g_hash_table_foreach(accels->acceleratables, _connect_accel, accels);
}


gchar *dt_accels_build_path(const gchar *scope, const gchar *feature)
{
  if(strncmp(scope, "<Ansel>/", strlen("<Ansel>/")) == 0)
    return g_strdup_printf("%s/%s", scope, feature);
  else
    return g_strdup_printf("<Ansel>/%s/%s", scope, feature);
}

void _accels_keys_decode(dt_accels_t *accels, GdkEvent *event, guint *keyval, GdkModifierType *mods)
{
  // Get modifiers
  gdk_event_get_state(event, mods);

  // Remove all modifiers that are irrelevant to key strokes
  *mods &= accels->default_mod_mask;

  // Get the canonical key code, that is without the modifiers
  GdkModifierType consumed;
  gdk_keymap_translate_keyboard_state(accels->keymap, event->key.hardware_keycode, event->key.state,
                                      event->key.group, // this ensures that numlock or shift are properly decoded
                                      keyval, NULL, NULL, &consumed);

  if(darktable.unmuted & DT_DEBUG_SHORTCUTS)
  {
    gchar *accel_name = gtk_accelerator_name(*keyval, *mods);
    dt_print(DT_DEBUG_SHORTCUTS, "[shortcuts] %s : %s\n",
             (event->type == GDK_KEY_PRESS) ? "Key pressed" : "Key released", accel_name);
    g_free(accel_name);
  }

  // Remove the consumed Shift modifier for numbers.
  // For French keyboards, numbers are accessed through Shift, e.g Shift + & = 1.
  // Keeping Shift here would be meaningless and gets in the way.
  if(gdk_keyval_to_lower(*keyval) == gdk_keyval_to_upper(*keyval))
  {
    *mods &= ~consumed;
  }

  // Shift + Tab gets decoded as ISO_Left_Tab and shift is consumed,
  // so it gets absorbed by the previous correction.
  // We need Ctrl+Shift+Tab to work as expected, so correct it.
  if(*keyval == GDK_KEY_ISO_Left_Tab)
  {
    *keyval = GDK_KEY_Tab;
    *mods |= GDK_SHIFT_MASK;
  }

  // Convert numpad keys to usual ones, because we care about WHAT is typed,
  // not WHERE it is typed.
  *keyval = dt_keys_mainpad_alternatives(*keyval);

  // Hopefully no more heuristics required...
}

typedef struct _accel_lookup_t
{
  GList *results;
  guint key;
  GdkModifierType modifier;
  GtkAccelGroup *group;
} _accel_lookup_t;


void _for_each_accel(gpointer key, gpointer value, gpointer user_data)
{
  dt_shortcut_t *shortcut = (dt_shortcut_t *)value;
  const gchar *path = (const gchar *)key;
  _accel_lookup_t *results = (_accel_lookup_t *)user_data;

  if(shortcut->accel_group == results->group
     && shortcut->key == results->key
     && shortcut->mods == results->modifier)
  {
    if(!g_strcmp0(path, shortcut->path))
    {
      results->results = g_list_prepend(results->results, shortcut->path);
      dt_print(DT_DEBUG_SHORTCUTS, "[shortcuts] Found accel %s for typed keys\n", path);
    }
    else
    {
      fprintf(stderr, "[shortcuts] ERROR: the shortcut path '%s' is known under the key '%s' in hashtable\n", shortcut->path, path);
    }
  }
}


// Find the accel path for the matching key & modifier within the specified accel group.
// Return the number of accels found (should be one).
guint _find_path_for_keys(dt_accels_t *accels, guint key, GdkModifierType modifier, GtkAccelGroup *group)
{
  _accel_lookup_t result = { .results = NULL, .key = key, .modifier = modifier, .group = group };
  g_hash_table_foreach(accels->acceleratables, _for_each_accel, &result);
  guint values = g_list_length(result.results);
  g_list_free(result.results);
  return values;
}


gboolean _key_pressed(GtkWidget *w, GdkEvent *event, dt_accels_t *accels, guint keyval, GdkModifierType mods)
{
  // Get the accelerator entry from the accel group
  gchar *accel_name = gtk_accelerator_name(keyval, mods);
  GQuark accel_quark = g_quark_from_string(accel_name);
  dt_print(DT_DEBUG_SHORTCUTS, "[shortcuts] Combination of keys decoded: %s\n", accel_name);
  g_free(accel_name);

  if(darktable.unmuted & DT_DEBUG_SHORTCUTS)
  {
    if(_find_path_for_keys(accels, keyval, mods, accels->active_group))
      dt_print(DT_DEBUG_SHORTCUTS, "[shortcuts] Action found in active accels group:\n");
  }

  // Look into the active group first, aka darkroom, lighttable, etc.
  if(gtk_accel_group_activate(accels->active_group, accel_quark, G_OBJECT(w), keyval, mods))
  {
    dt_print(DT_DEBUG_SHORTCUTS, "[shortcuts] Active group action executed\n");
    return TRUE;
  }

  if(darktable.unmuted & DT_DEBUG_SHORTCUTS)
  {
    if(_find_path_for_keys(accels, keyval, mods, accels->global_accels))
      dt_print(DT_DEBUG_SHORTCUTS, "[shortcuts] Action found in global accels group:\n");
  }

  // If nothing found, try again with global accels.
  if(gtk_accel_group_activate(accels->global_accels, accel_quark, G_OBJECT(w), keyval, mods))
  {
    dt_print(DT_DEBUG_SHORTCUTS, "[shortcuts] Global group action executed\n");
    return TRUE;
  }

  return FALSE;
}


gboolean dt_accels_dispatch(GtkWidget *w, GdkEvent *event, gpointer user_data)
{
  dt_accels_t *accels = (dt_accels_t *)user_data;
  if(accels->disable_accels) return FALSE;

  // Ditch everything that is not a key stroke or key strokes that are modifiers alone
  // Abort early for performance.
  if(event->key.is_modifier || accels->active_group == NULL || accels->reset > 0 || !gtk_window_is_active(GTK_WINDOW(w)))
    return FALSE;

  if(!(event->type == GDK_KEY_PRESS || event->type == GDK_KEY_RELEASE || event->type == GDK_SCROLL))
    return FALSE;

  // Scroll event: dispatch and return
  if(event->type == GDK_SCROLL)
  {
    if(accels->scroll.callback)
      return accels->scroll.callback(event->scroll, accels->scroll.data);
    else
      return FALSE;
  }

  // Key events: decode and dispatch
  GdkModifierType mods;
  guint keyval;
  _accels_keys_decode(accels, event, &keyval, &mods);

  if(event->type == GDK_KEY_PRESS)
  {
    // Store active keys until release
    accels->active_key.accel_key = keyval;
    accels->active_key.accel_mods = mods;
    return _key_pressed(w, event, accels, keyval, mods);
    // If return == FALSE, it is possible that we messed-up key value decoding
    // Default Gtk shortcuts handler will have another chance since the accel groups
    // are connected to the window in a standard way.
  }
  else if(event->type == GDK_KEY_RELEASE)
  {
    // Reset active keys
    accels->active_key.accel_key = 0;
    accels->active_key.accel_mods = 0;
    return FALSE;
  }

  return FALSE;
}


void dt_accels_attach_scroll_handler(dt_accels_t *accels, gboolean (*callback)(GdkEventScroll event, void *data), void *data)
{
  accels->scroll.callback = callback;
  accels->scroll.data = data;
}

void dt_accels_detach_scroll_handler(dt_accels_t *accels)
{
  accels->scroll.callback = NULL;
  accels->scroll.data = NULL;
}
