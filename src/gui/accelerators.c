#include "accelerators.h"
#include "common/darktable.h" // lots of garbage to include, only to get debug prints & flags
#include "gui/gtkentry.h"
#include "gui/gdkkeys.h"
#include "dtgtk/icon_cell_renderer.h"

#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif

#include <assert.h>
#include <glib.h>

typedef struct {
  GClosure  *base;
  gpointer  parent_data; // Reference to the closure->data of the parent shortcut instance, if any
} PayloadClosure;

typedef struct _accel_removal_t
{
  const char *path;
  gpointer data;
} _accel_removal_t;


static void _g_list_closure_unref(gpointer data)
{
  PayloadClosure *pc = (PayloadClosure *)data;
  if(pc->base) g_closure_unref(pc->base);
  g_free(pc);
}


static void _clean_shortcut(gpointer data)
{
  dt_shortcut_t *shortcut = (dt_shortcut_t *)data;
  g_free(shortcut->path);
  g_list_free_full(shortcut->closure, _g_list_closure_unref);
  g_free(shortcut);
}


// Return the last closure in the list
PayloadClosure *dt_shortcut_get_payload_closure(dt_shortcut_t *shortcut)
{
  GList *link = g_list_first(shortcut->closure);
  if(link)
    return (PayloadClosure *)link->data;
  else
    return NULL;
}

GClosure *dt_shortcut_get_closure(dt_shortcut_t *shortcut)
{
  PayloadClosure *pc = dt_shortcut_get_payload_closure(shortcut);
  if(pc)
    return pc->base;
  else
    return NULL;
}


// Remove the accel closure instance from shortcut that references data
// as its input or as its parent. Useful when module instances are destroyed,
// so we destroy the shortcut attached to the parent module, and the shortcut
// attached to all its children.
void dt_shortcut_remove_closure(dt_shortcut_t *shortcut, gpointer data)
{
  if(!shortcut->closure) return;

  PayloadClosure *cl = NULL;
  GList *link = NULL;

  if(data)
  {
    // Look for closures referencing data in their direct closure args
    for(link = g_list_first(shortcut->closure); link; link = g_list_next(link))
    {
      PayloadClosure *closure = (PayloadClosure *)link->data;
      if(closure->base->data == data || closure->parent_data == data)
      {
        cl = closure;
        break;
      }
    }
  }
  else
  {
    link = g_list_last(shortcut->closure);
    if(link) cl = (PayloadClosure *)link->data;
  }

  if(cl)
  {
    g_closure_unref(cl->base);
    shortcut->closure = g_list_delete_link(shortcut->closure, link);
    // fprintf(stdout, "removing: %s at %p - %i entries remaining\n", shortcut->path, data, g_list_length(shortcut->closure));
  }
}


static void _find_parent_hashtable(gpointer _key, gpointer value, gpointer user_data)
{
  dt_shortcut_t *parent_shortcut = (dt_shortcut_t *)value;
  dt_shortcut_t *child_shortcut = (dt_shortcut_t *)user_data;

  // Remove the last branch of the path to build the path of the immediate ancestor
  gchar **child_parts = g_strsplit(child_shortcut->path, "/", -1);
  guint n = g_strv_length(child_parts);
  g_free(child_parts[n - 1]);
  child_parts[n - 1] = NULL;
  gchar *parent_path = g_strjoinv ("/", child_parts);
  g_strfreev(child_parts);

  // This should technically match only once in the HashTable for_each()
  if(!g_strcmp0(parent_shortcut->path, parent_path))
  {
    GClosure *parent_closure = dt_shortcut_get_closure(parent_shortcut);
    PayloadClosure *child_closure = dt_shortcut_get_payload_closure(child_shortcut);
    child_closure->parent_data = parent_closure->data;

    /*
    fprintf(stdout, "%s is the parent of %s - pointer %p\n", parent_shortcut->path, child_shortcut->path,
            parent_closure->data);
    */
  }

  g_free(parent_path);
}


// Lookup all existing shortcuts that share their path root with this one,
// Consider them as parent of this one,
// write this one into their (dt_shortcut_t *)->children table.
// This assumes that parents are declared before children, which makes sense for widgets.
static void _insert_parent_data_into_children(dt_shortcut_t *shortcut)
{
  g_hash_table_foreach(shortcut->accels->acceleratables, _find_parent_hashtable, (gpointer)shortcut);
}


