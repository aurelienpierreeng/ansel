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

/**
 * @file history_merge.c
 * @brief Merge histories, including optional pipeline topology merge.
 * @date 2026-02-22
 *
 * Merge histories, for copy-pasting and styles.
 *
 * Merging histories is a twofold problems, we need to solve :
 *
 * 1. the inner of the module (parameters, blendops)
 * 2. the topology of the pipeline (ordering).
 *
 * Let 2 histories A and B, considering A is our existing one, and
 * B is our candidate for pasting. They each have a `devA` and `devB` `dt_develop_t` object.
 * The members of interest are `dev->history`, `dev->iop`, `dev->iop_order`
 *
 * The first problem is simple : we have append mode (concatenate `devB->history` at the end of `devA->history`)
 * or prepend mode (concatenate `devB->history` at the start of `devA->history`), decided by user.
 * Histories are commited to pipeline nodes ("popped") in the order of items.
 * Since each history item may overwrite any previous item targetting the same module,
 * this order defines which history takes precedence over the other, in case of conflicts.
 * We do not handle per-module conflict resolution.
 *
 * The second problem is much harder, because both histories may have different topologies (number of nodes and
 * relative ordering). For this, we need to build temporary pipelines `devA->iop` and `devB->iop` using each
 * histories, and turn them into directed graphs. Each module will be a digraph node defined by its previous and
 * next module in its original pipeline, identified by its `module->op` and `module->multi_name`, taken as unique
 * ID. Modules having the same ID in both pipes will be assumed to be the same entity and merged. Then we will need
 * a topological sort to resolve (if possible) the complete pipeline order, taking into account the ordering
 * constraints imposed by `devA` and by `devB`, which may overconstrain each entity. Once we have the topological
 * order sorted, we will need to create a new pipeline and apply ("pop") the concatenated histories as solved in
 * the first problem.
 *
 * This supposes users provided an explicit pipeline order as a { module -> index } list.
 *
 * However, when users don't specify a pipeline order in the merge, we are to assume they don't care.
 * In this case, we enforce a behaviour that is consistent with GUI operation :
 *
 * - the base instances of modules are directly matched B to A (no re-ordering, no new instance creation)
 * - the additional instances of modules that exist in A and B, and have matching `module->op` and
 * `module->multi_name` between A and B, are also directly matched (no re-ordering, no new instance),
 * - the additional instances of modules from B that don't exist in A will be created after the last module
 * instance of the same type in A (consistent with GUI interaction on "add new instance" event),
 * - the modules from B will never overwrite the pipeline order of those from A.
 *
 * There is also a special case for a "force new modules" merge. In this mode, all modules from B will be added to
 * A as new instances. This will need to rename and reindex instances properly for the B modules, then update B
 * history accordingly. Pipeline topology will obey the above rules, depending whether an explicit pipeline order
 * was specified or not.
 *
 * If A does not exist yet, it has to be inited with all the typical defaults applied to new edits. This is done
 * automatically in `dt_dev_history_read_history()`, which either load history from database or init a fresh one,
 * along with a pipeline (`dev->iop`).
 *
 * Before popping the history into modules (as pipeline nodes), each history item will need to be resynced with
 * nodes, especially the pipe order and `dt_iop_module_t *` references. Here again, `module->op` and
 * `module->multi_name` act as unique IDs to match history entries and pipe nodes. History entries that don't find
 * an existing module will need a new module to be created.
 *
 *
 */

#include "common/history_merge.h"
#include "common/history_merge_gui.h"

#include "common/darktable.h"
#include "common/debug.h"
#include "common/iop_order.h"
#include "common/topological_sort.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/dev_history.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include <glib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

char *_hm_make_node_id(const char *op, const char *multi_name)
{
  /* Build the unique node identifier used everywhere in this file.
   *
   * Convention:
   *   node_id := "<module->op>|<module->multi_name>"
   *
   * Rationale:
   * - `op` identifies the module kind.
   * - `multi_name` is the stable user-visible identifier for multi-instances.
   * - Base instances usually have an empty multi_name, giving "<op>|".
   *
   * Assumptions:
   * - `multi_name` is stable across reloads (unlike `instance`, which is a runtime counter).
   * - Neither `op` nor `multi_name` contains the '|' separator.
   *
   * Ownership:
   * - Returns a newly allocated string that must be freed with g_free().
   */
  return g_strdup_printf("%s|%s", op, multi_name);
}

static void _hm_free_input_node(dt_digraph_node_t *n)
{
  if(IS_NULL_PTR(n)) return;
  g_list_free(n->previous);
  n->previous = NULL;
  if(n->tag)
  {
    dt_free(n->tag);
  }
  dt_free(n->id);
  dt_free(n);
}

static void _hm_free_input_nodes(GList *input_nodes)
{
  /* Free the temporary graph nodes created during constraint construction.
   *
   * Context:
   * - `_hm_build_input_nodes_from_ids()` and `_iop_rules()` build a list of heap-allocated
   *   `dt_digraph_node_t` nodes used as INPUT to `flatten_nodes()`.
   * - `flatten_nodes()` allocates its OWN canonical node objects and does not take ownership
   *   of the input nodes, so we must always free the input side ourselves.
   *
   * Important detail:
   * - `node->previous` is a GList that stores non-owning pointers to other input nodes.
   *   We free only the list container, not the pointed-to nodes here.
   */
  g_list_free_full(input_nodes, (GDestroyNotify)_hm_free_input_node);
  input_nodes = NULL;
}

void _hm_id_to_op_name(const char *id, char *op, char *name)
{
  /* Parse a node id ("op|multi_name") into two fixed-size buffers.
   *
   * Why this exists:
   * - Many APIs in develop/ and history/ use fixed-size arrays for op/multi_name.
   * - This helper ensures consistent splitting and clamping to those sizes.
   *
   * Assumptions:
   * - `id` uses the `_hm_make_node_id()` convention.
   * - If no separator is found (defensive), the whole string is treated as `op`.
   */
  op[0] = '\0';
  name[0] = '\0';
  const char *sep = strchr(id, '|');
  if(IS_NULL_PTR(sep))
  {
    g_strlcpy(op, id, sizeof(((dt_dev_history_item_t *)0)->op_name));
    return;
  }

  const size_t op_len = MIN((size_t)(sep - id), sizeof(((dt_dev_history_item_t *)0)->op_name) - 1);
  memcpy(op, id, op_len);
  op[op_len] = '\0';

  g_strlcpy(name, sep + 1, sizeof(((dt_dev_history_item_t *)0)->multi_name));
}

static int _hm_build_prev_map_from_ids(const GList *ids, GHashTable **out_prev)
{
  /* Build an adjacency map from a list of node ids that is already in pipeline order.
   *
   * Output:
   *   prev[id_i] = id_{i-1}
   *
   * This is not a general graph analysis: it is a representation of the *local* "previous"
   * relationship implied by the linear list.
   *
   * Ownership:
   * - Returns a hashtable owning both keys and values (g_hash_table_destroy()).
   */
  GHashTable *prev = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, dt_free_gpointer);
  if(IS_NULL_PTR(prev)) return 1;

  const char *prev_id = NULL;
  // Walk the list in order to record each element's immediate predecessor.
  for(const GList *l = ids; l; l = g_list_next(l))
  {
    const char *id = (const char *)l->data;

    if(prev_id) g_hash_table_replace(prev, g_strdup(id), g_strdup(prev_id));

    prev_id = id;
  }

  *out_prev = prev;
  return 0;
}

static int _hm_build_next_map_from_ids(const GList *ids, GHashTable **out_next)
{
  /* Symmetric to `_hm_build_prev_map_from_ids()`.
   *
   * Output:
   *   next[id_{i-1}] = id_i
   *
   * Ownership:
   * - Returns a hashtable owning both keys and values (g_hash_table_destroy()).
   */
  GHashTable *next = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, dt_free_gpointer);
  if(IS_NULL_PTR(next)) return 1;

  const char *prev_id = NULL;
  // Walk the list in order to record each element's immediate successor.
  for(const GList *l = ids; l; l = g_list_next(l))
  {
    const char *id = (const char *)l->data;

    if(prev_id) g_hash_table_replace(next, g_strdup(prev_id), g_strdup(id));

    prev_id = id;
  }

  *out_next = next;
  return 0;
}

static gboolean _hm_node_has_predecessor(const dt_digraph_node_t *n, const dt_digraph_node_t *pred)
{
  /* Check whether the flattened canonical node `pred` is already registered as a predecessor of `n`.
   *
   * We use this to detect a direct 2-cycle:
   *   - `a` has predecessor `b`
   *   - `b` has predecessor `a`
   */

  // Linear scan: predecessor lists are small (pipeline-sized), so this is fine.
  for(const GList *p = g_list_first(n->previous); p; p = g_list_next(p))
    if(p->data == pred) return TRUE;
  return FALSE;
}

static void _hm_remove_predecessor(dt_digraph_node_t *n, const dt_digraph_node_t *pred)
{
  /* Remove a single predecessor edge `pred -> n` from the flattened graph.
   *
   * This is a *local* conflict resolver used to break direct 2-cycles before running the
   * full topological sort.
   */

  GList *link = g_list_find(n->previous, pred);
  if(link) n->previous = g_list_delete_link(n->previous, link);
}

typedef enum
{
  // Id was present in the pasted module list (`mod_list`).
  HM_ID_FROM_MOD_LIST = 1 << 0,
  // Id was present in the source pipeline (`dev_src->iop`).
  HM_ID_FROM_SRC_IOP = 1 << 1,
  // Id was present in the destination pipeline (`dev_dest->iop`).
  HM_ID_FROM_DST_IOP = 1 << 2,
  // Id was introduced by a global fence rule (base instance only, multi_name="").
  HM_ID_FROM_RULE = 1 << 3
} _hm_id_origin_t;

