/*
 *    This file is part of darktable,
 *    Copyright (C) 2016-2022 the darktable authors.
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    darktable is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    darktable is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Monitor ICC profile interrogation, extracted from common/colorspaces.c.
 *
 * For X11 this follows the ICC profile specification version 0.2 from
 * http://burtonini.com/blog/computers/xicc, based on code from Gimp's
 * modules/cdisplay_lcms.c.
 */

#include "system/display_profile.h"
#include "system/mem_alloc.h"

#include "system/macros.h"

#ifdef _WIN32
#include <dwmapi.h>
#include <gdk/gdkwin32.h>
#endif

#if defined GDK_WINDOWING_WAYLAND
#include <sys/mman.h>
#include <lcms2.h>
#include <gtk-3.0/gdk/gdkwayland.h>
#include "color-management-v1-client-protocol.h"
#endif

#if 0
#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreServices/CoreServices.h>
#endif

#if defined GDK_WINDOWING_X11
/* GdkMonitor carries no index of its own, and the X ICC atom is numbered by monitor
 * index, so the index has to be recovered by identity. */
static int _monitor_index(GdkMonitor *monitor)
{
  GdkDisplay *display = gdk_monitor_get_display(monitor);
  const int n_monitors = gdk_display_get_n_monitors(display);
  for(int i = 0; i < n_monitors; i++)
  {
    if(gdk_display_get_monitor(display, i) == monitor) return i;
  }

  return -1;
}
#endif

#if defined GDK_WINDOWING_WAYLAND
typedef struct {
  struct wl_surface *color_wl_surface;
  struct wp_color_manager_v1 *color_manager;
  struct wp_color_management_surface_v1 *color_surface;
  struct wp_color_management_surface_feedback_v1 *color_surface_feedback;
  struct wp_image_description_v1 *color_image_description;
  struct wp_image_description_info_v1 *color_image_description_info;
  guint8 *icc_buffer;
  gint icc_buffer_size;
  gint r_x;
  gint r_y;
  gint g_x;
  gint g_y;
  gint b_x;
  gint b_y;
  gint w_x;
  gint w_y;
  guint tf_power;
  gboolean have_registry;
  gboolean have_color_manager;
  gboolean have_icc;
  gboolean have_primaries;
  gboolean have_transfer_function;
} wayland_color_management_struct;

wayland_color_management_struct wayland_color_management = {0};

void handle_wp_image_description_info_done(void *data,
		                                       struct wp_image_description_info_v1 *wp_image_description_info_v1)
{
  wayland_color_management_struct *wcm = data;

  wp_image_description_info_v1_destroy(wcm->color_image_description_info);
  wcm->color_image_description_info = NULL;
}

void handle_wp_image_description_info_icc_file(void *data,
			                                         struct wp_image_description_info_v1 *wp_image_description_info_v1,
																							 int32_t icc,
																							 uint32_t icc_size)
{
  wayland_color_management_struct *wcm = data;

  if (icc_size == 0)
  {
    close(icc);
    wcm->icc_buffer = NULL;
    wcm->icc_buffer_size = 0;
    return;
  }

  wcm->icc_buffer = mmap(NULL, icc_size, PROT_READ, MAP_PRIVATE, icc, 0);
  wcm->icc_buffer_size = icc_size;

  if (wcm->icc_buffer == MAP_FAILED)
  {
    close(icc);
    wcm->icc_buffer = NULL;
    wcm->icc_buffer_size = 0;
    return;
  }

  close(icc);

  wcm->have_icc = TRUE;
}

