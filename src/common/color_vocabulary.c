/*
    This file is part of darktable,
    Copyright (C) 2022 Chris Elston.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2022 Pascal Obry.
    Copyright (C) 2022 Victor Forsiuk.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2024 Aurélien PIERRE.
    
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

#include "common/color_vocabulary.h"
#include "common/utility.h"
#include "system/macros.h"

#include <glib/gi18n.h>

// get a range of 2 x factor x std centered in avg
static range_t
_compute_range(const gaussian_stats_t stats, const float factor)
{
  range_t out;
  out.bottom = stats.avg - factor * stats.std;
  out.top = stats.avg + factor * stats.std;
  return out;
}

// Human skin tones database
// This is a racially-charged matter, tread with it carefully.

// Usable data are : tabulated avg ± std (P < 0.05) models on skin color measurements
// on more than 80 individuals under D65 illuminant.

// Notice all these data are valid only under D65 illuminant and errors up to delta E = 6 have
// been measured for A illuminant. Proper camera profiling and chromatic adaptation needs to be performed
// or all the following is meaningless.

// We use ranges of avg ± 2 std, giving 95 % of confidence in the prediction.

// We use CIE Lab instead of Lch coordinates, because a and b parameters are physiologically meaningful :
//  - a (redness) is linked to blood flow and health,
//  - b (yellowness) is linked to melanine and sun tan.

/* Reference :
    XIAO, Kaida, YATES, Julian M., ZARDAWI, Faraedon, et al.
    Characterising the variations in ethnic skin colours: a new calibrated data base for human skin.
    Skin Research and Technology, 2017, vol. 23, no 1, p. 21-29.
    https://onlinelibrary.wiley.com/doi/pdf/10.1111/srt.12295

    Sample : 187 caucasian, 202 chinese, 145 kurdish and 426 thai.

    DE RIGAL, Jean, DES MAZIS, Isabelle, DIRIDOLLOU, Stephane, et al.
    The effect of age on skin color and color heterogeneity in four ethnic groups.
    Skin Research and Technology, 2010, vol. 16, no 2, p. 168-178.
    https://pubmed.ncbi.nlm.nih.gov/20456097/

    Sample : 121 african-american, 64 mexican.
    Note : the data have been read on the graph and are inaccurate and std is majorated.
    The original authors have been contacted to get the tabulated, accurate data,
    but the main author is retired, co-authors have changed jobs, and the L'Oréal head of R&D
    did not respond. So the values here are given for what it's worth.
*/

// "Forearm" is the ventral forearm skin. It is the least sun-tanned part of skin.
// Sun tan will depend the most on lifestyle, therefore the ventral forearm
// is the least socially-biased skin color metric.

// "Forehead" is the most sun-tanned part of skin. This translates to high b coordinate.

// "Cheek" is the most redish part of skin. This translates to high a coordinate.

// L decreases with age in all ethnicities and with b/yellowness/melanine/tan.


char * _get_ethnicity_name(const ethnicities_t index)
{
  const ethnicity_t ethnies[ETHNIE_END] =
  { { .name = _("Chinese"),          .ethnicity = ETHNIE_CHINESE },
    { .name = _("Thai"),             .ethnicity = ETHNIE_THAI },
    { .name = _("Kurdish"),          .ethnicity = ETHNIE_KURDISH },
    { .name = _("Caucasian"),        .ethnicity = ETHNIE_CAUCASIAN },
    { .name = _("African-american"), .ethnicity = ETHNIE_AFRICAN_AM },
    { .name = _("Mexican"),          .ethnicity = ETHNIE_MEXICAN } };

  return ethnies[index].name;
}


