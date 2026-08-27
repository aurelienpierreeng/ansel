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

/**
 * @file colorprofiles/colorspaces.h
 * @brief The colour-profile module's API: which profiles exist, and how to apply one.
 *
 * @details The module owns its state. A single `dt_colorspaces_t` lives file-static in
 * colorspaces.c, built by dt_colorprofiles_init() and torn down by
 * dt_colorprofiles_cleanup(). It used to hang off the application struct as
 * `darktable.color_profiles`, which put every translation unit one dereference away from
 * the profile list, its rwlock and its cached LCMS transforms.
 *
 * The API is split in two halves, which is the whole design:
 *
 * - **CRUDE (metadata).** add/remove/fetch/enumerate answer questions ABOUT a profile
 *   -- `{type, filename, name}` for a `role` -- and answer them with VALUE copies.
 *   No lcms2 type crosses this boundary and no caller walks the list. No lock is taken:
 *   the list is built once at init and never appended to again.
 * - **Lock and Apply (data).** dt_colorspaces_lock_profiles() /
 *   dt_colorspaces_unlock_profiles() pin the profile handles while a caller derives from
 *   one; the prepared-transform entry points below run the pixel loop themselves, so the
 *   `cmsHTRANSFORM` never leaves the module.
 *
 * @note Lifetime here is answered by a lock, not by a copy, and that is a measurement,
 * not a taste: there is no `cmsDupProfile` in lcms2. The only true deep copy of a
 * profile is serialise-and-reopen -- about 0.005 ms for a built-in, but 1.02 ms for a
 * real colord display profile -- and copying a prepared `cmsHTRANSFORM` means rebuilding
 * it from scratch, 2.2 to 38 ms, with nothing to amortise it against.
 *
 * @note Including this header drags in `<lcms2.h>` and `<pthread.h>`. A translation unit
 * that only needs the vocabulary (a profile type to store in its params, an intent to
 * pass along) should include colorprofiles/profile_types.h instead, which is the reason
 * that header exists.
 *
 * @warning Profiles derived from ONE IMAGE -- DT_COLORSPACE_EMBEDDED_ICC through
 * DT_COLORSPACE_ALTERNATE_MATRIX -- are NOT registered in the list and cannot be resolved
 * by identity through dt_colorspaces_get_profile(): their matrices come from the image's
 * own camera data via iop/colorin.c. They live on the pipe that built them, never in the
 * shared list.
 */

#include "colorprofiles/profile_types.h"
#include "math/matrices.h"
#include "system/simd.h"

#include <glib.h>
#include <lcms2.h>
#include <pthread.h>

/**
 * @brief GtkWidget, opaque, spelled exactly as GTK spells it.
 *
 * @details dt_colorspaces_set_display_profile() only passes the window through to
 * system/display_profile.h, so this header needs the name and nothing else. Declaring it
 * here keeps `<gtk/gtk.h>` out of a header 40-odd files include, most of which have
 * nothing to do with the GUI.
 */
typedef struct _GtkWidget GtkWidget;
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Max samples in a tone-curve LUT built from a profile (65536).
 *  @note iop/colorin.c and iop/colorout.c each re-define the identical value locally for
 *  their own `lut[3][LUT_SAMPLES]` members; the definitions must stay in step. */
#define LUT_SAMPLES 0x10000

/** @brief lcms2 pixel format for float XYZ + one extra channel.
 *  @details This was removed from lcms2 in 2.4, so it is rebuilt from the same shift
 *  macros. Used for the prepared XYZ -> display transform, and by iop/colorout.c when the
 *  output profile is DT_COLORSPACE_XYZ. */
#ifndef TYPE_XYZA_FLT
  #define TYPE_XYZA_FLT (FLOAT_SH(1)|COLORSPACE_SH(PT_XYZ)|EXTRA_SH(1)|CHANNELS_SH(3)|BYTES_SH(4))
#endif


/**
 * @brief The module's private state. Declared here for the module's own .c files only.
 *
 * @details Nothing outside `src/colorprofiles/` names this type, and the single instance
 * is file-static in colorspaces.c -- there is no accessor for it in this header. It is
 * documented because the module's own translation units share it, not because it is an
 * interface.
 *
 * Almost all of it is immutable after dt_colorprofiles_init(). Exactly two things mutate
 * afterwards, both guarded by the transforms lock: the DT_COLORSPACE_DISPLAY entry's
 * `cmsHPROFILE`, and the four prepared transforms derived from it.
 *
 * @warning LOCK ORDER, where both are involved: `_transforms_lock` OUTER, the settings lock
 * (private to colorspaces.c) INNER. The display setters need both, because changing the
 * display profile identity also rebuilds the four transforms. Nothing takes them the
 * other way round.
 */
typedef struct dt_colorspaces_t
{
  /** Every registered profile, as `dt_colorspaces_color_profile_t *`, in REGISTRATION
   * order. That order is load-bearing: it is what dt_colorspaces_enumerate_profiles()
   * reproduces and what every stored combo index in every preset and conf key refers to.
   * Built once by init, sorted per directory batch before positions are handed out, and
   * never appended to at runtime. */
  GList *profiles;

  /** Guards the DT_COLORSPACE_DISPLAY entry's `cmsHPROFILE` and the four transforms
   * below -- the only mutable state in the module. Do not take it by hand: use
   * dt_colorspaces_lock_profiles() / dt_colorspaces_unlock_profiles(). */
  // xatom color profile:
  /** Path colord last reported for this window, kept so an unchanged path costs nothing. */
  gchar *colord_profile_file;
  /** Raw bytes of the monitor profile last read from the xatom or colord, and their
   * length. A new read is compared against these, and the transforms are rebuilt only
   * when the bytes actually differ. */
  uint8_t *xprofile_data;
  int xprofile_size;

  /** @name Settings group
   *  The seven fields the GUI writes and the pipeline reads. Read them through
   *  dt_colorprofiles_get_settings() and write them through the setters, which serialise
   *  on a lock private to colorspaces.c. Reading them directly is seven unsynchronised
   *  loads, and a 512-byte filename read while g_strlcpy() is writing it is a TORN
   *  string, not merely a stale one.
   *  @{ */
  // the current set of selected profiles
  dt_colorspaces_color_profile_type_t display_type;
  dt_colorspaces_color_profile_type_t softproof_type;
  char display_filename[512];
  char softproof_filename[512];
  dt_iop_color_intent_t display_intent;
  dt_iop_color_intent_t softproof_intent;

  dt_colorspaces_color_mode_t mode;
  /** @} */

  /** The prepared display transforms. Deleted and rebuilt in one go whenever the monitor
   * profile or the display intent changes, so a borrowed handle can be freed under its
   * user. They are reachable only through the entry points further down this header,
   * each of which holds the read lock for the whole conversion. */
  cmsHTRANSFORM transform_srgb_to_display, transform_adobe_rgb_to_display, transform_xyz_to_display, transform_display_to_adobe_rgb;

} dt_colorspaces_t;

