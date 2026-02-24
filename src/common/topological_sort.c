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

#include "common/topological_sort.h"
#include <stdio.h>
#include <string.h>

/* ------------------------- flatten_nodes() ------------------------- */

static dt_digraph_node_t *_get_or_create_node(GHashTable *by_id, const char *id)
{
  dt_digraph_node_t *n = g_hash_table_lookup(by_id, id);
  if(n) return n;

  n = g_new0(dt_digraph_node_t, 1);
  n->id = g_strdup(id);
  n->tag = NULL;
  n->previous = NULL;
  n->user_data = NULL;

  g_hash_table_insert(by_id, (gpointer)n->id, n);
  return n;
}

dt_digraph_node_t *dt_digraph_node_new(const char *id)
{
  dt_digraph_node_t *n = g_new0(dt_digraph_node_t, 1);
  if(!n) return NULL;
  n->id = g_strdup(id);
  n->tag = NULL;
  n->previous = NULL;
  n->user_data = NULL;
  return n;
}

int flatten_nodes(GList *input_nodes, GList **out_nodes)
{
  if(!out_nodes) return 1;
  *out_nodes = NULL;

  GHashTable *by_id = g_hash_table_new(g_str_hash, g_str_equal);
  if(!by_id) return 1;

  // 1) Create canonical nodes for every id we see (and keep first non-NULL user_data)
  for(GList *it = g_list_first(input_nodes); it; it = g_list_next(it))
  {
    dt_digraph_node_t *in = (dt_digraph_node_t *)it->data;
    if(!in || !in->id) continue;

    dt_digraph_node_t *canon = _get_or_create_node(by_id, in->id);
    if(!canon->user_data && in->user_data) canon->user_data = in->user_data;
    if(!canon->tag && in->tag) canon->tag = g_strdup(in->tag);
    if(canon->tag && in->tag && strcmp(canon->tag, in->tag))
    {
      // Minimal provenance merging: keep both when we see a conflict.
      if(((!strcmp(canon->tag, "dst") && !strcmp(in->tag, "src"))
          || (!strcmp(canon->tag, "src") && !strcmp(in->tag, "dst"))))
      {
        g_free(canon->tag);
        canon->tag = g_strdup("dst+src");
      }
      else if(strcmp(canon->tag, "dst+src"))
      {
        g_free(canon->tag);
        canon->tag = g_strdup("mixed");
      }
    }
  }

  // 2) Merge previous lists onto canonical nodes, remapping to canonical nodes by id and deduplicating
  for(GList *it = g_list_first(input_nodes); it; it = g_list_next(it))
  {
    dt_digraph_node_t *in = (dt_digraph_node_t *)it->data;
    if(!in || !in->id) continue;

    dt_digraph_node_t *self = _get_or_create_node(by_id, in->id);

    for(GList *p = g_list_first(in->previous); p; p = g_list_next(p))
    {
      dt_digraph_node_t *pred = (dt_digraph_node_t *)p->data;
      if(!pred || !pred->id) continue;

      dt_digraph_node_t *cpred = _get_or_create_node(by_id, pred->id);

      // add cpred to self->previous if not already present
      gboolean found = FALSE;
      for(GList *q = g_list_first(self->previous); q; q = g_list_next(q))
      {
        if(q->data == cpred) { found = TRUE; break; }
      }
      if(!found) self->previous = g_list_prepend(self->previous, cpred);
    }
  }

  // 3) Build output list with unique nodes, preserving first-seen order from input_nodes
  GHashTable *added = g_hash_table_new(g_str_hash, g_str_equal);
  if(!added)
  {
    g_hash_table_destroy(by_id);
    return 1;
  }

  for(GList *it = g_list_first(input_nodes); it; it = g_list_next(it))
  {
    dt_digraph_node_t *in = (dt_digraph_node_t *)it->data;
    if(!in || !in->id) continue;

    if(g_hash_table_contains(added, in->id)) continue;

    dt_digraph_node_t *canon = g_hash_table_lookup(by_id, in->id);
    if(canon)
    {
      *out_nodes = g_list_append(*out_nodes, canon);
      g_hash_table_add(added, (gpointer)canon->id);
    }
  }

  // Also include nodes that exist only because they were referenced by previous
  GHashTableIter iter;
  gpointer key = NULL, val = NULL;
  g_hash_table_iter_init(&iter, by_id);
  while(g_hash_table_iter_next(&iter, &key, &val))
  {
    const char *id = (const char *)key;
    dt_digraph_node_t *canon = (dt_digraph_node_t *)val;
    if(!g_hash_table_contains(added, id)) *out_nodes = g_list_append(*out_nodes, canon);
  }

  g_hash_table_destroy(added);
  g_hash_table_destroy(by_id);

  return 0;
}