// Append a new closure in the list
void dt_shortcut_set_closure(dt_shortcut_t *shortcut,
                             gboolean (*action_callback)(GtkAccelGroup *group, GObject *acceleratable,
                                                         guint keyval, GdkModifierType mods, gpointer user_data),
                             gpointer data)
{
  PayloadClosure *pc = malloc(sizeof(PayloadClosure));
  pc->base = g_cclosure_new(G_CALLBACK(action_callback), data, NULL);
  pc->parent_data = NULL;

  g_closure_set_marshal(pc->base, g_cclosure_marshal_generic);
  g_closure_ref(pc->base);
  g_closure_sink(pc->base);
  g_closure_ref(pc->base);
  shortcut->closure = g_list_append(shortcut->closure, pc);
  // fprintf(stdout, "appending closure for %s - %i entries\n", shortcut->path, g_list_length(shortcut->closure));
  _insert_parent_data_into_children(shortcut);
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
  dt_pthread_mutex_init(&accels->lock, NULL);
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

  dt_pthread_mutex_lock(&accels->lock);
  g_hash_table_unref(accels->acceleratables);
  dt_pthread_mutex_unlock(&accels->lock);

  dt_pthread_mutex_destroy(&accels->lock);

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


static gboolean _update_shortcut_state(dt_shortcut_t *shortcut, GtkAccelKey *key, gboolean init)
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

static void _add_widget_accel(dt_shortcut_t *shortcut, GtkAccelFlags flags)
{
  gtk_widget_add_accelerator(shortcut->widget, shortcut->signal, shortcut->accel_group, shortcut->key,
                             shortcut->mods, flags);

  // Numpad numbers register as different keys. Find the numpad equivalent key here, if any.
  guint alt_char = dt_keys_numpad_alternatives(shortcut->key);
  if(shortcut->key != alt_char)
    gtk_widget_add_accelerator(shortcut->widget, shortcut->signal, shortcut->accel_group, alt_char, shortcut->mods,
                               flags);
}


static void _remove_widget_accel(dt_shortcut_t *shortcut, const GtkAccelKey *old_key)
{
  gtk_widget_remove_accelerator(shortcut->widget, shortcut->accel_group, old_key->accel_key, old_key->accel_mods);

  // Numpad numbers register as different keys. Find the numpad equivalent key here, if any.
  guint alt_char = dt_keys_numpad_alternatives(old_key->accel_key);
  if(old_key->accel_key != alt_char)
    gtk_widget_remove_accelerator(shortcut->widget, shortcut->accel_group, alt_char, old_key->accel_mods);
}


static void _remove_generic_accel(dt_shortcut_t *shortcut)
{
  // Need to increase the number of references to avoid loosing the closure just yet.
  GClosure *cl = dt_shortcut_get_closure(shortcut);
  g_closure_ref(cl);
  g_closure_sink(cl);
  gtk_accel_group_disconnect(shortcut->accel_group, cl);
}


static void _add_generic_accel(dt_shortcut_t *shortcut, GtkAccelFlags flags)
{
  GClosure *closure = dt_shortcut_get_closure(shortcut);
  if(closure)
    gtk_accel_group_connect(shortcut->accel_group, shortcut->key, shortcut->mods, flags | GTK_ACCEL_VISIBLE, closure);
}


static void _insert_accel(dt_accels_t *accels, dt_shortcut_t *shortcut)
{
  // init an accel_map entry with no keys so Gtk collects them from user config later.
  gtk_accel_map_add_entry(shortcut->path, 0, 0);
  dt_pthread_mutex_lock(&accels->lock);
  g_hash_table_insert(accels->acceleratables, shortcut->path, shortcut);
  dt_pthread_mutex_unlock(&accels->lock);
}


static gboolean _virtual_shortcut_callback(GtkAccelGroup *group, GObject *acceleratable, guint keyval,
                                       GdkModifierType mods, gpointer user_data)
{
  dt_shortcut_t *shortcut = (dt_shortcut_t *)user_data;
  if(!shortcut->widget) return FALSE;

  // Focus the target widget
  gtk_widget_grab_focus(shortcut->widget);

  // Hardware-decode the shortcut key
  guint keycode = 0;
  GdkKeymapKey *keys = NULL;
  gint n = 0;
  GdkKeymap *keymap = gdk_keymap_get_for_display(gdk_display_get_default());
  if(gdk_keymap_get_entries_for_keyval(keymap, shortcut->key, &keys, &n))
  {
    if(n > 0) keycode = keys[0].keycode;
    g_free(keys);
  }

  // Create a virtual key stroke using our shortcut keys
  GdkEvent *ev = gdk_event_new(GDK_KEY_PRESS);
  ev->key.window = g_object_ref(gtk_widget_get_window(shortcut->widget));
  ev->key.send_event = TRUE;
  ev->key.time = GDK_CURRENT_TIME;
  ev->key.state = shortcut->mods;
  ev->key.keyval = shortcut->key;
  ev->key.hardware_keycode = keycode;
  ev->key.group = 0;
  ev->key.is_modifier = FALSE;

  // Fire the virtual keystroke to the target widget
  gtk_widget_event(shortcut->widget, ev);
  gdk_event_free(ev);

  return TRUE;
}


void dt_accels_new_virtual_shortcut(dt_accels_t *accels, GtkAccelGroup *accel_group, const gchar *accel_path,
                                    GtkWidget *widget, guint key_val, GdkModifierType accel_mods)
{
  // Our own circuitery to keep track of things after user-defined shortcuts are updated
  dt_pthread_mutex_lock(&accels->lock);
  dt_shortcut_t *shortcut = (dt_shortcut_t *)g_hash_table_lookup(accels->acceleratables, accel_path);
  dt_pthread_mutex_unlock(&accels->lock);

  if(!shortcut)
  {
    shortcut = malloc(sizeof(dt_shortcut_t));
    shortcut->accel_group = accel_group;
    shortcut->widget = widget;
    shortcut->closure = NULL;
    shortcut->path = g_strdup(accel_path);
    shortcut->signal = NULL;
    shortcut->key = key_val;
    shortcut->mods = accel_mods;
    shortcut->type = DT_SHORTCUT_UNSET;
    shortcut->locked = TRUE;
    shortcut->virtual_shortcut = TRUE;
    shortcut->description = _("Contextual interaction on focus");
    shortcut->accels = accels;
    dt_shortcut_set_closure(shortcut, _virtual_shortcut_callback, shortcut);
    _insert_accel(accels, shortcut);
  }
}

void dt_accels_new_virtual_instance_shortcut(dt_accels_t *accels,
                                             gboolean (*action_callback)(GtkAccelGroup *group,
                                                                         GObject *acceleratable, guint keyval,
                                                                         GdkModifierType mods, gpointer user_data),
                                             gpointer data, GtkAccelGroup *accel_group, const gchar *action_scope,
                                             const gchar *action_name)
{
  gchar *accel_path = dt_accels_build_path(action_scope, action_name);

  // Our own circuitery to keep track of things after user-defined shortcuts are updated
  dt_pthread_mutex_lock(&accels->lock);
  dt_shortcut_t *shortcut = (dt_shortcut_t *)g_hash_table_lookup(accels->acceleratables, accel_path);
  dt_pthread_mutex_unlock(&accels->lock);

  if(!shortcut)
  {
    shortcut = malloc(sizeof(dt_shortcut_t));
    shortcut->accel_group = accel_group;
    shortcut->widget = NULL;
    shortcut->closure = NULL;
    shortcut->path = g_strdup(accel_path);
    shortcut->signal = NULL;
    shortcut->key = 0;
    shortcut->mods = 0;
    shortcut->type = DT_SHORTCUT_DEFAULT;
    shortcut->locked = TRUE;
    shortcut->virtual_shortcut = TRUE;
    shortcut->description = _("Focuses the instance");
    shortcut->accels = accels;
    dt_shortcut_set_closure(shortcut, action_callback, data);

    dt_pthread_mutex_lock(&accels->lock);
    g_hash_table_insert(accels->acceleratables, shortcut->path, shortcut);
    dt_pthread_mutex_unlock(&accels->lock);
  }

  g_free(accel_path);
}


void dt_accels_new_widget_shortcut(dt_accels_t *accels, GtkWidget *widget, const gchar *signal,
                                   GtkAccelGroup *accel_group, const gchar *accel_path, guint key_val,
                                   GdkModifierType accel_mods, const gboolean lock)
{
  // Our own circuitery to keep track of things after user-defined shortcuts are updated
  dt_pthread_mutex_lock(&accels->lock);
  dt_shortcut_t *shortcut = (dt_shortcut_t *)g_hash_table_lookup(accels->acceleratables, accel_path);
  dt_pthread_mutex_unlock(&accels->lock);

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
    if(shortcut->key > 0) _add_widget_accel(shortcut, accels->flags);
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
    shortcut->virtual_shortcut = FALSE;
    shortcut->description = _("Trigger the action");
    shortcut->accels = accels;
    _insert_accel(accels, shortcut);
    // accel is inited with empty keys so user config may set it.
    // dt_accels_load_config needs to run next
    // then dt_accels_connect_accels will update keys and possibly wire the widgets in Gtk
  }
}


