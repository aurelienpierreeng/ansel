
/*
    This file is part of the Ansel project.
    Copyright (C) 2023 Davide Patria.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

/**
 * @file
 * @brief Experimental 3x3 polar decomposition helpers used for color transform analysis.
 *
 * @details
 * This implementation follows the 3x3 polar decomposition strategy described in
 * "An algorithm to compute the polar decomposition of a 3 x 3 matrix" by
 * Nicholas J. Higham and Vanni Noferini, 2016.
 *
 * Let A be a non-singular 3×3 matrice, like the ones used in channel mixer or
 *in cameras input profiles. Such matrices define transforms between RGB and XYZ
 *spaces depending on the vector base transform. The vector base is the 3 RGB
 *primaries, defined from/to XYZ reference (absolute) space. Converting between
 * color spaces is then only a change of coordinates for the pixels color
 *vector, depending on how the primaries rotate and rescale in XYZ.
 *
 * RGB spaces conversions are therefore linear maps from old RGB to XYZ to new
 *RGB. Geometrically, linear maps can be interpreted as a combination of
 *scalings (homothety), rotations and shear mapping (transvection).
 *
 * But they also have an interesting property : 
 *
 *   For any 3×3 invertible matrice A describing a linear map, the general
 *linear map can be decomposed as a single 3D rotation around a particular 3D
 *vector.
 *
 *   That is, there is a factorization of A = Q * H, where Q is
 *   the matrice of rotation around a axis of vector H.
 *
 *
 * This is interesting for us, on the GUI side. 3×3 matrices (9 params) are not
 *intuitive to users, and the visual result of a single coefficient change is
 *hard to predict. This method allows us to reduce 9 input parameters to :
 *  * 6 : 3 angles of rotation, and the 3D coordinates of the (non-unit)
 *rotation axis vector,
 *  * 7 : 3 angles of rotation, the 3D coordinates of the unit rotation axis
 *vector, and a scaling factor for this vector.
 *
 * Usually, this is achieved by using HSL spaces, which suck because they work
 *only for bounded signals in [ 0 ; 1 ]. Also, they are not colorspaces, not
 *connected to either physics or psychology, so they are bad. Anyone saying
 * otherwise is fake news.
 *
 * The present method generalizes the HSL approach to XYZ, LMS and weird spaces,
 *with none of the drawbacks of the the cheapo lazy-ass maths-disabled HSL
 *bullshit. It's great. You should try it some time. Simply the best.
 *
 * Reference paper:
 * https://www.researchgate.net/publication/296638898_An_algorithm_to_compute_the_polar_decomposition_of_a_3_3_matrix/link/56e29d8c08ae03f02790a388/download
 *
 * Reference implementation:
 * https://github.com/higham/polar-decomp-3by3
 */

// define the type for the variables that were previously TYPE as more precision was not though to be necessary
// an if so the type can be choosen with a gcc flag but would fallback to this if not defined
#ifndef TYPE
#define TYPE double
#endif

// define which ABS to use based on the chosen type
#if TYPE == double
#define ABS(n) fabs(n)
#define SQRT(n) sqrt(n)
#elif TYPE == TYPE
#define ABS(n) fabs(n)
#define SQRT(n) sqrtf(n)
#endif

#include <complex.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
// is it necessary?
// #include "QR_decomp.h"

// Note : if you review this code using the original Matlab implementation,
// remember Matlab indexes arrays from 1, while C starts at 0, so every index
// needs to be shifted by -1.

/**
 * @brief Swap two rows of a row-major dense matrix in place.
 *
 * @param[in,out] m Matrix data stored row-major.
 * @param[in] rows Number of rows in @p m.
 * @param[in] cols Number of columns in @p m.
 * @param[in] row0 Index of the first row to swap.
 * @param[in] row1 Index of the second row to swap.
 */
void swap_rows(TYPE *m, size_t rows, size_t cols, size_t row0, size_t row1);

/**
 * @brief Swap two columns of a row-major dense matrix in place.
 *
 * @param[in,out] m Matrix data stored row-major.
 * @param[in] rows Number of rows in @p m.
 * @param[in] cols Number of columns in @p m.
 * @param[in] col0 Index of the first column to swap.
 * @param[in] col1 Index of the second column to swap.
 */
void swap_cols(TYPE *m, size_t rows, size_t cols, size_t col0, size_t col1);

/**
 * @brief Replace each element of a vector with its absolute value.
 *
 * @param[in,out] matrix_abs Flat array to process in place.
 * @param[in] num_el Number of elements to process.
 */
void abs_matrix(TYPE *matrix_abs, size_t num_el);

/**
 * @brief Multiply two dense row-major matrices.
 *
 * The function assumes the matrix shapes are compatible and does not perform
 * dimension validation. It is used as a small utility inside the decomposition
 * code where the shapes are known statically at the call site.
 *
 * @param[in] A Left matrix stored row-major with shape @p rows_A x @p cols_A.
 * @param[in] B Right matrix stored row-major with shape @p cols_A x @p cols_B.
 * @param[out] RES Product matrix stored row-major with shape @p rows_A x @p cols_B.
 * @param[in] rows_A Number of rows in @p A.
 * @param[in] cols_A Number of columns in @p A and rows in @p B.
 * @param[in] cols_B Number of columns in @p B.
 */
void matrix_multiply(const TYPE *A, const TYPE *B, TYPE *RES, int rows_A, int cols_A, int cols_B);

