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

/* The Wayland half of ansel-cmstest: for every output, how the compositor describes its colour
 * (the wp_color_management_v1 protocol, when the compositor has it) and the profile colord
 * holds, and what ansel makes of the two.
 *
 * ansel follows the description the compositor prefers for its main window, which needs a
 * window. This program opens none: it asks for the description of each output instead, which
 * is what a compositor prefers for a window shown on it.
 *
 * It has its own connection to the compositor, so unlike src/gui/wayland_color.c it may wait
 * for answers: every question below is followed by wl_display_roundtrip().
 *
 * See doc/wayland-color.md for what ansel does with all this.
 */

#include "apps/ansel-cmstest/cmstest.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include "color-management-v1-client-protocol.h"

#include <glib.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client-core.h>
#include <wayland-client-protocol.h>

typedef struct output_t
{
  struct wl_output *output;
  char *name;                   // the connector: "eDP-1". What colord files the monitor under.
  char *description;            // what the compositor calls the monitor
  char *make;
  char *model;

  struct wp_color_management_output_v1 *color_output;
  struct wp_image_description_v1 *image_description;
  struct wp_image_description_info_v1 *info;
  gboolean answered;            // the compositor described the output, or said it cannot
  char *failure;                // why it cannot

  unsigned char *icc_data;      // the description is this ICC profile...
  size_t icc_size;
  gboolean has_primaries;       // ... or these numbers
  int32_t primaries[8];         // red, green, blue, white: x then y, times 1e6
  uint32_t primaries_named;     // 0 when the compositor named none
  uint32_t tf_named;            // 0 when the compositor named none
  uint32_t tf_power;            // exponent times 10000; 0 when the curve is not a power law
  gboolean has_luminances;
  uint32_t min_lum;             // cd/m² times 10000
  uint32_t max_lum;             // cd/m²
  uint32_t reference_lum;       // cd/m²
  gboolean has_target_luminance;
  uint32_t target_max_lum;      // cd/m²
  uint32_t max_cll;             // cd/m²; 0 when not sent
} output_t;

typedef struct session_t
{
  struct wl_display *display;
  struct wp_color_manager_v1 *manager;
  uint32_t manager_version;     // the version the compositor has; this client speaks 1
  uint32_t features;            // one bit per wp_color_manager_v1.feature value
  uint32_t tf_named;            // one bit per transfer function a client may name
  uint32_t primaries_named;     // one bit per set of primaries a client may name
  GList *outputs;
  int pending;                  // outputs whose description is still on its way
} session_t;

/* Indexed by the values of the protocol's enumerations, version 1. */
static const char *const _feature_names[] = { "icc_v2_v4", "parametric", "set_primaries", "set_tf_power",
                                              "set_luminances", "set_mastering_display_primaries",
                                              "extended_target_volume", "windows_scrgb" };
static const char *const _tf_names[] = { NULL, "bt1886", "gamma22", "gamma28", "st240", "ext_linear", "log_100",
                                         "log_316", "xvycc", "srgb", "ext_srgb", "st2084_pq", "st428", "hlg" };
static const char *const _primaries_names[] = { NULL, "srgb", "pal_m", "pal", "ntsc", "generic_film", "bt2020",
                                                "cie1931_xyz", "dci_p3", "display_p3", "adobe_rgb" };

static const char *_name_of(const char *const names[], const size_t count, const uint32_t value)
{
  return (value < count && !IS_NULL_PTR(names[value])) ? names[value] : "(unknown)";
}

/* ------------------------------------------------------------------------------------------
 * What the compositor says about one description.
 */

static void _info_done(void *data, struct wp_image_description_info_v1 *info)
{
  output_t *output = data;
  // `done` is this object's destructor
  wp_image_description_info_v1_destroy(info);
  output->info = NULL;
  output->answered = TRUE;
}

static void _info_icc_file(void *data, struct wp_image_description_info_v1 *info, int32_t descriptor, uint32_t size)
{
  output_t *output = data;

  // read rather than mapped: a file shorter than announced is then a short read, not a SIGBUS
  if(size > 0 && size <= 64 * 1024 * 1024 && IS_NULL_PTR(output->icc_data))
  {
    unsigned char *buffer = g_malloc(size);
    size_t done = 0;
    while(done < size)
    {
      const ssize_t count = pread(descriptor, buffer + done, size - done, (off_t)done);
      if(count <= 0) break;
      done += (size_t)count;
    }

    if(done == size)
    {
      output->icc_data = buffer;
      output->icc_size = size;
    }
    else
    {
      dt_free(buffer);
    }
  }
  close(descriptor);
}

