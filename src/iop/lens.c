/*
    This file is part of darktable,
    Copyright (C) 2009-2013, 2016 johannes hanika.
    Copyright (C) 2010 Alexandre Prokoudine.
    Copyright (C) 2010-2011 Bruce Guenter.
    Copyright (C) 2010-2011, 2013 Henrik Andersson.
    Copyright (C) 2010 Milan Knížek.
    Copyright (C) 2010, 2013-2014 Pascal de Bruijn.
    Copyright (C) 2010 Stuart Henderson.
    Copyright (C) 2010 Thierry Leconte.
    Copyright (C) 2011, 2013 Antony Dovgal.
    Copyright (C) 2011-2012 Jérémy Rosen.
    Copyright (C) 2011 Olivier Tribout.
    Copyright (C) 2011 Robert Bieber.
    Copyright (C) 2011 Rostyslav Pidgornyi.
    Copyright (C) 2011-2014, 2016-2019 Tobias Ellinghaus.
    Copyright (C) 2012 Edouard Gomez.
    Copyright (C) 2012-2013 Gabriel Ebner.
    Copyright (C) 2012, 2015, 2019 parafin.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2012 Sergey Pavlov.
    Copyright (C) 2012-2014, 2016-2017 Ulrich Pegelow.
    Copyright (C) 2013, 2020-2021 Aldric Renaudin.
    Copyright (C) 2013 Guilherme Brondani Torri.
    Copyright (C) 2013 Ivan Tarozzi.
    Copyright (C) 2013-2016 Roman Lebedev.
    Copyright (C) 2013 Simon Spannagel.
    Copyright (C) 2013 Thomas Pryds.
    Copyright (C) 2013-2015 Torsten Bronger.
    Copyright (C) 2015 Pedro Côrte-Real.
    Copyright (C) 2016, 2018-2022 Pascal Obry.
    Copyright (C) 2017 Heiko Bauke.
    Copyright (C) 2018-2026 Aurélien PIERRE.
    Copyright (C) 2018 Edgardo Hoszowski.
    Copyright (C) 2018 Kelvie Wong.
    Copyright (C) 2018 Maurizio Paglia.
    Copyright (C) 2018 Peter Budai.
    Copyright (C) 2018, 2021 rawfiner.
    Copyright (C) 2019 Andreas Schneider.
    Copyright (C) 2019 David-Tillmann Schaefer.
    Copyright (C) 2019 Diederik ter Rahe.
    Copyright (C) 2019 Jakub Filipowicz.
    Copyright (C) 2019 Kevin Daudt.
    Copyright (C) 2020-2021 Chris Elston.
    Copyright (C) 2020-2022 Diederik Ter Rahe.
    Copyright (C) 2020-2022 Hanno Schwalm.
    Copyright (C) 2020 Hubert Kowalski.
    Copyright (C) 2020-2021 Ralf Brown.
    Copyright (C) 2021 fvollmer.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Nicolas Auffray.
    Copyright (C) 2022 Philipp Lutz.
    Copyright (C) 2024-2025 Alynx Zhou.
    
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
#include "common/global_mutexes.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "common/utility.h"
#include "system/macros.h"
#include "common/module_versioning.h"
#include "common/logging.h"
#include "system/mem_alloc.h"
#include "system/openmp.h"
#include "system/target_clones.h"
#include "caches/pixelpipe_cache_alloc.h"
#include "glib.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "widgets/bauhaus.h"
#include "pixel/interpolation.h"
#include "common/file_location.h"
#include "common/imagebuf.h"
#include "common/opencl.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"

#include "iop/iop_api.h"
#include <assert.h>
#include <ctype.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "widgets/popup.h"
#include "widgets/widget_style.h"
#include "control/signal.h"

#include "develop/geometry/geometry.h"

#include "lensserious.h"    // side-by-side latch against lensfun, see feat/lensserious
#include "lensserious_db.h" // ... and its calibration database, latched the same way
#include "lensserious_vendor.h"


/* The correction axes and the projection numbering.
 *
 * These values are lensfun's, and they must stay lensfun's: `modify_flags` and
 * `target_geom` are SERIALIZED into every user's history and every preset that has ever
 * been saved. Defining them here rather than including them is what lets liblensfun go
 * without rewriting anyone's edits.
 *
 * The projection numbering is also, entry for entry, ls_lens_type_t's -- asserted below
 * rather than assumed, because the two now live in different repositories and nothing else
 * would notice one of them growing a member in the middle. */
/**
 * @brief What `modify_flags` holds, bit by bit.
 *
 * @details ONE 32-bit int, serialized into every user's history since Ansel began, now
 * carrying two unrelated things. This comment and the accessors below are the only places
 * that know how; nothing else in the file touches a bit of it.
 *
 * @verbatim
 *   bit  31 30 │ 29 28 │ 27 26 │ 25 24 │ 23 ... 6 │  5   4   3   2   1   0
 *        ──┬── │ ──┬── │ ──┬── │ ──┬── │ ──┬───── │  │   │   │   │   │   │
 *          │   │   │   │   │   │   │   │   │      │  │   │   │   │   │   └ TCA
 *          │   │   │   │   │   │   │   │   │      │  │   │   │   │   └──── VIGNETTING
 *          │   │   │   │   │   │   │   │   │      │  │   │   │   └──────── (unused)
 *          │   │   │   │   │   │   │   │   │      │  │   │   └──────────── DISTORTION
 *          │   │   │   │   │   │   │   │   │      │  │   └──────────────── GEOMETRY
 *          │   │   │   │   │   │   │   │   │      │  └──────────────────── SCALE
 *          │   │   │   │   │   │   │   │   └ free for a future axis
 *          │   │   │   │   │   │   └ TCA's source
 *          │   │   │   │   └ DISTORTION's source
 *          │   │   └ VIGNETTING's source
 *          └ free; 31 is the SIGN BIT and must stay clear
 * @endverbatim
 *
 * **Low half, bits 0..23 -- WHICH corrections run.** These five values are lensfun's own,
 * and they must stay lensfun's: they are in every history and every preset ever saved.
 * (Bit 2 is skipped because upstream skips it.) A future axis goes here too, and every mask
 * below keeps working without being edited.
 *
 * **High half, bits 24..31 -- WHERE each correction comes from.** Two bits per axis, values
 * from ::dt_lens_source_t. Ansel's own, growing down from the top so the two halves can
 * each grow for a long time before meeting.
 *
 * **Zero means the lens database.** Every edit ever saved has the high half clear, so clear
 * has to keep decoding to what those edits meant. The opposite polarity would behave
 * identically right up until someone opened an old edit.
 *
 * Do not read or write any of this directly. `_lens_source_get()`, `_lens_source_set()` and
 * the `_lens_*` predicates below are the interface, and they exist because the encoding has
 * three traps in it: a shift whose width is not the axis's width, an axis whose enable bit
 * is really three bits, and a legacy boolean that has to stay in step with one of the
 * fields.
 */
typedef enum dt_lens_modify_t
{
  /* --- low half: lensfun's serialized axis numbering, unchangeable --- */
  DT_LENS_MODIFY_TCA        = 0x00000001,
  DT_LENS_MODIFY_VIGNETTING = 0x00000002,
  DT_LENS_MODIFY_DISTORTION = 0x00000008,
  DT_LENS_MODIFY_GEOMETRY   = 0x00000010,
  DT_LENS_MODIFY_SCALE      = 0x00000020,

  /* --- high half: how far up sits each axis's two-bit source field --- */
  DT_LENS_SOURCE_SHIFT_TCA        = 24,
  DT_LENS_SOURCE_SHIFT_DISTORTION = 26,
  DT_LENS_SOURCE_SHIFT_VIGNETTING = 28,
  DT_LENS_SOURCE_BITS             = 0x3,
} dt_lens_modify_t;

/**
 * @brief The three lensfun axes that always move together, as one flag.
 *
 * @details Distortion, projection and scaling are three bits because lensfun numbers them
 * separately and that numbering is serialized. They are ONE correction: nothing has ever
 * offered them apart -- reload_defaults() sets all three, the old GUI's mask preserved them
 * untouched, and no code path clears one without the others -- and the profile a camera
 * embeds in a raw file describes the lot with a single curve. So they are set, read and
 * tested as a pack, and the DISTORTION bit is what says whether the pack is on.
 */
#define DT_LENS_MODIFY_DISTORTION_PACK \
  (DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE)

/** Everything Ansel stores in `modify_flags` that is not an axis. */
#define DT_LENS_MODIFY_ANSEL_MASK 0xFF000000u

/**
 * Every correction axis -- the ones that exist and the ones that do not yet.
 *
 * A macro rather than another `dt_lens_modify_t` member for a reason that would otherwise
 * bite silently: 0xFF000000 does not fit in a signed int, so an enumerator of that value
 * makes the whole enumeration's underlying type implementation-defined, and with it the
 * type of every constant beside it.
 */
#define DT_LENS_MODIFY_ALL_AXES ((int)~DT_LENS_MODIFY_ANSEL_MASK)

/* The boundary, checked rather than described. Each of these is a way the two halves could
 * come to overlap -- a sixth axis assigned too high, a source field assigned too low, a
 * field running into the sign bit -- and each would corrupt the other half's meaning in a
 * way that only shows up as a wrong correction on someone's photograph. */
_Static_assert((DT_LENS_MODIFY_ALL_AXES & (int)DT_LENS_MODIFY_ANSEL_MASK) == 0,
               "the axis space and Ansel's own bits must not overlap");
_Static_assert((DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_VIGNETTING | DT_LENS_MODIFY_DISTORTION_PACK)
                   == ((DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_VIGNETTING
                        | DT_LENS_MODIFY_DISTORTION_PACK) & DT_LENS_MODIFY_ALL_AXES),
               "every correction axis must live in the axis half");
_Static_assert((DT_LENS_SOURCE_BITS << DT_LENS_SOURCE_SHIFT_VIGNETTING) > 0,
               "no source field may reach the sign bit of modify_flags");
_Static_assert(DT_LENS_SOURCE_SHIFT_DISTORTION - DT_LENS_SOURCE_SHIFT_TCA >= 2
                   && DT_LENS_SOURCE_SHIFT_VIGNETTING - DT_LENS_SOURCE_SHIFT_DISTORTION >= 2,
               "the source fields must not overlap each other");

/** @brief The correction axes, as an index rather than a bit -- so a caller can loop. */
typedef enum dt_lens_axis_t
{
  DT_LENS_AXIS_TCA = 0,
  DT_LENS_AXIS_DISTORTION,
  DT_LENS_AXIS_VIGNETTING,
  DT_LENS_AXIS_LAST
} dt_lens_axis_t;

/**
 * @brief Where one axis takes its correction from. The same vocabulary for all three.
 *
 * @details OFF is stored as the axis's own enable bit being clear, and the rest in that
 * axis's two source bits at the top of the word. Splitting it across the two halves is not
 * elegance, it is compatibility: the enable bits are lensfun's and are already in
 * everyone's history, so they keep their meaning and the new information goes somewhere
 * that was empty in every edit ever saved.
 *
 * ZERO THEREFORE MEANS LENSFUN, and that is what makes this shippable without bumping the
 * params version. The opposite polarity would behave identically until someone opened an
 * old edit.
 *
 * Not every value is legal on every axis -- only TCA can be MANUAL, because only TCA has
 * coefficients a user can type. _lens_source_applicable() is the one place that knows which,
 * so the GUI and the pixel path cannot disagree about it.
 */
typedef enum dt_lens_source_t
{
  DT_LENS_SOURCE_OFF = 0,   /**< no correction on this axis */
  DT_LENS_SOURCE_LENSFUN,   /**< the calibration database. The value old edits decode to. */
  DT_LENS_SOURCE_EMBEDDED,  /**< the profile the camera wrote into the raw file */
  DT_LENS_SOURCE_MANUAL,    /**< coefficients the user typed. TCA only. */
  DT_LENS_SOURCE_LAST
} dt_lens_source_t;

typedef enum dt_lens_type_t
{
  DT_LENS_UNKNOWN = 0,
  DT_LENS_RECTILINEAR = 1,
  DT_LENS_FISHEYE = 2,
  DT_LENS_PANORAMIC = 3,
  DT_LENS_EQUIRECTANGULAR = 4,
  DT_LENS_FISHEYE_ORTHOGRAPHIC = 5,
  DT_LENS_FISHEYE_STEREOGRAPHIC = 6,
  DT_LENS_FISHEYE_EQUISOLID = 7,
  DT_LENS_FISHEYE_THOBY = 8,
} dt_lens_type_t;

_Static_assert((int)DT_LENS_RECTILINEAR == (int)LS_LENS_RECTILINEAR
                  && (int)DT_LENS_FISHEYE == (int)LS_LENS_FISHEYE
                  && (int)DT_LENS_PANORAMIC == (int)LS_LENS_PANORAMIC
                  && (int)DT_LENS_EQUIRECTANGULAR == (int)LS_LENS_EQUIRECTANGULAR
                  && (int)DT_LENS_FISHEYE_ORTHOGRAPHIC == (int)LS_LENS_FISHEYE_ORTHOGRAPHIC
                  && (int)DT_LENS_FISHEYE_STEREOGRAPHIC == (int)LS_LENS_FISHEYE_STEREOGRAPHIC
                  && (int)DT_LENS_FISHEYE_EQUISOLID == (int)LS_LENS_FISHEYE_EQUISOLID
                  && (int)DT_LENS_FISHEYE_THOBY == (int)LS_LENS_FISHEYE_THOBY,
              "projection numbering must match ls_lens_type_t: stored params depend on it");

DT_MODULE_INTROSPECTION(5, dt_iop_lensfun_params_t)

typedef enum dt_iop_lensfun_modflag_t
{
  /* The three axes a user can actually see corrected. Geometry and scale are not in it:
   * they are how the correction is presented, not whether a lens flaw was fixed. */
  LENSFUN_MODFLAG_MASK = DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_VIGNETTING
} dt_iop_lensfun_modflag_t;


typedef struct dt_iop_lensfun_params_t
{
  int modify_flags;
  int inverse; // $MIN: 0 $MAX: 1 $DEFAULT: 0 $DESCRIPTION: "mode"
  float scale; // $MIN: 0.1 $MAX: 2.0 $DEFAULT: 1.0
  float crop;
  float focal;
  float aperture;
  float distance;
  dt_lens_type_t target_geom; // $DEFAULT: DT_LENS_RECTILINEAR $DESCRIPTION: "geometry"
  char camera[128];
  char lens[128];
  gboolean tca_override; // $DEFAULT: FALSE $DESCRIPTION: "TCA overwrite"
  float tca_r; // $MIN: 0.99 $MAX: 1.01 $DEFAULT: 1.0 $DESCRIPTION: "TCA red"
  float tca_b; // $MIN: 0.99 $MAX: 1.01 $DEFAULT: 1.0 $DESCRIPTION: "TCA blue"
  /** Whether this edit's correction is described by ITS OWN parameters (1) or deferred to
   *  whatever reload_defaults() computes at render time (0).
   *
   *  Defaults to 1, and the 0 case is legacy only. See _lens_effective_params(). */
  int modified; // $DEFAULT: 1
} dt_iop_lensfun_params_t;

/* ========================================================================================
 * The modify_flags interface.
 *
 * Everything below this banner and above the next one is the only code in this file allowed
 * to touch a bit of modify_flags. Everything else asks in terms of an axis and a source.
 *
 * That rule is not tidiness. The encoding has three traps, and each has already been walked
 * into once: the source fields are two bits at a shift that is not the axis's bit position;
 * the distortion "bit" is really three bits, so clearing the obvious one leaves projection
 * and scaling running with their controls hidden; and manual TCA is ALSO recorded in a
 * separate legacy boolean, so writing one without the other makes an edit mean two different
 * things depending on which the reader trusts.
 * ===================================================================================== */

/**
 * @brief The enable bits for one axis. Three of them for distortion; see the pack.
 */
static inline int _lens_axis_flags(const dt_lens_axis_t axis)
{
  switch(axis)
  {
    case DT_LENS_AXIS_TCA:         return DT_LENS_MODIFY_TCA;
    case DT_LENS_AXIS_DISTORTION:  return DT_LENS_MODIFY_DISTORTION_PACK;
    case DT_LENS_AXIS_VIGNETTING:  return DT_LENS_MODIFY_VIGNETTING;
    default:                       return 0;
  }
}

/**
 * @brief The single bit that says whether an axis is on.
 *
 * @details Not the same as _lens_axis_flags() for distortion: the pack is three bits, and
 * asking "is any of them set" would call an old edit's leftover GEOMETRY bit an enabled
 * distortion correction. The DISTORTION bit alone is the answer; the other two ride with it.
 */
static inline int _lens_axis_presence_bit(const dt_lens_axis_t axis)
{
  return (axis == DT_LENS_AXIS_DISTORTION) ? DT_LENS_MODIFY_DISTORTION : _lens_axis_flags(axis);
}

/** @brief How far up modify_flags this axis's two source bits sit. */
static inline int _lens_axis_shift(const dt_lens_axis_t axis)
{
  switch(axis)
  {
    case DT_LENS_AXIS_TCA:         return DT_LENS_SOURCE_SHIFT_TCA;
    case DT_LENS_AXIS_DISTORTION:  return DT_LENS_SOURCE_SHIFT_DISTORTION;
    case DT_LENS_AXIS_VIGNETTING:  return DT_LENS_SOURCE_SHIFT_VIGNETTING;
    default:                       return DT_LENS_SOURCE_SHIFT_TCA;
  }
}

/**
 * @brief Whether @p source means anything on @p axis.
 *
 * @details MANUAL is TCA's alone: it is the only axis whose correction is two numbers a user
 * can reasonably type. The others would need a whole polynomial. The GUI asks this to decide
 * which rows to offer and the read path asks it to decide what an out-of-range stored value
 * decodes to, so the two cannot drift apart.
 */
static inline gboolean _lens_source_applicable(const dt_lens_axis_t axis,
                                               const dt_lens_source_t source)
{
  if(source == DT_LENS_SOURCE_MANUAL) return axis == DT_LENS_AXIS_TCA;
  return source > DT_LENS_SOURCE_OFF && source < DT_LENS_SOURCE_LAST;
}

/**
 * @brief Decode one axis's source out of a raw flag word.
 *
 * @param modify_flags the stored word.
 * @param tca_override the legacy boolean that is the ONLY record of manual TCA in an edit
 * saved before the source fields existed. Reading it here is what makes such an edit open as
 * MANUAL rather than as a database correction with two mysterious sliders beside it.
 *
 * @details Takes the two fields rather than a params block because the pixel path holds them
 * in ::dt_iop_lensfun_data_t, which is not a params block. One decode, two callers.
 */
static dt_lens_source_t _lens_source_decode(const int modify_flags,
                                            const gboolean tca_override,
                                            const dt_lens_axis_t axis)
{
  if(!(modify_flags & _lens_axis_presence_bit(axis))) return DT_LENS_SOURCE_OFF;

  dt_lens_source_t source
      = (dt_lens_source_t)((modify_flags >> _lens_axis_shift(axis)) & DT_LENS_SOURCE_BITS);

  /* Zero is the database, because that is what every edit written before this decoded to. */
  if(source == DT_LENS_SOURCE_OFF) source = DT_LENS_SOURCE_LENSFUN;

  if(axis == DT_LENS_AXIS_TCA && source == DT_LENS_SOURCE_LENSFUN && tca_override)
    source = DT_LENS_SOURCE_MANUAL;

  /* A value this build has no meaning for -- a newer Ansel wrote it, or the field was
   * corrupted. Correcting with the database beats refusing to correct at all. */
  if(!_lens_source_applicable(axis, source)) source = DT_LENS_SOURCE_LENSFUN;

  return source;
}

