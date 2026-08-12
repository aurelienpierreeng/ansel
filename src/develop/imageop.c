/*
    This file is part of darktable,
    Copyright (C) 2009-2014 johannes hanika.
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2012 Henrik Andersson.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2011 Antony Dovgal.
    Copyright (C) 2011 Brian Teague.
    Copyright (C) 2011-2012 Jérémy Rosen.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2020 Tobias Ellinghaus.
    Copyright (C) 2011-2015, 2017, 2019 Ulrich Pegelow.
    Copyright (C) 2012-2014, 2019-2022 Aldric Renaudin.
    Copyright (C) 2012 Edouard Gomez.
    Copyright (C) 2012-2013, 2015, 2018, 2020 parafin.
    Copyright (C) 2012-2013, 2018-2022 Pascal Obry.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2014, 2017, 2021 Dan Torop.
    Copyright (C) 2014 Mikhail Trishchenkov.
    Copyright (C) 2014-2016 Pedro Côrte-Real.
    Copyright (C) 2017 Dominik Markiewicz.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2017-2020 Heiko Bauke.
    Copyright (C) 2017 Peter Budai.
    Copyright (C) 2018 Matthieu Moy.
    Copyright (C) 2018 rawfiner.
    Copyright (C) 2019-2026 Aurélien PIERRE.
    Copyright (C) 2019-2022 Diederik Ter Rahe.
    Copyright (C) 2019 Jacques Le Clerc.
    Copyright (C) 2020-2022 Chris Elston.
    Copyright (C) 2020-2022 Hanno Schwalm.
    Copyright (C) 2020 Harold le Clément de Saint-Marcq.
    Copyright (C) 2020-2021 Hubert Kowalski.
    Copyright (C) 2020-2021 Marco.
    Copyright (C) 2020 Philippe Weyland.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 Mark-64.
    Copyright (C) 2021-2022 Philipp Lutz.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Miloš Komarčević.
    Copyright (C) 2022 Nicolas Auffray.
    Copyright (C) 2023 Alynx Zhou.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2025-2026 Guillaume Stutin.
    Copyright (C) 2025 Miguel Moquillon.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "develop/imageop_gui.h"
#include "develop/masks_gui.h"
#include "darktable.h"
#include "widgets/widget_settings.h"
#include "common/conf.h"
#include "common/sentry.h"
#include "common/telemetry.h"
#include "develop/imageop.h"
#include "widgets/bauhaus.h"
#include "common/collection.h"
#include "database/collection_query.h"
#include "database/database.h"
#include "database/history_repository.h"
#include "database/preset_repository.h"
#include "metadata/exif.h"
#include "history/history.h"
#include "common/imagebuf.h"
#include "imageio/imageio_rawspeed.h"
#include "pixel/interpolation.h"
#include "common/module.h"
#include "common/opencl.h"
#include "common/usermanual_url.h"
#include "control/control.h"
#include "control/signal.h"
#include "develop/blend.h"
#include "develop/blend_gui.h"
#include "develop/develop.h"
#include "pixel/format.h"
#include "develop/masks.h"
#include "develop/tiling.h"
#include "widgets/gdkkeys.h"
#include "gui/presets.h"
#include "widgets/button.h"
#include "widgets/expander.h"

#include "gui/color_picker_proxy.h"
#include "gui/application.h"
#include "develop/gui_throttle.h"
#include "gui/presets.h"
#ifdef GDK_WINDOWING_QUARTZ
#endif

#include "common/hash.h"
#include "common/module_versioning.h"

#include <assert.h>
#include <gmodule.h>
#include <math.h>
#include <complex.h>
#include <stdlib.h>
#include "widgets/container.h"
#include "widgets/label.h"
#include "widgets/popup.h"
#include "widgets/widget_style.h"

#include <string.h>
#include <strings.h>
#include <time.h>
#include "widgets/togglebutton.h"


typedef struct dt_iop_gui_simple_callback_t
{
  dt_iop_module_t *self;
  int index;
} dt_iop_gui_simple_callback_t;


float dt_dev_get_module_scale(const dt_dev_pixelpipe_t *const pipe, const dt_iop_roi_t *const roi_in)
{
  return pipe->iscale / roi_in->scale;
}

uint32_t dt_dev_get_roi_filters(const dt_dev_pixelpipe_iop_t *const piece, const dt_iop_roi_t *const roi_in)
{
  return dt_rawspeed_crop_dcraw_filters(piece->dsc_in.filters, roi_in->x, roi_in->y);
}


void dt_iop_load_default_params(dt_iop_module_t *module)
{
  memcpy(module->params, module->default_params, module->params_size);
  dt_develop_blend_colorspace_t cst = dt_develop_blend_default_module_blend_colorspace(module);
  dt_develop_blend_init_blend_parameters(module->default_blendop_params, cst);
  dt_iop_commit_blend_params(module, module->default_blendop_params);
  dt_iop_gui_blending_reload_defaults(module);
  dt_iop_compute_module_hash(module, module->dev->forms);
}

static void _iop_modify_roi_in(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                               struct dt_dev_pixelpipe_iop_t *piece,
                               const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  *roi_in = *roi_out;
}

static void _iop_modify_roi_out(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                                struct dt_dev_pixelpipe_iop_t *piece,
                                dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

/* default group for modules which do not implement the default_group() function */
static int default_default_group(void)
{
  return IOP_GROUP_TECHNICAL;
}

/* default flags for modules which does not implement the flags() function */
static int default_flags(void)
{
  return 0;
}

/* default operation tags for modules which does not implement the flags() function */
static int default_operation_tags(void)
{
  return 0;
}

/* default operation tags filter for modules which does not implement the flags() function */
static int default_operation_tags_filter(void)
{
  return 0;
}

static const char **default_description(struct dt_iop_module_t *self)
{
  return NULL;
}

static const char *default_aliases(void)
{
  return "";
}

static const char *default_deprecated_msg(void)
{
  return NULL;
}

static gboolean default_has_defaults(struct dt_iop_module_t *self)
{
  return memcmp(self->params, self->default_params, self->params_size) == 0;
}

static gboolean default_runtime_data_hash(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                          const dt_dev_pixelpipe_iop_t *piece)
{
  (void)self;
  (void)pipe;
  (void)piece;
  return FALSE;
}

static void default_commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params,
                                   dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, self->params_size);
}

// WARNING: this works only if the data struct has the same size
// as the param structure. You need to implement your own IOP module
// method if they don't match !!!
static void default_init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                              dt_dev_pixelpipe_iop_t *piece)
{
  size_t data_size = (size_t)self->params_size;
  piece->data = dt_calloc_align(data_size);
  piece->data_size = data_size;
}

static void default_cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                                 dt_dev_pixelpipe_iop_t *piece)
{
  dt_free_align(piece->data);
  piece->data = NULL;
}