skin_color_t get_skin_color(const size_t index)
{
  const skin_color_t skin[SKINS] = {
  { .name = _("forearm"),
    .ethnicity = ETHNIE_CHINESE,
    .L = { .avg = 60.9f, .std = 3.4f },
    .a = { .avg = 7.0f, .std = 1.7f },
    .b = { .avg = 15.0f, .std = 1.8f } },
  { .name = _("forearm"),
    .ethnicity = ETHNIE_THAI,
    .L = { .avg = 61.9f, .std = 3.7f },
    .a = { .avg = 7.1f, .std = 1.7f },
    .b = { .avg = 17.4f, .std = 2.0f } },
  { .name = _("forearm"),
    .ethnicity = ETHNIE_KURDISH,
    .L = { .avg = 60.6f, .std = 4.8f },
    .a = { .avg = 6.5f, .std = 1.6f },
    .b = { .avg = 16.4f, .std = 2.3f } },
  { .name = _("forearm"),
    .ethnicity = ETHNIE_CAUCASIAN,
    .L = { .avg = 63.0f, .std = 5.5f },
    .a = { .avg = 5.6f, .std = 1.9f },
    .b = { .avg = 14.0f, .std = 2.9f } },
  { .name = _("forehead"),
    .ethnicity = ETHNIE_CHINESE,
    .L = { .avg = 56.4f, .std = 3.2f },
    .a = { .avg = 11.7f, .std = 2.1f },
    .b = { .avg = 16.3f, .std = 1.4f } },
  { .name = _("forehead"),
    .ethnicity = ETHNIE_THAI,
    .L = { .avg = 56.8f, .std = 4.1f },
    .a = { .avg = 11.6f, .std = 2.2f },
    .b = { .avg = 17.7f, .std = 1.8f } },
  { .name = _("forehead"),
    .ethnicity = ETHNIE_KURDISH,
    .L = { .avg = 56.1f, .std = 4.5f },
    .a = { .avg = 11.3f, .std = 2.1f },
    .b = { .avg = 16.4f, .std = 2.2f } },
  { .name = _("forehead"),
    .ethnicity = ETHNIE_CAUCASIAN,
    .L = { .avg = 59.2f, .std = 5.1f },
    .a = { .avg = 11.6f, .std = 2.8f },
    .b = { .avg = 15.1f, .std = 2.3f } },
  { .name = _("forehead"),
    .ethnicity = ETHNIE_AFRICAN_AM,
    .L = { .avg = 44.0f, .std = 2.0f },
    .a = { .avg = 14.0f, .std = 1.0f },
    .b = { .avg = 19.0f, .std = 1.0f } },
  { .name = _("forehead"),
    .ethnicity = ETHNIE_MEXICAN,
    .L = { .avg = 58.0f, .std = 1.0f },
    .a = { .avg = 15.0f, .std = 1.0f },
    .b = { .avg = 21.0f, .std = 1.0f } },
  { .name = _("cheek"),
    .ethnicity = ETHNIE_CHINESE,
    .L = { .avg = 58.9f, .std = 3.1f },
    .a = { .avg = 11.4f, .std = 2.1f },
    .b = { .avg = 14.2f, .std = 1.5f } },
  { .name = _("cheek"),
    .ethnicity = ETHNIE_THAI,
    .L = { .avg = 60.7f, .std = 4.0f },
    .a = { .avg = 10.5f, .std = 2.3f },
    .b = { .avg = 17.2f, .std = 2.1f } },
  { .name = _("cheek"),
    .ethnicity = ETHNIE_KURDISH,
    .L = { .avg = 58.f,  .std = 4.4f },
    .a = { .avg = 11.7f, .std = 2.3f },
    .b = { .avg = 15.8f, .std = 2.1f } },
  { .name = _("cheek"),
    .ethnicity = ETHNIE_CAUCASIAN,
    .L = { .avg = 59.6f, .std = 5.5f },
    .a = { .avg = 11.8f, .std = 3.1f },
    .b = { .avg = 14.6f, .std = 2.6f } },
  { .name = _("cheek"),
    .ethnicity = ETHNIE_AFRICAN_AM,
    .L = { .avg = 48.0f, .std = 1.0f },
    .a = { .avg = 15.0f, .std = 1.0f },
    .b = { .avg = 20.0f, .std = 1.0f } },
  { .name = _("cheek"),
    .ethnicity = ETHNIE_MEXICAN,
    .L = { .avg = 63.0f, .std = 1.0f },
    .a = { .avg = 16.0f, .std = 1.0f },
    .b = { .avg = 21.0f, .std = 1.0f } } };

  return skin[index];
}

/** Hue sectors of 24 degrees, read at chroma 80-100. */
#define LCH_HUE_SECTORS 15

/** Lightness sectors of 20 %. */
#define LCH_LIGHTNESS_SECTORS 5

/* Colour names by [hue sector][lightness sector].
 *
 * Reference: https://chromatone.center/theory/color/models/perceptual/ -- ignored in places
 * where it gets too lyrical, in favour of more down-to-earth names.
 *
 * Sectors 10 (240 deg) and 11 (264 deg) hold the same names on purpose: CIE Lab 1976 is
 * poor in the blues, so the two were collapsed when this was a branch ladder. The
 * duplication is kept explicit rather than special-cased, so the table reads as a plain
 * grid and a future correction to either sector does not have to untangle a shared branch.
 *
 * N_() registers each name for translation; the lookup translates with _() at use.
 */