/** @brief Where this axis takes its correction from. */
static inline dt_lens_source_t _lens_source_get(const dt_iop_lensfun_params_t *p,
                                                const dt_lens_axis_t axis)
{
  return _lens_source_decode(p->modify_flags, p->tca_override, axis);
}

/**
 * @brief Point one axis at a source, leaving every other axis alone.
 *
 * @details Setting a source clears the others by construction -- they are one field, not
 * three flags -- so there is no illegal combination to guard against and no invariant a
 * caller can forget to restore.
 *
 * An inapplicable source is stored as OFF rather than silently corrected to something
 * plausible: a caller asking for manual vignetting has a bug, and turning the axis off makes
 * it visible instead of hiding it behind a database correction nobody asked for.
 */
static void _lens_source_set(dt_iop_lensfun_params_t *p, const dt_lens_axis_t axis,
                             dt_lens_source_t source)
{
  if(!_lens_source_applicable(axis, source)) source = DT_LENS_SOURCE_OFF;

  p->modify_flags &= ~(DT_LENS_SOURCE_BITS << _lens_axis_shift(axis));

  if(source == DT_LENS_SOURCE_OFF)
  {
    /* The whole pack for distortion: leaving GEOMETRY and SCALE set is what used to keep a
     * projection change and a zoom running after their correction had been switched off. */
    p->modify_flags &= ~_lens_axis_flags(axis);
    /* The source bits stay cleared, so an axis switched off and on again comes back as the
     * database rather than as whatever it was before. */
  }
  else
  {
    p->modify_flags |= _lens_axis_flags(axis);
    p->modify_flags |= ((int)source & DT_LENS_SOURCE_BITS) << _lens_axis_shift(axis);
  }

  /* Kept in step rather than left to rot: an older Ansel reading this edit sees only
   * tca_override, and it should still see the truth. */
  if(axis == DT_LENS_AXIS_TCA)
    p->tca_override = (source == DT_LENS_SOURCE_MANUAL) ? TRUE : FALSE;
}

/** @brief Is this axis taking its correction from @p source? */
static inline gboolean _lens_source_is(const dt_iop_lensfun_params_t *p,
                                       const dt_lens_axis_t axis,
                                       const dt_lens_source_t source)
{
  return _lens_source_get(p, axis) == source;
}

/** @brief Is this axis correcting at all, from wherever? */
static inline gboolean _lens_axis_enabled(const dt_iop_lensfun_params_t *p,
                                          const dt_lens_axis_t axis)
{
  return _lens_source_get(p, axis) != DT_LENS_SOURCE_OFF;
}

/**
 * @brief Does this flag word describe anything that MOVES pixels?
 *
 * @details Vignetting is a gain and leaves geometry alone, so a mask that includes it would
 * make distort_transform() claim it displaces points when it does not. Every caller wants
 * this question and several used to spell out the same four-term test, which is how one of
 * them came to be missing a term.
 */
static inline gboolean _lens_flags_move_pixels(const int modify_flags)
{
  return (modify_flags & (DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_DISTORTION_PACK)) != 0;
}

/**
 * @brief Is this source served by the LensSerious path, once the data has been prepared?
 *
 * @details MANUAL counts, and that is the whole trick of it. _lens_build_data() replaces the
 * lens's own TCA calibration with a linear one built from the user's two coefficients, so by
 * the time the library is asked there is nothing manual left to know about -- it is an
 * ordinary lens with ordinary terms, and asking for LS_ENABLE_TCA is exactly right.
 *
 * EMBEDDED counts too, since commit_params resolves the maker's table into the pipe data
 * the same way; OFF is the only source with nothing behind it.
 */
static inline gboolean _lens_source_is_library_served(const dt_lens_source_t source)
{
  return source == DT_LENS_SOURCE_LENSFUN || source == DT_LENS_SOURCE_MANUAL
         || source == DT_LENS_SOURCE_EMBEDDED;
}

/** @brief Is this axis present in a flag word -- typically get_modifier()'s `done`? */
static inline gboolean _lens_flags_have_axis(const int modify_flags, const dt_lens_axis_t axis)
{
  return (modify_flags & _lens_axis_presence_bit(axis)) != 0;
}

/**
 * @brief Which axes may be attempted on this image.
 *
 * @details A monochrome sensor has no colour channels to misalign, so lateral chromatic
 * aberration is not a correction that means anything there -- upstream refuses it and so do
 * we. Everything else is unrestricted, including an axis nobody has defined yet.
 */
static inline int _lens_mask_for_mono(const gboolean monochrome)
{
  return monochrome ? (DT_LENS_MODIFY_ALL_AXES & ~DT_LENS_MODIFY_TCA) : DT_LENS_MODIFY_ALL_AXES;
}


/* ===================================================================================== */

/**
 * @brief Does THIS image carry a maker's profile for this axis?
 *
 * @details Asked per axis rather than per file: the makers do not all write the same set.
 * Sony and Fujifilm publish distortion, lateral CA and vignetting; Olympus publishes the
 * first two and no falloff; a DNG carries whichever opcodes its writer chose to emit.
 *
 * Distortion and chromatic aberration answer from the same table -- one radial curve per
 * channel IS the two of them together, which is why the vendor formats do not separate them
 * either.
 */
static gboolean _lens_image_embeds(dt_iop_module_t *self, const dt_lens_axis_t axis)
{
  if(IS_NULL_PTR(self->dev)) return FALSE;
  const int axes = ls_vendor_axes(&self->dev->image_storage.exif_correction);

  switch(axis)
  {
    case DT_LENS_AXIS_DISTORTION:  return (axes & LS_ENABLE_DISTORTION) != 0;
    case DT_LENS_AXIS_TCA:         return (axes & LS_ENABLE_TCA) != 0;
    case DT_LENS_AXIS_VIGNETTING:  return (axes & LS_ENABLE_VIGNETTING) != 0;
    default:                       return FALSE;
  }
}

typedef struct dt_iop_lensfun_gui_data_t
{
  /** The camera shown in the picker, as a database id; -1 for none. */
  long long camera_id;
  GtkWidget *lens_param_box;
  GtkWidget *cbe[3];
  GtkWidget *camera_model;
  GtkMenu *camera_menu;
  GtkWidget *lens_model;
  GtkMenu *lens_menu;
  /** One source combobox per axis, indexed by dt_lens_axis_t. */
  GtkWidget *axis_source[DT_LENS_AXIS_LAST];
  /** What each combobox currently OFFERS, rebuilt per image: row -> source, and how many.
   *  A source with nothing behind it for this image is not in here, because it is not in
   *  the widget either -- displayed means available. */
  dt_lens_source_t axis_row[DT_LENS_AXIS_LAST][DT_LENS_SOURCE_LAST];
  int axis_rows[DT_LENS_AXIS_LAST];
  GtkWidget *target_geom, *reverse, *tca_r, *tca_b, *scale;
} dt_iop_lensfun_gui_data_t;

/* Defined with the widgets they act on, at the bottom of the GUI section. Declared here
 * because every setter that changes what the database can match -- the camera, the lens --
 * has to say so, and those sit above them. */
static void _lens_rebuild_axis_rows(dt_iop_module_t *self);
static void _lens_gui_update_sensitivity(dt_iop_module_t *self);

typedef struct dt_iop_lensfun_global_data_t
{
  gboolean db_tried;
  /** Pre-warm thread, see _lensfun_db_warm(). Joined by cleanup_global(). */
  GThread *db_warm;
  int kernel_lens_distort_bilinear;
  int kernel_lens_distort_bicubic;
  int kernel_lens_distort_mitchell;
  int kernel_lens_vignette;
} dt_iop_lensfun_global_data_t;

/* ---------------------------------------------------------------------------------------
 * The LensSerious calibration database, latched BESIDE liblensfun's rather than replacing
 * it.
 *
 * Every lookup below is answered twice -- once by liblensfun, once by LensSerious -- and
 * both answers are printed. lensfun keeps authoring the pixels; nothing here changes what
 * is rendered. The point is to run the real GUI on real images and read the log, because
 * the disagreements that matter are not the ones a synthetic harness reaches: the harness
 * walks the database, and this walks whatever a user's EXIF actually says, spelled however
 * the camera spelled it.
 *
 * Reading is lock-free by construction -- `mode=ro&immutable=1` with SQLITE_OPEN_NOMUTEX,
 * so SQLite takes no file lock, no shared-memory segment and no mutex. The price is ONE
 * HANDLE PER THREAD, so the handle is thread-local and closed by its destructor when the
 * thread ends. Threads that never touch a lens never open it.
 *
 * The one-entry caches beside it stand in for the two process-wide memo hash tables:
 * commit_params() resolves the camera and the lens on every pipe resync, for every pipe,
 * and asks the SAME question every time -- an image's camera and lens do not change while
 * it is open. A per-thread cache of the last answer serves that exactly, with no lock and
 * no unbounded growth, where a shared table would need a mutex back.
 * ------------------------------------------------------------------------------------ */
typedef struct _ls_tls_t
{
  ls_db_t *db;
  gboolean tried;

  char cam_key[512];
  ls_camera_t cam;
  gboolean cam_found;
  gboolean cam_cached;

  char lens_key[512];
  long long lens_id;
  gboolean lens_cached;
} _ls_tls_t;

/* Closed when the thread that opened it exits, which is the whole reason this is a GPrivate
 * and not a plain __thread pointer: the handle has to be RELEASED, and nothing else in C
 * runs code at thread exit. iop/drawlayer.c holds its per-thread scratch buffers the same
 * way, for the same reason. */
static void _ls_tls_free(gpointer data)
{
  _ls_tls_t *tls = (_ls_tls_t *)data;
  if(IS_NULL_PTR(tls)) return;
  if(!IS_NULL_PTR(tls->db)) ls_db_close(tls->db);
  dt_free(tls);
}

static GPrivate _ls_tls_key = G_PRIVATE_INIT(_ls_tls_free);

/** @brief This thread's cache block, allocated on first use. NULL only if that allocation
 *  failed, in which case every caller below degrades to "no database" rather than crashing. */
static _ls_tls_t *_ls_tls_get(void)
{
  _ls_tls_t *tls = (_ls_tls_t *)g_private_get(&_ls_tls_key);
  if(!IS_NULL_PTR(tls)) return tls;

  tls = (_ls_tls_t *)g_malloc0(sizeof(_ls_tls_t));
  if(IS_NULL_PTR(tls)) return NULL;
  /* The only field whose zeroed value is not the right one: -1 is "no lens", 0 is a
   * perfectly good row id. */
  tls->lens_id = -1;
  g_private_set(&_ls_tls_key, tls);
  return tls;
}

/**
 * @brief This thread's database handle, opened on first use.
 *
 * @details The user's configuration directory is searched before the installed data, so a
 * database regenerated against newer upstream calibrations can be dropped in without
 * rebuilding. A failed open is final for the thread: retrying per lookup would turn a
 * missing file into a slow one.
 */
/**
 * @brief Report one failed database path, in terms of what the reader should do about it.
 *
 * @details Silent for "there is nothing there", which is the ordinary case for the
 * configuration directory: only a user who ran ansel-lens-db-update has a file there, and
 * saying so on every start would be noise about a non-event.
 */
static void _lens_report_db_failure(const char *path, const ls_db_open_status_t status,
                                    const int found_version, const char *detail)
{
  const char *reason = (!IS_NULL_PTR(detail) && detail[0]) ? detail : "no further detail";

  switch(status)
  {
    case LS_DB_OPEN_NO_FILE:
      dt_print(DT_DEBUG_PIPE, "[lens] no `%s'\n", path);
      break;

    case LS_DB_OPEN_UNREADABLE:
      /* Printed with whatever refused it, because it is not always the file: the path goes
       * through a URI conversion before SQLite sees it, and on Windows a readable file got
       * lost in there while "check its permissions" sent its owner looking at permissions
       * that were fine. The detail says which of the two happened. */
      dt_print(DT_DEBUG_ALWAYS,
               "[lens] `%s' exists but could not be read: %s. Check its permissions, and "
               "that it is not truncated\n", path, reason);
      break;

    case LS_DB_OPEN_SCHEMA:
      /* The one that actually happens, and the one the old message hid: a lenses.db left
       * behind in a prefix by an earlier install. The build only installs the file when it
       * produced one, so a rebuild that could not run the importer leaves the old file in
       * place and the new binary refuses it. */
      dt_print(DT_DEBUG_ALWAYS,
               "[lens] `%s' is a lens database of schema v%d, and this Ansel reads v%d. It "
               "is almost certainly left over from an older install: rebuild and reinstall "
               "so the file is replaced, or delete it if it is a stale copy in your "
               "configuration directory.\n",
               path, found_version, ls_db_schema_required());
      break;

    case LS_DB_OPEN_OK:
    default:
      break;
  }
}

static ls_db_t *_ls_db(void)
{
  _ls_tls_t *tls = _ls_tls_get();
  if(IS_NULL_PTR(tls)) return NULL;

  if(tls->tried) return tls->db;
  tls->tried = TRUE;

  char dir[DT_PATH_MAX] = { 0 };
  char path[DT_PATH_MAX] = { 0 };
  char config_path[DT_PATH_MAX] = { 0 };
  ls_db_open_status_t config_status = LS_DB_OPEN_NO_FILE;
  int config_version = -1;
  char config_detail[256] = { 0 };

  dt_loc_get_user_config_dir(dir, sizeof(dir));
  dt_concat_path_file(config_path, dir, "lenses.db");
  tls->db = ls_db_open_diagnostic(config_path, &config_status, &config_version, config_detail,
                                  sizeof(config_detail));

  ls_db_open_status_t data_status = LS_DB_OPEN_NO_FILE;
  int data_version = -1;
  char data_detail[256] = { 0 };
  if(IS_NULL_PTR(tls->db))
  {
    dt_loc_get_datadir(dir, sizeof(dir));
    dt_concat_path_file(path, dir, "lenses.db");
    tls->db = ls_db_open_diagnostic(path, &data_status, &data_version, data_detail,
                                    sizeof(data_detail));
  }

  /* Say WHICH failure it was, per path. All three used to print "no calibration database",
   * so a user whose file sat exactly where the message said it had looked was told to go
   * find it again -- when what they actually had was a database from an older install,
   * perfectly readable and built for a schema this Ansel does not read. */
  if(IS_NULL_PTR(tls->db))
  {
    _lens_report_db_failure(config_path, config_status, config_version, config_detail);
    _lens_report_db_failure(path, data_status, data_version, data_detail);
    dt_print(DT_DEBUG_ALWAYS,
             "[lens] no calibration database, so no lens correction is available.\n");
  }
  else
  {
    /* A config-directory database that exists and was refused is worth one line even when
     * the shipped one saved the day: it is tried FIRST, so the user believes it is the one
     * in use, and ansel-lens-db-update is what put it there. */
    if(config_status != LS_DB_OPEN_OK && config_status != LS_DB_OPEN_NO_FILE)
      _lens_report_db_failure(config_path, config_status, config_version, config_detail);

    dt_print(DT_DEBUG_PIPE, "[lens] opened `%s' (schema v%d)\n",
             (config_status == LS_DB_OPEN_OK) ? config_path : path,
             ls_db_schema_version(tls->db));
  }

  return tls->db;
}

/** @brief The camera an EXIF maker/model names. @return TRUE when found. */
static gboolean _ls_find_camera(const char *maker, const char *model, ls_camera_t *out)
{
  if(IS_NULL_PTR(model) || !model[0]) return FALSE;
  /* _ls_db() has already established that this thread has a cache block; it cannot have
   * returned a database without one. */
  _ls_tls_t *tls = _ls_tls_get();
  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db) || IS_NULL_PTR(tls)) return FALSE;

  char key[512];
  snprintf(key, sizeof(key), "%s\x1f%s", maker ? maker : "", model);
  if(tls->cam_cached && !strcmp(key, tls->cam_key))
  {
    if(tls->cam_found) *out = tls->cam;
    return tls->cam_found ? TRUE : FALSE;
  }

  ls_camera_t cam;
  /* A miss is cached too: it costs a lookup to establish and it will not change. */
  const gboolean found = (ls_db_find_camera(db, maker, model, &cam) == 1) ? TRUE : FALSE;

  g_strlcpy(tls->cam_key, key, sizeof(tls->cam_key));
  tls->cam = cam;
  tls->cam_found = found;
  tls->cam_cached = TRUE;

  if(found) *out = cam;
  return found;
}

/**
 * @brief The lens a free-text name names, as an id.
 * @param mount_id the camera's mount, to prefer lenses that fit it; 0 for no preference.
 * @param crop the camera's crop factor, which decides between lenses that share a NAME and
 * differ only in the sensor they were calibrated on; 0 to ignore it.
 * @details The name comes from EXIF or from what the user typed, so this is the fuzzy
 * matcher, not a lookup.
 */
/**
 * @brief The database lens best matching this name, at this focal length.
 *
 * @details @p focal is not a preference, it is a REFUSAL: a lens whose range cannot contain
 * the focal the picture was taken at did not take the picture, whatever its name scores.
 *
 * That matters because a name is often not an identifier. 197 entries in the catalogue are
 * called "fixed lens" and 195 more "festes objektiv" -- one per compact body, the model
 * being the only thing that tells them apart. When the camera resolves, the mount does that
 * work. When it does not, the matcher is handed a name shared by hundreds of different
 * optics and every one of them scores identically: on a Ricoh GR II (18.3 mm, APS-C) the
 * five tied candidates included the GR Digital's 5.9 mm lens on a 4.8x crop sensor, and the
 * arbitrary pick among equals took it. Its distortion polynomial, applied to a frame it was
 * never measured on, visibly bent the image instead of straightening it.
 *
 * The library's own tie-breaks cannot help here: it weighs crop factor, but the crop passed
 * is the CAMERA's and is 0 when no camera resolved, so the tie-break is inert in exactly
 * the case that needs it. Focal range it never sees -- ls_db_match_lens() takes no focal.
 * Filtering here keeps that API unchanged; pushing the test into the matcher, where it
 * could also inform the score, is the better long-term home for it.
 *
 * Every one of the 1562 catalogue entries carries a usable range, so nothing is filtered
 * out for want of data.
 */
static long long _ls_find_lens(long long mount_id, float crop, float focal,
                               const char *lens_name)
{
  if(IS_NULL_PTR(lens_name) || !lens_name[0]) return -1;
  _ls_tls_t *tls = _ls_tls_get();
  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db) || IS_NULL_PTR(tls)) return -1;

  char key[512];
  snprintf(key, sizeof(key), "%lld\x1f%.4f\x1f%.4f\x1f%s", mount_id, (double)crop,
           (double)focal, lens_name);
  if(tls->lens_cached && !strcmp(key, tls->lens_key)) return tls->lens_id;

  /* Several candidates rather than one, because the best-scoring name may be a lens this
   * picture cannot have come through, and the next one down may be exactly right. */
  ls_db_match_t m[8];
  const int n = ls_db_match_lens(db, NULL, lens_name, mount_id, crop, m,
                                 (int)(sizeof(m) / sizeof(*m)));
  long long id = -1;
  for(int i = 0; i < n; i++)
  {
    ls_lens_t cand;
    if(ls_db_lens_by_id(db, m[i].lens_id, &cand) != 1) continue;

    /* A hair of tolerance: EXIF focals are rounded, and a prime's range is a single value
     * it must still match. Ranges are ordered by the importer. */
    if(focal > 0.f
       && (focal < cand.min_focal - 0.05f || focal > cand.max_focal + 0.05f))
      continue;

    id = m[i].lens_id;
    break;
  }

  g_strlcpy(tls->lens_key, key, sizeof(tls->lens_key));
  tls->lens_id = id;
  tls->lens_cached = TRUE;
  return id;
}