static void _info_primaries(void *data, struct wp_image_description_info_v1 *info, int32_t red_x, int32_t red_y,
                            int32_t green_x, int32_t green_y, int32_t blue_x, int32_t blue_y, int32_t white_x,
                            int32_t white_y)
{
  output_t *output = data;
  const int32_t primaries[8] = { red_x, red_y, green_x, green_y, blue_x, blue_y, white_x, white_y };
  memcpy(output->primaries, primaries, sizeof(primaries));
  output->has_primaries = TRUE;
}

static void _info_primaries_named(void *data, struct wp_image_description_info_v1 *info, uint32_t primaries)
{
  output_t *output = data;
  output->primaries_named = primaries;
}

static void _info_tf_power(void *data, struct wp_image_description_info_v1 *info, uint32_t exponent)
{
  output_t *output = data;
  output->tf_power = exponent;
}

static void _info_tf_named(void *data, struct wp_image_description_info_v1 *info, uint32_t tf_named)
{
  output_t *output = data;
  output->tf_named = tf_named;
}

static void _info_luminances(void *data, struct wp_image_description_info_v1 *info, uint32_t min_lum,
                             uint32_t max_lum, uint32_t reference_lum)
{
  output_t *output = data;
  output->min_lum = min_lum;
  output->max_lum = max_lum;
  output->reference_lum = reference_lum;
  output->has_luminances = TRUE;
}

static void _info_target_primaries(void *data, struct wp_image_description_info_v1 *info, int32_t red_x,
                                   int32_t red_y, int32_t green_x, int32_t green_y, int32_t blue_x, int32_t blue_y,
                                   int32_t white_x, int32_t white_y)
{
  // the volume the monitor can show, as opposed to the one its signal is encoded in: not reported
}

static void _info_target_luminance(void *data, struct wp_image_description_info_v1 *info, uint32_t min_lum,
                                   uint32_t max_lum)
{
  output_t *output = data;
  output->target_max_lum = max_lum;
  output->has_target_luminance = TRUE;
}

static void _info_target_max_cll(void *data, struct wp_image_description_info_v1 *info, uint32_t max_cll)
{
  output_t *output = data;
  output->max_cll = max_cll;
}

static void _info_target_max_fall(void *data, struct wp_image_description_info_v1 *info, uint32_t max_fall)
{
  // the frame average of HDR content: not reported
}

static const struct wp_image_description_info_v1_listener _info_listener = {
  .done = _info_done,
  .icc_file = _info_icc_file,
  .primaries = _info_primaries,
  .primaries_named = _info_primaries_named,
  .tf_power = _info_tf_power,
  .tf_named = _info_tf_named,
  .luminances = _info_luminances,
  .target_primaries = _info_target_primaries,
  .target_luminance = _info_target_luminance,
  .target_max_cll = _info_target_max_cll,
  .target_max_fall = _info_target_max_fall,
};

static void _description_failed(void *data, struct wp_image_description_v1 *description, uint32_t cause,
                                const char *message)
{
  output_t *output = data;
  output->failure = g_strdup(message);
  output->answered = TRUE;
}

static void _description_ready(void *data, struct wp_image_description_v1 *description, uint32_t identity)
{
  output_t *output = data;
  output->info = wp_image_description_v1_get_information(description);
  wp_image_description_info_v1_add_listener(output->info, &_info_listener, output);
}

static const struct wp_image_description_v1_listener _description_listener = {
  .failed = _description_failed,
  .ready = _description_ready,
};

static void _color_output_changed(void *data, struct wp_color_management_output_v1 *color_output)
{
  // the report is of one moment: a description that changes while it is being read is not followed
}

static const struct wp_color_management_output_v1_listener _color_output_listener = {
  .image_description_changed = _color_output_changed,
};

/* ------------------------------------------------------------------------------------------
 * Outputs and the colour manager, as the registry announces them.
 */

static void _output_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width,
                             int32_t physical_height, int32_t subpixel, const char *make, const char *model,
                             int32_t transform)
{
  output_t *output = data;
  dt_free(output->make);
  dt_free(output->model);
  output->make = g_strdup(make);
  output->model = g_strdup(model);
}

static void _output_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height,
                         int32_t refresh)
{
}

static void _output_done(void *data, struct wl_output *wl_output)
{
}

static void _output_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
}

