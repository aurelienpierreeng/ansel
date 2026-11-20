/*
 *    This file is part of Ansel,
 *    Copyright (C) 2026 Champions007.
 *    Copyright (C) 2026 Aurélien PIERRE.
 *
 *    Ansel is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    Ansel is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Colour management with a Wayland compositor. What it is for is in the header; how it was
 * arrived at, compositor by compositor, is in doc/wayland-color.md.
 *
 * Everything here runs on the GUI thread: the protocol objects are created on GDK's own
 * connection, so GDK's event source is what dispatches their events, from the main loop.
 * Nothing waits for the compositor -- no wl_display_roundtrip() -- and nothing commits a
 * surface: the surfaces are GDK's, and a description set on one is pending state that GDK
 * commits with its next frame.
 *
 * The order of events for one description, each step an answer to the previous one:
 *
 *   preferred_changed          the compositor prefers something else for the main window
 *   get_preferred  -> ready    it is ready to be asked about
 *   get_information -> done    its primaries, tone curve and luminances, or its ICC file
 *   (create -> ready)          a description of our own, when the preferred one says more
 *                              than an SDR window may claim: from the ICC profile itself
 *                              where the compositor takes profiles, from numbers otherwise
 *   offer + declare            the profile goes to colorprofiles/, the description to the
 *                              windows if the profile was taken
 */

#include "gui/wayland_color.h"

#include "colorprofiles/colorspaces.h"
#include "common/logging.h"
#include "control/signal.h"
#include "system/macros.h"
#include "system/mem_alloc.h"

#include "color-management-v1-client-protocol.h"

#include <gdk/gdkwayland.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client-protocol.h>

/* What the compositor says about one description, between get_information and `done`. */
typedef struct dt_wayland_description_t
{
  uint8_t *icc_data;            // the ICC file, copied; NULL when the description is parametric
  size_t icc_size;
  gboolean has_primaries;
  int32_t primaries[8];         // red, green, blue, white: x then y, times 1e6
  uint32_t primaries_named;     // 0 when the compositor named none
  uint32_t tf_named;            // 0 when the compositor named none
  uint32_t tf_power;            // exponent times 10000; 0 when the curve is not a power law
  gboolean has_luminances;
  uint32_t min_lum;             // cd/m² times 10000
  uint32_t max_lum;             // cd/m²
  uint32_t reference_lum;       // cd/m²
  gboolean has_target_primaries;
  int32_t target_primaries[8];
  gboolean has_target_luminance;
  uint32_t target_max_lum;      // cd/m²
  uint32_t max_cll;             // cd/m²; 0 when not sent
} dt_wayland_description_t;

/* The tone curve the windows are encoded with, and how the compositor is told about it. */
typedef struct dt_wayland_curve_t
{
  double gamma;                 // exponent of the power law
  gboolean srgb_curve;          // the sRGB piecewise curve instead
  uint32_t tf_named;            // said as a named function, or, when 0, ...
  uint32_t tf_power;            // ... as an exponent times 10000
} dt_wayland_curve_t;

/* One description on its way: from get_preferred to the offer, or to wherever it stops. */
typedef struct dt_wayland_request_t
{
  struct wp_image_description_v1 *preferred;    // what get_preferred returned
  struct wp_image_description_info_v1 *info;    // its information, while it arrives
  struct wp_image_description_v1 *created;      // our own description, while the compositor builds it
  dt_wayland_description_t description;
  uint8_t *profile;                             // the ICC profile that goes with it
  size_t profile_size;
} dt_wayland_request_t;

/* What is kept on a window while it is mapped. */
typedef struct dt_wayland_window_t
{
  struct wp_color_management_surface_v1 *color_surface;
  struct wl_surface *surface;                   // the surface it was made for
  guint declared_serial;                        // which description it was told about; 0 for none
} dt_wayland_window_t;

typedef struct dt_wayland_color_t
{
  struct wl_registry *registry;
  struct wp_color_manager_v1 *manager;
  gboolean manager_ready;       // its `done` came: the three capability sets below are complete
  uint32_t features;            // one bit per wp_color_manager_v1.feature value
  uint32_t tf_named;            // one bit per transfer function the parametric creator takes
  uint32_t primaries_named;     // one bit per named primaries it takes

  GtkWidget *main_window;       // the window whose preferred description is followed
  struct wp_color_management_surface_feedback_v1 *feedback;
  GSList *requests;             // every request still waiting for an event
  dt_wayland_request_t *latest; // the only one whose outcome counts

  /* The description the windows are declared in, the profile encoding the same thing, and
   * what the compositor said that produced them. */
  struct wp_image_description_v1 *declared;
  dt_wayland_description_t described;
  uint8_t *profile;
  size_t profile_size;
  guint serial;                 // changes each time `declared` is replaced; never 0
  gboolean declaring;           // the windows are told about `declared`, as opposed to nothing

  gulong map_hook;
  gulong unmap_hook;
} dt_wayland_color_t;

