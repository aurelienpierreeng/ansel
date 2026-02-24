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

#pragma once

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
    DT_HISTORY_MERGE_APPEND = 0,   // source applied after destination (source wins conflicts)
    DT_HISTORY_MERGE_APPSTART = 1, // source applied early in destination history (destination wins conflicts)
    DT_HISTORY_MERGE_REPLACE = 2   // entirely replace history and modules order
  } dt_history_merge_strategy_t;

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
   * @param strategy DT_HISTORY_MERGE_APPEND or DT_HISTORY_MERGE_APPSTART.
   * @param force_new_modules If TRUE, always add modules from source as new instances (when possible).
   *
   * @return 0 on success, 1 on error.
   */
  int dt_history_merge(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src, const int32_t dest_imgid,
                       const GList *mod_list, const gboolean merge_iop_order,
                       const dt_history_merge_strategy_t strategy, const gboolean force_new_modules);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
