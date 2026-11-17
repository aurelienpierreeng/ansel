/*
    This file is part of darktable,
    Copyright (C) 2013-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2014 Pascal de Bruijn.
    Copyright (C) 2014, 2016-2017 Roman Lebedev.
    Copyright (C) 2016 Kai-Uwe Behrmann.
    Copyright (C) 2017 Matthias Andree.
    Copyright (C) 2020-2021 Pascal Obry.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2022, 2026 Aurélien PIERRE.
    Copyright (C) 2022 Martin Bařinka.

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

/*
  You can compile this tool standalone, from src/apps/ansel-cmstest.
  Dependencies: liblcms2, libglib, optionally libcolord, and one or both of
  libX11 + libXrandr and libwayland-client.
  The Wayland half needs the protocol code generated first:
    wayland-scanner client-header ../../external/wayland-protocols/color-management-v1.xml \
        color-management-v1-client-protocol.h
    wayland-scanner private-code ../../external/wayland-protocols/color-management-v1.xml \
        color-management-v1-protocol.c
  Then compile with something like this:
    gcc -W -Wall -I../.. -I. \
        `pkg-config --cflags --libs glib-2.0 gobject-2.0 lcms2 colord x11 xrandr wayland-client` \
        -DHAVE_X11 -DHAVE_WAYLAND -DHAVE_COLORD -Ddarktable_package_version=\"'standalone'\" \
        main.c x11.c wayland.c color-management-v1-protocol.c -o ansel-cmstest
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "apps/ansel-cmstest/cmstest.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include <glib.h>
#include <lcms2.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_COLORD
#include <colord.h> // conditional-ok: colord is optional, and only dt_cmstest_colord_filename() names it
#endif

char *dt_cmstest_profile_description(const unsigned char *data, size_t data_size)
{
  cmsUInt32Number size;
  gchar *buf = NULL;
  wchar_t *wbuf = NULL;
  gchar *utf8 = NULL;
  char *result = NULL;

  if(IS_NULL_PTR(data) || data_size == 0) return NULL;

  cmsHPROFILE p = cmsOpenProfileFromMem(data, data_size);
  if(IS_NULL_PTR(p)) return NULL;

  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", NULL, 0);
  if(size == 0) goto error;

  buf = (char *)calloc(size + 1, sizeof(char));
  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", buf, size);
  if(size == 0) goto error;

  // most unix like systems should work with this, but at least Windows doesn't
  if(sizeof(wchar_t) != 4 || g_utf8_validate(buf, -1, NULL))
    result = g_strdup(buf); // better a little weird than totally borked
  else
  {
    wbuf = (wchar_t *)calloc(size + 1, sizeof(wchar_t));
    size = cmsGetProfileInfo(p, cmsInfoDescription, "en", "US", wbuf, sizeof(wchar_t) * size);
    if(size == 0) goto error;
    utf8 = g_ucs4_to_utf8((gunichar *)wbuf, -1, NULL, NULL, NULL);
    if(IS_NULL_PTR(utf8)) goto error;
    result = g_strdup(utf8);
  }

  dt_free(buf);
  dt_free(wbuf);
  dt_free(utf8);
  cmsCloseProfile(p);
  return result;

error:
  if(buf) result = g_strdup(buf); // better a little weird than totally borked
  dt_free(buf);
  dt_free(wbuf);
  dt_free(utf8);
  cmsCloseProfile(p);
  return result;
}

#ifdef HAVE_COLORD
/* One connection to colord for the whole run, made at the first question. A failure is said
 * once and every monitor then reads as having no profile there. */
static CdClient *_colord = NULL;
static gboolean _colord_asked = FALSE;
#endif

char *dt_cmstest_colord_filename(const char *output_name)
{
  char *filename = NULL;

#ifdef HAVE_COLORD
  if(!_colord_asked)
  {
    _colord_asked = TRUE;
    _colord = cd_client_new();
    if(IS_NULL_PTR(_colord) || !cd_client_connect_sync(_colord, NULL, NULL))
    {
      fprintf(stderr, "error connecting to colord\n");
      g_clear_object(&_colord);
    }
  }
  if(IS_NULL_PTR(_colord) || IS_NULL_PTR(output_name)) return NULL;

  CdDevice *device = cd_client_find_device_by_property_sync(_colord, CD_DEVICE_METADATA_XRANDR_NAME, output_name,
                                                            NULL, NULL);
  if(device && cd_device_connect_sync(device, NULL, NULL))
  {
    CdProfile *profile = cd_device_get_default_profile(device);
    if(profile)
    {
      if(cd_profile_connect_sync(profile, NULL, NULL))
      {
        CdIcc *icc = cd_profile_load_icc(profile, CD_ICC_LOAD_FLAGS_FALLBACK_MD5, NULL, NULL);
        if(icc)
        {
          filename = g_strdup(cd_icc_get_filename(icc));
          g_object_unref(icc);
        }
      }
      g_object_unref(profile);
    }
  }
  if(device) g_object_unref(device);
#endif // HAVE_COLORD

  return filename;
}