/**
 * @brief One registered profile: its identity, its LCMS handle, and where it sits in each
 * combo box.
 *
 * @details Entries in the application-wide list are created once at init and freed by
 * dt_colorprofiles_cleanup() (through `_colorspaces_destroy()`, which closes every
 * `profile` it finds). The same struct is also used for a container belonging to ONE
 * image -- see dt_colorspaces_new_image_profile() -- and those set every `*_pos` to -1 so
 * they are invisible to enumeration by construction.
 *
 * @warning Three entries are registered with `profile == NULL`: the categories
 * DT_COLORSPACE_WORK, DT_COLORSPACE_EXPORT and DT_COLORSPACE_SOFTPROOF, which name a
 * user setting rather than a colour space. Nothing NULL-checks the handle at ~40 call
 * sites that dereference `->profile`; what actually keeps them safe is that lookup never
 * gives the category entries a role (see @ref roles), so a category
 * entry can never be returned. Do not "fix" the lookup predicate to consult
 * them a role of their own without auditing those sites first.
 */
typedef struct dt_colorspaces_color_profile_t
{
  /** TRUE when this container created `profile` and must close it. FALSE when `profile` is
   * borrowed from the application-wide list, which owns and closes it -- see
   * dt_image_find_best_color_profile(), several of whose branches hand back a pointer into
   * that list rather than a fresh profile. Only per-image containers set this; entries in the
   * application list are freed by dt_colorprofiles_cleanup() as they always were.
   * @warning Getting this wrong double-frees on eviction: the image cache closes the
   * handle, then the application list closes it again at shutdown. */
  /** @brief Guards ::profile, and nothing else in this struct.
   *
   * @details Per ENTRY, not per module. Only the DT_COLORSPACE_DISPLAY entry's handle is
   * actually replaced at runtime (by the monitor-profile refresh), but a caller deriving
   * from any profile takes this rather than a module-wide lock, so a thumbnail conversion
   * running against sRGB does not stand between a monitor change and the display entry.
   *
   * Take it with dt_colorspaces_lock_profile() / dt_colorspaces_unlock_profile(); do not
   * touch it directly. The rest of the struct -- type, filename, name, the positions -- is
   * fixed at registration and needs no lock.
   */
  pthread_rwlock_t lock;
  gboolean owns_profile;
  dt_colorspaces_color_profile_type_t type; ///< filename is only used for type DT_COLORSPACE_FILE
  char filename[DT_IOP_COLOR_ICC_LEN];      ///< icc file name (absolute; compare with dt_colorspaces_is_profile_equal())
  char name[512];                           ///< product name, displayed in GUI (translated for built-ins)
  cmsHPROFILE profile;                      ///< the actual profile; NULL for the three category entries
  /** @brief Which menus this entry appears in, as a ::dt_colorspaces_profile_role_t mask.
   *
   * @details This used to be five separate `int *_pos` fields holding a combo-box row number
   * per menu, with -1 meaning "not in this one". Nothing ever read the numbers: every lookup
   * and every enumeration only asked whether one was >= 0. The positions were therefore a
   * second, hand-maintained copy of a fact the list order already carries -- enumeration
   * walks the list in registration order, so the k-th entry serving a role IS row k -- and
   * keeping them in step meant threading five `++counter`s through twenty registration calls
   * in the right order.
   *
   * Zero means no menu, which is what an image-owned container gets (see
   * dt_colorspaces_new_image_profile()) and what makes it unreachable by any lookup. The
   * three category entries are zero too, which is what keeps their `profile == NULL` away
   * from the call sites that dereference it unchecked.
   */
  dt_colorspaces_profile_role_t roles;
} dt_colorspaces_color_profile_t;

/**
 * @brief The three ITU-T H.273 code points that describe a colour space without an ICC
 * profile.
 *
 * @details Read out of AVIF/HEIF containers by imageio/imageio_avif.c and
 * imageio/imageio_heif.c, and mapped onto a dt_colorspaces_color_profile_type_t by
 * `dt_colorspaces_cicp_to_type()` (imageio/imageio_profile.c). Nothing in this module
 * consumes it; it lives here because it is part of the colour vocabulary.
 */
typedef struct dt_colorspaces_cicp_t
{
    dt_colorspaces_cicp_color_primaries_t color_primaries;
    dt_colorspaces_cicp_transfer_characteristics_t transfer_characteristics;
    dt_colorspaces_cicp_matrix_coefficients_t matrix_coefficients;
} dt_colorspaces_cicp_t;

/**
 * @brief Invert a 3x3 matrix stored row-major as 9 contiguous floats.
 *
 * @param dst destination, 9 floats, written only on success.
 * @param src source, 9 floats.
 * @return 0 on success, 1 when the matrix is singular (|det| < 1e-7), leaving `dst`
 * untouched.
 * @note Colour maths only by accident of history -- iop/ashift.c uses it to invert a
 * homography. A `double` variant is generated by the same macro in colorspaces.c but is
 * deliberately not exported.
 */
int mat3inv_float(float *const dst, const float *const src);

/** @brief Thin alias of mat3inv_float(), same contract. @see mat3inv_float */
int mat3inv(float *const dst, const float *const src);


/* --- lifecycle ----------------------------------------------------------- */

/**
 * @brief Build the module's single instance: register every built-in profile, load
 * the `color/in` and `color/out` directories under both userconfig and datadir, restore the display/soft-proof settings
 * from conf, and build the four prepared display transforms.
 *
 * @details The instance is file-static in colorspaces.c; there is no way to name it from
 * outside. It used to hang off darktable_t as `struct dt_colorspaces_t *color_profiles`,
 * which put the whole application one dereference away from the profile list, its rwlock
 * and its cached transforms.
 *
 * @note Idempotent: a second call with an instance already up returns immediately.
 * @warning Called once, by the application, with no threads running. Everything that
 * reads the profile list assumes it stops changing when this returns.
 * @note Debug builds additionally run a selftest proving that the enumeration order this
 * produces still matches the legacy per-entry `*_pos` integers -- if it ever stops
 * matching, every stored combo index in every preset points at the wrong profile.
 */