static dt_wayland_color_t _wayland = { 0 };

static const char *const _window_key = "dt-wayland-color";

static void _request_preferred(void);
static void _declare_everywhere(void);

/* The compositor announces what it supports one enumeration value at a time. Each of the three
 * lists is kept as a bit set: no value of protocol version 1 reaches 32. */
static gboolean _announced(const uint32_t announced, const uint32_t value)
{
  return value < 32 && (announced & (1u << value));
}

static void _announce(uint32_t *const announced, const uint32_t value)
{
  if(value < 32) *announced |= 1u << value;
}

/* ------------------------------------------------------------------------------------------
 * Windows. Every GtkWindow that is mapped carries a colour-management object for its surface.
 */

/* The surface GDK made for a mapped toplevel or popup; NULL for anything else. An offscreen
 * GtkWindow has none, and its GdkWindow is not backed by the Wayland implementation at all. */
static struct wl_surface *_window_surface(GtkWidget *window)
{
  if(!gtk_widget_get_mapped(window)) return NULL;

  GdkWindow *gdk_window = gtk_widget_get_window(window);
  if(IS_NULL_PTR(gdk_window) || !GDK_IS_WAYLAND_WINDOW(gdk_window)) return NULL;

  const GdkWindowType type = gdk_window_get_window_type(gdk_window);
  if(type != GDK_WINDOW_TOPLEVEL && type != GDK_WINDOW_TEMP) return NULL;

  return gdk_wayland_window_get_wl_surface(gdk_window);
}

static void _window_free(gpointer data)
{
  dt_wayland_window_t *tracked = data;
  // legal on an object whose surface is gone, unlike every other request
  wp_color_management_surface_v1_destroy(tracked->color_surface);
  dt_free(tracked);
}

/* Tell one surface what its pixels are encoded in, or that it is no longer told anything.
 * Either is pending state on the surface; the redraw is what makes GDK commit it. */