static void default_gui_cleanup(dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

static void default_cleanup(dt_iop_module_t *module)
{
  dt_free(module->params);
  dt_free(module->default_params);
}


static int default_distort_transform(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                     const dt_dev_pixelpipe_iop_t *piece, float *points,
                                     size_t points_count)
{
  (void)self;
  (void)pipe;
  (void)piece;
  (void)points;
  (void)points_count;
  return 1;
}
static int default_distort_backtransform(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                         const dt_dev_pixelpipe_iop_t *piece, float *points,
                                         size_t points_count)
{
  (void)self;
  (void)pipe;
  (void)piece;
  (void)points;
  (void)points_count;
  return 1;
}

static int default_process(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                           const struct dt_dev_pixelpipe_iop_t *piece,
                           const void *const i, void *const o)
{
  const struct dt_iop_roi_t *const roi_in = &piece->roi_in;
  const struct dt_iop_roi_t *const roi_out = &piece->roi_out;
  if(roi_in->width <= 1 || roi_in->height <= 1 || roi_out->width <= 1 || roi_out->height <= 1) return 0;
  return self->process_plain(self, pipe, piece, i, o);
}

static dt_introspection_field_t *default_get_introspection_linear(void)
{
  return NULL;
}
static dt_introspection_t *default_get_introspection(void)
{
  return NULL;
}
static void *default_get_p(const void *param, const char *name)
{
  return NULL;
}
static dt_introspection_field_t *default_get_f(const char *name)
{
  return NULL;
}

void dt_iop_default_init(dt_iop_module_t *module)
{
  size_t param_size = module->so->get_introspection()->size;
  module->params_size = param_size;
  /* Keep ownership explicit: if init is re-entered on the same instance,
   * release previous params before rebuilding defaults from introspection. */
  if(!IS_NULL_PTR(module->params))
  {
    dt_free(module->params);
    module->params = NULL;
  }
  if(!IS_NULL_PTR(module->default_params))
  {
    dt_free(module->default_params);
    module->default_params = NULL;
  }
  module->params = (dt_iop_params_t *)calloc(1, param_size);
  module->default_params = (dt_iop_params_t *)calloc(1, param_size);

  module->default_enabled = 0;

  dt_introspection_field_t *i = module->so->get_introspection_linear();
  while(i->header.type != DT_INTROSPECTION_TYPE_NONE)
  {
    switch(i->header.type)
    {
    case DT_INTROSPECTION_TYPE_FLOAT:
      *(float*)((uint8_t *)module->default_params + i->header.offset) = i->Float.Default;
      break;
    case DT_INTROSPECTION_TYPE_INT:
      *(int*)((uint8_t *)module->default_params + i->header.offset) = i->Int.Default;
      break;
    case DT_INTROSPECTION_TYPE_UINT:
      *(unsigned int*)((uint8_t *)module->default_params + i->header.offset) = i->UInt.Default;
      break;
    case DT_INTROSPECTION_TYPE_USHORT:
      *(unsigned short*)((uint8_t *)module->default_params + i->header.offset) = i->UShort.Default;
      break;
    case DT_INTROSPECTION_TYPE_ENUM:
      *(int*)((uint8_t *)module->default_params + i->header.offset) = i->Enum.Default;
      break;
    case DT_INTROSPECTION_TYPE_BOOL:
      *(gboolean*)((uint8_t *)module->default_params + i->header.offset) = i->Bool.Default;
      break;
    case DT_INTROSPECTION_TYPE_CHAR:
      *(char*)((uint8_t *)module->default_params + i->header.offset) = i->Char.Default;
      break;
    case DT_INTROSPECTION_TYPE_OPAQUE:
      memset((uint8_t *)module->default_params + i->header.offset, 0, i->header.size);
      break;
    case DT_INTROSPECTION_TYPE_ARRAY:
      {
        if(i->Array.type == DT_INTROSPECTION_TYPE_CHAR) break;

        size_t element_size = i->Array.field->header.size;
        if(element_size % sizeof(int))
        {
          int8_t *p = (int8_t *)module->default_params + i->header.offset;
          for (size_t c = element_size; c < i->header.size; c++, p++)
            p[element_size] = *p;
        }
        else
        {
          element_size /= sizeof(int);
          size_t num_ints = i->header.size / sizeof(int);

          int *p = (int *)((uint8_t *)module->default_params + i->header.offset);
          for (size_t c = element_size; c < num_ints; c++, p++)
            p[element_size] = *p;
        }
      }
      break;
    case DT_INTROSPECTION_TYPE_STRUCT:
      // ignore STRUCT; nothing to do
      break;
    default:
      fprintf(stderr, "unsupported introspection type \"%s\" encountered in dt_iop_default_init (field %s)\n", i->header.type_name, i->header.field_name);
      break;
    }

    i++;
  }
}


/* The bauhaus widgets ask a module to make itself visible before grabbing focus inside it.
 * For an IOP that means expanding a collapsed expander, and dropping stale focus first --
 * both develop/ concerns, which is why this lives here and not in the widget. */
static void _iop_ensure_visible(dt_gui_module_t *m)
{
  dt_iop_module_t *module = (dt_iop_module_t *)m;
  if(IS_NULL_PTR(module)) return;

  if(module->gui && !IS_NULL_PTR(module->gui->expander))
  {
    g_object_set_data(G_OBJECT(module->gui->expander), "dt-modulegroups-switch-from-active-once",
                      GINT_TO_POINTER(TRUE));
    dt_iop_gui_set_expanded(module, TRUE, TRUE);
  }

  // If the module is already marked focused, modulegroups may not re-emit its focus signal
  // and tab visibility can stay stale. Drop focus once so the next request replays the
  // full focus/update sequence.
  if(!IS_NULL_PTR(dt_dev_get_global()) && dt_dev_get_global()->gui_module == module)
    dt_iop_request_focus(NULL);
}

int default_iop_focus(dt_gui_module_t *m, gboolean toggle)
{
  dt_iop_module_t *module = (dt_iop_module_t *) m;

  // Expand and scroll
  if(module->dev->gui_module != module)
  {
    dt_iop_request_focus(module);
    dt_iop_gui_set_expanded(module, TRUE, TRUE);
  }
  else if(toggle)
  {
    module->dev->gui_module = NULL;
    dt_iop_gui_set_expanded(module, FALSE, TRUE);
    dt_gui_refocus_center();
  }

  return 1;
}

int dt_iop_load_module_so(void *m, const char *libname, const char *module_name)
{
  dt_iop_module_so_t *module = (dt_iop_module_so_t *)m;
  g_strlcpy(module->op, module_name, sizeof(module->op));

#define INCLUDE_API_FROM_MODULE_LOAD "iop_load_module"
#include "iop/iop_api.h"

  if(IS_NULL_PTR(module->init)) module->init = dt_iop_default_init;
  if(IS_NULL_PTR(module->modify_roi_in)) module->modify_roi_in = _iop_modify_roi_in;
  if(IS_NULL_PTR(module->modify_roi_out)) module->modify_roi_out = _iop_modify_roi_out;

  #ifdef HAVE_OPENCL
  if(IS_NULL_PTR(module->process_tiling_cl)) module->process_tiling_cl = dt_opencl_is_inited() ? default_process_tiling_cl : NULL;
  if(!dt_opencl_is_inited()) module->process_cl = NULL;
  #endif // HAVE_OPENCL

  module->process_plain = module->process;
  module->process = default_process;

  module->data = NULL;

  // the introspection api
  module->have_introspection = FALSE;
  if(module->introspection_init)
  {
    if(!module->introspection_init(module, DT_INTROSPECTION_VERSION))
    {
      // set the introspection related fields in module
      module->have_introspection = TRUE;

      if(module->get_p == default_get_p ||
         module->get_f == default_get_f ||
         module->get_introspection_linear == default_get_introspection_linear ||
         module->get_introspection == default_get_introspection)
        goto api_h_error;
    }
    else
      fprintf(stderr, "[iop_load_module] failed to initialize introspection for operation `%s'\n", module_name);
  }

  if(module->init_global) module->init_global(module);
  return 0;
}

/* The old inline widget members were zeroed here at load; the gui struct is calloc'd by
 * dt_iop_gui_init() when (and only when) a GUI attaches, so a headless load must not
 * touch module->gui at all -- it is NULL by design. */
int dt_iop_load_module_by_so(dt_iop_module_t *module, dt_iop_module_so_t *so, dt_develop_t *dev)
{
  module->dev = dev;
  module->hide_enable_button = 0;
  module->request_color_pick = DT_REQUEST_COLORPICK_OFF;
  module->request_histogram = DT_REQUEST_ONLY_IN_GUI;
  module->histogram_stats.bins_count = 0;
  module->histogram_stats.pixels = 0;
  module->multi_priority = 0;
  module->iop_order = 0;
  for(int k = 0; k < 3; k++)
  {
    module->picked_color[k] = module->picked_output_color[k] = 0.0f;
    module->picked_color_min[k] = module->picked_output_color_min[k] = 666.0f;
    module->picked_color_max[k] = module->picked_output_color_max[k] = -666.0f;
  }
  module->histogram_cst = IOP_CS_NONE;
  module->histogram = NULL;
  module->histogram_max[0] = module->histogram_max[1] = module->histogram_max[2] = module->histogram_max[3]
      = 0;
  module->histogram_middle_grey = FALSE;
  module->request_mask_display = DT_DEV_PIXELPIPE_DISPLAY_NONE;
  module->bypass_cache = FALSE;
  module->bypass_cache_variant = 0;
  module->enabled = module->default_enabled = module->workflow_enabled = 0; // all modules disabled by default.
  g_strlcpy(module->op, so->op, 20);
  module->raster_mask.source.users = g_hash_table_new(NULL, NULL);
  module->raster_mask.source.masks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, dt_free_gpointer);
  module->raster_mask.sink.source = NULL;
  module->raster_mask.sink.id = 0;

  // only reference cached results of dlopen:
  module->module = so->module;
  module->so = so;

#define INCLUDE_API_FROM_MODULE_LOAD_BY_SO
#include "iop/iop_api.h"

  module->version = so->version;
  module->process_plain = so->process_plain;
  module->have_introspection = so->have_introspection;


  module->global_data = so->data;

  // now init the instance:
  module->init(module);
  module->hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  module->blendop_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;

  if(module->params_size == 0)
  {
    fprintf(stderr, "[iop_load_module] `%s' needs to have a params size > 0!\n", so->op);
    return 1; // empty params hurt us in many places, just add a dummy value
  }

  /* Allocate params only when module init did not allocate them already.
   * Some init paths (notably default_init) already own the params buffers. */
  if(IS_NULL_PTR(module->params))
    module->params = calloc(1, module->params_size);
  if(IS_NULL_PTR(module->default_params))
    module->default_params = calloc(1, module->params_size);
  module->blend_params = calloc(1, sizeof(dt_develop_blend_params_t));
  module->default_blendop_params = calloc(1, sizeof(dt_develop_blend_params_t));

  // Don't init defaults here, it's done when reading/initing history

  /* pass on the dt_gui_module_t args for bauhaus widgets
   * only when a GUI lifetime exists for this module instance. */
  if(IS_NULL_PTR(module->dev) || module->dev->gui_attached)
  {
    module->common_fields.name = delete_underscore(module->name());
    module->common_fields.view = g_strdup(_("Darkroom")); // IOP modules belong necessarily to darkroom
  }
  else
  {
    module->common_fields.name = NULL;
    module->common_fields.view = NULL;
  }
  module->common_fields.widget_list = NULL;
  module->common_fields.widget_list_bh = NULL;
  module->common_fields.focus = module->iop_focus;
  module->common_fields.ensure_visible = _iop_ensure_visible;
  module->common_fields.deprecated = (module->flags() & IOP_FLAGS_DEPRECATED) == IOP_FLAGS_DEPRECATED;

  return 0;
}