void dt_colorprofiles_init(void);

/**
 * @brief Persist the display/soft-proof settings to conf, flush the derived matrix/LUT
 * memo, delete the prepared transforms, close every profile and free the instance.
 *
 * @details The memo goes first, deliberately: its entries are derived from the profiles
 * that are about to be closed.
 *
 * @note Idempotent, and safe to call when init never ran.
 * @warning Called once, by the application, with no threads running. Any
 * `dt_iop_order_iccprofile_info_t *` handed out by dt_colorspaces_add_profile() is
 * dangling afterwards.
 */
void dt_colorprofiles_cleanup(void);



/* --- CRUDE: the metadata half ------------------------------------------------
 *
 * Everything here answers a question ABOUT a profile and answers it with plain values.
 * No cmsHPROFILE crosses this boundary, and no caller iterates the list: enumeration
 * hands back a value array, everything else is a lookup.
 *
 * THE ROLE PREDICATE. `role` is mandatory and is not a nicety.
 * DT_COLORSPACE_SRGB is registered TWICE -- a v4 parametric-curve profile valid only as
 * input, and a v2 point-TRC profile valid for output/monitor/working -- and nothing else
 * distinguishes them. A multi-bit mask resolves to the first match in registration order,
 * which for sRGB is the v4 input entry; that is what DT_PROFILE_ROLE_ANY does.
 *
 * The predicate tests the entry's role mask, and the three NULL-profile category entries
 * have an empty one -- which is what makes them unreachable (see
 * dt_colorspaces_color_profile_t). MONITOR is not an optical direction at all; it is the eligibility
 * list for the monitor-profile menu, and it diverges from OUTPUT on 5 of the 21 built-in
 * entries.
 *
 * The index-valued calls REQUIRE a single-bit role and return -1 / FALSE otherwise:
 * an index means nothing outside the enumeration that produced it, and an index taken from
 * INPUT|OUTPUT equals neither menu's row number.
 *
 * None of these takes a lock, deliberately: the list is built once at init and the only
 * datum that mutates afterwards is the DT_COLORSPACE_DISPLAY entry's cmsHPROFILE, which
 * none of them reads. Adding a lock would put one on every call site to protect fields
 * nobody writes. */

/**
 * @brief A profile's public identity: what the GUI displays and stores, and nothing else.
 *
 * @details A plain value -- copy it, put it in GTK object data, outlive anything with it.
 * It carries no `cmsHPROFILE`, so it is unaffected by a monitor-profile change and needs
 * no lock to keep.
 */
typedef struct dt_colorprofile_desc_t
{
  dt_colorspaces_color_profile_type_t type; ///< what to store in params / conf
  char filename[DT_IOP_COLOR_ICC_LEN];  ///< "" unless type == DT_COLORSPACE_FILE
  char name[512];                       ///< translated, display-ready
} dt_colorprofile_desc_t;

/* --- LOCK: pin ONE profile's handle while deriving from it ------------------
 *
 * Per profile, not per module: several pipelines and the GUI derive from profiles at the
 * same time, and a module-wide lock would make a thumbnail conversion against sRGB stand
 * between a monitor-profile change and the display entry.
 *
 * Hold this across "resolve a profile, then derive from it" -- a matrix extraction, a
 * cmsCreateTransform -- and release once the derived artifact no longer refers to the
 * profile. An LCMS transform does not retain its source profiles, so that is the create
 * call, not the transform's lifetime.
 *
 * Read ::dt_colorspaces_color_profile_t::profile only AFTER taking the lock: the entry
 * pointer is stable for the life of the process, but its handle is not. */
void dt_colorspaces_lock_profile(const dt_colorspaces_color_profile_t *const profile);
void dt_colorspaces_unlock_profile(const dt_colorspaces_color_profile_t *const profile);

/**
 * @brief Ordered snapshot of every profile registered for one role.
 *
 * @details `(*out)[k]` is exactly the entry whose legacy `X_pos` was `k` for the
 * single-bit role `X`, so a combo box built by walking this array keeps today's
 * ordering and stays compatible with today's stored indices in presets and conf. A
 * debug-build selftest at init asserts that equivalence against the real installed
 * profile set.
 *
 * @param role which combo box to enumerate. Must be a single bit
 * (DT_PROFILE_ROLE_INPUT / _OUT / _WORK / _DISPLAY) for the index correspondence to
 * mean anything; a multi-bit mask enumerates the union in list order instead.
 * @param out receives a freshly allocated array of `count` descriptors, or NULL. The
 * CALLER owns it and frees it with `dt_free_align`.
 * @return the number of descriptors written. 0 with `*out == NULL` is a legal answer --
 * an empty role, or an allocation failure.
 * @note Value copies: the array stays valid across a monitor-profile change and needs no
 * lock.
 */
size_t dt_colorspaces_enumerate_profiles(const dt_colorspaces_profile_role_t role,
                                         dt_colorprofile_desc_t **out);

/**
 * @brief Combo position of `(type, filename)` within `direction`.
 *
 * @param role MUST be a single bit; anything else returns -1 rather than a
 * meaningless number.
 * @param type profile type to find.
 * @param filename only consulted for DT_COLORSPACE_FILE, and matched with
 * dt_colorspaces_is_profile_equal() so a bare basename stored by an old iop still
 * resolves.
 * @return the 0-based position within that direction's enumeration, or -1 when absent or
 * when `direction` has more than one bit set.
 * @note Callers add their own offset for leading non-profile rows ("same as original",
 * "image settings", ...); this function knows nothing about them.
 */
int dt_colorspaces_profile_index(const dt_colorspaces_profile_role_t role,
                                 const dt_colorspaces_color_profile_type_t type,
                                 const char *const filename);

/**
 * @brief Identity of the profile at `index` within `direction`.
 *
 * @param role MUST be a single bit.
 * @param index 0-based position within that direction's enumeration.
 * @param out filled on success, left completely untouched on failure.
 * @return TRUE on success. FALSE when the index is out of range, negative, or
 * `direction` is not a single bit.
 * @note FALSE is the "the stored choice is no longer installed, fall back" branch
 * expressed as a return value rather than a diagnostic print -- handle it, do not assert
 * on it.
 */
