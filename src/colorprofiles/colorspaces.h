/*
    This file is part of darktable,
    Copyright (C) 2010 Henrik Andersson.
    Copyright (C) 2010-2012, 2017 johannes hanika.
    Copyright (C) 2010 José Carlos García Sogo.
    Copyright (C) 2011 Bruce Guenter.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2012, 2014 Pascal de Bruijn.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013-2017 Tobias Ellinghaus.
    Copyright (C) 2014, 2019-2022 Pascal Obry.
    Copyright (C) 2014-2016 Roman Lebedev.
    Copyright (C) 2014 Ulrich Pegelow.
    Copyright (C) 2015-2016 Pedro Côrte-Real.
    Copyright (C) 2018-2019 Edgardo Hoszowski.
    Copyright (C) 2019 Philippe Weyland.
    Copyright (C) 2020, 2022-2025 Aurélien PIERRE.
    Copyright (C) 2020 Dan Torop.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2021 Miloš Komarčević.
    Copyright (C) 2021 Ralf Brown.
    Copyright (C) 2021 Sakari Kapanen.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2023, 2025 Alynx Zhou.
    
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

#ifndef DT_COLORPROFILES_COLORSPACES_H
#define DT_COLORPROFILES_COLORSPACES_H

#include "colorprofiles/profile_types.h"
#include "math/matrices.h"
#include "system/simd.h"

#include <glib.h>
#include <lcms2.h>
#include <pthread.h>

/* Opaque, exactly as GTK spells it: dt_colorspaces_set_display_profile() only passes the
 * window through to system/display_profile.h. Declaring it here keeps <gtk/gtk.h> out of a
 * header 200-odd files include, most of which have nothing to do with the GUI. */
typedef struct _GtkWidget GtkWidget;
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// max samples in a tone-curve LUT built from a profile
#define LUT_SAMPLES 0x10000

// this was removed from lcms2 in 2.4
#ifndef TYPE_XYZA_FLT
  #define TYPE_XYZA_FLT (FLOAT_SH(1)|COLORSPACE_SH(PT_XYZ)|EXTRA_SH(1)|CHANNELS_SH(3)|BYTES_SH(4))
#endif


typedef struct dt_colorspaces_t
{
  GList *profiles;

  // xatom color profile:
  pthread_rwlock_t xprofile_lock;
  gchar *colord_profile_file;
  uint8_t *xprofile_data;
  int xprofile_size;

  // the current set of selected profiles
  dt_colorspaces_color_profile_type_t display_type;
  dt_colorspaces_color_profile_type_t softproof_type;
  char display_filename[512];
  char softproof_filename[512];
  dt_iop_color_intent_t display_intent;
  dt_iop_color_intent_t softproof_intent;

  dt_colorspaces_color_mode_t mode;

  cmsHTRANSFORM transform_srgb_to_display, transform_adobe_rgb_to_display, transform_xyz_to_display, transform_display_to_adobe_rgb;

} dt_colorspaces_t;

typedef struct dt_colorspaces_color_profile_t
{
  /* TRUE when this container created `profile` and must close it. FALSE when `profile` is
   * borrowed from the application-wide list, which owns and closes it -- see
   * dt_image_find_best_color_profile(), several of whose branches hand back a pointer into
   * that list rather than a fresh profile. Only per-image containers set this; entries in the
   * application list are freed by dt_colorspaces_cleanup() as they always were. */
  gboolean owns_profile;
  dt_colorspaces_color_profile_type_t type; // filename is only used for type DT_COLORSPACE_FILE
  char filename[DT_IOP_COLOR_ICC_LEN];      // icc file name
  char name[512];                           // product name, displayed in GUI
  cmsHPROFILE profile;                      // the actual profile
  int in_pos;                               // position in input combo box, -1 if not applicable
  int out_pos;                              // position in output combo box, -1 if not applicable
  int display_pos;                          // position in display combo box, -1 if not applicable
  int category_pos;                         // position in category combo box, -1 if not applicable
  int work_pos;                             // position in working combo box, -1 if not applicable
} dt_colorspaces_color_profile_t;

