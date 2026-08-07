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

#include "common/macros.h"

#ifdef _WIN32
#include <dwmapi.h>
#include <gdk/gdkwin32.h>
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