gboolean dt_colorspaces_profile_at(const dt_colorspaces_profile_role_t role,
                                   const int index,
                                   dt_colorprofile_desc_t *const out);

/**
 * @brief Is this identity registered for this direction?
 *
 * @details The one query in this group that accepts a multi-bit mask, because a yes/no
 * answer over a union is still meaningful where an index would not be.
 * @param role one or more direction bits.
 * @param type profile type to test.
 * @param filename only consulted for DT_COLORSPACE_FILE.
 * @return TRUE when some entry serving `direction` matches.
 */
gboolean dt_colorspaces_profile_exists(const dt_colorspaces_profile_role_t role,
                                       const dt_colorspaces_color_profile_type_t type,
                                       const char *const filename);

/**
 * @brief Create a linear-gamma RGB profile from an XYZ->camera matrix.
 * @param cam_xyz the XYZ->camera matrix; it is inverted internally, so the profile
 * describes camera->XYZ.
 * @return a fresh `cmsHPROFILE` the CALLER owns and closes with
 * dt_colorspaces_cleanup_profile(), or NULL when the matrix could not be inverted.
 */
cmsHPROFILE dt_colorspaces_create_xyzimatrix_profile(float cam_xyz[3][3]);

/**
 * @brief Create an ICC virtual profile from the shipped profiled colour matrices.
 * @param makermodel camera identifier, matched case-INsensitively against
 * `dt_profiled_colormatrices`.
 * @return a fresh `cmsHPROFILE` the CALLER owns, or NULL when this camera has no such
 * matrix. Close it with dt_colorspaces_cleanup_profile().
 */
cmsHPROFILE dt_colorspaces_create_darktable_profile(const char *makermodel);

/**
 * @brief Create an ICC virtual profile from the shipped vendor matrices.
 * @param makermodel camera identifier, matched case-SENSITIVELY against
 * `dt_vendor_colormatrices`.
 * @return a fresh `cmsHPROFILE` the CALLER owns, or NULL when absent.
 */
cmsHPROFILE dt_colorspaces_create_vendor_profile(const char *makermodel);

/**
 * @brief Create an ICC virtual profile from the shipped alternate matrices.
 * @param makermodel camera identifier, matched case-SENSITIVELY against
 * `dt_alternate_colormatrices`.
 * @return a fresh `cmsHPROFILE` the CALLER owns, or NULL when absent.
 */
cmsHPROFILE dt_colorspaces_create_alternate_profile(const char *makermodel);

/** return the work profile as set in colorin */


/**
 * @brief Run a caller-owned LCMS transform over one row of RGBA float pixels.
 *
 * @details These two take a transform the CALLER built and owns (iop/colorin.c,
 * iop/colorout.c, colorprofiles/iop_profile.c). For the module's own prepared display
 * transforms use the entry points below instead -- those handles are rebuilt on
 * monitor-profile changes and must never be borrowed.
 *
 * @param transform an lcms2 transform whose input and output formats are 4-channel float.
 * @param in source row, `width` * 4 floats. May be the same buffer as `out`.
 * @param out destination row, `width` * 4 floats.
 * @param width pixels in the row.
 * @warning LCMS transform handles are not safe to rediscover indirectly from mutable owner
 * structs inside OpenMP regions. Alias the `cmsHTRANSFORM` to a local variable BEFORE
 * entering a parallel region, declare that alias shared there, and pass only that stable
 * handle in.
 */
void dt_colorspaces_transform_rgba_float_row(const cmsHTRANSFORM transform, const float *in, float *out,
                                             const int width);

/**
 * @brief Run a caller-owned LCMS transform over a whole RGBA float image, one OpenMP
 * task per row.
 *
 * @param transform an lcms2 transform whose input and output formats are 4-channel float.
 * @param image_in source, `width` * `height` * 4 floats.
 * @param image_out destination, same size.
 * @param width image width in pixels.
 * @param height image height in pixels.
 * @note A NULL transform, a NULL buffer or a non-positive dimension is a no-op, not a
 * crash.
 * @see dt_colorspaces_transform_rgba_float_row for the aliasing rule that applies here too.
 */
void dt_colorspaces_transform_rgba_float_image(const cmsHTRANSFORM transform, const float *image_in, float *image_out,
                                               const int width, const int height);


/* --- prepared display transforms: the cmsHTRANSFORM never leaves the module ---
 *
 * The four cached transforms are deleted and rebuilt whenever the monitor profile or
 * the display intent changes, so a borrowed handle can be freed under its user. These
 * functions take the read lock internally, for the whole conversion -- which is what
 * keeps the handle alive while the pixels are being converted. */

/**
 * @brief Convert one D50 XYZ pixel to display RGB.
 *
 * @param XYZ input pixel, D50-referred XYZ, 4th channel ignored.
 * @param RGB output pixel, display RGB.
 * @note Falls back to `dt_XYZ_to_sRGB()` when no display profile has been resolved yet
 * (startup, headless, or a monitor whose profile could not be read). Two open-coded,
 * byte-identical copies of this function used to dereference the cached handle without a
 * lock and without that check, on a path that repaints on window move and resize -- the
 * very events that free it.
 * @warning One pixel per call, lock included. This is for colour pickers and overlay
 * swatches, not for a pixel loop.
 */
void dt_colorprofiles_xyz_to_display(const dt_aligned_pixel_t XYZ, dt_aligned_pixel_t RGB);

/**
 * @brief Convert a whole 8-bit plane from `src_space` to the display profile: packed
 * RGBA8 in, BGRA8 out (cairo byte order).
 *
 * @details DT_COLORSPACE_SRGB and DT_COLORSPACE_ADOBERGB use the module's prepared
 * transforms; DT_COLORSPACE_DISPLAY is already in display space and passes through with
 * an R <-> B swap; anything else is resolved for DT_PROFILE_ROLE_MONITOR and a
 * transform is built and destroyed inside the call.
 *
 * @param in source plane, `width` * `height` * 4 bytes.
 * @param out destination plane, same size. Alpha is forced to 255.
 * @param width plane width in pixels.
 * @param height plane height in pixels.
 * @param src_space colour space the source is tagged with.
 * @return TRUE when the pixels were colour-managed (or were already in display space);
 * FALSE when no transform could be built and only the byte swap was applied -- a
 * thumbnail cached with an exotic tag that has no DISPLAY-direction profile, for
 * instance.
 * @warning `in` and `out` may be the same buffer only on the colour-managed path, which
 * relies on lcms2 converting in place between same-size formats. On the swap-only
 * fallback the per-pixel swap reads `in[0]` after having written `out[0]`, so an aliased
 * call loses the red channel; both parameters are additionally `restrict`-qualified
 * internally, which declares that they do not alias.
 */