typedef struct dt_colorspaces_cicp_t
{
    dt_colorspaces_cicp_color_primaries_t color_primaries;
    dt_colorspaces_cicp_transfer_characteristics_t transfer_characteristics;
    dt_colorspaces_cicp_matrix_coefficients_t matrix_coefficients;
} dt_colorspaces_cicp_t;

int mat3inv_float(float *const dst, const float *const src);
int mat3inv(float *const dst, const float *const src);


/* --- lifecycle -------------------------------------------------------------
 *
 * The module owns its state. It used to hang off darktable_t as
 * `struct dt_colorspaces_t *color_profiles`, which put the whole application one
 * dereference away from the profile list, its rwlock and its cached transforms. The
 * instance is file-static in colorspaces.c now; these two are the only way to bring it
 * up and take it down, and they are called once each, by the application, with no
 * threads running. */
void dt_colorprofiles_init(void);
void dt_colorprofiles_cleanup(void);

/* BEING RETIRED -- do not add callers.
 *
 * The remaining consumers that still walk ->profiles or take ->xprofile_lock by hand
 * (16 list walks and 2 lock regions at the time of writing, all counted by
 * tools/check_module_boundaries.sh) need a name for the instance until the query API
 * replaces them. This declaration disappears with the last of them, together with
 * dt_colorspaces_t itself. */
dt_colorspaces_t *dt_colorspaces_get_global(void);


/* --- CRUDE: the metadata half ------------------------------------------------
 *
 * Everything here answers a question ABOUT a profile and answers it with plain values.
 * No cmsHPROFILE crosses this boundary, and no caller iterates the list: enumeration
 * hands back a value array, everything else is a lookup.
 *
 * `direction` is mandatory and is not a nicety. DT_COLORSPACE_SRGB is registered TWICE --
 * a v4 parametric-curve profile valid only as input, and a v2 point-TRC profile valid for
 * out/display/category/work -- and nothing else distinguishes them. A multi-bit mask
 * resolves to the first match in registration order, which for sRGB is the v4 input entry;
 * that is what DT_PROFILE_DIRECTION_ANY does today, bug included.
 *
 * The index-valued calls REQUIRE a single-bit direction and return -1 / FALSE otherwise:
 * an index means nothing outside the enumeration that produced it, and an index taken from
 * IN|OUT equals neither the old in_pos nor the old out_pos.
 *
 * None of these takes a lock, deliberately: the list is built once at init and the only
 * datum that mutates afterwards is the DT_COLORSPACE_DISPLAY entry's cmsHPROFILE, which
 * none of them reads. */

/** A profile's public identity: what the GUI displays and stores, and nothing else.
 * A plain value -- copy it, put it in GTK object data, outlive anything with it. */
typedef struct dt_colorprofile_desc_t
{
  dt_colorspaces_color_profile_type_t type;
  char filename[DT_IOP_COLOR_ICC_LEN];  // "" unless type == DT_COLORSPACE_FILE
  char name[512];                       // translated, display-ready
} dt_colorprofile_desc_t;

/** Ordered snapshot of one direction. out[k] is exactly the entry whose legacy X_pos was
 * k for the single-bit direction X, so a combo built from it keeps today's ordering and
 * today's stored indices. Caller owns *out and frees it with dt_free_align.
 * Returns the count; 0 with *out == NULL is a legal answer. */
size_t dt_colorspaces_enumerate_profiles(const dt_colorspaces_profile_direction_t direction,
                                         dt_colorprofile_desc_t **out);

/** Combo position of (type, filename) within `direction`, or -1 when absent or when
 * `direction` has more than one bit set. Callers add their own offset for leading
 * non-profile entries ("same as original", "image settings", ...). */