void dt_iop_init_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                      struct dt_dev_pixelpipe_iop_t *piece)
{
  module->init_pipe(module, pipe, piece);
  piece->blendop_data = dt_calloc_align(sizeof(dt_develop_blend_params_t));
}

/**
 * @brief Release module-owned resources for one pixelpipe node.
 *
 * @details The instance callback is an immutable copy of the callback stored in
 * the loaded shared-object descriptor. Pixelpipes can retain removed module
 * instances until their next topology rebuild, so validate both the descriptor
 * lifetime and that callback identity before calling through the instance.
 * A mismatch means the instance storage is no longer trustworthy; calling the
 * descriptor callback with that same instance would only move the use-after-free
 * into the module cleanup code.
 */
void dt_iop_cleanup_pipe(struct dt_iop_module_t *module, struct dt_dev_pixelpipe_t *pipe,
                        struct dt_dev_pixelpipe_iop_t *piece)
{
  if(IS_NULL_PTR(piece)) return;

  if(IS_NULL_PTR(module))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_iop_cleanup_pipe] missing module, skipping module pipe cleanup\n");
  }
  else if(IS_NULL_PTR(pipe))
  {
    dt_print(DT_DEBUG_ALWAYS,
             "[dt_iop_cleanup_pipe] missing pipe for `%s`, skipping module pipe cleanup\n",
             module->op);
  }
  else
  {
    dt_iop_module_so_t *module_so = module->so;
    gboolean module_so_loaded = FALSE;

    /* Search the process-owned descriptor list by address before reading the
     * candidate. A removed instance may outlive its GUI, but its descriptor
     * must remain one of the objects loaded at startup. */
    for(GList *iop = g_list_first(darktable.iop); iop; iop = g_list_next(iop))
    {
      if(iop->data == module_so)
      {
        module_so_loaded = TRUE;
        break;
      }
    }

    if(!module_so_loaded)
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_iop_cleanup_pipe] invalid shared-object descriptor %p, skipping module pipe cleanup\n",
               (void *)module_so);
    else if(module->cleanup_pipe != module_so->cleanup_pipe)
      dt_print(DT_DEBUG_ALWAYS,
               "[dt_iop_cleanup_pipe] stale cleanup callback for `%s`, skipping module pipe cleanup\n",
               module_so->op);
    else
      module_so->cleanup_pipe(module, pipe, piece);
  }

  if(!IS_NULL_PTR(piece->blendop_data))
  {
    dt_free_align(piece->blendop_data);
    piece->blendop_data = NULL;
  }
}

gboolean dt_iop_gui_commit_iop_order_change(dt_develop_t *dev, dt_iop_module_t *module,
                                            gboolean enable, gboolean write_history, const char *reason)
{
  if(IS_NULL_PTR(dev)) return FALSE;

  dt_dev_pixelpipe_rebuild_all(dev);
  if(write_history) dt_dev_add_history_item(dev, module, enable, TRUE);

  if(!IS_NULL_PTR(reason)) dt_ioppr_check_iop_order(dev, 0, reason);

  dt_dev_signal_modules_moved(dev);
  return TRUE;
}