typedef struct
{
  // Bitmask of `_hm_id_origin_t` describing where the id was seen.
  guint flags;
  // Non-owning pointer to the module instance from the pasted set (if any).
  const dt_iop_module_t *mod_list;
  // Non-owning pointer to the module instance in the source pipeline (if any).
  const dt_iop_module_t *src_iop;
  // Non-owning pointer to the module instance in the destination pipeline (if any).
  dt_iop_module_t *dst_iop;
} _hm_id_info_t;

typedef struct
{
  GList *history;
  int history_end;
  GList *iop_order_list;
  GPtrArray *orig_labels;
  GPtrArray *orig_styles;
  GHashTable *orig_ids;
} _hm_dest_backup_t;

static int _hm_build_last_history_by_id_from_history(GList *history, const int history_end, GHashTable **out_map)
{
  GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  if(IS_NULL_PTR(map)) return 1;

  int idx = 0;
  for(GList *l = g_list_first(history); l && idx < history_end; l = g_list_next(l), idx++)
  {
    dt_dev_history_item_t *hist = (dt_dev_history_item_t *)l->data;
    g_hash_table_replace(map, _hm_make_node_id(hist->op_name, hist->multi_name), hist);
  }

  *out_map = map;
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_build_last_history_by_id_from_history] history_end=%d scanned=%d entries=%d\n",
           history_end, idx, g_hash_table_size(map));
  return 0;
}

static int _hm_backup_dest(const dt_develop_t *dev_dest, const GHashTable *mod_list_ids, _hm_dest_backup_t *backup)
{
  *backup = (_hm_dest_backup_t){ 0 };
  backup->history = dt_history_duplicate(dev_dest->history);
  backup->history_end = dt_dev_get_history_end_ext((dt_develop_t *)dev_dest);
  backup->iop_order_list = dt_ioppr_iop_order_copy_deep(dev_dest->iop_order_list);
  if(IS_NULL_PTR(backup->iop_order_list)) return 1;

  GHashTable *last_by_id = NULL;
  if(_hm_build_last_history_by_id_from_history(backup->history, backup->history_end, &last_by_id)) return 1;
  backup->orig_labels = _hm_collect_labels_from_history_map(last_by_id, mod_list_ids, &backup->orig_styles);
  if(IS_NULL_PTR(backup->orig_labels) || IS_NULL_PTR(backup->orig_styles))
  {
    if(backup->orig_labels) g_ptr_array_free(backup->orig_labels, TRUE);
    if(backup->orig_styles) g_ptr_array_free(backup->orig_styles, TRUE);
    g_hash_table_destroy(last_by_id);
    return 1;
  }
  backup->orig_ids = last_by_id;
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_backup_dest] imgid=%d history_end=%d history_len=%d iop_order=%d labels=%u selected=%d\n",
           dev_dest->image_storage.id, backup->history_end, g_list_length(backup->history),
           g_list_length(backup->iop_order_list), backup->orig_labels->len,
           mod_list_ids ? g_hash_table_size((GHashTable *)mod_list_ids) : 0);
  return 0;
}

static void _hm_restore_dest_from_backup(dt_develop_t *dev_dest, _hm_dest_backup_t *backup)
{
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_restore_dest_from_backup] imgid=%d history_end=%d history_len=%d iop_order=%d\n",
           dev_dest->image_storage.id, backup->history_end, g_list_length(backup->history),
           g_list_length(backup->iop_order_list));

  dt_dev_history_free_history(dev_dest);
  dev_dest->history = backup->history;
  backup->history = NULL;
  dt_dev_set_history_end_ext(dev_dest, backup->history_end);

  g_list_free_full(dev_dest->iop_order_list, dt_free_gpointer);
  dev_dest->iop_order_list = backup->iop_order_list;
  backup->iop_order_list = NULL;

  dt_dev_pop_history_items_ext(dev_dest);
  dt_dev_write_history_ext(dev_dest, dev_dest->image_storage.id);
}

static void _hm_backup_cleanup(_hm_dest_backup_t *backup)
{
  if(backup->history)
  {
    g_list_free_full(backup->history, dt_dev_free_history_item);
    backup->history = NULL;
  }
  if(backup->iop_order_list)
  {
    g_list_free_full(backup->iop_order_list, dt_free_gpointer);
    backup->iop_order_list = NULL;
  }
  if(backup->orig_labels) g_ptr_array_free(backup->orig_labels, TRUE);
  if(backup->orig_styles) g_ptr_array_free(backup->orig_styles, TRUE);
  if(backup->orig_ids) g_hash_table_destroy(backup->orig_ids);
  backup->history = NULL;
  backup->iop_order_list = NULL;
  backup->orig_labels = NULL;
  backup->orig_styles = NULL;
  backup->orig_ids = NULL;
}

int _hm_build_last_history_by_id(const dt_develop_t *dev, GHashTable **out_map)
{
  /* Build a map of last history item per module instance in the given develop stack.
   *
   * Key:   "<op>|<multi_name>"
   * Value: dt_dev_history_item_t* (non-owning)
   *
   * This is used to decide whether a post-merge history item matches the source or destination history.
   */
  GHashTable *map = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  if(IS_NULL_PTR(map)) return 1;

  const int history_end = dt_dev_get_history_end_ext((dt_develop_t *)dev);
  for(GList *modules = g_list_first(dev->iop); modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    dt_dev_history_item_t *hist = dt_dev_history_get_last_item_by_module(dev->history, mod, history_end);
    g_hash_table_replace(map, _hm_make_node_id(mod->op, mod->multi_name), hist);
  }

  *out_map = map;
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_build_last_history_by_id] imgid=%d history_end=%d iop=%d entries=%d\n",
           dev->image_storage.id, history_end, g_list_length(dev->iop), g_hash_table_size(map));
  return 0;
}

static int _hm_build_id_set_from_mod_list(const GList *mod_list, GHashTable **out_ids)
{
  /* Build a set of node ids from the pasted module list. */
  GHashTable *ids = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  if(IS_NULL_PTR(ids)) return 1;
  for(const GList *l = g_list_first((GList *)mod_list); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    g_hash_table_add(ids, _hm_make_node_id(mod->op, mod->multi_name));
  }
  *out_ids = ids;
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_build_id_set_from_mod_list] modules=%d ids=%d\n",
           g_list_length((GList *)mod_list), g_hash_table_size(ids));
  return 0;
}

static _hm_id_info_t *_hm_id_info_upsert(GHashTable *id_ht, const char *op, const char *multi_name,
                                         const _hm_id_origin_t origin, const dt_iop_module_t *mod_list,
                                         const dt_iop_module_t *src_iop, dt_iop_module_t *dst_iop)
{
  /* Insert or update an entry in the id->info table.
   *
   * This table is the "join" structure for the whole merge:
   * - keys are node ids ("op|multi_name"),
   * - values store:
   *   - where the id was seen (bitmask),
   *   - pointers to the corresponding module instance when available.
   *
   * We populate it in a deliberate order (mod_list -> src_iop -> dst_iop) so later stages can:
   * - decide whether a node should be copied (present in mod_list),
   * - reuse an existing destination instance when possible,
   * - create missing destination instances for nodes that appear in the solved ordering.
   *
   * Ownership:
   * - The hashtable owns the key string (allocated here via `_hm_make_node_id()`).
   * - The `_hm_id_info_t` is heap-allocated and owned by the hashtable, but the stored module pointers
   *   are non-owning references (modules are owned by the develop contexts).
   */

  char *id = _hm_make_node_id(op, multi_name);
  _hm_id_info_t *info = (_hm_id_info_t *)g_hash_table_lookup(id_ht, id);
  if(IS_NULL_PTR(info))
  {
    if(origin & HM_ID_FROM_MOD_LIST)
      dt_print(DT_DEBUG_HISTORY,
            "[_hm_id_info_upsert] %s input node \n",
            id);
    info = g_new0(_hm_id_info_t, 1);
    g_hash_table_insert(id_ht, id, info);
  }
  else
  {
    dt_free(id);
  }

  info->flags |= origin;
  if(mod_list) info->mod_list = mod_list;
  if(src_iop && IS_NULL_PTR(info->src_iop)) info->src_iop = src_iop;
  if(dst_iop && IS_NULL_PTR(info->dst_iop)) info->dst_iop = dst_iop;

  return info;
}

/* Build a list of node ids in pipeline order, restricted by an origin bitmask.
 *
 * This transforms a `dev->iop` list into a list of freshly allocated ids.
 * We filter using `id_ht` so later steps can operate on a consistent kept set.
 *
 * Ownership:
 * - Returned ids are owned by the caller (free with g_list_free_full(list, dt_free_gpointer)).
 */
static int _hm_ids_from_iop_list(GList *iop, GHashTable *id_ht, const guint keep_mask, GList **out_ids)
{
  GList *ids = NULL;
  // Walk the iop list in order so the returned ids encode adjacency constraints.
  for(const GList *l = iop; l; l = g_list_next(l))
  {
    const dt_iop_module_t *const mod = (const dt_iop_module_t *)l->data;

    char *id = _hm_make_node_id(mod->op, mod->multi_name);
    const _hm_id_info_t *info = (_hm_id_info_t *)g_hash_table_lookup(id_ht, id);
    if(info && (info->flags & keep_mask))
      ids = g_list_append(ids, id);
    else
      dt_free(id);
  }
  *out_ids = ids;
  return 0;
}

