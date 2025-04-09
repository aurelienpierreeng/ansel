/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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

#pragma once

#include "common/act_on.h"

#include "common/history.h"
#include "common/image.h"
#ifdef HAVE_PRINT
#include "common/cups_print.h"
#include "common/printing.h"
#endif
#ifdef HAVE_MAP
#include "common/geo.h"
#include "common/map_locations.h"
#include <osm-gps-map.h>
#endif
#include <cairo.h>
#include <gmodule.h>
#include <gui/gtk.h>
#include <inttypes.h>
#include <sqlite3.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/events.h"
#include "lua/modules.h"
#include "lua/types.h"
#include "lua/view.h"
#endif

/** available views flags, a view should return its type and
    is also used in modules flags available in src/libs to
    control which view the module should be available in also
    which placement in the panels the module have.
*/
typedef enum
{
  DT_VIEW_LIGHTTABLE = 1 << 0,
  DT_VIEW_DARKROOM = 1 << 2,
  DT_VIEW_MAP = 1 << 3,
  DT_VIEW_SLIDESHOW = 1 << 4,
  DT_VIEW_PRINT = 1 << 5,
} dt_view_type_flags_t;

// flags that a view can set in flags()
typedef enum dt_view_flags_t
{
  VIEW_FLAGS_NONE = 0,
  VIEW_FLAGS_HIDDEN = 1 << 0,       // Hide the view from userinterface
} dt_view_flags_t;

typedef enum dt_darkroom_layout_t
{
  DT_DARKROOM_LAYOUT_FIRST = -1,
  DT_DARKROOM_LAYOUT_EDITING = 0,
  DT_DARKROOM_LAYOUT_COLOR_ASSESMENT = 1,
  DT_DARKROOM_LAYOUT_LAST = 3
} dt_darkroom_layout_t;


// flags that a view can set in flags()
typedef enum dt_view_surface_value_t
{
  DT_VIEW_SURFACE_OK = 0,
  DT_VIEW_SURFACE_KO,
} dt_view_surface_value_t;

#define DT_VIEW_ALL                                                                              \
  (DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_SLIDESHOW | \
   DT_VIEW_PRINT)

/* maximum zoom factor for the lighttable */
#define DT_LIGHTTABLE_MAX_ZOOM 12

/**
 * main dt view module (as lighttable or darkroom)
 */
typedef struct dt_view_t
{
#define INCLUDE_API_FROM_MODULE_H
#include "views/view_api.h"

  char module_name[64];
  // dlopened module
  GModule *module;
  // custom data for module
  void *data;
  // width and height of allocation
  uint32_t width, height;
  // scroll bar control
  float vscroll_size, vscroll_lower, vscroll_viewport_size, vscroll_pos;
  float hscroll_size, hscroll_lower, hscroll_viewport_size, hscroll_pos;
} dt_view_t;

typedef enum dt_view_image_over_t
{
  DT_VIEW_ERR     = -1,
  DT_VIEW_DESERT  =  0,
  DT_VIEW_STAR_1  =  1,
  DT_VIEW_STAR_2  =  2,
  DT_VIEW_STAR_3  =  3,
  DT_VIEW_STAR_4  =  4,
  DT_VIEW_STAR_5  =  5,
  DT_VIEW_REJECT  =  6,
  DT_VIEW_GROUP   =  7,
  DT_VIEW_AUDIO   =  8,
  DT_VIEW_ALTERED =  9,
  DT_VIEW_END     = 10, // placeholder for the end of the list
} dt_view_image_over_t;

/** returns an uppercase string of file extension **plus** some flag information **/
char* dt_view_extend_modes_str(const char * name, const gboolean is_hdr, const gboolean is_bw, const gboolean is_bw_flow);
/** expose an image and return a cair0_surface. */
dt_view_surface_value_t dt_view_image_get_surface(int32_t imgid, int width, int height, cairo_surface_t **surface,
                                                  int zoom);


/**
 * holds all relevant data needed to manage the view
 * modules.
 */
typedef struct dt_view_manager_t
{
  GList *views;
  dt_view_t *current_view;

  // images currently active in the main view (there can be more than 1 in culling)
  GList *active_images;

  // copy/paste history structure
  dt_history_copy_item_t copy_paste;

  /* reusable db statements
   * TODO: reconsider creating a common/database helper API
   *       instead of having this spread around in sources..
   */
  struct
  {
    GPid audio_player_pid;   // the pid of the child process
    int32_t audio_player_id; // the imgid of the image the audio is played for
    guint audio_player_event_source;
  } audio;

  // toggle button for guides (in the module toolbox)
  GtkWidget *guides_toggle, *guides, *guides_colors, *guides_contrast, *guides_popover;

  /*
   * Proxy
   */
  struct
  {
    /* module toolbox proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*add)(struct dt_lib_module_t *, GtkWidget *, dt_view_type_flags_t);
    } module_toolbox;

    /* module collection proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*update)(struct dt_lib_module_t *);
    } module_collect;

    /* darkroom view proxy object */
    struct
    {
      struct dt_view_t *view;
      dt_darkroom_layout_t (*get_layout)(struct dt_view_t *view);
    } darkroom;

/* map view proxy object */
#ifdef HAVE_MAP
    struct
    {
      struct dt_view_t *view;
      void (*center_on_location)(const dt_view_t *view, gdouble lon, gdouble lat, double zoom);
      void (*center_on_bbox)(const dt_view_t *view, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2);
      void (*show_osd)(const dt_view_t *view);
      void (*set_map_source)(const dt_view_t *view, OsmGpsMapSource_t map_source);
      GObject *(*add_marker)(const dt_view_t *view, dt_geo_map_display_t type, GList *points);
      gboolean (*remove_marker)(const dt_view_t *view, dt_geo_map_display_t type, GObject *marker);
      void (*add_location)(const dt_view_t *view, dt_map_location_data_t *p, const guint posid);
      void (*location_action)(const dt_view_t *view, const int action);
      void (*drag_set_icon)(const dt_view_t *view, GdkDragContext *context, const int32_t imgid, const int count);
      gboolean (*redraw)(gpointer user_data);
      gboolean (*display_selected)(gpointer user_data);
    } map;
#endif

    /* map view proxy object */
#ifdef HAVE_PRINT
    struct
    {
      struct dt_view_t *view;
      void (*print_settings)(const dt_view_t *view, dt_print_info_t *pinfo, dt_images_box *imgs);
    } print;
#endif
  } proxy;


} dt_view_manager_t;