typedef struct dt_iop_lensfun_data_t
{
  int modify_flags;
  int inverse;
  float scale;
  float crop;
  float focal;
  float aperture;
  float distance;
  dt_lens_type_t target_geom;
  gboolean do_nan_checks;
  gboolean tca_override;

  /** The lens as DATA: a value owned by this struct, read out of the database at commit
   *  and valid for as long as the struct is -- there is nothing here to free. */
  ls_lens_t ls_lens;
  gboolean ls_have;

  /** The maker's own profile, read out of the raw file and turned into the same shape the
   *  correction model takes. Resolved at commit like the database lens beside it, for the
   *  same reason: the pixel path must not go looking things up. */
  ls_knots_t ls_knots;
  gboolean knots_have;
  /** What the maker's own autoscale says removes the borders this profile leaves. */
  float knots_scale;
} dt_iop_lensfun_data_t;

/**
 * @brief Does this pipe data hold ANY resolvable correction source?
 *
 * @details One predicate for every entry point, because the duplicated inline test already
 * drifted once inside a single diff: get_modifier() learned that a maker's embedded profile
 * needs no database lens and no crop factor, while process(), process_cl(), the three
 * distort_*() callbacks, modify_roi_in() and the geometry record kept the old
 * database-only bail-out -- so a body absent from the database but carrying its own profile
 * (the exact case the embedded path exists for) copied its input and returned before
 * get_modifier() was ever consulted, and would have desynced masks from pixels the day only
 * some of the seven were patched.
 */
static inline gboolean _lens_data_available(const dt_iop_lensfun_data_t *d)
{
  return d->knots_have || (d->ls_have && d->crop > 0.f);
}


const char *name()
{
  return _("_lens correction");
}

const char *aliases()
{
  return _("vignette|chromatic aberrations|distortion");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("correct lenses optical flaws"),
                                      _("corrective"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric and reconstruction, RGB"),
                                      _("linear, RGB, scene-referred"));
}


int default_group()
{
  return IOP_GROUP_REPAIR;
}

int operation_tags()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_UNSAFE_COPY;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 2 && new_version == 5)
  {
    // legacy params of version 2; version 1 comes from ancient times and seems to be forgotten by now
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      dt_lens_type_t target_geom;
      char camera[52];
      char lens[52];
      int tca_override;
      float tca_r, tca_b;
    } dt_iop_lensfun_params_v2_t;

    const dt_iop_lensfun_params_v2_t *o = (dt_iop_lensfun_params_v2_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->modify_flags = o->modify_flags;
    n->inverse = o->inverse;
    n->scale = o->scale;
    n->crop = o->crop;
    n->focal = o->focal;
    n->aperture = o->aperture;
    n->distance = o->distance;
    n->target_geom = o->target_geom;
    n->tca_override = o->tca_override;
    g_strlcpy(n->camera, o->camera, sizeof(n->camera));
    g_strlcpy(n->lens, o->lens, sizeof(n->lens));
    n->modified = 1;

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }
  if(old_version == 3 && new_version == 5)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      dt_lens_type_t target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r, tca_b;
    } dt_iop_lensfun_params_v3_t;

    const dt_iop_lensfun_params_v3_t *o = (dt_iop_lensfun_params_v3_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    /* The whole OLD struct, sized from the old struct. It used to be sized as the new one
     * minus an int, which is the same number only for as long as v5 has exactly one field
     * more than v3 -- add a second and this reads off the end of the caller's buffer. */
    memcpy(n, o, sizeof(*o));

    // one more parameter and changed parameters in case we autodetect
    n->modified = 1;

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }

  if(old_version == 4 && new_version == 5)
  {
    typedef struct
    {
      int modify_flags;
      int inverse;
      float scale;
      float crop;
      float focal;
      float aperture;
      float distance;
      dt_lens_type_t target_geom;
      char camera[128];
      char lens[128];
      int tca_override;
      float tca_r, tca_b;
      int modified;
    } dt_iop_lensfun_params_v4_t;

    const dt_iop_lensfun_params_v4_t *o = (dt_iop_lensfun_params_v4_t *)old_params;
    dt_iop_lensfun_params_t *n = (dt_iop_lensfun_params_t *)new_params;
    dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    memcpy(n, o, sizeof(dt_iop_lensfun_params_t));

    // old versions had R and B swapped
    n->tca_r = o->tca_b;
    n->tca_b = o->tca_r;

    return 0;
  }

  return 1;
}

static char *_lens_sanitize(const char *orig_lens)
{
  const char *found_or = strstr(orig_lens, " or ");
  const char *found_parenthesis = strstr(orig_lens, " (");

  if(found_or || found_parenthesis)
  {
    size_t pos_or = (size_t)(found_or - orig_lens);
    size_t pos_parenthesis = (size_t)(found_parenthesis - orig_lens);
    size_t pos = pos_or < pos_parenthesis ? pos_or : pos_parenthesis;

    if(pos > 0)
    {
      char *new_lens = (char *)malloc(pos + 1);

      strncpy(new_lens, orig_lens, pos);
      new_lens[pos] = '\0';

      return new_lens;
    }
    else
    {
      char *new_lens = strdup(orig_lens);
      return new_lens;
    }
  }
  else
  {
    char *new_lens = strdup(orig_lens);
    return new_lens;
  }
}

__DT_CLONE_TARGETS__
/**
 * @brief Resolve the lens at one shooting configuration. THE modifier factory.
 *
 * @param mods_done receives the axes actually resolved, as DT_LENS_MODIFY_* -- an axis with
 * no calibration for this focal is absent, which is how every caller decides whether there
 * is anything to do.
 * @param w, h the frame the correction is expressed over, in pixels.
 * @param d the committed correction state.
 * @param mods_filter the axes the caller will accept, intersected with the user's own.
 * @param force_inverse flip the direction, for the callers that undo a correction.
 * @param mod filled in. It owns nothing: an ls_modifier_t is a value, so there is no
 * counterpart to the `delete modifier` this replaces and no way to leak one.
 * @return non-zero when at least one axis resolved.
 */
static int get_modifier(int *mods_done, int w, int h, const dt_iop_lensfun_data_t *d,
                        int mods_filter, gboolean force_inverse, ls_modifier_t *mod,
                        ls_modifier_t *vig_mod)
{
  memset(mod, 0, sizeof(*mod));
  if(mods_done) *mods_done = 0;
  /* A maker's own profile needs no database entry and no crop factor -- it describes THIS
   * body and lens, and it arrived in the file. Refusing it for want of a database lens is
   * how the embedded path would have been unreachable on exactly the bodies that carry
   * one and are not calibrated upstream. */
  if(!_lens_data_available(d)) return 0;

  int mods_todo = d->modify_flags & mods_filter;

  /* Drop any axis whose source this build cannot serve, so an unknown source from some
   * future Ansel visibly does nothing rather than silently correcting from the database.
   * Filtering here, at the one place that decides what to ask a resolver for, means the
   * rest of the module never re-derives it. */
  for(dt_lens_axis_t axis = 0; axis < DT_LENS_AXIS_LAST; axis++)
  {
    const dt_lens_source_t source = _lens_source_decode(d->modify_flags, d->tca_override, axis);
    /* The whole pack, so switching distortion to another source stops the projection change
     * and the scaling with it -- they are one correction, and leaving them running was what
     * kept geometry applying after its control had been hidden. */
    if(!_lens_source_is_library_served(source)) mods_todo &= ~_lens_axis_flags(axis);
  }

  int want = 0;
  if(mods_todo & DT_LENS_MODIFY_DISTORTION) want |= LS_ENABLE_DISTORTION;
  if(mods_todo & DT_LENS_MODIFY_TCA) want |= LS_ENABLE_TCA;
  if(mods_todo & DT_LENS_MODIFY_VIGNETTING) want |= LS_ENABLE_VIGNETTING;
  if(mods_todo & DT_LENS_MODIFY_GEOMETRY) want |= LS_ENABLE_GEOMETRY;
  if(mods_todo & DT_LENS_MODIFY_SCALE) want |= LS_ENABLE_SCALE;

  const int reverse = force_inverse ? !d->inverse : d->inverse;

  /* Which resolver, decided by the distortion axis because that is the one that owns the
   * coordinate system: a maker's table indexes radius against the half diagonal, the
   * database against lensfun's short side, and a single modifier can only be in one of
   * them. Vignetting is not bound by that -- it is a gain, resolved separately below. */
  const dt_lens_source_t geom_source
      = _lens_source_decode(d->modify_flags, d->tca_override, DT_LENS_AXIS_DISTORTION);
  const gboolean geom_embedded = (geom_source == DT_LENS_SOURCE_EMBEDDED) && d->knots_have;

  /* The one remaining way to end up on the database while the user asked for the file: the
   * table did not resolve. commit_params has already said why, once, so this stays quiet --
   * it runs per pipe per frame and would drown the reason it is reporting. */

  int got;
  if(geom_embedded)
  {
    /* The maker measured the lens as it shipped, so there is no projection change to make
     * and the scale to use is the one their own profile asks for. */
    /* The maker's own autoscale AND the user's slider, composed. The profile ships the
     * factor that just clears the borders it leaves; the slider is what the user wants on
     * top of that, and passing only the first left the slider inert in this mode -- a
     * control that moves and changes nothing. */
    const float knots_scale = d->knots_scale * ((d->scale > 0.f) ? d->scale : 1.f);

    /* Ask the table for chromatic aberration only if that axis actually chose it. The
     * maker's curve carries distortion and CA together, so asking for both when CA was set
     * to the database or to typed values would apply the maker's aberration on top of the
     * other one -- over-correcting the fringing by exactly the maker's amount, with the
     * panel showing a source the pixels never used. Asked for distortion alone, the table
     * hands back pure geometry and the chosen TCA model runs after it. */
    int knot_want = want & ~LS_ENABLE_GEOMETRY;
    if(_lens_source_decode(d->modify_flags, d->tca_override, DT_LENS_AXIS_TCA)
       != DT_LENS_SOURCE_EMBEDDED)
      knot_want &= ~LS_ENABLE_TCA;

    got = ls_modifier_init_knots(mod, &d->ls_knots, w, h, knots_scale, knot_want, reverse);

    /* TCA from somewhere else rides on the table's geometry. ls_modifier_init_knots() only
     * fills the knot tables, so the polynomial the database resolved -- or the linear one
     * _lens_build_data() synthesised from the user's two numbers -- has to be put in.
     *
     * ONLY THIS DIRECTION. The reverse -- a database distortion wearing the maker's
     * aberration -- is not implemented and is not an oversight:
     *
     *   - physically it is two calibrations of the same lens applied to each other. The
     *     maker measured their aberration as a departure from THEIR geometry, so it is only
     *     the right departure when that geometry is the one in force;
     *   - mechanically the table has no separate aberration to lift out. Distortion and CA
     *     are one per-channel curve; green is the geometry and the other two are defined
     *     relative to it, so "the CA alone" would be the ratio knot_c[c]/knot_c[1] resampled
     *     onto the database's radius axis -- which is a different normalization again (half
     *     short side against half diagonal), needing the radius conversion factor that
     *     ls_modifier_set_projection() documents.
     *
     * If it is ever wanted: add LS_EVAL_TCA_KNOTS carrying those ratios plus that factor,
     * and expect to answer the physical objection first. _lens_rebuild_axis_row() withdraws
     * the row meanwhile, so the combination cannot be selected and then quietly ignored. */
    if((want & LS_ENABLE_TCA) && !(knot_want & LS_ENABLE_TCA) && d->ls_have)
    {
      ls_modifier_t tca_mod;
      if(ls_modifier_init(&tca_mod, &d->ls_lens, d->crop, w, h, d->focal, d->aperture,
                          d->distance, 1.f, (int)d->target_geom, LS_ENABLE_TCA, reverse)
         & LS_ENABLE_TCA)
      {
        mod->tca = tca_mod.tca;
        mod->enabled |= LS_ENABLE_TCA;
        got |= LS_ENABLE_TCA;
      }
    }

    /* The projection, put back on top of the maker's table.
     *
     * The table cannot supply it -- it describes the lens in the projection it shipped
     * with -- but the database entry matched alongside knows the lens's type, and that is
     * all the stage needs besides a focal. Without this, asking for a fisheye to be
     * rectilinear did nothing whenever distortion came from the file, which is the one
     * case where the two sources visibly disagreed about what the module could do.
     *
     * The crop here is the SHOOTING camera's, unlike the database path's: this modifier
     * normalizes radius against the half diagonal of the frame in hand, so that is the
     * sensor its projection focal has to be expressed against. */
    if((want & LS_ENABLE_GEOMETRY) && d->ls_have && d->crop > 0.f && d->focal > 0.f)
    {
      if(ls_modifier_set_projection(mod, (int)d->ls_lens.type, (int)d->target_geom,
                                    d->focal, d->crop))
        got |= LS_ENABLE_GEOMETRY;
    }
  }
  else
    got = ls_modifier_init(mod, &d->ls_lens, d->crop, w, h, d->focal, d->aperture,
                           d->distance, d->scale, (int)d->target_geom, want, reverse);

  int done = 0;
  if(got & LS_ENABLE_DISTORTION) done |= DT_LENS_MODIFY_DISTORTION;
  if(got & LS_ENABLE_TCA) done |= DT_LENS_MODIFY_TCA;
  if(got & LS_ENABLE_VIGNETTING) done |= DT_LENS_MODIFY_VIGNETTING;
  if(got & LS_ENABLE_GEOMETRY) done |= DT_LENS_MODIFY_GEOMETRY;
  if(got & LS_ENABLE_SCALE) done |= DT_LENS_MODIFY_SCALE;

  /* A projection change LensSerious will not serve -- panoramic or equirectangular on
   * either side, which map x and y differently and are not radially expressible -- is
   * reported as not done rather than approximated. */
  if(mod->geometry_unsupported) done &= ~DT_LENS_MODIFY_GEOMETRY;

  /* Vignetting, resolved on its own when it does not come from the same place as the
   * geometry. That combination is not exotic: an Olympus body embeds distortion and lateral
   * CA and no falloff at all, so "geometry from the file, vignetting from the database" is
   * the ordinary case for that maker rather than a corner of one.
   *
   * It works because the two halves of the correction never share a number. Vignetting
   * reads its own scale, its own centre and its own model, so a resolver that normalises
   * radius differently cannot disturb it -- which is exactly what
   * ls_eval_adopt_vignetting() relies on when the two are put back into one block. */
  if(vig_mod)
  {
    const dt_lens_source_t vig_source
        = _lens_source_decode(d->modify_flags, d->tca_override, DT_LENS_AXIS_VIGNETTING);
    const gboolean vig_embedded = (vig_source == DT_LENS_SOURCE_EMBEDDED) && d->knots_have;
    const gboolean vig_wanted = (mods_todo & DT_LENS_MODIFY_VIGNETTING) != 0;

    if(!vig_wanted || vig_embedded == geom_embedded)
    {
      /* Same resolver, or nothing asked for: the main modifier's own state answers. When
       * the axis is off, `want` never carried vignetting, so this copy carries none either
       * -- resolving one here anyway was the bug: the GPU path grafts whatever this block
       * holds into its single eval block and the fused kernels apply what is enabled there,
       * so a falloff nobody asked for came back through the graft with the axis OFF. */
      *vig_mod = *mod;
    }
    else if(vig_embedded)
      ls_modifier_init_knots(vig_mod, &d->ls_knots, w, h, 1.f, LS_ENABLE_VIGNETTING, reverse);
    else
      ls_modifier_init(vig_mod, &d->ls_lens, d->crop, w, h, d->focal, d->aperture,
                       d->distance, 1.f, (int)d->target_geom, LS_ENABLE_VIGNETTING, reverse);

    /* The truth about vignetting now lives in vig_mod, whichever resolver produced it, so
     * the reported axis must be read from THERE. Reading it from the main modifier is how
     * the flagship hybrid silently lost its falloff: an Olympus table has no vignetting
     * knots, so the main (knots) resolver reported the axis not-done, the database
     * vignetting sat correctly resolved in vig_mod -- and every caller gates the falloff
     * on the done bit, so it was never applied. */
    if(vig_wanted && (vig_mod->enabled & LS_ENABLE_VIGNETTING))
      done |= DT_LENS_MODIFY_VIGNETTING;
    else
      done &= ~DT_LENS_MODIFY_VIGNETTING;
  }

  if(mods_done) *mods_done = done;
  return done != 0;
}

static inline void _lens_fill_vignette_row(float *const buf, const int width, const int ch)
{
  if(ch == DT_PIXEL_SIMD_CHANNELS)
  {
    const dt_aligned_pixel_simd_t half = dt_simd_set1(0.5f);
    for(int x = 0; x < width; x++) dt_store_simd_aligned(buf + (size_t)x * ch, half);
  }
  else
  {
    for(int k = 0; k < ch * width; k++) buf[k] = 0.5f;
  }
}

/* Why do we care about being a monochrome image or not?
 The lensfun library does not have an algorithm for distortion or tca correction specialized for monochrome images,
   the builtin correction works with subtle differences for the color channels leading to some colorizing of the images.
 How is this fixed here:
   Monochrome images (from pure monochrome cameras or cameras with the color filter removed from the sensor) have
   all three rgb colors set to the same value by the demosaicer.
   Looking through lensfun code & docs the ApplySubpixelGeometryDistortion algorithm makes assumptions from given
   coeffs how far data are displaced for the different wavelengths of light.
   As green / Y channel is the most centric i took that as the canonical value instead of taking the mean.
*/

