/*
    This file is part of darktable,
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019, 2021, 2025 Aurélien PIERRE.
    Copyright (C) 2019 Hanno Schwalm.
    Copyright (C) 2019 luzpaz.
    Copyright (C) 2019 Marcus Rückert.
    Copyright (C) 2019-2020 Pascal Obry.
    Copyright (C) 2020 Philippe Weyland.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.
    
    You should have received a copy of the GNU Lesser General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
/**
   What is the IOP order ?

      The IOP order support is the way to order the modules in the
      pipe. There was the pre-3.0 version which is called "legacy" and
      the post-3.0 called v3.0 which has been introduced to keep a
      clean linear part in the pipe to avoid different issues about
      color shift.

   How is this stored in the DB ?

      For each image we keep record of the iop-order, there is
      basically three cases:

      1. order is legacy (built-in)

         All modules are sorted using the legacy order (see table
         below). We still have a legacy order if all multiple
         instances of the same module are grouped together.

      2. order is v3.0 (built-in)

         All modules are sorted using the v3.0 order (see table
         below). We still have a v3.0 order if all multiple
         instances of the same module are grouped together.

      3. order is custom

         All other cases. Either:
         - the modules are not sorted using one of the order above.
         - some instances have been moved and so not grouped together.

      The order for each image is stored into the table module_order,
      the table contains:
         - imgid    : the id of the image
         - iop_list : the ordered list of modules + the multi-priority
         - version  : the iop order version

       For each version we set:

         - legacy : iop_list = NULL / version = 1
         - v3.0   : iop_list = NULL / version = 2
         - custom : iop_list to ordered list of each modules / version = 0

         This writing is done with dt_ioppr_write_iop_order.

   How to ensure the order is correct ?

      Initial implementation:

         We used to have a double value to sort the modules in memory
         for the final part order. Adding a new instance meant to use
         a value (double) in middle of the before and after modules'
         iop-order. Also this double was stored with the history and
         we supposed to be stable for all the life the picture. This
         did not worked as expected as with each instance created,
         removed or moved the gap between each modules was shrinking
         and finally created clashes (multiple modules with the same
         order).

         Also the history had only the active modules and no
         information at all about other modules. It was impossible to
         properly migrate some pictures because of this.

      New (this) implementation:

         The iop-order is a simple chained list and only this list is
         used to order the module. One can create, delete or move
         instances at will. There won't be clashes. We still have an
         iop-order integer used to reorder the module in memory by
         using a simple sort. But this is not used to map the history
         at all. This makes it possible to migrate from one order to
         another. (see below for a discussion about the history
         mapping).

         The iop-order list kept into the database contains all known
         modules. So we can migrate and/or copy/paste with better
         respect of the source or target order for example.

         For example we can copy an history from an image using a
         legacy order and paste it to an image using the v3.0 order
         and place the module at the proper position in the pipe for
         the target. Likewise for styles.

   How is this used to read an image (setup the iop-order) ?

      Loading and image means:

         - getting the iop-order list (dt_ioppr_get_iop_order_list)
         - reading the history and mapping it to the iop-order list

      How is the mapping of history and iop-order list done ?

         Each history item contains the name of the operation
         (e.g. exposure, clip) and the multi-instance number. Both
         information are used as the stable key to map the history
         item into the iop-list.

         This is done by using the dt_ioppr_get_iop_order.
 */

#ifndef DT_IOP_ORDER_H
#define DT_IOP_ORDER_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct dt_iop_module_t;
struct dt_develop_t;
struct dt_dev_pixelpipe_t;

typedef enum dt_iop_order_t
{
  DT_IOP_ORDER_CUSTOM  = 0, // a customr order (re-ordering the pipe)
  DT_IOP_ORDER_LEGACY  = 1, // up to dt 2.6.3
  DT_IOP_ORDER_V30     = 2, // starts with dt 3.0
  DT_IOP_ORDER_V30_JPG = 3, // same as previous but tuned for non-linear input
  DT_IOP_ORDER_ANSEL_RAW = 4,
  DT_IOP_ORDER_ANSEL_JPG = 5,
  DT_IOP_ORDER_LAST    = 6
} dt_iop_order_t;

typedef struct dt_iop_order_entry_t
{
  union {
    double iop_order_f;  // only used for backward compatibility while migrating db
    int iop_order;       // order from 1 and incrementing
  } o;
  // operation + instance is the unique id for an active module in the pipe
  char operation[20];
  int32_t instance;      // or previously named multi_priority
  char name[25];
} dt_iop_order_entry_t;

typedef struct dt_iop_order_rule_t
{
  char op_prev[20];
  char op_next[20];
} dt_iop_order_rule_t;

/**
 * @brief Return the human-readable name for an IOP order enum value.
 *
 * This is mostly used for debug strings, logs, and UI messages.
 *
 * @param order IOP order enum value.
 * @return Static string describing the order.
 */
const char *dt_iop_order_string(const dt_iop_order_t order);

/**
 * @brief Fetch the IOP order version stored for an image.
 *
 * If the image has no stored order, this falls back to the default
 * (currently DT_IOP_ORDER_ANSEL_RAW).
 *
 * @param imgid Image id.
 * @return Stored order version or the default built-in order.
 */