void dt_view_manager_init(dt_view_manager_t *vm);
void dt_view_manager_gui_init(dt_view_manager_t *vm);
void dt_view_manager_cleanup(dt_view_manager_t *vm);

/** return translated name. */
const char *dt_view_manager_name(dt_view_manager_t *vm);
/** switch to this module. returns non-null if the module fails to change. */
int dt_view_manager_switch(dt_view_manager_t *vm, const char *view_name);
int dt_view_manager_switch_by_view(dt_view_manager_t *vm, const dt_view_t *new_view);
/** expose current module. */
void dt_view_manager_expose(dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height,
                            int32_t pointerx, int32_t pointery);
/** reset current view. */
void dt_view_manager_reset(dt_view_manager_t *vm);
/** get current view of the view manager. */
const dt_view_t *dt_view_manager_get_current_view(dt_view_manager_t *vm);

void dt_view_manager_mouse_enter(dt_view_manager_t *vm);
void dt_view_manager_mouse_leave(dt_view_manager_t *vm);
void dt_view_manager_mouse_moved(dt_view_manager_t *vm, double x, double y, double pressure, int which);
int dt_view_manager_button_released(dt_view_manager_t *vm, double x, double y, int which, uint32_t state);
int dt_view_manager_button_pressed(dt_view_manager_t *vm, double x, double y, double pressure, int which,
                                   int type, uint32_t state);
int dt_view_manager_key_pressed(dt_view_manager_t *vm, GdkEventKey *event);
void dt_view_manager_configure(dt_view_manager_t *vm, int width, int height);
int dt_view_manager_scrolled(dt_view_manager_t *vm, double x, double y, int up, int state);

/** add widget to the current view toolbox */
void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t view);

/** add widget to the current module toolbox */
void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t view);

/*
 * Tethering View PROXY
 */
/** get the current selected image id for tethering session */
int32_t dt_view_tethering_get_selected_imgid(const dt_view_manager_t *vm);
/** set the current jobcode for tethering session */
void dt_view_tethering_set_job_code(const dt_view_manager_t *vm, const char *name);
/** get the current jobcode for tethering session */
const char *dt_view_tethering_get_job_code(const dt_view_manager_t *vm);

/** update the collection module */
void dt_view_collection_update(const dt_view_manager_t *vm);

// active images functions
void dt_view_active_images_reset(gboolean raise);
void dt_view_active_images_set(GList *images, gboolean raise);
void dt_view_active_images_add(int32_t imgid, gboolean raise);
void dt_view_active_images_remove(int32_t imgid, gboolean raise);
gboolean dt_view_active_images_has_imgid(int32_t imgid);

GList *dt_view_active_images_get_all();
int32_t dt_view_active_images_get_first();

/** get the darkroom current layout */
dt_darkroom_layout_t dt_view_darkroom_get_layout(dt_view_manager_t *vm);

/* audio */
void dt_view_audio_start(dt_view_manager_t *vm, int32_t imgid);
void dt_view_audio_stop(dt_view_manager_t *vm);

/*
 * Map View Proxy
 */
#ifdef HAVE_MAP
void dt_view_map_center_on_location(const dt_view_manager_t *vm, gdouble lon, gdouble lat, gdouble zoom);
void dt_view_map_center_on_bbox(const dt_view_manager_t *vm, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2);
void dt_view_map_show_osd(const dt_view_manager_t *vm);
void dt_view_map_set_map_source(const dt_view_manager_t *vm, OsmGpsMapSource_t map_source);
GObject *dt_view_map_add_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GList *points);
gboolean dt_view_map_remove_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GObject *marker);
void dt_view_map_add_location(const dt_view_manager_t *vm, dt_map_location_data_t *p, const guint posid);
void dt_view_map_location_action(const dt_view_manager_t *vm, const int action);
void dt_view_map_drag_set_icon(const dt_view_manager_t *vm, GdkDragContext *context, const int32_t imgid, const int count);
#endif

/*
 * Print View Proxy
 */
#ifdef HAVE_PRINT
void dt_view_print_settings(const dt_view_manager_t *vm, dt_print_info_t *pinfo, dt_images_box *imgs);
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