gboolean dt_iop_gui_move_module_before(dt_iop_module_t *module, dt_iop_module_t *module_next,
                                       const char *reason)
{
  if(!dt_ioppr_move_iop_before(module->dev, module, module_next)) return FALSE;
  return dt_iop_gui_commit_iop_order_change(module->dev, module, TRUE, TRUE, reason);
}

gboolean dt_iop_gui_move_module_after(dt_iop_module_t *module, dt_iop_module_t *module_prev,
                                      const char *reason)
{
  if(!dt_ioppr_move_iop_after(module->dev, module, module_prev)) return FALSE;
  return dt_iop_gui_commit_iop_order_change(module->dev, module, TRUE, TRUE, reason);
}

gboolean dt_iop_so_is_hidden(dt_iop_module_so_t *module)
{
  gboolean is_hidden = TRUE;
  if(!(module->flags() & IOP_FLAGS_HIDDEN))
  {
    if(IS_NULL_PTR(module->gui_init))
      g_debug("Module '%s' is not hidden and lacks implementation of gui_init()...", module->op);
    else if(!module->gui_cleanup)
      g_debug("Module '%s' is not hidden and lacks implementation of gui_cleanup()...", module->op);
    else
      is_hidden = FALSE;
  }
  return is_hidden;
}

gboolean dt_iop_is_hidden(dt_iop_module_t *module)
{
  return dt_iop_so_is_hidden(module->so);
}

void dt_iop_reload_defaults(dt_iop_module_t *module)
{
  // Suppress GUI callbacks while a module's reload_defaults() rewrites widget defaults. This is
  // a no-op off the GUI thread (worker-thread thumbnail/export devs have no widgets, so they
  // must never touch -- and race/drift -- the shared depth); the central API enforces that.
  // The previous root cause of NULL-bauhaus-widget crashes was exactly this counter drifting
  // (Sentry #129494618, #129578628, #129908540). Scope the freeze to the params work so the
  // header update below still runs unsuppressed, exactly as before.
  {
    dt_gui_widget_freeze();

    if(module->reload_defaults)
    {
      // report if reload_defaults was called unnecessarily => this should be considered a bug
      // the whole point of reload_defaults is to update defaults _based on current image_
      // any required initialisation should go in init (and not be performed repeatedly here)
      if(module->dev)
      {
        module->reload_defaults(module);
        dt_print(DT_DEBUG_PARAMS, "[params] defaults reloaded for %s\n", module->op);
      }
      else
      {
        fprintf(stderr, "reload_defaults should not be called without image.\n");
      }
    }
    dt_iop_load_default_params(module);
  }

  if(module->gui && module->gui->header) dt_iop_gui_update_header(module);
}


static void _init_presets(dt_iop_module_so_t *module_so)
{
  // Skip auto-preset regeneration when the build + UI language haven't changed.
  if(module_so->init_presets && dt_gui_presets_autogen_enabled())
    module_so->init_presets(module_so);

  // this seems like a reasonable place to check for and update legacy
  // presets.

  const int32_t module_version = module_so->version();

  // The rows are read out in full first: the loop below writes back to data.presets, and it
  // used to do so while still stepping a cursor over that same table.
  GList *presets = dt_preset_repository_list_for_upgrade(module_so->op);
  for(GList *l = presets; l; l = g_list_next(l))
  {
    const dt_module_preset_t *preset = (const dt_module_preset_t *)l->data;
    const char *name = preset->name;
    int32_t old_params_version = preset->op_version;
    const void *old_params = preset->op_params;
    const int32_t old_params_size = preset->op_params_size;
    const int32_t old_blend_params_version = preset->blendop_version;
    const void *old_blend_params = preset->blendop_params;
    const int32_t old_blend_params_size = preset->blendop_params_size;

    if(old_params_version == 0)
    {
      // this preset doesn't have a version.  go digging through the database
      // to find a history entry that matches the preset params, and get
      // the module version from that.

      old_params_version = dt_history_repository_find_version_for_params(module_so->op, old_params,
                                                                         old_params_size);
      if(old_params_version == 0)
      {
        fprintf(stderr, "[imageop_init_presets] WARNING: Could not find versioning information for '%s' "
                        "preset '%s'\nUntil some is found, the preset will be unavailable.\n(To make it "
                        "return, please load an image that uses the preset.)\n",
                module_so->op, name);
        continue;
      }

      // we found an old params version.  Update the database with it.

      fprintf(stderr, "[imageop_init_presets] Found version %d for '%s' preset '%s'\n", old_params_version,
              module_so->op, name);

      // ONLY the version: this preset's blob is already right, and a setter that also wrote
      // op_params would overwrite every same-named preset at every other version with it.
      dt_preset_repository_set_module_version(module_so->op, name, old_params_version);
    }

    if(module_version > old_params_version && !IS_NULL_PTR(module_so->legacy_params))
    {
      // we need a dt_iop_module_t for legacy_params()
      dt_iop_module_t *module;
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module_by_so(module, module_so, NULL))
      {
        dt_free(module);
        continue;
      }
/*
      module->init(module);
      if(module->params_size == 0)
      {
        dt_iop_cleanup_module(module);
        dt_free(module);
        continue;
      }
      // we call reload_defaults() in case the module defines it
      if(module->reload_defaults) module->reload_defaults(module); // why not call dt_iop_reload_defaults? (if needed at all)
*/

      const int32_t new_params_size = module->params_size;
      void *new_params = calloc(1, new_params_size);

      // convert the old params to new
      if(module->legacy_params(module, old_params, old_params_version, new_params, module_version))
      {
        dt_free(new_params);
        dt_iop_cleanup_module(module);
        dt_free(module);
        continue;
      }

      fprintf(stderr, "[imageop_init_presets] updating '%s' preset '%s' from version %d to version %d\nto:'%s'",
              module_so->op, name, old_params_version, module_version,
              dt_exif_xmp_encode(new_params, new_params_size, NULL));

      // and write the new params back to the database
      dt_preset_repository_update_module_params(module->op, name, module->version(), new_params,
                                                new_params_size);

      dt_free(new_params);
      dt_iop_cleanup_module(module);
      dt_free(module);
    }
    else if(module_version > old_params_version)
    {
      fprintf(stderr, "[imageop_init_presets] Can't upgrade '%s' preset '%s' from version %d to %d, no "
                      "legacy_params() implemented \n",
              module_so->op, name, old_params_version, module_version);
    }

    if(IS_NULL_PTR(old_blend_params) || dt_develop_blend_version() > old_blend_params_version)
    {
      fprintf(stderr,
              "[imageop_init_presets] updating '%s' preset '%s' from blendop version %d to version %d\n",
              module_so->op, name, old_blend_params_version, dt_develop_blend_version());

      // we need a dt_iop_module_t for dt_develop_blend_legacy_params()
      // using dt_develop_blend_legacy_params_by_so won't help as we need "module" anyway
      dt_iop_module_t *module;
      module = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));
      if(dt_iop_load_module_by_so(module, module_so, NULL))
      {
        dt_free(module);
        continue;
      }

      if(module->params_size == 0)
      {
        dt_iop_cleanup_module(module);
        dt_free(module);
        continue;
      }
      void *new_blend_params = malloc(sizeof(dt_develop_blend_params_t));

      // convert the old blend params to new
      if(old_blend_params
         && dt_develop_blend_legacy_params(module, old_blend_params, old_blend_params_version,
                                           new_blend_params, dt_develop_blend_version(),
                                           old_blend_params_size) == 0)
      {
        // do nothing
      }
      else
      {
        memcpy(new_blend_params, module->default_blendop_params, sizeof(dt_develop_blend_params_t));
      }

      // and write the new blend params back to the database
      dt_preset_repository_set_blend_params(module->op, name, dt_develop_blend_version(),
                                            new_blend_params, sizeof(dt_develop_blend_params_t));

      dt_free(new_blend_params);
      dt_iop_cleanup_module(module);
      dt_free(module);
    }
  }
  g_list_free_full(presets, dt_module_preset_free);
}