static const char *const _lch_color_names[LCH_HUE_SECTORS][LCH_LIGHTNESS_SECTORS] = {
  { N_("deep purple"), N_("fuchsia"), N_("medium magenta"), N_("violet pink"), N_("plum violet") },  // 0°
  { N_("dark red"), N_("red"), N_("crimson"), N_("salmon"), N_("pink") },  // 24°
  { N_("maroon"), N_("dark orange red"), N_("orange red"), N_("coral"), N_("khaki") },  // 48°
  { N_("brown"), N_("chocolate"), N_("dark gold"), N_("gold"), N_("sandy brown") },  // 72°
  { N_("dark green"), N_("dark olive green"), N_("olive"), N_("khaki"), N_("beige") },  // 96°
  { N_("dark green"), N_("forest green"), N_("olive drab"), N_("yellow green"), N_("pale green") },  // 120°
  { N_("dark green"), N_("green"), N_("forest green"), N_("lime green"), N_("pale green") },  // 144°
  { N_("dark sea green"), N_("sea green"), N_("teal"), N_("light sea green"), N_("turquoise") },  // 168°
  { N_("dark slate gray"), N_("light slate gray"), N_("dark cyan"), N_("aqua"), N_("cyan") },  // 192°
  { N_("navy blue"), N_("teal"), N_("dark cyan"), N_("deep sky blue"), N_("aquamarine blue") },  // 216°
  { N_("dark blue"), N_("medium blue"), N_("azure blue"), N_("deep sky blue"), N_("aqua") },  // 240°
  { N_("dark blue"), N_("medium blue"), N_("azure blue"), N_("deep sky blue"), N_("aqua") },  // 264°
  { N_("dark blue"), N_("medium blue"), N_("blue"), N_("light sky blue"), N_("light blue") },  // 288°
  { N_("indigo"), N_("dark violet"), N_("blue violet"), N_("violet"), N_("plum") },  // 312°
  { N_("purple"), N_("dark magenta"), N_("magenta"), N_("violet"), N_("lavender") },  // 336°
};

char *Lch_to_color_name(dt_aligned_pixel_t color)
{
  // color must be Lch derivated from CIE Lab 1976 turned into polar coordinates

  // First check if we have a gray (chromacity < epsilon)
  if(color[1] < 2.0f) return g_strdup(_("gray"));

  // Start with special cases : skin tones
  dt_aligned_pixel_t Lab;
  dt_LCH_2_Lab(color, Lab);

  gchar *out = NULL;
  gboolean matches[ETHNIE_END] = { FALSE };

  // Find a match against any body part and write the associated ethnicity
  for(int elem = 0; elem < SKINS; ++elem)
  {
    skin_color_t skin = get_skin_color(elem);
    range_t L = _compute_range(skin.L, 1.5f);
    range_t a = _compute_range(skin.a, 1.5f);
    range_t b = _compute_range(skin.b, 1.5f);

    const gboolean match = (Lab[0] > L.bottom && Lab[0] < L.top)
                        && (Lab[1] > a.bottom && Lab[1] < a.top)
                        && (Lab[2] > b.bottom && Lab[2] < b.top);

    if(match) matches[skin.ethnicity] = TRUE;
  }

  // Write all matching ethnicities
  for(ethnicities_t elem = 0; elem < ETHNIE_END; ++elem)
    if(matches[elem])
      out = dt_util_dstrcat(out, _("average %s skin tone\n"), _get_ethnicity_name(elem));

  if(!IS_NULL_PTR(out)) return out;

  const float h = color[2] * 360.f; // degrees
  const float L = color[0];         // percent

  const int step_h = (int)h / 24;
  const int step_L = (int)(fminf(L, 100.f)) / 20;

  // h == 360 exactly (and any out-of-domain hue) lands outside the table.
  if(step_h < 0 || step_h >= LCH_HUE_SECTORS) return g_strdup(_("color not found"));
  if(step_L < 0 || step_L >= LCH_LIGHTNESS_SECTORS) return g_strdup(_("color not found"));

  return g_strdup(_(_lch_color_names[step_h][step_L]));
}

void get_skin_tones_range()
{
  float max_chroma = 0.f;
  float min_chroma = 99999.f;

  // Mind the fact that Lch uses atan2 which returns angles in ]-pi;pi]
  float max_hue = -M_PI_F;
  float min_hue = M_PI_F;

  for(int elem = 0; elem < SKINS; ++elem)
  {
    skin_color_t skin = get_skin_color(elem);
    range_t Lr = _compute_range(skin.L, 3.f);
    range_t ar = _compute_range(skin.a, 1.5f);
    range_t br = _compute_range(skin.b, 1.5f);

    const float L[2] = { Lr.top, Lr.bottom };
    const float a[2] = { ar.top, ar.bottom };
    const float b[2] = { br.top, br.bottom };

    for(int i = 0; i < 2; i++)
      for(int j = 0; j < 2; j++)
        for(int k = 0; k < 2; k++)
        {
          dt_aligned_pixel_t Lab = { L[i], a[j], b[k], 1.f };
          dt_aligned_pixel_t XYZ = { 0.f };
          dt_aligned_pixel_t xyY = { 0.f };
          dt_aligned_pixel_t Lch = { 0.f };
          dt_Lab_to_XYZ(Lab, XYZ);
          dt_XYZ_to_xyY(XYZ, xyY);
          dt_xyY_to_Lch(xyY, Lch);

          if(Lch[2] > max_hue) max_hue = Lch[2];
          if(Lch[2] < min_hue) min_hue = Lch[2];

          if(Lch[1] < min_chroma) min_chroma = Lch[1];
          if(Lch[1] > max_chroma) max_chroma = Lch[1];
        }
  }

  fprintf(stdout, "Chroma : [%f;%f], Hue : [%f;%f]\n", min_chroma, max_chroma, min_hue, max_hue);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