static int _hm_build_input_nodes_from_ids(const GList *ids, const char *tag, GList **out_nodes)
{
  /* Build a list of digraph nodes encoding linear "previous" constraints.
   *
   * For ids [a,b,c], we build nodes with:
   *   b.previous = [a]
   *   c.previous = [b]
   *
   * This is enough for topological sorting: it constrains only immediate adjacency, not all pairs.
   *
   * Tag:
   * - Used as provenance metadata ("src"/"dst"/"rule") that is propagated during flattening for debug.
   */
  GList *nodes = NULL;
  dt_digraph_node_t *prev = NULL;

  // Iterate in order; link each node to the previously created node.
  for(const GList *l = ids; l; l = g_list_next(l))
  {
    const char *id = (const char *)l->data;

    dt_digraph_node_t *n = dt_digraph_node_new(id);
    if(IS_NULL_PTR(n))
    {
      _hm_free_input_nodes(nodes);
      return 1;
    }
    if(tag)
    {
      n->tag = g_strdup(tag);
      if(IS_NULL_PTR(n->tag))
      {
        _hm_free_input_node(n);
        _hm_free_input_nodes(nodes);
        return 1;
      }
    }

    if(prev) n->previous = g_list_append(n->previous, prev);

    nodes = g_list_append(nodes, n);
    prev = n;
  }
  *out_nodes = nodes;
  return 0;
}

static int _hm_build_input_nodes_from_ids_filtered(const GList *ids, const char *tag, const GHashTable *focus,
                                                   GList **out_nodes)
{
  /* Build input constraint nodes from an ordered id list, optionally filtering which adjacency edges are kept.
   *
   * When `focus` is NULL, this is equivalent to `_hm_build_input_nodes_from_ids()` (all consecutive edges).
   *
   * When `focus` is non-NULL, we keep an edge prev->cur only if:
   * - prev is in focus, OR
   * - cur is in focus.
   *
   * This allows importing ordering constraints from a full source pipeline while restricting them to
   * the neighborhood of the modules we actually want to position.
   */
  GList *nodes = NULL;
  dt_digraph_node_t *prev = NULL;

  for(const GList *l = ids; l; l = g_list_next(l))
  {
    const char *id = (const char *)l->data;

    dt_digraph_node_t *n = dt_digraph_node_new(id);
    if(IS_NULL_PTR(n))
    {
      _hm_free_input_nodes(nodes);
      return 1;
    }
    if(tag)
    {
      n->tag = g_strdup(tag);
      if(IS_NULL_PTR(n->tag))
      {
        _hm_free_input_node(n);
        _hm_free_input_nodes(nodes);
        return 1;
      }
    }

    if(prev)
    {
      const gboolean keep_edge = !focus || g_hash_table_contains((GHashTable *)focus, prev->id)
                                 || g_hash_table_contains((GHashTable *)focus, n->id);
      if(keep_edge) n->previous = g_list_append(n->previous, prev);
    }

    nodes = g_list_append(nodes, n);
    prev = n;
  }
  *out_nodes = nodes;
  return 0;
}

static int _hm_build_isolated_nodes_from_modules(const GList *modules, const char *tag, GList **out_nodes)
{
  /* Build nodes without edges, so modules are present in the graph even if no adjacency constraints apply. */
  GList *nodes = NULL;
  for(const GList *l = g_list_first((GList *)modules); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;

    char *id = _hm_make_node_id(mod->op, mod->multi_name);
    dt_digraph_node_t *n = dt_digraph_node_new(id);
    if(IS_NULL_PTR(n))
    {
      dt_free(id);
      _hm_free_input_nodes(nodes);
      return 1;
    }
    if(tag)
    {
      n->tag = g_strdup(tag);
      if(IS_NULL_PTR(n->tag))
      {
        dt_free(id);
        _hm_free_input_node(n);
        _hm_free_input_nodes(nodes);
        return 1;
      }
    }
    nodes = g_list_append(nodes, n);
    dt_free(id);
  }
  *out_nodes = nodes;
  return 0;
}

/* Build constraint nodes enforcing raster-mask producer -> user ordering. */
static int _hm_build_raster_mask_nodes_from_modules(const GList *modules, GHashTable *id_ht, const guint keep_mask,
                                                    const char *tag, GList **out_nodes)
{
  GList *nodes = NULL;
  // For each module using a raster mask, add a producer->user edge if both are kept in the graph.
  for(const GList *l = g_list_first((GList *)modules); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    const dt_iop_module_t *producer = mod->raster_mask.sink.source;
    if(IS_NULL_PTR(producer)) continue;

    char *user_id = _hm_make_node_id(mod->op, mod->multi_name);
    char *prod_id = _hm_make_node_id(producer->op, producer->multi_name);

    const _hm_id_info_t *user_info = (_hm_id_info_t *)g_hash_table_lookup(id_ht, user_id);
    const _hm_id_info_t *prod_info = (_hm_id_info_t *)g_hash_table_lookup(id_ht, prod_id);
    if(!(user_info && (user_info->flags & keep_mask) && prod_info && (prod_info->flags & keep_mask)))
    {
      dt_free(user_id);
      dt_free(prod_id);
      continue;
    }

    dt_digraph_node_t *prod = dt_digraph_node_new(prod_id);
    dt_digraph_node_t *user = dt_digraph_node_new(user_id);
    if(IS_NULL_PTR(prod) || IS_NULL_PTR(user))
    {
      _hm_free_input_node(prod);
      _hm_free_input_node(user);
      _hm_free_input_nodes(nodes);
      return 1;
    }
    if(tag)
    {
      prod->tag = g_strdup(tag);
      user->tag = g_strdup(tag);
      if(IS_NULL_PTR(prod->tag) || IS_NULL_PTR(user->tag))
      {
        _hm_free_input_node(prod);
        _hm_free_input_node(user);
        _hm_free_input_nodes(nodes);
        return 1;
      }
    }
    user->previous = g_list_append(user->previous, prod);

    nodes = g_list_append(nodes, prod);
    nodes = g_list_append(nodes, user);

    dt_free(user_id);
    dt_free(prod_id);
  }
  *out_nodes = nodes;
  return 0;
}

// Extract rules from IOP global fences
static int _iop_rules(GHashTable *keep, GList **out_nodes)
{
  /* Convert global iop-order fence rules into digraph constraints.
   *
   * Each rule (op_prev -> op_next) becomes an edge in the constraint graph.
   * We always use node ids "op|" (empty multi_name) to target base instances.
   *
   * Note: `keep` is currently unused (hook for potential future filtering).
   */
  GList *iop_rules = NULL;
  // Walk the global rule list; each entry yields two nodes (prev,next) and one predecessor edge.
  for(const GList *rules = g_list_first(darktable.iop_order_rules); rules; rules = g_list_next(rules))
  {
    const dt_iop_order_rule_t *const restrict rule = (dt_iop_order_rule_t *)rules->data;

    // Always use "op|" as the node id for rules, to match dev->iop instance names
    char next_id[256], prev_id[256];
    snprintf(next_id, sizeof(next_id), "%s|", rule->op_next);
    snprintf(prev_id, sizeof(prev_id), "%s|", rule->op_prev);

    dt_digraph_node_t *next = dt_digraph_node_new(next_id);
    dt_digraph_node_t *prev = dt_digraph_node_new(prev_id);
    if(IS_NULL_PTR(next) || IS_NULL_PTR(prev))
    {
      _hm_free_input_node(next);
      _hm_free_input_node(prev);
      _hm_free_input_nodes(iop_rules);
      return 1;
    }
    next->tag = g_strdup("rule");
    prev->tag = g_strdup("rule");
    if(IS_NULL_PTR(next->tag) || IS_NULL_PTR(prev->tag))
    {
      _hm_free_input_node(next);
      _hm_free_input_node(prev);
      _hm_free_input_nodes(iop_rules);
      return 1;
    }
    next->previous = g_list_append(next->previous, prev);
    iop_rules = g_list_append(iop_rules, next);
    iop_rules = g_list_append(iop_rules, prev);
  }
  *out_nodes = iop_rules;
  return 0;
}

typedef struct _hm_topo_merge_ctx_t
{
  /* Transient state for a single topological merge attempt.
   *
   * Keeping this in a struct makes the main function `_hm_try_merge_iop_order_topologically()`
   * easier to read and reduces the risk of leaking allocations on error paths.
   */
  // id string ("op|multi_name") -> `_hm_id_info_t` (ownership: ctx owns keys + values).
  GHashTable *id_ht;
  // Bitmask of `_hm_id_origin_t` selecting the kept node set for this merge attempt.
  guint keep_mask;
  // Ordered list of kept ids representing destination adjacency constraints (allocated ids).
  GList *dest_ids;
  // Ordered list of kept ids representing source pipeline adjacency constraints (allocated ids).
  GList *src_ids;
  // Raw constraint nodes built from dest/src/rules before flattening (ownership: ctx).
  GList *input_nodes;
  // Canonical flattened nodes (ownership: ctx via dt_digraph_cleanup_full()).
  GList *flat;
  // Topologically sorted solution order (list container only; nodes are owned by `flat`).
  GList *sorted;
  // When FALSE, topo merge only updates ordering/instances; it does not copy module content.
  gboolean copy_module_contents;
  // When FALSE, the source iop list cannot impose successors around newly-created instances.
  gboolean source_iop_order;
  // Set of ids (strings) selecting modules that source-order or destination-slot constraints should position.
  // In source-order mode, edges touching this set are imported; otherwise this set holds missing instances.
  GHashTable *src_focus_ids;
  // Modules selected for pasting (source instances).
  const GList *mod_list;
  // Destination develop context for raster-mask constraints.
  dt_develop_t *dev_dest;
} _hm_topo_merge_ctx_t;

static void _hm_topo_merge_cleanup(_hm_topo_merge_ctx_t *ctx)
{
  /* Free everything allocated in the topo-merge context.
   *
   * This must be safe to call multiple times and after partial initialization, hence the NULL checks.
   */

  if(ctx->sorted)
  {
    g_list_free(ctx->sorted);
    ctx->sorted = NULL;
  }
  if(ctx->flat) dt_digraph_cleanup_full(ctx->flat, NULL, NULL);
  if(ctx->input_nodes) _hm_free_input_nodes(ctx->input_nodes);
  if(ctx->dest_ids)
  {
    g_list_free_full(ctx->dest_ids, dt_free_gpointer);
    ctx->dest_ids = NULL;
  }
  if(ctx->src_ids)
  {
    g_list_free_full(ctx->src_ids, dt_free_gpointer);
    ctx->src_ids = NULL;
  }
  if(ctx->src_focus_ids) g_hash_table_destroy(ctx->src_focus_ids);
  if(ctx->id_ht) g_hash_table_destroy(ctx->id_ht);

  ctx->sorted = NULL;
  ctx->flat = NULL;
  ctx->input_nodes = NULL;
  ctx->dest_ids = NULL;
  ctx->src_ids = NULL;
  ctx->src_focus_ids = NULL;
  ctx->id_ht = NULL;
}