// Multiple instances of modules will have the same path for the same control
// meaning they all share the same shortcut object, which is not possible
// because they are referenced by pathes and those are unique.
// We handle this here by overriding any pre-existing closure
// with a reference to the current widget, meaning
// the last module in the order of GUI inits wins the shortcut.
void dt_accels_new_action_shortcut(dt_accels_t *accels,
                                   gboolean (*action_callback)(GtkAccelGroup *group, GObject *acceleratable,
                                                               guint keyval, GdkModifierType mods,
                                                               gpointer user_data),
                                   gpointer data, GtkAccelGroup *accel_group, const gchar *action_scope,
                                   const gchar *action_name, guint key_val, GdkModifierType accel_mods,
                                   const gboolean lock, const char *description)
{
  // Our own circuitery to keep track of things after user-defined shortcuts are updated
  gchar *accel_path = dt_accels_build_path(action_scope, action_name);

  dt_pthread_mutex_lock(&accels->lock);
  dt_shortcut_t *shortcut = (dt_shortcut_t *)g_hash_table_lookup(accels->acceleratables, accel_path);
  dt_pthread_mutex_unlock(&accels->lock);

  if(shortcut && dt_shortcut_get_closure(shortcut) && dt_shortcut_get_closure(shortcut)->data == data)
  {
    // reference is still up-to-date: nothing to do.
    return;
  }
  else if(shortcut && shortcut->type != DT_SHORTCUT_UNSET)
  {
    // If we already have a shortcut object wired to Gtk for this accel path, just update it
    if(shortcut->key > 0) _remove_generic_accel(shortcut);
    dt_shortcut_set_closure(shortcut, action_callback, data);
    if(shortcut->key > 0) _add_generic_accel(shortcut, accels->flags);
  }
  // else if shortcut && shortcut->type == DT_SHORTCUT_UNSET, we need to wait for the next call to dt_accels_connect_accels()
  else if(!shortcut)
  {
    // Create a new object.
    shortcut = malloc(sizeof(dt_shortcut_t));
    shortcut->accel_group = accel_group;
    shortcut->widget = NULL;
    shortcut->closure = NULL;
    shortcut->path = g_strdup(accel_path);
    shortcut->signal = "";
    shortcut->key = key_val;
    shortcut->mods = accel_mods;
    shortcut->type = DT_SHORTCUT_UNSET;
    shortcut->locked = lock;
    shortcut->virtual_shortcut = FALSE;
    shortcut->description = description;
    shortcut->accels = accels;
    dt_shortcut_set_closure(shortcut, action_callback, data);
    _insert_accel(accels, shortcut);
    // accel is inited with empty keys so user config may set it.
    // dt_accels_load_config needs to run next
    // then dt_accels_connect_accels will update keys and possibly wire the widgets in Gtk
  }

  g_free(accel_path);
}


void dt_accels_load_user_config(dt_accels_t *accels)
{
  gtk_accel_map_load(accels->config_file);
}

// Resync the GtkAccelMap with our shortcut, meaning key changes should happen in GtkAccelMap before
static void _connect_accel(dt_shortcut_t *shortcut)
{
  GtkAccelKey key = { 0 };

  // All shortcuts should be known, they are added to accel_map at init time.
  const gboolean is_known = gtk_accel_map_lookup_entry(shortcut->path, &key);
  if(!is_known) return;

  // Remember previous values
  const GtkAccelKey oldkey = { .accel_key = shortcut->key, .accel_mods = shortcut->mods, .accel_flags = 0 };
  const dt_shortcut_type_t oldtype = shortcut->type;

  // Resync our shortcut object key/mods with what is currently defined in the GtkAccelMap
  const gboolean changed = _update_shortcut_state(shortcut, &key, shortcut->accels->init);

  // if old_key was non zero, we already had an accel on the stack.
  // then, if the new shortcut is different, that means we need to remove the old accel.
  const gboolean needs_cleanup = changed && oldkey.accel_key > 0 && oldtype != DT_SHORTCUT_UNSET;

  // if key is non zero and new, or updated, we need to add a new accel
  const gboolean needs_init = changed && key.accel_key > 0;

  if(dt_shortcut_get_closure(shortcut))
  {
    if(needs_cleanup) _remove_generic_accel(shortcut);
    if(needs_init) _add_generic_accel(shortcut, shortcut->accels->flags);
    // closures can be connected only at one accel at a time, so we don't handle keypad duplicates
  }
  else if(shortcut->widget)
  {
    if(needs_cleanup) _remove_widget_accel(shortcut, &oldkey);
    if(needs_init) _add_widget_accel(shortcut, shortcut->accels->flags);
  }
  else
  {
    // Nothing
  }
}

static void _connect_accel_hashtable(gpointer _key, gpointer value, gpointer user_data)
{
  dt_shortcut_t *shortcut = (dt_shortcut_t *)value;
  _connect_accel(shortcut);
}


void dt_accels_connect_accels(dt_accels_t *accels)
{
  dt_pthread_mutex_lock(&accels->lock);
  g_hash_table_foreach(accels->acceleratables, _connect_accel_hashtable, NULL);
  dt_pthread_mutex_unlock(&accels->lock);
}

static void
_remove_accel_hashtable(gpointer _key, gpointer value, gpointer user_data)
{
  dt_shortcut_t *shortcut = (dt_shortcut_t *)value;
  _accel_removal_t *params = (_accel_removal_t *)user_data;
  if(g_strrstr(shortcut->path, params->path) != NULL)
  {
    //fprintf(stdout, "removing %s\n", shortcut->path);
    if(dt_shortcut_get_closure(shortcut))
    {
      // Detach the accel from the accel group
      if(shortcut->key > 0) _remove_generic_accel(shortcut);

      // Remove the closure matching user_data, or the last one
      dt_shortcut_remove_closure(shortcut, params->data);

      // Reattach the accel to the accel group using the last closure in the list
      if(shortcut->key > 0) _add_generic_accel(shortcut, shortcut->accels->flags);
    }
    /* Should we handle that too ?
    else if(shortcut->widget)
    {
      GtkAccelKey key = { 0 };
      if(gtk_accel_map_lookup_entry(shortcut->path, &key))
        _remove_widget_accel(shortcut, &key);
    }
    */
  }
}

