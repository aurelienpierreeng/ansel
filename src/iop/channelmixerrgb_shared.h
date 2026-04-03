#pragma once

#include "common/chromatic_adaptation.h"
#include "common/iop_profile.h"

#include <glib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _GtkWidget GtkWidget;

#define DT_IOP_CHANNELMIXER_SHARED_SIMPLE_TAN_SCALE 1.55f
#define DT_IOP_CHANNELMIXER_SHARED_SIMPLE_EPS 5e-5f
#define DT_IOP_CHANNELMIXER_SHARED_SIMPLE_CHROMA_PROBE 0.5f

typedef enum dt_iop_channelmixer_shared_primaries_basis_t
{
  DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_RGB = 0,
  DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_XYZ = 1,
  DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_BRADFORD = 2,
  DT_IOP_CHANNELMIXER_SHARED_PRIMARIES_BASIS_CAT16 = 3,
} dt_iop_channelmixer_shared_primaries_basis_t;

typedef enum dt_iop_channelmixer_shared_simple_probe_t
{
  DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_ROTATION = 0,
  DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_AXIS_1 = 1,
  DT_IOP_CHANNELMIXER_SHARED_SIMPLE_PROBE_AXIS_2 = 2,
} dt_iop_channelmixer_shared_simple_probe_t;

typedef struct dt_iop_channelmixer_shared_simple_params_t
{
  float theta;
  float psi;
  float stretch_1;
  float stretch_2;
  float coupling_amount;
  float coupling_hue;
} dt_iop_channelmixer_shared_simple_params_t;

typedef struct dt_iop_channelmixer_shared_primaries_params_t
{
  float achromatic_hue;
  float achromatic_purity;
  float red_hue;
  float red_purity;
  float green_hue;
  float green_purity;
  float blue_hue;
  float blue_purity;
  float gain;
} dt_iop_channelmixer_shared_primaries_params_t;

float dt_iop_channelmixer_shared_wrap_pi(float angle);
float dt_iop_channelmixer_shared_wrap_half_pi(float angle);
float dt_iop_channelmixer_shared_encode_simple_stretch(float stretch);
float dt_iop_channelmixer_shared_decode_simple_stretch(float slider);
float dt_iop_channelmixer_shared_encode_simple_coupling_amount(float amount);
float dt_iop_channelmixer_shared_decode_simple_coupling_amount(float slider);
void dt_iop_channelmixer_shared_simple_from_sliders(GtkWidget *const widgets[6],
                                                    dt_iop_channelmixer_shared_simple_params_t *simple);
void dt_iop_channelmixer_shared_simple_to_sliders(const dt_iop_channelmixer_shared_simple_params_t *simple,
                                                  GtkWidget *const widgets[6]);
void dt_iop_channelmixer_shared_primaries_from_sliders(GtkWidget *const widgets[9],
                                                       dt_iop_channelmixer_shared_primaries_params_t *primaries);
void dt_iop_channelmixer_shared_primaries_to_sliders(const dt_iop_channelmixer_shared_primaries_params_t *primaries,
                                                     GtkWidget *const widgets[9]);

gboolean dt_iop_channelmixer_shared_rows_are_normalized(const gboolean normalize[3]);
gboolean dt_iop_channelmixer_shared_get_matrix(const float rows[3][3], const gboolean normalize[3],
                                               gboolean force_normalize, float M[3][3]);
void dt_iop_channelmixer_shared_set_matrix(float rows[3][3], const float M[3][3]);
void dt_iop_channelmixer_shared_mul3x3(const float A[3][3], const float B[3][3], float C[3][3]);

void dt_iop_channelmixer_shared_simple_from_matrix(const float M[3][3],
                                                   dt_iop_channelmixer_shared_simple_params_t *simple);
void dt_iop_channelmixer_shared_simple_to_matrix(const dt_iop_channelmixer_shared_simple_params_t *simple,
                                                 float M[3][3]);
float dt_iop_channelmixer_shared_roundtrip_error(const float M[3][3], const float roundtrip[3][3]);

dt_iop_channelmixer_shared_primaries_basis_t
dt_iop_channelmixer_shared_primaries_basis_from_adaptation(dt_adaptation_t adaptation);
gboolean dt_iop_channelmixer_shared_primaries_to_matrix(dt_iop_channelmixer_shared_primaries_basis_t basis,
                                                        const dt_iop_channelmixer_shared_primaries_params_t *primaries,
                                                        float M[3][3]);
gboolean dt_iop_channelmixer_shared_primaries_from_matrix(dt_iop_channelmixer_shared_primaries_basis_t basis,
                                                          const float M[3][3],
                                                          dt_iop_channelmixer_shared_primaries_params_t *primaries);

void dt_iop_channelmixer_shared_simple_probe_source(dt_iop_channelmixer_shared_simple_probe_t probe, float source[3]);
void dt_iop_channelmixer_shared_work_rgb_to_display(const dt_aligned_pixel_t work_rgb,
                                                    const dt_iop_order_iccprofile_info_t *work_profile,
                                                    const dt_iop_order_iccprofile_info_t *display_profile,
                                                    dt_aligned_pixel_t display_rgb);
void dt_iop_channelmixer_shared_module_color_to_display(const float module_color[3], dt_adaptation_t adaptation,
                                                        const dt_iop_order_iccprofile_info_t *work_profile,
                                                        const dt_iop_order_iccprofile_info_t *display_profile,
                                                        float display_rgb[3]);
void dt_iop_channelmixer_shared_paint_temperature_slider(GtkWidget *widget, float temperature_min,
                                                         float temperature_max);
void dt_iop_channelmixer_shared_paint_row_sliders(dt_adaptation_t adaptation,
                                                  const dt_iop_order_iccprofile_info_t *work_profile,
                                                  const dt_iop_order_iccprofile_info_t *display_profile,
                                                  float r, float g, float b, gboolean normalize,
                                                  const float row[3], GtkWidget *const widgets[3]);
void dt_iop_channelmixer_shared_paint_simple_sliders(dt_adaptation_t adaptation,
                                                     const dt_iop_order_iccprofile_info_t *work_profile,
                                                     const dt_iop_order_iccprofile_info_t *display_profile,
                                                     const dt_iop_channelmixer_shared_simple_params_t *simple,
                                                     GtkWidget *const widgets[6]);
void dt_iop_channelmixer_shared_paint_primaries_sliders(
    dt_adaptation_t adaptation, const dt_iop_order_iccprofile_info_t *work_profile,
    const dt_iop_order_iccprofile_info_t *display_profile,
    dt_iop_channelmixer_shared_primaries_basis_t basis,
    const dt_iop_channelmixer_shared_primaries_params_t *primaries, GtkWidget *const widgets[9]);

#ifdef __cplusplus
}
#endif