/**
 * @brief Return the maximum entry of a flat matrix buffer.
 *
 * @param[in] matrice Flat matrix storage.
 * @param[in] size Number of elements in @p matrice.
 * @return Largest value found in the buffer.
 */
TYPE max_val_matrix(TYPE *matrice, size_t size);

/**
 * @brief Normalize a vector to unit Euclidean norm.
 *
 * @param[in,out] vector Vector to normalize in place.
 * @param[in] size Number of elements in @p vector.
 */
static inline void normalize_array(TYPE *vector, int size);

/**
 * @brief Print a row-major matrix for manual debugging.
 *
 * @param[in] m Matrix data stored row-major.
 * @param[in] rows Number of rows.
 * @param[in] cols Number of columns.
 */
void stampa_matrice(TYPE *m, size_t rows, size_t cols);

/**
 * @brief Compute one non-zero vector in the null space of a symmetric 2x2 matrix.
 *
 * The coefficients correspond to the matrix
 * <tt>[ a b ; b c ]</tt>, which is the shape produced by the reduced system in
 * the polar decomposition code.
 *
 * @param[out] nullspace Output vector of length 2.
 * @param[in] a Matrix coefficient (0, 0).
 * @param[in] b Matrix coefficient (0, 1) and (1, 0).
 * @param[in] c Matrix coefficient (1, 1).
 */
void compute_null_space(TYPE *nullspace, const TYPE a, const TYPE b, const TYPE c);

/**
 * @brief Copy a flat buffer.
 *
 * @param[out] dest Destination buffer.
 * @param[in] source Source buffer.
 * @param[in] num_el Number of elements to copy.
 */
void copy(TYPE *dest, TYPE *source, size_t num_el);

/**
 * @brief Build two orthonormal candidate eigenvectors from symbolic QR factors.
 *
 * @param[out] v0 First output vector of length 4.
 * @param[out] v1 Second output vector of length 4.
 * @param[in] v00 First symbolic coefficient.
 * @param[in] v10 Second symbolic coefficient.
 * @param[in] v01 Third symbolic coefficient.
 * @param[in] v11 Fourth symbolic coefficient.
 */
void orthonormalize_v_with_qr(TYPE *v0, TYPE *v1, const TYPE v00, const TYPE v10, const TYPE v01, const TYPE v11);

/**
 * @brief Multiply a vector by the lower-triangular inverse factor used in the LDL solve.
 *
 * @param[out] RES Output vector of length 4.
 * @param[in] IL01 Lower-triangular coefficient.
 * @param[in] IL02 Lower-triangular coefficient.
 * @param[in] IL03 Lower-triangular coefficient.
 * @param[in] IL12 Lower-triangular coefficient.
 * @param[in] IL13 Lower-triangular coefficient.
 * @param[in] v Input vector of length 4.
 */
void multiply_il_v(TYPE *RES, const TYPE IL01, const TYPE IL02, const TYPE IL03, const TYPE IL12, const TYPE IL13,
                   const TYPE *v);

/**
 * @brief Multiply a vector by the block-diagonal inverse factor used in the LDL solve.
 *
 * @param[out] RES Output vector of length 4.
 * @param[in] ID00 Reciprocal of the first diagonal coefficient.
 * @param[in] ID11 Reciprocal of the second diagonal coefficient.
 * @param[in] ID Pointer to the trailing 2x2 block stored row-major.
 * @param[in] v Input vector of length 4.
 */
void multiply_id_v(TYPE *RES, const TYPE ID00, const TYPE ID11, const TYPE *ID, const TYPE *v);

/**
 * @brief Multiply a vector by the transpose-side lower-triangular factor used in the LDL solve.
 *
 * @param[out] RES Output vector of length 4.
 * @param[in] v Input vector of length 4.
 * @param[in] IL01 Lower-triangular coefficient.
 * @param[in] IL02 Lower-triangular coefficient.
 * @param[in] IL03 Lower-triangular coefficient.
 * @param[in] IL12 Lower-triangular coefficient.
 * @param[in] IL13 Lower-triangular coefficient.
 */
void multiply_v_il(TYPE *RES, const TYPE *v, const TYPE IL01, const TYPE IL02, const TYPE IL03, const TYPE IL12,
                   const TYPE IL13);

/**
 * @brief Re-orthonormalize two 4D vectors after repeated inverse iteration steps.
 *
 * @param[in,out] v0 First vector of length 4.
 * @param[in,out] v1 Second vector of length 4.
 */
void orthonormalize_v_with_qr_single(TYPE *v0, TYPE *v1);

/**
 * @brief Apply the negative D factor of the LDL factorization to a vector.
 *
 * @param[out] RES Output vector of length 4.
 * @param[in] v Input vector of length 4.
 * @param[in] D Pointer to the 4x4 factor stored row-major.
 */
void multiply_minus_v_d(TYPE *RES, TYPE *v, TYPE *D);

/**
 * @brief Compute the scalar product of two vectors.
 *
 * @param[in] a First vector.
 * @param[in] b Second vector.
 * @param[in] num_el Number of elements in both vectors.
 * @return Dot product of @p a and @p b.
 */
TYPE vector_scalar(const TYPE *a, const TYPE *b, const size_t num_el);

/**
 * @brief Compute the polar decomposition of a 3x3 matrix.
 *
 * The function factorizes the input matrix as <tt>A = Q * H</tt>, where
 * <tt>Q</tt> is orthogonal and <tt>H</tt> is symmetric positive semidefinite.
 * The implementation first normalizes the input matrix, then builds a reduced
 * eigenproblem whose dominant eigenvector is converted into the quaternion-like
 * parametrization used to reconstruct the orthogonal factor.
 *
 * @param[in,out] A Input 3x3 matrix, temporarily normalized in place and
 * restored before returning.
 * @param[out] Q Orthogonal factor.
 * @param[out] H Symmetric factor.
 */