// For all shortcuts matching path (fully or partially), remove the closure instance referencing data
void dt_accels_remove_accel(dt_accels_t *accels, const char *path, gpointer data)
{
  if(!accels || !accels->acceleratables) return;

  _accel_removal_t *params = malloc(sizeof(_accel_removal_t));
  params->path = path;
  params->data = data;

  dt_pthread_mutex_lock(&accels->lock);
  g_hash_table_foreach(accels->acceleratables, _remove_accel_hashtable, (gpointer)params);
  dt_pthread_mutex_unlock(&accels->lock);

  free(params);
}

void dt_accels_remove_shortcut(dt_accels_t *accels, const char *path)
{
  dt_pthread_mutex_lock(&accels->lock);
  g_hash_table_remove(accels->acceleratables, path);
  dt_pthread_mutex_unlock(&accels->lock);
}


gchar *dt_accels_build_path(const gchar *scope, const gchar *feature)
{
  if(strncmp(scope, "<Ansel>/", strlen("<Ansel>/")) == 0)
    return g_strdup_printf("%s/%s", scope, feature);
  else
    return g_strdup_printf("<Ansel>/%s/%s", scope, feature);
}

static void _accels_keys_decode(dt_accels_t *accels, GdkEvent *event, guint *keyval, GdkModifierType *mods)
{
  if(accels == NULL) return;

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


static inline void _for_each_accel(gpointer key, gpointer value, gpointer user_data)
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
// Return the path of the first accel found
static const char * _find_path_for_keys(dt_accels_t *accels, guint key, GdkModifierType modifier, GtkAccelGroup *group)
{
  _accel_lookup_t result = { .results = NULL, .key = key, .modifier = modifier, .group = group };

  dt_pthread_mutex_lock(&accels->lock);
  g_hash_table_foreach(accels->acceleratables, _for_each_accel, &result);
  dt_pthread_mutex_unlock(&accels->lock);

  char *path = NULL;
  GList *item = g_list_first(result.results);
  if(item) path = (char *)item->data;

  g_list_free(result.results);
  return path;
}


static gboolean _key_pressed(GtkWidget *w, GdkEvent *event, dt_accels_t *accels, guint keyval, GdkModifierType mods)
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

// Ugly. Use that only for callbacks of the shortcuts GUI popup
// when the user_data pointer is already used for something else.
// This will be inited when opening the popup, so there is only
// one place/thread accessing it and the reference is up to date
// within the scope where it's used.
static dt_accels_t *accels_global_ref = NULL;

enum
{
  COL_NAME,
  COL_KEYS,
  COL_CLEAR,
  COL_DESCRIPTION,
  COL_PATH,
  COL_SHORTCUT,
  COL_KEYVAL,
  COL_MODS,
  NUM_COLUMNS
};

typedef struct _accel_treeview_t
{
  GtkTreeStore *store;
  GHashTable *node_cache;
} _accel_treeview_t;


static void _make_column_editable(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                                  GtkTreeIter *iter, gpointer data)
{
  dt_shortcut_t *shortcut;
  gtk_tree_model_get(model, iter, COL_SHORTCUT, &shortcut, -1);
  g_object_set(renderer,
               "visible", (shortcut != NULL),
               "editable", (shortcut != NULL && !shortcut->locked),
               "accel-mode", GTK_CELL_RENDERER_ACCEL_MODE_OTHER,
               NULL);
}

static void _make_column_clearable(GtkTreeViewColumn *col, GtkCellRenderer *renderer, GtkTreeModel *model,
                                   GtkTreeIter *iter, gpointer data)
{
  dt_shortcut_t *shortcut;
  gtk_tree_model_get(model, iter, COL_SHORTCUT, &shortcut, -1);
  g_object_set (renderer,
                "icon-name", (shortcut != NULL && !shortcut->locked) ? "edit-delete-symbolic" : "lock",
                "visible",   (shortcut != NULL),
                "sensitive", (shortcut != NULL && !shortcut->locked && shortcut->key),
                NULL);
}


static int guess_key_group(dt_accels_t *accels, guint keyval, guint hardware_keycode)
{
  GdkKeymapKey *keys;
  guint *keyvals;
  gint n_keys;

  if(!gdk_keymap_get_entries_for_keycode(accels->keymap, hardware_keycode, &keys, &keyvals, &n_keys))
    return 0;

  for(int i = 0; i < n_keys; ++i)
  {
    if(keyvals[i] == keyval)
    {
      int group = keys[i].group;
      g_free(keys);
      g_free(keyvals);
      return group; // found matching group
    }
  }

  g_free(keys);
  g_free(keyvals);
  return 0; // not found, default
}

static void _shortcut_edited(GtkCellRenderer *cell, const gchar *path_string, guint keyval, GdkModifierType mods,
                             guint hardware_key, gpointer user_data)
{
  // The tree model passed as arg is the filtered proxy.
  // We will need to access its underlying store (full, unfiltered)
  GtkTreeModel *filter = GTK_TREE_MODEL(user_data);
  GtkTreeModel *store = gtk_tree_model_filter_get_model(GTK_TREE_MODEL_FILTER(filter));
  if(!store) return;

  GtkTreePath *path = gtk_tree_path_new_from_string(path_string);
  dt_shortcut_t *shortcut = NULL;

  // f_iter is the row coordinates relative to the filtered model
  // That's what we need to READ data
  GtkTreeIter f_iter;
  if(gtk_tree_model_get_iter(GTK_TREE_MODEL(filter), &f_iter, path))
    gtk_tree_model_get(GTK_TREE_MODEL(filter), &f_iter, COL_SHORTCUT, &shortcut, -1);

  const char *shortcut_path = NULL;

  // mods input arg doesn't record states (numlock, capslock), so we need to fetch it
  // directly before decoding full key combinations
  GdkDisplay *display = gdk_display_get_default();
  GdkSeat *seat = gdk_display_get_default_seat(display);
  GdkDevice *pointer = gdk_seat_get_pointer(seat);
  GdkModifierType state;
  gdk_device_get_state(pointer, gdk_get_default_root_window(), NULL, &state);

  // Ensure modifiers, language-heuristics and numpad/keypad keys are uniformingly decoded
  GdkEventKey event = { 0 };
  event.type = GDK_KEY_PRESS;
  event.state = mods | state;
  event.keyval = keyval;
  event.hardware_keycode = hardware_key;
  event.group = guess_key_group(accels_global_ref, keyval, hardware_key);
  _accels_keys_decode(accels_global_ref, (GdkEvent *)&event, &keyval, &mods);

  if(shortcut)
  {
    // Lookup this keys combination in the current accel_group (only if key is not empty)
    if(!(keyval == 0 && mods == 0))
      shortcut_path = _find_path_for_keys(shortcut->accels, keyval, mods, shortcut->accel_group);

    // Try to update the GtkAccelMap with new keys
    if(!shortcut_path && gtk_accel_map_change_entry(shortcut->path, keyval, mods, FALSE))
    {
      // Success:
      // Resync our internal shortcut object and its GtkAccelGroup to GtkAccelMap
      _connect_accel(shortcut);

      // s_iter is the row coordinates relative to the child/source model (unfiltered)
      // That's what we need to WRITE data
      // And write new keys into the source model
      GtkTreeIter s_iter;
      gtk_tree_model_filter_convert_iter_to_child_iter(GTK_TREE_MODEL_FILTER(filter), &s_iter, &f_iter);
      gtk_tree_store_set(GTK_TREE_STORE(store), &s_iter, COL_KEYVAL, keyval, COL_MODS, mods, -1);
    }
  }

  if(shortcut_path)
  {
    // The GtkAccelMap could not be updated because another accel uses the same keys
    // That also happens if we try to unset a shortcut more than once, but then it's no issue.
    char *new_text = gtk_accelerator_name(keyval, mods);
    GtkWidget *dlg
        = gtk_message_dialog_new_with_markup(NULL, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s <tt>%s</tt>\n%s <tt>%s</tt>.\n%s",
                                              _("The shortcut for"), shortcut_path,
                                              _("is already using the key combination"), new_text,
                                              _("Delete it first."));
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(new_text);
  }

  gtk_tree_path_free(path);
}


static gboolean _icon_activate(GtkCellRenderer *cell, GdkEvent *event, GtkWidget *treeview, const gchar *path_str,
                               GdkRectangle *background, GdkRectangle *cell_area, GtkCellRendererState flags,
                               gpointer user_data)
{
  // Reset accel at current path
  _shortcut_edited(cell, path_str, 0, 0, 0, user_data);
  return TRUE;
}


static void _create_main_row(GtkTreeStore *store, GtkTreeIter *iter, const char *label, const char *path,
                             dt_shortcut_t *shortcut)
{
  gtk_tree_store_set(store, iter,
                     COL_NAME, label,
                     COL_DESCRIPTION, shortcut->description,
                     COL_PATH, path,
                     COL_KEYVAL, shortcut->key,
                     COL_MODS, shortcut->mods,
                     COL_SHORTCUT, shortcut, -1);
}

void _for_each_accel_create_treeview_row(gpointer key, gpointer value, gpointer user_data)
{
  // Extract HashTable key/value
  dt_shortcut_t *shortcut = (dt_shortcut_t *)value;
  if(!shortcut) return;
  const gchar *path = (const gchar *)key;

  // Extract user_data
  _accel_treeview_t *_data = (_accel_treeview_t *)user_data;
  GHashTable *node_cache = _data->node_cache;
  GtkTreeStore *store = _data->store;

  GtkTreeIter *parent = NULL;
  GtkTreeIter *iter = NULL;

  // Split the shortcut accel path on /.
  // Then we reconstruct it piece by piece and add a tree node fore each piece,
  // which lets us manage parents/children.
  // Note 1: parts[0] is always "<Ansel>"
  // Note 2: that fails if widget labels contain /
  gchar **parts = g_strsplit(path, "/", -1);
  gchar *accum = g_strdup("<Ansel>");

  // We will copy pathes after <Ansel> string because that makes
  // treeview markup parsers fail since it looks like markup
  const size_t len_ansel = strlen(accum);
  for(int i = 1; parts[i]; ++i)
  {
    // Build the partial path so far
    gchar *tmp = g_strconcat(accum, "/", parts[i], NULL);
    g_free(accum);
    accum = tmp;

    // Find out if current node exists.
    // If it does, it will be our parent for the next step.
    iter = g_hash_table_lookup(node_cache, accum);

    // If current node is not already in tree, add it.
    if(!iter)
    {
      // We need a heap-allocated iter to pass it along to the hashtable.
      // This will be freed when cleaning up the hashtable.
      GtkTreeIter new_iter;
      gtk_tree_store_append(store, &new_iter, parent);

      // heap‑copy the struct to pass it along to the HashTable
      iter = g_new(GtkTreeIter, 1);
      *iter = new_iter;
      g_hash_table_insert(node_cache, g_strdup(accum), iter);
    }

    // Capitalize first letter for GUI purposes
    gchar *label = g_strdup(parts[i]);
    label[0] = g_unichar_toupper(label[0]);

    // Write the shortcut only if we are at the terminating point of the path
    if(!g_strcmp0(accum, path))
      _create_main_row(store, iter, label, path + len_ansel, shortcut);
    else
      gtk_tree_store_set(store, iter, COL_NAME, parts[i], COL_KEYS, "", COL_PATH, accum + len_ansel, -1);

    g_free(label);

    parent = iter;
  }

  g_free(accum);
  g_strfreev(parts);
}


void _for_each_path_create_treeview_row(gpointer key, gpointer value, gpointer user_data)
{
  // Extract HashTable key/value
  dt_shortcut_t *shortcut = (dt_shortcut_t *)value;
  if(!shortcut) return;
  const gchar *path = (const gchar *)key;

  GtkListStore *store = (GtkListStore *)user_data;
  if(!store) return;

  dt_accels_t *accels = shortcut->accels;
  //g_print("My object is a <%s>\n", G_OBJECT_TYPE_NAME(store));

  // Append the shortcut path, minus initial <Ansel> root, to a flat list
  // only if the shortcut belongs to one currently-active accel group
  if(shortcut->accel_group == accels->global_accels ||
     shortcut->accel_group == accels->active_group)
  {
    // Discard <Ansel>/View/ from the start of the path to make the view narrower
    gchar **parts = g_strsplit(path, "/", -1);
    gchar *tail = g_strjoinv ("/", parts + 2);

    GtkTreeIter iter;
    gtk_list_store_append(store, &iter);
    gtk_list_store_set(store, &iter,
                       0, tail, // shortcut path
                       1, shortcut, // shortcut object
                       2, 0, // init relevance
                       3, shortcut->description, // description
                       5, shortcut->key,
                       6, shortcut->mods,
                       -1);
    g_free(tail);
    g_strfreev(parts);
  }
}

// Relevance coeff stored in column index 2
static gint _sort_model_by_relevance_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
  int ka, kb;
  gtk_tree_model_get(model, a, 2, &ka, -1);
  gtk_tree_model_get(model, b, 2, &kb, -1);
  return ka - kb;
}