dt_iop_order_t dt_ioppr_get_iop_order_version(const int32_t imgid);

/**
 * @brief Determine the kind of an order list by inspecting its content.
 *
 * Compares the list ordering to the known built-in lists to decide whether
 * this is a built-in version or a custom order.
 *
 * @param iop_order_list List of @ref dt_iop_order_entry_t.
 * @return A built-in order version or DT_IOP_ORDER_CUSTOM.
 */
dt_iop_order_t dt_ioppr_get_iop_order_list_kind(GList *iop_order_list);

/**
 * @brief Check whether the image has an explicit order list stored in DB.
 *
 * @param imgid Image id.
 * @return TRUE if an order list exists, FALSE otherwise.
 */
gboolean dt_ioppr_has_iop_order_list(int32_t imgid);

/**
 * @brief Load the order list for an image from the DB.
 *
 * If no list is found, returns NULL.
 *
 * @param imgid Image id.
 * @param sorted If TRUE, entries are sorted by iop_order; otherwise the stored order is kept.
 * @return A newly-allocated list of @ref dt_iop_order_entry_t or NULL.
 */
GList *dt_ioppr_get_iop_order_list(int32_t imgid, gboolean sorted);
/**
 * @brief Return the built-in order list for a given version.
 *
 * @param version Built-in order version.
 * @return A newly-allocated list of @ref dt_iop_order_entry_t.
 */
GList *dt_ioppr_get_iop_order_list_version(dt_iop_order_t version);
/**
 * @brief Find a list link matching an operation and instance.
 *
 * @param iop_order_list Order list to search.
 * @param op_name Operation name.
 * @param multi_priority Instance priority.
 * @return The matching list node or NULL if not found.
 */
GList *dt_ioppr_get_iop_order_link(GList *iop_order_list, const char *op_name, const int multi_priority);
/**
 * @brief Detect whether multiple instances are grouped for a non-custom order.
 *
 * Used to decide if a list follows the built-in ordering conventions.
 *
 * @param iop_order_list Order list to inspect.
 * @return TRUE if instances are grouped together, FALSE otherwise.
 */
gboolean dt_ioppr_has_multiple_instances(GList *iop_order_list);

/**
 * @brief Return the iop_order for a given operation/instance pair.
 *
 * @param iop_order_list Order list to search.
 * @param op_name Operation name.
 * @param multi_priority Instance priority.
 * @return The iop_order value, or 0 if not found.
 */
int dt_ioppr_get_iop_order(GList *iop_order_list, const char *op_name, const int multi_priority);

/**
 * @brief Persist an order list to the DB for a given image.
 *
 * @param iop_order_list Order list to store.
 * @param imgid Image id.
 * @return TRUE on success, FALSE on error.
 */
gboolean dt_ioppr_write_iop_order_list(GList *iop_order_list, const int32_t imgid);

/**
 * @brief Serialize an order list into a binary blob (used for presets).
 *
 * @param iop_order_list Order list to serialize.
 * @param size Output size of the serialized blob.
 * @return Allocated buffer to be freed by the caller.
 */
void *dt_ioppr_serialize_iop_order_list(GList *iop_order_list, size_t *size);
/**
 * @brief Deserialize an order list from a binary blob.
 *
 * @param buf Serialized buffer.
 * @param size Buffer size in bytes.
 * @return Newly-allocated order list or NULL on error.
 */
GList *dt_ioppr_deserialize_iop_order_list(const char *buf, size_t size);
/**
 * @brief Serialize an order list to a text representation.
 *
 * @param iop_order_list Order list to serialize.
 * @return Newly-allocated NUL-terminated string.
 */
char *dt_ioppr_serialize_text_iop_order_list(GList *iop_order_list);
/**
 * @brief Deserialize an order list from a text representation.
 *
 * @param buf NUL-terminated string.
 * @return Newly-allocated order list or NULL on error.
 */
GList *dt_ioppr_deserialize_text_iop_order_list(const char *buf);

/**
 * @brief Ensure a module instance has an entry in dev->iop_order_list.
 *
 * Inserts a new entry if missing, keeping list consistency for subsequent
 * reordering or serialization.
 *
 * @param dev Develop context.
 * @param module Module instance to insert.
 */
void dt_ioppr_insert_module_instance(struct dt_develop_t *dev, struct dt_iop_module_t *module);
/**
 * @brief Update dev->iop module order values from dev->iop_order_list.
 *
 * This writes iop_order fields on modules to match the stored list.
 *
 * @param dev Develop context.
 */
void dt_ioppr_resync_modules_order(struct dt_develop_t *dev);
/**
 * @brief Resynchronize pipeline order and related structures.
 *
 * Rebuilds ordering for modules/history and optionally checks for duplicate
 * iop_order values.
 *
 * @param dev Develop context.
 * @param imgid Image id (for diagnostics).
 * @param msg Optional debug message.
 * @param check_duplicates Whether to validate duplicate iop_order entries.
 */
void dt_ioppr_resync_pipeline(struct dt_develop_t *dev, const int32_t imgid, const char *msg, gboolean check_duplicates);