void polar_decomposition(TYPE A[3][3], TYPE Q[3][3], TYPE H[3][3])
{
  // Frobenius / L2 norm of the matrice - aka we sum the squares of each
  // matrice element and take the sqrt
  const TYPE norm
      = SQRT(A[0][0] * A[0][0] + A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][0] * A[1][0] + A[1][1] * A[1][1]
             + A[1][2] * A[1][2] + A[2][0] * A[2][0] + A[2][1] * A[2][1] + A[2][2] * A[2][2]);

  // Normalize the matrice A in-place, so A norm is 1
  for(size_t i = 0; i < 3; i++)
    for(size_t j = 0; j < 3; j++) A[i][j] /= norm;

  // Compute the conditionning of the matrice
  TYPE m = A[1][1] * A[2][2] - A[1][2] * A[2][1];
  TYPE b = m * m;

  m = A[1][0] * A[2][2] - A[1][2] * A[2][0];
  b += m * m;
  m = A[1][0] * A[2][1] - A[1][1] * A[2][0];
  b += m * m;

  m = A[0][0] * A[2][1] - A[0][1] * A[2][0];
  b += m * m;
  m = A[0][0] * A[2][2] - A[0][2] * A[2][0];
  b += m * m;
  m = A[0][1] * A[2][2] - A[0][2] * A[2][1];
  b += m * m;

  m = A[0][1] * A[1][2] - A[0][2] * A[1][1];
  b += m * m;
  m = A[0][0] * A[1][2] - A[0][2] * A[1][0];
  b += m * m;
  m = A[0][0] * A[1][1] - A[0][1] * A[1][0];
  b += m * m;

  b = -4.0 * b + 1.0;

  bool subspa = false;
  int quick = 1;
  TYPE d = 0.0;
  TYPE dd = 1.0;
  TYPE nit = 0.0;
  TYPE U[3] = { 0.0, 0.0, 0.0 };
  TYPE AA[3][3];

  // copy of A
  copy(*AA, *A, 9);

  if((b - 1.0 + 1.0e-4) > 0.0)
  {
    quick = 0;

    // LU (full).
    size_t r = 0;
    size_t c = 0;

    // Search index (r, c) of the max element in matrice
    if(ABS(A[1][0]) > ABS(A[0][0])) r = 1;
    if(ABS(A[2][0]) > ABS(A[r][c])) r = 2;
    if(ABS(A[0][1]) > ABS(A[r][c]))
    {
      r = 0;
      c = 1;
    }
    if(ABS(A[1][1]) > ABS(A[r][c]))
    {
      r = 1;
      c = 1;
    }
    if(ABS(A[2][1]) > ABS(A[r][c]))
    {
      r = 2;
      c = 1;
    }
    if(ABS(A[0][2]) > ABS(A[r][c]))
    {
      r = 0;
      c = 2;
    }
    if(ABS(A[1][2]) > ABS(A[r][c]))
    {
      r = 1;
      c = 2;
    }
    if(ABS(A[2][2]) > ABS(A[r][c]))
    {
      r = 2;
      c = 2;
    }

    if(r > 0)
    {
      // invert lines 0 and r
      swap_rows(*AA, 3, 3, 0, r);
      dd = -1.0;
    }

    if(c > 0)
    {
      swap_cols(*AA, 3, 3, 0, c);
      dd = -dd;
    }

    U[0] = AA[0][0];

    const TYPE m0 = AA[0][1] / AA[0][0];
    const TYPE m1 = AA[0][2] / AA[0][0];
    TYPE AAA[2][2] = { { AA[1][1] - AA[1][0] * m0, AA[1][2] - AA[1][0] * m1 },
                       { AA[2][1] - AA[2][0] * m0, AA[2][2] - AA[2][0] * m1 } };

    r = 0;
    c = 0;
    if(ABS(AAA[1][0]) > ABS(AAA[0][0])) r = 1;
    if(ABS(AAA[0][1]) > ABS(AAA[r][c]))
    {
      r = 0;
      c = 1;
    }
    if(ABS(AAA[1][1]) > ABS(AAA[r][c]))
    {
      r = 1;
      c = 1;
    }

    if(r == 1) dd = -dd;
    if(c > 0) dd = -dd;

    U[1] = AAA[r][c];

    // fixed from U(2). needs check
    if(U[1] == 0)
      U[2] = 0;
    else
      U[2] = AAA[1 - r][1 - c] - AAA[r][1 - c] * AAA[1 - r][c] / U[1];

    d = dd;
    dd = dd * U[0] * U[1] * U[2];

    if(U[0] < 0) d = -d;
    if(U[1] < 0) d = -d;
    if(U[2] < 0) d = -d;

    const TYPE AU = ABS(U[1]);
    if(AU > 6.607e-8)
    {
      nit = 16.8 + 2.0 * log10(AU);
      nit = ceil(15.0 / nit);
    }
    else
    {
      subspa = true;
    }
  }
  else
  {
    // LU (partial).
    if(ABS(A[1][0]) > ABS(A[2][0]))
    {
      if(ABS(A[0][0]) > ABS(A[1][0]))
      {
        copy(*AA, *A, 9);
        dd = 1.0;
      }
      else
      {
        copy(AA[0], A[1], 3);
        copy(AA[1], A[0], 3);
        copy(AA[2], A[2], 3);
        dd = -1.0;
      }
    }
    else
    {
      if(ABS(A[0][0]) > ABS(A[2][0]))
      {
        copy(*AA, *A, 9);
        dd = 1.0;
      }
      else
      {
        copy(AA[0], A[2], 3);
        copy(AA[1], A[1], 3);
        copy(AA[2], A[0], 3);
        dd = -1.0;
      }
    }

    d = dd;
    U[0] = AA[0][0];
    if(U[0] < 0) d = -d;

    const TYPE m0 = AA[0][1] / AA[0][0];
    const TYPE m1 = AA[0][2] / AA[0][0];
    TYPE AAA[2][2] = { { AA[1][1] - AA[1][0] * m0, AA[1][2] - AA[1][0] * m1 },
                       { AA[2][1] - AA[2][0] * m0, AA[2][2] - AA[2][0] * m1 } };

    if(ABS(AAA[0][0]) < ABS(AAA[1][0]))
    {
      U[1] = AAA[1][0];
      U[2] = AAA[0][1] - AAA[0][0] * AAA[1][1] / AAA[1][0];
      dd = -dd;
      d = -d;
      if(U[1] < 0) d = -d;
      if(U[2] < 0) d = -d;
    }
    else if(AAA[0][0] == 0)
    {
      U[1] = 0;
      U[2] = 0;
    }
    else
    {
      U[1] = AAA[0][0];
      U[2] = AAA[1][1] - AAA[1][0] * AAA[0][1] / AAA[0][0];
      if(U[1] < 0) d = -d;
      if(U[2] < 0) d = -d;
    }

    dd = dd * U[0] * U[1] * U[2];
  }

  if(d == 0) d = 1.0;

  dd = 8.0 * d * dd;

  const TYPE t = A[0][0] + A[1][1] + A[2][2];
  TYPE B[4][4] = { { t, A[1][2] - A[2][1], A[2][0] - A[0][2], A[0][1] - A[1][0] },
                   { 0.0, 2.0 * A[0][0] - t, A[0][1] + A[1][0], A[0][2] + A[2][0] },
                   { 0.0, 0.0, 2.0 * A[1][1] - t, A[1][2] + A[2][1] },
                   { 0.0, 0.0, 0.0, 2.0 * A[2][2] - t } };

  for(size_t i = 0; i < 4; i++)
    for(size_t j = 0; j < 4; j++) B[i][j] *= d;

  B[1][0] = B[0][1];
  B[2][0] = B[0][2];
  B[3][0] = B[0][3];
  B[2][1] = B[1][2];
  B[3][1] = B[1][3];
  B[3][2] = B[2][3];

  // Find largest eigenvalue by analytic formula
  TYPE x = 0.0;
  if(b >= -0.3332)
  {
    const TYPE Delta0 = 1.0 + 3.0 * b;
    const TYPE Delta1 = -1.0 + (27.0 / 16.0) * dd * dd + 9.0 * b;
    TYPE phi = Delta1 / Delta0;
    phi /= SQRT(Delta0);
    const TYPE SS = (4.0 / 3.0) * (1.0 + cos(acos(phi) / 3.0) * SQRT(Delta0));
    const TYPE S = SQRT(SS) / 2.0;
    x = S + 0.5 * SQRT(fmax(0.0, -SS + 4.0 + dd / S));
  }
  else
  {
    x = SQRT(3.0);
    TYPE xold = 3.0;
    while((xold - x) > 1.0e-12)
    {
      xold = x;
      const TYPE px = x * (x * (x * x - 2.0) - dd) + b;
      const TYPE dpx = x * (4.0 * x * x - 4.0) - dd;
      x = x - px / dpx;
    }
  }

  TYPE v[4] = { 0.0, 0.0, 0.0, 0.0 };

  if(quick)
  {
    // LDL
    TYPE BB[4][4];
    copy(*BB, *B, 16);
    for(size_t i = 0; i < 4; i++)
    {
      for(size_t j = 0; j < 4; j++) BB[i][j] = -BB[i][j];
      BB[i][i] += x;
    }

    size_t p[4] = { 0, 1, 2, 3 };
    TYPE L[4][4]
        = { { 1.0, 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0, 0.0 }, { 0.0, 0.0, 0.0, 1.0 } };
    TYPE Ddiag[4] = { 0.0, 0.0, 0.0, 0.0 };

    // First step
    size_t r = 3;
    if(BB[3][3] < BB[2][2]) r = 2;
    if(BB[r][r] < BB[1][1]) r = 1;
    if(BB[r][r] > BB[0][0])
    {
      const size_t tmp = p[0];
      p[0] = p[r];
      p[r] = tmp;
      swap_rows(*BB, 4, 4, 0, r);
      swap_cols(*BB, 4, 4, 0, r);
    }

    Ddiag[0] = BB[0][0];
    L[1][0] = BB[1][0] / Ddiag[0];
    L[2][0] = BB[2][0] / Ddiag[0];
    L[3][0] = BB[3][0] / Ddiag[0];

    BB[1][1] -= L[1][0] * BB[0][1];
    BB[2][1] -= L[1][0] * BB[0][2];
    BB[1][2] = BB[2][1];
    BB[3][1] -= L[1][0] * BB[0][3];
    BB[1][3] = BB[3][1];
    BB[2][2] -= L[2][0] * BB[0][2];
    BB[3][2] -= L[2][0] * BB[0][3];
    BB[2][3] = BB[3][2];
    BB[3][3] -= L[3][0] * BB[0][3];

    // Second step
    r = 3;
    if(BB[3][3] < BB[2][2]) r = 2;
    if(BB[r][r] > BB[1][1])
    {
      const size_t tmp = p[1];
      p[1] = p[r];
      p[r] = tmp;
      swap_rows(*BB, 4, 4, 1, r);
      swap_cols(*BB, 4, 4, 1, r);
      swap_rows(*L, 4, 4, 1, r);
      swap_cols(*L, 4, 4, 1, r);
    }

    Ddiag[1] = BB[1][1];
    L[2][1] = BB[2][1] / Ddiag[1];
    L[3][1] = BB[3][1] / Ddiag[1];
    BB[2][2] -= L[2][1] * BB[1][2];
    BB[3][2] -= L[2][1] * BB[1][3];
    BB[2][3] = BB[3][2];
    BB[3][3] -= L[3][1] * BB[1][3];

    // Third step
    if(BB[2][2] < BB[3][3])
    {
      Ddiag[2] = BB[3][3];
      swap_rows(*BB, 4, 4, 2, 3);
      swap_cols(*BB, 4, 4, 2, 3);
      swap_rows(*L, 4, 4, 2, 3);
      swap_cols(*L, 4, 4, 2, 3);
      const size_t tmp = p[2];
      p[2] = p[3];
      p[3] = tmp;
    }
    else
    {
      Ddiag[2] = BB[2][2];
    }

    L[3][2] = BB[3][2] / Ddiag[2];
    v[0] = L[1][0] * L[3][1] + L[2][0] * L[3][2] - L[1][0] * L[3][2] * L[2][1] - L[3][0];
    v[1] = L[3][2] * L[2][1] - L[3][1];
    v[2] = -L[3][2];
    v[3] = 1.0;
    normalize_array(v, 4);

    TYPE temp[4];
    copy(temp, v, 4);
    for(size_t i = 0; i < 4; i++) v[p[i]] = temp[i];
  }
  else
  {
    // LDL
    TYPE BB[4][4];
    copy(*BB, *B, 16);
    for(size_t i = 0; i < 4; i++)
    {
      for(size_t j = 0; j < 4; j++) BB[i][j] = -BB[i][j];
      BB[i][i] += x;
    }

    size_t p[4] = { 0, 1, 2, 3 };
    TYPE L[4][4]
        = { { 1.0, 0.0, 0.0, 0.0 }, { 0.0, 1.0, 0.0, 0.0 }, { 0.0, 0.0, 1.0, 0.0 }, { 0.0, 0.0, 0.0, 1.0 } };
    TYPE D[4][4]
        = { { 0.0, 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0, 0.0 } };

    // First step
    size_t r = 3;
    if(BB[3][3] < BB[2][2]) r = 2;
    if(BB[r][r] < BB[1][1]) r = 1;
    if(BB[r][r] > BB[0][0])
    {
      const size_t tmp = p[0];
      p[0] = p[r];
      p[r] = tmp;
      swap_rows(*BB, 4, 4, 0, r);
      swap_cols(*BB, 4, 4, 0, r);
    }

    D[0][0] = BB[0][0];
    L[1][0] = BB[1][0] / D[0][0];
    L[2][0] = BB[2][0] / D[0][0];
    L[3][0] = BB[3][0] / D[0][0];
    BB[1][1] -= L[1][0] * BB[0][1];
    BB[2][1] -= L[1][0] * BB[0][2];
    BB[1][2] = BB[2][1];
    BB[3][1] -= L[1][0] * BB[0][3];
    BB[1][3] = BB[3][1];
    BB[2][2] -= L[2][0] * BB[0][2];
    BB[3][2] -= L[2][0] * BB[0][3];
    BB[2][3] = BB[3][2];
    BB[3][3] -= L[3][0] * BB[0][3];

    // Second step
    r = 2;
    if(BB[2][2] < BB[1][1]) r = 1;
    if(BB[r][r] > BB[0][0])
    {
      const size_t tmp = p[1];
      p[1] = p[r];
      p[r] = tmp;
      swap_rows(*BB, 4, 4, 1, r);
      swap_cols(*BB, 4, 4, 1, r);
      swap_rows(*L, 4, 4, 1, r);
      swap_cols(*L, 4, 4, 1, r);
    }

    D[1][1] = BB[1][1];
    L[2][1] = BB[2][1] / D[1][1];
    L[3][1] = BB[3][1] / D[1][1];
    D[2][2] = BB[2][2] - L[2][1] * BB[1][2];
    D[3][2] = BB[3][2] - L[2][1] * BB[1][3];
    D[2][3] = D[3][2];
    D[3][3] = BB[3][3] - L[3][1] * BB[1][3];

    const TYPE DD = D[2][2] * D[3][3] - D[2][3] * D[2][3];
    if(DD == 0)
    {
      const TYPE max_abs = fmax(fmax(ABS(D[2][2]), ABS(D[2][3])), ABS(D[3][3]));
      if(max_abs == 0)
      {
        v[0] = L[1][0] * L[3][1] - L[3][0];
        v[1] = -L[3][1];
        v[2] = 0.0;
        v[3] = 1.0;
      }
      else
      {
        TYPE nullspace[2];
        compute_null_space(nullspace, D[2][2], D[2][3], D[3][3]);
        v[3] = nullspace[1];
        v[2] = nullspace[0] - L[3][2] * v[3];
        v[1] = -L[2][1] * v[2] - L[3][1] * v[3];
        v[0] = -L[1][0] * v[1] - L[2][0] * v[2] - L[3][0] * v[3];
      }
      normalize_array(v, 4);
    }
    else
    {
      const TYPE ID[2][2] = { { D[3][3], -D[2][3] }, { -D[2][3], D[2][2] } };

      if(subspa)
      {
        TYPE vmat[4][2] = { { L[1][0] * L[2][1] - L[2][0], L[1][0] * L[3][1] - L[3][0] },
                            { -L[2][1], -L[3][1] },
                            { 1.0, 0.0 },
                            { 0.0, 1.0 } };
        TYPE IL[4][4] = { { 1.0, 0.0, 0.0, 0.0 },
                          { -L[1][0], 1.0, 0.0, 0.0 },
                          { vmat[0][0], vmat[1][0], vmat[2][0], vmat[3][0] },
                          { vmat[0][1], vmat[1][1], vmat[2][1], vmat[3][1] } };

        {
          TYPE col0[4] = { vmat[0][0], vmat[1][0], vmat[2][0], vmat[3][0] };
          TYPE col1[4] = { vmat[0][1], vmat[1][1], vmat[2][1], vmat[3][1] };
          normalize_array(col0, 4);
          const TYPE proj = vector_scalar(col0, col1, 4);
          for(size_t i = 0; i < 4; i++) col1[i] -= proj * col0[i];
          normalize_array(col1, 4);
          for(size_t i = 0; i < 4; i++)
          {
            vmat[i][0] = col0[i];
            vmat[i][1] = col1[i];
          }
        }

        for(size_t it = 0; it < 2; it++)
        {
          TYPE tmp[4][2] = { { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.0, 0.0 } };
          TYPE tmp2[4][2] = { { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.0, 0.0 }, { 0.0, 0.0 } };

          for(size_t i = 0; i < 4; i++)
            for(size_t j = 0; j < 2; j++)
              for(size_t k = 0; k < 4; k++) tmp[i][j] += IL[i][k] * vmat[k][j];

          for(size_t j = 0; j < 2; j++)
          {
            tmp[0][j] /= D[0][0];
            tmp[1][j] /= D[1][1];
            const TYPE t0 = tmp[2][j];
            const TYPE t1 = tmp[3][j];
            tmp[2][j] = (ID[0][0] * t0 + ID[0][1] * t1) / DD;
            tmp[3][j] = (ID[1][0] * t0 + ID[1][1] * t1) / DD;
          }

          for(size_t i = 0; i < 4; i++)
            for(size_t j = 0; j < 2; j++)
              for(size_t k = 0; k < 4; k++) tmp2[i][j] += IL[k][i] * tmp[k][j];

          copy(*vmat, *tmp2, 8);
        }

        {
          TYPE col0[4] = { vmat[0][0], vmat[1][0], vmat[2][0], vmat[3][0] };
          TYPE col1[4] = { vmat[0][1], vmat[1][1], vmat[2][1], vmat[3][1] };
          normalize_array(col0, 4);
          const TYPE proj = vector_scalar(col0, col1, 4);
          for(size_t i = 0; i < 4; i++) col1[i] -= proj * col0[i];
          normalize_array(col1, 4);
          for(size_t i = 0; i < 4; i++)
          {
            vmat[i][0] = col0[i];
            vmat[i][1] = col1[i];
          }
        }

        TYPE HL[2][4] = { { 0.0, 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0, 0.0 } };
        TYPE HD[2][4] = { { 0.0, 0.0, 0.0, 0.0 }, { 0.0, 0.0, 0.0, 0.0 } };
        TYPE Hsmall[2][2] = { { 0.0, 0.0 }, { 0.0, 0.0 } };

        for(size_t i = 0; i < 2; i++)
          for(size_t j = 0; j < 4; j++)
            for(size_t k = 0; k < 4; k++) HL[i][j] += vmat[k][i] * L[k][j];

        for(size_t i = 0; i < 2; i++)
          for(size_t j = 0; j < 4; j++)
            for(size_t k = 0; k < 4; k++) HD[i][j] += HL[i][k] * D[k][j];

        for(size_t i = 0; i < 2; i++)
          for(size_t j = 0; j < 2; j++)
            for(size_t k = 0; k < 4; k++) Hsmall[i][j] -= HD[i][k] * HL[j][k];

        if(ABS(Hsmall[0][1]) < 1.0e-15)
        {
          for(size_t i = 0; i < 4; i++) v[i] = vmat[i][Hsmall[0][0] > Hsmall[0][1] ? 0 : 1];
        }
        else
        {
          const TYPE ratio = (Hsmall[0][0] - Hsmall[1][1]) / (2.0 * Hsmall[0][1]);
          const TYPE alpha = ratio + (Hsmall[0][1] < 0 ? -1.0 : 1.0) * SQRT(1.0 + ratio * ratio);
          for(size_t i = 0; i < 4; i++) v[i] = vmat[i][0] * alpha + vmat[i][1];
          normalize_array(v, 4);
        }
      }
      else
      {
        v[0] = L[1][0] * L[3][1] + L[2][0] * L[3][2] - L[1][0] * L[3][2] * L[2][1] - L[3][0];
        v[1] = L[3][2] * L[2][1] - L[3][1];
        v[2] = -L[3][2];
        v[3] = 1.0;

        TYPE IL[4][4] = { { 1.0, 0.0, 0.0, 0.0 },
                          { -L[1][0], 1.0, 0.0, 0.0 },
                          { L[1][0] * L[2][1] - L[2][0], -L[2][1], 1.0, 0.0 },
                          { v[0], v[1], v[2], v[3] } };

        normalize_array(v, 4);

        for(size_t it = 0; it < (size_t)nit; it++)
        {
          TYPE tmp[4] = { 0.0, 0.0, 0.0, 0.0 };
          TYPE tmp2[4] = { 0.0, 0.0, 0.0, 0.0 };

          for(size_t i = 0; i < 4; i++)
            for(size_t k = 0; k < 4; k++) tmp[i] += IL[i][k] * v[k];

          tmp[0] /= D[0][0];
          tmp[1] /= D[1][1];
          {
            const TYPE t0 = tmp[2];
            const TYPE t1 = tmp[3];
            tmp[2] = (ID[0][0] * t0 + ID[0][1] * t1) / DD;
            tmp[3] = (ID[1][0] * t0 + ID[1][1] * t1) / DD;
          }

          for(size_t i = 0; i < 4; i++)
            for(size_t k = 0; k < 4; k++) tmp2[i] += IL[k][i] * tmp[k];

          copy(v, tmp2, 4);
          normalize_array(v, 4);
        }
      }

      TYPE temp[4];
      copy(temp, v, 4);
      for(size_t i = 0; i < 4; i++) v[p[i]] = temp[i];
    }
  }

  // Polar factor (up to sign).
  const TYPE v22 = 2.0 * v[1] * v[1];
  const TYPE v33 = 2.0 * v[2] * v[2];
  const TYPE v44 = 2.0 * v[3] * v[3];
  const TYPE v23 = 2.0 * v[1] * v[2];
  const TYPE v14 = 2.0 * v[0] * v[3];
  const TYPE v24 = 2.0 * v[1] * v[3];
  const TYPE v13 = 2.0 * v[0] * v[2];
  const TYPE v12 = 2.0 * v[0] * v[1];
  const TYPE v34 = 2.0 * v[2] * v[3];

  Q[0][0] = 1.0 - v33 - v44;
  Q[0][1] = v23 + v14;
  Q[0][2] = v24 - v13;
  Q[1][0] = v23 - v14;
  Q[1][1] = 1.0 - v22 - v44;
  Q[1][2] = v12 + v34;
  Q[2][0] = v13 + v24;
  Q[2][1] = v34 - v12;
  Q[2][2] = 1.0 - v22 - v33;

  if(d == -1) for(size_t i = 0; i < 3; i++) for(size_t j = 0; j < 3; j++) Q[i][j] = -Q[i][j];

  TYPE QT[3][3];
  for(size_t i = 0; i < 3; i++)
    for(size_t j = 0; j < 3; j++) QT[j][i] = Q[i][j];

  matrix_multiply(*QT, *A, *H, 3, 3, 3);
  for(size_t i = 0; i < 9; i++)
  {
    (*H)[i] *= norm;
    (*A)[i] *= norm;
  }
}