static gint _sort_model_func(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b, gpointer data)
{
  gchar *ka, *kb;
  gtk_tree_model_get(model, a, GPOINTER_TO_INT(data), &ka, -1);
  gtk_tree_model_get(model, b, GPOINTER_TO_INT(data), &kb, -1);

  gint res = 0;
  if(ka && kb)
  {
    // Make strings case-insensitive
    gchar *ka_ci = g_utf8_casefold(ka, -1);
    gchar *kb_ci = g_utf8_casefold(kb, -1);

    // Compare strings
    res = g_utf8_collate(ka_ci, kb_ci);

    g_free(ka_ci);
    g_free(kb_ci);
  }

  g_free(ka);
  g_free(kb);
  return res;
}

typedef struct _accel_window_params_t
{
  GtkWidget *path_search;
  GtkWidget *keys_search;
  GtkWidget *tree_view;
} _accel_window_params_t;


static gboolean filter_callback(GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
  _accel_window_params_t *params = (_accel_window_params_t *)user_data;

  // Everything visible if needle is empty or NULL, aka no active search
  const gchar *needle_path = gtk_entry_get_text(GTK_ENTRY(params->path_search));
  const gchar *needle_keys = gtk_entry_get_text(GTK_ENTRY(params->keys_search));

  if((needle_path == NULL || needle_path[0] == '\0') &&
     (needle_keys == NULL || needle_keys[0] == '\0'))
    return TRUE;

  gboolean show = TRUE;

  // Check if path matches
  gchar *path = NULL;
  gtk_tree_model_get(model, iter, COL_PATH, &path, -1);
  if(needle_path && needle_path[0])
  {
    if(path && path[0] != '\0')
    {
      gchar *needle_ci = g_utf8_casefold(needle_path, -1);
      gchar *haystack_ci = g_utf8_casefold(path, -1);
      show &= (g_strrstr(haystack_ci, needle_ci) != NULL);
      g_free(needle_ci);
      g_free(haystack_ci);
      g_free(path);
    }
    else
    {
      show &= FALSE;
    }
  }

  // Check if keys match
  if(needle_keys && needle_keys[0] != '\0')
  {
    guint search_keyval = 0;
    GdkModifierType search_mods = 0;
    gtk_accelerator_parse(needle_keys, &search_keyval, &search_mods);
    if(search_keyval || search_mods)
    {
      guint keyval = 0;
      GdkModifierType mods = 0;
      gtk_tree_model_get(model, iter, COL_KEYVAL, &keyval, COL_MODS, &mods, -1);

      // If both keyval and mods are searched, use strict mode.
      // Else use fuzzy mode
      if(search_keyval && search_mods)
        show &= (keyval == search_keyval && mods == search_mods);
      else
        show &= ((keyval && keyval == search_keyval) || (mods && mods == search_mods));
    }
    else
    {
      // Parsing failed, keys/modifiers syntax is wrong: let user know
      show &= FALSE;
    }
  }

  if(show) return TRUE;

  // Check again recursively if any of the current item's children has an accel path matching
  if(gtk_tree_model_iter_has_child(model, iter))
  {
    GtkTreeIter child;
    if(gtk_tree_model_iter_children(model, &child, iter))
    {
      do
      {
        if(filter_callback(model, &child, user_data))
          return TRUE;
      } while(gtk_tree_model_iter_next(model, &child));
    }
  }

  return FALSE;
}

