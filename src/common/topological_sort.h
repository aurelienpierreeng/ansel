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
 * @file topological_sort.h
 * @brief Small directed-graph helper for constraint aggregation and topological sorting.
 *
 * This module provides:
 *  - A node model (@ref dt_digraph_node_t) with per-node constraint sets.
 *  - A "flatten" step that canonicalizes nodes by string id and aggregates constraints.
 *  - A depth-first-search topological sort.
 *  - A one-shot cleanup API to free all nodes/constraints/ids (and optionally node payloads).
 *
 * ## Key concepts
 *
 * ### Nodes
 * A node is identified by @ref dt_digraph_node_t::id (a NUL-terminated UTF-8 string).
 * Nodes also carry an opaque payload pointer @ref dt_digraph_node_t::user_data.
 *
 * ### Constraints
 * Constraints are stored per node as a list (@ref dt_digraph_node_t::constraints) of
 * @ref dt_digraph_node_constraints_t*. Each constraint is interpreted *relative to the node
 * owning it*:
 *
 * - If constraint->previous is non-NULL: it means `previous` MUST be ordered before `self`.
 *   This creates a directed edge: `previous -> self`.
 * - If constraint->next is non-NULL: it means `next` MUST be ordered after `self`.
 *   This creates a directed edge: `self -> next`.
 *
 * A node may have **more than one constraint set**, represented by multiple
 * @ref dt_digraph_node_constraints_t objects in its list.
 *
 * ### Flattening / canonicalization
 * In some pipelines, nodes may be duplicated (same id) while each duplicate declares
 * different constraints. @ref flatten_nodes() merges (canonicalizes) such duplicates:
 *
 * - Produces one canonical node per id.
 * - Aggregates all constraint objects onto the canonical node's constraint list.
 * - Ensures constraint endpoints (previous/next) point to canonical nodes as well.
 *
 * The resulting node list is suitable for @ref topological_sort().
 *
 * ## Ownership and lifetime
 *
 * This module assumes that canonical nodes created by @ref flatten_nodes() own:
 *  - node->id (duplicated with g_strdup())
 *  - each dt_digraph_node_constraints_t object in node->constraints
 *  - the node object itself
 *
 * Pointers stored in dt_digraph_node_constraints_t::previous/next are **non-owning references**
 * to other nodes; they must never be freed via the constraint object.
 *
 * The cleanup helper @ref dt_digraph_cleanup_full() frees canonical nodes, their ids,
 * their constraint lists, and each constraint object. It can optionally free user_data
 * using a callback.
 
 * dt_digraph_cleanup_full(flat, NULL, my_user_data_destroy);
 * @endcode
 */

#include <glib.h>

G_BEGIN_DECLS

/**
 * @brief Forward declaration of node type.
 */
typedef struct dt_digraph_node_t dt_digraph_node_t;

/**
 * @brief One constraint set relative to the node that owns it.
 *
 * Each instance encodes up to two ordering relations involving the owning node ("self"):
 *  - If @ref previous is non-NULL: `previous` must come before `self`.
 *  - If @ref next is non-NULL: `self` must come before `next`.
 *
 * Either pointer may be NULL (meaning "no constraint of this kind" for that set).
 * The obvious thing to consider here is that previous/next doesn't imply _immediately_
 * adjacent, otherwise that would be a linked list.
 *
 * @note The pointers stored here are non-owning references.
 *       The constraint object does not own @ref previous or @ref next.
 */
typedef struct
{
  dt_digraph_node_t *previous; /**< Node that must precede the owner node (edge previous->self). */
  dt_digraph_node_t *next;     /**< Node that must follow  the owner node (edge self->next).     */
} dt_digraph_node_constraints_t;

/**
 * @brief Directed graph node.
 *
 * Nodes are identified by @ref id and carry:
 *  - a list of predecessor nodes (@ref previous)
 *  - an opaque payload pointer (@ref user_data)
 *
 * Semantics:
 *  - Each element of node->previous is a dt_digraph_node_t* that MUST come before the owner node.
 *    This encodes a directed edge: previous -> self.
 *
 * When produced by @ref flatten_nodes(), @ref id is owned (g_strdup()) and must be freed.
 */
struct dt_digraph_node_t
{
  const char *id;      /**< Node identifier (owned for canonical nodes created by flatten). */
  char *tag;           /**< Optional label for debugging/provenance (owned; may be NULL). */
  GList *previous;     /**< GList of dt_digraph_node_t* (each element is a predecessor node). */
  void *user_data;     /**< Arbitrary payload (ownership defined by caller; may be freed via callback). */
};

/**
 * @brief Optional destructor for node payloads.
 *
 * This callback is invoked by @ref dt_digraph_cleanup_full() for each canonical node
 * that has a non-NULL @ref dt_digraph_node_t::user_data.
 *
 * If you pass NULL, user_data is left untouched.
 */
typedef void (*dt_node_user_data_destroy_t)(void *data);