//==============================================================================

void abs_matrix(TYPE *matrix_abs, size_t num_el)
{
  int i, j;

  for(i = 0; i < 2; i++)
  {
    matrix_abs[i] = ABS(matrix_abs[i]);
    // printf("%f\n", matricella[i][j] );
  }
  // printf("ok 3\n");
  printf("\n");
};

void matrix_multiply(const TYPE *A, const TYPE *B, TYPE *RES, int rows_A, int cols_A, int cols_B)
{
  TYPE product;

  // k is the row of A that is being multiplied
  for(size_t k = 0; k < rows_A; ++k)
  {
    // is has to be multiplied to each of the columns of B
    for(size_t j = 0; j < cols_B; ++j)
    {
      // use a separate variable to be sure to start from 0 with the product
      product = 0;
      for(size_t i = 0; i < cols_A; ++i)
      {
        product += A[cols_A * k + i] * B[j + cols_B * i];
      }
      RES[k * cols_B + j] = product;
    }
  }
}

TYPE max_val_matrix(TYPE *matrice, size_t size)
{
  TYPE max;

  for(size_t i = 0; i < size; i++)
  {
    if(matrice[i] > max)
    {
      max = matrice[i];
    }
  }
  return max;
}

static inline void normalize_array(TYPE *vector, int size)
{
  int ii;
  TYPE norm = 0;

  // computing the square of the norm of v
  for(ii = 0; ii < 4; ii++)
  {
    norm += vector[ii] * vector[ii];
  }
  // squared root of the norm of v
  norm = SQRT(norm);

  // printf("norm is: %f\n", norm);

  // dividing each element of v for the norm
  for(ii = 0; ii < size; ii++)
  {
    vector[ii] /= norm;
  }
}