static void search_changed(GtkEntry *entry, gpointer user_data)
{
  _accel_window_params_t *params = (_accel_window_params_t *)user_data;
  GtkTreeView *tree_view = GTK_TREE_VIEW(params->tree_view);
  gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(gtk_tree_view_get_model(tree_view)));

  // Everything visible if needle is empty or NULL, aka no active search
  const gchar *needle_path = gtk_entry_get_text(GTK_ENTRY(params->path_search));
  const gchar *needle_keys = gtk_entry_get_text(GTK_ENTRY(params->keys_search));

  if((needle_path == NULL || needle_path[0] == '\0') &&
      (needle_keys == NULL || needle_keys[0] == '\0'))
    gtk_tree_view_collapse_all(GTK_TREE_VIEW(params->tree_view));
  else
    gtk_tree_view_expand_all(GTK_TREE_VIEW(params->tree_view));
}


void dt_accels_window(dt_accels_t *accels, GtkWindow *main_window)
{
  // Update the ugly global variable referencing accels
  accels_global_ref = accels;

  _accel_window_params_t *params = malloc(sizeof(_accel_window_params_t));
  params->keys_search = gtk_search_entry_new();
  params->path_search = gtk_search_entry_new();
  GtkWidget *tree_view = params->tree_view = gtk_tree_view_new();

  // Setup auto-completion on key modifiers because they are annoying
  // Note: omit the initial < character in modifier names as it is used to trigger matching
  // and won't be appended
  static dt_gtkentry_completion_spec default_path_compl_list[]
      = { { "Primary>", N_("<Primary> - Decoded as <Control> on Windows/Linux or <Meta> on Mac OS") },
          { "Control>", N_("<Control>") },
          { "Shift>", N_("<Shift>") },
          { "Alt>", N_("<Alt>") },
          { "Super>", N_("<Super> - The Windows key on PC") },
          { "Hyper>", N_("<Hyper>") },
          { "Meta>", N_("<Meta> - Decoded as <Command> on Mac OS") },
          { NULL, NULL } };
  dt_gtkentry_setup_completion(GTK_ENTRY(params->keys_search), default_path_compl_list, "<");
  gtk_widget_set_tooltip_text(params->keys_search, _("Look for keys and modifiers codes, as `<Modifier>Key`.\n"
                                                     "Type `<` to start the auto-completion"));

  gtk_widget_set_tooltip_text(params->path_search, _("Case-insensitive search for keywords of full pathes.\n"
                                                     "Ex: `darkroom/controls/sliders`"));

  // Set dialog window properties
  GtkWidget *dialog = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dialog), _("Ansel - Keyboard shortcuts"));

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
#endif

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), main_window);
  gtk_window_set_default_size(GTK_WINDOW(dialog), 1100, 900);

  // Create the full (non-filtered) tree view model
  GtkTreeStore *store = gtk_tree_store_new(NUM_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, GDK_TYPE_PIXBUF, G_TYPE_STRING,
                                           G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_UINT, G_TYPE_UINT);

  // Add a tree view row for each accel
  GHashTable *node_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  _accel_treeview_t _data = { .store = store , .node_cache = node_cache};
  g_hash_table_foreach(accels->acceleratables, _for_each_accel_create_treeview_row, &_data);
  g_hash_table_destroy(node_cache);

  // Sort rows alphabetically by path
  for(int i = COL_NAME; i < COL_KEYS; i++)
  {
    gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store), i, (GtkTreeIterCompareFunc)_sort_model_func,
                                    GINT_TO_POINTER(i), NULL);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), i, GTK_SORT_ASCENDING);
  }

  // Set the search feature, aka wire the Gtk search entry to a GtkTreeModelFilter
  GtkTreeModel *filter_model = gtk_tree_model_filter_new(GTK_TREE_MODEL(store), NULL);
  gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter_model), filter_callback, params, NULL);

  // So the content of the treeview is NOT the original (full) model, but the filtered one
  gtk_tree_view_set_model(GTK_TREE_VIEW(tree_view), filter_model);
  gtk_tree_view_set_tooltip_column(GTK_TREE_VIEW(tree_view), COL_PATH);
  gtk_widget_set_hexpand(tree_view, TRUE);
  gtk_widget_set_vexpand(tree_view, TRUE);
  gtk_widget_set_halign(tree_view, GTK_ALIGN_FILL);
  gtk_widget_set_valign(tree_view, GTK_ALIGN_FILL);

  g_signal_connect(G_OBJECT(params->path_search), "changed", G_CALLBACK(search_changed), params);
  g_signal_connect(G_OBJECT(params->keys_search), "changed", G_CALLBACK(search_changed), params);

  // Add tree view columns
  GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(_("View / Scope / Feature / Control"), gtk_cell_renderer_text_new(), "text", COL_NAME, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

  GtkCellRenderer *renderer = gtk_cell_renderer_accel_new();
  column = gtk_tree_view_column_new_with_attributes(_("Keys"), renderer, "accel-key", COL_KEYVAL, "accel-mods",
                                                    COL_MODS, NULL);
  gtk_tree_view_column_set_cell_data_func(column, renderer, _make_column_editable, NULL, NULL);
  g_signal_connect(renderer, "accel-edited", G_CALLBACK(_shortcut_edited), filter_model);
  gtk_tree_view_column_set_min_width(column, 100);
  gtk_tree_view_column_set_resizable(column, TRUE);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

  renderer = dtgtk_cell_renderer_button_new();
  g_object_set(renderer, "mode", GTK_CELL_RENDERER_MODE_ACTIVATABLE, NULL);
  column = gtk_tree_view_column_new_with_attributes(_("Clear"), renderer, "pixbuf", COL_CLEAR, NULL);
  gtk_tree_view_column_set_cell_data_func(column, renderer, _make_column_clearable, NULL, NULL);
  g_signal_connect(renderer, "activate", G_CALLBACK(_icon_activate), filter_model);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

  column = gtk_tree_view_column_new_with_attributes(_("Description"), gtk_cell_renderer_text_new(), "text", COL_DESCRIPTION, NULL);
  gtk_tree_view_append_column(GTK_TREE_VIEW(tree_view), column);

  // Pack and show widgets
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), box, TRUE, TRUE, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("Search by feature : ")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), params->path_search, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), gtk_label_new(_("Search by keys : ")), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), params->keys_search, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(box), hbox, FALSE, FALSE, 0);

  GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
  gtk_container_add(GTK_CONTAINER(scrolled_window), tree_view);
  gtk_box_pack_start(GTK_BOX(box), scrolled_window, TRUE, TRUE, 0);

  gtk_widget_set_visible(tree_view, TRUE);
  gtk_widget_show_all(dialog);

  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
  g_object_unref(filter_model);
  g_object_unref(store);
  g_free(params);
}