static int _hm_topo_build_id_info_table(_hm_topo_merge_ctx_t *ctx, dt_develop_t *dev_dest, dt_develop_t *dev_src,
                                        const GList *mod_list)
{
  /* Build the global id->info table used throughout the topo merge.
   *
   * We record membership ("seen in dst", "seen in mod_list", ...) and pointers to the relevant module
   * instances so that later steps can:
   * - create missing destination instances,
   * - copy module contents for the pasted set.
   */

  // Build a single ID->info table, filled in the requested order:
  // 1) mod_list, 2) dev_src->iop, 3) dev_dest->iop.
  ctx->id_ht = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, dt_free_gpointer);
  if(IS_NULL_PTR(ctx->id_ht)) return 1;

  // Register ids for modules we intend to paste.
  for(const GList *l = g_list_first((GList *)mod_list); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    _hm_id_info_upsert(ctx->id_ht, mod->op, mod->multi_name, HM_ID_FROM_MOD_LIST, mod, NULL, NULL);
  }

  // Register ids for the source pipeline (useful for debugging/incompatibility resolution).
  for(const GList *l = g_list_first(dev_src->iop); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    _hm_id_info_upsert(ctx->id_ht, mod->op, mod->multi_name, HM_ID_FROM_SRC_IOP, NULL, mod, NULL);
  }

  // Register ids for the destination pipeline so we can reuse existing instances when applying the solution.
  for(const GList *l = g_list_first(dev_dest->iop); l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    _hm_id_info_upsert(ctx->id_ht, mod->op, mod->multi_name, HM_ID_FROM_DST_IOP, NULL, NULL, mod);
  }

  // Also register fence rules (base instances only, multi_name="") so they can participate in constraints.
  // These don't have module pointers; they participate only as ordering constraints.
  for(const GList *rules = g_list_first(darktable.iop_order_rules); rules; rules = g_list_next(rules))
  {
    const dt_iop_order_rule_t *rule = (dt_iop_order_rule_t *)rules->data;
    _hm_id_info_upsert(ctx->id_ht, rule->op_next, "", HM_ID_FROM_RULE, NULL, NULL, NULL);
    _hm_id_info_upsert(ctx->id_ht, rule->op_prev, "", HM_ID_FROM_RULE, NULL, NULL, NULL);
  }

  return 0;
}

static int _hm_topo_build_constraint_ids(_hm_topo_merge_ctx_t *ctx, dt_develop_t *dev_dest, dt_develop_t *dev_src,
                                         const GList *mod_list, const gboolean merge_iop_order)
{
  /* Build the ordered id lists that represent adjacency constraints for the merge.
   *
   * We merge constraints from:
   * - destination pipeline order (to keep the current image stable),
   * - the source pipeline order (to constrain where pasted modules are placed),
   * - global fence rules (added later when building input nodes).
   *
   * We filter both lists to the kept set (dst ∪ mod_list ∪ rules) to avoid importing unrelated
   * source-only modules into the ordering problem.
   */

  // Build a focus set selecting which pasted modules need source-order or insertion-slot constraints.
  // - merge_iop_order=TRUE: import source ordering constraints around all pasted modules.
  // - merge_iop_order=FALSE: slot only modules missing in destination into the existing destination pipeline.
  ctx->src_focus_ids = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  if(IS_NULL_PTR(ctx->src_focus_ids)) return 1;
  for(const GList *l = g_list_first((GList *)mod_list); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;

    char *id = _hm_make_node_id(mod->op, mod->multi_name);
    const _hm_id_info_t *info = (_hm_id_info_t *)g_hash_table_lookup(ctx->id_ht, id);
    const gboolean exists_in_dest = (info->flags & HM_ID_FROM_DST_IOP);

    if(merge_iop_order || !exists_in_dest) g_hash_table_add(ctx->src_focus_ids, g_strdup(id));

    dt_free(id);
  }

  // Restrict sorting to everything in destination plus what we need to paste, plus rule nodes.
  ctx->keep_mask = HM_ID_FROM_DST_IOP | HM_ID_FROM_MOD_LIST | HM_ID_FROM_RULE;

  // Destination constraints: current destination pipeline order, filtered to the kept ids.
  if(_hm_ids_from_iop_list(g_list_first(dev_dest->iop), ctx->id_ht, ctx->keep_mask, &ctx->dest_ids)) return 1;
  // Source constraints: the source pipeline order, filtered to the kept ids.
  // We will later import only the edges that touch the focus set (`ctx->src_focus_ids`).
  if(_hm_ids_from_iop_list(g_list_first(dev_src->iop), ctx->id_ht, ctx->keep_mask, &ctx->src_ids)) return 1;

  dt_print(DT_DEBUG_HISTORY,
           "[dt_history_merge_module_list_into_image_topological] iop-order solve: merge_iop_order=%d mod_list=%d "
           "src_iop=%d "
           "dst_iop=%d keep(dst+mod+rules) dest_constraints=%d src_constraints=%d focus=%d\n",
           merge_iop_order, g_list_length((GList *)mod_list), g_list_length(dev_src->iop),
           g_list_length(dev_dest->iop), g_list_length(ctx->dest_ids), g_list_length(ctx->src_ids),
           g_hash_table_size(ctx->src_focus_ids));

  return 0;
}

// Resolve direct 2-cycles after flattening (declared here because `_hm_topo_flatten_constraints()` calls it).
static int _hm_topo_resolve_incompatible_constraints(GList *flat, GHashTable *id_ht, const GList *src_ids,
                                                     const GList *dest_ids);