int dt_colorspaces_profile_index(const dt_colorspaces_profile_direction_t direction,
                                 const dt_colorspaces_color_profile_type_t type,
                                 const char *const filename);

/** Identity at `index` within `direction`. FALSE, leaving *out untouched, when the index
 * is out of range or `direction` is not a single bit -- which is the "stored choice is no
 * longer installed, fall back" branch as a return value rather than a diagnostic print. */
gboolean dt_colorspaces_profile_at(const dt_colorspaces_profile_direction_t direction,
                                   const int index,
                                   dt_colorprofile_desc_t *const out);

/** Is this identity registered for this direction? Valid for a multi-bit mask too. */
gboolean dt_colorspaces_profile_exists(const dt_colorspaces_profile_direction_t direction,
                                       const dt_colorspaces_color_profile_type_t type,
                                       const char *const filename);

/** create a profile from a xyz->camera matrix. */
cmsHPROFILE dt_colorspaces_create_xyzimatrix_profile(float cam_xyz[3][3]);

/** create a ICC virtual profile from the shipped presets in darktable. */
cmsHPROFILE dt_colorspaces_create_darktable_profile(const char *makermodel);

/** create a ICC virtual profile from the shipped vendor matrices in darktable. */
cmsHPROFILE dt_colorspaces_create_vendor_profile(const char *makermodel);

/** create a ICC virtual profile from the shipped alternate matrices in darktable. */
cmsHPROFILE dt_colorspaces_create_alternate_profile(const char *makermodel);

/** return the work profile as set in colorin */


/* LCMS transform handles are not safe to rediscover indirectly from mutable owner
 * structs inside OpenMP regions. Alias the cmsHTRANSFORM to a local variable before
 * entering a parallel region, declare that alias shared there, and pass only that
 * stable handle to these helpers.
 *
 * These take a transform the CALLER built and owns (iop/colorin.c, iop/colorout.c).
 * For the module's own prepared display transforms, use the entry points below --
 * those handles are rebuilt on monitor-profile changes and must never be borrowed. */
void dt_colorspaces_transform_rgba_float_row(const cmsHTRANSFORM transform, const float *in, float *out,
                                             const int width);
void dt_colorspaces_transform_rgba_float_image(const cmsHTRANSFORM transform, const float *image_in, float *image_out,
                                               const int width, const int height);


/* --- prepared display transforms: the cmsHTRANSFORM never leaves the module ---
 *
 * The four cached transforms are deleted and rebuilt whenever the monitor profile or
 * the display intent changes, so a borrowed handle can be freed under its user. These
 * functions take the read lock internally, for the whole conversion. */

/** D50 XYZ -> display RGB, one pixel. Falls back to sRGB when no display profile has
 * been resolved. */
void dt_colorprofiles_xyz_to_display(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t RGB);

/** Whole 8-bit plane, packed RGBA8 in -> BGRA8 out (cairo byte order), from `src_space`
 * to the display profile. DT_COLORSPACE_DISPLAY passes through with an R <-> B swap.
 * `in` and `out` may alias. Returns FALSE when the pixels could not be colour-managed
 * and only the byte swap was applied. */
gboolean dt_colorprofiles_rgba8_to_display_bgra8(const uint8_t *const in, uint8_t *const out,
                                                 const int width, const int height,
                                                 const dt_colorspaces_color_profile_type_t src_space);

/** The storage leg: 8-bit plane from `src_space` (BGRA8) to AdobeRGB (RGBA8), for
 * thumbnails written to the mipmap cache. `in` and `out` may alias. */
gboolean dt_colorprofiles_bgra8_to_adobergb_rgba8(const uint8_t *const in, uint8_t *const out,
                                                  const int width, const int height,
                                                  const dt_colorspaces_color_profile_type_t src_space);

/** Strided, packed-RGB(A) 8-bit buffer (GdkPixbuf shape), sRGB -> display, in place.
 * Plain integers only: the module never sees a GdkPixbuf. Returns FALSE when no display
 * transform is available, leaving the pixels untouched. */