// Case-insensitive partial matching
// Return:
// - 0: perfect match
// - > 0: matches increasingly worse (rank)
// - -1: no match
static int _match_text(GtkTreeModel *model, GtkTreeIter *iter, const char *needle)
{
  int ret = -1;
  if(needle == NULL || needle[0] == '\0') return 0;

  // Get row entry
  gchar *label;
  gtk_tree_model_get(model, iter, 0, &label, -1);
  if(label == NULL || label[0] == '\0') return -1;

  // Convert to lowercase
  gchar *label_ci = g_utf8_casefold(label, -1);

  // Find match
  const char *match = g_strrstr(label_ci, needle);
  if(match != NULL)
  {
    // Index results by relevance.
    // Since pathes start generic and end specific, we posit that
    // most specific matches are most relevant results,
    // aka matching at the end of the path is more relevant than matching
    // at the start.
    const int match_pos = match - label_ci;
    const int relevance = strlen(label_ci) - match_pos;
    ret = relevance;
  }

  g_free(label);
  g_free(label_ci);

  return ret;
}

static void _find_and_rank_matches(GtkTreeModel *model, GtkWidget *search_entry)
{
  const gchar *needle = gtk_entry_get_text(GTK_ENTRY(search_entry));
  gchar *needle_ci = g_utf8_casefold(needle, -1);

  // Block sorting while we update the content of the column used to sort rows
  // otherwise that makes updating iterations recurse and ultimately fail
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model), GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
                                       GTK_SORT_ASCENDING);

  GtkTreeIter iter;
  if(gtk_tree_model_get_iter_first(model, &iter))
  {
    do
    {
      int rank = _match_text(model, &iter, needle_ci);
      gtk_list_store_set(GTK_LIST_STORE(model), &iter, 2, rank, -1);

    } while(gtk_tree_model_iter_next(model, &iter));
  }

  g_free(needle_ci);

  // Restore sorting
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(model), 2, GTK_SORT_ASCENDING);
  gtk_tree_sortable_sort_column_changed(GTK_TREE_SORTABLE(model));
}

// redo the suggestion list on each entry change
static void _search_entry_changed(GtkWidget *widget, gpointer user_data)
{
  _find_and_rank_matches(GTK_TREE_MODEL(user_data), widget);
}

// fire action callbacks even when they don't have a keyboard shortcut defined
static void _call_shortcut_cclosure(dt_shortcut_t *shortcut, GtkWindow *main_window)
{
  /*
    Accel callback signature is:
    `GtkAccelGroup *group, GObject *acceleratable, guint keyval, GdkModifierType mods, gpointer user_data`
    but `user_data` is handled in the closure already
  */
  GValue params[4] = { G_VALUE_INIT };

  g_value_init(&params[0], G_TYPE_POINTER);
  g_value_set_pointer(&params[0], shortcut->accel_group);

  g_value_init(&params[1], G_TYPE_POINTER);
  g_value_set_pointer(&params[1], G_OBJECT(main_window));

  g_value_init(&params[2], G_TYPE_UINT);
  g_value_set_uint(&params[2], shortcut->key);

  g_value_init(&params[3], G_TYPE_UINT);
  g_value_set_uint(&params[3], shortcut->mods);

  GValue ret = G_VALUE_INIT;
  g_value_init (&ret, G_TYPE_BOOLEAN);

  g_closure_invoke(dt_shortcut_get_closure(shortcut), &ret, 4, params, NULL);
}