typedef enum dt_cmstest_session_t
{
  DT_CMSTEST_SESSION_AUTO = 0,
  DT_CMSTEST_SESSION_WAYLAND,
  DT_CMSTEST_SESSION_X11
} dt_cmstest_session_t;

static void _usage(FILE *stream)
{
  fprintf(stream, "usage: ansel-cmstest [--wayland | --x11]\n"
                  "  --wayland  examine the Wayland compositor\n"
                  "  --x11      examine the X11 display\n"
                  "Without an option, the one ansel itself would run on is examined: Wayland when a\n"
                  "compositor answers, unless GDK_BACKEND asks for x11.\n");
}

int main(int argc, char *argv[])
{
  dt_cmstest_session_t session = DT_CMSTEST_SESSION_AUTO;
  for(int i = 1; i < argc; i++)
  {
    if(!g_strcmp0(argv[i], "--wayland"))
      session = DT_CMSTEST_SESSION_WAYLAND;
    else if(!g_strcmp0(argv[i], "--x11"))
      session = DT_CMSTEST_SESSION_X11;
    else if(!g_strcmp0(argv[i], "--help") || !g_strcmp0(argv[i], "-h"))
    {
      _usage(stdout);
      return EXIT_SUCCESS;
    }
    else
    {
      _usage(stderr);
      return EXIT_FAILURE;
    }
  }

  printf("ansel-cmstest version %s\n", darktable_package_version);

#ifdef HAVE_COLORD
  printf("this executable was built with colord support enabled\n");
#else  // HAVE_COLORD
  printf("this executable was built without colord support\n");
#endif // HAVE_COLORD

#ifdef USE_COLORDGTK
  printf("ansel itself was built with colord support enabled\n");
#else
  printf("ansel itself was built without colord support\n");
#endif // USE_COLORDGTK

#ifdef HAVE_X11
  printf("this executable was built with X11 support enabled\n");
#else
  printf("this executable was built without X11 support\n");
#endif // HAVE_X11

#ifdef HAVE_WAYLAND
  printf("this executable was built with Wayland support enabled\n");
#else
  printf("this executable was built without Wayland support\n");
#endif // HAVE_WAYLAND

#ifdef HAVE_WAYLAND_COLOR
  printf("ansel itself was built with Wayland color management enabled\n");
#else
  printf("ansel itself was built without Wayland color management\n");
#endif // HAVE_WAYLAND_COLOR

  printf("\n");

  /* GDK takes Wayland when a compositor answers and X11 otherwise, and GDK_BACKEND overrides
   * that with a list of backends to try in order. Only its first entry is looked at here. */
  if(session == DT_CMSTEST_SESSION_AUTO)
  {
    const char *gdk_backend = g_getenv("GDK_BACKEND");
    if(!IS_NULL_PTR(gdk_backend) && g_str_has_prefix(gdk_backend, "x11"))
      session = DT_CMSTEST_SESSION_X11;
    else if(!IS_NULL_PTR(gdk_backend) && g_str_has_prefix(gdk_backend, "wayland"))
      session = DT_CMSTEST_SESSION_WAYLAND;
#ifdef HAVE_WAYLAND
    else if(dt_cmstest_wayland_available())
      session = DT_CMSTEST_SESSION_WAYLAND;
#endif
    else
      session = DT_CMSTEST_SESSION_X11;
  }

  int status = EXIT_FAILURE;
  if(session == DT_CMSTEST_SESSION_WAYLAND)
  {
#ifdef HAVE_WAYLAND
    status = dt_cmstest_wayland();
#else
    printf("this executable cannot examine a Wayland compositor\n");
#endif
  }
  else
  {
#ifdef HAVE_X11
    status = dt_cmstest_x11();
#else
    printf("this executable cannot examine an X11 display\n");
#endif
  }

#ifdef HAVE_COLORD
  g_clear_object(&_colord);
#endif

  return status;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
