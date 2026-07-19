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
 * @brief Small standalone test program for the experimental polar decomposition code.
 *
 * The program feeds a few hard-coded 3x3 matrices into
 * ::polar_decomposition(), prints the resulting factors, and checks that the
 * recovered orthogonal matrix has nearly orthogonal rows.
 */

#include "polar_decomposition.h"
#include <stdio.h>

/** @brief Print one scalar diagnostic produced by the orthogonality check. */
void print_float(TYPE num);

/** @brief Print the pairwise row dot products of a 3x3 matrix. */
void check_orthonormal(TYPE Q[3][3]);

/** @brief Print a 3x3 matrix in row-major order. */
void stampa_3x3(TYPE Q[3][3]);

/** @brief Copy a flat buffer used to initialize test matrices. */
void copy(TYPE *dest, TYPE *source, size_t num_el);

#ifndef TEST_N
#define TEST_N 1
#endif

int main()
{

  TYPE A[3][3];
  // simple case one
  switch(TEST_N)
  {
    case 1:
    {
      TYPE temp[3][3] = { { 1, 2, 3 }, { 4, 5, 6 }, { 7, 8, 9 } };
      copy(*A, *temp, 9);
    }
    case 2:
    {
      TYPE temp[3][3] = { { 0.1, 0.1, 0.3 }, { 0.2, 0.1, 0.2 }, { 0.3, 0.0, 0.1 } };
      copy(*A, *temp, 9);
    }
    // TODO: check and use here the maledict test mentioned in the cpp version
    case 3:
    {
      TYPE temp[3][3] = { { -1, 2, 0 }, { 1.3, 0.4, 3.4 }, { 0.3, 2.4, 3.4 } };
      copy(*A, *temp, 9);
    }
  }

  TYPE Q[3][3];
  TYPE H[3][3];

  // stampa_3x3(A);
  polar_decomposition(A, Q, H);

  // stampa_3x3(A);
  stampa_3x3(Q);
  stampa_3x3(H);

  matrix_multiply(*(Q), *(H), *(A), 3, 3, 3);

  // stampa_3x3(A);

  check_orthonormal(Q);

  return 0;
}

/** @brief Print a 3x3 matrix in row-major order. */
void stampa_3x3(TYPE Q[3][3])
{
  for(size_t riga = 0; riga < 3; riga++)
  {
    for(size_t col = 0; col < 3; col++)
    {
      printf("%f ", Q[riga][col]);
      if(col == 2) printf("\n");
    }
  }
  printf("\n");
}

/** @brief Print the row-wise orthogonality residuals of a 3x3 matrix. */
void check_orthonormal(TYPE Q[3][3])
{
  TYPE scalar01 = 0;
  TYPE scalar12 = 0;
  TYPE scalar02 = 0;

  for(size_t i = 0; i < 3; i++)
  {
    scalar01 += Q[0][i] * Q[1][i];
    scalar12 += Q[1][i] * Q[2][i];
    scalar02 += Q[0][i] * Q[2][i];
  }
  print_float(scalar01);
  print_float(scalar12);
  print_float(scalar02);
}

/** @brief Print one scalar residual. */
void print_float(TYPE num)
{
  printf("scalar12: %f\n", num);
}