gboolean dt_colorprofiles_rgba8_to_display_bgra8(const uint8_t *const in, uint8_t *const out,
                                                 const int width, const int height,
                                                 const dt_colorspaces_color_profile_type_t src_space);

/**
 * @brief The storage leg: convert an 8-bit plane from `src_space` (BGRA8) to AdobeRGB
 * (RGBA8), for thumbnails written to the mipmap cache.
 *
 * @details DT_COLORSPACE_DISPLAY uses the prepared display->AdobeRGB transform; anything
 * else is resolved for DT_PROFILE_ROLE_MONITOR and a transform is built and
 * destroyed inside the call.
 *
 * @param in source plane, `width` * `height` * 4 bytes.
 * @param out destination plane, same size. Alpha is forced to 255.
 * @param width plane width in pixels.
 * @param height plane height in pixels.
 * @param src_space colour space the source is tagged with.
 * @return TRUE when a transform was applied, FALSE when only the R <-> B swap was.
 * @warning Same aliasing caveat as dt_colorprofiles_rgba8_to_display_bgra8(): safe in
 * place only while a transform exists. common/mipmap_cache.c calls this with `buf, buf`.
 */
gboolean dt_colorprofiles_bgra8_to_adobergb_rgba8(const uint8_t *const in, uint8_t *const out,
                                                  const int width, const int height,
                                                  const dt_colorspaces_color_profile_type_t src_space);

/**
 * @brief Convert a strided, packed-RGB(A) 8-bit buffer (GdkPixbuf shape) from sRGB to the
 * display profile, in place.
 *
 * @details Plain integers only: the module never sees a `GdkPixbuf`. Each row is widened
 * to RGBA8 in per-thread scratch, converted, then written back narrowed and R <-> B
 * swapped.
 *
 * @param pixels first byte of the buffer; converted in place.
 * @param width pixels per row.
 * @param height rows.
 * @param rowstride bytes between the starts of two rows.
 * @param n_channels bytes per pixel, 3 or 4.
 * @param has_alpha whether the 4th channel is alpha to be preserved.
 * @return TRUE on success. FALSE -- leaving the pixels untouched -- on a bad argument,
 * when no display transform is available, or when the scratch allocation fails.
 * @note The scratch for every thread is one allocation made BEFORE the parallel region on
 * purpose: a per-thread allocation that could fail would put the worksharing loop behind
 * a condition some threads take and others do not, which hangs.
 */
gboolean dt_colorprofiles_srgb_to_display_strided(uint8_t *const pixels, const int width, const int height,
                                                  const int rowstride, const int n_channels,
                                                  const gboolean has_alpha);


/* --- display and soft-proofing settings: whole struct in, whole struct out --- */

/**
 * @brief Consistent snapshot of the display and soft-proofing settings.
 *
 * @details The seven fields cross the module boundary only together. Reading them one at
 * a time -- which is what direct member access forced -- lets a reader observe a new
 * profile type paired with the previous filename, and a 512-byte filename read while it
 * is being `g_strlcpy`'d is a TORN string, not merely a stale one. Both groups were read
 * that way by iop/colorout.c (the display triple and the soft-proof pair, as separate
 * unsynchronised loads spread over ~170 lines of `commit_params`) and by iop/filmicrgb.c
 * on pipeline threads, while the GUI thread wrote them.
 *
 * @warning A module that snapshots this for its hash must then RENDER from the same
 * snapshot. iop/filmicrgb.c snapshotted the soft-proof state for `runtime_data_hash` in
 * `commit_params`, then read the live global again from `process()` / `process_cl()`,
 * once per tile -- rendering from state its cache key did not describe.
 */
typedef struct dt_colorprofiles_settings_t
{
  dt_colorspaces_color_mode_t mode;                    ///< NORMAL / SOFTPROOF / GAMUTCHECK
  dt_colorspaces_color_profile_type_t display_type;    ///< monitor profile identity
  char display_filename[DT_IOP_COLOR_ICC_LEN];         ///< only meaningful for DT_COLORSPACE_FILE
  dt_iop_color_intent_t display_intent;                ///< rendering intent to the monitor
  dt_colorspaces_color_profile_type_t softproof_type;  ///< proofing target identity
  char softproof_filename[DT_IOP_COLOR_ICC_LEN];       ///< only meaningful for DT_COLORSPACE_FILE
  dt_iop_color_intent_t softproof_intent;              ///< rendering intent to the proofing target

  /** Advances on every accepted change (any setter that actually changed something), and
   * never wraps in practice. A pipeline module can fold this one number into its hash
   * instead of the individual fields -- cheaper, and it cannot go stale field by field.
   * @note It is global to the group: a soft-proof change bumps it for a module that only
   * cares about the display triple. That costs a recompute, never a wrong render. */
  uint64_t generation;
} dt_colorprofiles_settings_t;

/**
 * @brief Copy the current settings into caller-provided storage, under one lock.
 * @param out destination; ignored when NULL.
 * @note The only supported way to read these seven fields. Take one snapshot and use it
 * for the whole operation.
 */
void dt_colorprofiles_get_settings(dt_colorprofiles_settings_t *const out);

/**
 * @brief Set the monitor profile identity and rebuild the four prepared transforms.
 *
 * @details Identity and transforms are written under the same `_transforms_lock` hold, so a
 * reader can never see one without the other.
 *
 * @param type new display profile type.
 * @param filename only meaningful for DT_COLORSPACE_FILE; NULL is treated as "".
 * @return TRUE when something actually changed, FALSE when the choice was already in
 * effect.
 * @note Returning "did it change" is the point: callers used to decide that for
 * themselves against a value read separately, and re-selecting the ALREADY ACTIVE display
 * profile then reset the user to the system profile through an inherited "profile not
 * found" fallback -- firing on the one case where nothing should happen.
 * @warning Takes `_transforms_lock` for WRITING. Never call it while holding
 * dt_colorspaces_lock_profiles().
 */