__DT_CLONE_TARGETS__
int process(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
            const void *const ivoid, void *const ovoid)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  const int ch = piece->dsc_in.channels;
  const int ch_width = ch * roi_in->width;
  const int mask_display = pipe->mask_display;


  if(!_lens_data_available(d))
  {
    dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, ch);
    return 0;
  }

  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int used_lf_mask = _lens_mask_for_mono(raw_monochrome);

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;

  int modflags;
  ls_modifier_t modifier;
  ls_modifier_t vig_modifier;
  get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE, &modifier, &vig_modifier);
  dt_print(DT_DEBUG_PIPE, "[lens] resolved 0x%x of 0x%x requested (%d dist, %d tca, %d vig"
           " calibrations, crop %.4f, focal %.1f)\n", modflags, d->modify_flags,
           d->ls_lens.n_dist, d->ls_lens.n_tca, d->ls_lens.n_vig, (double)d->crop,
           (double)d->focal);


  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  /* Vignetting is folded into the resampling loops below rather than run as a pass of its
   * own over a whole copy of the frame. ls_eval_vignette_factor() answers 1 when vignetting
   * is not enabled, so the loops need no second branch for it.
   *
   * Which FRAME the falloff lives in depends on the direction. Correcting, it belongs to
   * the source, so each channel takes the factor at ITS OWN source coordinate -- exactly
   * what the two-pass did, which darkened the input and then let each channel sample its
   * own position in it. Reversing, it is being put back onto the frame being produced, so
   * it is evaluated at the destination. */
  /* Flattened from the VIGNETTING modifier, not the geometry one. They are usually the same
   * object; they differ when the two axes take different sources, and this is where that
   * costs nothing -- the falloff already had a block of its own. */
  ls_eval_t vp;
  const gboolean have_vig = _lens_flags_have_axis(modflags, DT_LENS_AXIS_VIGNETTING)
                            && ls_eval_from_modifier(&vig_modifier, &vp);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if(_lens_flags_move_pixels(modflags))
    {
      // acquire temp memory for distorted pixel coords
      const size_t bufsize = (size_t)roi_out->width * 2 * 3;

      size_t padded_bufsize;
      float *const buf = dt_pixelpipe_cache_alloc_perthread_float(bufsize, &padded_bufsize);
      if(IS_NULL_PTR(buf)) return 1;

#ifdef _OPENMP
#pragma omp parallel for default(none)  \
  firstprivate(roi_out, roi_in, padded_bufsize, modifier, ch, d, buf, ovoid, ivoid, ch_width, interpolation, raw_monochrome, mask_display, have_vig, vp)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *bufptr = (float*)dt_get_perthread(buf, padded_bufsize);
        ls_modifier_apply_subpixel_geometry(&modifier, roi_out->x, roi_out->y + y, roi_out->width, 1, bufptr);

        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, bufptr += 6, out += ch)
        {
          dt_aligned_pixel_simd_t pixel = { 0.f };
          for(int c = 0; c < 3; c++)
          {
            if(d->do_nan_checks && (!isfinite(bufptr[c * 2]) || !isfinite(bufptr[c * 2 + 1])))
            {
              pixel[c] = 0.0f;
              continue;
            }

            const float *const inptr = (const float *const)ivoid + (size_t)c;
            const float pi0 = fmaxf(fminf(bufptr[c * 2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
            const float pi1 = fmaxf(fminf(bufptr[c * 2 + 1] - roi_in->y, roi_in->height - 1.0f), 0.0f);
            pixel[c] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                                       roi_in->height, ch, ch_width);
          }

          if(have_vig)
          {
            /* Reversing: the falloff belongs to the frame being produced. */
            const float v = ls_eval_vignette_factor(&vp, (float)(roi_out->x + x),
                                                    (float)(roi_out->y + y));
            for(int c = 0; c < 3; c++) pixel[c] *= v;
          }
          if(raw_monochrome) pixel[0] = pixel[2] = pixel[1];

          if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
          {
            if(d->do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
            {
              pixel[3] = 0.0f;
            }
            else
            {
              // take green channel distortion also for alpha channel
              const float *const inptr = (const float *const)ivoid + (size_t)3;
              const float pi0 = fmaxf(fminf(bufptr[2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
              const float pi1 = fmaxf(fminf(bufptr[3] - roi_in->y, roi_in->height - 1.0f), 0.0f);
              pixel[3] = dt_interpolation_compute_sample(interpolation, inptr, pi0, pi1, roi_in->width,
                                                         roi_in->height, ch, ch_width);
            }

            if(ch == DT_PIXEL_SIMD_CHANNELS) dt_store_simd_aligned(out, pixel);
            else for(int c = 0; c < ch; c++) out[c] = pixel[c];
          }
          else
          {
            for(int c = 0; c < 3; c++) out[c] = pixel[c];
          }
        }
      }
      dt_pixelpipe_cache_free_align(buf);
    }
    else
    {
      dt_iop_image_copy_by_size((float*)ovoid, (float*)ivoid, roi_out->width, roi_out->height, ch);

      /* Nothing moved, so there was no resampling loop to fold the falloff into. */
      if(have_vig)
      {
        __OMP_PARALLEL_FOR__(firstprivate(modifier, ovoid, roi_out, ch))
        for(int y = 0; y < roi_out->height; y++)
        {
          float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
          ls_modifier_apply_vignetting(&modifier, roi_out->x, roi_out->y + y, roi_out->width, 1,
                                       out, (int)((ch * roi_out->width) * sizeof(float)));
        }
      }
    }
  }
  else // correct distortions:
  {
    /* No copy of the input, and no separate vignetting pass over it. This used to
     * duplicate the whole frame -- 387 MB for a 24 Mpx RGBA buffer -- darken the copy, and
     * resample from it. The falloff is a per-source-pixel gain, so folding it into the
     * resampling loop below gives the same answer while reading the caller's own buffer. */

    if(_lens_flags_move_pixels(modflags))
    {
      // acquire temp memory for distorted pixel coords
      const size_t buf2size = (size_t)roi_out->width * 2 * 3;
      size_t padded_buf2size;
      float *const buf2 = dt_pixelpipe_cache_alloc_perthread_float(buf2size, &padded_buf2size);
      if(IS_NULL_PTR(buf2)) return 1;


#ifdef _OPENMP
#pragma omp parallel for default(none)  \
  firstprivate(roi_out, roi_in, ovoid, ivoid, ch, padded_buf2size, modifier, mask_display, raw_monochrome, interpolation, ch_width, d, buf2, have_vig, vp)
#endif
      for(int y = 0; y < roi_out->height; y++)
      {
        float *buf2ptr = (float*)dt_get_perthread(buf2, padded_buf2size);
        ls_modifier_apply_subpixel_geometry(&modifier, roi_out->x, roi_out->y + y, roi_out->width, 1, buf2ptr);
        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + (size_t)y * roi_out->width * ch;
        for(int x = 0; x < roi_out->width; x++, buf2ptr += 6, out += ch)
        {
          dt_aligned_pixel_simd_t pixel = { 0.f };
          for(int c = 0; c < 3; c++)
          {
            if(d->do_nan_checks && (!isfinite(buf2ptr[c * 2]) || !isfinite(buf2ptr[c * 2 + 1])))
            {
              pixel[c] = 0.0f;
              continue;
            }

            const float *bufptr = ((const float *)ivoid) + c;
            const float pi0 = fmaxf(fminf(buf2ptr[c * 2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
            const float pi1 = fmaxf(fminf(buf2ptr[c * 2 + 1] - roi_in->y, roi_in->height - 1.0f), 0.0f);
            pixel[c] = dt_interpolation_compute_sample(interpolation, bufptr, pi0, pi1, roi_in->width,
                                                       roi_in->height, ch, ch_width);
            /* Correcting: the falloff belongs to the source, so each channel takes it at
             * its own source coordinate -- which is what sampling an already-darkened input
             * amounted to. */
            if(have_vig)
              pixel[c] *= ls_eval_vignette_factor(&vp, buf2ptr[c * 2], buf2ptr[c * 2 + 1]);
          }
          if(raw_monochrome) pixel[0] = pixel[2] = pixel[1];
          if(mask_display & DT_DEV_PIXELPIPE_DISPLAY_MASK)
          {
            if(d->do_nan_checks && (!isfinite(buf2ptr[2]) || !isfinite(buf2ptr[3])))
            {
              pixel[3] = 0.0f;
            }
            else
            {
              // take green channel distortion also for alpha channel
              const float *bufptr = ((const float *)ivoid) + 3;
              const float pi0 = fmaxf(fminf(buf2ptr[2] - roi_in->x, roi_in->width - 1.0f), 0.0f);
              const float pi1 = fmaxf(fminf(buf2ptr[3] - roi_in->y, roi_in->height - 1.0f), 0.0f);
              pixel[3] = dt_interpolation_compute_sample(interpolation, bufptr, pi0, pi1, roi_in->width,
                                                         roi_in->height, ch, ch_width);
            }

            if(ch == DT_PIXEL_SIMD_CHANNELS) dt_store_simd_aligned(out, pixel);
            else for(int c = 0; c < ch; c++) out[c] = pixel[c];
          }
          else
          {
            for(int c = 0; c < 3; c++) out[c] = pixel[c];
          }
        }
      }
      dt_pixelpipe_cache_free_align(buf2);
    }
    else
    {
      dt_iop_image_copy_by_size((float *)ovoid, (float *)ivoid, roi_out->width, roi_out->height, ch);

      /* Nothing moved, so there was no resampling loop to fold the falloff into. */
      if(have_vig)
      {
        __OMP_PARALLEL_FOR__(firstprivate(modifier, ovoid, roi_in, ch))
        for(int y = 0; y < roi_in->height; y++)
        {
          float *out = ((float *)ovoid) + (size_t)ch * roi_in->width * y;
          ls_modifier_apply_vignetting(&modifier, roi_in->x, roi_in->y + y, roi_in->width, 1, out,
                                       (int)((ch * roi_in->width) * sizeof(float)));
        }
      }
    }
  }

  /* No GUI state is written here. Which corrections apply is a property of the
   * camera/lens/params combination, not of a rendered frame -- the label is computed on the
   * GUI thread by _lens_corrections_available(). */
  return 0;
}

#ifdef HAVE_OPENCL


int process_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out)
{
  const dt_iop_roi_t *const roi_in = &piece->roi_in;
  const dt_iop_roi_t *const roi_out = &piece->roi_out;
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  const gboolean raw_monochrome = dt_image_is_monochrome(&self->dev->image_storage);
  const int used_lf_mask = _lens_mask_for_mono(raw_monochrome);

  cl_int err = -999;

  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->global_data;
  ls_modifier_t modifier;
  /* Declared before the first `goto error`: C++ forbids jumping over an initialisation. */
  ls_eval_t p;
  gboolean have_eval = FALSE, do_geom = FALSE, do_vig = FALSE;

  const int devid = pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const int roi_in_x = roi_in->x;
  const int roi_in_y = roi_in->y;
  const int roi_out_x = roi_out->x;
  const int roi_out_y = roi_out->y;

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;

  size_t origin[] = { 0, 0, 0 };
  size_t oregion[] = { (size_t)owidth, (size_t)oheight, 1 };
  size_t isizes[] = { (size_t)ROUNDUPDWD(iwidth, devid), (size_t)ROUNDUPDHT(iheight, devid), 1 };
  size_t osizes[] = { (size_t)ROUNDUPDWD(owidth, devid), (size_t)ROUNDUPDHT(oheight, devid), 1 };

  int modflags;
  int ldkernel = -1;
  /* Declared here, ahead of every `goto error`: C++ will not let one jump over an
   * initialisation. Resolved once below, after get_modifier() has settled modflags. */
  const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  if(!_lens_data_available(d))
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  switch(interpolation->id)
  {
    case DT_INTERPOLATION_BILINEAR:
      ldkernel = gd->kernel_lens_distort_bilinear;
      break;
    case DT_INTERPOLATION_BICUBIC:
      ldkernel = gd->kernel_lens_distort_bicubic;
      break;
    case DT_INTERPOLATION_MITCHELL:
      ldkernel = gd->kernel_lens_distort_mitchell;
      break;
    default:
      return FALSE;
  }


  ls_modifier_t vig_modifier;
  get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE, &modifier, &vig_modifier);

  /* One kernel, in and out, in both directions.
   *
   * The correction crosses as an ls_eval_t -- 632 bytes of coefficients passed by value --
   * and each work-item evaluates its own source coordinates from it, so there is no
   * displacement map, no host buffer and no upload. Vignetting rides along inside the same
   * resampling pass rather than writing a whole intermediate image for the resampler to
   * read back.
   *
   * The direction lives in the block: ls_eval_map() reads p.reverse and composes the chain
   * accordingly, and _lens_devignette() places the falloff in the frame that direction puts
   * it in. So both directions are the same launch, which is why the branch that used to
   * distinguish them is gone. */
  have_eval = ls_eval_from_modifier(&modifier, &p) != 0;

  /* One block serves both halves here, unlike the CPU path, so where the two axes took
   * different sources the falloff has to be grafted in from its own modifier. The two
   * halves of ls_eval_t share no field -- vignetting carries its own scale, centre and
   * model -- which is what makes the graft safe across resolvers that normalise radius
   * differently. A second block would be the obvious alternative and does not fit: 632
   * bytes each against the 1024 OpenCL 1.2 guarantees for a kernel's whole argument list. */
  if(have_eval)
  {
    ls_eval_t vp;
    if(ls_eval_from_modifier(&vig_modifier, &vp)) ls_eval_adopt_vignetting(&p, &vp);
  }
  do_geom = have_eval
      && _lens_flags_move_pixels(modflags);
  do_vig = have_eval && _lens_flags_have_axis(modflags, DT_LENS_AXIS_VIGNETTING) != 0;

  if(do_geom)
  {
    /* Vignetting, if any, is applied inside this pass -- the kernel reads it out of p. */
    dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&owidth);
    dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&oheight);
    dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
    dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
    dt_opencl_set_kernel_arg(devid, ldkernel, 6, sizeof(int), (void *)&roi_in_x);
    dt_opencl_set_kernel_arg(devid, ldkernel, 7, sizeof(int), (void *)&roi_in_y);
    dt_opencl_set_kernel_arg(devid, ldkernel, 8, sizeof(int), (void *)&roi_out_x);
    dt_opencl_set_kernel_arg(devid, ldkernel, 9, sizeof(int), (void *)&roi_out_y);
    dt_opencl_set_kernel_arg(devid, ldkernel, 10, sizeof(ls_eval_t), (void *)&p);
    dt_opencl_set_kernel_arg(devid, ldkernel, 11, sizeof(int), (void *)&(d->do_nan_checks));
    dt_opencl_set_kernel_arg(devid, ldkernel, 12, sizeof(int), (void *)&(raw_monochrome));
    err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
    if(err != CL_SUCCESS) goto error;
  }
  else if(do_vig)
  {
    /* Nothing moves, so there is nothing to resample: a dedicated pass costs one fetch per
     * pixel where the fused one would cost the resampler's full tap count for an identity
     * map. Which frame the falloff belongs to is the same question as above, and with no
     * geometry in play the two coincide. */
    const int vx = d->inverse ? roi_out_x : roi_in_x;
    const int vy = d->inverse ? roi_out_y : roi_in_y;
    const int vw = d->inverse ? owidth : iwidth;
    const int vh = d->inverse ? oheight : iheight;
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 0, sizeof(cl_mem), (void *)&dev_in);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 1, sizeof(cl_mem), (void *)&dev_out);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 2, sizeof(int), (void *)&vw);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 3, sizeof(int), (void *)&vh);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 4, sizeof(int), (void *)&vx);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 5, sizeof(int), (void *)&vy);
    dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 6, sizeof(ls_eval_t), (void *)&p);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette,
                                      d->inverse ? osizes : isizes);
    if(err != CL_SUCCESS) goto error;
  }
  else
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
  }

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_lens] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, const struct dt_dev_pixelpipe_iop_t *piece, struct dt_develop_tiling_t *tiling)
{
  /* CPU: in + out, and nothing else of image size.
   *
   * The whole-frame copy process() used to make -- to darken before resampling from it --
   * is gone with the separate vignetting pass: the falloff is folded into the resampling
   * loop, which now reads the caller's own input buffer. The displacement map is not a
   * whole-image temporary either: it is built a row at a time into a per-thread buffer of
   * width*6 floats, so it grows with the frame's WIDTH and the thread count rather than
   * its area -- ~2 MB for a 6000 px frame on 16 threads, against ~384 MB for one 24 Mpx
   * RGBA buffer. Counting it here would reserve memory nothing allocates.
   *
   * GPU: in + out, and nothing else at all -- one kernel reads the input and writes the
   * output, with vignetting folded into the same pass.
   *
   * Both figures used to be 4.5, meaning in + out + tmp + a six-float-per-pixel map buffer
   * (1.5x an RGBA one) that had to be built on the host and uploaded. The GPU path no
   * longer has that buffer -- each work-item evaluates its own coordinates from ~80 bytes
   * of coefficients passed as a kernel argument -- so reserving 1.5 image buffers for it
   * made the tile solver split frames that would have fitted whole.
   *
   * factor_cl and maxbuf_cl have to be set explicitly: dt_develop_tiling_t defaults them to
   * the CPU figures (develop/tiling.c), so a module that sets only `factor` silently
   * describes its GPU path with its CPU path's appetite. */
  tiling->factor = 2.0f;    // in + out
  tiling->maxbuf = 1.0f;
  tiling->factor_cl = 2.0f; // in + out; no intermediate at all
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

int distort_transform(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                      float *const __restrict points, size_t points_count)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  if(!_lens_data_available(d)) return 0;

  const float orig_w = piece->buf_in.width, orig_h = piece->buf_in.height;
  int modflags;

  const int used_lf_mask = _lens_mask_for_mono(dt_image_is_monochrome(&self->dev->image_storage));

  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, TRUE, &modifier, NULL);

  if(_lens_flags_move_pixels(modflags))
  {
    __OMP_PARALLEL_FOR__(firstprivate(points, points_count, modifier) if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      ls_modifier_apply_subpixel_geometry(&modifier, points[i], points[i + 1], 1, 1, buf);
      // take green channel distortion, like distort_mask() does, so x and y come from the
      // same color channel's distortion field instead of mixing red's x with green's y.
      points[i] = buf[2];
      points[i + 1] = buf[3];
    }
  }


  return 1;
}

int distort_backtransform(dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe, const dt_dev_pixelpipe_iop_t *piece,
                          float *const __restrict points, size_t points_count)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  if(!_lens_data_available(d)) return 0;

  const int used_lf_mask = _lens_mask_for_mono(dt_image_is_monochrome(&self->dev->image_storage));

  const float orig_w = piece->buf_in.width, orig_h = piece->buf_in.height;
  int modflags;
  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d, used_lf_mask, FALSE, &modifier, NULL);


  if(_lens_flags_move_pixels(modflags))
  {
    __OMP_PARALLEL_FOR__(firstprivate(points_count, modifier, points) if(points_count > 100))
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      ls_modifier_apply_subpixel_geometry(&modifier, points[i], points[i + 1], 1, 1, buf);
      // take green channel distortion, like distort_mask() does, so x and y come from the
      // same color channel's distortion field instead of mixing red's x with green's y.
      points[i] = buf[2];
      points[i + 1] = buf[3];
    }
  }


  return 1;
}

// TODO: Shall we keep DT_LENS_MODIFY_TCA in the modifiers?
void distort_mask(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe, struct dt_dev_pixelpipe_iop_t *piece,
                  const float *const in, float *const out, const dt_iop_roi_t *const roi_in,
                  const dt_iop_roi_t *const roi_out)
{
  (void)pipe;
  const dt_iop_lensfun_data_t *const d = (dt_iop_lensfun_data_t *)piece->data;

  if(!_lens_data_available(d))
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const float orig_w = roi_in->scale * piece->buf_in.width, orig_h = roi_in->scale * piece->buf_in.height;
  int modflags;
  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d,
               /*DT_LENS_MODIFY_TCA |*/ DT_LENS_MODIFY_DISTORTION | DT_LENS_MODIFY_GEOMETRY
                   | DT_LENS_MODIFY_SCALE,
               FALSE, &modifier, NULL);

  if(!_lens_flags_move_pixels(modflags))
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 1);
    return;
  }

  const struct dt_interpolation *const interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  // acquire temp memory for distorted pixel coords
  const size_t bufsize = (size_t)roi_out->width * 2 * 3;
  size_t padded_bufsize;
  float *const buf = dt_pixelpipe_cache_alloc_perthread_float(bufsize, &padded_bufsize);
  if(IS_NULL_PTR(buf)) return;
  __OMP_PARALLEL_FOR__(firstprivate(buf, padded_bufsize, d, modifier, in, out, interpolation, roi_in, roi_out))
  for(int y = 0; y < roi_out->height; y++)
  {
    float *bufptr = (float*)dt_get_perthread(buf, padded_bufsize);
    ls_modifier_apply_subpixel_geometry(&modifier, roi_out->x, roi_out->y + y, roi_out->width, 1, bufptr);

    // reverse transform the global coords from lf to our buffer
    float *_out = out + (size_t)y * roi_out->width;
    for(int x = 0; x < roi_out->width; x++, bufptr += 6, _out++)
    {
      if(d->do_nan_checks && (!isfinite(bufptr[2]) || !isfinite(bufptr[3])))
      {
        *_out = 0.0f;
        continue;
      }

      // take green channel distortion also for alpha channel
      const float pi0 = bufptr[2] - roi_in->x;
      const float pi1 = bufptr[3] - roi_in->y;
      *_out = dt_interpolation_compute_sample(interpolation, in, pi0, pi1, roi_in->width, roi_in->height, 1,
                                              roi_in->width);
    }
  }
  
  
  dt_pixelpipe_cache_free_align(buf);
}

