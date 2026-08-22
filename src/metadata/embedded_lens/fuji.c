#include "embedded_lens.h"
#include "embedded_lens_vendors.h"

#include <math.h>


gboolean _fuji_has_data(const dt_image_correction_data_t *cd)
{
  return cd->fuji.nc > 0 && cd->fuji.nc <= 11;
}

gboolean _fuji_has_distortion(const dt_image_correction_data_t *cd)
{
  /* nc bounds the tables below; _fuji_has_data owns that check. */
  if(!_fuji_has_data(cd)) return FALSE;

  for(int i = 0; i < cd->fuji.nc; i++)
    if(cd->fuji.distortion[i] != 0.0f) return TRUE;
  return FALSE;
}

gboolean _fuji_has_vignetting(const dt_image_correction_data_t *cd)
{
  /* nc bounds the tables below; _fuji_has_data owns that check. */
  if(!_fuji_has_data(cd)) return FALSE;

  for(int i = 0; i < cd->fuji.nc; i++)
    if(cd->fuji.vignetting[i] != 0.0f) return TRUE;
  return FALSE;
}

gboolean _fuji_has_ca(const dt_image_correction_data_t *cd)
{
  /* nc bounds the tables below; _fuji_has_data owns that check. */
  if(!_fuji_has_data(cd)) return FALSE;

  for(int i = 0; i < cd->fuji.nc; i++)
    if(cd->fuji.ca_r[i] != 0.0f || cd->fuji.ca_b[i] != 0.0f) return TRUE;
  return FALSE;
}

int _fuji_populate(const dt_image_correction_data_t *cd,
                    const dt_embedded_lens_finetune_t *ft,
                    struct dt_embedded_lens_knots_t *knots,
                    const float *out_scale)
{
  (void)out_scale;
  const dt_image_correction_fuji_t *const fuji = &cd->fuji;
  const int ncsrc = fuji->nc;

  float knots_in[LENS_MAXKNOTS] = { 0.f };
  float cor_rgb_in[LENS_MAXKNOTS] = { 0.f };
  float cor_ca_r_in[LENS_MAXKNOTS] = { 0.f };
  float cor_ca_b_in[LENS_MAXKNOTS] = { 0.f };

  int j = 0;
  if(fuji->knots[0] > 0.0f)
  {
    knots_in[j] = 0.0f;
    cor_rgb_in[j] = 1.0f;
    cor_ca_r_in[j] = 0.0f;
    cor_ca_b_in[j] = 0.0f;
    knots->knots_vig[j] = 0.0f;
    knots->vig[j] = 1.0f;
    j++;
  }

  for(int i = 0; i < ncsrc; i++, j++)
  {
    knots_in[j] = fuji->cropf * fuji->knots[i];
    cor_rgb_in[j] = ft->distortion * (fuji->distortion[i] / 100.0f) + 1.0f;
    cor_ca_r_in[j] = ft->ca_red * fuji->ca_r[i];
    cor_ca_b_in[j] = ft->ca_blue * fuji->ca_b[i];

    knots->knots_vig[j] = fuji->cropf * fuji->knots[i];
    knots->vig[j] = 1.0f - ft->vignette * (1.0f - fuji->vignetting[i] / 100.0f);
  }
  const int ncin = j;
  if(ncin <= 0) return 0;

  for(int k = ncin; k < LENS_MAXKNOTS; k++)
  {
    knots->knots_vig[k] = knots->knots_vig[ncin - 1] + (float)(k - ncin + 1);
    knots->vig[k] = knots->vig[ncin - 1];
  }

  const int nc = LENS_MAXKNOTS;
  for(int i = 0; i < nc; i++)
  {
    const float rin = (float)i / (float)(nc - 1);
    const float m = dt_embedded_lens_linear_spline(knots_in, cor_rgb_in, ncin, rin);
    const float r = (fabsf(m) > 1e-6f) ? rin / m : rin;
    knots->knots_dist[i] = r;

    knots->cor_rgb[0][i] = m;
    knots->cor_rgb[1][i] = m;
    knots->cor_rgb[2][i] = m;

    const float mcar = dt_embedded_lens_linear_spline(knots_in, cor_ca_r_in, ncin, rin);
    const float mcab = dt_embedded_lens_linear_spline(knots_in, cor_ca_b_in, ncin, rin);
    knots->cor_rgb[0][i] *= mcar + 1.0f;
    knots->cor_rgb[2][i] *= mcab + 1.0f;
  }

  return nc;
}