/* ---------------------- topological_sort() (DFS) ---------------------- */

/*
  Constraint semantics used here (common with this structure):
    - Each dt_digraph_node_constraints_t is stored in node->constraints, so "self" is the owner.
    - If c->previous != NULL: edge (c->previous -> self)
    - If c->next     != NULL: edge (self -> c->next)

  Returns 0 on success, 1 on cycle / unsatisfiable.
*/

typedef enum
{
  DT_VISIT_WHITE = 0,
  DT_VISIT_GRAY = 1,
  DT_VISIT_BLACK = 2
} dt_visit_color_t;

static gboolean _add_edge(GHashTable *outgoing, dt_digraph_node_t *from, dt_digraph_node_t *to)
{
  if(!from || !to) return FALSE;

  GList *lst = (GList *)g_hash_table_lookup(outgoing, from);
  lst = g_list_prepend(lst, to);
  g_hash_table_replace(outgoing, from, lst);
  return TRUE;
}

static GList *_toposort_extract_cycle_from_stack(const GList *dfs_stack, const dt_digraph_node_t *gray)
{
  /* Build a cycle list from the current DFS recursion stack.
   *
   * We keep `dfs_stack` as a LIFO list whose head is the currently explored node.
   * When DFS reaches a GRAY node again, that node is an ancestor present somewhere in the stack.
   *
   * The cycle nodes are the sub-path:
   *   gray -> ... -> current
   * in traversal order. The returned list contains each node once (no closing duplicate).
   *
   * Ownership:
   * - The returned list container is newly allocated and must be freed with g_list_free() by the caller.
   * - The node pointers are not owned (they belong to the canonical node graph).
   */
  if(!dfs_stack || !gray) return NULL;

  GList *cycle = NULL;
  for(const GList *it = dfs_stack; it; it = g_list_next(it))
  {
    dt_digraph_node_t *n = (dt_digraph_node_t *)it->data;
    if(!n) continue;
    cycle = g_list_prepend(cycle, n);
    if(n == gray) break;
  }
  return g_list_reverse(cycle);
}