void swap_cols(TYPE *m, size_t rows, size_t cols, size_t col0, size_t col1)
{
  // vector with the same size of one row input matrix (that is the number of columns)
  TYPE *temp = (TYPE *)calloc(cols, sizeof(TYPE));

  for(int i = 0; i < rows; i++)
  {
    temp[i] = m[col0 + cols * i];
    m[col0 + cols * i] = m[col1 + cols * i];
    m[col1 + cols * i] = temp[i];
  }
  free(temp);
}

void swap_rows(TYPE *m, size_t rows, size_t cols, size_t row0, size_t row1)
{
  // vector with the same size of one row input matrix (that is the number of columns)
  TYPE *temp = (TYPE *)calloc(cols, sizeof(TYPE));

  for(int i = 0; i < cols; i++)
  {
    temp[i] = m[cols * row0 + i];
    m[cols * row0 + i] = m[cols * row1 + i];
    m[cols * row1 + i] = temp[i];
  }
  free(temp);
}

void stampa_matrice(TYPE *m, size_t rows, size_t cols)
{
  int i;

  for(i = 0; i < rows * cols; i++)
  {
    printf("%f ", m[i]);
    if(i % cols == cols - 1)
    {
      printf("\n");
    }
  }
  // un bel a capo prima di chiudere
  printf("\n");
}

