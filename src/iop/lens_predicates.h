#ifndef DT_IOP_LENS_PREDICATES_H
#define DT_IOP_LENS_PREDICATES_H

#include <glib.h>
#include <lensfun.h>

#if !defined(N_)
#define N_(String) (String)
#endif

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

static inline int correction_source_selector_entries(gboolean has_embedded,
                                                     const char *out_labels[3],
                                                     int out_values[3])
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

static inline int tca_selector_entries(gboolean has_embedded,
                                        const char *out_labels[4],
                                        int out_values[4])
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
  else
  {
    out_labels[1] = N_("Lensfun");
    out_values[1] = static_cast<int>(dt_iop_lens_tca_source_t::LENSFUN_DB);
    out_labels[2] = N_("manual");
    out_values[2] = static_cast<int>(dt_iop_lens_tca_source_t::MANUAL);
    return 3;
  }
}

static inline gboolean tca_show_manual_sliders(dt_iop_lens_tca_source_t tca_method)
{
  return tca_method == dt_iop_lens_tca_source_t::MANUAL;
}

static inline int per_axis_modify_flags(dt_iop_lens_correction_source_t dist,
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

static inline const char *corrections_status_string(
    dt_iop_lens_correction_source_t dist_method,
    dt_iop_lens_correction_source_t vig_method,
    dt_iop_lens_tca_source_t tca_method,
    gboolean monochrome)
{
  static char buf[128];

  const char *dist_label;
  switch(dist_method)
  {
    case dt_iop_lens_correction_source_t::OFF:        dist_label = "off"; break;
    case dt_iop_lens_correction_source_t::EMBEDDED:   dist_label = "embedded"; break;
    case dt_iop_lens_correction_source_t::LENSFUN_DB: dist_label = "Lensfun"; break;
    default:                                           dist_label = "off";
  }

  const char *vig_label;
  switch(vig_method)
  {
    case dt_iop_lens_correction_source_t::OFF:        vig_label = "off"; break;
    case dt_iop_lens_correction_source_t::EMBEDDED:   vig_label = "embedded"; break;
    case dt_iop_lens_correction_source_t::LENSFUN_DB: vig_label = "Lensfun"; break;
    default:                                           vig_label = "off";
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
    default:                                    tca_label = "off";
  }

  g_snprintf(buf, sizeof(buf), "distortion: %s, vignetting: %s, TCA: %s",
             dist_label, vig_label, tca_label);
  return buf;
}

#endif // DT_IOP_LENS_PREDICATES_H
