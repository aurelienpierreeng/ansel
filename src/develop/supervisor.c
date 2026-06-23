/*
    This file is part of Ansel.
    Copyright (C) 2026 Aurélien Pierre.

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

#include "develop/supervisor.h"
#include "common/image.h"            // dt_image_t
#include "common/introspection.h"    // dt_introspection_field_t
#include "develop/imageop.h"         // dt_iop_module_t (introspection accessors)
#include "develop/pixelpipe_cache.h" // DT_PIXELPIPE_CACHE_HASH_INVALID
#include "develop/pixelpipe_hb.h"    // dt_pixelpipe_get_pipe_name

#include <json-glib/json-glib.h>
#include <stdio.h>

// Which facets registered against a given hash. A node-output hash, its
// cacheline and the backbuffer that promotes it all share one key, so several
// facets coexist on the same entry.
typedef enum dt_sv_facet_t
{
  DT_SV_F_HISTORY = 1 << 0,
  DT_SV_F_NODE = 1 << 1,
  DT_SV_F_CACHE = 1 << 2,
  DT_SV_F_BACKBUF = 1 << 3,
  DT_SV_F_WIDGET = 1 << 4,
  DT_SV_F_THUMB = 1 << 5,
  DT_SV_F_MIPMAP = 1 << 6,
  DT_SV_F_IMAGE = 1 << 7,
} dt_sv_facet_t;

// One registry entry, identified by a single hash. All fields are value-copied;
// the registry never points at a live foreign object. Entries are never removed
// (registry as memory); `alive` tracks whether the represented object still
// exists in the application.
typedef struct dt_sv_entry_t
{
  uint64_t hash;
  guint facets;     // bitmask of dt_sv_facet_t
  gboolean alive;   // FALSE after the represented object was deleted/evicted

  // linkage edges (INVALID means "unset")
  uint64_t param_hash;   // node/cacheline -> history/module parameter identity
  uint64_t input_hash;   // cacheline      -> the input cacheline it consumes
  uint64_t node_hash;    // cacheline      -> producing topology node
  uint64_t history_hash; // backbuf        -> history-stack state
  uint64_t image_hash;   // mipmap         -> image cache object
  uint64_t pred_hash;    // node           -> predecessor node in the pipeline

  // descriptive metadata (copied in)
  char op_name[32];
  char multi_name[64];
  int multi_priority;
  int iop_order;
  int history_index;
  int pipe_type;
  int32_t imgid;
  int roi_w, roi_h;
  int devid;
  gboolean enabled;

  // cacheline facet
  size_t size;
  int owner_pipe_id;
  char cl_name[64];

  // backbuffer facet
  int bb_w, bb_h, bb_bpp;

  // widget facet
  char widget_tag[48];

  // thumbnail facet
  int mip;
  gboolean thumb_success;

  // mipmap facet
  char img_filename[128];
} dt_sv_entry_t;

// Cap on the retained GUI event log. ~200 B/event keeps this a few MB.
#define DT_SV_LOG_MAX 20000

gint dt_supervisor_recording = 0;

static struct
{
  GHashTable *entries; // uint64_t hash -> dt_sv_entry_t*
  GQueue *log;         // of dt_sv_logged_event_t*, oldest at head
  uint64_t next_seq;   // monotonic capture sequence
  dt_pthread_mutex_t lock;
  gboolean inited;
} _sv = { 0 };

static void _logged_event_free(gpointer p)
{
  dt_sv_logged_event_t *e = (dt_sv_logged_event_t *)p;
  if(!e) return;
  if(e->links) g_array_free(e->links, TRUE);
  g_free(e->json);
  g_free(e);
}

static inline gboolean _hash_is_set(const uint64_t h)
{
  return h != 0 && h != DT_PIXELPIPE_CACHE_HASH_INVALID;
}

static const char *_op_str(const dt_sv_op_t op)
{
  switch(op)
  {
    case DT_SV_CREATE: return "create";
    case DT_SV_UPDATE: return "update";
    case DT_SV_READ:   return "read";
    case DT_SV_DELETE: return "delete";
    default:           return "?";
  }
}

// FNV-1a over a string, mixed with two ints. Used for synthetic stable keys.
static uint64_t _fnv1a(const char *s, const int a, const int b)
{
  uint64_t h = 14695981039346656037ULL;
  for(const char *p = s; p && *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
  h ^= (uint64_t)(uint32_t)a; h *= 1099511628211ULL;
  h ^= (uint64_t)(uint32_t)b; h *= 1099511628211ULL;
  // never collide with the INVALID sentinel
  if(h == DT_PIXELPIPE_CACHE_HASH_INVALID) h ^= 1;
  return h;
}

uint64_t dt_supervisor_node_key(const int pipe_type, const char *op_name, const int multi_priority)
{
  return _fnv1a(op_name ? op_name : "?", pipe_type, multi_priority);
}

static uint64_t _thumb_key(const int32_t imgid, const int mip)
{
  return _fnv1a("thumbnail", imgid, mip);
}

uint64_t dt_supervisor_thumbnail_key(const int32_t imgid, const int mip)
{
  return _thumb_key(imgid, mip);
}

static uint64_t _mipmap_key(const int32_t imgid, const int mip)
{
  return _fnv1a("mipmap", imgid, mip);
}

uint64_t dt_supervisor_mipmap_key(const int32_t imgid, const int mip)
{
  return _mipmap_key(imgid, mip);
}

static uint64_t _image_key(const int32_t imgid)
{
  return _fnv1a("image", imgid, 0);
}

uint64_t dt_supervisor_image_key(const int32_t imgid)
{
  return _image_key(imgid);
}

// devid < 0 is the CPU path; otherwise an OpenCL device slot.
static void _device_string(const int devid, char *out, const size_t out_size)
{
  if(devid < 0) g_strlcpy(out, "cpu", out_size);
  else g_snprintf(out, out_size, "opencl-%d", devid);
}

static const char *_thread_tag(void)
{
  static __thread char tag[24];
  g_snprintf(tag, sizeof(tag), "thread-%p", (void *)g_thread_self());
  return tag;
}

void dt_supervisor_init(void)
{
  if(_sv.inited) return;
  _sv.entries = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, g_free);
  _sv.log = g_queue_new();
  dt_pthread_mutex_init(&_sv.lock, NULL);
  _sv.inited = TRUE;
}

void dt_supervisor_cleanup(void)
{
  if(!_sv.inited) return;
  dt_pthread_mutex_lock(&_sv.lock);
  g_hash_table_destroy(_sv.entries);
  _sv.entries = NULL;
  g_queue_free_full(_sv.log, _logged_event_free);
  _sv.log = NULL;
  dt_pthread_mutex_unlock(&_sv.lock);
  dt_pthread_mutex_destroy(&_sv.lock);
  _sv.inited = FALSE;
}

void dt_supervisor_set_recording(const gboolean on)
{
  g_atomic_int_set(&dt_supervisor_recording, on ? 1 : 0);
}

void dt_supervisor_events_clear(void)
{
  if(!_sv.inited) return;
  dt_pthread_mutex_lock(&_sv.lock);
  g_queue_free_full(_sv.log, _logged_event_free);
  _sv.log = g_queue_new();
  dt_pthread_mutex_unlock(&_sv.lock);
}

// Must be called with _sv.lock held. Returns the entry for `hash`, creating an
// empty one if needed. *created is set TRUE when a fresh entry was allocated.
static dt_sv_entry_t *_entry_get_locked(const uint64_t hash, gboolean *created)
{
  dt_sv_entry_t *e = (dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &hash);
  if(e) { if(created) *created = FALSE; return e; }

  e = (dt_sv_entry_t *)g_malloc0(sizeof(dt_sv_entry_t));
  e->hash = hash;
  e->alive = TRUE;
  e->input_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  e->param_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  e->node_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  e->history_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  e->image_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  e->pred_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  e->devid = -1;

  uint64_t *key = (uint64_t *)g_malloc(sizeof(uint64_t));
  *key = hash;
  g_hash_table_insert(_sv.entries, key, e);
  if(created) *created = TRUE;
  return e;
}

// Apply the alive/resurrection bookkeeping for an op. Returns TRUE if this op
// landed on a previously-deleted entry (reuse-after-delete).
static gboolean _touch_alive_locked(dt_sv_entry_t *e, const gboolean created, const dt_sv_op_t op)
{
  if(op == DT_SV_DELETE)
  {
    e->alive = FALSE;
    return FALSE;
  }
  const gboolean resurrected = !created && !e->alive;
  e->alive = TRUE;
  return resurrected;
}

// Domain string for an entry, by facet precedence (widget excluded: a widget
// never owns a hash, it only annotates the buffer it consumes).
static const char *_primary_domain(const dt_sv_entry_t *e)
{
  if(!e) return "unknown";
  if(e->facets & DT_SV_F_IMAGE)   return "image";
  if(e->facets & DT_SV_F_MIPMAP)  return "mipmap";
  if(e->facets & DT_SV_F_THUMB)   return "thumbnail";
  if(e->facets & DT_SV_F_BACKBUF) return "backbuf";
  if(e->facets & DT_SV_F_CACHE)   return "cacheline";
  if(e->facets & DT_SV_F_NODE)    return "node";
  if(e->facets & DT_SV_F_HISTORY) return "history";
  return "unknown";
}

// Pipe type to expose in the envelope: only for pipe-bound facets, 0/NONE omits.
static int _entry_pipe_type(const dt_sv_entry_t *e)
{
  if(e && e->pipe_type && (e->facets & (DT_SV_F_NODE | DT_SV_F_CACHE | DT_SV_F_BACKBUF)))
    return e->pipe_type;
  return -1;
}

// "module/instance" label for an entry, or "?" when unknown.
static void _module_label(const dt_sv_entry_t *e, char *out, const size_t out_size)
{
  if(e && e->op_name[0]) g_snprintf(out, out_size, "%s/%d", e->op_name, e->multi_priority);
  else g_strlcpy(out, "?", out_size);
}

static void _set_hash_member(JsonObject *o, const char *name, const uint64_t hash)
{
  char buf[32];
  g_snprintf(buf, sizeof(buf), "0x%016" G_GINT64_MODIFIER "x", hash);
  json_object_set_string_member(o, name, buf);
}

// Shallow JSON description of the registry entry at `hash`, for a resolved
// `input` / `params` / `consumes` edge. Called with _sv.lock held. NULL when
// the hash is unset or unknown.
static JsonObject *_resolve_locked(const uint64_t hash)
{
  if(!_hash_is_set(hash)) return NULL;
  const dt_sv_entry_t *e = (dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &hash);
  if(!e) return NULL;

  JsonObject *o = json_object_new();
  _set_hash_member(o, "hash", hash);

  char module[128];
  _module_label(e, module, sizeof(module));
  if(strcmp(module, "?") != 0) json_object_set_string_member(o, "module", module);

  if(e->facets & DT_SV_F_HISTORY)
  {
    json_object_set_int_member(o, "history_index", e->history_index);
    json_object_set_boolean_member(o, "enabled", e->enabled);
  }
  if(e->facets & (DT_SV_F_NODE | DT_SV_F_CACHE)) json_object_set_int_member(o, "iop_order", e->iop_order);
  json_object_set_boolean_member(o, "alive", e->alive);
  return o;
}

static uint64_t _parse_hash(const char *s)
{
  if(!s) return 0;
  return (uint64_t)g_ascii_strtoull(s, NULL, 0); // 0 base auto-detects the 0x prefix
}

// Build a retained event from the JSON root, extracting the navigable edges.
static dt_sv_logged_event_t *_extract_event(JsonObject *root, const gchar *json_str)
{
  dt_sv_logged_event_t *e = g_new0(dt_sv_logged_event_t, 1);
  if(json_object_has_member(root, "ts")) e->ts = json_object_get_double_member(root, "ts");
  if(json_object_has_member(root, "thread"))
    g_strlcpy(e->thread, json_object_get_string_member(root, "thread"), sizeof(e->thread));
  if(json_object_has_member(root, "op"))
    g_strlcpy(e->op, json_object_get_string_member(root, "op"), sizeof(e->op));
  if(json_object_has_member(root, "domain"))
    g_strlcpy(e->domain, json_object_get_string_member(root, "domain"), sizeof(e->domain));
  if(json_object_has_member(root, "hash"))
    e->hash = _parse_hash(json_object_get_string_member(root, "hash"));
  e->json = g_strdup(json_str);
  e->links = g_array_new(FALSE, FALSE, sizeof(dt_sv_link_t));

  // A short human mnemonic for the GUI row, picked per domain:
  //  - cacheline/node/history: module ("op/instance");  - widget: widget tag;
  //  - backbuf: pipe;  - mipmap/image/thumbnail: image id.
  const char *d = e->domain;
  if((!g_strcmp0(d, "cacheline") || !g_strcmp0(d, "node") || !g_strcmp0(d, "history"))
     && json_object_has_member(root, "module"))
    g_strlcpy(e->mnemonic, json_object_get_string_member(root, "module"), sizeof(e->mnemonic));
  else if(!g_strcmp0(d, "widget") && json_object_has_member(root, "widget"))
    g_strlcpy(e->mnemonic, json_object_get_string_member(root, "widget"), sizeof(e->mnemonic));
  else if(!g_strcmp0(d, "backbuf") && json_object_has_member(root, "pipe"))
    g_strlcpy(e->mnemonic, json_object_get_string_member(root, "pipe"), sizeof(e->mnemonic));
  else if((!g_strcmp0(d, "image") || !g_strcmp0(d, "mipmap")) && json_object_has_member(root, "filename"))
    g_strlcpy(e->mnemonic, json_object_get_string_member(root, "filename"), sizeof(e->mnemonic));
  else if((!g_strcmp0(d, "mipmap") || !g_strcmp0(d, "image") || !g_strcmp0(d, "thumbnail"))
          && json_object_has_member(root, "imgid"))
    g_snprintf(e->mnemonic, sizeof(e->mnemonic), "#%d", (int)json_object_get_int_member(root, "imgid"));

  // nested-object edges carry the linked object id in their "hash" member
  static const char *obj_edges[]
      = { "params", "input", "node", "consumes", "mipmap", "image", "predecessor", NULL };
  for(int i = 0; obj_edges[i]; i++)
  {
    if(!json_object_has_member(root, obj_edges[i])) continue;
    JsonObject *o = json_object_get_object_member(root, obj_edges[i]);
    if(o && json_object_has_member(o, "hash"))
    {
      dt_sv_link_t lk = { 0 };
      g_strlcpy(lk.label, obj_edges[i], sizeof(lk.label));
      lk.hash = _parse_hash(json_object_get_string_member(o, "hash"));
      g_array_append_val(e->links, lk);
    }
  }
  // the rekey chain links are plain hash strings
  static const char *str_edges[] = { "rekeyed_from", "rekeyed_to", NULL };
  for(int i = 0; str_edges[i]; i++)
  {
    if(!json_object_has_member(root, str_edges[i])) continue;
    dt_sv_link_t lk = { 0 };
    g_strlcpy(lk.label, str_edges[i], sizeof(lk.label));
    lk.hash = _parse_hash(json_object_get_string_member(root, str_edges[i]));
    g_array_append_val(e->links, lk);
  }
  return e;
}

static void _log_push(dt_sv_logged_event_t *e)
{
  dt_pthread_mutex_lock(&_sv.lock);
  if(_sv.log)
  {
    e->seq = ++_sv.next_seq;
    g_queue_push_tail(_sv.log, e);
    while(g_queue_get_length(_sv.log) > DT_SV_LOG_MAX)
      _logged_event_free(g_queue_pop_head(_sv.log));
  }
  else
    _logged_event_free(e);
  dt_pthread_mutex_unlock(&_sv.lock);
}

// Serialize `root` as one compact NDJSON line. Takes ownership of root. Dumps to
// stderr when `-d supervisor` is set, and captures to the GUI log when recording.
static void _emit_line(JsonObject *root)
{
  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_take_object(node, root);
  JsonGenerator *gen = json_generator_new();
  json_generator_set_root(gen, node);
  json_generator_set_pretty(gen, FALSE);

  gsize len = 0;
  gchar *str = json_generator_to_data(gen, &len);

  if(darktable.unmuted & DT_DEBUG_SUPERVISOR)
  {
    fprintf(stderr, "%s\n", str);
    fflush(stderr);
  }
  if(g_atomic_int_get(&dt_supervisor_recording))
    _log_push(_extract_event(root, str));

  g_free(str);
  g_object_unref(gen);
  json_node_free(node);
}

// Recursively render one introspection field's value as "path = value" strings
// into `out`. Mirrors the offset handling of libs/history.c's tooltip walker but
// dumps absolute values (not diffs).
static void _introspect_dump(dt_introspection_field_t *field, const char *prefix, gpointer params,
                             JsonArray *out)
{
  if(!field) return;
  void *p = (uint8_t *)params + field->header.offset;
  const char *name = prefix ? prefix : "";

  switch(field->header.type)
  {
    case DT_INTROSPECTION_TYPE_STRUCT:
    case DT_INTROSPECTION_TYPE_UNION:
      for(int i = 0; i < (int)field->Struct.entries; i++)
      {
        dt_introspection_field_t *e = field->Struct.fields[i];
        const char *nm = (e->header.description && *e->header.description) ? e->header.description
                                                                          : e->header.field_name;
        gchar *pre = prefix ? g_strdup_printf("%s.%s", prefix, nm) : g_strdup(nm);
        _introspect_dump(e, pre, params, out); // struct fields share the params base
        g_free(pre);
      }
      break;
    case DT_INTROSPECTION_TYPE_ARRAY:
      if(field->Array.type == DT_INTROSPECTION_TYPE_CHAR)
      {
        char *s = (char *)p;
        if(g_utf8_validate(s, -1, NULL))
        {
          gchar *t = g_strdup_printf("%s = \"%s\"", name, s);
          json_array_add_string_element(out, t);
          g_free(t);
        }
      }
      else
      {
        for(int i = 0, off = 0; i < (int)field->Array.count && i < 8;
            i++, off += field->Array.field->header.size)
        {
          gchar *pre = g_strdup_printf("%s[%d]", name, i);
          _introspect_dump(field->Array.field, pre, (uint8_t *)params + off, out);
          g_free(pre);
        }
      }
      break;
    case DT_INTROSPECTION_TYPE_FLOAT:
    {
      gchar *t = g_strdup_printf("%s = %.4f", name, *(float *)p);
      json_array_add_string_element(out, t);
      g_free(t);
      break;
    }
    case DT_INTROSPECTION_TYPE_DOUBLE:
    {
      gchar *t = g_strdup_printf("%s = %.4f", name, *(double *)p);
      json_array_add_string_element(out, t);
      g_free(t);
      break;
    }
    case DT_INTROSPECTION_TYPE_INT:
    case DT_INTROSPECTION_TYPE_SHORT:
    {
      const int val = field->header.type == DT_INTROSPECTION_TYPE_SHORT ? *(short *)p : *(int *)p;
      gchar *t = g_strdup_printf("%s = %d", name, val);
      json_array_add_string_element(out, t);
      g_free(t);
      break;
    }
    case DT_INTROSPECTION_TYPE_UINT:
    case DT_INTROSPECTION_TYPE_USHORT:
    {
      const unsigned val = field->header.type == DT_INTROSPECTION_TYPE_USHORT ? *(unsigned short *)p
                                                                              : *(unsigned int *)p;
      gchar *t = g_strdup_printf("%s = %u", name, val);
      json_array_add_string_element(out, t);
      g_free(t);
      break;
    }
    case DT_INTROSPECTION_TYPE_INT8:
    {
      gchar *t = g_strdup_printf("%s = %d", name, (int)*(int8_t *)p);
      json_array_add_string_element(out, t);
      g_free(t);
      break;
    }
    case DT_INTROSPECTION_TYPE_UINT8:
    {
      gchar *t = g_strdup_printf("%s = %u", name, (unsigned)*(uint8_t *)p);
      json_array_add_string_element(out, t);
      g_free(t);
      break;
    }
    case DT_INTROSPECTION_TYPE_BOOL:
    {
      gchar *t = g_strdup_printf("%s = %s", name, *(gboolean *)p ? "on" : "off");
      json_array_add_string_element(out, t);
      g_free(t);
      break;
    }
    case DT_INTROSPECTION_TYPE_ENUM:
    {
      const int val = *(int *)p;
      const char *str = "?";
      for(dt_introspection_type_enum_tuple_t *i = field->Enum.values; i && i->name; i++)
        if(i->value == val)
        {
          str = (i->description && *i->description) ? i->description : i->name;
          break;
        }
      gchar *t = g_strdup_printf("%s = %s", name, str);
      json_array_add_string_element(out, t);
      g_free(t);
      break;
    }
    default:
      break; // OPAQUE / LONG / FLOATCOMPLEX etc. are not rendered
  }
}

// Build a "parameters" JSON array (human-legible) from a module's introspection.
static JsonArray *_params_json(const dt_iop_module_t *module, const void *params)
{
  if(!module || !params || !module->have_introspection || !module->get_introspection) return NULL;
  dt_introspection_t *intro = module->get_introspection();
  if(!intro || !intro->field) return NULL;
  JsonArray *out = json_array_new();
  _introspect_dump(intro->field, NULL, (gpointer)params, out);
  if(json_array_get_length(out) == 0)
  {
    json_array_unref(out);
    return NULL;
  }
  return out;
}

// Common envelope shared by every event. `resurrected` adds the reuse-after-
// delete marker. Pass pipe_type < 0 to omit the pipe field.
static JsonObject *_envelope(const dt_sv_op_t op, const char *domain, const uint64_t hash,
                             const int pipe_type, const int32_t imgid, const gboolean alive,
                             const gboolean resurrected)
{
  JsonObject *o = json_object_new();
  json_object_set_double_member(o, "ts", dt_get_wtime() - darktable.start_wtime);
  json_object_set_string_member(o, "thread", _thread_tag());
  json_object_set_string_member(o, "op", _op_str(op));
  json_object_set_string_member(o, "domain", domain);
  if(pipe_type >= 0)
    json_object_set_string_member(o, "pipe", dt_pixelpipe_get_pipe_name((dt_dev_pixelpipe_type_t)pipe_type));
  if(imgid > 0) json_object_set_int_member(o, "imgid", imgid);
  _set_hash_member(o, "hash", hash);
  json_object_set_boolean_member(o, "alive", alive);
  if(resurrected) json_object_set_boolean_member(o, "resurrected", TRUE);
  return o;
}

void dt_supervisor_history(const dt_sv_op_t op, const uint64_t param_hash, const char *op_name,
                           const int multi_priority, const char *multi_name, const int iop_order,
                           const int history_index, const int32_t imgid, const gboolean enabled,
                           const dt_iop_module_t *module, const void *params)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  // Render the parameters outside the lock (introspection only reads the module).
  JsonArray *parameters = _params_json(module, params);

  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(param_hash, &created);
  e->facets |= DT_SV_F_HISTORY;
  if(op_name) g_strlcpy(e->op_name, op_name, sizeof(e->op_name));
  if(multi_name) g_strlcpy(e->multi_name, multi_name, sizeof(e->multi_name));
  e->multi_priority = multi_priority;
  e->iop_order = iop_order;
  e->history_index = history_index;
  e->imgid = imgid;
  e->enabled = enabled;
  const gboolean resurrected = _touch_alive_locked(e, created, op);

  // history is not tied to a pipeline: pass -1 so the envelope omits the pipe field
  JsonObject *root = _envelope(op, "history", param_hash, -1, imgid, e->alive, resurrected);
  char module_label[128];
  _module_label(e, module_label, sizeof(module_label));
  json_object_set_string_member(root, "module", module_label);
  json_object_set_int_member(root, "iop_order", iop_order);
  json_object_set_int_member(root, "history_index", history_index);
  json_object_set_boolean_member(root, "enabled", enabled);
  if(parameters) json_object_set_array_member(root, "parameters", parameters);
  // Hold the image filename (borrowed from the registered image object).
  if(imgid > 0)
  {
    const uint64_t image_key = _image_key(imgid);
    const dt_sv_entry_t *image_e = (const dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &image_key);
    if(image_e && image_e->img_filename[0])
      json_object_set_string_member(root, "filename", image_e->img_filename);
  }
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

void dt_supervisor_node(const dt_sv_op_t op, const uint64_t node_hash, const uint64_t param_hash,
                        const uint64_t predecessor_hash, const char *op_name, const int multi_priority,
                        const int iop_order, const int pipe_type, const int32_t imgid)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(node_hash, &created);
  e->facets |= DT_SV_F_NODE;
  if(op_name) g_strlcpy(e->op_name, op_name, sizeof(e->op_name));
  e->multi_priority = multi_priority;
  e->iop_order = iop_order;
  e->pipe_type = pipe_type;
  e->imgid = imgid;
  // Bind the node to its history item once synchronized (param_hash set).
  if(_hash_is_set(param_hash)) e->param_hash = param_hash;
  // Reference the predecessor node in the pipeline (set at topology creation).
  if(_hash_is_set(predecessor_hash)) e->pred_hash = predecessor_hash;
  const gboolean resurrected = _touch_alive_locked(e, created, op);

  JsonObject *root = _envelope(op, "node", node_hash, pipe_type, imgid, e->alive, resurrected);
  char module[128];
  _module_label(e, module, sizeof(module));
  json_object_set_string_member(root, "module", module);
  json_object_set_int_member(root, "iop_order", iop_order);
  JsonObject *params = _resolve_locked(e->param_hash);
  if(params) json_object_set_object_member(root, "params", params);
  JsonObject *pred = _resolve_locked(e->pred_hash);
  if(pred) json_object_set_object_member(root, "predecessor", pred);
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

void dt_supervisor_cacheline_create(const uint64_t hash, const uint64_t node_hash,
                                    const uint64_t param_hash, const uint64_t input_hash,
                                    const char *op_name, const int multi_priority, const int iop_order,
                                    const int pipe_type, const int32_t imgid, const int roi_w,
                                    const int roi_h, const int devid, const size_t size,
                                    const char *name)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(hash, &created);
  e->facets |= DT_SV_F_CACHE;
  e->node_hash = node_hash;
  // Prefer the producing node's history binding (set at synchronization) over the
  // passed piece->hash: the latter folds in runtime_data_hash() for some modules
  // and then no longer matches the history entry key. Fall back when the node is
  // not bound yet (e.g. default-param node, or recording started mid-session).
  const dt_sv_entry_t *node_e = (const dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &node_hash);
  e->param_hash = (node_e && _hash_is_set(node_e->param_hash)) ? node_e->param_hash : param_hash;
  e->input_hash = input_hash;
  if(op_name) g_strlcpy(e->op_name, op_name, sizeof(e->op_name));
  e->multi_priority = multi_priority;
  e->iop_order = iop_order;
  e->pipe_type = pipe_type;
  e->imgid = imgid;
  e->roi_w = roi_w;
  e->roi_h = roi_h;
  e->devid = devid;
  e->size = size;
  if(name) g_strlcpy(e->cl_name, name, sizeof(e->cl_name));
  const gboolean resurrected = _touch_alive_locked(e, created, DT_SV_CREATE);

  JsonObject *root = _envelope(DT_SV_CREATE, "cacheline", hash, pipe_type, imgid, e->alive, resurrected);
  char module[128];
  _module_label(e, module, sizeof(module));
  json_object_set_string_member(root, "module", module);
  json_object_set_int_member(root, "iop_order", iop_order);
  json_object_set_int_member(root, "size", (gint64)size);
  if(e->cl_name[0]) json_object_set_string_member(root, "name", e->cl_name);

  char dev[32];
  _device_string(devid, dev, sizeof(dev));
  json_object_set_string_member(root, "device", dev);

  JsonArray *roi = json_array_new();
  json_array_add_int_element(roi, roi_w);
  json_array_add_int_element(roi, roi_h);
  json_object_set_array_member(root, "roi", roi);

  JsonObject *params = _resolve_locked(e->param_hash); // node-bound history key
  if(params) json_object_set_object_member(root, "params", params);
  JsonObject *in = _resolve_locked(input_hash);
  if(in) json_object_set_object_member(root, "input", in);
  JsonObject *node = _resolve_locked(node_hash);
  if(node) json_object_set_object_member(root, "node", node);
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

void dt_supervisor_cacheline_read(const uint64_t hash, const size_t size)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(hash, &created);
  e->facets |= DT_SV_F_CACHE;
  if(size) e->size = size;
  const gboolean resurrected = _touch_alive_locked(e, created, DT_SV_READ);

  JsonObject *root = _envelope(DT_SV_READ, "cacheline", hash, e->pipe_type ? e->pipe_type : -1,
                               e->imgid, e->alive, resurrected);
  json_object_set_int_member(root, "size", (gint64)e->size);
  char module[128];
  _module_label(e, module, sizeof(module));
  if(strcmp(module, "?") != 0) json_object_set_string_member(root, "module", module);
  // Expose the full linkage so every related hash is clickable from a read too.
  JsonObject *params = _resolve_locked(e->param_hash);
  if(params) json_object_set_object_member(root, "params", params);
  JsonObject *node = _resolve_locked(e->node_hash);
  if(node) json_object_set_object_member(root, "node", node);
  JsonObject *in = _resolve_locked(e->input_hash);
  if(in) json_object_set_object_member(root, "input", in);
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

void dt_supervisor_cacheline_delete(const uint64_t hash, const size_t size, const int owner_pipe_id,
                                    const char *name)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(hash, &created);
  e->facets |= DT_SV_F_CACHE;
  if(size) e->size = size;
  if(owner_pipe_id) e->owner_pipe_id = owner_pipe_id;
  if(name) g_strlcpy(e->cl_name, name, sizeof(e->cl_name));
  _touch_alive_locked(e, created, DT_SV_DELETE);

  JsonObject *root = _envelope(DT_SV_DELETE, "cacheline", hash, e->pipe_type ? e->pipe_type : -1,
                               e->imgid, e->alive, FALSE);
  json_object_set_int_member(root, "size", (gint64)e->size);
  if(e->cl_name[0]) json_object_set_string_member(root, "name", e->cl_name);
  json_object_set_int_member(root, "owner_pipe", e->owner_pipe_id);
  char module[128];
  _module_label(e, module, sizeof(module));
  if(strcmp(module, "?") != 0) json_object_set_string_member(root, "module", module);
  JsonObject *params = _resolve_locked(e->param_hash);
  if(params) json_object_set_object_member(root, "params", params);
  JsonObject *node = _resolve_locked(e->node_hash);
  if(node) json_object_set_object_member(root, "node", node);
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

// Build one side of a rekey record (called with _sv.lock held).
static JsonObject *_rekey_record(const dt_sv_op_t op, const dt_sv_entry_t *e, const uint64_t hash,
                                 const char *link_field, const uint64_t link_hash, const gboolean alive)
{
  JsonObject *o = _envelope(op, _primary_domain(e), hash, _entry_pipe_type(e), e->imgid, alive, FALSE);
  char module[128];
  _module_label(e, module, sizeof(module));
  if(strcmp(module, "?") != 0) json_object_set_string_member(o, "module", module);
  _set_hash_member(o, link_field, link_hash);
  return o;
}

void dt_supervisor_rekey(const uint64_t old_hash, const uint64_t new_hash)
{
  if(!dt_supervisor_active() || !_sv.inited) return;
  if(!_hash_is_set(old_hash) || !_hash_is_set(new_hash) || old_hash == new_hash) return;

  dt_pthread_mutex_lock(&_sv.lock);
  dt_sv_entry_t *old = (dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &old_hash);
  gboolean created;
  dt_sv_entry_t *new_e = _entry_get_locked(new_hash, &created);

  if(old)
  {
    // The new key is the same logical object: inherit the old metadata.
    new_e->facets |= old->facets;
    new_e->param_hash = old->param_hash;
    new_e->input_hash = old->input_hash;
    new_e->node_hash = old->node_hash;
    new_e->history_hash = old->history_hash;
    g_strlcpy(new_e->op_name, old->op_name, sizeof(new_e->op_name));
    g_strlcpy(new_e->multi_name, old->multi_name, sizeof(new_e->multi_name));
    new_e->multi_priority = old->multi_priority;
    new_e->iop_order = old->iop_order;
    new_e->history_index = old->history_index;
    new_e->pipe_type = old->pipe_type;
    new_e->imgid = old->imgid;
    new_e->roi_w = old->roi_w;
    new_e->roi_h = old->roi_h;
    new_e->devid = old->devid;
    new_e->enabled = old->enabled;
    new_e->size = old->size;
    g_strlcpy(new_e->cl_name, old->cl_name, sizeof(new_e->cl_name));
    new_e->bb_w = old->bb_w;
    new_e->bb_h = old->bb_h;
    new_e->bb_bpp = old->bb_bpp;
    old->alive = FALSE;
  }
  new_e->alive = TRUE;

  JsonObject *old_rec = old ? _rekey_record(DT_SV_DELETE, old, old_hash, "rekeyed_to", new_hash, FALSE) : NULL;
  JsonObject *new_rec = _rekey_record(DT_SV_CREATE, new_e, new_hash, "rekeyed_from", old_hash, TRUE);
  dt_pthread_mutex_unlock(&_sv.lock);

  if(old_rec) _emit_line(old_rec);
  _emit_line(new_rec);
}

void dt_supervisor_backbuf(const dt_sv_op_t op, const uint64_t hash, const uint64_t history_hash,
                           const int w, const int h, const int bpp, const int pipe_type,
                           const int devid)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(hash, &created);
  e->facets |= DT_SV_F_BACKBUF;
  e->history_hash = history_hash;
  e->bb_w = w;
  e->bb_h = h;
  e->bb_bpp = bpp;
  e->pipe_type = pipe_type;
  if(devid >= 0) e->devid = devid;
  const gboolean resurrected = _touch_alive_locked(e, created, op);

  JsonObject *root = _envelope(op, "backbuf", hash, pipe_type, e->imgid, e->alive, resurrected);
  char dev[32];
  _device_string(e->devid, dev, sizeof(dev));
  json_object_set_string_member(root, "device", dev);
  _set_hash_member(root, "history_hash", history_hash);

  JsonArray *size = json_array_new();
  json_array_add_int_element(size, w);
  json_array_add_int_element(size, h);
  json_array_add_int_element(size, bpp);
  json_object_set_array_member(root, "size", size);

  // The backbuf hash is the producing node output hash, so it shares this entry.
  char module[128];
  _module_label(e, module, sizeof(module));
  if(strcmp(module, "?") != 0) json_object_set_string_member(root, "module", module);
  JsonObject *params = _resolve_locked(e->param_hash);
  if(params) json_object_set_object_member(root, "params", params);
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

void dt_supervisor_widget(const dt_sv_op_t op, const char *widget_tag, const uint64_t consumed_hash,
                          const int pipe_type, const int32_t imgid)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  dt_pthread_mutex_lock(&_sv.lock);
  // The widget does not own `consumed_hash`; only annotate the buffer entry.
  dt_sv_entry_t *src = (dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &consumed_hash);
  if(src) src->facets |= DT_SV_F_WIDGET;

  JsonObject *root = _envelope(op, "widget", consumed_hash, pipe_type, imgid,
                               src ? src->alive : TRUE, FALSE);
  if(widget_tag) json_object_set_string_member(root, "widget", widget_tag);
  JsonObject *consumed = _resolve_locked(consumed_hash);
  if(consumed) json_object_set_object_member(root, "consumes", consumed);
  JsonObject *params = src ? _resolve_locked(src->param_hash) : NULL;
  if(params) json_object_set_object_member(root, "params", params);
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

void dt_supervisor_thumbnail(const dt_sv_op_t op, const int32_t imgid, const int width,
                             const int height, const int mip, const gboolean success)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  const uint64_t key = _thumb_key(imgid, mip);
  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(key, &created);
  e->facets |= DT_SV_F_THUMB;
  e->imgid = imgid;
  e->roi_w = width;
  e->roi_h = height;
  e->mip = mip;
  e->thumb_success = success;
  const gboolean resurrected = _touch_alive_locked(e, created, op);

  JsonObject *root = _envelope(op, "thumbnail", key, -1, imgid, e->alive, resurrected);
  json_object_set_int_member(root, "mip", mip);
  json_object_set_boolean_member(root, "success", success);
  JsonArray *size = json_array_new();
  json_array_add_int_element(size, width);
  json_array_add_int_element(size, height);
  json_object_set_array_member(root, "size", size);

  // Reference the mipmap object this thumbnail displays (by imgid + mip), so the
  // displayed mipmap hash is visible and clickable.
  const uint64_t mk = _mipmap_key(imgid, mip);
  JsonObject *mm = json_object_new();
  _set_hash_member(mm, "hash", mk);
  json_object_set_int_member(mm, "mip", mip);
  const dt_sv_entry_t *me = (const dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &mk);
  if(me)
  {
    json_object_set_boolean_member(mm, "alive", me->alive);
    if(me->img_filename[0]) json_object_set_string_member(mm, "filename", me->img_filename);
  }
  json_object_set_object_member(root, "mipmap", mm);
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

// A curated subset of dt_image_t: the mipmap object's "properties".
static JsonObject *_image_properties_json(const dt_image_t *img)
{
  JsonObject *o = json_object_new();
  json_object_set_int_member(o, "id", img->id);
  json_object_set_string_member(o, "filename", img->filename);
  json_object_set_string_member(o, "folder", img->folder);
  json_object_set_int_member(o, "film_id", img->film_id);
  json_object_set_int_member(o, "group_id", img->group_id);
  json_object_set_int_member(o, "version", img->version);

  JsonArray *dim = json_array_new();
  json_array_add_int_element(dim, img->width);
  json_array_add_int_element(dim, img->height);
  json_object_set_array_member(o, "dimensions", dim);
  JsonArray *pdim = json_array_new();
  json_array_add_int_element(pdim, img->p_width);
  json_array_add_int_element(pdim, img->p_height);
  json_object_set_array_member(o, "processed", pdim);

  char flags[16];
  g_snprintf(flags, sizeof(flags), "0x%08x", (unsigned)img->flags);
  json_object_set_string_member(o, "flags", flags);
  json_object_set_int_member(o, "rating", img->flags & 0x7);
  json_object_set_int_member(o, "orientation", (int)img->orientation);
  json_object_set_int_member(o, "loader", (int)img->loader);

  json_object_set_string_member(o, "camera", img->camera_makermodel);
  json_object_set_string_member(o, "lens", img->exif_lens);
  json_object_set_string_member(o, "datetime", img->datetime);
  json_object_set_double_member(o, "iso", img->exif_iso);
  json_object_set_double_member(o, "aperture", img->exif_aperture);
  json_object_set_double_member(o, "exposure", img->exif_exposure);
  json_object_set_double_member(o, "focal_length", img->exif_focal_length);
  return o;
}

void dt_supervisor_mipmap(const dt_sv_op_t op, const int32_t imgid, const int mip)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  const uint64_t key = _mipmap_key(imgid, mip);
  const uint64_t image_key = _image_key(imgid);
  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(key, &created);
  e->facets |= DT_SV_F_MIPMAP;
  e->imgid = imgid;
  e->mip = mip;
  e->image_hash = image_key;
  const gboolean resurrected = _touch_alive_locked(e, created, op);

  JsonObject *root = _envelope(op, "mipmap", key, -1, imgid, e->alive, resurrected);
  json_object_set_int_member(root, "mip", mip);
  // Link to the image cache object instead of duplicating its dt_image_t values.
  // Borrow only the filename (for the row mnemonic) from the linked image entry.
  const dt_sv_entry_t *image_e = (const dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &image_key);
  if(image_e && image_e->img_filename[0])
    json_object_set_string_member(root, "filename", image_e->img_filename);
  JsonObject *im = _resolve_locked(image_key);
  if(!im)
  {
    im = json_object_new();
    _set_hash_member(im, "hash", image_key);
  }
  json_object_set_object_member(root, "image", im);
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

void dt_supervisor_image(const dt_sv_op_t op, const int32_t imgid, const dt_image_t *img)
{
  if(!dt_supervisor_active() || !_sv.inited) return;

  const uint64_t key = _image_key(imgid);
  dt_pthread_mutex_lock(&_sv.lock);
  gboolean created;
  dt_sv_entry_t *e = _entry_get_locked(key, &created);
  e->facets |= DT_SV_F_IMAGE;
  e->imgid = imgid;
  if(img && img->filename[0]) g_strlcpy(e->img_filename, img->filename, sizeof(e->img_filename));
  // The image cache `allocate` callback (the natural create point) only fires on
  // a miss, but images are usually already cached when a consumer (e.g. a mipmap)
  // first references them. So the first time we see an image, treat it as a
  // create regardless of the caller's intent, so it exists for navigation.
  const dt_sv_op_t eff_op = created ? DT_SV_CREATE : op;
  const gboolean resurrected = _touch_alive_locked(e, created, eff_op);

  JsonObject *root = _envelope(eff_op, "image", key, -1, imgid, e->alive, resurrected);
  if(e->img_filename[0]) json_object_set_string_member(root, "filename", e->img_filename);
  if(img) json_object_set_object_member(root, "properties", _image_properties_json(img));
  dt_pthread_mutex_unlock(&_sv.lock);

  _emit_line(root);
}

// Append " 0xHASH (module)" for a resolved edge, marking dead entries.
static void _describe_edge(GString *s, const char *prefix, const uint64_t hash)
{
  if(!_hash_is_set(hash)) return;
  const dt_sv_entry_t *e = (dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &hash);
  if(!e) return;
  char module[128];
  _module_label(e, module, sizeof(module));
  g_string_append_printf(s, "%s 0x%016" G_GINT64_MODIFIER "x", prefix, hash);
  if(strcmp(module, "?") != 0) g_string_append_printf(s, " (%s)", module);
  if(!e->alive) g_string_append(s, " [deleted]");
}

gchar *dt_supervisor_describe(const uint64_t hash)
{
  if(!dt_supervisor_active() || !_sv.inited) return NULL;

  dt_pthread_mutex_lock(&_sv.lock);
  const dt_sv_entry_t *e = (dt_sv_entry_t *)g_hash_table_lookup(_sv.entries, &hash);
  if(!e)
  {
    dt_pthread_mutex_unlock(&_sv.lock);
    return NULL;
  }

  GString *s = g_string_new(NULL);
  char module[128];
  _module_label(e, module, sizeof(module));
  const char *pipe = (e->pipe_type >= 0 && (e->facets & (DT_SV_F_NODE | DT_SV_F_CACHE | DT_SV_F_BACKBUF)))
                         ? dt_pixelpipe_get_pipe_name((dt_dev_pixelpipe_type_t)e->pipe_type)
                         : NULL;
  char dev[32];
  _device_string(e->devid, dev, sizeof(dev));

  if(e->facets & DT_SV_F_IMAGE)
  {
    g_string_append_printf(s, "image #%d", e->imgid);
    if(e->img_filename[0]) g_string_append_printf(s, " (%s)", e->img_filename);
  }
  else if(e->facets & DT_SV_F_MIPMAP)
  {
    g_string_append_printf(s, "mipmap image #%d mip %d", e->imgid, e->mip);
    if(e->img_filename[0]) g_string_append_printf(s, " (%s)", e->img_filename);
  }
  else if(e->facets & DT_SV_F_THUMB)
  {
    g_string_append_printf(s, "thumbnail for image #%d (%dx%d, mip %d) %s", e->imgid, e->roi_w,
                           e->roi_h, e->mip, e->thumb_success ? "ready" : "pending");
  }
  else if(e->facets & DT_SV_F_BACKBUF)
  {
    g_string_append_printf(s, "backbuffer 0x%016" G_GINT64_MODIFIER "x", hash);
    if(strcmp(module, "?") != 0) g_string_append_printf(s, " (%s)", module);
    g_string_append_printf(s, " published by %s pipe (%dx%dx%d, %s)", pipe ? pipe : "?", e->bb_w,
                           e->bb_h, e->bb_bpp, dev);
    _describe_edge(s, "; params", e->param_hash);
  }
  else if(e->facets & DT_SV_F_CACHE)
  {
    g_string_append_printf(s, "cacheline 0x%016" G_GINT64_MODIFIER "x", hash);
    if(strcmp(module, "?") != 0) g_string_append_printf(s, " (%s", module);
    g_string_append_printf(s, ", %s, %dx%d, %s)", pipe ? pipe : "?", e->roi_w, e->roi_h, dev);
    _describe_edge(s, " computed from input", e->input_hash);
    _describe_edge(s, "; params", e->param_hash);
  }
  else if(e->facets & DT_SV_F_NODE)
  {
    g_string_append_printf(s, "node 0x%016" G_GINT64_MODIFIER "x (%s, %s pipe, iop_order %d)", hash,
                           module, pipe ? pipe : "?", e->iop_order);
  }
  else if(e->facets & DT_SV_F_HISTORY)
  {
    g_string_append_printf(s, "history state #%d: %s (%s)", e->history_index, module,
                           e->enabled ? "enabled" : "disabled");
  }
  else
  {
    g_string_append_printf(s, "0x%016" G_GINT64_MODIFIER "x", hash);
  }

  if(!e->alive) g_string_append(s, " [DELETED]");
  dt_pthread_mutex_unlock(&_sv.lock);

  return g_string_free(s, FALSE);
}

static dt_sv_logged_event_t *_event_copy(const dt_sv_logged_event_t *src)
{
  dt_sv_logged_event_t *c = g_new0(dt_sv_logged_event_t, 1);
  *c = *src; // copies scalars and the fixed char arrays
  c->json = g_strdup(src->json);
  c->links = g_array_new(FALSE, FALSE, sizeof(dt_sv_link_t));
  if(src->links && src->links->len)
    g_array_append_vals(c->links, src->links->data, src->links->len);
  return c;
}

GPtrArray *dt_supervisor_events_snapshot(void)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(_logged_event_free);
  if(!_sv.inited) return out;

  dt_pthread_mutex_lock(&_sv.lock);
  for(GList *l = _sv.log ? _sv.log->head : NULL; l; l = l->next)
    g_ptr_array_add(out, _event_copy((const dt_sv_logged_event_t *)l->data));
  dt_pthread_mutex_unlock(&_sv.lock);
  return out;
}

GPtrArray *dt_supervisor_events_snapshot_since(const uint64_t after_seq, uint64_t *out_last_seq)
{
  GPtrArray *out = g_ptr_array_new_with_free_func(_logged_event_free);
  if(out_last_seq) *out_last_seq = after_seq;
  if(!_sv.inited) return out;

  dt_pthread_mutex_lock(&_sv.lock);
  // New events are at the tail; walk back until we reach a seq we already have.
  for(GList *l = _sv.log ? _sv.log->tail : NULL; l; l = l->prev)
  {
    const dt_sv_logged_event_t *ev = (const dt_sv_logged_event_t *)l->data;
    if(ev->seq <= after_seq) break;
    g_ptr_array_add(out, _event_copy(ev));
  }
  dt_pthread_mutex_unlock(&_sv.lock);

  // collected newest-first: reverse to oldest-first
  for(guint i = 0, j = out->len ? out->len - 1 : 0; i < j; i++, j--)
  {
    gpointer t = out->pdata[i];
    out->pdata[i] = out->pdata[j];
    out->pdata[j] = t;
  }
  if(out_last_seq && out->len)
    *out_last_seq = ((const dt_sv_logged_event_t *)g_ptr_array_index(out, out->len - 1))->seq;
  return out;
}

guint dt_supervisor_events_count(void)
{
  if(!_sv.inited) return 0;
  dt_pthread_mutex_lock(&_sv.lock);
  const guint n = _sv.log ? g_queue_get_length(_sv.log) : 0;
  dt_pthread_mutex_unlock(&_sv.lock);
  return n;
}

void dt_supervisor_events_free(GPtrArray *events)
{
  if(events) g_ptr_array_free(events, TRUE);
}