/**
 * @brief Canonicalize / merge duplicated nodes by id.
 *
 * Input may contain multiple dt_digraph_node_t objects with the same @ref dt_digraph_node_t::id.
 * Each duplicate can define an independent set of predecessors (i.e. multiple previous entries).
 *
 * This function produces a *flattened* node list containing exactly one canonical node per id,
 * with the following behavior:
 *
 *  - Creates one canonical node per distinct id, allocating it with g_new0().
 *  - Duplicates each canonical id string with g_strdup().
 *  - Aggregates predecessor lists (node->previous) from *all* input duplicates onto the
 *    canonical node’s previous list, merging duplicates (same id) so each predecessor appears
 *    only once.
 *  - Remaps predecessor pointers to point to canonical nodes by matching ids.
 *  - Copies payload: if multiple duplicates carry user_data, the **first non-NULL** user_data
 *    encountered for that id is kept (others are ignored).
 *
 * The resulting list is suitable as input to @ref topological_sort().
 *
 * @param[in]  input_nodes  GList of dt_digraph_node_t* (may contain duplicates by id).
 * @param[out] out_nodes    Receives the canonical node list (unique by id). The list is newly
 *                          allocated; the nodes inside are newly allocated canonical nodes.
 *
 * @return 0 on success, 1 on allocation failure or invalid arguments.
 *
 * @warning This function does NOT free or modify @p input_nodes or the nodes it contains.
 *          If those were heap-allocated, you must free them separately (or ensure they were
 *          owned elsewhere). Only the canonical nodes returned through @p out_nodes are owned
 *          by this module’s cleanup helper.
 */
int flatten_nodes(GList *input_nodes, GList **out_nodes);

/**
 * @brief Perform a topological sort using depth-first search (DFS).
 *
 * Builds the directed edges implied by each node's predecessor lists and produces a list
 * in which every constraint is satisfied (when possible).
 *
 * Edge interpretation (owner node = "self"):
 *  - For each predecessor p in self->previous:
 *      - add edge `p -> self`
 *
 * Then performs a standard DFS topological ordering (reverse postorder). Cycles are detected
 * via a 3-color visitation scheme (WHITE/GRAY/BLACK). If a GRAY node is visited again, the
 * constraint system is unsatisfiable (cycle).
 *
 * @param[in]  nodes   GList of dt_digraph_node_t* representing the graph nodes. Typically the
 *                     output of @ref flatten_nodes(). Nodes referenced only through predecessor
 *                     lists are still considered during sorting if they are present in those lists.
 * @param[out] sorted  Receives the sorted list (GList of dt_digraph_node_t*). The list container
 *                     is newly allocated, but it contains the same node pointers from @p nodes.
 * @param[out] cycle_out Optional. When non-NULL and sorting fails due to a cycle, receives a newly
 *                     allocated GList of dt_digraph_node_t* corresponding to the detected cycle
 *                     (container ownership transferred to caller; nodes are not owned).
 *
 * @return 0 if sorting succeeded, 1 if the constraints contain at least one cycle.
 *
 * @note On success, @p *sorted must be freed with g_list_free() by the caller (container only).
 *       The nodes themselves are not freed by freeing the sorted list.
 */
int topological_sort(GList *nodes, GList **sorted, GList **cycle_out);

/**
 * @brief Free a canonical graph (nodes, constraints, ids) in one call.
 *
 * This is the recommended cleanup for canonical node graphs produced by @ref flatten_nodes().
 * It frees, for each node in the provided set:
 *  - every dt_digraph_node_constraints_t object in node->constraints
 *  - the node->constraints GList container
 *  - node->id (assumed owned; freed with g_free)
 *  - the node itself
 *  - optionally node->user_data via @p user_destroy
 *
 * You may provide either:
 *  - @p nodes: a GList of canonical nodes (unique pointers)
 *  - @p node_ht: a hashtable mapping id->node (or any key->node), containing each canonical
 *    node exactly once as a value
 *
 * If both are provided (non-NULL), @p node_ht is used as the authoritative set and @p nodes
 * is ignored for node destruction (you may still free the list container yourself).
 *
 * @param[in] nodes       Canonical node list (may be NULL if node_ht is provided).
 * @param[in] node_ht     Hashtable containing canonical nodes as values (may be NULL).
 * @param[in] user_destroy Optional destructor for node->user_data; may be NULL.
 *
 * @note If you call this with a hashtable and you also keep a list of the same nodes,
 *       you must NOT free nodes twice. In that scenario, call:
 *       - dt_digraph_cleanup_full(NULL, ht, ...)
 *       - g_list_free(list) // container only
 */
void dt_digraph_cleanup_full(
  GList *nodes,
  GHashTable *node_ht,
  dt_node_user_data_destroy_t user_destroy);

/**
 * @brief Allocate and initialize a new digraph node with the given id.
 *
 * The returned node owns its id (g_strdup), has NULL previous and user_data.
 *
 * @param id NUL-terminated string identifier (copied).
 * @return Newly allocated node, or NULL on allocation failure.
 */
dt_digraph_node_t *dt_digraph_node_new(const char *id);

G_END_DECLS
