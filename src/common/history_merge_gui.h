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

#include "common/history_merge.h"

#include <glib.h>

struct dt_develop_t;

char *_hm_make_node_id(const char *op, const char *multi_name);
void _hm_id_to_op_name(const char *id, char *op, char *name);

typedef enum dt_hm_constraint_choice_t
{
  // Keep the destination adjacency constraints when breaking incompatible 2-cycles.
  DT_HM_CONSTRAINTS_PREFER_DEST = 0,
  // Keep the source/pasted adjacency constraints when breaking incompatible 2-cycles.
  DT_HM_CONSTRAINTS_PREFER_SRC = 1
} dt_hm_constraint_choice_t;

dt_hm_constraint_choice_t _hm_ask_user_constraints_choice(GHashTable *id_ht, const char *faulty_id,
                                                         const char *src_prev, const char *src_next,
                                                         const char *dst_prev, const char *dst_next);

gboolean _hm_warn_missing_raster_producers(const GList *mod_list);

void _hm_show_toposort_cycle_popup(GList *cycle_nodes, GHashTable *id_ht);

int _hm_build_last_history_by_id(const struct dt_develop_t *dev, GHashTable **out_map);

GPtrArray *_hm_collect_labels_from_history_map(GHashTable *last_by_id, const GHashTable *mod_list_ids,
                                               GPtrArray **out_styles);

gboolean _hm_show_merge_report_popup(struct dt_develop_t *dev_dest, struct dt_develop_t *dev_src,
                                     const gboolean merge_iop_order, const gboolean used_source_order,
                                     const dt_history_merge_strategy_t strategy, GHashTable *src_last_by_id,
                                     GHashTable *dst_last_before_by_id, const GPtrArray *orig_labels,
                                     const GPtrArray *orig_styles, const GHashTable *orig_ids,
                                     const GHashTable *mod_list_ids);