static void _window_declare(GtkWidget *window, dt_wayland_window_t *tracked)
{
  const guint wanted = _wayland.declaring ? _wayland.serial : 0;
  if(tracked->declared_serial == wanted) return;

  if(wanted)
    wp_color_management_surface_v1_set_image_description(tracked->color_surface, _wayland.declared,
                                                         WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
  else
    wp_color_management_surface_v1_unset_image_description(tracked->color_surface);

  tracked->declared_serial = wanted;
  gtk_widget_queue_draw(window);
}

static void _feedback_preferred_changed(void *data, struct wp_color_management_surface_feedback_v1 *feedback,
                                        uint32_t identity)
{
  if(feedback != _wayland.feedback) return;
  _request_preferred();
}

static const struct wp_color_management_surface_feedback_v1_listener _feedback_listener = {
  .preferred_changed = _feedback_preferred_changed,
};

static void _window_attach(GtkWidget *window)
{
  if(!_wayland.manager_ready) return;
  // one object per surface: a second one is a protocol error, which closes the connection
  if(!IS_NULL_PTR(g_object_get_data(G_OBJECT(window), _window_key))) return;

  struct wl_surface *surface = _window_surface(window);
  if(IS_NULL_PTR(surface)) return;

  dt_wayland_window_t *tracked = g_new0(dt_wayland_window_t, 1);
  tracked->surface = surface;
  tracked->color_surface = wp_color_manager_v1_get_surface(_wayland.manager, surface);
  g_object_set_data_full(G_OBJECT(window), _window_key, tracked, _window_free);
  _window_declare(window, tracked);

  if(window == _wayland.main_window && IS_NULL_PTR(_wayland.feedback))
  {
    _wayland.feedback = wp_color_manager_v1_get_surface_feedback(_wayland.manager, surface);
    wp_color_management_surface_feedback_v1_add_listener(_wayland.feedback, &_feedback_listener, NULL);
    _request_preferred();
  }
}

/* The window is unmapped, so GDK has destroyed its surface, and what was attached to it can
 * only be destroyed too. No compositor event is dispatched between the two: both happen
 * inside the same emission of GtkWidget::unmap. */
static void _window_detach(GtkWidget *window)
{
  if(window == _wayland.main_window && !IS_NULL_PTR(_wayland.feedback))
  {
    wp_color_management_surface_feedback_v1_destroy(_wayland.feedback);
    _wayland.feedback = NULL;
  }
  g_object_set_data(G_OBJECT(window), _window_key, NULL);
}

/* GtkWidget::map and ::unmap of every widget of the process come through these two. That is
 * the one place where a window GTK creates on its own -- a menu, a tooltip, a drag icon --
 * can be caught, and it is what makes them agree with the main window. Both signals run
 * their class handler first, so GDK has already made or destroyed the surface. */
static gboolean _map_hook(GSignalInvocationHint *hint, guint n_values, const GValue *values, gpointer user_data)
{
  GObject *object = g_value_get_object(&values[0]);
  if(GTK_IS_WINDOW(object)) _window_attach(GTK_WIDGET(object));
  return TRUE;
}

static gboolean _unmap_hook(GSignalInvocationHint *hint, guint n_values, const GValue *values, gpointer user_data)
{
  GObject *object = g_value_get_object(&values[0]);
  if(GTK_IS_WINDOW(object)) _window_detach(GTK_WIDGET(object));
  return TRUE;
}

/* Does GDK map this window as a popup? Its own rule (should_map_as_popup() in
 * gdkwindow-wayland.c) is the window type and these three type hints. */
static gboolean _is_popup(GtkWindow *window)
{
  if(gtk_window_get_window_type(window) != GTK_WINDOW_TOPLEVEL) return TRUE;

  const GdkWindowTypeHint hint = gtk_window_get_type_hint(window);
  return hint == GDK_WINDOW_TYPE_HINT_POPUP_MENU
         || hint == GDK_WINDOW_TYPE_HINT_DROPDOWN_MENU
         || hint == GDK_WINDOW_TYPE_HINT_COMBO;
}

/* Bring every window in line with what colorprofiles/ says the pixels are encoded for.
 *
 * Popups are told once, when they are mapped, and never again. GDK destroys and recreates a
 * popup's surface on its own -- when the compositor dismisses it, when it is resized before
 * its first configure -- without the widget being unmapped, and a request on the object of a
 * dead surface is a protocol error. A popup does not live long enough to need a second word. */
static void _declare_everywhere(void)
{
  _wayland.declaring = !IS_NULL_PTR(_wayland.declared) && dt_colorspaces_display_follows_compositor();

  GList *toplevels = gtk_window_list_toplevels();
  for(GList *iter = toplevels; iter; iter = g_list_next(iter))
  {
    GtkWidget *window = GTK_WIDGET(iter->data);
    if(_is_popup(GTK_WINDOW(window))) continue;

    dt_wayland_window_t *tracked = g_object_get_data(G_OBJECT(window), _window_key);
    if(IS_NULL_PTR(tracked)) continue;

    if(tracked->surface != _window_surface(window))
    {
      // not the surface the object was made for: start over with the one there is
      g_object_set_data(G_OBJECT(window), _window_key, NULL);
      _window_attach(window);
      continue;
    }
    _window_declare(window, tracked);
  }
  g_list_free(toplevels);
}

static void _profile_changed(gpointer instance, gpointer user_data)
{
  _declare_everywhere();
}

static void _profile_user_changed(gpointer instance, int type, gpointer user_data)
{
  if(type == DT_COLORSPACES_PROFILE_TYPE_DISPLAY) _declare_everywhere();
}

/* ------------------------------------------------------------------------------------------
 * Requests.
 */

static void _request_free(dt_wayland_request_t *request)
{
  if(!IS_NULL_PTR(request->info)) wp_image_description_info_v1_destroy(request->info);
  if(!IS_NULL_PTR(request->created)) wp_image_description_v1_destroy(request->created);
  if(!IS_NULL_PTR(request->preferred)) wp_image_description_v1_destroy(request->preferred);
  dt_free(request->description.icc_data);
  dt_free(request->profile);

  _wayland.requests = g_slist_remove(_wayland.requests, request);
  if(_wayland.latest == request) _wayland.latest = NULL;
  dt_free(request);
}

/* The request went all the way: `description` is ready and says what `request->profile`
 * encodes. Both replace what the windows were declared in. The previous description can be
 * destroyed at once -- a surface keeps a copy of what it was told. */
static void _request_succeeded(dt_wayland_request_t *request, struct wp_image_description_v1 **description)
{
  if(!IS_NULL_PTR(_wayland.declared)) wp_image_description_v1_destroy(_wayland.declared);
  _wayland.declared = *description;
  *description = NULL;

  dt_free(_wayland.profile);
  _wayland.profile = request->profile;
  _wayland.profile_size = request->profile_size;
  request->profile = NULL;

  memcpy(&_wayland.described, &request->description, sizeof(_wayland.described));
  _wayland.described.icc_data = NULL;

  _wayland.serial++;
  if(_wayland.serial == 0) _wayland.serial = 1;

  _request_free(request);

  /* The profile first, the windows second, and nothing in between that could draw: when the
   * offer is taken, the changed handler it fires only queues work. */
  dt_colorspaces_offer_compositor_display_profile(_wayland.profile, _wayland.profile_size);
  _declare_everywhere();
}

static void _created_failed(void *data, struct wp_image_description_v1 *description, uint32_t cause,
                            const char *message)
{
  dt_print(DT_DEBUG_CONTROL, "[wayland color] the compositor refused the description of the windows: %s (cause %" PRIu32 ")\n",
           message, cause);
  _request_free(data);
}

static void _created_ready(void *data, struct wp_image_description_v1 *description, uint32_t identity)
{
  dt_wayland_request_t *request = data;
  if(request != _wayland.latest)
  {
    _request_free(request);
    return;
  }
  _request_succeeded(request, &request->created);
}

static const struct wp_image_description_v1_listener _created_listener = {
  .failed = _created_failed,
  .ready = _created_ready,
};

/* The tone curve of the description, when it is one an 8-bit window can be encoded with. */
static gboolean _mirror_curve(const dt_wayland_description_t *description, dt_wayland_curve_t *curve)
{
  memset(curve, 0, sizeof(*curve));
  curve->tf_named = description->tf_named;

  switch(description->tf_named)
  {
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
      curve->gamma = 2.2;
      return TRUE;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28:
      curve->gamma = 2.8;
      return TRUE;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
      curve->gamma = 2.4;
      return TRUE;
    case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_SRGB:
      curve->srgb_curve = TRUE;
      return TRUE;
    case 0:
      break;
    default:
      // PQ, HLG, linear light, a logarithm
      return FALSE;
  }

  // the protocol's own bounds for an exponent
  if(description->tf_power < 10000 || description->tf_power > 100000) return FALSE;
  curve->gamma = description->tf_power / 10000.0;
  curve->tf_power = description->tf_power;
  return TRUE;
}

/* Can the parametric creator be told this curve? A named function it does not list is said
 * as an exponent when it is a power law and the compositor takes exponents. */
static gboolean _curve_sayable(dt_wayland_curve_t *curve)
{
  if(curve->tf_named && _announced(_wayland.tf_named, curve->tf_named)) return TRUE;
  if(curve->srgb_curve) return FALSE;
  if(!_announced(_wayland.features, WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER)) return FALSE;

  curve->tf_named = 0;
  curve->tf_power = (uint32_t)lround(curve->gamma * 10000.0);
  return TRUE;
}

/* Is the preferred description, as it is, what an SDR window encoded with its primaries and
 * its tone curve contains? The protocol asks for it to be set on a surface only then. It is
 * not as soon as it announces anything brighter than its reference white, or a target volume
 * other than the one its pixels are encoded in: both are claims about HDR content, and a
 * compositor acts on them -- KWin raises a laptop's backlight to make room for the highlights. */
static gboolean _is_exact_match(const dt_wayland_description_t *description)
{
  if(!description->has_luminances) return FALSE;
  if(description->max_lum > description->reference_lum) return FALSE;
  if(description->has_target_luminance && description->target_max_lum > description->max_lum) return FALSE;
  if(description->max_cll > description->max_lum) return FALSE;
  if(description->has_target_primaries
     && memcmp(description->target_primaries, description->primaries, sizeof(description->primaries)))
    return FALSE;
  return TRUE;
}

/* Ask the compositor for a description made from the very profile the windows are encoded
 * with. Nothing can then differ between what is declared and what is sent, and an ICC profile
 * makes no claim about luminance. FALSE when it could not be asked for.
 *
 * This is preferred to the parametric creator wherever it exists, and on Weston it is not a
 * preference: Weston 15.0.1 announces the parametric creator, answers `ready`, then closes
 * the connection ("no memory") on the first commit of a surface carrying the result -- even a
 * description equal to its own output's. See doc/wayland-color.md. */
static gboolean _create_description_from_profile(dt_wayland_request_t *request)
{
  // the protocol's bound on a profile; a larger one is a protocol error
  if(request->profile_size == 0 || request->profile_size > 32 * 1024 * 1024) return FALSE;

  // the compositor wants a descriptor it can seek and read: an unlinked temporary file
  gchar *path = NULL;
  const int descriptor = g_file_open_tmp("ansel-display-profile-XXXXXX", &path, NULL);
  if(descriptor < 0) return FALSE;
  g_unlink(path);
  dt_free(path);

  size_t done = 0;
  while(done < request->profile_size)
  {
    const ssize_t count = write(descriptor, request->profile + done, request->profile_size - done);
    if(count <= 0) break;
    done += (size_t)count;
  }
  if(done != request->profile_size)
  {
    close(descriptor);
    return FALSE;
  }

  struct wp_image_description_creator_icc_v1 *creator = wp_color_manager_v1_create_icc_creator(_wayland.manager);
  wp_image_description_creator_icc_v1_set_icc_file(creator, descriptor, 0, (uint32_t)request->profile_size);
  // `create` destroys the creator
  request->created = wp_image_description_creator_icc_v1_create(creator);
  wp_image_description_v1_add_listener(request->created, &_created_listener, request);

  // the request carries a duplicate of the descriptor
  close(descriptor);
  return TRUE;
}

/* Ask the compositor for a description of our own: the preferred primaries, `curve`, and
 * nothing brighter than the reference white. FALSE when it cannot be asked for, in which case
 * nothing was sent. Every request below is a protocol error when the compositor did not
 * announce the capability it needs, hence the checks before the first of them. */
static gboolean _create_description(dt_wayland_request_t *request, const dt_wayland_curve_t *curve)
{
  const dt_wayland_description_t *description = &request->description;

  if(!_announced(_wayland.features, WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC)) return FALSE;

  const gboolean named_primaries = description->primaries_named
                                   && _announced(_wayland.primaries_named, description->primaries_named);
  if(!named_primaries && !_announced(_wayland.features, WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES)) return FALSE;

  struct wp_image_description_creator_params_v1 *creator
      = wp_color_manager_v1_create_parametric_creator(_wayland.manager);

  if(named_primaries)
    wp_image_description_creator_params_v1_set_primaries_named(creator, description->primaries_named);
  else
    wp_image_description_creator_params_v1_set_primaries(creator,
                                                         description->primaries[0], description->primaries[1],
                                                         description->primaries[2], description->primaries[3],
                                                         description->primaries[4], description->primaries[5],
                                                         description->primaries[6], description->primaries[7]);

  if(curve->tf_named)
    wp_image_description_creator_params_v1_set_tf_named(creator, curve->tf_named);
  else
    wp_image_description_creator_params_v1_set_tf_power(creator, curve->tf_power);

  /* The window's white is the reference white, and the black level is the compositor's own.
   * On an SDR output that is the preferred description again, so the compositor has nothing
   * to convert. The protocol wants both luminances above the minimum, which it counts in
   * 1/10000 cd/m². Without this request the defaults are those of an sRGB monitor. */
  if(_announced(_wayland.features, WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES)
     && description->has_luminances
     && (uint64_t)description->reference_lum * 10000 > description->min_lum)
    wp_image_description_creator_params_v1_set_luminances(creator, description->min_lum,
                                                          description->reference_lum, description->reference_lum);

  // `create` destroys the creator
  request->created = wp_image_description_creator_params_v1_create(creator);
  wp_image_description_v1_add_listener(request->created, &_created_listener, request);
  return TRUE;
}

static gboolean _same_as_declared(const dt_wayland_request_t *request)
{
  if(IS_NULL_PTR(_wayland.declared)) return FALSE;
  if(request->profile_size != _wayland.profile_size) return FALSE;
  if(memcmp(request->profile, _wayland.profile, request->profile_size)) return FALSE;

  dt_wayland_description_t description;
  memcpy(&description, &request->description, sizeof(description));
  description.icc_data = NULL;
  return !memcmp(&description, &_wayland.described, sizeof(description));
}

/* Everything the compositor has to say about the preferred description is in. */
static void _description_received(dt_wayland_request_t *request)
{
  dt_wayland_description_t *description = &request->description;

  if(!IS_NULL_PTR(description->icc_data))
  {
    /* An ICC description is the profile the pixels will be encoded with, no more and no less:
     * it is declared as it is. */
    request->profile = description->icc_data;
    request->profile_size = description->icc_size;
    description->icc_data = NULL;

    if(_same_as_declared(request))
      _request_free(request);
    else
      _request_succeeded(request, &request->preferred);
    return;
  }

  if(!description->has_primaries || (!description->tf_named && !description->tf_power))
  {
    dt_print(DT_DEBUG_CONTROL, "[wayland color] the compositor sent neither a profile nor primaries and a tone curve\n");
    _request_free(request);
    return;
  }

  dt_wayland_curve_t curve;
  const gboolean mirrored = _mirror_curve(description, &curve);
  const gboolean as_it_is = mirrored && _is_exact_match(description);
  const gboolean by_profile = _announced(_wayland.features, WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4);

  /* The compositor's own curve is kept when an 8-bit window can be encoded with it and, where
   * the description has to be said back in numbers, when the compositor takes it back. */
  gboolean keep_curve = mirrored;
  if(keep_curve && !as_it_is && !by_profile) keep_curve = _curve_sayable(&curve);

  if(!keep_curve)
  {
    /* The preferred curve is for HDR content, or one the compositor will not take back from
     * a client. An SDR window in the same primaries is then encoded with gamma 2.2, which the
     * compositor places at its reference white. */
    memset(&curve, 0, sizeof(curve));
    curve.gamma = 2.2;
    curve.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22;
    if(!by_profile && !_curve_sayable(&curve))
    {
      dt_print(DT_DEBUG_CONTROL, "[wayland color] the compositor takes no tone curve an SDR window can use\n");
      _request_free(request);
      return;
    }
  }

  const dt_colorspaces_display_description_t numbers = {
    .red = { description->primaries[0] / 1e6, description->primaries[1] / 1e6 },
    .green = { description->primaries[2] / 1e6, description->primaries[3] / 1e6 },
    .blue = { description->primaries[4] / 1e6, description->primaries[5] / 1e6 },
    .white = { description->primaries[6] / 1e6, description->primaries[7] / 1e6 },
    .gamma = curve.gamma,
    .srgb_curve = curve.srgb_curve,
  };
  request->profile = dt_colorspaces_build_display_profile(&numbers, &request->profile_size);
  if(IS_NULL_PTR(request->profile))
  {
    dt_print(DT_DEBUG_CONTROL, "[wayland color] the compositor's primaries do not make an RGB space\n");
    _request_free(request);
    return;
  }

  if(_same_as_declared(request))
  {
    _request_free(request);
    return;
  }

  if(as_it_is)
  {
    _request_succeeded(request, &request->preferred);
    return;
  }

  const gboolean asked = by_profile ? _create_description_from_profile(request)
                                    : _create_description(request, &curve);
  if(!asked)
  {
    dt_print(DT_DEBUG_CONTROL,
             "[wayland color] the compositor cannot be told about an SDR window in its preferred primaries\n");
    _request_free(request);
  }
}

static void _info_done(void *data, struct wp_image_description_info_v1 *info)
{
  dt_wayland_request_t *request = data;

  // `done` is this object's destructor: the compositor has already forgotten it
  wp_image_description_info_v1_destroy(info);
  request->info = NULL;

  if(request != _wayland.latest)
  {
    _request_free(request);
    return;
  }
  _description_received(request);
}

static void _info_icc_file(void *data, struct wp_image_description_info_v1 *info, int32_t descriptor, uint32_t size)
{
  dt_wayland_request_t *request = data;
  dt_wayland_description_t *description = &request->description;

  /* Read rather than mapped: a file shorter than announced is then a short read, where a
   * mapping would be a SIGBUS. 64 MB is what the X atom reader allows a profile to be. */
  if(size > 0 && size <= 64 * 1024 * 1024 && IS_NULL_PTR(description->icc_data))
  {
    uint8_t *buffer = g_malloc(size);
    size_t done = 0;
    while(done < size)
    {
      const ssize_t count = pread(descriptor, buffer + done, size - done, (off_t)done);
      if(count <= 0) break;
      done += (size_t)count;
    }

    if(done == size)
    {
      description->icc_data = buffer;
      description->icc_size = size;
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
  dt_wayland_request_t *request = data;
  const int32_t primaries[8] = { red_x, red_y, green_x, green_y, blue_x, blue_y, white_x, white_y };
  memcpy(request->description.primaries, primaries, sizeof(primaries));
  request->description.has_primaries = TRUE;
}

static void _info_primaries_named(void *data, struct wp_image_description_info_v1 *info, uint32_t primaries)
{
  dt_wayland_request_t *request = data;
  request->description.primaries_named = primaries;
}

static void _info_tf_power(void *data, struct wp_image_description_info_v1 *info, uint32_t exponent)
{
  dt_wayland_request_t *request = data;
  request->description.tf_power = exponent;
}

static void _info_tf_named(void *data, struct wp_image_description_info_v1 *info, uint32_t tf_named)
{
  dt_wayland_request_t *request = data;
  request->description.tf_named = tf_named;
}

static void _info_luminances(void *data, struct wp_image_description_info_v1 *info, uint32_t min_lum,
                             uint32_t max_lum, uint32_t reference_lum)
{
  dt_wayland_request_t *request = data;
  request->description.min_lum = min_lum;
  request->description.max_lum = max_lum;
  request->description.reference_lum = reference_lum;
  request->description.has_luminances = TRUE;
}

static void _info_target_primaries(void *data, struct wp_image_description_info_v1 *info, int32_t red_x,
                                   int32_t red_y, int32_t green_x, int32_t green_y, int32_t blue_x, int32_t blue_y,
                                   int32_t white_x, int32_t white_y)
{
  dt_wayland_request_t *request = data;
  const int32_t primaries[8] = { red_x, red_y, green_x, green_y, blue_x, blue_y, white_x, white_y };
  memcpy(request->description.target_primaries, primaries, sizeof(primaries));
  request->description.has_target_primaries = TRUE;
}

static void _info_target_luminance(void *data, struct wp_image_description_info_v1 *info, uint32_t min_lum,
                                   uint32_t max_lum)
{
  dt_wayland_request_t *request = data;
  request->description.target_max_lum = max_lum;
  request->description.has_target_luminance = TRUE;
}

static void _info_target_max_cll(void *data, struct wp_image_description_info_v1 *info, uint32_t max_cll)
{
  dt_wayland_request_t *request = data;
  request->description.max_cll = max_cll;
}

static void _info_target_max_fall(void *data, struct wp_image_description_info_v1 *info, uint32_t max_fall)
{
  // the frame average of content this window does not have
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

static void _preferred_failed(void *data, struct wp_image_description_v1 *description, uint32_t cause,
                              const char *message)
{
  /* Whatever the windows are declared in stays: it is still a true statement about their
   * pixels, and the compositor converts from it. */
  dt_print(DT_DEBUG_CONTROL, "[wayland color] the compositor did not describe what it prefers: %s (cause %" PRIu32 ")\n",
           message, cause);
  _request_free(data);
}

static void _preferred_ready(void *data, struct wp_image_description_v1 *description, uint32_t identity)
{
  dt_wayland_request_t *request = data;
  if(request != _wayland.latest)
  {
    // the compositor has announced another one since
    _request_free(request);
    return;
  }
  request->info = wp_image_description_v1_get_information(request->preferred);
  wp_image_description_info_v1_add_listener(request->info, &_info_listener, request);
}

static const struct wp_image_description_v1_listener _preferred_listener = {
  .failed = _preferred_failed,
  .ready = _preferred_ready,
};

/* Each request carries its own state and is the `data` of its own listeners, so two of them
 * in flight cannot write into each other; only the latest one is allowed an outcome. */
static void _request_preferred(void)
{
  if(IS_NULL_PTR(_wayland.feedback)) return;

  dt_wayland_request_t *request = g_new0(dt_wayland_request_t, 1);
  request->preferred = wp_color_management_surface_feedback_v1_get_preferred(_wayland.feedback);
  wp_image_description_v1_add_listener(request->preferred, &_preferred_listener, request);

  _wayland.requests = g_slist_prepend(_wayland.requests, request);
  _wayland.latest = request;
}

/* ------------------------------------------------------------------------------------------
 * The colour manager.
 */

static void _manager_supported_intent(void *data, struct wp_color_manager_v1 *manager, uint32_t render_intent)
{
  // perceptual is the one intent used here, and the one every compositor has to support
}

static void _manager_supported_feature(void *data, struct wp_color_manager_v1 *manager, uint32_t feature)
{
  _announce(&_wayland.features, feature);
}

static void _manager_supported_tf_named(void *data, struct wp_color_manager_v1 *manager, uint32_t tf_named)
{
  _announce(&_wayland.tf_named, tf_named);
}

static void _manager_supported_primaries_named(void *data, struct wp_color_manager_v1 *manager, uint32_t primaries)
{
  _announce(&_wayland.primaries_named, primaries);
}

static void _manager_done(void *data, struct wp_color_manager_v1 *manager)
{
  if(_wayland.manager_ready) return;
  _wayland.manager_ready = TRUE;

  _wayland.map_hook = g_signal_add_emission_hook(g_signal_lookup("map", GTK_TYPE_WIDGET), 0, _map_hook, NULL, NULL);
  _wayland.unmap_hook = g_signal_add_emission_hook(g_signal_lookup("unmap", GTK_TYPE_WIDGET), 0, _unmap_hook, NULL,
                                                   NULL);

  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_CHANGED,
                                  G_CALLBACK(_profile_changed), NULL);
  DT_DEBUG_CONTROL_SIGNAL_CONNECT(dt_control_signal_get_global(), DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED,
                                  G_CALLBACK(_profile_user_changed), NULL);

  // the windows that were mapped before the compositor answered
  GList *toplevels = gtk_window_list_toplevels();
  for(GList *iter = toplevels; iter; iter = g_list_next(iter))
    _window_attach(GTK_WIDGET(iter->data));
  g_list_free(toplevels);
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
  if(!IS_NULL_PTR(_wayland.manager)) return;
  if(g_strcmp0(interface, wp_color_manager_v1_interface.name)) return;

  /* Version 1, whatever the compositor has: it is the one this file was written against, and
   * a compositor only sends a client the events of the version the client bound. */
  _wayland.manager = wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1);
  if(IS_NULL_PTR(_wayland.manager)) return;
  wp_color_manager_v1_add_listener(_wayland.manager, &_manager_listener, NULL);
}

static void _registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
  // objects made from a global outlive its withdrawal, so there is nothing to undo
}

static const struct wl_registry_listener _registry_listener = {
  .global = _registry_global,
  .global_remove = _registry_global_remove,
};

void dt_gui_wayland_color_init(GtkWidget *main_window)
{
  if(IS_NULL_PTR(main_window) || !IS_NULL_PTR(_wayland.registry)) return;

  // both backends are compiled into GTK: which one runs is known from the display only
  GdkDisplay *display = gtk_widget_get_display(main_window);
  if(!GDK_IS_WAYLAND_DISPLAY(display)) return;

  struct wl_display *connection = gdk_wayland_display_get_wl_display(display);
  if(IS_NULL_PTR(connection)) return;

  _wayland.main_window = main_window;
  _wayland.registry = wl_display_get_registry(connection);
  wl_registry_add_listener(_wayland.registry, &_registry_listener, NULL);
}

void dt_gui_wayland_color_cleanup(void)
{
  if(IS_NULL_PTR(_wayland.registry)) return;

  if(_wayland.manager_ready)
  {
    g_signal_remove_emission_hook(g_signal_lookup("map", GTK_TYPE_WIDGET), _wayland.map_hook);
    g_signal_remove_emission_hook(g_signal_lookup("unmap", GTK_TYPE_WIDGET), _wayland.unmap_hook);
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_profile_changed), NULL);
    DT_DEBUG_CONTROL_SIGNAL_DISCONNECT(dt_control_signal_get_global(), G_CALLBACK(_profile_user_changed), NULL);
  }

  // destroying a proxy removes its listener: no request answers after this
  while(!IS_NULL_PTR(_wayland.requests))
    _request_free(_wayland.requests->data);

  GList *toplevels = gtk_window_list_toplevels();
  for(GList *iter = toplevels; iter; iter = g_list_next(iter))
    _window_detach(GTK_WIDGET(iter->data));
  g_list_free(toplevels);

  if(!IS_NULL_PTR(_wayland.declared)) wp_image_description_v1_destroy(_wayland.declared);
  if(!IS_NULL_PTR(_wayland.manager)) wp_color_manager_v1_destroy(_wayland.manager);
  wl_registry_destroy(_wayland.registry);
  dt_free(_wayland.profile);

  memset(&_wayland, 0, sizeof(_wayland));
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