void compute_null_space(TYPE *nullspace, const TYPE a, const TYPE b, const TYPE c)
{
  // chceck that determinant is zero and do something about it
  if(a * c - b * b == 0)
  {
  }

  if(a != 0)
  {
    nullspace[0] = b;
    nullspace[1] = -a;
  }
  else
  {
    // check on these too
    // assert(a == 0);
    // assert(b == 0);
    // assert(c != 0);
    nullspace[0] = c;
    nullspace[1] = -b;
  }
}

void copy(TYPE *dest, TYPE *source, size_t num_el)
{
  for(size_t psi = 0; psi < num_el; psi++)
  {
    dest[psi] = source[psi];
  }
}

void orthonormalize_v_with_qr(TYPE *v0, TYPE *v1, const TYPE v00, const TYPE v10, const TYPE v01, const TYPE v11)
{
  v0[0] = v00;
  v0[1] = v01;
  v0[2] = 1;
  v0[3] = 0;
  normalize_array(v0, 4);

  v1[0] = v10 + v01 * v01 * v10 - v00 * v01 * v11;
  v1[1] = v11 - v00 * v01 * v10 + v00 * v00 * v11;
  v1[2] = -v00 * v10 - v01 * v11;
  v1[3] = v00 * v00 + v01 * v01 + 1;
  normalize_array(v1, 4);
}