gboolean dt_colorprofiles_srgb_to_display_strided(uint8_t *const pixels, const int width, const int height,
                                                  const int rowstride, const int n_channels,
                                                  const gboolean has_alpha);


/* --- display and soft-proofing settings: whole struct in, whole struct out ---
 *
 * The seven fields cross the boundary only together. Reading them one at a time --
 * which is what direct member access forced -- lets a reader observe a new profile
 * type paired with the previous filename, and a 512-byte filename read while it is
 * being g_strlcpy'd is a torn string, not a stale one. Both groups are read that way
 * today by iop/colorout.c and iop/filmicrgb.c on pipeline threads while the GUI
 * thread writes them. */
typedef struct dt_colorprofiles_settings_t
{
  dt_colorspaces_color_mode_t mode;                    // NORMAL / SOFTPROOF / GAMUTCHECK
  dt_colorspaces_color_profile_type_t display_type;
  char display_filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t display_intent;
  dt_colorspaces_color_profile_type_t softproof_type;
  char softproof_filename[DT_IOP_COLOR_ICC_LEN];
  dt_iop_color_intent_t softproof_intent;

  /** Advances on every accepted change. A pipeline module can fold this one number into
   * its hash instead of the individual fields. */
  uint64_t generation;
} dt_colorprofiles_settings_t;

/** Atomic snapshot into caller-provided storage. */
void dt_colorprofiles_get_settings(dt_colorprofiles_settings_t *const out);

/* Each setter returns whether anything actually changed, so callers stop deciding that
 * for themselves against a value they read separately. The display ones also rebuild the
 * four prepared transforms, under the same lock, so identity and transforms never
 * disagree. `filename` is only meaningful for DT_COLORSPACE_FILE. */
gboolean dt_colorprofiles_set_display_profile_choice(const dt_colorspaces_color_profile_type_t type,
                                                     const char *const filename);
gboolean dt_colorprofiles_set_display_intent(const dt_iop_color_intent_t intent);
gboolean dt_colorprofiles_set_softproof_profile_choice(const dt_colorspaces_color_profile_type_t type,
                                                       const char *const filename);
gboolean dt_colorprofiles_set_softproof_intent(const dt_iop_color_intent_t intent);
gboolean dt_colorprofiles_set_mode(const dt_colorspaces_color_mode_t mode);

/** Turn `mode` on, or back to NORMAL if it is already the current mode, as one locked
 * read-modify-write. Returns the mode now in effect. The two toggle buttons each
 * open-coded this and were not atomic against each other. */
dt_colorspaces_color_mode_t dt_colorprofiles_toggle_mode(const dt_colorspaces_color_mode_t mode);


/** return an rgb lcms2 profile from data. if data points to a grayscale profile a new rgb profile is created
 * that has the same TRC, black and white point and rec709 primaries. */
cmsHPROFILE dt_colorspaces_get_rgb_profile_from_mem(uint8_t *data, uint32_t size);

/** free the resources of a profile created with the functions above. */
void dt_colorspaces_cleanup_profile(cmsHPROFILE p);

/** extracts tonecurves and color matrix prof to XYZ from a given input profile, returns 0 on success (curves
 * and matrix are inverted for input) */
int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                 float *lutb, const int lutsize);

/** extracts tonecurves and color matrix prof to XYZ from a given output profile, returns 0 on success. */
int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize);

/** wrapper to get the name from a color profile. this tries to handle character encodings. */
void dt_colorspaces_get_profile_name(cmsHPROFILE p, const char *language, const char *country, char *name,
                                     size_t len);

/** get a nice printable name. */
const char *dt_colorspaces_get_name(dt_colorspaces_color_profile_type_t type, const char *filename);