gboolean dt_colorprofiles_set_display_profile_choice(const dt_colorspaces_color_profile_type_t type,
                                                     const char *const filename);

/**
 * @brief Set the rendering intent used towards the monitor, rebuilding the four prepared
 * transforms.
 * @param intent new display intent.
 * @return TRUE when it changed.
 * @warning Takes `_transforms_lock` for WRITING; see
 * dt_colorprofiles_set_display_profile_choice().
 */
gboolean dt_colorprofiles_set_display_intent(const dt_iop_color_intent_t intent);

/**
 * @brief Set the soft-proofing target identity.
 * @param type new soft-proof profile type.
 * @param filename only meaningful for DT_COLORSPACE_FILE; NULL is treated as "".
 * @return TRUE when it changed.
 * @note No transform rebuild here: the soft-proof settings feed transforms that
 * iop/colorout.c builds per `commit_params`, and nothing cached in this module derives
 * from them. It therefore takes only the settings lock, not `_transforms_lock`.
 */
gboolean dt_colorprofiles_set_softproof_profile_choice(const dt_colorspaces_color_profile_type_t type,
                                                       const char *const filename);

/**
 * @brief Set the rendering intent used towards the soft-proofing target.
 * @param intent new soft-proof intent.
 * @return TRUE when it changed.
 */
gboolean dt_colorprofiles_set_softproof_intent(const dt_iop_color_intent_t intent);

/**
 * @brief Set the proofing mode outright.
 * @param mode DT_PROFILE_NORMAL, DT_PROFILE_SOFTPROOF or DT_PROFILE_GAMUTCHECK.
 * @return TRUE when it changed.
 * @see dt_colorprofiles_toggle_mode for the "press the button again to leave" behaviour.
 */
gboolean dt_colorprofiles_set_mode(const dt_colorspaces_color_mode_t mode);

/**
 * @brief Turn `mode` on, or back to DT_PROFILE_NORMAL if it is already the current mode,
 * as one locked read-modify-write.
 * @param mode the mode the caller's button stands for.
 * @return the mode now in effect -- which is what the caller should reflect in its UI,
 * rather than assuming its own button won.
 * @note The two toggle buttons each open-coded "read mode, compare, write the opposite",
 * which is not atomic: two accelerator presses in flight could both read
 * DT_PROFILE_NORMAL and leave soft-proof and gamut-check disagreeing about which of them
 * is on.
 */
dt_colorspaces_color_mode_t dt_colorprofiles_toggle_mode(const dt_colorspaces_color_mode_t mode);


/**
 * @brief Open an lcms2 RGB profile from an in-memory ICC blob.
 *
 * @details If the blob is a GRAYSCALE profile, it is closed and a new RGB profile is
 * synthesised from it with the same TRC, black point and white point, and Rec709
 * primaries -- the pipeline has no grayscale path.
 *
 * @param data the ICC blob.
 * @param size its length in bytes.
 * @return a fresh `cmsHPROFILE` the CALLER owns and closes with
 * dt_colorspaces_cleanup_profile(), or NULL when the blob is not a readable profile.
 */
cmsHPROFILE dt_colorspaces_get_rgb_profile_from_mem(uint8_t *data, uint32_t size);

/**
 * @brief Close a profile created by any of the `dt_colorspaces_create_*` /
 * `dt_colorspaces_get_rgb_profile_from_mem` functions.
 * @param p profile handle; NULL is a no-op.
 * @warning Only for profiles the caller OWNS. A handle obtained from
 * dt_colorspaces_get_profile() belongs to the module's list and is closed by
 * dt_colorprofiles_cleanup(); closing it here double-frees at shutdown.
 */
void dt_colorspaces_cleanup_profile(cmsHPROFILE p);

/**
 * @brief Extract the profile->XYZ matrix and the per-channel tone curves from an INPUT
 * profile.
 *
 * @param prof source profile.
 * @param matrix receives the colour matrix, or NULL to only probe whether extraction is
 * possible.
 * @param lutr,lutg,lutb receive `lutsize` samples each, or NULL to only probe. A channel
 * whose TRC is linear is flagged by writing -1.0f into `lut*[0]` and nothing else -- test
 * for it before using the LUT.
 * @param lutsize samples per curve, normally LUT_SAMPLES.
 * @return 0 on success. Non-zero when the profile is not a matrix-shaper, carries a CLUT
 * for any intent (in which case only LCMS may apply it), lacks a required tag, or has an
 * all-zero colorant matrix.
 * @note Curves and matrix are in the input sense; the output variant inverts them.
 */
int dt_colorspaces_get_matrix_from_input_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                 float *lutb, const int lutsize);

/**
 * @brief Extract the XYZ->profile matrix and the inverse tone curves from an OUTPUT
 * profile.
 * @param prof source profile.
 * @param matrix receives the inverted colour matrix, or NULL to only probe.
 * @param lutr,lutg,lutb receive `lutsize` samples of the REVERSED curves, or NULL to only
 * probe.
 * @param lutsize samples per curve, normally LUT_SAMPLES.
 * @return 0 on success, non-zero otherwise; same rejection reasons as the input variant.
 * @note Calling it with every pointer NULL is how init decides whether a loaded .icc
 * qualifies as a working/histogram profile.
 */
int dt_colorspaces_get_matrix_from_output_profile(cmsHPROFILE prof, dt_colormatrix_t matrix, float *lutr, float *lutg,
                                                  float *lutb, const int lutsize);

/**
 * @brief Read a profile's description tag into `name`, handling character encodings.
 * @param p profile to describe.
 * @param language two-letter ISO language code, e.g. "en".
 * @param country two-letter ISO country code, e.g. "US".
 * @param name destination buffer.
 * @param len its size in bytes.
 * @note On failure `name` is set to the empty string rather than left uninitialised, so
 * the caller can test `name[0]`.
 */
void dt_colorspaces_get_profile_name(cmsHPROFILE p, const char *language, const char *country, char *name,
                                     size_t len);

/**
 * @brief Printable name for a profile identity, without touching the profile list.
 * @param type profile type.
 * @param filename returned verbatim for DT_COLORSPACE_FILE, ignored otherwise.
 * @return a translated static string for the built-ins, `filename` for
 * DT_COLORSPACE_FILE, or NULL for DT_COLORSPACE_NONE and DT_COLORSPACE_LAST. Never free
 * it.
 * @note This is a pure switch on the enum, so it also names identities that are NOT
 * registered in the list (the per-image DT_COLORSPACE_EMBEDDED_* / *_MATRIX entries) --
 * which is exactly what error messages about an unresolvable profile need.
 */