static int _hm_topo_flatten_constraints(_hm_topo_merge_ctx_t *ctx)
{
  /* Build and flatten constraint nodes into canonical digraph nodes.
   *
   * - `_hm_build_input_nodes_from_ids()` turns a linear id list into predecessor edges.
   * - `_iop_rules()` adds global fence constraints (base-instance only).
   * - `flatten_nodes()` merges nodes with identical ids and deduplicates predecessor edges.
   *
   * The result `ctx->flat` is the canonical constraint graph given to `topological_sort()`.
   */



  GList *dest_nodes = NULL;
  GList *src_nodes = NULL;
  GList *mod_nodes = NULL;
  GList *dst_raster_nodes = NULL;
  GList *src_raster_nodes = NULL;
  GList *rule_nodes = NULL;
  GHashTable *dest_rank = NULL;

  if(_hm_build_input_nodes_from_ids(ctx->dest_ids, "dst", &dest_nodes)) goto error;
  if(ctx->source_iop_order)
  {
    // Source-order merge imports the local source neighborhood around pasted modules.
    if(_hm_build_input_nodes_from_ids_filtered(ctx->src_ids, "src", ctx->src_focus_ids, &src_nodes)) goto error;
  }
  else
  {
    /* Destination-order merge must not let a style's full iop_list move existing destination modules.
     *
     * For each missing source instance, we use the style order only to find a destination slot:
     * - the latest destination module that appears before it in the style,
     * - the earliest destination module that appears after it in the style and still follows that lower bound.
     *
     * This keeps the destination pipe stable while allowing style instances to land in their intended
     * pipeline region. It also drops stale successor constraints that would move existing modules or create
     * cycles, such as "Exposure (AAA) before Mask manager" on current RAW orders.
     */
    dest_rank = g_hash_table_new(g_str_hash, g_str_equal);
    if(IS_NULL_PTR(dest_rank)) goto error;

    int rank = 1;
    for(const GList *d = g_list_first(ctx->dest_ids); d; d = g_list_next(d), rank++)
      g_hash_table_insert(dest_rank, d->data, GINT_TO_POINTER(rank));

    for(const GList *l = g_list_first(ctx->src_ids); l; l = g_list_next(l))
    {
      const char *id = (const char *)l->data;
      if(!g_hash_table_contains(ctx->src_focus_ids, id)) continue;

      int lower_rank = 0;
      int upper_rank = 0;
      const char *lower_id = NULL;
      const char *upper_id = NULL;
      const char *prev_focus_id = NULL;
      const char *next_focus_id = NULL;

      const GList *prev_link = g_list_previous((GList *)l);
      if(prev_link && g_hash_table_contains(ctx->src_focus_ids, prev_link->data))
        prev_focus_id = (const char *)prev_link->data;
      const GList *next_link = g_list_next(l);
      if(next_link && g_hash_table_contains(ctx->src_focus_ids, next_link->data))
        next_focus_id = (const char *)next_link->data;

      // Scan source predecessors and keep the latest one in destination order as lower slot boundary.
      for(const GList *p = g_list_first(ctx->src_ids); p && p != l; p = g_list_next(p))
      {
        const char *candidate_id = (const char *)p->data;
        const int candidate_rank = GPOINTER_TO_INT(g_hash_table_lookup(dest_rank, candidate_id));
        if(candidate_rank > lower_rank)
        {
          lower_rank = candidate_rank;
          lower_id = candidate_id;
        }
      }

      // Scan source successors and keep the earliest destination node that still follows the lower boundary.
      for(const GList *n = g_list_next(l); n; n = g_list_next(n))
      {
        const char *candidate_id = (const char *)n->data;
        const int candidate_rank = GPOINTER_TO_INT(g_hash_table_lookup(dest_rank, candidate_id));
        if(candidate_rank <= lower_rank) continue;
        if(upper_rank == 0 || candidate_rank < upper_rank)
        {
          upper_rank = candidate_rank;
          upper_id = candidate_id;
        }
      }

      dt_digraph_node_t *cur = dt_digraph_node_new(id);
      if(IS_NULL_PTR(cur)) goto error;
      src_nodes = g_list_append(src_nodes, cur);

      if(lower_id)
      {
        dt_digraph_node_t *lower = dt_digraph_node_new(lower_id);
        if(IS_NULL_PTR(lower)) goto error;
        cur->previous = g_list_append(cur->previous, lower);
        src_nodes = g_list_append(src_nodes, lower);
      }
      if(prev_focus_id)
      {
        dt_digraph_node_t *prev_focus = dt_digraph_node_new(prev_focus_id);
        if(IS_NULL_PTR(prev_focus)) goto error;
        cur->previous = g_list_append(cur->previous, prev_focus);
        src_nodes = g_list_append(src_nodes, prev_focus);
      }

      if(next_focus_id)
      {
        dt_digraph_node_t *next_focus = dt_digraph_node_new(next_focus_id);
        dt_digraph_node_t *next_focus_prev = dt_digraph_node_new(id);
        if(IS_NULL_PTR(next_focus) || IS_NULL_PTR(next_focus_prev))
        {
          _hm_free_input_node(next_focus);
          _hm_free_input_node(next_focus_prev);
          goto error;
        }
        next_focus->previous = g_list_append(next_focus->previous, next_focus_prev);
        src_nodes = g_list_append(src_nodes, next_focus_prev);
        src_nodes = g_list_append(src_nodes, next_focus);
      }
      if(upper_id)
      {
        dt_digraph_node_t *upper = dt_digraph_node_new(upper_id);
        dt_digraph_node_t *upper_prev = dt_digraph_node_new(id);
        if(IS_NULL_PTR(upper) || IS_NULL_PTR(upper_prev))
        {
          _hm_free_input_node(upper);
          _hm_free_input_node(upper_prev);
          goto error;
        }
        upper->previous = g_list_append(upper->previous, upper_prev);
        src_nodes = g_list_append(src_nodes, upper_prev);
        src_nodes = g_list_append(src_nodes, upper);
      }

      dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
               "[_hm_topo_flatten_constraints] destination slot: %s after %s%s%s before %s%s%s\n",
               id, lower_id ? lower_id : "(none)", (lower_id && prev_focus_id) ? ", " : "",
               prev_focus_id ? prev_focus_id : "", upper_id ? upper_id : "(end)",
               (upper_id && next_focus_id) ? ", " : "", next_focus_id ? next_focus_id : "");
    }
    g_hash_table_destroy(dest_rank);
    dest_rank = NULL;
  }
  // Ensure all pasted modules are present in the graph, even if they don't appear in src/dst adjacency lists.
  if(_hm_build_isolated_nodes_from_modules(ctx->mod_list, "mod", &mod_nodes)) goto error;
  // Raster mask constraints: producer must come before user.
  if(_hm_build_raster_mask_nodes_from_modules(ctx->dev_dest->iop, ctx->id_ht, ctx->keep_mask, "dst-raster",
                                              &dst_raster_nodes))
    goto error;
  if(_hm_build_raster_mask_nodes_from_modules(ctx->mod_list, ctx->id_ht, ctx->keep_mask, "src-raster",
                                              &src_raster_nodes))
    goto error;
  if(_iop_rules(NULL, &rule_nodes)) goto error;

  const int dest_nodes_len = g_list_length(dest_nodes);
  const int src_nodes_len = g_list_length(src_nodes);
  const int mod_nodes_len = g_list_length(mod_nodes);
  const int dst_raster_nodes_len = g_list_length(dst_raster_nodes);
  const int src_raster_nodes_len = g_list_length(src_raster_nodes);
  const int rule_nodes_len = g_list_length(rule_nodes);

  ctx->input_nodes = g_list_concat(
      g_list_concat(g_list_concat(dest_nodes, src_nodes),
                    g_list_concat(mod_nodes, g_list_concat(dst_raster_nodes, src_raster_nodes))),
      rule_nodes);
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_topo_flatten_constraints] input nodes: dst=%d src=%d mod=%d dst-raster=%d src-raster=%d "
           "rules=%d total=%d\n",
           dest_nodes_len, src_nodes_len, mod_nodes_len, dst_raster_nodes_len, src_raster_nodes_len,
           rule_nodes_len, g_list_length(ctx->input_nodes));

  if(flatten_nodes(ctx->input_nodes, &ctx->flat))
  {
    dt_print(DT_DEBUG_HISTORY,
             "[dt_history_merge_module_list_into_image_topological] iop-order merge: flatten failed\n");
    return 1;
  }

  if(_hm_topo_resolve_incompatible_constraints(ctx->flat, ctx->id_ht, ctx->src_ids, ctx->dest_ids)) return 1;

  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_topo_flatten_constraints] flat nodes=%d\n", g_list_length(ctx->flat));
  return 0;

error:
  if(dest_rank) g_hash_table_destroy(dest_rank);
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_topo_flatten_constraints] failed while building input nodes: dst=%d src=%d mod=%d "
           "dst-raster=%d src-raster=%d rules=%d\n",
           g_list_length(dest_nodes), g_list_length(src_nodes), g_list_length(mod_nodes),
           g_list_length(dst_raster_nodes), g_list_length(src_raster_nodes), g_list_length(rule_nodes));
  _hm_free_input_nodes(dest_nodes);
  _hm_free_input_nodes(src_nodes);
  _hm_free_input_nodes(mod_nodes);
  _hm_free_input_nodes(dst_raster_nodes);
  _hm_free_input_nodes(src_raster_nodes);
  _hm_free_input_nodes(rule_nodes);
  return 1;
}