void handle_wp_image_description_info_primaries(void *data,
			                                          struct wp_image_description_info_v1 *wp_image_description_info_v1,
																								int32_t r_x,
																						    int32_t r_y,
																								int32_t g_x,
																						    int32_t g_y,
																								int32_t b_x,
																						    int32_t b_y,
																								int32_t w_x,
																						    int32_t w_y)
{
  wayland_color_management_struct *wcm = data;

  wcm->r_x = r_x;
  wcm->r_y = r_y;
  wcm->g_x = g_x;
  wcm->g_y = g_y;
  wcm->b_x = b_x;
  wcm->b_y = b_y;
  wcm->w_x = w_x;
  wcm->w_y = w_y;

  wcm->have_primaries = TRUE;
}

void handle_wp_image_description_info_primaries_named(void *data,
				                                              struct wp_image_description_info_v1 *wp_image_description_info_v1,
																											uint32_t primaries)
{
  // Unused.
}

void handle_wp_image_description_info_tf_power(void *data,
                                               struct wp_image_description_info_v1 *wp_image_description_info_v1,
                                               uint32_t eexp)
{
  wayland_color_management_struct *wcm = data;

  wcm->tf_power = eexp;

  wcm->have_transfer_function = TRUE;
}

void handle_wp_image_description_info_tf_named(void *data,
			                                         struct wp_image_description_info_v1 *wp_image_description_info_v1,
																							 uint32_t tf)
{
  wayland_color_management_struct *wcm = data;

  switch (tf)
  {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
    {
      wcm->tf_power = 22000;
    } break;
  }

  wcm->have_transfer_function = TRUE;
}

void handle_wp_image_description_info_luminances(void *data,
			                                           struct wp_image_description_info_v1 *wp_image_description_info_v1,
																								 uint32_t min_lum,
																						     uint32_t max_lum,
																								 uint32_t reference_lum)
{
  // Unused.
}

void handle_wp_image_description_info_target_primaries(void *data,
				                                               struct wp_image_description_info_v1 *wp_image_description_info_v1,
																											 int32_t r_x,
																											 int32_t r_y,
																											 int32_t g_x,
																											 int32_t g_y,
																											 int32_t b_x,
																											 int32_t b_y,
																											 int32_t w_x,
																											 int32_t w_y)
{
  // Unused.
}

void handle_wp_image_description_info_target_luminance(void *data,
				                                               struct wp_image_description_info_v1 *wp_image_description_info_v1,
																											 uint32_t min_lum,
																											 uint32_t max_lum)
{
  // Unused.
}

void handle_wp_image_description_info_target_max_cll(void *data,
			                                               struct wp_image_description_info_v1 *wp_image_description_info_v1,
																										 uint32_t max_cll)
{
  // Unused.
}

void handle_wp_image_description_info_target_max_fall(void *data,
				                                              struct wp_image_description_info_v1 *wp_image_description_info_v1,
																											uint32_t max_fall)
{
  // Unused.
}

struct wp_image_description_info_v1_listener wp_image_description_info_v1_listener = {
  .done = handle_wp_image_description_info_done,
	.icc_file = handle_wp_image_description_info_icc_file,
	.primaries = handle_wp_image_description_info_primaries,
	.primaries_named = handle_wp_image_description_info_primaries_named,
	.tf_power = handle_wp_image_description_info_tf_power,
	.tf_named = handle_wp_image_description_info_tf_named,
	.luminances = handle_wp_image_description_info_luminances,
	.target_primaries = handle_wp_image_description_info_target_primaries,
	.target_luminance = handle_wp_image_description_info_target_luminance,
	.target_max_cll = handle_wp_image_description_info_target_max_cll,
	.target_max_fall = handle_wp_image_description_info_target_max_fall,
};

void handle_wp_image_description_failed(void *data,
		                                    struct wp_image_description_v1 *wp_image_description_v1,
																				uint32_t cause,
													              const char *msg)
{
  // Unused, could be useful for debugging.
}