const char *dt_colorspaces_get_name(dt_colorspaces_color_profile_type_t type, const char *filename);

/**
 * @brief Convert RGB to HSL. Common helper used by iop modules.
 * @param rgb input pixel, components expected in [0, 1].
 * @param h,s,l receive hue, saturation and lightness, each in [0, 1].
 */
void rgb2hsl(const dt_aligned_pixel_t rgb, float *h, float *s, float *l);

/**
 * @brief Convert HSL back to RGB. Common helper used by iop modules.
 * @param rgb receives the RGB pixel; the 4th channel is left alone.
 * @param h,s,l hue, saturation and lightness, each in [0, 1].
 */
void hsl2rgb(dt_aligned_pixel_t rgb, float h, float s, float l);

/**
 * @brief Build a container for a profile that belongs to ONE image rather than to the
 * application.
 *
 * @details An embedded ICC profile is a property of its image, so the image owns it
 * (`dt_image_t.embedded_profile`). It is deliberately NOT appended to the module's list:
 * that list is built once at init and read from ~23 places with no lock, and appending to
 * it from parallel export jobs was an unsynchronised write, unbounded growth, and a leak
 * all at once.
 *
 * @param type profile type this container stands for.
 * @param profile the LCMS2 handle to wrap.
 * @param owns_profile whether the container must CLOSE that handle when freed. Pass FALSE
 * when the profile is borrowed from the application-wide list, which owns and closes it.
 * @return a container the caller owns, freed with dt_colorspaces_free_image_profile().
 * @note Every `*_pos` is -1, so it is hidden from every combo box by construction.
 * @warning `owns_profile` is not cosmetic. Several branches of
 * dt_image_find_best_color_profile() hand back a pointer INTO the application list and
 * leave their `new_profile` out-parameter FALSE; passing TRUE for one of those
 * double-frees at shutdown.
 */
struct dt_colorspaces_color_profile_t *dt_colorspaces_new_image_profile(
    dt_colorspaces_color_profile_type_t type, cmsHPROFILE profile, gboolean owns_profile);

/**
 * @brief Release a profile container owned by an image, closing the LCMS2 handle inside it
 * if and only if the container owns it.
 * @param profile container to free; NULL is a no-op.
 * @note Called by the image cache when the image is evicted; nothing else should need it.
 * Declared here so common/image_cache.c does not need the struct layout.
 */
void dt_colorspaces_free_image_profile(struct dt_colorspaces_color_profile_t *profile);

/** @brief Callback invoked after the monitor profile actually changed. */
typedef void (*dt_colorspaces_profile_changed_handler_t)(void);

/**
 * @brief Register the one callback fired when the display profile changes.
 * @param handler the callback, or NULL to unregister.
 * @note The application relays it on its signal bus; this module does not know there is
 * one. Unregistered, the notification is dropped -- correct for a headless run, where
 * nothing is watching a monitor.
 * @warning It fires from wherever the change was detected, including the colord async
 * callback, and it fires AFTER `_transforms_lock` has been released -- so a handler may take
 * the lock, but must not assume it already holds it.
 */
void dt_colorspaces_set_profile_changed_handler(dt_colorspaces_profile_changed_handler_t handler);

/** trigger updating the display profile from the system settings (x atom, colord, ...) */
/**
 * @brief Refresh the cached display profile from the monitor showing `widget` (X atom,
 * colord, or the platform equivalent).
 *
 * @details On a real change it replaces the DT_COLORSPACE_DISPLAY entry's `cmsHPROFILE`,
 * rebuilds the four prepared transforms, drops the derived matrix/LUT memo and fires the
 * changed handler. Nothing happens when the bytes read match the ones already cached.
 *
 * @param profile_type the display profile type in effect; passed through to the colord
 * callback.
 * @param widget any realized widget on the monitor to inspect; NULL returns immediately.
 * The caller owns the window -- this module never asks the GUI which one to look at.
 * @warning It acquires `_transforms_lock` with `trywrlock` and RETURNS SILENTLY if that
 * fails, because it is called from window move/resize handlers. A refresh is therefore
 * best-effort, not guaranteed; never rely on the profile having been updated when this
 * returns.
 */
void dt_colorspaces_set_display_profile(const dt_colorspaces_color_profile_type_t profile_type,
                                       GtkWidget *widget);

/**
 * @brief Resolve a profile identity to its registered entry.
 *
 * @param type profile type to find.
 * @param filename only consulted for DT_COLORSPACE_FILE, matched with
 * dt_colorspaces_is_profile_equal().
 * @param role direction mask; the first entry in registration order that serves any
 * of its bits and matches the identity wins.
 * @return a pointer into the module's list, owned by the module and valid until
 * dt_colorprofiles_cleanup() -- do NOT close its `profile`. NULL when nothing matches.
 * @warning This does not support image specifics: embedded profiles and camera matrices
 * (DT_COLORSPACE_EMBEDDED_ICC .. DT_COLORSPACE_ALTERNATE_MATRIX) are not registered and
 * always return NULL, as do the three category types.
 * @warning When `type` can be DT_COLORSPACE_DISPLAY, wrap the resolve AND everything
 * derived from `->profile` in dt_colorspaces_lock_profiles() /
 * dt_colorspaces_unlock_profiles(): that handle is closed and replaced on monitor changes.
 * @warning A multi-bit `direction` returns the first match in registration order, which
 * for DT_COLORSPACE_SRGB is the v4 INPUT-only entry -- so DT_PROFILE_ROLE_ANY
 * resolves the working profile to the wrong sRGB variant. Name the direction you mean.
 */
const dt_colorspaces_color_profile_t *
dt_colorspaces_get_profile(dt_colorspaces_color_profile_type_t type, const char *filename,
                           dt_colorspaces_profile_role_t role);

/**
 * @brief Do these two names refer to the same profile file?
 * @param fullname the registered entry's name, always a full path.
 * @param filename the stored name, which may be a full path or just a base name.
 * @return TRUE when they match.
 * @note The basename leniency exists for backward compatibility: older iop params recorded
 * only the base name.
 */
gboolean  dt_colorspaces_is_profile_equal(const char *fullname, const char *filename);