void modify_roi_out(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                    struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

void modify_roi_in(struct dt_iop_module_t *self, const struct dt_dev_pixelpipe_t *pipe,
                   struct dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *const roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  *roi_in = *roi_out;
  // inverse transform with given params

  if(!_lens_data_available(d)) return;

  const float orig_w = roi_in->scale * piece->buf_in.width;
  const float orig_h = roi_in->scale * piece->buf_in.height;
  int modflags;
  ls_modifier_t modifier;
  get_modifier(&modflags, orig_w, orig_h, d, DT_LENS_MODIFY_ALL_AXES, FALSE, &modifier, NULL);

  if(_lens_flags_move_pixels(modflags))
  {
    const int xoff = roi_in->x;
    const int yoff = roi_in->y;
    const int width = roi_in->width;
    const int height = roi_in->height;
    const int awidth = abs(width);
    const int aheight = abs(height);
    const int xstep = (width < 0) ? -1 : 1;
    const int ystep = (height < 0) ? -1 : 1;

    float xm = FLT_MAX, xM = -FLT_MAX, ym = FLT_MAX, yM = -FLT_MAX;
    const size_t nbpoints = 2 * awidth + 2 * aheight;

  // ROI planning passes the active pipe now, but this temporary edge buffer only needs an
  // allocator bucket id, so use a stable generic bucket.
    float *const buf = (float *)dt_pixelpipe_cache_alloc_align_cache(sizeof(float) * nbpoints * 2 * 3,
                                                                     DT_DEV_PIXELPIPE_FULL);
    if(IS_NULL_PTR(buf)) return;

#ifdef _OPENMP
#pragma omp parallel default(none) reduction(min : xm, ym) reduction(max : xM, yM) \
  firstprivate(modifier, xoff, yoff, awidth, aheight, width, height, nbpoints, ystep, xstep, buf)
#endif
    {
      __OMP_FOR__()
      for(int i = 0; i < awidth; i++)
        ls_modifier_apply_subpixel_geometry(&modifier, xoff + i * xstep, yoff, 1, 1, buf + 6 * i);
      __OMP_FOR__()
      for(int i = 0; i < awidth; i++)
        ls_modifier_apply_subpixel_geometry(&modifier, xoff + i * xstep, yoff + (height - 1), 1, 1, buf + 6 * (awidth + i));
      __OMP_FOR__()
      for(int j = 0; j < aheight; j++)
        ls_modifier_apply_subpixel_geometry(&modifier, xoff, yoff + j * ystep, 1, 1, buf + 6 * (2 * awidth + j));
      __OMP_FOR__()
      for(int j = 0; j < aheight; j++)
        ls_modifier_apply_subpixel_geometry(&modifier, xoff + (width - 1), yoff + j * ystep, 1, 1, buf + 6 * (2 * awidth + aheight + j));

#ifdef _OPENMP
#pragma omp barrier
#endif
      __OMP_FOR__()
      for(size_t k = 0; k < nbpoints; k++)
      {
        // iterate over RGB channels x and y coordinates
        for(size_t c = 0; c < 6; c+=2)
        {
          const float x = buf[6 * k + c];
          const float y = buf[6 * k + c + 1];
          xm = isnan(x) ? xm : MIN(xm, x);
          xM = isnan(x) ? xM : MAX(xM, x);
          ym = isnan(y) ? ym : MIN(ym, y);
          yM = isnan(y) ? yM : MAX(yM, y);
        }
      }
    }

  dt_pixelpipe_cache_free_align(buf);

    // LensFun can return NAN coords, so we need to handle them carefully.
    if(!isfinite(xm) || !(0 <= xm && xm < orig_w)) xm = 0;
    if(!isfinite(xM) || !(1 <= xM && xM < orig_w)) xM = orig_w;
    if(!isfinite(ym) || !(0 <= ym && ym < orig_h)) ym = 0;
    if(!isfinite(yM) || !(1 <= yM && yM < orig_h)) yM = orig_h;

    const struct dt_interpolation *interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
    roi_in->x = fmaxf(0.0f, roundf(xm - interpolation->width));
    roi_in->y = fmaxf(0.0f, roundf(ym - interpolation->width));
    roi_in->width = roundf(fminf(orig_w - roi_in->x, xM - roi_in->x + interpolation->width));
    roi_in->height = roundf(fminf(orig_h - roi_in->y, yM - roi_in->y + interpolation->width));

    // sanity check.
    roi_in->x = CLAMP(roi_in->x, 0, (int)floorf(orig_w));
    roi_in->y = CLAMP(roi_in->y, 0, (int)floorf(orig_h));
    roi_in->width = CLAMP(roi_in->width, 1, (int)ceilf(orig_w) - roi_in->x);
    roi_in->height = CLAMP(roi_in->height, 1, (int)ceilf(orig_h) - roi_in->y);
  }
}

/* --- the shared geometry core ----------------------------------------------------------
 *
 * lens resolves its effective parameters and then builds a lensfun state out of them, and both
 * halves are needed twice: once for the pixel pipe, once for the record the geometry service
 * composes GUI coordinates from (develop/geometry/geometry.h). Expressed once here.
 *
 * Note what lens does NOT contribute: modify_roi_out() is the identity, so this module changes
 * no dimensions. It is on the geometry roster purely for its point transforms.
 */

/**
 * @brief Which parameters are actually in force.
 *
 * @details p->modified == 0 means an edit whose correction was never written down: the
 * parameters in force are the module's DEFAULTS, recomputed by reload_defaults() from the
 * image's EXIF and whatever the calibration database happens to say TODAY -- not the ones in
 * history.
 *
 * That is not a feature, it is a defect with a long tail. An image corrected on "auto" when
 * its lens had no vignetting calibration silently GAINS vignetting the day the database
 * learns one: same file, same history, different picture, and no way for the user to see it
 * coming or pin it down. Adding a second source made it worse still, since the same edit
 * would move to the maker's profile the day the reader for it shipped.
 *
 * So new edits no longer take this path: `modified` defaults to 1 and reload_defaults()
 * sets it, which writes the decision into history where it can be reproduced. This function
 * exists for the edits already saved with 0, which must keep rendering the way they always
 * have -- migrating them would change pictures the user never asked to change, which is the
 * very complaint.
 */
static const dt_iop_lensfun_params_t *_lens_effective_params(dt_iop_module_t *self,
                                                             const dt_iop_lensfun_params_t *const p)
{
  return (p->modified == 0) ? (const dt_iop_lensfun_params_t *)self->default_params : p;
}

/**
 * @brief Build the lensfun state from resolved parameters. THE constructor.
 *
 * @details @p d is zeroed or already owns a lens; either way it owns a fresh deep copy of the
 * database's calibration on return as a VALUE -- nothing is owned, nothing is freed, and
 * no lock is taken.
 */
static void _lens_build_data(dt_iop_module_t *self, const dt_iop_lensfun_params_t *const p,
                             dt_iop_lensfun_data_t *d)
{
  (void)self;
  memset(&d->ls_lens, 0, sizeof(d->ls_lens));
  d->ls_have = FALSE;

  /* No lock. The reader is lock-free by construction and its handle is thread-local, so a
   * pipeline thread resolving a lens no longer serialises against anything -- least of all
   * against RawSpeed decoding a file, which is what sharing dt_plugin_threadsafe_mutex()
   * used to mean. And nothing is owned on return: an ls_lens_t is a value, valid after the
   * handle that produced it is closed, so there is no deep copy to make and no delete to
   * forget. */
  long long mount_id = 0;
  float camera_crop = 0.f;

  /* The image's own crop factor, which reload_defaults() takes from EXIF and the camera
   * picker overwrites. Seeded unconditionally because piece->data is REUSED across commits:
   * assigning it only inside the lookup below left a piece whose previous params resolved a
   * camera still carrying that camera's crop when the current ones do not resolve one. */
  d->crop = p->crop;

  if(p->camera[0])
  {
    ls_camera_t camera;
    /* The stored camera name is a model with no maker -- what the picker writes and what
     * EXIF gives -- so the matcher is asked for one rather than guessing the other. */
    if(_ls_find_camera(NULL, p->camera, &camera))
    {
      d->crop = camera.crop_factor;
      camera_crop = camera.crop_factor;
      mount_id = camera.mount_id;
    }
  }

  /* No camera, no database lens -- and that is a correctness rule, not caution.
   *
   * The mount is what makes a lens name mean one lens. Without it the search runs over the
   * whole catalogue, where hundreds of unrelated optics share the names "fixed lens" and
   * "festes objektiv", tie on score, and are separated by nothing at all. Correcting from
   * an arbitrary pick among those is how a Ricoh GR II came to be corrected with a 5.9 mm
   * compact's distortion.
   *
   * It also restores the panel's own invariant. gui_update() shows a lens only when a
   * camera resolved, so this was the one path where the pipeline corrected from something
   * the panel could not name and the user could not check or override. The recovery is the
   * picker: choose the body by hand, the mount is known, and the correct lens resolves and
   * is shown. */
  if(p->lens[0] && mount_id > 0)
  {
    const long long lens_id = _ls_find_lens(mount_id, camera_crop, p->focal, p->lens);
    ls_db_t *db = _ls_db();
    if(lens_id >= 0 && !IS_NULL_PTR(db) && ls_db_lens_by_id(db, lens_id, &d->ls_lens) == 1)
    {
      d->ls_have = TRUE;
    }
  }
  else if(p->lens[0])
  {
    /* DT_DEBUG_PIPE, not ALWAYS: nobody chose this, so it is a default declining to guess
     * rather than a request that could not be honoured -- and this runs from the GUI's
     * availability query too, not only at commit. The panel already says it, by showing
     * neither a camera nor a lens. */
    dt_print(DT_DEBUG_PIPE,
             "[lens] `%s' is not a camera this database knows, so `%s' cannot be resolved to"
             " one lens; correcting from the database is declined\n", p->camera, p->lens);
  }

  /* Typed coefficients are a SOURCE of their own, not an edit applied to a database row.
   *
   * They used to be written inside the lookup above, over the calibration of a lens the
   * database had just returned -- so on a body-and-lens pair the database does not know,
   * d->ls_have stayed FALSE, _lens_data_available() answered no, and process(),
   * process_cl(), the three distort_*() callbacks and modify_roi_in() all copied their
   * input and returned. The two sliders moved and the pipeline never saw them, on exactly
   * the images manual correction exists for.
   *
   * With a database lens the manual entry still REPLACES its aberration, which is what
   * manual means, and the coefficients live in that lens's calibration frame. With none,
   * the frame is a lens declared calibrated on THIS camera, so ls_modifier_init()'s
   * calibration-crop / image-crop rescaling is 1 and the two numbers act directly.
   * aspect_ratio is deliberately left at 0, which the library reads as its own 1.5 default
   * -- the same normalisation a 3:2-calibrated database entry gives, so a coefficient does
   * not change meaning on the day a profile for the lens appears.
   *
   * Writing it here rather than there also means it no longer depends on p->lens[0] being
   * non-empty: an unnamed lens is not a reason to refuse numbers the user typed. */
  if(_lens_source_is(p, DT_LENS_AXIS_TCA, DT_LENS_SOURCE_MANUAL))
  {
    if(!d->ls_have)
    {
      d->ls_lens.type = LS_LENS_RECTILINEAR;
      d->ls_lens.crop_factor = d->crop;
      d->ls_lens.min_focal = p->focal;
      d->ls_lens.max_focal = p->focal;
      d->ls_have = TRUE;
    }

    /* One entry at the shooting focal is exactly what the two sliders describe. ls_lens_t
     * is this module's own copy, so overwriting the array is both cheaper and clearer than
     * upstream's remove-every-entry-then-add dance on a shared object -- which is what the
     * code here used to do, twice, under two different lensfun APIs. */
    d->ls_lens.n_tca = 1;
    d->ls_lens.tca[0].model = LS_TCA_LINEAR;
    d->ls_lens.tca[0].focal = p->focal;
    d->ls_lens.tca[0].terms[0] = p->tca_r;
    d->ls_lens.tca[0].terms[1] = p->tca_b;
    for(int i = 2; i < 6; i++) d->ls_lens.tca[0].terms[i] = 0.f;
  }

  d->modify_flags = p->modify_flags;
  if(dt_image_is_monochrome(&self->dev->image_storage)) d->modify_flags &= ~DT_LENS_MODIFY_TCA;
  d->inverse = p->inverse;
  d->scale = p->scale;
  d->focal = p->focal;
  d->aperture = p->aperture;
  d->distance = p->distance;
  d->target_geom = p->target_geom;
  d->do_nan_checks = TRUE;
  d->tca_override = p->tca_override;

  /* The maker's own profile, if any axis asked for it and the file carries one.
   *
   * Resolved HERE, at commit, for the same reason the database lens is: the pixel path gets
   * values and does no lookups. ls_vendor_resolve() normalises whichever vendor format the
   * file holds -- Sony, Fuji, Olympus or a DNG opcode list -- straight into the knot table
   * the evaluators consume; past that call nothing here knows which maker wrote it.
   *
   * The finetune is NULL: "as the maker measured", which is the library's documented
   * meaning for it. The per-class strength blends the vendor GUIs offer are not exposed by
   * this module. */
  d->knots_have = FALSE;
  d->knots_scale = 1.f;
  {
    gboolean wants_embedded = FALSE;
    for(dt_lens_axis_t axis = 0; axis < DT_LENS_AXIS_LAST; axis++)
      if(_lens_source_is(p, axis, DT_LENS_SOURCE_EMBEDDED)) wants_embedded = TRUE;

    if(wants_embedded)
    {
      const dt_image_t *const img = &self->dev->image_storage;
      float scale = 1.f;
      const int got = ls_vendor_resolve(&img->exif_correction, NULL,
                                        img->p_width, img->p_height, &d->ls_knots, &scale);

      /* The two failure reports stay distinct, because they mean different things to the
       * person reading them: "this camera embeds nothing" is a fact about the hardware,
       * "we failed to read what it embedded" is a bug to report. Both then correct from
       * the database, and both say so -- a user who picked a source and got another one
       * is entitled to know. Once, at commit; not per pipe per frame. */
      if(got == 0)
        dt_print(DT_DEBUG_ALWAYS,
                 "[lens] an axis asks for the vendor profile but this file carries none;"
                 " correcting from the community profile instead\n");
      else if(got < 0)
        dt_print(DT_DEBUG_ALWAYS,
                 "[lens] this file's vendor profile could not be decoded;"
                 " correcting from the community profile instead\n");
      else
      {
        d->knots_scale = (scale > 0.f) ? scale : 1.f;
        d->knots_have = TRUE;
      }
    }
  }

  /*
   * there are certain situations when LensFun can return NAN coordinated.
   * most common case would be when the FOV is increased.
   */
  if(d->target_geom == DT_LENS_RECTILINEAR)
  {
    d->do_nan_checks = FALSE;
  }
  else if((int)d->target_geom == (int)d->ls_lens.type)
  {
    d->do_nan_checks = FALSE;
  }
}

/** @brief The lensfun modify mask this image allows: monochrome sensors get no TCA correction. */
static int _lens_used_mask(dt_iop_module_t *self)
{
  return _lens_mask_for_mono(dt_image_is_monochrome(&self->dev->image_storage));
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_lensfun_params_t *p = _lens_effective_params(self, (dt_iop_lensfun_params_t *)p1);

  // FIXME: this is utter shit and should be made into a GUI "mode".
  // If p->modified == 0, mode = auto and hide all controls
  // if p->modidified == 1, mode = manual and show all controls.
  if(((dt_iop_lensfun_params_t *)p1)->modified == 0)
  {
    // Temporary fix pending GUI unfucking
    dt_iop_compute_module_hash(self, self->dev->forms);
  }

  _lens_build_data(self, p, (dt_iop_lensfun_data_t *)piece->data);

  piece->cache_output_on_ram = TRUE;
}

/* --- the geometry service's view of this module (develop/geometry/geometry.h) ---------
 *
 * The one record in the service whose payload is not plain data: evaluating a lens correction
 * needs the resolved calibration, so the record owns a
 * deep copy and frees it. That is what dt_geometry_record_t::free_data exists for.
 */

typedef struct dt_iop_lens_geometry_t
{
  dt_iop_lensfun_data_t data;   /**< its own copy, exactly like a pipe piece has */
  int used_lf_mask;
} dt_iop_lens_geometry_t;

static void _lens_free_data(void *ptr)
{
  dt_iop_lens_geometry_t *g = (dt_iop_lens_geometry_t *)ptr;
  if(!g) return;
  free(g);
}

/** @brief Apply the correction to points. @p inverse selects the direction, as get_modifier()
 *  means it: distort_transform() passes TRUE, distort_backtransform() passes FALSE. */
static int _lens_geometry_apply(const void *data, const dt_geometry_record_t *const record,
                                float *points, size_t points_count, gboolean inverse)
{
  const dt_iop_lens_geometry_t *const g = (const dt_iop_lens_geometry_t *)data;
  const dt_iop_lensfun_data_t *const d = &g->data;

  if(!_lens_data_available(d)) return 0;
  if(record->in.width <= 0 || record->in.height <= 0) return 0;

  int modflags = 0;
  ls_modifier_t modifier;
  if(!get_modifier(&modflags, record->in.width, record->in.height, d, g->used_lf_mask, inverse,
                   &modifier, NULL))
    return 0;

  if(_lens_flags_move_pixels(modflags))
  {
    for(size_t i = 0; i < points_count * 2; i += 2)
    {
      float DT_ALIGNED_ARRAY buf[6];
      ls_modifier_apply_subpixel_geometry(&modifier, points[i], points[i + 1], 1, 1, buf);
      // green channel, like distort_transform() and distort_mask() do, so x and y come from the
      // same colour channel's distortion field instead of mixing red's x with green's y.
      points[i] = buf[2];
      points[i + 1] = buf[3];
    }
  }

  return 1;
}

static int _lens_geometry_transform(const void *data, const dt_geometry_record_t *const record,
                                    dt_geometry_chain_t *chain, float *points, size_t points_count)
{
  return _lens_geometry_apply(data, record, points, points_count, TRUE);
}

static int _lens_geometry_backtransform(const void *data, const dt_geometry_record_t *const record,
                                        dt_geometry_chain_t *chain, float *points, size_t points_count)
{
  return _lens_geometry_apply(data, record, points, points_count, FALSE);
}

static const dt_geometry_vtable_t _lens_geometry_vtable = {
  /* .map_size = */ NULL,   // modify_roi_out() is the identity: lens changes no dimensions
  /* .transform = */ _lens_geometry_transform,
  /* .backtransform = */ _lens_geometry_backtransform,
};

gboolean geometry_record(dt_iop_module_t *self, const void *params, dt_geometry_record_t *record)
{
  dt_iop_lens_geometry_t *g = (dt_iop_lens_geometry_t *)calloc(1, sizeof(dt_iop_lens_geometry_t));
  if(!g) return FALSE;

  const dt_iop_lensfun_params_t *p
      = _lens_effective_params(self, (const dt_iop_lensfun_params_t *)params);
  _lens_build_data(self, p, &g->data);
  g->used_lf_mask = _lens_used_mask(self);

  record->data = g;
  record->free_data = _lens_free_data;
  record->vtable = &_lens_geometry_vtable;
  return TRUE;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = dt_calloc_align(sizeof(dt_iop_lensfun_data_t));
  piece->data_size = sizeof(dt_iop_lensfun_data_t);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  /* init_pipe() may have failed to allocate, and cleanup runs regardless. */
  if(IS_NULL_PTR(piece->data)) return;

  /* Nothing to free but the piece itself: ls_lens_t is a value living inside it, where the
   * lfLens it replaces was a heap object this had to remember to delete. */
  dt_free_align(piece->data);
  piece->data = NULL;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_lensfun_global_data_t *gd
      = (dt_iop_lensfun_global_data_t *)calloc(1, sizeof(dt_iop_lensfun_global_data_t));
  module->data = gd;
  gd->kernel_lens_distort_bilinear = dt_opencl_create_kernel(program, "lens_distort_bilinear");
  gd->kernel_lens_distort_bicubic = dt_opencl_create_kernel(program, "lens_distort_bicubic");
  gd->kernel_lens_distort_mitchell = dt_opencl_create_kernel(program, "lens_distort_mitchell");
  gd->kernel_lens_vignette = dt_opencl_create_kernel(program, "lens_vignette");

  /* Nothing to pre-warm any more. Opening the calibration database is one mmap of an
   * already-parsed file, done lazily per thread on first use and measured at 0.18 ms --
   * there is no 100 ms XML parse left to hide behind a startup thread. */
}

static float get_autoscale(dt_iop_module_t *self, dt_iop_lensfun_params_t *p);

void reload_defaults(dt_iop_module_t *module)
{
  char *new_lens;
  const dt_image_t *img = &module->dev->image_storage;

  // reload image specific stuff
  // get all we can from exif:
  dt_iop_lensfun_params_t *d = (dt_iop_lensfun_params_t *)module->default_params;

  new_lens = _lens_sanitize(img->exif_lens);
  g_strlcpy(d->lens, new_lens, sizeof(d->lens));
  dt_free(new_lens);
  g_strlcpy(d->camera, img->exif_model, sizeof(d->camera));
  d->crop = img->exif_crop;
  d->aperture = img->exif_aperture;
  d->focal = img->exif_focal_length;
  d->scale = 1.0;
  d->modify_flags = DT_LENS_MODIFY_TCA | DT_LENS_MODIFY_VIGNETTING | DT_LENS_MODIFY_DISTORTION |
                    DT_LENS_MODIFY_GEOMETRY | DT_LENS_MODIFY_SCALE;

  /* Everything decided here is now WRITTEN DOWN, rather than recomputed whenever the image
   * is opened. See _lens_effective_params() for what the alternative cost. */
  d->modified = 1;

  /* Prefer what the camera measured about its own lens, per axis, whenever the file carries
   * it. The maker had the actual body and the actual lens on a bench; the database has a
   * community measurement of that model. Where both exist the maker's is the better default,
   * and where only one exists this picks the one that works.
   *
   * Per axis, because the makers do not all write the same set: an Olympus body publishes
   * distortion and lateral CA and no falloff, so that axis correctly keeps the database.
   *
   * Scale stays 1: the embedded resolver applies the profile's own autoscale, which already
   * clears the borders it leaves. Anything else here would be a second, arbitrary zoom on
   * top of a factor the maker chose. */
  for(dt_lens_axis_t axis = 0; axis < DT_LENS_AXIS_LAST; axis++)
  {
    if(!_lens_source_applicable(axis, DT_LENS_SOURCE_EMBEDDED)) continue;
    if(_lens_image_embeds(module, axis))
      _lens_source_set(d, axis, DT_LENS_SOURCE_EMBEDDED);
  }
  // if we did not find focus_distance in EXIF, lets default to 1000
  d->distance = img->exif_focus_distance == 0.0f ? 1000.0f : img->exif_focus_distance;
  d->target_geom = DT_LENS_RECTILINEAR;

  if(dt_image_is_monochrome(img))
    d->modify_flags &= ~DT_LENS_MODIFY_TCA;

  // init crop from db:
  char model[100]; // truncate often complex descriptions.
  g_strlcpy(model, img->exif_model, sizeof(model));
  for(char cnt = 0, *c = model; c < model + 100 && *c != '\0'; c++)
    if(*c == ' ')
      if(++cnt == 2) *c = '\0';
  if(img->exif_maker[0] || model[0])
  {
    ls_camera_t cam;
    if(!_ls_find_camera(img->exif_maker, img->exif_model, &cam)) return;

    /* Upstream spells a real fact into the mount NAME: a lower-case initial means a
     * fixed-lens camera. That is how a compact is told from an interchangeable-lens body,
     * and it decides both branches below. */
    char mount[128] = { 0 };
    ls_db_t *db = _ls_db();
    if(IS_NULL_PTR(db)) return;
    ls_db_mount_name(db, cam.mount_id, mount, sizeof(mount));
    const gboolean fixed_lens = (mount[0] != '\0') && islower((unsigned char)mount[0]);

    long long lens_id = _ls_find_lens(cam.mount_id, cam.crop_factor, d->focal, d->lens);

    if(lens_id < 0 && fixed_lens)
    {
      /* A fixed-lens camera whose EXIF lens string matched nothing -- it is "(65535)", or
       * a name upstream files as "fixed lens". The lens is whatever is built into this
       * mount, so ask the mount directly instead of matching a name. */
      g_strlcpy(d->lens, "", sizeof(d->lens));

      const int n = ls_db_lenses_for_mount(db, cam.mount_id, NULL, 0);
      if(n > 0)
      {
        long long *ids = (long long *)dt_alloc_align(sizeof(long long) * (size_t)n);
        if(!IS_NULL_PTR(ids))
        {
          ls_db_lenses_for_mount(db, cam.mount_id, ids, n);
          /* The shortest model name, as before: a fixed-lens mount can carry several
           * entries for one physical lens and the shortest is the plain one. */
          size_t shortest = SIZE_MAX;
          for(int i = 0; i < n; i++)
          {
            char maker[128] = "", lmodel[256] = "";
            if(ls_db_lens_name(db, ids[i], maker, sizeof(maker), lmodel, sizeof(lmodel)) <= 0)
              continue;
            const size_t len = strlen(lmodel);
            if(len < shortest)
            {
              shortest = len;
              lens_id = ids[i];
              g_strlcpy(d->lens, lmodel, sizeof(d->lens));
            }
          }
          dt_free_align(ids);
        }
      }
    }

    if(lens_id >= 0)
    {
      ls_lens_t lens;
      if(ls_db_lens_by_id(db, lens_id, &lens) == 1)
        d->target_geom = (dt_lens_type_t)lens.type;
    }

    d->crop = cam.crop_factor;
    d->scale = get_autoscale(module, d);
    module->workflow_enabled = dt_image_needs_rawprepare(img);
  }

  // reload_defaults() stays params-only and never touches gui_data.
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)module->data;

  /* Before anything is freed: the pre-warm thread may still be building the database. */
  /* No database to tear down and no thread to join. Each thread's handle closes itself
   * when that thread ends, and the one-entry caches beside it die with it. */

  dt_opencl_free_kernel(gd->kernel_lens_distort_bilinear);
  dt_opencl_free_kernel(gd->kernel_lens_distort_bicubic);
  dt_opencl_free_kernel(gd->kernel_lens_distort_mitchell);
  dt_opencl_free_kernel(gd->kernel_lens_vignette);
  dt_free(module->data);
}