static void _init_module_so(void *m)
{
  dt_iop_module_so_t *module = (dt_iop_module_so_t *)m;

  _init_presets(module);

  // do not init accelerators if there is no gui
  if(dt_gui_get_global())
  {
    // create a gui and have the widgets register their accelerators
    dt_iop_module_t *module_instance = (dt_iop_module_t *)calloc(1, sizeof(dt_iop_module_t));

    if(module->gui_init && !dt_iop_load_module_by_so(module_instance, module, NULL))
    {
      dt_iop_gui_init(module_instance);

      static gboolean blending_accels_initialized = FALSE;
      if(!blending_accels_initialized)
      {
        dt_iop_colorspace_type_t cst = module->blend_colorspace(module_instance, NULL, NULL);

        if((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) &&
           !(module->flags() & IOP_FLAGS_NO_MASKS) &&
           (cst == IOP_CS_LAB || dt_iop_colorspace_is_rgb(cst)))
        {
          dt_iop_gui_init_blending(module_instance);
          dt_iop_gui_cleanup_blending(module_instance);

          blending_accels_initialized = TRUE;
        }
      }

      dt_iop_gui_cleanup_module(module_instance);

      dt_iop_cleanup_module(module_instance);

    }

    dt_free(module_instance);
  }
}

void dt_iop_load_modules_so(void)
{
  // Batch presets initialization in a single transaction to avoid per-module BEGIN/COMMIT overhead.
  dt_database_begin_transaction_batch();
  darktable.iop = dt_module_load_modules("/plugins", sizeof(dt_iop_module_so_t), dt_iop_load_module_so,
                                         _init_module_so, NULL);
  dt_database_end_transaction_batch();
}

int dt_iop_load_module(dt_iop_module_t *module, dt_iop_module_so_t *module_so, dt_develop_t *dev)
{
  memset(module, 0, sizeof(dt_iop_module_t));
  if(dt_iop_load_module_by_so(module, module_so, dev))
  {
    dt_free(module);
    return 1;
  }
  return 0;
}

void dt_iop_cleanup_module(dt_iop_module_t *module)
{
  // Safety net: dt_iop_cleanup_module() is the one choke point every module teardown path (darkroom
  // leave(), studio_capture's viewer teardown, dev->alliop reclaim...) calls right before dt_free(module).
  // Some of those paths skip dt_iop_gui_cleanup_module() entirely (e.g. studio_capture never runs
  // gui_init on its modules), and dt_iop_gui_cleanup_module() itself only invokes the module's own
  // gui_cleanup() -- which does the DT_DEBUG_CONTROL_SIGNAL_DISCONNECT() calls -- when gui_data is
  // non-NULL. Any signal a module connected in gui_init() (module_moved_callback in lut3d.c,
  // _develop_ui_pipe_started_callback in toneequal.c...) is broadcast globally on the signal bus,
  // so a single missed disconnect leaves a dangling self pointer that a later, unrelated dev's signal
  // raise will invoke -- SIGSEGV on the freed instance. Disconnecting everything keyed on this module's
  // address here, unconditionally, guarantees no module can be invoked again once freed.
  dt_control_signal_disconnect_all(dt_control_signal_get_global(), module);

  module->cleanup(module);

  if(!IS_NULL_PTR(module->common_fields.name))
  {
    dt_free(module->common_fields.name);
    module->common_fields.name = NULL;
  }
  if(!IS_NULL_PTR(module->common_fields.view))
  {
    dt_free(module->common_fields.view);
    module->common_fields.view = NULL;
  }

  dt_free(module->blend_params);
  dt_free(module->default_blendop_params);

  // don't have a picker pointing to a disappeared module
  dt_develop_t *const dev = dt_dev_get_global();
  if(dev
     && dev->color_picker.picker
     && dev->color_picker.picker->module == module)
  {
    dev->color_picker.picker = NULL;
    dev->color_picker.widget = NULL;
    dev->color_picker.module = NULL;
    dev->color_picker.enabled = FALSE;
    dev->color_picker.update_pending = FALSE;
  }

  dt_free(module->histogram);
  g_hash_table_destroy(module->raster_mask.source.users);
  g_hash_table_destroy(module->raster_mask.source.masks);
  module->raster_mask.source.users = NULL;
  module->raster_mask.source.masks = NULL;
}

void dt_iop_unload_modules_so()
{
  while(darktable.iop)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)darktable.iop->data;
    if(module->cleanup_global) module->cleanup_global(module);
    if(module->module) g_module_close(module->module);
    dt_free(darktable.iop->data);
    darktable.iop = g_list_delete_link(darktable.iop, darktable.iop);
  }
}

void dt_iop_set_mask_mode(dt_iop_module_t *module, int mask_mode)
{
  (void)mask_mode;
  static const int key = 0;

  gboolean drawn_used = FALSE;
  gboolean parametric_used = FALSE;
  dt_develop_blend_get_mask_usage(module, module->blend_params, NULL, NULL, &drawn_used, &parametric_used);

  if(drawn_used || parametric_used)
  {
    char *modulename = dt_history_item_get_name(module);
    g_hash_table_insert(module->raster_mask.source.masks, GINT_TO_POINTER(key), modulename);
  }
  else
  {
    g_hash_table_remove(module->raster_mask.source.masks, GINT_TO_POINTER(key));
  }
}

gboolean dt_iop_module_has_raster_mask(const dt_iop_module_t *module)
{
  if(IS_NULL_PTR(module)) return FALSE;

  const gboolean mask_mode_raster = module->blend_params
                                    && ((module->blend_params->mask_mode & DEVELOP_MASK_RASTER) == DEVELOP_MASK_RASTER);
  const gboolean has_raster_sink = (!IS_NULL_PTR(module->raster_mask.sink.source));

  return mask_mode_raster || has_raster_sink;
}