void handle_wp_image_description_ready(void *data,
		                                   struct wp_image_description_v1 *wp_image_description_v1,
																			 uint32_t identity)
{
  wayland_color_management_struct *wcm = data;

  // Reset old image description color state.
  if (wcm->icc_buffer && wcm->icc_buffer_size > 0)
  {
    munmap(wcm->icc_buffer, wcm->icc_buffer_size);
    wcm->icc_buffer_size = 0;
  }

  wcm->r_x = 0;
  wcm->r_y = 0;
  wcm->g_x = 0;
  wcm->g_y = 0;
  wcm->b_x = 0;
  wcm->b_y = 0;
  wcm->w_x = 0;
  wcm->w_y = 0;
  wcm->tf_power = 0;

  wcm->have_icc = FALSE;
  wcm->have_primaries = FALSE;
  wcm->have_transfer_function = FALSE;

  wcm->color_image_description_info = wp_image_description_v1_get_information(wcm->color_image_description);
  wp_image_description_info_v1_add_listener(wcm->color_image_description_info, &wp_image_description_info_v1_listener, &wayland_color_management);

  // Set image description on surface to tell compositor in which colorspace window is in.
  wp_color_management_surface_v1_set_image_description(wcm->color_surface, wcm->color_image_description, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);

  wl_surface_commit(wcm->color_wl_surface);
}

struct wp_image_description_v1_listener wp_image_description_v1_listener = {
  .failed = handle_wp_image_description_failed,
	.ready = handle_wp_image_description_ready,
};

void handle_wp_color_management_surface_feedback_preferred_changed(void *data,
				                                                           struct wp_color_management_surface_feedback_v1 *wp_color_management_surface_feedback_v1,
																																	 uint32_t identity)
{
  wayland_color_management_struct *wcm = data;

  if (wcm->color_image_description)
  {
    wp_image_description_v1_destroy(wcm->color_image_description);
    wcm->color_image_description = NULL;
  }

  wcm->color_image_description = wp_color_management_surface_feedback_v1_get_preferred(wcm->color_surface_feedback);
  wp_image_description_v1_add_listener(wcm->color_image_description, &wp_image_description_v1_listener, &wayland_color_management);
}

struct wp_color_management_surface_feedback_v1_listener wp_color_management_surface_feedback_v1_listener = {
  .preferred_changed = handle_wp_color_management_surface_feedback_preferred_changed,
};

void handle_wl_registry_global(void *data,
		                           struct wl_registry *wl_registry,
															 uint32_t name,
									             const char *interface,
															 uint32_t version)
{
  wayland_color_management_struct *wcm = data;

  if (strcmp(interface, wp_color_manager_v1_interface.name) == 0)
  {
    wcm->color_manager = wl_registry_bind(wl_registry, name, &wp_color_manager_v1_interface, 1);

    if (wcm->color_manager)
    {
      wcm->have_color_manager = TRUE;
    }
  }
}

void handle_wl_registry_global_remove(void *data,
			                                struct wl_registry *wl_registry,
																			uint32_t name)
{
  // Unused, compositor will not send global remove events.
}

struct wl_registry_listener wl_registry_listener = {
	.global = handle_wl_registry_global,
	.global_remove = handle_wl_registry_global_remove,
};
#endif