/// ############################################################
/// gui stuff: inspired by ufraws lensfun tab:

/* simple function to compute the floating-point precision
   which is enough for "normal use". The criteria is to have
   about 3 leading digits after the initial zeros.  */
static int precision(double x, double adj)
{
  x *= adj;

  if(x == 0) return 1;
  if(x < 1.0)
    if(x < 0.1)
      if(x < 0.01)
        return 5;
      else
        return 4;
    else
      return 3;
  else if(x < 100.0)
    if(x < 10.0)
      return 2;
    else
      return 1;
  else
    return 0;
}

/* -- ufraw ptr array functions -- */

static int ptr_array_insert_sorted(GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  g_ptr_array_set_size(array, length + 1);
  const void **root = (const void **)array->pdata;

  int m = 0, l = 0, r = length - 1;

  // Skip trailing NULL, if any
  if(l <= r && !root[r]) r--;

  while(l <= r)
  {
    m = (l + r) / 2;
    int cmp = compare(root[m], item);

    if(cmp == 0)
    {
      ++m;
      goto done;
    }
    else if(cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }
  if(r == m) m++;

done:
  memmove(root + m + 1, root + m, sizeof(void *) * (length - m));
  root[m] = item;
  return m;
}

static int ptr_array_find_sorted(const GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  void **root = array->pdata;

  int l = 0, r = length - 1;
  int m = 0, cmp = 0;

  if(!length) return -1;

  // Skip trailing NULL, if any
  if(!root[r]) r--;

  while(l <= r)
  {
    m = (l + r) / 2;
    cmp = compare(root[m], item);

    if(cmp == 0)
      return m;
    else if(cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }

  return -1;
}

static void ptr_array_insert_index(GPtrArray *array, const void *item, int index)
{
  const void **root;
  int length = array->len;
  g_ptr_array_set_size(array, length + 1);
  root = (const void **)array->pdata;
  memmove(root + index + 1, root + index, sizeof(void *) * (length - index));
  root[index] = item;
}

/* -- end ufraw ptr array functions -- */

/* -- camera -- */

/**
 * @brief Write the camera the user picked into the params. USER INTERACTION ONLY.
 *
 * @details Split out of camera_set(), which refreshes the view and nothing else.
 * gui_update() calls that one on every panel refresh, and it used to write p->camera and
 * p->crop as it went -- so merely opening the module rewrote the edit, replacing the stored
 * camera string with the database's own spelling of the same body and the stored crop with
 * that row's crop factor, with nothing but whatever committed history next deciding whether
 * it stuck. Parameters change on user interaction and in reload_defaults(). Nowhere else.
 */
static void _lens_params_set_camera(dt_iop_module_t *self, const long long camera_id)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  ls_db_t *db = _ls_db();
  char maker[128] = "", model[256] = "", variant[128] = "";
  ls_camera_t cam;
  if(camera_id < 0 || IS_NULL_PTR(db)
     || ls_db_camera_name(db, camera_id, maker, sizeof(maker), model, sizeof(model),
                          variant, sizeof(variant)) != 1
     || ls_db_camera_by_id(db, camera_id, &cam) != 1)
    return;

  g_strlcpy(p->camera, model, sizeof(p->camera));
  p->crop = cam.crop_factor;
}

/**
 * @brief Show a camera in the panel. VIEW ONLY -- writes no parameter.
 * @param camera_id the database id, or < 0 to clear the widget.
 *
 * @details It takes an ID rather than a pointer because a camera is no longer a durable
 * object owned by a process-wide database -- it is a row, read on demand. The menu items
 * below carry the same id, so nothing holds a pointer whose lifetime it does not control.
 */
static void camera_set(dt_iop_module_t *self, long long camera_id)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);

  ls_db_t *db = _ls_db();
  char maker[128] = "", model[256] = "", variant[128] = "";
  ls_camera_t cam;
  if(camera_id < 0 || IS_NULL_PTR(db)
     || ls_db_camera_name(db, camera_id, maker, sizeof(maker), model, sizeof(model),
                          variant, sizeof(variant)) != 1
     || ls_db_camera_by_id(db, camera_id, &cam) != 1)
  {
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), "");
    gtk_widget_set_tooltip_text(GTK_WIDGET(g->camera_model), "");
    g->camera_id = -1;
    return;
  }

  g->camera_id = camera_id;

  gchar *fm = maker[0] ? g_strdup_printf("%s, %s", maker, model) : g_strdup(model);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), fm);
  dt_free(fm);

  // sizeof(variant) + 4, not 128: the format adds " (" and ")" around a string that can
  // itself fill variant[], so an equal-sized buffer drops the closing parenthesis on a long
  // camera variant. Derived from variant's size so it stays right if that changes.
  char _variant[sizeof(variant) + 4];
  if(variant[0])
    snprintf(_variant, sizeof(_variant), " (%s)", variant);
  else
    _variant[0] = 0;

  char mount[128] = "";
  ls_db_mount_name(db, cam.mount_id, mount, sizeof(mount));

  fm = g_strdup_printf(_("maker:\t\t%s\n"
                         "model:\t\t%s%s\n"
                         "mount:\t\t%s\n"
                         "crop factor:\t%.1f"),
                       maker, model, _variant, mount, cam.crop_factor);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->camera_model), fm);
  dt_free(fm);
}

static void camera_menu_select(GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  /* First, and for the whole handler: a suppressed callback is a programmatic widget
   * update, and this one writes parameters and commits history. */
  if(dt_gui_widgets_suppressed()) return;

  const long long camera_id =
      (long long)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuitem), "lens-camera-id"));

  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  _lens_params_set_camera(self, camera_id);
  p->modified = 1;

  /* View second, because it reads the parameters just written. The camera decides the mount
   * and the crop factor, and a lens is matched against those -- so a lens that matched the
   * old camera may not match the new one, and the other way round. Every other caller of
   * camera_set() runs a lens_set() straight afterwards and gets the rebuild from there;
   * this one does not, so it asks itself. */
  {
    dt_gui_widget_freeze();
    camera_set(self, camera_id);
    _lens_rebuild_axis_rows(self);
    _lens_gui_update_sensitivity(self);
  }

  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

/**
 * @brief Build the camera picker from a list of database ids.
 *
 * @param ids the cameras to offer, @p n of them. Grouped by maker into submenus, as before.
 */
static void camera_menu_fill(dt_iop_module_t *self, const long long *ids, int n)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  GPtrArray *makers, *submenus;

  if(g->camera_menu)
  {
    gtk_widget_destroy(GTK_WIDGET(g->camera_menu));
    g->camera_menu = NULL;
  }

  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db)) return;

  /* Count all existing camera makers and create a sorted list */
  makers = g_ptr_array_new_with_free_func(dt_free_gpointer);
  submenus = g_ptr_array_new();
  for(int i = 0; i < n; i++)
  {
    char maker[128] = "", model[256] = "", variant[128] = "";
    if(ls_db_camera_name(db, ids[i], maker, sizeof(maker), model, sizeof(model),
                         variant, sizeof(variant)) != 1)
      continue;

    GtkWidget *submenu, *item;
    int idx = ptr_array_find_sorted(makers, maker, (GCompareFunc)g_utf8_collate);
    if(idx < 0)
    {
      /* No such maker yet, insert it into the array. The strings are OWNED now: they used
       * to point into a database that outlived the menu, and they no longer do. */
      idx = ptr_array_insert_sorted(makers, g_strdup(maker), (GCompareFunc)g_utf8_collate);
      /* Create a submenu for cameras by this maker */
      submenu = gtk_menu_new();
      ptr_array_insert_index(submenus, submenu, idx);
    }

    submenu = (GtkWidget *)g_ptr_array_index(submenus, idx);
    /* Append current camera name to the submenu */
    if(!variant[0])
      item = gtk_menu_item_new_with_label(model);
    else
    {
      gchar *fm = g_strdup_printf("%s (%s)", model, variant);
      item = gtk_menu_item_new_with_label(fm);
      dt_free(fm);
    }
    gtk_widget_show(item);
    g_object_set_data(G_OBJECT(item), "lens-camera-id", GINT_TO_POINTER((gint)ids[i]));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(camera_menu_select), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
  }

  g->camera_menu = GTK_MENU(gtk_menu_new());
  for(unsigned i = 0; i < makers->len; i++)
  {
    GtkWidget *item = (GtkWidget *)gtk_menu_item_new_with_label((const gchar *)g_ptr_array_index(makers, i));
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->camera_menu), item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), (GtkWidget *)g_ptr_array_index(submenus, i));
  }

  g_ptr_array_free(submenus, TRUE);
  g_ptr_array_free(makers, TRUE);
}

/** @brief Every camera in the database, as ids the caller must free with dt_free_align(). */
static long long *_camera_all_ids(int *out_n)
{
  *out_n = 0;
  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db)) return NULL;
  const int n = ls_db_list_cameras(db, NULL, 0);
  if(n <= 0) return NULL;
  long long *ids = (long long *)dt_alloc_align(sizeof(long long) * (size_t)n);
  if(IS_NULL_PTR(ids)) return NULL;
  *out_n = ls_db_list_cameras(db, ids, n);
  return ids;
}

static void parse_model(const char *txt, char *model, size_t sz_model)
{
  while(txt[0] && isspace(txt[0])) txt++;
  size_t len = strlen(txt);
  if(len > sz_model - 1) len = sz_model - 1;
  memcpy(model, txt, len);
  model[len] = 0;
}

static void camera_menusearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);

  (void)button;

  int n = 0;
  long long *ids = _camera_all_ids(&n);
  if(IS_NULL_PTR(ids)) return;
  camera_menu_fill(self, ids, n);
  dt_free_align(ids);

  dt_gui_menu_popup(GTK_MENU(g->camera_menu), button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}


/* -- end camera -- */

static void lens_comboentry_focal_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->focal);
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void lens_comboentry_aperture_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->aperture);
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void lens_comboentry_distance_update(GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text) (void)sscanf(text, "%f", &p->distance);
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void delete_children(GtkWidget *widget, gpointer data)
{
  (void)data;
  gtk_widget_destroy(widget);
}

/** @brief A projection's name, replacing lfLens::GetLensTypeDesc(). */
static const char *_lens_type_name(int type)
{
  switch(type)
  {
    case DT_LENS_RECTILINEAR:           return _("rectilinear");
    case DT_LENS_FISHEYE:               return _("fisheye");
    case DT_LENS_PANORAMIC:             return _("panoramic");
    case DT_LENS_EQUIRECTANGULAR:       return _("equirectangular");
    case DT_LENS_FISHEYE_ORTHOGRAPHIC:  return _("orthographic fisheye");
    case DT_LENS_FISHEYE_STEREOGRAPHIC: return _("stereographic fisheye");
    case DT_LENS_FISHEYE_EQUISOLID:     return _("equisolid fisheye");
    case DT_LENS_FISHEYE_THOBY:         return _("Thoby fisheye");
    default:                            return _("unknown");
  }
}

/**
 * @brief Write the lens the user picked into the params. USER INTERACTION ONLY.
 *
 * @details The counterpart of _lens_params_set_camera(), for the same reason: lens_set()
 * refreshes the view and is called from gui_update(), so it cannot be the thing that writes
 * p->lens.
 */
static void _lens_params_set_lens(dt_iop_module_t *self, const long long lens_id)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  ls_db_t *db = _ls_db();
  char l_maker[128] = "", l_model[256] = "";
  if(lens_id < 0 || IS_NULL_PTR(db)
     || ls_db_lens_name(db, lens_id, l_maker, sizeof(l_maker), l_model, sizeof(l_model)) <= 0)
    return;

  g_strlcpy(p->lens, l_model, sizeof(p->lens));
}

/** @brief Show a lens in the panel, and offer the sources it makes available. VIEW ONLY --
 *  writes no parameter. */