/**
 * @brief Rebuild dev->iop_order_list from a list of ordered modules.
 *
 * @param dev Develop context.
 * @param ordered_modules Modules in the desired pipeline order.
 */
void dt_ioppr_rebuild_iop_order_from_modules(struct dt_develop_t *dev, GList *ordered_modules);

/**
 * @brief Update dev->iop_order_list with modules referenced by style items.
 *
 * @param dev Develop context.
 * @param st_items Style items list.
 * @param append Whether to append new entries at the end (TRUE) or merge into list order (FALSE).
 */
void dt_ioppr_update_for_style_items(struct dt_develop_t *dev, GList *st_items, gboolean append);
/**
 * @brief Update dev->iop_order_list with modules from a module list.
 *
 * @param dev Develop context.
 * @param modules List of module instances.
 * @param append Whether to append new entries at the end (TRUE) or merge into list order (FALSE).
 */
void dt_ioppr_update_for_modules(struct dt_develop_t *dev, GList *modules, gboolean append);

/**
 * @brief Set dev->iop_order_list to the default order for a given image.
 *
 * Uses the image type/workflow to pick an appropriate built-in list.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 */
void dt_ioppr_set_default_iop_order(struct dt_develop_t *dev, const int32_t imgid);
/**
 * @brief Replace the current order list with a new one and persist it.
 *
 * @param dev Develop context.
 * @param imgid Image id.
 * @param new_iop_list New order list (ownership remains with caller).
 */
void dt_ioppr_change_iop_order(struct dt_develop_t *dev, const int32_t imgid, GList *new_iop_list);

/**
 * @brief Check whether any module .so is missing an iop_order entry.
 *
 * @param iop_list List of module .so entries.
 * @param iop_order_list Current order list.
 * @return 1 if any module is missing an entry, 0 otherwise.
 */
int dt_ioppr_check_so_iop_order(GList *iop_list, GList *iop_order_list);

/**
 * @brief Return the list of ordering rules (prev/next constraints).
 *
 * @return List of @ref dt_iop_order_rule_t.
 */
GList *dt_ioppr_get_iop_order_rules();

/**
 * @brief Deep-copy an order list.
 *
 * @param iop_order_list Source list.
 * @return Newly-allocated list with duplicated entries.
 */
GList *dt_ioppr_iop_order_copy_deep(GList *iop_order_list);

/**
 * @brief Compare two module instances by iop_order for sorting.
 *
 * @param a First module pointer.
 * @param b Second module pointer.
 * @return Sorting comparison result.
 */
gint dt_sort_iop_by_order(gconstpointer a, gconstpointer b);
/**
 * @brief Compare two list nodes holding modules by iop_order.
 *
 * @param a GList node containing a module.
 * @param b GList node containing a module.
 * @return Sorting comparison result.
 */
gint dt_sort_iop_list_by_order_f(gconstpointer a, gconstpointer b);

/**
 * @brief Validate whether module can be moved before module_next.
 *
 * @param iop_list Current module list.
 * @param module Module to move.
 * @param module_next Target module that should follow.
 * @return TRUE if move is allowed, FALSE otherwise.
 */
gboolean dt_ioppr_check_can_move_before_iop(GList *iop_list, struct dt_iop_module_t *module, struct dt_iop_module_t *module_next);
/**
 * @brief Validate whether module can be moved after module_prev.
 *
 * @param iop_list Current module list.
 * @param module Module to move.
 * @param module_prev Target module that should precede.
 * @return TRUE if move is allowed, FALSE otherwise.
 */
gboolean dt_ioppr_check_can_move_after_iop(GList *iop_list, struct dt_iop_module_t *module, struct dt_iop_module_t *module_prev);

/**
 * @brief Move a module instance before another module in the pipe.
 *
 * Updates module ordering and related lists.
 *
 * @param dev Develop context.
 * @param module Module to move.
 * @param module_next Module that should follow after move.
 * @return TRUE if move succeeded, FALSE otherwise.
 */
gboolean dt_ioppr_move_iop_before(struct dt_develop_t *dev, struct dt_iop_module_t *module, struct dt_iop_module_t *module_next);
/**
 * @brief Move a module instance after another module in the pipe.
 *
 * Updates module ordering and related lists.
 *
 * @param dev Develop context.
 * @param module Module to move.
 * @param module_prev Module that should precede after move.
 * @return TRUE if move succeeded, FALSE otherwise.
 */
gboolean dt_ioppr_move_iop_after(struct dt_develop_t *dev, struct dt_iop_module_t *module, struct dt_iop_module_t *module_prev);

/**
 * @brief Debug helper to validate the current order for a develop context.
 *
 * Logs inconsistencies and optionally reports the state with @p msg.
 *
 * @param dev Develop context.
 * @param imgid Image id (for diagnostics).
 * @param msg Optional debug message.
 * @return 0 on success, non-zero if issues were detected.
 */
int dt_ioppr_check_iop_order(struct dt_develop_t *dev, const int32_t imgid, const char *msg);

#ifdef __cplusplus
}
#endif

#endif
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