gboolean dt_iop_module_needs_mask_history_ext(const dt_iop_module_t *module, gboolean *raster, gboolean *drawn, gboolean *parametric)
{
  gboolean raster_used = FALSE;
  gboolean drawn_used = FALSE;
  gboolean parametric_used = FALSE;
  if(IS_NULL_PTR(module)) return FALSE;

  const gboolean supports_blending
      = ((module->flags() & IOP_FLAGS_SUPPORTS_BLENDING) == IOP_FLAGS_SUPPORTS_BLENDING);
  const gboolean internal_masks = ((module->flags() & IOP_FLAGS_INTERNAL_MASKS) == IOP_FLAGS_INTERNAL_MASKS);

  if(!supports_blending) return internal_masks;
  if(IS_NULL_PTR(module->blend_params)) return internal_masks;

  dt_develop_blend_get_mask_usage(module, module->blend_params, NULL, &raster_used, &drawn_used, &parametric_used);

  if(!IS_NULL_PTR(raster)) *raster = raster_used;
  if(!IS_NULL_PTR(drawn)) *drawn = drawn_used;
  if(!IS_NULL_PTR(parametric)) *parametric = parametric_used;

  return raster_used || drawn_used || parametric_used || internal_masks;
}

gboolean dt_iop_module_needs_mask_history(const dt_iop_module_t *module)
{
  return dt_iop_module_needs_mask_history_ext(module, NULL, NULL, NULL);
}

// make sure that blend_params are in sync with the iop struct
void dt_iop_commit_blend_params(dt_iop_module_t *module, const dt_develop_blend_params_t *blendop_params)
{
  if(module->raster_mask.sink.source)
    g_hash_table_remove(module->raster_mask.sink.source->raster_mask.source.users, module);

  if(module->blend_params != blendop_params)
    memcpy(module->blend_params, blendop_params, sizeof(dt_develop_blend_params_t));

  if(blendop_params->blend_cst == DEVELOP_BLEND_CS_NONE)
  {
    module->blend_params->blend_cst = dt_develop_blend_default_module_blend_colorspace(module);
  }
  dt_iop_set_mask_mode(module, blendop_params->mask_mode);

  // This assumes that the module providing raster mask to the current one is ALWAYS
  // MANDATORILY before the current one BOTH in history order AND in pipe order,
  // because the current function is run in history order when we load/reload/pop history
  if(module->dev)
    for(GList *iter = g_list_first(module->dev->iop); iter; iter = g_list_next(iter))
    {
      dt_iop_module_t *m = (dt_iop_module_t *)iter->data;
      if(!strcmp(m->op, blendop_params->raster_mask_source))
      {
        if(m->multi_priority == blendop_params->raster_mask_instance)
        {
          g_hash_table_insert(m->raster_mask.source.users, module, GINT_TO_POINTER(blendop_params->raster_mask_id));
          dt_print(DT_DEBUG_MASKS, "[raster masks] Committing raster mask from %s (%s) into %s (%s)\n", m->op, m->multi_name, module->op,
                  module->multi_name);
          module->raster_mask.sink.source = m;
          module->raster_mask.sink.id = blendop_params->raster_mask_id;
          return;
        }
      }
    }
  // else if no module->dev, it means we are only loading module's .so

  module->raster_mask.sink.source = NULL;
  module->raster_mask.sink.id = 0;
}