static void lens_set(dt_iop_module_t *self, long long lens_id)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  /* const, and effective: this reads the focal length, aperture and distance to seed the
   * editable comboboxes, and shows what is in force rather than what is stored. */
  const dt_iop_lensfun_params_t *const p = _lens_effective_params(
      self, (const dt_iop_lensfun_params_t *)self->params);

  gchar *fm;
  const char *maker, *model;
  unsigned i;
  gdouble focal_values[]
      = { -INFINITY, 4.5, 8,   10,  12,  14,  15,  16,  17,  18,  20,  24,  28,   30,      31,  35,
          38,        40,  43,  45,  50,  55,  60,  70,  75,  77,  80,  85,  90,   100,     105, 110,
          120,       135, 150, 200, 210, 240, 250, 300, 400, 500, 600, 800, 1000, INFINITY };
  gdouble aperture_values[]
      = { -INFINITY, 0.7, 0.8, 0.9, 1, 1.1, 1.2, 1.4, 1.8, 2,  2.2, 2.5, 2.8, 3.2, 3.4, 4,  4.5, 5.0,
          5.6,       6.3, 7.1, 8,   9, 10,  11,  13,  14,  16, 18,  20,  22,  25,  29,  32, 38,  INFINITY };

  ls_db_t *db = _ls_db();
  ls_lens_t lens_v;
  char l_maker[128] = "", l_model[256] = "";
  float min_focal = 0.f, max_focal = 0.f, min_ap = 0.f, max_ap = 0.f;
  const gboolean have = (lens_id >= 0) && !IS_NULL_PTR(db)
                        && (ls_db_lens_by_id(db, lens_id, &lens_v) == 1)
                        && (ls_db_lens_name(db, lens_id, l_maker, sizeof(l_maker),
                                            l_model, sizeof(l_model)) > 0);
  if(have) ls_db_lens_range(db, lens_id, &min_focal, &max_focal, &min_ap, &max_ap);

  /* Which sources exist is a property of the lens, so it is re-decided HERE and not only
   * when the image is loaded: picking a different lens from the menu is exactly the moment
   * a database profile appears or disappears. Without this the panel kept whatever rows the
   * image arrived with, and choosing a lens the database does know left every axis still
   * offering nothing. */
  if(!have)
  {
    /* Nothing is disabled on this path. Availability is expressed by which ROWS a picker
     * holds, never by whether the picker is sensitive -- greying the whole widget withdrew
     * the two sources that need no database entry at all, the file's own embedded profile
     * and hand-typed TCA coefficients, which is precisely what a lens the database has
     * never heard of is left with. */
    _lens_rebuild_axis_rows(self);
    _lens_gui_update_sensitivity(self);
    return;
  }

  maker = l_maker[0] ? l_maker : NULL;
  model = l_model[0] ? l_model : NULL;

  if(model)
  {
    if(maker)
      fm = g_strdup_printf("%s, %s", maker, model);
    else
      fm = g_strdup_printf("%s", model);
    gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), fm);
    dt_free(fm);
  }

  char focal[100], aperture[100], mounts[200];

  if(min_focal < max_focal)
    snprintf(focal, sizeof(focal), "%g-%gmm", min_focal, max_focal);
  else
    snprintf(focal, sizeof(focal), "%gmm", min_focal);
  if(min_ap < max_ap)
    snprintf(aperture, sizeof(aperture), "%g-%g", min_ap, max_ap);
  else
    snprintf(aperture, sizeof(aperture), "%g", min_ap);

  mounts[0] = 0;
  ls_db_lens_mounts(db, lens_id, mounts, sizeof(mounts));

  fm = g_strdup_printf(_("maker:\t\t%s\n"
                         "model:\t\t%s\n"
                         "focal range:\t%s\n"
                         "aperture:\t%s\n"
                         "crop factor:\t%.1f\n"
                         "type:\t\t%s\n"
                         "mounts:\t%s"),
                       maker ? maker : "?", model ? model : "?", focal, aperture,
                       lens_v.crop_factor, _lens_type_name((int)lens_v.type), mounts);

  gtk_widget_set_tooltip_text(GTK_WIDGET(g->lens_model), fm);
  dt_free(fm);

  /* Create the focal/aperture/distance combo boxes */
  gtk_container_foreach(GTK_CONTAINER(g->lens_param_box), delete_children, NULL);

  int ffi = 1, fli = -1;
  for(i = 1; i < sizeof(focal_values) / sizeof(gdouble) - 1; i++)
  {
    if(focal_values[i] < min_focal) ffi = i + 1;
    if(focal_values[i] > max_focal && fli == -1) fli = i;
  }
  if(focal_values[ffi] > min_focal)
  {
    focal_values[ffi - 1] = min_focal;
    ffi--;
  }
  if(max_focal == 0 || fli < 0) fli = sizeof(focal_values) / sizeof(gdouble) - 2;
  if(focal_values[fli + 1] < max_focal)
  {
    focal_values[fli + 1] = max_focal;
    ffi++;
  }
  if(fli < ffi) fli = ffi + 1;

  GtkWidget *w;
  char txt[30];

  // focal length
  w = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(w, N_("mm"));
  gtk_widget_set_tooltip_text(w, _("focal length (mm)"));
  snprintf(txt, sizeof(txt), "%.*f", precision(p->focal, 10.0), p->focal);
  dt_bauhaus_combobox_add(w, txt);
  for(int k = 0; k < fli - ffi; k++)
  {
    snprintf(txt, sizeof(txt), "%.*f", precision(focal_values[ffi + k], 10.0), focal_values[ffi + k]);
    dt_bauhaus_combobox_add(w, txt);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(lens_comboentry_focal_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[0] = w;

  // f-stop
  ffi = 1, fli = sizeof(aperture_values) / sizeof(gdouble) - 1;
  for(i = 1; i < sizeof(aperture_values) / sizeof(gdouble) - 1; i++)
    if(aperture_values[i] < min_ap) ffi = i + 1;
  if(aperture_values[ffi] > min_ap)
  {
    aperture_values[ffi - 1] = min_ap;
    ffi--;
  }

  w = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(w, N_("f"));
  gtk_widget_set_tooltip_text(w, _("f-number (aperture)"));
  snprintf(txt, sizeof(txt), "%.*f", precision(p->aperture, 10.0), p->aperture);
  dt_bauhaus_combobox_add(w, txt);
  for(int k = 0; k < fli - ffi; k++)
  {
    snprintf(txt, sizeof(txt), "%.*f", precision(aperture_values[ffi + k], 10.0), aperture_values[ffi + k]);
    dt_bauhaus_combobox_add(w, txt);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(lens_comboentry_aperture_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[1] = w;

  w = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(w, N_("d"));
  gtk_widget_set_tooltip_text(w, _("distance to subject"));
  snprintf(txt, sizeof(txt), "%.*f", precision(p->distance, 10.0), p->distance);
  dt_bauhaus_combobox_add(w, txt);
  float val = 0.25f;
  for(int k = 0; k < 25; k++)
  {
    if(val > 1000.0f) val = 1000.0f;
    snprintf(txt, sizeof(txt), "%.*f", precision(val, 10.0), val);
    dt_bauhaus_combobox_add(w, txt);
    if(val >= 1000.0f) break;
    val *= sqrtf(2.0f);
  }
  g_signal_connect(G_OBJECT(w), "value-changed", G_CALLBACK(lens_comboentry_distance_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[2] = w;

  gtk_widget_show_all(g->lens_param_box);

  /* Last, because the rows are read out of the params, which a caller acting on user input
   * has already written through _lens_params_set_lens(). */
  _lens_rebuild_axis_rows(self);
  _lens_gui_update_sensitivity(self);
}

static void lens_menu_select(GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  if(dt_gui_widgets_suppressed()) return;

  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const long long lens_id =
      (long long)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(menuitem), "lens-id"));

  /* Parameters first: both the autoscale below and the row rebuild inside lens_set() ask
   * the database what THIS lens offers, and they read it out of the params. */
  _lens_params_set_lens(self, lens_id);
  p->modified = 1;
  p->scale = get_autoscale(self, p);

  {
    /* Widget writes, not user input. Without the freeze dt_bauhaus_slider_set() emits
     * value-changed, whose default callback writes the field again and commits a SECOND
     * history item for the one lens the user picked. */
    dt_gui_widget_freeze();
    lens_set(self, lens_id);
    dt_bauhaus_slider_set(g->scale, p->scale);
  }

  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

static void lens_menu_fill(dt_iop_module_t *self, const long long *ids, int n)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  GPtrArray *makers, *submenus;

  if(g->lens_menu)
  {
    gtk_widget_destroy(GTK_WIDGET(g->lens_menu));
    g->lens_menu = NULL;
  }

  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db)) return;

  /* Count all existing lens makers and create a sorted list */
  makers = g_ptr_array_new_with_free_func(dt_free_gpointer);
  submenus = g_ptr_array_new();
  for(int i = 0; i < n; i++)
  {
    char maker[128] = "", model[256] = "";
    if(ls_db_lens_name(db, ids[i], maker, sizeof(maker), model, sizeof(model)) <= 0) continue;

    GtkWidget *submenu, *item;
    int idx = ptr_array_find_sorted(makers, maker, (GCompareFunc)g_utf8_collate);
    if(idx < 0)
    {
      /* No such maker yet, insert it into the array. Owned strings: these no longer point
       * into a database that outlives the menu. */
      idx = ptr_array_insert_sorted(makers, g_strdup(maker), (GCompareFunc)g_utf8_collate);
      /* Create a submenu for lenses by this maker */
      submenu = gtk_menu_new();
      ptr_array_insert_index(submenus, submenu, idx);
    }

    submenu = (GtkWidget *)g_ptr_array_index(submenus, idx);
    /* Append current lens name to the submenu */
    item = gtk_menu_item_new_with_label(model);
    gtk_widget_show(item);
    g_object_set_data(G_OBJECT(item), "lens-id", GINT_TO_POINTER((gint)ids[i]));
    g_signal_connect(G_OBJECT(item), "activate", G_CALLBACK(lens_menu_select), self);
    gtk_menu_shell_append(GTK_MENU_SHELL(submenu), item);
  }

  g->lens_menu = GTK_MENU(gtk_menu_new());
  for(unsigned i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label((const gchar *)g_ptr_array_index(makers, i));
    gtk_widget_show(item);
    gtk_menu_shell_append(GTK_MENU_SHELL(g->lens_menu), item);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(item), (GtkWidget *)g_ptr_array_index(submenus, i));
  }

  g_ptr_array_free(submenus, TRUE);
  g_ptr_array_free(makers, TRUE);
}

/**
 * @brief The lenses to offer for the camera currently shown, as ids.
 *
 * @param model when non-NULL and non-empty, only lenses whose name matches it -- the fuzzy
 * matcher, so an abbreviated EXIF string finds the full name.
 * @param out_n how many were written. Free the result with dt_free_align().
 *
 * @details Replaces FindLenses(camera, NULL, model, LF_SEARCH_SORT_AND_UNIQUIFY). The
 * SORT half is done by lens_menu_fill(), which groups by maker and inserts sorted; the
 * UNIQUIFY half is not needed, because these are database ids and a row cannot repeat.
 */
static long long *_lens_ids_for_camera(dt_iop_module_t *self, const char *model, int *out_n)
{
  *out_n = 0;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  ls_db_t *db = _ls_db();
  if(IS_NULL_PTR(db)) return NULL;

  ls_camera_t cam;
  const gboolean have_cam = (g->camera_id >= 0) && (ls_db_camera_by_id(db, g->camera_id, &cam) == 1);

  if(model && model[0])
  {
    enum { MAX_HITS = 32 };
    ls_db_match_t m[MAX_HITS];
    const int n = ls_db_match_lens(db, NULL, model, have_cam ? cam.mount_id : 0,
                                   have_cam ? cam.crop_factor : 0.f, m, MAX_HITS);
    if(n <= 0) return NULL;
    long long *ids = (long long *)dt_alloc_align(sizeof(long long) * (size_t)n);
    if(IS_NULL_PTR(ids)) return NULL;
    for(int i = 0; i < n; i++) ids[i] = m[i].lens_id;
    *out_n = n;
    return ids;
  }

  /* Everything that fits the camera's mount, or the whole catalogue when no camera is
   * selected -- which is what upstream answered for a NULL camera too. */
  const int total = ls_db_list_lenses(db, NULL, 0);
  if(total <= 0) return NULL;
  long long *all = (long long *)dt_alloc_align(sizeof(long long) * (size_t)total);
  if(IS_NULL_PTR(all)) return NULL;
  const int got = ls_db_list_lenses(db, all, total);

  if(!have_cam)
  {
    *out_n = got;
    return all;
  }

  int keep = 0;
  for(int i = 0; i < got; i++)
    if(ls_db_lens_fits_mount(db, all[i], cam.mount_id) == 1) all[keep++] = all[i];
  *out_n = keep;
  return all;
}

static void lens_menusearch_clicked(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  (void)button;

  int n = 0;
  long long *ids = _lens_ids_for_camera(self, NULL, &n);
  if(IS_NULL_PTR(ids)) return;
  lens_menu_fill(self, ids, n);
  dt_free_align(ids);

  dt_gui_menu_popup(GTK_MENU(g->lens_menu), button, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH);
}


/* -- end lens -- */

static void target_geometry_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  int pos = dt_bauhaus_combobox_get(widget);
  p->target_geom = (dt_lens_type_t)(pos + DT_LENS_UNKNOWN + 1);
  p->modified = 1;
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

/**
 * @brief Show only the controls the current sources actually use.
 *
 * @details Geometry belongs to the database's projection model, and the manual TCA
 * coefficients to TCA-as-typed-by-the-user; neither means anything when its axis is off or
 * fed from somewhere else. Hiding rather than merely grey-listing them is what keeps the
 * module short: with three sources per axis the panel would otherwise grow a row for every
 * combination nobody selected.
 */
/* Defined further down, next to the data they work on. */
static int _lens_source_row(const dt_iop_lensfun_gui_data_t *g, const dt_lens_axis_t axis,
                            const dt_lens_source_t source);
static int _lens_corrections_available(dt_iop_module_t *self,
                                       const dt_iop_lensfun_params_t *const p);

/**
 * @brief Which axes the lens DATABASE could correct for this image, whatever is selected.
 *
 * @details _lens_corrections_available() answers what WILL run, because get_modifier() now
 * drops any axis pointed at another source -- which is what the "corrections done" label
 * wants and exactly not what greying a row wants. Asking with every axis forced to the
 * database separates the two questions: capability here, selection there.
 */
static int _lens_database_offers(dt_iop_module_t *self, const dt_iop_lensfun_params_t *p)
{
  dt_iop_lensfun_params_t probe = *p;
  for(dt_lens_axis_t axis = 0; axis < DT_LENS_AXIS_LAST; axis++)
    _lens_source_set(&probe, axis, DT_LENS_SOURCE_LENSFUN);
  return _lens_corrections_available(self, &probe);
}


/**
 * @brief The source the panel is SHOWING for this axis.
 *
 * @details The stored one when this image can supply it, and OFF when it cannot.
 * _lens_rebuild_axis_row() drops the rows an image has nothing behind and falls back to OFF
 * when the stored source is one of them -- deliberately WITHOUT rewriting the params, so
 * merely opening an image never edits its history. The dependent controls therefore have to
 * follow the widget rather than the params: reading p directly is how the scale slider, or
 * the manual coefficients, could sit under a picker reading "no correction".
 */
static dt_lens_source_t _lens_source_displayed(const dt_iop_lensfun_gui_data_t *g,
                                               const dt_iop_lensfun_params_t *p,
                                               const dt_lens_axis_t axis)
{
  const dt_lens_source_t source = _lens_source_get(p, axis);
  return (_lens_source_row(g, axis, source) >= 0) ? source : DT_LENS_SOURCE_OFF;
}

static void _lens_gui_update_sensitivity(dt_iop_module_t *self)
{
  const dt_iop_lensfun_params_t *p = _lens_effective_params(
      self, (const dt_iop_lensfun_params_t *)self->params);
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  if(IS_NULL_PTR(g)) return;

  const dt_lens_source_t dist = _lens_source_displayed(g, p, DT_LENS_AXIS_DISTORTION);
  const dt_lens_source_t tca = _lens_source_displayed(g, p, DT_LENS_AXIS_TCA);

  /* Projection and scaling are the other two thirds of the distortion pack, so they appear
   * exactly when that pack runs. Leaving the scale slider visible while it does nothing was
   * the same defect as the hidden geometry combobox that kept applying -- a control and its
   * effect disagreeing -- just the other way round. */
  /* Scaling belongs to the pack, so it shows whenever the pack runs -- from either source.
   * Projection does not: changing the lens's projection is a property of the database's
   * model, and a maker's profile describes the lens in the projection it shipped with. */
  gtk_widget_set_visible(g->scale, dist != DT_LENS_SOURCE_OFF);
  gtk_widget_set_visible(g->target_geom, dist == DT_LENS_SOURCE_LENSFUN);

  gtk_widget_set_visible(g->tca_r, tca == DT_LENS_SOURCE_MANUAL);
  gtk_widget_set_visible(g->tca_b, tca == DT_LENS_SOURCE_MANUAL);

  /* Soft deprecation of the distort/correct mode: shown only to an edit that already uses
   * it, so nobody loses a setting they made, and offered to nobody else. The module is
   * called lens correction; deliberately ADDING a lens's flaws is not something anyone has
   * been found to want, and the row cost more panel than the feature was worth. */
  gtk_widget_set_visible(g->reverse, p->inverse != 0);

  /* Which sources this image can actually offer is decided in _lens_rebuild_axis_rows(),
   * which runs from gui_update() and from every setter that changes what the database can
   * match: a source with nothing behind it gets no row at all. Greying was not enough --
   * bauhaus honours entry sensitivity when the list is scrolled but not when a row is
   * clicked, so a greyed row was still selectable and still did nothing. Nor is greying the
   * whole picker: OFF and manual TCA are available to every image, database or not. */
}

/* What each source is called in the panel. Indexed by dt_lens_source_t, so the name and
 * the value cannot drift apart the way two parallel lists would.
 *
 * The two that ARE profiles say whose measurement they are, because that is the choice the
 * user is actually making: the camera maker measured this body with this lens on a bench,
 * while the database is a community measurement of that lens model. "Embedded" and
 * "database" described where the numbers were stored, which is the one thing about them
 * nobody needs to know. The other two are not profiles and do not pretend to be. */
static const char *const _lens_source_names[DT_LENS_SOURCE_LAST] = {
  [DT_LENS_SOURCE_OFF]      = N_("no correction"),
  [DT_LENS_SOURCE_LENSFUN]  = N_("community profile"),
  [DT_LENS_SOURCE_EMBEDDED] = N_("vendor profile"),
  [DT_LENS_SOURCE_MANUAL]   = N_("manual correction"),
};

/* Which sources one axis's combobox offers, in the order they appear. OFF is always first
 * and LENSFUN always second, so a row's index means the same thing on every axis and the
 * user is not re-learning the widget three times. */
static const dt_lens_source_t _lens_axis_sources[DT_LENS_AXIS_LAST][DT_LENS_SOURCE_LAST] = {
  [DT_LENS_AXIS_TCA]        = { DT_LENS_SOURCE_OFF, DT_LENS_SOURCE_LENSFUN,
                                DT_LENS_SOURCE_EMBEDDED, DT_LENS_SOURCE_MANUAL },
  [DT_LENS_AXIS_DISTORTION] = { DT_LENS_SOURCE_OFF, DT_LENS_SOURCE_LENSFUN,
                                DT_LENS_SOURCE_EMBEDDED, DT_LENS_SOURCE_LAST },
  [DT_LENS_AXIS_VIGNETTING] = { DT_LENS_SOURCE_OFF, DT_LENS_SOURCE_LENSFUN,
                                DT_LENS_SOURCE_EMBEDDED, DT_LENS_SOURCE_LAST },
};

/**
 * @brief The row a source sits on in THIS image's combobox, or -1 if it is not offered.
 *
 * @details Against what the widget currently holds, not against the catalogue: the rows are
 * rebuilt per image, so a source the file cannot supply has no row at all and its index
 * would otherwise point at whatever moved up into its place.
 */
static int _lens_source_row(const dt_iop_lensfun_gui_data_t *g, const dt_lens_axis_t axis,
                            const dt_lens_source_t source)
{
  for(int i = 0; i < g->axis_rows[axis]; i++)
    if(g->axis_row[axis][i] == source) return i;
  return -1;
}

