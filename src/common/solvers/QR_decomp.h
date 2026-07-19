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
 * @brief QR decomposition helpers used by the experimental SVD and polar decomposition code.
 *
 * The implementation uses a classical Gram-Schmidt orthonormalization on the
 * column vectors of the input matrix. This file is currently header-only
 * because the prototype code was imported as a self-contained module.
 */

#include <math.h>
#include <stdio.h>

/**
 * @brief Decompose a dense matrix into an orthonormal basis and an upper-triangular factor.
 *
 * The input matrix is interpreted as a row-major @p rows x @p cols array. The
 * implementation copies one column at a time into @p Q, removes the projection
 * on the previously orthonormalized columns, then stores the corresponding
 * coefficients into @p R.
 *
 * @param[in] A Input matrix stored row-major.
 * @param[out] Q Output orthonormal basis, stored row-major with the same shape as @p A.
 * @param[out] R Output upper-triangular matrix, stored row-major as a @p cols x @p cols matrix.
 * @param[in] rows Number of rows in @p A and @p Q.
 * @param[in] cols Number of columns in @p A, @p Q and @p R.
 */
inline void QR_dec(double *A, double *Q, double *R, int rows, int cols)
{
  // The function decomposes the input matrix A into the matrices Q and R: one
  // simmetric, one orthonormal and one upper triangular, by using the
  // Gram-Schmidt method. The input matrice A is defined as A[rows][cols], so
  // are the output matrices Q and R. This function is meant to be used in the
  // polar decomposition algorithm and has been tested with different sizes of
  // input matrices. Supposedly the algorithm works with any matrix, as long as
  // the columns vectors are independent.
  //
  // For tests for the standalone function please refer to the original github
  // repo this has been developed in.
  // https://github.com/DavidePatria/QR_decomposition_C/blob/main/README.md

  // As already mentioned in the README the matrices orders are: A mxn => Q mxn
  // , R nxn and rank(A) must be n The matrix A[m x n] = [A_00, A_01, ... A_0n;
  // ...... ; A_m0, ... , A_mn] can be accessed as a vector that has all its
  // rows consecutively written in a long vector, even if passed as a *A and
  // defined as A[m][n].
  //
  // If A(mxn) has m<n the function still returs R(mxn) and Q(nxn), but it is
  // enough to get the submatrices Q(mxm) and R(mxn) as a valid decomposition.
  // This is what also octave does.

  // vectors for internal coputations
  double T[rows];
  double S[rows];
  double norm;
  int i, ii, j, jj, k, kk;
  double r;

  for(i = 0; i < cols; i++)
  {
    printf("\n");

    // scrolling a column and copying it
    for(ii = 0; ii < rows; ii++)
    {
      Q[ii * cols + i] = A[ii * cols + i];
    }

    for(j = 0; j < i; j++)
    {

      // copying columns into auxiliary variables
      for(jj = 0; jj < rows; jj++)
      {
        T[jj] = Q[cols * jj + j];
        S[jj] = A[cols * jj + i];
      }

      // temporary storing T*K in r
      r = 0;
      for(k = 0; k < rows; k++)
      {
        r += T[k] * S[k];
      }

      // setting R[j][i] to r
      R[cols * j + i] = r;

      for(kk = 0; kk < rows; kk++)
      {
        // multiplying vector T by r
        T[kk] *= r;
        // subtract T[kk] from i-th column of Q
        Q[cols * kk + i] -= T[kk];
      }
    }

    // rezeroing norm at each cycle
    norm = 0;
    // norm of the i-th column
    for(k = 0; k < rows; k++)
    {
      // computing norm^2
      norm += Q[cols * k + i] * Q[cols * k + i];
    }
    norm = sqrt(norm);

    // assigning i-th element of R diagonal
    R[cols * i + i] = norm;

    for(k = 0; k < rows; k++)
    {
      Q[cols * k + i] /= R[cols * i + i];
    }
  }
}