void dt_display_profile_read(GtkWidget *widget, guint8 **buffer, gint *buffer_size, gchar **source)
{
  if(IS_NULL_PTR(widget) || IS_NULL_PTR(buffer) || IS_NULL_PTR(buffer_size) || IS_NULL_PTR(source)) return;

#if defined GDK_WINDOWING_X11

  GdkWindow *window = gtk_widget_get_window(widget);
  GdkScreen *screen = gtk_widget_get_screen(widget);
  if(IS_NULL_PTR(screen)) screen = gdk_screen_get_default();

  GdkDisplay *display = gtk_widget_get_display(widget);
  const int monitor = _monitor_index(gdk_display_get_monitor_at_window(display, window));

  char *atom_name;
  if(monitor > 0)
    atom_name = g_strdup_printf("_ICC_PROFILE_%d", monitor);
  else
    atom_name = g_strdup("_ICC_PROFILE");

  *source = g_strdup_printf("xatom %s", atom_name);

  GdkAtom type = GDK_NONE;
  gint format = 0;
  gdk_property_get(gdk_screen_get_root_window(screen), gdk_atom_intern(atom_name, FALSE), GDK_NONE, 0,
                   64 * 1024 * 1024, FALSE, &type, &format, buffer_size, buffer);
  dt_free(atom_name);

#endif

#if defined GDK_WINDOWING_WAYLAND
  GdkDisplay *wayland_gdk_display = gtk_widget_get_display(widget);
  GdkWindow *wayland_gdk_window = gtk_widget_get_window(widget);

  if (GDK_IS_WAYLAND_DISPLAY(wayland_gdk_display))
  {
    if (!wayland_color_management.color_manager)
    {
      struct wl_display *wl_display = gdk_wayland_display_get_wl_display(wayland_gdk_display);

      if (!wayland_color_management.have_registry)
      {
        struct wl_registry *wl_registry = wl_display_get_registry(wl_display);
        wl_registry_add_listener(wl_registry, &wl_registry_listener, &wayland_color_management);

        // Initial roundtrip for wl_registry events
        wl_display_roundtrip(wl_display);
        wayland_color_management.have_registry = TRUE;
      }

      if (wayland_color_management.have_color_manager)
      {
        // Initial roundtrip for wp_color_manager events.
        wl_display_roundtrip(wl_display);

        wayland_color_management.color_wl_surface = gdk_wayland_window_get_wl_surface(wayland_gdk_window);

        wayland_color_management.color_surface = wp_color_manager_v1_get_surface(wayland_color_management.color_manager, wayland_color_management.color_wl_surface);

        wayland_color_management.color_surface_feedback = wp_color_manager_v1_get_surface_feedback(wayland_color_management.color_manager, wayland_color_management.color_wl_surface);
        wp_color_management_surface_feedback_v1_add_listener(wayland_color_management.color_surface_feedback, &wp_color_management_surface_feedback_v1_listener, &wayland_color_management);

        wayland_color_management.color_image_description = wp_color_management_surface_feedback_v1_get_preferred(wayland_color_management.color_surface_feedback);
        wp_image_description_v1_add_listener(wayland_color_management.color_image_description, &wp_image_description_v1_listener, &wayland_color_management);

        // Initial roundtrip for wp_image_description events
        wl_display_roundtrip(wl_display);

        // Initial roundtrip for wp_image_description_info events
        wl_display_roundtrip(wl_display);
      }
    }

    if (wayland_color_management.color_manager && wayland_color_management.have_icc)
    {
      *buffer = g_malloc(wayland_color_management.icc_buffer_size);
      *buffer_size = wayland_color_management.icc_buffer_size;
      *source = g_strdup("Wayland color profile api");

      if (*buffer == NULL)
      {
        g_free(*source);
        *source = NULL;
        *buffer_size = 0;
        return;
      }

      memcpy(*buffer, wayland_color_management.icc_buffer, wayland_color_management.icc_buffer_size);
      return;
    }

    if (wayland_color_management.color_manager && wayland_color_management.have_primaries && wayland_color_management.have_transfer_function)
    {
      cmsCIExyY white_point;
      cmsCIExyYTRIPLE primaries;
      cmsToneCurve *curve[3];
      cmsHPROFILE lcms_profile;

      primaries.Red.x = wayland_color_management.r_x / 1000000.0;
      primaries.Red.y = wayland_color_management.r_y / 1000000.0;
      primaries.Red.Y = 1.0;
      primaries.Green.x = wayland_color_management.g_x / 1000000.0;
      primaries.Green.y = wayland_color_management.g_y / 1000000.0;
      primaries.Green.Y = 1.0;
      primaries.Blue.x = wayland_color_management.b_x / 1000000.0;
      primaries.Blue.y = wayland_color_management.b_y / 1000000.0;
      primaries.Blue.Y = 1.0;

      white_point.x = wayland_color_management.w_x / 1000000.0;
      white_point.y = wayland_color_management.w_y / 1000000.0;
      white_point.Y = 1.0;

      cmsToneCurve *single_curve = cmsBuildGamma(NULL, wayland_color_management.tf_power / 10000.0);
      if (!single_curve)
      {
        return;
      }

      curve[0] = curve[1] = curve[2] = single_curve;

      lcms_profile = cmsCreateRGBProfile(&white_point, &primaries, curve);

      cmsFreeToneCurve(single_curve);

      if (!lcms_profile)
      {
        return;
      }

      cmsUInt32Number size = 0;

      if (!cmsSaveProfileToMem(lcms_profile, NULL, &size) || size == 0)
      {
        cmsCloseProfile(lcms_profile);
        return;
      }

      guint8 *data = g_malloc(size);
      if (!data)
      {
        cmsCloseProfile(lcms_profile);
        return;
      }

      if (cmsSaveProfileToMem(lcms_profile, data, &size))
      {
        *buffer = data;
        *buffer_size = (gint)size;
        *source = g_strdup("Wayland color profile api");
      }
      else
      {
        g_free(data);
      }

      cmsCloseProfile(lcms_profile);
      return;
    }

  }

#elif defined GDK_WINDOWING_QUARTZ
#if 0
  GdkScreen *screen = gtk_widget_get_screen(widget);
  if(IS_NULL_PTR(screen)) screen = gdk_screen_get_default();
  const int monitor = gdk_screen_get_monitor_at_window(screen, gtk_widget_get_window(widget));

  CGDirectDisplayID ids[monitor + 1];
  uint32_t total_ids;
  CMProfileRef prof = NULL;
  if(CGGetOnlineDisplayList(monitor + 1, &ids[0], &total_ids) == kCGErrorSuccess && total_ids == monitor + 1)
    CMGetProfileByAVID(ids[monitor], &prof);
  if(!IS_NULL_PTR(prof))
  {
    CFDataRef data;
    data = CMProfileCopyICCData(NULL, prof);
    CMCloseProfile(prof);

    UInt8 *tmp_buffer = (UInt8 *)g_malloc(CFDataGetLength(data));
    CFDataGetBytes(data, CFRangeMake(0, CFDataGetLength(data)), tmp_buffer);

    *buffer = (guint8 *)tmp_buffer;
    *buffer_size = CFDataGetLength(data);

    CFRelease(data);
  }
  *source = g_strdup("osx color profile api");
#endif

#elif defined G_OS_WIN32

  GdkWindow *window = gtk_widget_get_window(widget);
  HWND hwnd = (HWND)gdk_win32_window_get_handle(window);                 // get window handle
  HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST); // get monitor handle
  if(IS_NULL_PTR(hMonitor)) return;                                      // TODO log error

  MONITORINFOEX monitorInfo;
  monitorInfo.cbSize = sizeof(MONITORINFOEX);
  if(!GetMonitorInfoW(hMonitor, (LPMONITORINFO)&monitorInfo)) return;    // TODO log error

  HDC hdc = CreateIC(L"MONITOR", monitorInfo.szDevice, NULL, NULL);      // device-info context of the monitor
  if(!IS_NULL_PTR(hdc))
  {
    DWORD len = 0;
    GetICMProfile(hdc, &len, NULL);
    wchar_t *wpath = g_new(wchar_t, len);

    if(GetICMProfileW(hdc, &len, wpath))
    {
      gchar *path = g_utf16_to_utf8(wpath, -1, NULL, NULL, NULL);
      if(path)
      {
        gsize size;
        g_file_get_contents(path, (gchar **)buffer, &size, NULL);
        *buffer_size = size;
        dt_free(path);
      }
    }
    dt_free(wpath);
    DeleteDC(hdc);
  }
  *source = g_strdup("windows color profile api");

#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