static int _hm_topo_resolve_incompatible_constraints(GList *flat, GHashTable *id_ht, const GList *src_ids,
                                                     const GList *dest_ids)
{
  /* Break direct 2-cycles (A<->B) by removing one of the two conflicting edges.
   *
   * The topo-sort implementation cannot succeed on cyclic graphs. Some cycles are unavoidable,
   * but direct 2-cycles are frequently caused by the destination and source imposing contradictory
   * immediate-predecessor constraints. We handle those specifically because:
   * - they are easy to detect,
   * - they are easy to resolve by choosing one of the two orderings.
   *
   * When a GUI is available, we ask the user whether to preserve source or destination ordering.
   * Otherwise we default to preserving destination ordering.
   */

  GList *_hm_cycles = NULL;
  GHashTable *seen_cycles = NULL;
  // Build adjacency lookup tables from the original linear constraints.
  GHashTable *src_prev = NULL;
  GHashTable *src_next = NULL;
  GHashTable *dst_prev = NULL;
  GHashTable *dst_next = NULL;
  const char *cleanup_reason = NULL;
  int cleanup_line = 0;
  if(_hm_build_prev_map_from_ids(src_ids, &src_prev))
  {
    cleanup_reason = "_hm_build_prev_map_from_ids(src_ids)";
    cleanup_line = __LINE__;
    goto cleanup;
  }
  if(_hm_build_next_map_from_ids(src_ids, &src_next))
  {
    cleanup_reason = "_hm_build_next_map_from_ids(src_ids)";
    cleanup_line = __LINE__;
    goto cleanup;
  }
  if(_hm_build_prev_map_from_ids(dest_ids, &dst_prev))
  {
    cleanup_reason = "_hm_build_prev_map_from_ids(dest_ids)";
    cleanup_line = __LINE__;
    goto cleanup;
  }
  if(_hm_build_next_map_from_ids(dest_ids, &dst_next))
  {
    cleanup_reason = "_hm_build_next_map_from_ids(dest_ids)";
    cleanup_line = __LINE__;
    goto cleanup;
  }

  typedef struct
  {
    // One node participating in the 2-cycle (edge b->a and a->b).
    dt_digraph_node_t *a;
    // The other node participating in the 2-cycle.
    dt_digraph_node_t *b;
    // Node we report to the user as "faulty" (prefer one belonging to the pasted set).
    dt_digraph_node_t *faulty;
  } _hm_cycle_t;

  seen_cycles = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  if(IS_NULL_PTR(seen_cycles))
  {
    cleanup_reason = "g_hash_table_new_full(seen_cycles)";
    cleanup_line = __LINE__;
    goto cleanup;
  }

  // Scan all edges a<-b and record those where b also has a as predecessor (2-cycle).
  for(GList *it = g_list_first(flat); it; it = g_list_next(it))
  {
    dt_digraph_node_t *a = (dt_digraph_node_t *)it->data;

    // Each `a->previous` entry means an edge (pred -> a).
    for(GList *p = g_list_first(a->previous); p; p = g_list_next(p))
    {
      dt_digraph_node_t *b = (dt_digraph_node_t *)p->data;

      if(!_hm_node_has_predecessor(b, a)) continue;

      // Deduplicate: we'll discover (a,b) and (b,a), so normalize the key ordering.
      const char *id1 = a->id;
      const char *id2 = b->id;
      if(strcmp(id1, id2) > 0)
      {
        id1 = b->id;
        id2 = a->id;
      }

      gchar *key = g_strdup_printf("%s<->%s", id1, id2);
      if(IS_NULL_PTR(key))
      {
        cleanup_reason = "g_strdup_printf(cycle key)";
        cleanup_line = __LINE__;
        goto cleanup;
      }
      if(g_hash_table_contains(seen_cycles, key))
      {
        dt_free(key);
        continue;
      }
      g_hash_table_add(seen_cycles, key);

      _hm_cycle_t *c = g_new0(_hm_cycle_t, 1);
      if(IS_NULL_PTR(c))
      {
        cleanup_reason = "g_new0(_hm_cycle_t)";
        cleanup_line = __LINE__;
        goto cleanup;
      }
      c->a = a;
      c->b = b;

      // Prefer blaming a pasted module when possible, because that's usually what users care about.
      const _hm_id_info_t *ai = (_hm_id_info_t *)g_hash_table_lookup(id_ht, a->id);
      const _hm_id_info_t *bi = (_hm_id_info_t *)g_hash_table_lookup(id_ht, b->id);
      c->faulty = (bi->flags & HM_ID_FROM_MOD_LIST) ? b : a;
      if(ai->flags & HM_ID_FROM_MOD_LIST) c->faulty = a;

      _hm_cycles = g_list_append(_hm_cycles, c);
    }
  }

  if(_hm_cycles)
  {
    // Ask once (for the first faulty module) and apply the chosen policy to all found 2-cycles.
    const _hm_cycle_t *first = (const _hm_cycle_t *)_hm_cycles->data;
    const dt_digraph_node_t *faulty = first ? first->faulty : NULL;

    const char *sp = (faulty && src_prev) ? (const char *)g_hash_table_lookup(src_prev, faulty->id) : NULL;
    const char *sn = (faulty && src_next) ? (const char *)g_hash_table_lookup(src_next, faulty->id) : NULL;
    const char *dp = (faulty && dst_prev) ? (const char *)g_hash_table_lookup(dst_prev, faulty->id) : NULL;
    const char *dn = (faulty && dst_next) ? (const char *)g_hash_table_lookup(dst_next, faulty->id) : NULL;

    dt_print(
        DT_DEBUG_HISTORY,
        "[dt_history_merge_module_list_into_image_topological] incompatible constraints: found %d 2-cycle(s)\n",
        g_list_length(_hm_cycles));

    const dt_hm_constraint_choice_t choice
        = _hm_ask_user_constraints_choice(id_ht, faulty ? faulty->id : NULL, sp, sn, dp, dn);

    dt_print(DT_DEBUG_HISTORY,
             "[dt_history_merge_module_list_into_image_topological] incompatible constraints choice: %s\n",
             (choice == DT_HM_CONSTRAINTS_PREFER_SRC) ? "src" : "dst");

    for(GList *l = _hm_cycles; l; l = g_list_next(l))
    {
      _hm_cycle_t *c = (_hm_cycle_t *)l->data;

      dt_digraph_node_t *a = c->a;
      dt_digraph_node_t *b = c->b;

      // Resolve this 2-cycle based on the chosen topology by removing the opposite edge.
      const char *want_prev_a = NULL;
      const char *want_prev_b = NULL;
      if(choice == DT_HM_CONSTRAINTS_PREFER_SRC)
      {
        // Preserve the predecessor relationship implied by the source/paste list.
        want_prev_a = src_prev ? (const char *)g_hash_table_lookup(src_prev, a->id) : NULL;
        want_prev_b = src_prev ? (const char *)g_hash_table_lookup(src_prev, b->id) : NULL;
      }
      else
      {
        // Preserve the predecessor relationship implied by the destination list.
        want_prev_a = dst_prev ? (const char *)g_hash_table_lookup(dst_prev, a->id) : NULL;
        want_prev_b = dst_prev ? (const char *)g_hash_table_lookup(dst_prev, b->id) : NULL;
      }

      if(want_prev_a && !strcmp(want_prev_a, b->id))
      {
        // Keep b -> a, remove a -> b
        _hm_remove_predecessor(b, a);
      }
      else if(want_prev_b && !strcmp(want_prev_b, a->id))
      {
        // Keep a -> b, remove b -> a
        _hm_remove_predecessor(a, b);
      }
      else
      {
        // Fallback: keep destination ordering (least surprising for the current image).
        const char *dpa = dst_prev ? (const char *)g_hash_table_lookup(dst_prev, a->id) : NULL;
        const char *dpb = dst_prev ? (const char *)g_hash_table_lookup(dst_prev, b->id) : NULL;
        if(dpa && !strcmp(dpa, b->id))
          _hm_remove_predecessor(b, a);
        else if(dpb && !strcmp(dpb, a->id))
          _hm_remove_predecessor(a, b);
        else
          _hm_remove_predecessor(b, a);
      }
    }
  }

  g_list_free_full(_hm_cycles, dt_free_gpointer);
  _hm_cycles = NULL;
  g_hash_table_destroy(seen_cycles);
  g_hash_table_destroy(src_prev);
  g_hash_table_destroy(src_next);
  g_hash_table_destroy(dst_prev);
  g_hash_table_destroy(dst_next);
  return 0;

cleanup:
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_topo_resolve_incompatible_constraints] cleanup from line %d: %s\n",
           cleanup_line, cleanup_reason ? cleanup_reason : "unknown");
  if(seen_cycles) g_hash_table_destroy(seen_cycles);
  if(src_prev) g_hash_table_destroy(src_prev);
  if(src_next) g_hash_table_destroy(src_next);
  if(dst_prev) g_hash_table_destroy(dst_prev);
  if(dst_next) g_hash_table_destroy(dst_next);
  g_list_free_full(_hm_cycles, dt_free_gpointer);
  _hm_cycles = NULL;
  return 1;
}

static int _hm_topo_sort_constraints(_hm_topo_merge_ctx_t *ctx)
{
  /* Run a topological sort on the flattened constraint graph.
   *
   * Returns:
   * - 0 on success (ctx->sorted is a linear ordering of nodes),
   * - 1 when constraints are unsatisfiable (cycle).
   *
   * Note:
   * - We may have already removed some direct 2-cycles, but longer cycles can still remain.
   */

  GList *cycle_nodes = NULL;
  const int topo_err = topological_sort(ctx->flat, &ctx->sorted, &cycle_nodes);
  if(topo_err != 0)
  {
    dt_print(DT_DEBUG_HISTORY, "[dt_history_merge_module_list_into_image_topological] iop-order merge: "
                               "unsatisfiable constraints (cycle)\n");
    _hm_show_toposort_cycle_popup(cycle_nodes, ctx->id_ht);
    if(cycle_nodes)
    {
      g_list_free(cycle_nodes);
      cycle_nodes = NULL;
    }
    return 1;
  }

  if(cycle_nodes)
  {
    g_list_free(cycle_nodes);
    cycle_nodes = NULL;
  }
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_topo_sort_constraints] sorted nodes=%d\n", g_list_length(ctx->sorted));
  return 0;
}

static int _hm_topo_apply_solution(_hm_topo_merge_ctx_t *ctx, dt_develop_t *dev_dest, dt_develop_t *dev_src)
{
  /* Apply the topological solution to the destination develop context.
   *
   * For each node id in `ctx->sorted` (solution order), we:
   *  1) ensure the corresponding destination module instance exists (create if missing),
   *  2) copy module contents when the id belongs to the pasted module list,
   *  3) rebuild `dev_dest->iop_order_list` from scratch to match that order.
   *
   * We rebuild the iop_order_list in one shot to avoid leaving partially updated state on error paths.
   */

  // Apply solution:
  // - ensure every solved node exists in dev_dest->iop (create missing as new instances),
  // - copy module content for items that are in mod_list,
  // - rebuild dev_dest->iop_order_list from scratch.
  GList *ordered_modules = NULL;
  int created = 0;
  int copied = 0;

  // Iterate in the final solved order; this is the new pipeline order.
  for(const GList *l = g_list_first(ctx->sorted); l; l = g_list_next(l))
  {
    const dt_digraph_node_t *n = (const dt_digraph_node_t *)l->data;

    _hm_id_info_t *info = (_hm_id_info_t *)g_hash_table_lookup(ctx->id_ht, n->id);
    // Skip nodes that are not part of the kept set (dst/mod/rule).
    if(!(info->flags & ctx->keep_mask)) continue;

    char op[sizeof(((dt_dev_history_item_t *)0)->op_name)];
    char name[sizeof(((dt_dev_history_item_t *)0)->multi_name)];
    _hm_id_to_op_name(n->id, op, name);

    // Resolve (or create) the destination instance for this id.
    dt_iop_module_t *mod_dest = info->dst_iop ? info->dst_iop : dt_dev_get_module_instance(dev_dest, op, name, 0);
    if(IS_NULL_PTR(mod_dest))
    {
      mod_dest = dt_dev_create_module_instance(dev_dest, op, name, 0, TRUE);
      if(IS_NULL_PTR(mod_dest)) return 1;
      created++;
      info->dst_iop = mod_dest;
      info->flags |= HM_ID_FROM_DST_IOP;
    }

    // Only nodes originating from mod_list trigger content overwrite, and only when requested by caller.
    if(ctx->copy_module_contents && info->mod_list)
    {
      if(dt_dev_copy_module_contents(dev_dest, dev_src, mod_dest, info->mod_list)) return 1;
      copied++;
    }

    ordered_modules = g_list_append(ordered_modules, mod_dest);
  }

  // Replace iop_order_list in one shot.
  if(ordered_modules)
  {
    dt_ioppr_rebuild_iop_order_from_modules(dev_dest, ordered_modules);
    g_list_free(ordered_modules);
    ordered_modules = NULL;
  }

  dt_print(
      DT_DEBUG_HISTORY,
      "[dt_history_merge_module_list_into_image_topological] iop-order solve: created=%d copied=%d\n",
      created, copied);
  return 0;
}