static void _output_name(void *data, struct wl_output *wl_output, const char *name)
{
  output_t *output = data;
  dt_free(output->name);
  output->name = g_strdup(name);
}

static void _output_description(void *data, struct wl_output *wl_output, const char *description)
{
  output_t *output = data;
  dt_free(output->description);
  output->description = g_strdup(description);
}

static const struct wl_output_listener _output_listener = {
  .geometry = _output_geometry,
  .mode = _output_mode,
  .done = _output_done,
  .scale = _output_scale,
  .name = _output_name,
  .description = _output_description,
};

static void _manager_supported_intent(void *data, struct wp_color_manager_v1 *manager, uint32_t render_intent)
{
}

static void _manager_supported_feature(void *data, struct wp_color_manager_v1 *manager, uint32_t feature)
{
  session_t *session = data;
  if(feature < 32) session->features |= 1u << feature;
}

static void _manager_supported_tf_named(void *data, struct wp_color_manager_v1 *manager, uint32_t tf_named)
{
  session_t *session = data;
  if(tf_named < 32) session->tf_named |= 1u << tf_named;
}

static void _manager_supported_primaries_named(void *data, struct wp_color_manager_v1 *manager, uint32_t primaries)
{
  session_t *session = data;
  if(primaries < 32) session->primaries_named |= 1u << primaries;
}

static void _manager_done(void *data, struct wp_color_manager_v1 *manager)
{
}

static const struct wp_color_manager_v1_listener _manager_listener = {
  .supported_intent = _manager_supported_intent,
  .supported_feature = _manager_supported_feature,
  .supported_tf_named = _manager_supported_tf_named,
  .supported_primaries_named = _manager_supported_primaries_named,
  .done = _manager_done,
};

static void _registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface,
                             uint32_t version)
{
  session_t *session = data;

  if(!g_strcmp0(interface, wl_output_interface.name))
  {
    output_t *output = g_new0(output_t, 1);
    // version 4 is the one that gives an output its name; older compositors give what they have
    output->output = wl_registry_bind(registry, name, &wl_output_interface, MIN(version, 4));
    wl_output_add_listener(output->output, &_output_listener, output);
    session->outputs = g_list_append(session->outputs, output);
  }
  else if(!g_strcmp0(interface, wp_color_manager_v1_interface.name) && IS_NULL_PTR(session->manager))
  {
    // version 1, as ansel: a compositor only sends a client the events of the version it bound
    session->manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1);
    session->manager_version = version;
    wp_color_manager_v1_add_listener(session->manager, &_manager_listener, session);
  }
}

static void _registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener _registry_listener = {
  .global = _registry_global,
  .global_remove = _registry_global_remove,
};

/* ------------------------------------------------------------------------------------------
 * The report.
 */

/* Does the compositor show a window that declares nothing as it is on this output? It reads
 * such a window as sRGB, so it does when the output is itself described as sRGB: its primaries,
 * and the 2.2 power or the sRGB curve. Anything else -- another gamut, an ICC profile, PQ --
 * and the compositor converts. */
static gboolean _is_srgb(const output_t *output)
{
  if(!IS_NULL_PTR(output->icc_data) || !output->has_primaries) return FALSE;

  const gboolean curve = output->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22
                         || output->tf_named == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB
                         || (output->tf_named == 0 && output->tf_power == 22000);
  if(!curve) return FALSE;
  if(output->primaries_named == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) return TRUE;

  // a monitor's own primaries are never sRGB's to the last digit: 0.002 is well inside the difference
  // between sRGB and any other gamut a monitor is sold as
  static const int32_t srgb[8] = { 640000, 330000, 300000, 600000, 150000, 60000, 312700, 329000 };
  for(int k = 0; k < 8; k++)
  {
    if(abs(output->primaries[k] - srgb[k]) > 2000) return FALSE;
  }
  return TRUE;
}

static void _print_capabilities(const session_t *session)
{
  if(IS_NULL_PTR(session->manager))
  {
    printf("the compositor does not manage color: no wp_color_manager_v1\n");
    printf("\twindows are shown as they are, and ansel relies on colord for the monitor profile\n");
    return;
  }

  printf("the compositor manages color: wp_color_manager_v1 version %" PRIu32 "\n", session->manager_version);
  printf("\ta client may describe its windows with:");
  for(uint32_t k = 0; k < G_N_ELEMENTS(_feature_names); k++)
  {
    if(session->features & (1u << k)) printf(" %s", _feature_names[k]);
  }
  printf("\n\tnamed transfer functions:");
  for(uint32_t k = 1; k < G_N_ELEMENTS(_tf_names); k++)
  {
    if(session->tf_named & (1u << k)) printf(" %s", _tf_names[k]);
  }
  printf("\n\tnamed primaries:");
  for(uint32_t k = 1; k < G_N_ELEMENTS(_primaries_names); k++)
  {
    if(session->primaries_named & (1u << k)) printf(" %s", _primaries_names[k]);
  }
  printf("\n");
}

