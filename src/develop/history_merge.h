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

#ifndef DT_DEVELOP_HISTORY_MERGE_H
#define DT_DEVELOP_HISTORY_MERGE_H

#include <glib.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C"
{
#endif

  struct dt_develop_t;
  struct dt_iop_module_t;

  typedef enum dt_history_merge_strategy_t
  {
    DT_HISTORY_MERGE_PREPEND = 0,   // source applied early in destination history (destination wins conflicts)
    DT_HISTORY_MERGE_APPEND = 1,   // source applied after destination (source wins conflicts)
    DT_HISTORY_MERGE_REPLACE = 2   // entirely replace history and modules order
  } dt_history_merge_strategy_t;

  typedef enum dt_hm_batch_decision_t
  {
    DT_HM_BATCH_UNDECIDED = 0,  // show the report dialog for each image
    DT_HM_BATCH_ACCEPT,         // silently keep the merge result
    DT_HM_BATCH_REVERT,         // silently revert the merge
  } dt_hm_batch_decision_t;

  typedef struct dt_hm_batch_state_t
  {
    dt_hm_batch_decision_t decision;
    // Resolved destination module order (list of owned "op|multi_name" strings) captured from the first
    // accepted image of a batch. When non-NULL, later images replay this order instead of re-solving it,
    // so a single high-level decision (and any manual reorder done in the report) applies to the whole batch.
    GList *order_ids;
  } dt_hm_batch_state_t;

  /**
   * @brief Release resources held by a batch state (the cached order). Safe to call on a zeroed state.
   */
  void dt_hm_batch_state_cleanup(dt_hm_batch_state_t *batch);

  /**
   * @brief Merge a list of modules into a destination image, solving pipeline topologies
   * for proper insertion of source modules.
   *
   * @param dev_dest Destination develop stack (must be initialized, history read and popped).
   * @param dev_src  Source develop stack (must be initialized, history read and popped). May be NULL if
   *                 @p merge_iop_order is FALSE (masks won't be copied).
   * @param dest_imgid Destination image id.
   * @param mod_list List of dt_iop_module_t* to merge (usually coming from dev_src).
   * @param merge_iop_order If TRUE, attempt to merge the pipeline order constraints from src and dest
   *                        using a topological sort. On unsatisfiable constraints, falls back to
   *                        overwriting the destination iop-order list with the source list.
   * @param strategy DT_HISTORY_MERGE_APPEND or DT_HISTORY_MERGE_PREPEND.
   * @param force_new_modules If TRUE, always add modules from source as new instances (when possible).
   * @param source_label Optional source label for the report header (style name, for example).
   *
   * @return 0 on success, 1 on error.
   */
  int dt_history_merge(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src, const int32_t dest_imgid,
                       const GList *mod_list, const gboolean merge_iop_order,
                       const dt_history_merge_strategy_t strategy, const gboolean force_new_modules,
                       const char *source_label, dt_hm_batch_state_t *batch);

#ifdef __cplusplus
}
#endif


/* Node identity helpers, defined in history_merge.c. They were declared in
 * gui/develop/history_merge_gui.h, which had the backend including a GUI header to see
 * its own functions; the GUI half needs them too, and gets them from here. */
char *_hm_make_node_id(const char *op, const char *multi_name);
void _hm_id_to_op_name(const char *id, char *op, char *name);
int _hm_build_last_history_by_id(const struct dt_develop_t *dev, GHashTable **out_map);


typedef enum dt_hm_constraint_choice_t
{
  // Keep the destination adjacency constraints when breaking incompatible 2-cycles.
  DT_HM_CONSTRAINTS_PREFER_DEST = 0,
  // Keep the source/pasted adjacency constraints when breaking incompatible 2-cycles.
  DT_HM_CONSTRAINTS_PREFER_SRC = 1
} dt_hm_constraint_choice_t;

/* The four moments a merge needs a user for.
 *
 * Merging runs deep in the backend but hits situations only a person can settle. Those
 * dialogs used to be called from here directly, which is why a layer-1 file included a gui/
 * header. They are handlers now, registered by gui/develop/history_merge_gui.c, and each has
 * a defined answer for when nobody registered one -- a headless merge must still finish, or
 * refuse, on its own.
 */

/** Break an incompatible 2-cycle by keeping either the destination's or the source's
 *  adjacency constraints. With no handler: PREFER_DEST. The destination image's existing
 *  pipeline order is the thing a merge nobody is watching must not quietly rearrange. */
typedef dt_hm_constraint_choice_t (*dt_hm_constraints_choice_handler_t)(GHashTable *id_ht, const char *faulty_id,
                                                                       const char *src_prev, const char *src_next,
                                                                       const char *dst_prev, const char *dst_next);

/** Warn that pasted modules reference raster mask providers not coming with them; return
 *  FALSE to abort. With no handler: proceed. The modules land without their raster source,
 *  which is what the warning is about -- not grounds to refuse work nobody can confirm. */
typedef gboolean (*dt_hm_missing_raster_handler_t)(const GList *mod_list);

/** Report that the module graph could not be topologically sorted. Informational. */
typedef void (*dt_hm_toposort_cycle_handler_t)(GList *cycle_nodes, GHashTable *id_ht);

/** Show what the merge did and offer to revert it; return TRUE to revert. With no handler:
 *  FALSE. A merge that completed is kept. */
typedef gboolean (*dt_hm_merge_report_handler_t)(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src,
                                                 const gboolean merge_iop_order, const gboolean used_source_order,
                                                 const dt_history_merge_strategy_t strategy,
                                                 GHashTable *src_last_by_id, GHashTable *dst_last_before_by_id,
                                                 const GHashTable *orig_ids, const GHashTable *mod_list_ids,
                                                 const char *source_label, dt_hm_batch_state_t *batch);

void dt_hm_set_constraints_choice_handler(dt_hm_constraints_choice_handler_t handler);
void dt_hm_set_missing_raster_handler(dt_hm_missing_raster_handler_t handler);
void dt_hm_set_toposort_cycle_handler(dt_hm_toposort_cycle_handler_t handler);
void dt_hm_set_merge_report_handler(dt_hm_merge_report_handler_t handler);

#endif // DT_DEVELOP_HISTORY_MERGE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