static gboolean _dfs_visit(dt_digraph_node_t *n,
                           GHashTable *outgoing,
                           GHashTable *color,
                           GList **result,
                           int *dfs_count,
                           GList **dfs_stack,
                           GList **cycle_out)
{
  dt_visit_color_t c = (dt_visit_color_t)GPOINTER_TO_INT(g_hash_table_lookup(color, n));

  if(c == DT_VISIT_GRAY)
  {
    fprintf(stderr, "[toposort] Cycle detected visiting node '%s'\n", n->id);
    if(cycle_out && !*cycle_out) *cycle_out = _toposort_extract_cycle_from_stack(*dfs_stack, n);
    return TRUE;   // cycle
  }
  if(c == DT_VISIT_BLACK)
  {
    fprintf(stderr, "[toposort] Already finished node '%s'\n", n->id);
    return FALSE; // already done
  }

  *dfs_count = *dfs_count + 1;

  // Push on the recursion stack so we can reconstruct a cycle path if needed.
  if(dfs_stack) *dfs_stack = g_list_prepend(*dfs_stack, n);

  g_hash_table_replace(color, n, GINT_TO_POINTER(DT_VISIT_GRAY));

  GList *nbrs = (GList *)g_hash_table_lookup(outgoing, n);
  for(GList *it = nbrs; it; it = g_list_next(it))
  {
    dt_digraph_node_t *m = (dt_digraph_node_t *)it->data;
    if(_dfs_visit(m, outgoing, color, result, dfs_count, dfs_stack, cycle_out))
    {
      // Pop before propagating the cycle error.
      if(dfs_stack && *dfs_stack) *dfs_stack = g_list_delete_link(*dfs_stack, *dfs_stack);
      return TRUE;
    }
  }

  g_hash_table_replace(color, n, GINT_TO_POINTER(DT_VISIT_BLACK));
  *result = g_list_prepend(*result, n);

  // Pop on normal exit.
  if(dfs_stack && *dfs_stack) *dfs_stack = g_list_delete_link(*dfs_stack, *dfs_stack);
  return FALSE;
}

int topological_sort(GList *nodes, GList **sorted, GList **cycle_out)
{
  if(!sorted) return 1;
  *sorted = NULL;
  if(cycle_out) *cycle_out = NULL;

  // Debug: print initial nodes and their predecessors
  fprintf(stderr, "[toposort] Initial node list and constraints:\n");
  for(GList *it = g_list_first(nodes); it; it = g_list_next(it))
  {
    dt_digraph_node_t *n = (dt_digraph_node_t *)it->data;
    fprintf(stderr, "[toposort]   node '%s'", n->id);
    if(n->tag) fprintf(stderr, " [%s]", n->tag);
    fprintf(stderr, " (predecessors:");
    for(GList *p = g_list_first(n->previous); p; p = g_list_next(p))
    {
      dt_digraph_node_t *pred = (dt_digraph_node_t *)p->data;
      fprintf(stderr, " '%s'", pred->id);
    }
    fprintf(stderr, ")\n");
  }

  // Debug: print all edges (previous -> node)
  fprintf(stderr, "[toposort] Edges:\n");
  for(GList *it = g_list_first(nodes); it; it = g_list_next(it))
  {
    dt_digraph_node_t *n = (dt_digraph_node_t *)it->data;
    for(GList *p = g_list_first(n->previous); p; p = g_list_next(p))
    {
      dt_digraph_node_t *pred = (dt_digraph_node_t *)p->data;
      fprintf(stderr, "[toposort]   '%s' -> '%s'\n", pred->id, n->id);
    }
  }

  GHashTable *outgoing = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
  GHashTable *color = g_hash_table_new(g_direct_hash, g_direct_equal);
  if(!outgoing || !color)
  {
    if(outgoing) g_hash_table_destroy(outgoing);
    if(color) g_hash_table_destroy(color);
    return 1;
  }

  // Ensure every node appears as a key (even if it has no outgoing edges)
  for(GList *it = g_list_first(nodes); it; it = g_list_next(it))
  {
    dt_digraph_node_t *n = (dt_digraph_node_t *)it->data;
    if(!n) continue;

    if(!g_hash_table_contains(outgoing, n)) g_hash_table_insert(outgoing, n, NULL);
    if(!g_hash_table_contains(color, n)) g_hash_table_insert(color, n, GINT_TO_POINTER(DT_VISIT_WHITE));
  }

  // Build edges from previous lists: for each node, for each pred in node->previous, add edge pred -> node
  for(GList *it = g_list_first(nodes); it; it = g_list_next(it))
  {
    dt_digraph_node_t *self = (dt_digraph_node_t *)it->data;
    if(!self) continue;

    for(GList *p = g_list_first(self->previous); p; p = g_list_next(p))
    {
      dt_digraph_node_t *pred = (dt_digraph_node_t *)p->data;
      if(!pred) continue;

      if(!g_hash_table_contains(outgoing, pred)) g_hash_table_insert(outgoing, pred, NULL);
      if(!g_hash_table_contains(color, pred))
        g_hash_table_insert(color, pred, GINT_TO_POINTER(DT_VISIT_WHITE));

      _add_edge(outgoing, pred, self);
    }
  }

  // DFS over all nodes (including any that appeared only in previous)
  GList *result = NULL;
  GList *cycle = NULL;
  GList *dfs_stack = NULL;

  int dfs_count = 0;

  GHashTableIter iter;
  gpointer key = NULL, val = NULL;
  g_hash_table_iter_init(&iter, outgoing);
  while(g_hash_table_iter_next(&iter, &key, &val))
  {
    dt_digraph_node_t *n = (dt_digraph_node_t *)key;
    dt_visit_color_t c = (dt_visit_color_t)GPOINTER_TO_INT(g_hash_table_lookup(color, n));
    if(c == DT_VISIT_WHITE)
    {
      if(_dfs_visit(n, outgoing, color, &result, &dfs_count, &dfs_stack, &cycle))
      {
        g_list_free(result);
        if(dfs_stack) g_list_free(dfs_stack);
        g_hash_table_destroy(outgoing);
        g_hash_table_destroy(color);
        *sorted = NULL;
        if(cycle_out) *cycle_out = cycle;
        else if(cycle) g_list_free(cycle);
        return 1;
      }
      dfs_count++;
    }
  }

  if(dfs_stack) g_list_free(dfs_stack);
  if(cycle) g_list_free(cycle); // should never happen on success, but keep cleanup symmetric

  g_hash_table_destroy(outgoing);
  g_hash_table_destroy(color);

  *sorted = result;

  // Debug: print the sorted solution
  fprintf(stderr, "[toposort] Solution order:\n");
  int idx = 0;
  for(GList *it = g_list_first(result); it; it = g_list_next(it), idx++)
  {
    dt_digraph_node_t *n = (dt_digraph_node_t *)it->data;
    fprintf(stderr, "[toposort]   %d: '%s'", idx, n->id);
    if(n->tag) fprintf(stderr, " [%s]", n->tag);
    fprintf(stderr, "\n");
  }

  fprintf(stderr, "[toposort] DFS visits performed: %d\n", dfs_count);

  return 0;
}