gboolean _iop_validate_params(dt_introspection_field_t *field, gpointer params, gboolean report)
{
  dt_iop_params_t *p = (dt_iop_params_t *)((uint8_t *)params + field->header.offset);

  gboolean all_ok = TRUE;

  switch(field->header.type)
  {
  case DT_INTROSPECTION_TYPE_STRUCT:
    for(int i = 0; i < field->Struct.entries; i++)
    {
      dt_introspection_field_t *entry = field->Struct.fields[i];

      all_ok &= _iop_validate_params(entry, params, report);
    }
    break;
  case DT_INTROSPECTION_TYPE_UNION:
    all_ok = FALSE;
    for(int i = field->Union.entries - 1; i >= 0 ; i--)
    {
      dt_introspection_field_t *entry = field->Union.fields[i];

      if(_iop_validate_params(entry, params, report && i == 0))
      {
        all_ok = TRUE;
        break;
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_ARRAY:
    if(field->Array.type == DT_INTROSPECTION_TYPE_CHAR)
    {
      if(!memchr(p, '\0', field->Array.count))
      {
        if(report)
          fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\"; string not null terminated.\n",
                          field->header.type_name);
        all_ok = FALSE;
      }
    }
    else
    {
      for(int i = 0, item_offset = 0; i < field->Array.count; i++, item_offset += field->Array.field->header.size)
      {
        if(!_iop_validate_params(field->Array.field, (uint8_t *)params + item_offset, report))
        {
          if(report)
            fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\", for array element \"%d\"\n",
                            field->header.type_name, i);
          all_ok = FALSE;
          break;
        }
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_FLOAT:
    all_ok = isnan(*(float*)p) || ((*(float*)p >= field->Float.Min && *(float*)p <= field->Float.Max));
    break;
  case DT_INTROSPECTION_TYPE_INT:
    all_ok = (*(int*)p >= field->Int.Min && *(int*)p <= field->Int.Max);
    break;
  case DT_INTROSPECTION_TYPE_UINT:
    all_ok = (*(unsigned int*)p >= field->UInt.Min && *(unsigned int*)p <= field->UInt.Max);
    break;
  case DT_INTROSPECTION_TYPE_USHORT:
    all_ok = (*(unsigned short int*)p >= field->UShort.Min && *(unsigned short int*)p <= field->UShort.Max);
    break;
  case DT_INTROSPECTION_TYPE_INT8:
    all_ok = (*(uint8_t*)p >= field->Int8.Min && *(uint8_t*)p <= field->Int8.Max);
    break;
  case DT_INTROSPECTION_TYPE_CHAR:
    all_ok = (*(char*)p >= field->Char.Min && *(char*)p <= field->Char.Max);
    break;
  case DT_INTROSPECTION_TYPE_FLOATCOMPLEX:
    all_ok = creal(*(float complex*)p) >= creal(field->FloatComplex.Min) &&
             creal(*(float complex*)p) <= creal(field->FloatComplex.Max) &&
             cimag(*(float complex*)p) >= cimag(field->FloatComplex.Min) &&
             cimag(*(float complex*)p) <= cimag(field->FloatComplex.Max);
    break;
  case DT_INTROSPECTION_TYPE_ENUM:
    all_ok = FALSE;
    for(dt_introspection_type_enum_tuple_t *i = field->Enum.values; i && i->name; i++)
    {
      if(i->value == *(int*)p)
      {
        all_ok = TRUE;
        break;
      }
    }
    break;
  case DT_INTROSPECTION_TYPE_BOOL:
    // *(gboolean*)p
    break;
  case DT_INTROSPECTION_TYPE_OPAQUE:
    // TODO: special case float2
    break;
  default:
    fprintf(stderr, "unsupported introspection type \"%s\" encountered in _iop_validate_params (field %s)\n",
                    field->header.type_name, field->header.name);
    all_ok = FALSE;
    break;
  }

  if(!all_ok && report)
    fprintf(stderr, "validation check failed in _iop_validate_params for type \"%s\"%s%s\n",
                    field->header.type_name, (*field->header.name ? ", field: " : ""), field->header.name);

  return all_ok;
}


gboolean dt_iop_check_modules_equal(dt_iop_module_t *mod_1, dt_iop_module_t *mod_2)
{
  // Use module fingerprints to determine if two instances are actually the same
  return mod_1 == mod_2
          && mod_1->instance == mod_2->instance
          && mod_1->multi_priority == mod_2->multi_priority
          && mod_1->iop_order == mod_2->iop_order;
}


void _hash_raster_masks(gpointer key, gpointer value, uint64_t *hash)
{
  dt_iop_module_t *module = (dt_iop_module_t *)key;

  // Use only "constant" module params with regard to the pipeline
  // init/resync aka we can't use any module pre-computed hash.
  *hash = dt_hash(*hash, (char *)module->op, sizeof(module->op));
  *hash = dt_hash(*hash, (char *)&module->iop_order, sizeof(module->iop_order));
  *hash = dt_hash(*hash, (char *)&module->instance, sizeof(module->instance));
  *hash = dt_hash(*hash, (char *)&module->multi_priority, sizeof(module->multi_priority));
  *hash = dt_hash(*hash, (char *)module->blend_params, sizeof(dt_develop_blend_params_t));
}


void dt_iop_compute_blendop_hash(dt_iop_module_t *module, uint64_t hash, GList *masks)
{
  // Blend params are always inited even when module doesn't support blending
  hash = dt_hash(hash, (char *)module->blend_params, sizeof(dt_develop_blend_params_t));

  if(module->flags() & IOP_FLAGS_SUPPORTS_BLENDING)
  {
    // The caller owns the choice of forms list: the commit path passes the live dev->forms
    // (dt_dev_history_item_update_from_params falls back to it when no snapshot was taken, so
    // the hash of a module whose blend params reference a mask group can never come out blind
    // to that group's content — issue #1060 family), while history replay passes the snapshot
    // accumulated at the item's own position. NO ambient fallback here: this function cannot
    // know whether live forms describe the state being hashed (they don't during replay,
    // where dev->forms still holds the state being left).
    if(!IS_NULL_PTR(masks))
    {
      dt_masks_form_t *grp = dt_masks_get_from_id_ext(masks, module->blend_params->mask_id);
      hash = dt_masks_group_get_hash_ext(hash, masks, grp);
    }

    // else : no module->dev when running from init_default_params()

    // If module PROVIDES raster masks to others later in the pipe:
    // Account for later modules that reuse the raster mask provided by the current module.
    // This is a little cache invalidation trick: we change the final piece hash of this module,
    // to signal to the pipeline that it needs to recompute from lower than just the last changed module,
    // if that module references the raster mask produced here.
    // This contains the list of consumer modules:
    g_hash_table_foreach(module->raster_mask.source.users, (GHFunc)_hash_raster_masks, (gpointer)&hash);

    // module->raster_mask.source.masks contains only one mask as of now,
    // aka its blendop output, so no need to iterate over that.

    // If module CONSUMES raster masks from a module earlier in the pipe:
    // Account for its blendops.
    dt_iop_module_t *raster_source = module->raster_mask.sink.source;
    if(raster_source)
    {
      // Drawn masks
      if(!IS_NULL_PTR(masks))
      {
        dt_masks_form_t *raster_grp = dt_masks_get_from_id_ext(masks, raster_source->blend_params->mask_id);
        hash = dt_masks_group_get_hash_ext(hash, masks, raster_grp);
      }

      // Blending
      hash = dt_hash(hash, (char *)raster_source->blend_params, sizeof(dt_develop_blend_params_t));
    }
  }

  module->blendop_hash = hash;
}


void dt_iop_compute_module_hash(dt_iop_module_t *module, GList *masks)
{
  // Uniform way of getting the full state hash of user-defined parameters,
  // including masks and blending.
  // WARNING: doesn't take into account parameters dynamically set at runtime.

  uint64_t hash = dt_hash(5381, (char *)module->op, sizeof(dt_dev_operation_t));
  hash = dt_hash(hash, (char *)&module->enabled, sizeof(gboolean));
  hash = dt_hash(hash, (char *)&module->instance, sizeof(int32_t));
  hash = dt_hash(hash, (char *)&module->multi_priority, sizeof(int));
  hash = dt_hash(hash, (char *)&module->iop_order, sizeof(int));

  // Compute stand-alone blendop hash (mask hash) from the above
  // save to module->blendop_hash
  dt_iop_compute_blendop_hash(module, hash, masks);

  // Finish our module-wide (output) hash
  hash = dt_hash(hash, (char *)module->params, module->params_size);
  hash = dt_hash(hash, (char *)&module->blendop_hash, sizeof(uint64_t));

  module->hash = hash;
}

void dt_iop_commit_params(dt_iop_module_t *module, dt_iop_params_t *params,
                          dt_develop_blend_params_t *blendop_params, dt_dev_pixelpipe_t *pipe,
                          dt_dev_pixelpipe_iop_t *piece)
{
  // We need to commit also modules that are disabled because some of them
  // may self-enabled at commit time, depending on image input.
  // 1. commit params
  memcpy(piece->blendop_data, blendop_params, sizeof(dt_develop_blend_params_t));

#ifdef HAVE_OPENCL
  // assume process_cl is ready, commit_params can overwrite this.
  if(module->process_cl)
    piece->process_cl_ready = 1;

  piece->cache_output_on_ram = 0;
#endif // HAVE_OPENCL

  // register if module allows tiling, commit_params can overwrite this.
  if(module->flags() & IOP_FLAGS_ALLOW_TILING)
    piece->process_tiling_ready = 1;

  if(dt_get_debug_flags() & DT_DEBUG_PARAMS && module->so->get_introspection())
    _iop_validate_params(module->so->get_introspection()->field, params, TRUE);

  module->commit_params(module, params, pipe, piece);

  gchar *string = g_strdup_printf("/plugins/%s/opencl", module->op);

  if(!dt_conf_key_exists(string) || !dt_conf_key_not_empty(string)) 
      dt_conf_set_bool(string, TRUE);

  piece->process_cl_ready &= dt_conf_get_bool(string);
  dt_free(string);

  //uint64_t old_hash = module->hash;

  // 2. Update the internal hash
  // We need to update the blendop params dynamically, because drawn masks (forms)
  // belong to pipeline not to modules user params, and raster masks travel through the pipe.
  // So, module's blendops depend on the current and whole state of dev->forms if they use them
  dt_iop_compute_module_hash(module, module->dev->forms);

  uint64_t hash = module->hash;

  //if(old_hash != hash)
  //  fprintf(stdout, "WARNING: hash changed at history -> pipeline commit time for %s\n", module->op);

  /* Some modules seal part of their effective runtime contract only in commit_params(),
   * after reading GUI-only state, pipe mode, or other non-history inputs. Those modules
   * must opt in explicitly, because piece->data may also contain transient pointers or
   * scratch state for other modules and therefore cannot be hashed blindly. */
  if(module->runtime_data_hash(module, pipe, piece))
  {
    hash = dt_hash(hash, (const char *)piece->data, piece->data_size);
  }

  piece->global_hash = piece->hash = hash;
  piece->global_mask_hash = piece->blendop_hash = module->blendop_hash;

  dt_print(DT_DEBUG_PARAMS, "[pixelpipe] params commit for %s (%s) in pipe %s with hash %" PRIu64 "\n", 
           module->op, module->multi_name, 
           dt_pixelpipe_get_pipe_name(pipe->type), piece->hash);
}

void dt_iop_gui_reset(dt_iop_module_t *module)
{
  dt_gui_freeze_begin();
  if(module->gui_reset && !dt_iop_is_hidden(module)) module->gui_reset(module);
  dt_gui_freeze_end();
}

void dt_iop_nap(int32_t usec)
{
  if(usec <= 0) return;

  // relinquish processor
  sched_yield();

  // additionally wait the given amount of time
  g_usleep(usec);
}

gboolean dt_iop_get_cache_bypass(dt_iop_module_t *module)
{
  return module->bypass_cache;
}

void dt_iop_set_cache_bypass(dt_iop_module_t *module, gboolean state)
{
  module->bypass_cache = state;

  if(state && module->dev)
  {
    // Disable other modules bypass if set.
    for(GList *iop = g_list_last(module->dev->iop);
        iop;
        iop = g_list_previous(iop))
    {
      dt_iop_module_t *current = (dt_iop_module_t *)iop->data;
      if(current != module && current->bypass_cache) current->bypass_cache = FALSE;
    }
  }
}

void dt_iop_set_cache_bypass_variant(dt_iop_module_t *module, int variant)
{
  module->bypass_cache_variant = variant;
}


dt_iop_module_t *dt_iop_get_colorout_module(void)
{
  return dt_iop_get_module_from_list(dt_dev_get_global()->iop, "colorout");
}

dt_iop_module_t *dt_iop_get_module_from_list(GList *iop_list, const char *op)
{
  dt_iop_module_t *result = NULL;

  for(GList *modules = iop_list; modules; modules = g_list_next(modules))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)modules->data;
    if(strcmp(mod->op, op) == 0)
    {
      result = mod;
      break;
    }
  }

  return result;
}