static int _hm_try_merge_iop_order_topologically(dt_develop_t *dev_dest, dt_develop_t *dev_src,
                                                 const GList *mod_list, const gboolean merge_iop_order)
{
  /* Topologically merge ordering constraints and apply the result to the destination pipeline.
   *
   * This is intentionally structured as a sequence of small steps operating on `_hm_topo_merge_ctx_t`
   * so that intermediate artifacts (id tables, constraint nodes, flattened graph, sorted list) can be
   * reused and cleaned up reliably.
   */

  _hm_topo_merge_ctx_t ctx = { 0 };
  ctx.copy_module_contents = merge_iop_order;
  ctx.source_iop_order = merge_iop_order;
  ctx.mod_list = mod_list;
  ctx.dev_dest = dev_dest;

  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_try_merge_iop_order_topologically] start merge_iop_order=%d modules=%d dst_iop=%d src_iop=%d\n",
           merge_iop_order, g_list_length((GList *)mod_list), g_list_length(dev_dest->iop),
           g_list_length(dev_src->iop));

  if(_hm_topo_build_id_info_table(&ctx, dev_dest, dev_src, mod_list))
  {
    dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_try_merge_iop_order_topologically] failed: id info table\n");
    _hm_topo_merge_cleanup(&ctx);
    return 1;
  }

  if(_hm_topo_build_constraint_ids(&ctx, dev_dest, dev_src, mod_list, merge_iop_order))
  {
    dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_try_merge_iop_order_topologically] failed: constraint ids\n");
    _hm_topo_merge_cleanup(&ctx);
    return 1;
  }

  if(_hm_topo_flatten_constraints(&ctx))
  {
    dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_try_merge_iop_order_topologically] failed: flatten constraints\n");
    _hm_topo_merge_cleanup(&ctx);
    return 1;
  }

  if(_hm_topo_sort_constraints(&ctx))
  {
    dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_try_merge_iop_order_topologically] failed: topological sort\n");
    _hm_topo_merge_cleanup(&ctx);
    return 1;
  }

  if(_hm_topo_apply_solution(&ctx, dev_dest, dev_src))
  {
    dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_try_merge_iop_order_topologically] failed: apply solution\n");
    _hm_topo_merge_cleanup(&ctx);
    return 1;
  }

  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
           "[_hm_try_merge_iop_order_topologically] success merge_iop_order=%d dst_iop=%d order=%d\n",
           merge_iop_order, g_list_length(dev_dest->iop), g_list_length(dev_dest->iop_order_list));
  _hm_topo_merge_cleanup(&ctx);
  return 0;
}

static void _hm_renumber_history(GList *history)
{
  /* Ensure each history item's `num` matches its position in the list.
   *
   * After concatenation (append/prepend), list indices change. We renumber so that:
   * - debug output is consistent,
   * - DB write/read paths that assume `num` is sequential do not get confused.
   */
  int idx = 0;
  // Assign sequential numbers in list order.
  for(GList *it = g_list_first(history); it; it = g_list_next(it), idx++)
  {
    dt_dev_history_item_t *h = (dt_dev_history_item_t *)it->data;
    h->num = idx;
  }
  dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[_hm_renumber_history] history_len=%d\n", idx);
}

static void _hm_truncate_dest_redo_tail(dt_develop_t *dev_dest)
{
  /* Drop the redo tail from destination history if destination is not at the tip.
   *
   * If a user has undone some steps (history_end < len), the items after history_end are redoable.
   * Any new change (including history merge) invalidates redo, so we remove those items now.
   *
   * This mirrors standard undo/redo behavior: after a new edit you can no longer redo the old tail.
   */

  const int history_end = dt_dev_get_history_end_ext(dev_dest);
  const int history_len = g_list_length(dev_dest->history);

  if(history_end >= history_len)
  {
    dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
             "[_hm_truncate_dest_redo_tail] no redo tail: imgid=%d end=%d len=%d\n",
             dev_dest->image_storage.id, history_end, history_len);
    return;
  }

  dt_print(DT_DEBUG_HISTORY,
           "[dt_history_merge_module_list_into_image_advanced] truncating destination redo tail: end=%d len=%d\n",
           history_end, history_len);

  // history_end is a cursor expressed in "number of applied items" terms:
  // - keep items [0..history_end-1]
  // - remove items [history_end..]
  GList *link = g_list_nth(dev_dest->history, history_end);
  // Walk from the first redo item to the end, freeing and unlinking each node.
  while(link)
  {
    GList *next = g_list_next(link);
    dt_dev_free_history_item(link->data);
    dev_dest->history = g_list_delete_link(dev_dest->history, link);
    link = next;
  }
}

void dt_hm_batch_state_cleanup(dt_hm_batch_state_t *batch)
{
  if(IS_NULL_PTR(batch)) return;
  if(batch->order_ids)
  {
    g_list_free_full(batch->order_ids, dt_free_gpointer);
    batch->order_ids = NULL;
  }
}

static dt_iop_module_t *_hm_dest_module_from_id(dt_develop_t *dev, const char *id)
{
  /* Resolve a node id ("op|multi_name") to a destination module instance.
   * Mirrors the GUI report resolver so cached-order replay matches what the user saw. */
  char op[sizeof(((dt_dev_history_item_t *)0)->op_name)];
  char name[sizeof(((dt_dev_history_item_t *)0)->multi_name)];
  _hm_id_to_op_name(id, op, name);

  dt_iop_module_t *mod = dt_iop_get_module_by_instance_name(dev->iop, op, name);
  if(IS_NULL_PTR(mod) && name[0] == '\0') mod = dt_iop_get_module_by_op_priority(dev->iop, op, 0);
  if(IS_NULL_PTR(mod) && name[0] == '\0') mod = dt_iop_get_module_by_op_priority(dev->iop, op, -1);
  return mod;
}

static GList *_hm_capture_order_ids(dt_develop_t *dev_dest)
{
  /* Snapshot the current destination pipeline order as a list of owned node ids.
   * dev_dest->iop is already sorted in pipeline order after the solve / report reorder. */
  GList *ids = NULL;
  for(GList *l = g_list_first(dev_dest->iop); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    if(IS_NULL_PTR(mod) || mod->iop_order == INT_MAX) continue;
    ids = g_list_append(ids, _hm_make_node_id(mod->op, mod->multi_name));
  }
  return ids;
}

static gboolean _hm_cached_order_applicable(dt_develop_t *dev_dest, GList *order_ids)
{
  /* Decide whether a cached representative order can be replayed verbatim on this image.
   *
   * We require an exact match between the ordering-relevant module set of this image (after the solve has
   * created the pasted instances) and the cached set. If this image has extra modules, or is missing some
   * that the representative had, the topology differs and replaying would silently produce a questionable
   * order. In that case the caller falls back to the interactive merge report for manual control. */
  GHashTable *cached = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  for(GList *l = g_list_first(order_ids); l; l = g_list_next(l))
    g_hash_table_add(cached, g_strdup((const char *)l->data));

  gboolean applicable = TRUE;
  GHashTable *present = g_hash_table_new_full(g_str_hash, g_str_equal, dt_free_gpointer, NULL);
  for(GList *l = g_list_first(dev_dest->iop); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod = (const dt_iop_module_t *)l->data;
    if(IS_NULL_PTR(mod) || mod->iop_order == INT_MAX) continue;
    char *id = _hm_make_node_id(mod->op, mod->multi_name);
    g_hash_table_add(present, id);
    if(!g_hash_table_contains(cached, id)) applicable = FALSE; // extra module not seen on the representative
  }
  if(applicable)
  {
    // Every module the representative ordered must also exist here.
    for(GList *l = g_list_first(order_ids); l; l = g_list_next(l))
      if(!g_hash_table_contains(present, (const char *)l->data))
      {
        applicable = FALSE;
        break;
      }
  }

  g_hash_table_destroy(cached);
  g_hash_table_destroy(present);
  return applicable;
}

static void _hm_apply_cached_order(dt_develop_t *dev_dest, GList *order_ids)
{
  /* Reorder the destination pipeline to match a previously cached order.
   *
   * Modules listed in `order_ids` are placed in that order; any destination module not present in the
   * cache (e.g. an extra module in this image) keeps its current relative position at the tail. This is
   * robust to heterogeneous batches while still replaying the first image's resolved order exactly. */
  GHashTable *placed = g_hash_table_new(g_direct_hash, g_direct_equal);
  GList *ordered = NULL;

  for(GList *l = g_list_first(order_ids); l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = _hm_dest_module_from_id(dev_dest, (const char *)l->data);
    if(mod && !g_hash_table_contains(placed, mod))
    {
      ordered = g_list_append(ordered, mod);
      g_hash_table_add(placed, mod);
    }
  }

  for(GList *l = g_list_first(dev_dest->iop); l; l = g_list_next(l))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)l->data;
    if(mod && !g_hash_table_contains(placed, mod))
    {
      ordered = g_list_append(ordered, mod);
      g_hash_table_add(placed, mod);
    }
  }

  if(ordered) dt_ioppr_rebuild_iop_order_from_modules(dev_dest, ordered);
  g_list_free(ordered);
  g_hash_table_destroy(placed);
}

// Translate an internal `cleanup_reason` tag from `dt_history_merge()` into a message the user
// can actually act on. Returns NULL for reasons that aren't user-facing failures (e.g. a
// deliberate cancel from the merge report dialog).
static const char *_hm_failure_message(const char *cleanup_reason)
{
  if(IS_NULL_PTR(cleanup_reason)) return NULL;

  if(!g_strcmp0(cleanup_reason, "_hm_show_merge_report_popup() revert"))
    return NULL; // user-initiated cancel, not a failure

  if(!g_strcmp0(cleanup_reason, "_hm_try_merge_iop_order_topologically()"))
    return _("Could not paste: the pasted modules require a pipeline order that conflicts "
             "with this image's current module order.");

  if(!g_strcmp0(cleanup_reason, "dt_dev_history_item_from_source_history_item()"))
    return _("Could not paste: one of the pasted modules could not be recreated on the "
             "destination image.");

  return _("Could not paste: an internal error occurred while preparing the merge.");
}