/*
  Frees one node and everything owned by it.
  - constraints
  - constraint objects
  - node
  Optional:
  - id
  - user_data
*/

static void dt_digraph_node_free_full(dt_digraph_node_t *node, dt_node_user_data_destroy_t user_destroy)
{
  if(!node) return;

  if(node->previous) g_list_free(node->previous);

  if(user_destroy && node->user_data) user_destroy(node->user_data);

  if(node->tag) g_free(node->tag);
  g_free((char *)node->id);

  g_free(node);
}

static void _dt_digraph_nodes_free_full(GList *nodes, dt_node_user_data_destroy_t user_destroy)
{
  for(GList *it = g_list_first(nodes); it; it = g_list_next(it))
    dt_digraph_node_free_full(it->data, user_destroy);

  g_list_free(nodes);
}

static void _dt_digraph_nodes_hashtable_free_full(GHashTable *ht, dt_node_user_data_destroy_t user_destroy)
{
  if(!ht) return;

  GHashTableIter iter;
  gpointer key, val;

  g_hash_table_iter_init(&iter, ht);
  while(g_hash_table_iter_next(&iter, &key, &val))
  {
    dt_digraph_node_t *node = val;
    dt_digraph_node_free_full(node, user_destroy);
  }

  g_hash_table_destroy(ht);
}

void dt_digraph_cleanup_full(GList *nodes, GHashTable *node_ht, dt_node_user_data_destroy_t user_destroy)
{
  if(node_ht)
    _dt_digraph_nodes_hashtable_free_full(node_ht, user_destroy);
  else
    _dt_digraph_nodes_free_full(nodes, user_destroy);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