/**
 * @brief Rebuild each axis's combobox to hold exactly the sources THIS image can supply.
 *
 * @details Displayed means available. A source with nothing behind it gets no row, rather
 * than a greyed one: bauhaus honours entry sensitivity when the list is scrolled but not
 * when a row is clicked, so a greyed row stayed selectable and then silently did nothing --
 * which is the same class of bug as the fallback that used to be silent.
 *
 * Rebuilt per image because availability is a property of the image: whether the database
 * matched a lens, and whether the file carries a maker's profile for that axis. It does NOT
 * depend on the current params, so this runs from gui_update() and not from the change
 * callback, which would otherwise rebuild the widget it is being called by.
 *
 * OFF is always offered -- refusing to correct is always possible -- and it is always first,
 * so the list has a fixed anchor whatever else appears.
 */
static void _lens_rebuild_axis_row(dt_iop_module_t *self, const dt_lens_axis_t axis)
{
  const dt_iop_lensfun_params_t *p = _lens_effective_params(
      self, (const dt_iop_lensfun_params_t *)self->params);
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  if(IS_NULL_PTR(g)) return;

  const int offers = _lens_database_offers(self, p);

  /* A monochrome sensor has no colour channels to shift against each other, so lateral CA
   * is not a correction that exists for this image at all -- _lens_mask_for_mono() already
   * withdraws it from every pipe. Offering it here would be a row that renders nothing,
   * which is the one thing the per-image row list exists to prevent. */
  const gboolean mono = !IS_NULL_PTR(self->dev)
                        && dt_image_is_monochrome(&self->dev->image_storage);

  {
    GtkWidget *w = g->axis_source[axis];
    int n = 0;

    /* Filling a combobox is a programmatic update, and _lens_axis_source_changed() must not
     * mistake it for the user picking a row: it commits to history. Two callers are already
     * outside gui_update()'s own freeze -- the distortion handler rebuilding the TCA row,
     * and lens_set() rebuilding everything when the lens changes -- so the freeze belongs
     * here, where the widget is actually written, rather than at each call site. */
    dt_gui_widget_freeze();

    dt_bauhaus_combobox_clear(w);
    for(int i = 0; i < DT_LENS_SOURCE_LAST; i++)
    {
      const dt_lens_source_t src = _lens_axis_sources[axis][i];
      if(src == DT_LENS_SOURCE_LAST) break;

      gboolean have;
      switch(src)
      {
        case DT_LENS_SOURCE_OFF:      have = TRUE; break;
        case DT_LENS_SOURCE_LENSFUN:  have = _lens_flags_have_axis(offers, axis); break;
        case DT_LENS_SOURCE_EMBEDDED:
          have = _lens_image_embeds(self, axis);
          /* A maker's aberration is measured against their OWN geometry, so it is offered
           * only when the geometry is theirs too. Stacking it on a database distortion
           * would be two calibrations of one lens applied to each other -- and the
           * evaluator cannot express it either: the table's CA lives in the same curve as
           * its distortion. The other direction is fine and supported: the table's geometry
           * happily wears a database or hand-typed aberration. */
          if(axis == DT_LENS_AXIS_TCA
             && !_lens_source_is(p, DT_LENS_AXIS_DISTORTION, DT_LENS_SOURCE_EMBEDDED))
            have = FALSE;
          break;
        case DT_LENS_SOURCE_MANUAL:   have = (axis == DT_LENS_AXIS_TCA); break;
        default:                      have = FALSE; break;
      }
      if(mono && axis == DT_LENS_AXIS_TCA && src != DT_LENS_SOURCE_OFF) have = FALSE;
      if(!have) continue;

      dt_bauhaus_combobox_add(w, _(_lens_source_names[src]));
      g->axis_row[axis][n++] = src;
    }
    g->axis_rows[axis] = n;

    /* Show what the params say, or fall back to OFF -- which is row 0 and always there.
     * A stored source this image cannot supply has no row to select, and the correction
     * would not have run either; commit_params has already said so. */
    int row = _lens_source_row(g, axis, _lens_source_get(p, axis));
    if(row < 0) row = 0;
    dt_bauhaus_combobox_set(w, row);
  }
}

/** @brief Every axis, for a fresh image. */
static void _lens_rebuild_axis_rows(dt_iop_module_t *self)
{
  for(dt_lens_axis_t axis = 0; axis < DT_LENS_AXIS_LAST; axis++)
    _lens_rebuild_axis_row(self, axis);
}


/** @brief Which axis a combobox belongs to, by widget identity. */
static dt_lens_axis_t _lens_axis_of_widget(const dt_iop_lensfun_gui_data_t *g,
                                           const GtkWidget *w)
{
  for(dt_lens_axis_t axis = 0; axis < DT_LENS_AXIS_LAST; axis++)
    if(g->axis_source[axis] == w) return axis;
  return DT_LENS_AXIS_LAST;
}

static void _lens_axis_source_changed(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(dt_gui_widgets_suppressed()) return;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);

  const dt_lens_axis_t axis = _lens_axis_of_widget(g, widget);
  if(axis == DT_LENS_AXIS_LAST) return;

  /* Against what the widget currently holds, not the catalogue: rows the image cannot
   * supply are absent, so a catalogue index would name the wrong source. */
  const int row = dt_bauhaus_combobox_get(widget);
  if(row < 0 || row >= g->axis_rows[axis]) return;

  const dt_lens_source_t source = g->axis_row[axis][row];

  _lens_source_set(p, axis, source);
  p->modified = 1;

  /* Distortion decides whether the maker's aberration may be offered, so its rows move when
   * distortion does. Rebuilding a DIFFERENT axis's widget from this handler is safe;
   * rebuilding the one being handled would not be. */
  if(axis == DT_LENS_AXIS_DISTORTION) _lens_rebuild_axis_row(self, DT_LENS_AXIS_TCA);

  _lens_gui_update_sensitivity(self);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

/**
 * @brief Build one axis's source combobox and pack it at the end of the module.
 *
 * @details Called in panel order, so the caller's sequence IS the layout: each axis's
 * dependent controls are created straight after its combobox and land under it.
 */
static GtkWidget *_lens_add_axis_combobox(dt_iop_module_t *self, dt_iop_lensfun_gui_data_t *g,
                                          const dt_lens_axis_t axis, const char *label,
                                          const char *tooltip)
{
  GtkWidget *w = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  g->axis_source[axis] = w;
  dt_bauhaus_widget_set_label(w, label);
  gtk_widget_set_tooltip_text(w, _(tooltip));
  gtk_box_pack_start(GTK_BOX(self->gui->widget), w, TRUE, TRUE, 0);

  /* Left empty: the rows depend on the image, and gui_update() fills them. */
  g->axis_rows[axis] = 0;
  g_signal_connect(G_OBJECT(w), "value-changed",
                   G_CALLBACK(_lens_axis_source_changed), (gpointer)self);
  return w;
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  /* Which controls are visible is decided in ONE place, _lens_gui_update_sensitivity(), out
   * of the sources the pickers are actually showing. It used to be decided here as well,
   * out of p->tca_override and the monochrome flag, and the two could disagree: this runs
   * LAST out of gui_update() and so had the final word, putting the manual coefficient
   * sliders back on screen for an image whose TCA picker was not offering manual at all. */
  if(w)
  {
    // user did modify something with some widget
    p->modified = 1;
  }
}


static float get_autoscale(dt_iop_module_t *self, dt_iop_lensfun_params_t *p)
{
  float scale = 1.0f;

  /* Built by THE constructor, not by hand. The hand-rolled version resolved only the
   * database lens, so with distortion taking the maker's embedded profile this measured a
   * correction the pipe was not applying -- and the result was then multiplied ON TOP of
   * the profile's own autoscale by get_modifier(), giving a doubled zoom. It also returned
   * 1.0 outright when no database lens matched, which is precisely the body an embedded
   * profile exists to serve. */
  dt_iop_lensfun_data_t d;
  memset(&d, 0, sizeof(d));
  _lens_build_data(self, p, &d);
  if(!_lens_data_available(&d)) return scale;

  /* Measure the correction itself, not a scaling already applied to it. For the embedded
   * resolver this leaves the profile's own autoscale in place and asks what is still needed
   * on top of it -- which is what get_modifier() then composes. */
  d.scale = 1.0f;

  const dt_image_t *img = &(self->dev->image_storage);
  // FIXME: get those from rawprepare IOP somehow !!!
  const int iwd = img->width - img->crop_x - img->crop_width,
            iht = img->height - img->crop_y - img->crop_height;

  ls_modifier_t modifier;
  if(get_modifier(NULL, iwd, iht, &d, DT_LENS_MODIFY_ALL_AXES, FALSE, &modifier, NULL))
    scale = ls_modifier_autoscale(&modifier);
  return scale;
}

static void autoscale_pressed(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const float scale = get_autoscale(self, p);
  p->modified = 1;
  dt_bauhaus_slider_set(g->scale, scale);
  dt_dev_add_history_item(self->dev, self, TRUE, TRUE);
}

/**
 * @brief Which corrections this configuration will actually apply.
 *
 * @return the DT_LENS_MODIFY_* axes that resolve, masked to what the GUI reports.
 *
 * @details GUI THREAD ONLY, and it needs no pipeline: which corrections a lens can serve is
 * a pure function of the lens, the shooting configuration and the user's own switches. The
 * database answers it in ~0.2 ms.
 *
 * This used to be discovered by RENDERING. process() and process_cl() wrote the resolved
 * flags into gui_data from the pipeline thread under a critical section, and a
 * preview-pipe-finished signal then woke the GUI to read them back. That is a data race
 * whatever the lock does -- gui_data belongs to the GUI thread, which may free it while a
 * worker is mid-write -- and it made a label depend on a frame having been drawn. Neither
 * was necessary: nothing here needs a pixel.
 */
static int _lens_corrections_available(dt_iop_module_t *self,
                                       const dt_iop_lensfun_params_t *const p)
{
  if(IS_NULL_PTR(self->dev)) return 0;

  dt_iop_lensfun_data_t d;
  memset(&d, 0, sizeof(d));
  _lens_build_data(self, p, &d);
  if(!_lens_data_available(&d)) return 0;

  /* The frame the correction is expressed over. Only its aspect matters to which axes
   * resolve, so the full image is a fine stand-in when the pipe has not published one. */
  dt_iop_roi_t roi = { 0, 0, 0, 0, 1.f };
  if(!dt_dev_module_geometry_gui(self->dev, self, &roi, NULL) || roi.width <= 0
     || roi.height <= 0)
  {
    const dt_image_t *img = &self->dev->image_storage;
    roi.width = img->width;
    roi.height = img->height;
  }
  if(roi.width <= 0 || roi.height <= 0) return 0;

  const int mask = _lens_mask_for_mono(dt_image_is_monochrome(&self->dev->image_storage));
  int modflags = 0;
  ls_modifier_t m;
  get_modifier(&modflags, roi.width, roi.height, &d, mask, FALSE, &m, NULL);
  return modflags & LENSFUN_MODFLAG_MASK;
}


void gui_init(struct dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = IOP_GUI_ALLOC(lensfun);

  g->camera_id = -1;
  g->camera_menu = NULL;
  g->lens_menu = NULL;


  self->gui->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_GUI_BOX_SPACING);
    gtk_widget_set_name(self->gui->widget, "lens-module");

  /* One button per row, not two. Each row used to carry a second, unlabelled arrow that
   * listed the Exif match instead of the whole catalogue -- for the camera, a menu of
   * exactly one entry, the one reload_defaults() had already applied.
   *
   * Filtering these lists by Exif is useless by construction: they are opened precisely
   * BECAUSE Exif matching failed. A lens with no CPU reports no lens at all, so there is
   * nothing to filter on; a body absent from the database is chosen by picking a near
   * relative deliberately -- a Mk I for a Mk II -- which an Exif filter would never
   * suggest. Both cases want the full list, which is what the model button has always
   * shown. */

  // camera selector
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  g->camera_model = dt_iop_button_new(self, N_("camera model"),
                                      G_CALLBACK(camera_menusearch_clicked), FALSE, 0, (GdkModifierType)0,
                                      NULL, 0, hbox);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), hbox, TRUE, TRUE, 0);

  // lens selector
  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  g->lens_model = dt_iop_button_new(self, N_("lens model"),
                                    G_CALLBACK(lens_menusearch_clicked), FALSE, 0, (GdkModifierType)0,
                                    NULL, 0, hbox);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), hbox, TRUE, TRUE, 0);

  // lens properties
  g->lens_param_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, DT_GUI_BOX_SPACING);
  gtk_box_pack_start(GTK_BOX(self->gui->widget), g->lens_param_box, TRUE, TRUE, 0);


  /* The panel reads top to bottom as: what was detected, how much to rescale, then one
   * section per correction axis. Each section is its source combobox followed by exactly
   * the controls that source uses -- geometry under distortion, the manual coefficients
   * under TCA -- so a control is never far from the thing that decides whether it applies.
   *
   * That is why these are written out one at a time rather than looped: the loop would have
   * to put every axis's extra controls somewhere else, and "somewhere else" is how the old
   * panel ended up with a TCA override checkbox three rows away from the TCA setting. */

  // 2. vignetting
  _lens_add_axis_combobox(self, g, DT_LENS_AXIS_VIGNETTING, N_("vignetting"),
                          N_("correct the lens's light falloff, and where to take it from"));

  // 3. distortion, with the projection and the scaling that go with it
  _lens_add_axis_combobox(self, g, DT_LENS_AXIS_DISTORTION, N_("distortion"),
                          N_("correct the lens's geometric distortion, and where to take it from"));

  g->target_geom = dt_bauhaus_combobox_new(dt_bauhaus_get_global(), DT_GUI_MODULE(self));
  dt_bauhaus_widget_set_label(g->target_geom, N_("geometry"));
  gtk_box_pack_start(GTK_BOX(self->gui->widget), g->target_geom, TRUE, TRUE, 0);
  gtk_widget_set_tooltip_text(g->target_geom, _("target geometry"));
  dt_bauhaus_combobox_add(g->target_geom, _("rectilinear"));
  dt_bauhaus_combobox_add(g->target_geom, _("fish-eye"));
  dt_bauhaus_combobox_add(g->target_geom, _("panoramic"));
  dt_bauhaus_combobox_add(g->target_geom, _("equirectangular"));
  dt_bauhaus_combobox_add(g->target_geom, _("orthographic"));
  dt_bauhaus_combobox_add(g->target_geom, _("stereographic"));
  dt_bauhaus_combobox_add(g->target_geom, _("equisolid angle"));
  dt_bauhaus_combobox_add(g->target_geom, _("thoby fish-eye"));
  g_signal_connect(G_OBJECT(g->target_geom), "value-changed", G_CALLBACK(target_geometry_changed),
                   (gpointer)self);

  /* Scaling closes the distortion section rather than opening the module. It is the third
   * of the pack -- distortion, projection, scale -- and it only ever runs when that pack
   * runs, so putting it at the top left the user reading a control whose effect lived four
   * rows further down, under a different heading. */
  g->scale = dt_bauhaus_slider_from_params(self, N_("scale"));
  dt_bauhaus_slider_set_digits(g->scale, 3);
  dt_bauhaus_widget_set_quad_paint(g->scale, dtgtk_cairo_paint_refresh, 0, NULL);
  g_signal_connect(G_OBJECT(g->scale), "quad-pressed", G_CALLBACK(autoscale_pressed), self);
  gtk_widget_set_tooltip_text(g->scale, _("auto scale"));

  // 4. chromatic aberrations, and the coefficients the manual source uses
  _lens_add_axis_combobox(self, g, DT_LENS_AXIS_TCA, N_("chromatic aberrations"),
                          N_("correct lateral chromatic aberration, and where to take it from"));

  /* p->tca_override has NO widget. It survives only as storage -- _lens_source_set()
   * writes it whenever TCA is set to manual, so an edit saved by this version is still read
   * correctly by one that predates the per-axis sources. The checkbox it used to drive is
   * gone: "TCA = manual correction" is a row of the combobox above, and one state behind two
   * controls is one state they can disagree about. */

  g->tca_r = dt_bauhaus_slider_from_params(self, "tca_r");
  dt_bauhaus_slider_set_digits(g->tca_r, 5);
  gtk_widget_set_tooltip_text(g->tca_r, _("Transversal Chromatic Aberration red"));

  g->tca_b = dt_bauhaus_slider_from_params(self, "tca_b");
  dt_bauhaus_slider_set_digits(g->tca_b, 5);
  gtk_widget_set_tooltip_text(g->tca_b, _("Transversal Chromatic Aberration blue"));

  /* Last, because it is on its way out: it inverts every axis at once, nobody has been
   * found who uses it, and it is kept only so existing edits that set it keep rendering. */
  g->reverse = dt_bauhaus_combobox_from_params(self, "inverse");
  dt_bauhaus_combobox_add(g->reverse, _("correct"));
  dt_bauhaus_combobox_add(g->reverse, _("distort"));
  gtk_widget_set_tooltip_text(g->reverse, _("correct distortions or apply them"));


}

void gui_update(struct dt_iop_module_t *self)
{
  // let gui elements reflect params
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)dt_iop_gui_data(self);

  /* What is actually IN FORCE, which for an edit saved with modified == 0 is default_params
   * rather than params -- see _lens_effective_params(). The view shows that; it does not
   * write it back. This function used to memcpy default_params over self->params right
   * here, which mutated an edit from a panel refresh, and did not even reach the widgets it
   * was meant to fix: dt_iop_gui_update() runs dt_bauhaus_update_module() out of
   * self->params BEFORE calling this, so the copy only ever showed up one refresh late. */
  const dt_iop_lensfun_params_t *const p = _lens_effective_params(
      self, (const dt_iop_lensfun_params_t *)self->params);

  // these are the wrong (untranslated) strings in general but that's ok, they will be overwritten further
  // down
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), p->camera);
  gtk_label_set_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), p->lens);
  gtk_widget_set_tooltip_text(g->camera_model, "");
  gtk_widget_set_tooltip_text(g->lens_model, "");

  _lens_rebuild_axis_rows(self);
  _lens_gui_update_sensitivity(self);

  /* dt_bauhaus_update_module() has already synced these from self->params. Re-set them from
   * the effective params so a modified == 0 edit shows what it renders rather than what it
   * stores; for every other edit the two are the same object and this is a no-op. */
  dt_bauhaus_combobox_set(g->target_geom, p->target_geom - DT_LENS_UNKNOWN - 1);
  dt_bauhaus_combobox_set(g->reverse, p->inverse);
  dt_bauhaus_slider_set(g->scale, p->scale);
  dt_bauhaus_slider_set(g->tca_r, p->tca_r);
  dt_bauhaus_slider_set(g->tca_b, p->tca_b);

  g->camera_id = -1;
  if(p->camera[0])
  {
    /* Resolved the same way the pipeline resolves it. Recovering the id by comparing the
     * params string against stored model names cannot work: matching is on the normalised
     * form, so "NIKON D5300" finds a row whose model column reads "D5300" -- which is why
     * this label was blank on every image while the correction itself was applied. */
    ls_camera_t cam;
    camera_set(self, _ls_find_camera(NULL, p->camera, &cam) ? cam.id : -1);
  }
  if(g->camera_id >= 0 && p->lens[0])
  {
    char model[200];
    parse_model(p->lens, model, sizeof(model));
    int n = 0;
    long long *ids = _lens_ids_for_camera(self, model[0] ? model : NULL, &n);
    lens_set(self, (n > 0 && !IS_NULL_PTR(ids)) ? ids[0] : -1);
    if(!IS_NULL_PTR(ids)) dt_free_align(ids);
  }
  else
  {
    lens_set(self, -1);
  }



  gui_changed(self, NULL, NULL);
}

void gui_cleanup(struct dt_iop_module_t *self)
{

  IOP_GUI_FREE;
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