static void _print_description(const output_t *output)
{
  if(!IS_NULL_PTR(output->icc_data))
  {
    char *description = dt_cmstest_profile_description(output->icc_data, output->icc_size);
    printf("\tcompositor:\tICC profile (%" G_GSIZE_FORMAT " bytes)\n\t\tdescription: %s\n", output->icc_size,
           description ? description : "(none)");
    dt_free(description);
    return;
  }

  if(!output->has_primaries)
  {
    printf("\tcompositor:\t(no description%s%s)\n", output->failure ? ": " : "",
           output->failure ? output->failure : "");
    return;
  }

  const int32_t *coords = output->primaries;
  printf("\tcompositor:\tprimaries and transfer function\n");
  printf("\t\tprimaries: red %.4f %.4f, green %.4f %.4f, blue %.4f %.4f, white %.4f %.4f", coords[0] / 1e6,
         coords[1] / 1e6, coords[2] / 1e6, coords[3] / 1e6, coords[4] / 1e6, coords[5] / 1e6, coords[6] / 1e6,
         coords[7] / 1e6);
  if(output->primaries_named)
    printf(" (%s)", _name_of(_primaries_names, G_N_ELEMENTS(_primaries_names), output->primaries_named));
  printf("\n");

  if(output->tf_named)
    printf("\t\ttransfer function: %s\n", _name_of(_tf_names, G_N_ELEMENTS(_tf_names), output->tf_named));
  else
    printf("\t\ttransfer function: power %.4f\n", output->tf_power / 10000.0);

  if(output->has_luminances)
    printf("\t\tluminances: min %.4f, max %" PRIu32 ", reference %" PRIu32 " cd/m2\n", output->min_lum / 10000.0,
           output->max_lum, output->reference_lum);
  if(output->has_target_luminance)
    printf("\t\ttarget: max %" PRIu32 " cd/m2\n", output->target_max_lum);
  if(output->max_cll) printf("\t\tmax content light level: %" PRIu32 " cd/m2\n", output->max_cll);
}

gboolean dt_cmstest_wayland_available(void)
{
  struct wl_display *display = wl_display_connect(NULL);
  if(IS_NULL_PTR(display)) return FALSE;
  wl_display_disconnect(display);
  return TRUE;
}