dt_iop_module_t *dt_iop_get_module(const char *op)
{
  return dt_iop_get_module_from_list(dt_dev_get_global()->iop, op);
}

int dt_iop_get_module_flags(const char *op)
{
  GList *modules = darktable.iop;
  while(modules)
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)modules->data;
    if(!strcmp(module->op, op)) return module->flags();
    modules = g_list_next(modules);
  }
  return 0;
}

// to be called before issuing any query based on memory.darktable_iop_names
void dt_iop_set_darktable_iop_table()
{
  if(IS_NULL_PTR(darktable.iop)) return;

  const guint count = g_list_length(darktable.iop);
  if(count == 0) return;

  // module->name() is the localised display name, which only the module object can give --
  // hence the array rather than a table the repository could fill by itself.
  dt_iop_name_row_t *rows = (dt_iop_name_row_t *)calloc(count, sizeof(dt_iop_name_row_t));
  if(IS_NULL_PTR(rows)) return;

  guint i = 0;
  for(GList *iop = darktable.iop; iop && i < count; iop = g_list_next(iop))
  {
    dt_iop_module_so_t *module = (dt_iop_module_so_t *)iop->data;
    rows[i].operation = module->op;
    rows[i].name = module->name();
    i++;
  }

  dt_collection_query_set_iop_names(rows, i);
  dt_free(rows);
}

const gchar *dt_iop_get_localized_name(const gchar *op)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_names = NULL;
  if(IS_NULL_PTR(module_names))
  {
    module_names = g_hash_table_new(g_str_hash, g_str_equal);
    for(GList *iop = darktable.iop; iop; iop = g_list_next(iop))
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)iop->data;
      g_hash_table_insert(module_names, module->op, g_strdup(module->name()));
    }
  }
  if(!IS_NULL_PTR(op))
  {
    return (gchar *)g_hash_table_lookup(module_names, op);
  }
  else {
    return _("ERROR");
  }
}

const gchar *dt_iop_get_localized_aliases(const gchar *op)
{
  // Prepare mapping op -> localized name
  static GHashTable *module_aliases = NULL;
  if(IS_NULL_PTR(module_aliases))
  {
    module_aliases = g_hash_table_new(g_str_hash, g_str_equal);
    for(GList *iop = darktable.iop; iop; iop = g_list_next(iop))
    {
      dt_iop_module_so_t *module = (dt_iop_module_so_t *)iop->data;
      g_hash_table_insert(module_aliases, module->op, g_strdup(module->aliases()));
    }
  }
  if(!IS_NULL_PTR(op))
  {
    return (gchar *)g_hash_table_lookup(module_aliases, op);
  }
  else {
    return _("ERROR");
  }
}

void dt_iop_update_multi_priority(dt_iop_module_t *module, int new_priority)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, module->raster_mask.source.users);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    dt_iop_module_t *sink_module = (dt_iop_module_t *)key;

    sink_module->blend_params->raster_mask_instance = new_priority;

    // also fix history entries
    for(GList *hiter = module->dev->history; hiter; hiter = g_list_next(hiter))
    {
      dt_dev_history_item_t *hist = (dt_dev_history_item_t *)hiter->data;
      if(hist->module == sink_module)
        hist->blend_params->raster_mask_instance = new_priority;
    }
  }

  module->multi_priority = new_priority;
}

gboolean dt_iop_is_raster_mask_used(dt_iop_module_t *module, int id)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init(&iter, module->raster_mask.source.users);
  while(g_hash_table_iter_next(&iter, &key, &value))
  {
    if(GPOINTER_TO_INT(value) == id)
      return TRUE;
  }
  return FALSE;
}

dt_iop_module_t *dt_iop_get_module_by_op_priority(GList *modules, const char *operation, const int multi_priority)
{
  dt_iop_module_t *mod_ret = NULL;

  for(GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m->data;

    if(strcmp(mod->op, operation) == 0
       && (mod->multi_priority == multi_priority || multi_priority == -1))
    {
      mod_ret = mod;
      break;
    }
  }
  return mod_ret;
}

dt_iop_module_t *dt_iop_get_module_by_instance_name(GList *modules, const char *operation, const char *multi_name)
{
  dt_iop_module_t *mod_ret = NULL;

  for(GList *m = modules; m; m = g_list_next(m))
  {
    dt_iop_module_t *mod = (dt_iop_module_t *)m->data;

    if((strcmp(mod->op, operation) == 0)
       && ((IS_NULL_PTR(multi_name)) || (strcmp(mod->multi_name, multi_name) == 0)))
    {
      mod_ret = mod;
      break;
    }
  }
  return mod_ret;
}

gboolean dt_iop_is_first_instance(GList *modules, dt_iop_module_t *module)
{
  gboolean is_first = TRUE;
  GList *iop = modules;
  while(iop)
  {
    dt_iop_module_t *m = (dt_iop_module_t *)iop->data;
    if(!strcmp(m->op, module->op))
    {
      is_first = (m == module);
      break;
    }
    iop = g_list_next(iop);
  }

  return is_first;
}

const char **dt_iop_set_description(dt_iop_module_t *module, const char *main_text, const char *purpose, const char *input, const char *process,
                             const char *output)
{
  static const char *str_out[5] = {NULL, NULL, NULL, NULL, NULL};

  str_out[0] = main_text;
  str_out[1] = purpose;
  str_out[2] = input;
  str_out[3] = process;
  str_out[4] = output;

  return (const char **)str_out;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
