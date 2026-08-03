#include "lens_predicates.h"

#ifdef BUILD_TESTING
extern "C" {

int test_distortion_selector_entries(gboolean has_embedded,
                                     const char *out_labels[3])
{
  return distortion_selector_entries(has_embedded, out_labels);
}

int test_vignetting_selector_entries(gboolean has_embedded,
                                      const char *out_labels[3])
{
  return vignetting_selector_entries(has_embedded, out_labels);
}

int test_tca_selector_entries(gboolean has_embedded,
                               const char *out_labels[3])
{
  return tca_selector_entries(has_embedded, out_labels);
}

gboolean test_tca_show_manual_sliders(dt_iop_lens_tca_source_t tca_method)
{
  return tca_show_manual_sliders(tca_method);
}

gboolean test_tca_show_override_sliders(dt_iop_lens_tca_source_t tca_method)
{
  return tca_show_override_sliders(tca_method);
}

int test_per_axis_modify_flags(dt_iop_lens_correction_source_t dist,
                                dt_iop_lens_correction_source_t vig,
                                dt_iop_lens_tca_source_t tca,
                                gboolean monochrome)
{
  return per_axis_modify_flags(dist, vig, tca, monochrome);
}

const char *test_corrections_status_string(
    dt_iop_lens_correction_source_t dist,
    dt_iop_lens_correction_source_t vig,
    dt_iop_lens_tca_source_t tca,
    gboolean monochrome)
{
  return corrections_status_string(dist, vig, tca, monochrome);
}

}
#endif
