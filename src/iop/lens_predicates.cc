#include "lens_predicates.h"

#if !defined(N_)
#define N_(String) (String)
#endif

int correction_source_selector_entries(gboolean has_embedded,
                                       const char *out_labels[DT_IOP_LENS_CORRECTION_SOURCE_MAX_ENTRIES],
                                       int out_values[DT_IOP_LENS_CORRECTION_SOURCE_MAX_ENTRIES])
{
  if(!out_labels || !out_values) return 0;

  out_labels[0] = N_("off");
  out_values[0] = static_cast<int>(dt_iop_lens_correction_source_t::OFF);

  if(has_embedded)
  {
    out_labels[1] = N_("embedded");
    out_values[1] = static_cast<int>(dt_iop_lens_correction_source_t::EMBEDDED);
    out_labels[2] = N_("Lensfun");
    out_values[2] = static_cast<int>(dt_iop_lens_correction_source_t::LENSFUN_DB);
    return 3;
  }

  out_labels[1] = N_("Lensfun");
  out_values[1] = static_cast<int>(dt_iop_lens_correction_source_t::LENSFUN_DB);
  return 2;
}

int tca_selector_entries(gboolean has_embedded,
                         const char *out_labels[DT_IOP_LENS_TCA_SOURCE_MAX_ENTRIES],
                         int out_values[DT_IOP_LENS_TCA_SOURCE_MAX_ENTRIES])
{
  if(!out_labels || !out_values) return 0;

  out_labels[0] = N_("off");
  out_values[0] = static_cast<int>(dt_iop_lens_tca_source_t::OFF);

  if(has_embedded)
  {
    out_labels[1] = N_("embedded");
    out_values[1] = static_cast<int>(dt_iop_lens_tca_source_t::EMBEDDED);
    out_labels[2] = N_("Lensfun");
    out_values[2] = static_cast<int>(dt_iop_lens_tca_source_t::LENSFUN_DB);
    out_labels[3] = N_("manual");
    out_values[3] = static_cast<int>(dt_iop_lens_tca_source_t::MANUAL);
    return 4;
  }

  out_labels[1] = N_("Lensfun");
  out_values[1] = static_cast<int>(dt_iop_lens_tca_source_t::LENSFUN_DB);
  out_labels[2] = N_("manual");
  out_values[2] = static_cast<int>(dt_iop_lens_tca_source_t::MANUAL);
  return 3;
}

gboolean tca_show_manual_sliders(dt_iop_lens_tca_source_t tca_method)
{
  return tca_method == dt_iop_lens_tca_source_t::MANUAL;
}

int per_axis_modify_flags(dt_iop_lens_correction_source_t dist,
                          dt_iop_lens_correction_source_t vig,
                          dt_iop_lens_tca_source_t tca,
                          gboolean monochrome)
{
  int flags = 0;

  if(dist == dt_iop_lens_correction_source_t::LENSFUN_DB)
    flags |= LF_MODIFY_DISTORTION;
  if(vig == dt_iop_lens_correction_source_t::LENSFUN_DB)
    flags |= LF_MODIFY_VIGNETTING;
  if(tca == dt_iop_lens_tca_source_t::LENSFUN_DB && !monochrome)
    flags |= LF_MODIFY_TCA;

  return flags & LENSFUN_MODFLAG_MASK;
}

const char *corrections_status_string(dt_iop_lens_correction_source_t dist_method,
                                      dt_iop_lens_correction_source_t vig_method,
                                      dt_iop_lens_tca_source_t tca_method,
                                      gboolean monochrome,
                                      char *buf, size_t buflen)
{
  if(!buf || buflen == 0) return NULL;

  const char *dist_label;
  switch(dist_method)
  {
    case dt_iop_lens_correction_source_t::OFF:        dist_label = "off"; break;
    case dt_iop_lens_correction_source_t::EMBEDDED:   dist_label = "embedded"; break;
    case dt_iop_lens_correction_source_t::LENSFUN_DB: dist_label = "Lensfun"; break;
    default:                                          dist_label = "off";
  }

  const char *vig_label;
  switch(vig_method)
  {
    case dt_iop_lens_correction_source_t::OFF:        vig_label = "off"; break;
    case dt_iop_lens_correction_source_t::EMBEDDED:   vig_label = "embedded"; break;
    case dt_iop_lens_correction_source_t::LENSFUN_DB: vig_label = "Lensfun"; break;
    default:                                          vig_label = "off";
  }

  const char *tca_label;
  if(monochrome)
  {
    tca_label = "off";
  }
  else switch(tca_method)
  {
    case dt_iop_lens_tca_source_t::OFF:        tca_label = "off"; break;
    case dt_iop_lens_tca_source_t::MANUAL:     tca_label = "manual"; break;
    case dt_iop_lens_tca_source_t::LENSFUN_DB: tca_label = "Lensfun"; break;
    case dt_iop_lens_tca_source_t::EMBEDDED:   tca_label = "embedded"; break;
    default:                                   tca_label = "off";
  }

  g_snprintf(buf, buflen, "distortion: %s, vignetting: %s, TCA: %s",
             dist_label, vig_label, tca_label);
  return buf;
}

#ifdef BUILD_TESTING
/* The C test cannot spell the C++ enum classes above, so it calls these instead. The array
 * extents mirror the real ones: a shim that understates them is how the caller-side buffer
 * overflow in test_lens_predicates.c came about. */
extern "C" {

int test_correction_source_selector_entries(gboolean has_embedded,
                                            const char *out_labels[DT_IOP_LENS_CORRECTION_SOURCE_MAX_ENTRIES],
                                            int out_values[DT_IOP_LENS_CORRECTION_SOURCE_MAX_ENTRIES])
{
  return correction_source_selector_entries(has_embedded, out_labels, out_values);
}

int test_tca_selector_entries(gboolean has_embedded,
                              const char *out_labels[DT_IOP_LENS_TCA_SOURCE_MAX_ENTRIES],
                              int out_values[DT_IOP_LENS_TCA_SOURCE_MAX_ENTRIES])
{
  return tca_selector_entries(has_embedded, out_labels, out_values);
}

gboolean test_tca_show_manual_sliders(dt_iop_lens_tca_source_t tca_method)
{
  return tca_show_manual_sliders(tca_method);
}

int test_per_axis_modify_flags(dt_iop_lens_correction_source_t dist,
                               dt_iop_lens_correction_source_t vig,
                               dt_iop_lens_tca_source_t tca,
                               gboolean monochrome)
{
  return per_axis_modify_flags(dist, vig, tca, monochrome);
}

const char *test_corrections_status_string(dt_iop_lens_correction_source_t dist,
                                           dt_iop_lens_correction_source_t vig,
                                           dt_iop_lens_tca_source_t tca,
                                           gboolean monochrome,
                                           char *buf, size_t buflen)
{
  return corrections_status_string(dist, vig, tca, monochrome, buf, buflen);
}

}
#endif
