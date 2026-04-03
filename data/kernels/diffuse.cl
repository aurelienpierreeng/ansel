/*
    This file is part of darktable,
    Copyright (C) 2021-2022 Aurélien PIERRE.
    Copyright (C) 2021-2022 Sakari Kapanen.
    Copyright (C) 2023 Luca Zulberti.
    
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

#include "common.h"
#include "noise_generator.h"


// Normalization scaling of the wavelet to approximate a laplacian
// from the function above for sigma = B_SPLINE_SIGMA as a constant
#define B_SPLINE_TO_LAPLACIAN 3.182727439285017f
#define B_SPLINE_TO_LAPLACIAN_2 10.129753952777762f // square


typedef enum dt_isotropy_t
{
  DT_ISOTROPY_ISOTROPE = 0, // diffuse in all directions with same intensity
  DT_ISOTROPY_ISOPHOTE = 1, // diffuse more in the isophote direction (orthogonal to gradient)
  DT_ISOTROPY_GRADIENT = 2  // diffuse more in the gradient direction
} dt_isotropy_t;


// Discretization parameters for the Partial Derivative Equation solver
#define H_STEP 1    // spatial step
#define KAPPA 0.25f // 0.25 if h = 1, 1 if h = 2


static inline void find_gradient(const float4 pixels[9], float4 xy[2])
{
  // Compute the gradient with centered finite differences in a 3x3 stencil
  // warning : x is vertical, y is horizontal
  xy[0] = (pixels[7] - pixels[1]) / 2.f;
  xy[1] = (pixels[5] - pixels[3]) / 2.f;
}

static inline void find_laplacian(const float4 pixels[9], float4 xy[2])
{
  // Compute the laplacian with centered finite differences in a 3x3 stencil
  // warning : x is vertical, y is horizontal
  xy[0] = (pixels[7] + pixels[1]) - 2.f * pixels[4];
  xy[1] = (pixels[5] + pixels[3]) - 2.f * pixels[4];
}

static inline float4 sqf(const float4 in)
{
  return in * in;
}

static inline void rotation_matrix_isophote(const float4 c2,
                                     const float4 cos_theta_sin_theta,
                                     const float4 cos_theta2, const float4 sin_theta2,
                                     float4 a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // taken from https://www.researchgate.net/publication/220663968
  // c dampens the gradient direction
  a[0][0] = cos_theta2 + c2 * sin_theta2;
  a[1][1] = c2 * cos_theta2 + sin_theta2;
  a[0][1] = a[1][0] = (c2 - 1.0f) * cos_theta_sin_theta;
}

static inline void rotation_matrix_gradient(const float4 c2,
                                     const float4 cos_theta_sin_theta,
                                     const float4 cos_theta2, const float4 sin_theta2,
                                     float4 a[2][2])
{
  // Write the coefficients of a square symmetrical matrice of rotation of the gradient :
  // [[ a11, a12 ],
  //  [ a12, a22 ]]
  // based on https://www.researchgate.net/publication/220663968 and inverted
  // c dampens the isophote direction
  a[0][0] = c2 * cos_theta2 + sin_theta2;
  a[1][1] = cos_theta2 + c2 * sin_theta2;
  a[0][1] = a[1][0] = (1.0f - c2) * cos_theta_sin_theta;
}


static inline void build_matrix(const float4 a[2][2], float4 kern[9])
{
  const float4 b11 = a[0][1] / 2.0f;
  const float4 b13 = -b11;
  const float4 b22 = -2.0f * (a[0][0] + a[1][1]);

  // build the kernel of rotated anisotropic laplacian
  // from https://www.researchgate.net/publication/220663968 :
  // [ [ a12 / 2,  a22,            -a12 / 2 ],
  //   [ a11,      -2 (a11 + a22), a11      ],
  //   [ -a12 / 2,   a22,          a12 / 2  ] ]
  // N.B. we have flipped the signs of the a12 terms
  // compared to the paper. There's probably a mismatch
  // of coordinate convention between the paper and the
  // original derivation of this convolution mask
  // (Witkin 1991, https://doi.org/10.1145/127719.122750).
  kern[0] = b11;
  kern[1] = a[1][1];
  kern[2] = b13;
  kern[3] = a[0][0];
  kern[4] = b22;
  kern[5] = a[0][0];
  kern[6] = b13;
  kern[7] = a[1][1];
  kern[8] = b11;
}


static inline void isotrope_laplacian(float4 kern[9])
{
  // see in https://eng.aurelienpierre.com/2021/03/rotation-invariant-laplacian-for-2d-grids/#Second-order-isotropic-finite-differences
  // for references (Oono & Puri)
  kern[0] = 0.25f;
  kern[1] = 0.5f;
  kern[2] = 0.25f;
  kern[3] = 0.5f;
  kern[4] = -3.f;
  kern[5] = 0.5f;
  kern[6] = 0.25f;
  kern[7] = 0.5f;
  kern[8] = 0.25f;
}


static inline void compute_kern(const float4 grad[2], const float anisotropy,
                  const dt_isotropy_t isotropy_type, float4 kern[9])
{
  const float4 magnitude = hypot(grad[0], grad[1]);
  // Compute cos/sin(arg(grad)) with a branchless normalization, forcing
  // arg(grad)=0 when magnitude is zero.
  const float4 nonzero = convert_float4(magnitude != 0.f);
  const float4 inv_mag = 1.f / (magnitude + (1.f - nonzero));
  const float4 cos_theta = grad[0] * inv_mag + (1.f - nonzero); // cos(0)
  const float4 sin_theta = grad[1] * inv_mag;                  // sin(0)
  const float4 c2 = native_exp(-magnitude * anisotropy);
  const float4 cos_theta2 = sqf(cos_theta);
  const float4 sin_theta2 = sqf(sin_theta);
  const float4 cos_theta_sin_theta = cos_theta * sin_theta;

  // Build the matrix of rotation with anisotropy

  switch(isotropy_type)
  {
    case(DT_ISOTROPY_ISOTROPE):
    default:
    {
      isotrope_laplacian(kern);
      break;
    }
    case(DT_ISOTROPY_ISOPHOTE):
    {
      float4 a[2][2] = { { (float4)0.f } };
      rotation_matrix_isophote(c2, cos_theta_sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kern);
      break;
    }
    case(DT_ISOTROPY_GRADIENT):
    {
      float4 a[2][2] = { { (float4)0.f } };
      rotation_matrix_gradient(c2, cos_theta_sin_theta, cos_theta2, sin_theta2, a);
      build_matrix(a, kern);
      break;
    }
  }
}

static inline float4 convolve_kernel(const float4 input[9], const float4 grad[2],
                                     const float anisotropy, const dt_isotropy_t isotropy_type)
{
  float4 kern[9];
  compute_kern(grad, anisotropy, isotropy_type, kern);

  float4 acc = (float4)0.f;
  for(int k = 0; k < 9; k++)
    acc += kern[k] * input[k];

  return acc;
}


kernel void
diffuse_pde(read_only image2d_t HF, read_only image2d_t LF,
            read_only image2d_t mask, const int has_mask,
            write_only image2d_t output,
            const int width, const int height,
            const float4 anisotropy, const int4 isotropy_type,
            const float normalized_regularization, const float variance_threshold,
            const int mult, const float4 ABCD,
            const float strength)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;

  const char opacity = (has_mask) ? read_imageui(mask, sampleri, (int2)(x, y)).x : 1;

  float4 out;

  if(opacity)
  {
    // Walk the same sparse a-trous sub-lattice as the historical solver.
    const int j_neighbours[3] = {
      clamp((x - mult * H_STEP), 0, width - 1),
      x,
      clamp((x + mult * H_STEP), 0, width - 1) };
    const int i_neighbours[3] = {
      clamp((y - mult * H_STEP), 0, height - 1),
      y,
      clamp((y + mult * H_STEP), 0, height - 1) };

    // Fetch the local 3x3 neighborhood for HF, compute its gradient direction,
    // and accumulate HF/LF energy in the same pass.
    float4 neighbour_pixel_HF[9];
    float4 neighbour_pixel_LF[9];
    float4 energy = (float4)0.f;

    for(int ii = 0; ii < 3; ii++)
      for(int jj = 0; jj < 3; jj++)
      {
        const int k = 3 * ii + jj;
        const int2 p = (int2)(j_neighbours[ii], i_neighbours[jj]);
        const float4 hf_value = read_imagef(HF, samplerA, p);
        const float4 lf_value = read_imagef(LF, samplerA, p);
        neighbour_pixel_HF[k] = hf_value;
        neighbour_pixel_LF[k] = lf_value;
        const float4 safe_lf = fmax(lf_value - (float4)(FLT_MIN), (float4)0.f) + (float4)(FLT_MIN);
        const float4 ratio = hf_value / safe_lf;
        energy += ratio * ratio;
      }

    // normalized_regularization already folds together the user
    // regularization, the 3x3-support averaging factor, the physical blur
    // radius carried by the current wavelet band and its scale normalization.
    energy = variance_threshold + energy * normalized_regularization;

    float4 hf_gradient[2], lf_gradient[2];
    find_gradient(neighbour_pixel_HF, hf_gradient);
    find_gradient(neighbour_pixel_LF, lf_gradient);

    // Convolve filters and accumulate the local HF band energy over the
    // current 3x3 support. This is not a statistical variance estimator:
    // HF is a band-pass residual, so we normalize each sample by the
    // corresponding LF value before squaring it, then normalize the summed
    // ratio by the physical kernel-variance increment of the current
    // wavelet band.
    float4 derivatives[4];
    derivatives[0] = convolve_kernel(neighbour_pixel_LF, lf_gradient,
                                     anisotropy.x, isotropy_type.x);
    derivatives[1] = convolve_kernel(neighbour_pixel_LF, hf_gradient,
                                     anisotropy.y, isotropy_type.y);
    derivatives[2] = convolve_kernel(neighbour_pixel_HF, lf_gradient,
                                     anisotropy.z, isotropy_type.z);
    derivatives[3] = convolve_kernel(neighbour_pixel_HF, hf_gradient,
                                     anisotropy.w, isotropy_type.w);

    // Compute the update on the current a-trous sub-lattice.
    const float4 acc
        = neighbour_pixel_HF[4] * strength 
          + (derivatives[0] * ABCD.x + derivatives[1] * ABCD.y
               + derivatives[2] * ABCD.z + derivatives[3] * ABCD.w) / energy;

    // update the solution
    out = fmax(acc + neighbour_pixel_LF[4], 0.f);
  }
  else
  {
    float4 hf = read_imagef(HF, samplerA, (int2)(x, y));
    float4 lf = read_imagef(LF, samplerA, (int2)(x, y));
    out = fmax(hf + lf, 0.f);
  }

  write_imagef(output, (int2)(x, y), out);
}

kernel void
build_mask(read_only image2d_t in, write_only image2d_t mask,
           const float threshold, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float4 pix_in = read_imagef(in, samplerA, (int2)(x, y));
  float m = (pix_in.x > threshold || pix_in.y > threshold || pix_in.z > threshold);
  write_imageui(mask, (int2)(x, y), m);
}

kernel void
inpaint_mask(write_only image2d_t inpainted, read_only image2d_t original,
              read_only image2d_t mask, const int width, const int height)
{
  const int x = get_global_id(0);
  const int y = get_global_id(1);

  if(x >= width || y >= height) return;
  float4 pix_in = read_imagef(original, samplerA, (int2)(x, y));
  char m = read_imageui(mask, samplerA, (int2)(x, y)).x;
  float4 pix_out = pix_in;

  if(m)
  {
    unsigned int state[4] = { splitmix32(x + 1), splitmix32((x + 1) * (y + 3)), splitmix32(1337), splitmix32(666) };
    xoshiro128plus(state);
    xoshiro128plus(state);
    xoshiro128plus(state);
    xoshiro128plus(state);
    pix_out = fabs(gaussian_noise_simd(pix_in, pix_in, state));
  }

  write_imagef(inpainted, (int2)(x, y), pix_out);
}