/** common functions to change between colorspaces, used in iop modules */
void rgb2hsl(const dt_aligned_pixel_t rgb, float *h, float *s, float *l);
void hsl2rgb(dt_aligned_pixel_t rgb, float h, float s, float l);

/* Release a profile container owned by an image (dt_image_t.embedded_profile), closing the
 * LCMS2 handle inside it. Called by the image cache when the image is evicted; nothing else
 * should need it. Declared here so common/image_cache.c does not need the struct layout. */
/* Build a container for a profile that belongs to ONE image rather than to the application.
 * @p owns_profile says whether the container must close the LCMS2 handle: pass FALSE when the
 * profile is borrowed from the application-wide list, which owns and closes it. Hidden from
 * every combo box by construction. Freed with dt_colorspaces_free_image_profile(). */
struct dt_colorspaces_color_profile_t *dt_colorspaces_new_image_profile(
    dt_colorspaces_color_profile_type_t type, cmsHPROFILE profile, gboolean owns_profile);

void dt_colorspaces_free_image_profile(struct dt_colorspaces_color_profile_t *profile);

/* Notification that the display profile changed. The application relays it on its signal bus;
 * this module does not know there is one. Unregistered, the notification is dropped. */
typedef void (*dt_colorspaces_profile_changed_handler_t)(void);
void dt_colorspaces_set_profile_changed_handler(dt_colorspaces_profile_changed_handler_t handler);

/** trigger updating the display profile from the system settings (x atom, colord, ...) */
/** Refresh the cached display profile from the monitor showing `widget`.
 *  The caller owns the window: this module never asks the GUI which one to look at. */
void dt_colorspaces_set_display_profile(const dt_colorspaces_color_profile_type_t profile_type,
                                       GtkWidget *widget);

/** get the profile described by type & filename.
 *  this doesn't support image specifics like embedded profiles or camera matrices */
const dt_colorspaces_color_profile_t *
dt_colorspaces_get_profile(dt_colorspaces_color_profile_type_t type, const char *filename,
                           dt_colorspaces_profile_direction_t direction);

/** check whether filename is the same profil as fullname, this is taking into account that
 *  fullname is always the fullpathname to the profile and filename may be a full pathname
 *  or just a base name */
gboolean  dt_colorspaces_is_profile_equal(const char *fullname, const char *filename);


/** update the display transforms of srgb and adobergb to the display profile.
 * make sure that dt_colorspaces_get_global()->xprofile_lock is held when calling this! */
void dt_colorspaces_update_display_transforms();

/** Calculate CAM->XYZ, XYZ->CAM matrices **/
int dt_colorspaces_conversion_matrices_xyz(const float adobe_XYZ_to_CAM[4][3], float in_XYZ_to_CAM[9], double XYZ_to_CAM[4][3], double CAM_to_XYZ[3][4]);

/** Calculate CAM->RGB, RGB->CAM matrices and default WB multipliers */
int dt_colorspaces_conversion_matrices_rgb(const float adobe_XYZ_to_CAM[4][3], double RGB_to_CAM[4][3], double CAM_to_RGB[3][4], const float *embedded_matrix, double mul[4]);

/** Applies CYGM WB coeffs to an image that's already been converted to RGB by dt_colorspaces_cygm_to_rgb */
// FIXME: CRITICAL: why is this function NOT used anywhere ???
void dt_colorspaces_cygm_apply_coeffs_to_rgb(float *out, const float *in, int num, double RGB_to_CAM[4][3], double CAM_to_RGB[3][4], dt_aligned_pixel_t coeffs);

/** convert CYGM buffer to RGB */
void dt_colorspaces_cygm_to_rgb(float *out, int num, double CAM_to_RGB[3][4]);

/** convert RGB buffer to CYGM */
void dt_colorspaces_rgb_to_cygm(float *out, int num, double RGB_to_CAM[4][3]);







#ifdef __cplusplus
}
#endif

#endif // DT_COLORPROFILES_COLORSPACES_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