/**
 * @brief Delete and rebuild the four prepared display transforms from the current display
 * profile and intent.
 * @warning The caller must already hold the module's profile lock FOR WRITING. There is no
 * public way to do that -- dt_colorspaces_lock_profiles() is a read lock, and the instance
 * itself is private -- so in practice this is only callable from inside the module, and the
 * setters above (dt_colorprofiles_set_display_profile_choice(),
 * dt_colorprofiles_set_display_intent()) are what external code should use. It has no
 * remaining callers outside colorspaces.c.
 */
void dt_colorspaces_update_display_transforms();

/**
 * @brief Compute the XYZ->camera and camera->XYZ matrices for an image.
 *
 * @param adobe_XYZ_to_CAM the camera's built-in Adobe matrix, used only as the fallback.
 * @param in_XYZ_to_CAM 9 floats read from the file (a DNG ColorMatrix, say). When
 * `in_XYZ_to_CAM[0]` is NaN the Adobe matrix is used instead; otherwise this one wins.
 * @param XYZ_to_CAM receives the forward matrix; the 4th row is zeroed when it came from
 * the 9-float form.
 * @param CAM_to_XYZ receives the pseudo-inverse.
 * @return TRUE on success, FALSE when neither source matrix is usable (both NaN).
 * @note 4 rows because the camera may be 4-colour (CYGM, RGBE).
 */
int dt_colorspaces_conversion_matrices_xyz(const float adobe_XYZ_to_CAM[4][3], float in_XYZ_to_CAM[9], double XYZ_to_CAM[4][3], double CAM_to_XYZ[3][4]);

/**
 * @brief Compute the sRGB->camera and camera->sRGB matrices, and the default white
 * balance multipliers.
 *
 * @details Converted from dcraw's `cam_xyz_coeff()`. Rows of RGB->CAM are normalised so
 * that RGB->CAM applied to (1,1,1) gives (1,1,1,1), and the multipliers fall out of that
 * normalisation.
 *
 * @param adobe_XYZ_to_CAM the camera's built-in Adobe matrix.
 * @param RGB_to_CAM receives the forward matrix, or NULL if not wanted.
 * @param CAM_to_RGB receives the pseudo-inverse, or NULL if not wanted.
 * @param embedded_matrix 9 floats from the file; takes PRIORITY over the Adobe matrix
 * when non-NULL and not NaN. Keep in sync with `reload_defaults` in iop/colorin.c.
 * @param mul receives the 4 default WB multipliers, or NULL if not wanted.
 * @return TRUE on success, FALSE when no usable matrix was available.
 */
/* RGB_to_CAM, CAM_to_RGB and mul are OPTIONAL -- the implementation guards each with a NULL
 * check, and both in-tree callers pass NULL for the first two. They are therefore declared as
 * pointers, NOT as double[4][3] / double[3][4] / double[4]. A declared array bound on a
 * parameter is an access contract to GCC (-Wstringop-overflow reads it as "must point to at
 * least this many elements"), so the array spelling asserts a minimum size that NULL cannot
 * satisfy, and every call warned. Do not restore the array bounds. */
/* adobe_XYZ_to_CAM is 12 consecutive floats, row-major, 4 rows of 3 -- pass &m[0][0].
 *
 * It is a FLAT pointer rather than float[4][3] on purpose. As a 2D array parameter it decays
 * to pointer-to-row, and GCC then sizes the accessible region as a single row and reports
 * every call as an overflow: "accessing 48 bytes in a region of size 12". The 48 is right and
 * the 12 is not -- a _Static_assert on sizeof(((dt_image_t *)0)->adobe_XYZ_to_CAM) == 48
 * compiles clean. Four other spellings were tried and none silenced it: pointer-to-row, the
 * C99 [static 4][3] bound, a #pragma GCC diagnostic at the call site (the warning comes from
 * the middle end, which ignores it), and __attribute__((noclone)).
 *
 * RGB_to_CAM, CAM_to_RGB and mul are OPTIONAL -- each is NULL-checked, and both in-tree
 * callers pass NULL for the first two. They are pointers for the same reason: a declared
 * array bound is an access contract GCC reads as "must point to at least this many
 * elements", which NULL cannot satisfy. Do not restore array bounds to any of these. */
int dt_colorspaces_conversion_matrices_rgb(const float *adobe_XYZ_to_CAM, double (*RGB_to_CAM)[3], double (*CAM_to_RGB)[4], const float *embedded_matrix, double *mul);

/**
 * @brief Apply CYGM white-balance coefficients to an image already converted to RGB by
 * dt_colorspaces_cygm_to_rgb().
 * @param out destination, `num` pixels of 4 floats; only the first 3 channels are written.
 * @param in source, `num` pixels of 4 floats.
 * @param num pixel count.
 * @param RGB_to_CAM forward matrix from dt_colorspaces_conversion_matrices_rgb().
 * @param CAM_to_RGB its pseudo-inverse.
 * @param coeffs the 4 per-camera-channel WB coefficients.
 * @warning Dead code: verified to have no caller anywhere in the tree. Kept because the
 * CYGM WB path is otherwise unimplemented, not because it is exercised.
 */
// FIXME: CRITICAL: why is this function NOT used anywhere ???
void dt_colorspaces_cygm_apply_coeffs_to_rgb(float *out, const float *in, int num, double RGB_to_CAM[4][3], double CAM_to_RGB[3][4], dt_aligned_pixel_t coeffs);

/**
 * @brief Convert a 4-channel CYGM buffer to RGB, in place.
 * @param out buffer of `num` pixels, stride 4 floats; channels 0..2 are overwritten with
 * RGB and channel 3 is left as it was.
 * @param num pixel count.
 * @param CAM_to_RGB matrix from dt_colorspaces_conversion_matrices_rgb().
 */
void dt_colorspaces_cygm_to_rgb(float *out, int num, double CAM_to_RGB[3][4]);

/**
 * @brief Convert an RGB buffer to 4-channel CYGM, in place.
 * @param out buffer of `num` pixels; it is READ with stride 3 and WRITTEN with 4
 * components per pixel, so it must have room for `num` * 4 floats and the two strides do
 * not agree for `num > 1`. The only caller (iop/invert.c) passes `num == 1`, where the
 * mismatch cannot bite.
 * @param num pixel count.
 * @param RGB_to_CAM matrix from dt_colorspaces_conversion_matrices_rgb().
 */
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
