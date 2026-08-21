#ifndef DT_IOP_LENS_PREDICATES_H
#define DT_IOP_LENS_PREDICATES_H

#include <glib.h>
#include <lensfun.h>

/* Where a lens correction comes from, per axis. The GUI offers one combobox per axis
 * (distortion, vignetting, TCA) and these are the values it stores in the params. */

enum class dt_iop_lens_correction_source_t
{
  OFF = 0,
  EMBEDDED = 1,
  LENSFUN_DB = 2
};

enum class dt_iop_lens_tca_source_t
{
  OFF = 0,
  MANUAL = 1,
  LENSFUN_DB = 2,
  EMBEDDED = 3
};

#define LENSFUN_MODFLAG_MASK (LF_MODIFY_DISTORTION | LF_MODIFY_TCA | LF_MODIFY_VIGNETTING | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE)

/** How many entries the correction-source and TCA comboboxes can ever hold. Size the
 * caller's arrays with these, not with a literal: tca_selector_entries() writes a fourth
 * entry as soon as the image carries embedded calibration. */
#define DT_IOP_LENS_CORRECTION_SOURCE_MAX_ENTRIES 3
#define DT_IOP_LENS_TCA_SOURCE_MAX_ENTRIES 4

/** @brief Fill the distortion/vignetting combobox with the sources this image offers.
 *
 * @param has_embedded whether the image carries embedded calibration for this axis.
 * @param out_labels untranslated labels, to be passed through _() by the caller.
 * @param out_values the matching dt_iop_lens_correction_source_t, as int.
 * @return how many entries were written, or 0 if either output pointer is NULL.
 */
int correction_source_selector_entries(gboolean has_embedded,
                                       const char *out_labels[DT_IOP_LENS_CORRECTION_SOURCE_MAX_ENTRIES],
                                       int out_values[DT_IOP_LENS_CORRECTION_SOURCE_MAX_ENTRIES]);

/** @brief Fill the TCA combobox. Manual entry is always offered; embedded only when the
 * image carries it, which is why this writes up to four entries and its correction-source
 * counterpart writes up to three. */
int tca_selector_entries(gboolean has_embedded,
                         const char *out_labels[DT_IOP_LENS_TCA_SOURCE_MAX_ENTRIES],
                         int out_values[DT_IOP_LENS_TCA_SOURCE_MAX_ENTRIES]);

/** @brief Whether the manual tca_r/tca_b sliders apply to the current selection. */
gboolean tca_show_manual_sliders(dt_iop_lens_tca_source_t tca_method);

/** @brief The lensfun modify flags implied by the per-axis selection.
 *
 * @details Only axes set to LENSFUN_DB contribute: an axis corrected from embedded
 * metadata is handled outside lensfun and must not raise its bit. A monochrome sensor
 * never gets TCA correction.
 */
int per_axis_modify_flags(dt_iop_lens_correction_source_t dist,
                          dt_iop_lens_correction_source_t vig,
                          dt_iop_lens_tca_source_t tca,
                          gboolean monochrome);

/** @brief Format the per-axis correction status into @p buf, and return @p buf.
 *
 * @details The buffer belongs to the caller: this is called from the GUI thread and from
 * debug output on pipeline threads, so there is no shared scratch to return a pointer to.
 * Returns NULL if @p buf is NULL or @p buflen is 0.
 */
const char *corrections_status_string(dt_iop_lens_correction_source_t dist_method,
                                      dt_iop_lens_correction_source_t vig_method,
                                      dt_iop_lens_tca_source_t tca_method,
                                      gboolean monochrome,
                                      char *buf, size_t buflen);

#endif // DT_IOP_LENS_PREDICATES_H