int dt_history_merge(dt_develop_t *dev_dest, dt_develop_t *dev_src, const int32_t dest_imgid,
                     const GList *mod_list, const gboolean merge_iop_order,
                     const dt_history_merge_strategy_t strategy, const gboolean force_new_modules,
                     const char *source_label, dt_hm_batch_state_t *batch)
{
  /* Merge module edits from `dev_src` into `dev_dest` and write the resulting history to DB.
   *
   * Inputs:
   * - `mod_list` is the set of source module instances we want to paste.
   * - `merge_iop_order` decides whether we try to also merge pipeline ordering constraints.
   * - `strategy` chooses whether the pasted history is appended or prepended (prepend) to destination history.
   *
   * High-level algorithm:
   *  1) Invalidate destination redo tail (new edit semantics).
   *  2) Solve and apply a (possibly constrained) iop order topologically.
   *  3) Ensure destination has all module instances required by `mod_list`.
   *  4) Build a temporary history: one history item per module (the last relevant source history item),
   *     but bound to the destination module ordering (which may have changed in step 2).
   *  5) Concatenate temporary history with destination history (append/prepend), renumber, set history_end,
   *     pop into modules, and write to DB.
   *
   * Assumptions:
   * - `dev_dest` is initialized for `dest_imgid` (pipeline loaded, defaults applied).
   * - `dev_src` is non-NULL when `merge_iop_order` is requested.
   */
  if(dest_imgid <= 0) return 1;
  if(IS_NULL_PTR(mod_list)) return 0;

  if(!_hm_warn_missing_raster_producers(mod_list)) return 1;

  int rc = 1;
  gboolean used_source_order = merge_iop_order;
  gboolean revert = FALSE;
  GHashTable *mod_list_ids = NULL;
  GHashTable *src_last_by_id = NULL;
  GHashTable *dst_last_before_by_id = NULL;
  _hm_dest_backup_t backup = { 0 };
  const char *cleanup_reason = NULL;
  int cleanup_line = 0;

  // Snapshot the original destination pipeline and last history items before we modify the destination history.
  if(_hm_build_id_set_from_mod_list(mod_list, &mod_list_ids))
  {
    cleanup_reason = "_hm_build_id_set_from_mod_list(mod_list)";
    cleanup_line = __LINE__;
    goto cleanup;
  }
  if(_hm_backup_dest(dev_dest, mod_list_ids, &backup))
  {
    cleanup_reason = "_hm_backup_dest(dev_dest)";
    cleanup_line = __LINE__;
    goto cleanup;
  }
  if(_hm_build_last_history_by_id(dev_src, &src_last_by_id))
  {
    cleanup_reason = "_hm_build_last_history_by_id(dev_src)";
    cleanup_line = __LINE__;
    goto cleanup;
  }
  if(_hm_build_last_history_by_id(dev_dest, &dst_last_before_by_id))
  {
    cleanup_reason = "_hm_build_last_history_by_id(dev_dest)";
    cleanup_line = __LINE__;
    goto cleanup;
  }

  if(force_new_modules)
    dt_print(DT_DEBUG_HISTORY, "[dt_history_merge] force_new_modules is "
                               "temporarily unsupported, ignoring\n");

  dt_print(DT_DEBUG_HISTORY,
           "[dt_history_merge] imgid=%d merge_iop_order=%d strategy=%d "
           "force_new=%d modules=%d\n",
           dest_imgid, merge_iop_order, strategy, force_new_modules, g_list_length((GList *)mod_list));

  // If the destination history has an undo/redo tail (history_end < length), any new merge must invalidate
  // the redo part, like a regular edit does.
  _hm_truncate_dest_redo_tail(dev_dest);

  // Always run a topological solve so we can insert missing source instances into the destination pipeline.
  // The difference between merge_iop_order modes is which source edges we import (see
  // `_hm_topo_build_constraint_ids()`).
  // The solve always runs (it is the only way to create missing instances and copy their contents), but in
  // batch mode the *ordering* it produces is overridden below by the order resolved on the first accepted
  // image, so a single high-level decision applies uniformly to every image in the batch.
  if(_hm_try_merge_iop_order_topologically(dev_dest, dev_src, mod_list, merge_iop_order))
  {
    // If it failed with source IOP order, retry with destination order. If it was already
    // destination order (or the retry also fails), there is no further fallback: the missing
    // source instances never get created in dev_dest->iop, so continuing would build a temp
    // history against a NULL destination module. Abort instead of falling through.
    gboolean recovered = FALSE;
    if(merge_iop_order)
    {
      used_source_order = FALSE;
      recovered = !_hm_try_merge_iop_order_topologically(dev_dest, dev_src, mod_list, FALSE);
    }
    if(!recovered)
    {
      cleanup_reason = "_hm_try_merge_iop_order_topologically()";
      cleanup_line = __LINE__;
      goto cleanup;
    }
  }

  // Batch replay: if a previous image of this batch already settled an order, reuse it verbatim (including
  // any manual reorder the user did in the report) instead of the order the solve just derived. As a safety
  // net, we only replay when that order maps cleanly onto this image; otherwise we leave the freshly solved
  // order and force the interactive report below so the user can review and reorder manually.
  gboolean use_cached_order = FALSE;
  if(!IS_NULL_PTR(batch) && !IS_NULL_PTR(batch->order_ids))
  {
    use_cached_order = _hm_cached_order_applicable(dev_dest, batch->order_ids);
    if(use_cached_order)
      _hm_apply_cached_order(dev_dest, batch->order_ids);
    else
      dt_print(DT_DEBUG_HISTORY,
               "[dt_history_merge] imgid=%d cached batch order not applicable (topology mismatch); "
               "showing merge report for manual control\n",
               dest_imgid);
  }

  // Sanitize and flatten module order
  dt_ioppr_resync_pipeline(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge", FALSE);

  GList *temp_history = NULL;
  // Build the temporary history list from the source history stack, module-by-module.
  for(const GList *l = g_list_first((GList *)mod_list); l; l = g_list_next(l))
  {
    const dt_iop_module_t *mod_src = (const dt_iop_module_t *)l->data;

    // Last history item for this module in the source history stack.
    const int src_end = dt_dev_get_history_end_ext(dev_src);
    const dt_dev_history_item_t *hist_src
        = dt_dev_history_get_last_item_by_module(dev_src->history, (dt_iop_module_t *)mod_src, src_end);

    // Destination module instance and its current pipeline ordering info. Single-instance
    // modules are keyed by operation only in the destination pipe, so ignore source
    // multi-instance metadata when binding the history item.
    dt_iop_module_t *mod_dest = NULL;
    if((mod_src->flags() & IOP_FLAGS_ONE_INSTANCE) == IOP_FLAGS_ONE_INSTANCE)
      mod_dest = dt_iop_get_module_by_op_priority(dev_dest->iop, mod_src->op, -1);
    else
      mod_dest = dt_dev_get_module_instance(dev_dest, mod_src->op, mod_src->multi_name, mod_src->multi_priority);
    dt_dev_history_item_t *hist = NULL;
    dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE,
             "[dt_history_merge] build temp history: src=%s multi='%s' priority=%d hist=%s dest=%s dest_priority=%d\n",
             mod_src->op, mod_src->multi_name, mod_src->multi_priority,
             hist_src ? "yes" : "no", mod_dest ? "yes" : "no",
             mod_dest ? mod_dest->multi_priority : -1);
    if(dt_dev_history_item_from_source_history_item(dev_dest, dev_src, hist_src, mod_dest, &hist))
    {
      cleanup_reason = "dt_dev_history_item_from_source_history_item()";
      cleanup_line = __LINE__;
      goto cleanup;
    }

    temp_history = g_list_append(temp_history, hist);
  }

  // Concatenate temporary history with destination history in the requested order.
  if(strategy == DT_HISTORY_MERGE_APPEND)
    dev_dest->history = g_list_concat(dev_dest->history, temp_history);
  else // DT_HISTORY_MERGE_PREPEND
    dev_dest->history = g_list_concat(temp_history, dev_dest->history);

  // Don't g_list_free(temp_history), it belongs to dev_dst->history now

  _hm_renumber_history(dev_dest->history);
  dt_dev_set_history_end_ext(dev_dest, g_list_length(dev_dest->history));

  dt_print(DT_DEBUG_HISTORY, "[dt_history_merge] merged history: end=%d len=%d\n",
           dt_dev_get_history_end_ext(dev_dest), g_list_length(dev_dest->history));

  // Stay silent only when the batch already settled a decision AND we can honor it without guessing:
  // a revert is always safe to repeat, but a silent accept requires the cached order to have applied
  // cleanly to this image. Otherwise we re-open the report so the user keeps control.
  const gboolean silent = batch
                          && (batch->decision == DT_HM_BATCH_REVERT
                              || (batch->decision == DT_HM_BATCH_ACCEPT && use_cached_order));
  if(silent)
    revert = (batch->decision == DT_HM_BATCH_REVERT);
  else
    revert = _hm_show_merge_report_popup(dev_dest, dev_src, merge_iop_order, used_source_order, strategy,
                                         src_last_by_id, dst_last_before_by_id, backup.orig_labels,
                                         backup.orig_styles, backup.orig_ids, mod_list_ids, source_label,
                                         batch);

  // Capture the resolved order once, from the first image where the user opted into a silent "accept" for
  // the whole batch. Subsequent images replay it via `_hm_apply_cached_order()` above.
  if(batch && !revert && batch->decision == DT_HM_BATCH_ACCEPT && IS_NULL_PTR(batch->order_ids))
    batch->order_ids = _hm_capture_order_ids(dev_dest);

  if(revert)
  {
    _hm_restore_dest_from_backup(dev_dest, &backup);
    cleanup_reason = "_hm_show_merge_report_popup() revert";
    cleanup_line = __LINE__;
    goto cleanup;
  }

  // Sanitize and flatten module order
  dt_ioppr_resync_pipeline(dev_dest, dest_imgid, "_history_copy_and_paste_on_image_merge 2", FALSE);

  rc = 0;

cleanup:
  if(cleanup_reason)
    dt_print(DT_DEBUG_HISTORY | DT_DEBUG_VERBOSE, "[dt_history_merge] cleanup from line %d: %s\n",
             cleanup_line, cleanup_reason);
  if(rc)
  {
    const char *msg = _hm_failure_message(cleanup_reason);
    if(msg) dt_control_log("%s", msg);
  }
  if(src_last_by_id) g_hash_table_destroy(src_last_by_id);
  if(dst_last_before_by_id) g_hash_table_destroy(dst_last_before_by_id);
  if(mod_list_ids) g_hash_table_destroy(mod_list_ids);
  _hm_backup_cleanup(&backup);
  return rc;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