static gboolean _run_action_from_shortcut(dt_shortcut_t *shortcut, GtkDialog *dialog, GtkWindow *main_window)
{
  if(dt_shortcut_get_closure(shortcut))
  {
    gtk_dialog_response(dialog, GTK_RESPONSE_ACCEPT);
    _call_shortcut_cclosure(shortcut, main_window);
    return TRUE;
  }
  else if(shortcut->widget)
  {
    gtk_dialog_response(dialog, GTK_RESPONSE_ACCEPT);
    gtk_widget_activate(shortcut->widget);
    return TRUE;
  }
  // should never happen
  return FALSE;
}

// Click on one of the suggestions
static gboolean _match_selected(GtkEntryCompletion *cmp, GtkTreeModel *model, GtkTreeIter *iter, gpointer user_data)
{
  GtkDialog *dialog = GTK_DIALOG(gtk_widget_get_ancestor(gtk_entry_completion_get_entry(cmp), GTK_TYPE_DIALOG));
  dt_shortcut_t *shortcut;
  gtk_tree_model_get(model, iter, 1, &shortcut, -1);
  return _run_action_from_shortcut(shortcut, dialog, user_data);
}

static gboolean _search_entry_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer user_data)
{
  if(event->keyval == GDK_KEY_Escape)
  {
    // Close the popup
    GtkDialog *dialog = GTK_DIALOG(gtk_widget_get_ancestor(widget, GTK_TYPE_DIALOG));
    gtk_dialog_response(dialog, GTK_RESPONSE_CANCEL);
    return TRUE;
  }
  return FALSE;
}

// Restrict the suggestions list to actual matches
gboolean _match_func(GtkEntryCompletion *completion, const gchar *key, GtkTreeIter *iter, gpointer user_data)
{
  GtkTreeModel *model = gtk_entry_completion_get_model(completion);
  int rank;
  gtk_tree_model_get(model, iter, 2, &rank, -1);
  return rank > -1;
}

void dt_accels_search(dt_accels_t *accels, GtkWindow *main_window)
{
  // Set dialog window properties
  GtkWidget *dialog = gtk_dialog_new();
  gtk_window_set_title(GTK_WINDOW(dialog), _("Ansel - Search accelerators"));

#ifdef GDK_WINDOWING_QUARTZ
  dt_osx_disallow_fullscreen(dialog);
#endif

  const int dialog_width = 800;
  const int dialog_height = 0;

  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  gtk_window_set_decorated(GTK_WINDOW(dialog), FALSE);
  gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER_ON_PARENT);
  gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
  gtk_window_set_transient_for(GTK_WINDOW(dialog), main_window);
  gtk_window_set_default_size(GTK_WINDOW(dialog), dialog_width, dialog_height);
  gtk_widget_set_name(dialog, "shortcut-search-dialog");

  // Build the list of currently-relevant shortcut pathes
  GtkListStore *store = gtk_list_store_new(7, G_TYPE_STRING, G_TYPE_POINTER, G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_UINT);
  g_hash_table_foreach(accels->acceleratables, _for_each_path_create_treeview_row, store);

  // Sort the filtered model by relevance
  gtk_tree_sortable_set_sort_func(GTK_TREE_SORTABLE(store), 2,
                                  (GtkTreeIterCompareFunc)_sort_model_by_relevance_func, NULL, NULL);
  gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store), 2, GTK_SORT_ASCENDING);

  // Build the search entry
  GtkWidget *search_entry = gtk_search_entry_new();
  gtk_grab_add(search_entry);
  gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), search_entry, TRUE, TRUE, 0);

  // Attach the completion list to the search entry
  GtkEntryCompletion *completion = gtk_entry_completion_new();
  gtk_entry_set_completion(GTK_ENTRY(search_entry), completion);
  gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(store));
  gtk_entry_completion_set_text_column(completion, 0);
  gtk_entry_completion_set_inline_completion(completion, TRUE);
  gtk_entry_completion_set_inline_selection(completion, FALSE);
  gtk_entry_completion_set_popup_completion(completion, TRUE);
  gtk_entry_completion_set_minimum_key_length(completion, 1);
  gtk_entry_completion_set_popup_single_match(completion, TRUE);
  gtk_entry_completion_set_match_func(completion, _match_func, NULL, NULL);

  // Completion cells rendering
  GtkCellRenderer *r2 = gtk_cell_renderer_text_new ();
  g_object_set(r2, "foreground", "#ccc", "xpad", 10,NULL);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(completion), r2, TRUE);
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(completion), r2, "text", 3);

  GtkCellRenderer *r1 = gtk_cell_renderer_accel_new ();
  g_object_set(r1, "editable", FALSE, "accel-mode", GTK_CELL_RENDERER_ACCEL_MODE_OTHER, "foreground", "#eee", "xpad", 10, NULL);
  gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(completion), r1, TRUE);
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(completion), r1, "accel-key", 5);
  gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(completion), r1, "accel-mods", 6);

  // Note: we can't set CSS classes on cell renderers, so hard-coded style it is

  // Style the main column
  GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(completion));
  GtkCellRenderer *txt = cells ? cells->data : NULL;
  if(txt)
    g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, "ellipsize-set", TRUE, "max-width-chars", 70, NULL);

  // Wire callbacks
  g_signal_connect(G_OBJECT(search_entry), "changed", G_CALLBACK(_search_entry_changed), store);
  g_signal_connect(G_OBJECT(search_entry), "key-press-event", G_CALLBACK(_search_entry_key_pressed), main_window);
  g_signal_connect(G_OBJECT(completion), "match-selected", G_CALLBACK(_match_selected), main_window);

  gtk_widget_show_all(dialog);

  // Manually-position the popup at the top of the window
  GtkAllocation tmp = { 0 };
  gtk_window_get_size(main_window, &tmp.width, &tmp.height);
  gdk_window_move_to_rect(GDK_WINDOW(gtk_widget_get_window(dialog)), &tmp, GDK_GRAVITY_NORTH, GDK_GRAVITY_NORTH, 0,
                          0, 0);

  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_grab_remove(search_entry);
  gtk_widget_destroy(dialog);
  g_object_unref(store);
}