void multiply_il_v(TYPE *RES, const TYPE IL01, const TYPE IL02, const TYPE IL03, const TYPE IL12, const TYPE IL13,
                   const TYPE *v)
{
  RES[0] = v[0];
  RES[1] = v[0] * IL01 + v[1];
  RES[2] = v[0] * IL02 + v[1] * IL12 + v[2];
  RES[3] = v[0] * IL03 + v[1] * IL13 + v[3];
}

// ID is 2x2
void multiply_id_v(TYPE *RES, const TYPE ID00, const TYPE ID11, const TYPE *ID, const TYPE *v)
{
  RES[0] = v[0] * ID00;
  RES[1] = v[1] * ID11;
  RES[2] = v[2] * ID[0] + v[3] * ID[1];
  RES[3] = v[2] * ID[2] + v[3] * ID[3];
}

void multiply_v_il(TYPE *RES, const TYPE *v, const TYPE IL01, const TYPE IL02, const TYPE IL03, const TYPE IL12,
                   const TYPE IL13)
{
  RES[0] = v[0] + v[1] * IL01 + v[2] * IL02 + v[3] * IL03;
  RES[1] = v[1] + v[2] * IL12 + v[3] * IL13;
  RES[2] = v[2];
  RES[3] = v[3];
}

void orthonormalize_v_with_qr_single(TYPE *v0, TYPE *v1)
{
  // The factorization was obtained symbolically by WolframAlpha
  // by running the following query:
  //
  // QRDecomposition[{{a,e},{b,f},{c,g},{d,h}}]

  normalize_array(v0, 4);

  // To avoid numerical stability issues when multiplying too big values in the solution,
  // we scale down the second vector.
  TYPE factor = v1[0];
  if(factor < ABS(v1[1])) factor = ABS(v1[1]);
  if(factor < ABS(v1[2])) factor = ABS(v1[2]);
  if(factor < ABS(v1[3])) factor = ABS(v1[3]);
  // TODO: find and equivalent in C
  // factor = 1 / (factor + std::numeric_limits<TReal>::min());
  factor = 1 / (factor);

  const TYPE a = v0[0];
  const TYPE b = v0[1];
  const TYPE c = v0[2];
  const TYPE d = v0[3];
  const TYPE e = v1[0] * factor;
  const TYPE f = v1[1] * factor;
  const TYPE g = v1[2] * factor;
  const TYPE h = v1[3] * factor;

  // OPTME: We could reuse some of the multiplications.
  // ANSME: Is there a more numerically stable way to do this?
  v1[0] = b * b * e + c * c * e + d * d * e - a * b * f - a * c * g - a * d * h;
  v1[1] = -a * b * e + a * a * f + c * c * f + d * d * f - b * c * g - b * d * h;
  v1[2] = -a * c * e - b * c * f + a * a * g + b * b * g + d * d * g - c * d * h;
  v1[3] = -a * d * e - b * d * f - c * d * g + a * a * h + b * b * h + c * c * h;
  normalize_array(v1, 4);
}

void multiply_minus_v_d(TYPE *RES, TYPE *v, TYPE *D)
{
  RES[0] = -v[0] * D[0];
  RES[1] = -v[1] * D[5];
  RES[2] = -v[2] * D[5] - v[3] * D[14];
  RES[3] = -v[2] * D[11] - v[3] * D[15];
}

// scalar product of two vectors
TYPE vector_scalar(const TYPE *a, const TYPE *b, const size_t num_el)
{
  TYPE somma = { 0.0 };
  for(size_t jod = 0; jod < num_el; jod++)
  {
    somma += a[jod] * b[jod];
  }
  return somma;
}