int dt_cmstest_wayland(void)
{
  session_t session = { 0 };

  session.display = wl_display_connect(NULL);
  if(IS_NULL_PTR(session.display))
  {
    const char *socket = g_getenv("WAYLAND_DISPLAY");
    fprintf(stderr, "can't connect to the Wayland compositor `%s'\n", socket ? socket : "wayland-0");
    return EXIT_FAILURE;
  }

  // the globals, then what the outputs and the colour manager say about themselves
  struct wl_registry *registry = wl_display_get_registry(session.display);
  wl_registry_add_listener(registry, &_registry_listener, &session);
  wl_display_roundtrip(session.display);
  wl_display_roundtrip(session.display);

  // the description of each output, when there is someone to ask
  if(!IS_NULL_PTR(session.manager))
  {
    for(GList *iter = session.outputs; iter; iter = g_list_next(iter))
    {
      output_t *output = iter->data;
      output->color_output = wp_color_manager_v1_get_output(session.manager, output->output);
      wp_color_management_output_v1_add_listener(output->color_output, &_color_output_listener, output);
      output->image_description = wp_color_management_output_v1_get_image_description(output->color_output);
      wp_image_description_v1_add_listener(output->image_description, &_description_listener, output);
    }

    /* `ready`, then the information, then `done`: one round trip each at most. The bound is for
     * a compositor that never answers; a lost connection ends the loop by itself. */
    for(int attempt = 0; attempt < 16; attempt++)
    {
      session.pending = 0;
      for(GList *iter = session.outputs; iter; iter = g_list_next(iter))
      {
        const output_t *output = iter->data;
        if(!output->answered) session.pending++;
      }
      if(session.pending == 0) break;
      if(wl_display_roundtrip(session.display) < 0) break;
    }
  }

  const char *socket = g_getenv("WAYLAND_DISPLAY");
  printf("Wayland compositor on `%s'\n", socket ? socket : "wayland-0");
  _print_capabilities(&session);

  gboolean any_unprofiled_monitor = FALSE;
  gboolean any_double_correction = FALSE;
  gboolean any_srgb_only = FALSE;

  if(IS_NULL_PTR(session.outputs)) printf("\nthe compositor announced no output\n");

  for(GList *iter = session.outputs; iter; iter = g_list_next(iter))
  {
    const output_t *output = iter->data;

    char *colord_filename = dt_cmstest_colord_filename(output->name);
    unsigned char *colord_data = NULL;
    size_t colord_size = 0;
    if(!IS_NULL_PTR(colord_filename))
      g_file_get_contents(colord_filename, (gchar **)&colord_data, &colord_size, NULL);
    const gboolean colord_has_profile = colord_size > 0;

    const gboolean described = !IS_NULL_PTR(output->icc_data) || output->has_primaries;

    // what ansel does with the default `all` for its display profile source: see doc/wayland-color.md
    const char *message = NULL;
    if(colord_has_profile && described && !_is_srgb(output))
    {
      message = "ansel uses the profile from colord, and the compositor converts on top of it";
      any_double_correction = TRUE;
    }
    else if(colord_has_profile)
    {
      message = "ansel uses the profile from colord";
    }
    else if(described)
    {
#ifdef HAVE_WAYLAND_COLOR
      message = "ansel renders in the compositor's description of this output";
#else
      message = "ansel renders in sRGB and the compositor converts to this output";
      any_srgb_only = TRUE;
#endif
    }
    else
    {
      message = "nothing describes this monitor";
      any_unprofiled_monitor = TRUE;
    }

    // print it
    printf("\n%s\t%s\n", output->name ? output->name : "(unnamed output)", message);
    // a compositor may send any of the three empty
    const char *description = (output->description && *output->description) ? output->description : NULL;
    const char *make = (output->make && *output->make) ? output->make : NULL;
    const char *model = (output->model && *output->model) ? output->model : NULL;
    if(description || make || model)
      printf("\tmonitor:\t%s%s%s%s%s\n", description ? description : "",
             (description && (make || model)) ? " -- " : "", make ? make : "", (make && model) ? " " : "",
             model ? model : "");
    if(!IS_NULL_PTR(session.manager)) _print_description(output);

    char *colord_description = dt_cmstest_profile_description(colord_data, colord_size);
    printf("\tcolord:\t\"%s\"\n\t\tdescription: %s\n", colord_filename ? colord_filename : "(none)",
           colord_description ? colord_description : (colord_filename ? "(file not found)" : "(none)"));

    dt_free(colord_description);
    dt_free(colord_data);
    dt_free(colord_filename);
  }

  // conclusion
  if(any_double_correction || any_unprofiled_monitor)
  {
    printf("\nBetter check your system setup\n");
    if(any_double_correction)
      printf(" - some monitors have a profile in colord while the compositor already converts to them:\n"
             "   color is corrected twice. Remove the profile from colord, or set ansel's\n"
             "   \"method to use for getting the display profile\" to \"wayland\"\n");
    if(any_unprofiled_monitor)
      printf(" - some monitors lacked a profile: ansel treats them as sRGB\n");
    printf("You may experience inconsistent color rendition between color managed applications\n");
  }
  else
  {
    printf("\nYour system seems to be correctly configured\n");
    if(any_srgb_only)
      printf("ansel was built without Wayland color management: it is limited to the sRGB gamut here\n");
  }

  // cleanup
  for(GList *iter = session.outputs; iter; iter = g_list_next(iter))
  {
    output_t *output = iter->data;
    if(!IS_NULL_PTR(output->info)) wp_image_description_info_v1_destroy(output->info);
    if(!IS_NULL_PTR(output->image_description)) wp_image_description_v1_destroy(output->image_description);
    if(!IS_NULL_PTR(output->color_output)) wp_color_management_output_v1_destroy(output->color_output);
    wl_output_destroy(output->output);
    dt_free(output->name);
    dt_free(output->description);
    dt_free(output->make);
    dt_free(output->model);
    dt_free(output->failure);
    dt_free(output->icc_data);
  }
  g_list_free_full(session.outputs, dt_free_gpointer);
  if(!IS_NULL_PTR(session.manager)) wp_color_manager_v1_destroy(session.manager);
  wl_registry_destroy(registry);
  wl_display_disconnect(session.display);

  return EXIT_SUCCESS;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
