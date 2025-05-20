/*
    This file is part of darktable,
    Copyright (C) 2020 darktable developers,
    Copyright (C) 2025 Guillaume Stutin.

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

#include "common/colorspaces.h"
#include "common/file_location.h"
#include "darktable.h"

#include <glib.h>
#include <inttypes.h>
#include <lcms2.h>

/**
 * These are the CIELab values of Color Checker reference targets
 */


// types of targets we support
typedef enum dt_color_checker_targets
{
  COLOR_CHECKER_XRITE_24_2000 = 0,
  COLOR_CHECKER_XRITE_24_2014 = 1,
  COLOR_CHECKER_SPYDER_24 = 2,
  COLOR_CHECKER_SPYDER_24_V2 = 3,
  COLOR_CHECKER_SPYDER_48 = 4,
  COLOR_CHECKER_SPYDER_48_V2 = 5,
  COLOR_CHECKER_USER_REF = 6,
  COLOR_CHECKER_LAST
} dt_color_checker_targets;

// helper to deal with patch color
typedef struct dt_color_checker_patch
{
  char *name;             // mnemonic name for the patch
  dt_aligned_pixel_t Lab; // reference color in CIE Lab

  // (x, y) position of the patch center, relatively to the guides (white dots)
  // in ratio of the grid dimension along that axis
  struct
  {
    float x;
    float y;
  };
} dt_color_checker_patch;

typedef struct dt_color_checker_t
{
  char *name;
  char *author;
  char *date;
  char *manufacturer;
  dt_color_checker_targets type;

  float ratio;                     // format ratio of the chart, guide to guide (white dots)
  float radius;                    // radius of a patch in ratio of the checker diagonal
  size_t patches;                  // number of patches in target
  size_t size[2];                  // dimension along x, y axes
  size_t middle_grey;              // index of the closest patch to 20% neutral grey
  size_t white;                    // index of the closest patch to pure white
  size_t black;                    // index of the closest patch to pure black
  dt_color_checker_patch values[]; // array of colors
} dt_color_checker_t;

dt_color_checker_t xrite_24_2000 = { .name = "Xrite ColorChecker 24 before 2014",
                                     .author = "X-Rite",
                                     .date = "3/27/2000",
                                     .manufacturer = "X-Rite/Gretag Macbeth",
                                     .type = COLOR_CHECKER_XRITE_24_2000,
                                     .radius = 0.055f,
                                     .ratio = 2.f / 3.f,
                                     .patches = 24,
                                     .size = { 4, 6 },
                                     .middle_grey = 21,
                                     .white = 18,
                                     .black = 23,
                                     .values = { { "A1", { 37.986, 13.555, 14.059 }, { 0.087, 0.125 } },
                                                 { "A2", { 65.711, 18.13, 17.81 }, { 0.250, 0.125 } },
                                                 { "A3", { 49.927, -04.88, -21.905 }, { 0.417, 0.125 } },
                                                 { "A4", { 43.139, -13.095, 21.905 }, { 0.584, 0.125 } },
                                                 { "A5", { 55.112, 08.844, -25.399 }, { 0.751, 0.125 } },
                                                 { "A6", { 70.719, -33.397, -0.199 }, { 0.918, 0.125 } },
                                                 { "B1", { 62.661, 36.067, 57.096 }, { 0.087, 0.375 } },
                                                 { "B2", { 40.02, 10.41, -45.964 }, { 0.250, 0.375 } },
                                                 { "B3", { 51.124, 48.239, 16.248 }, { 0.417, 0.375 } },
                                                 { "B4", { 30.325, 22.976, -21.587 }, { 0.584, 0.375 } },
                                                 { "B5", { 72.532, -23.709, 57.255 }, { 0.751, 0.375 } },
                                                 { "B6", { 71.941, 19.363, 67.857 }, { 0.918, 0.375 } },
                                                 { "C1", { 28.778, 14.179, -50.297 }, { 0.087, 0.625 } },
                                                 { "C2", { 55.261, -38.342, 31.37 }, { 0.250, 0.625 } },
                                                 { "C3", { 42.101, 53.378, 28.19 }, { 0.417, 0.625 } },
                                                 { "C4", { 81.733, 04.039, 79.819 }, { 0.584, 0.625 } },
                                                 { "C5", { 51.935, 49.986, -14.574 }, { 0.751, 0.625 } },
                                                 { "C6", { 51.038, -28.631, -28.638 }, { 0.918, 0.625 } },
                                                 { "D1", { 96.539, -0.425, 1.186 }, { 0.087, 0.875 } },
                                                 { "D2", { 81.257, -0.638, -0.335 }, { 0.250, 0.875 } },
                                                 { "D3", { 66.766, -0.734, -0.504 }, { 0.417, 0.875 } },
                                                 { "D4", { 50.867, -0.153, -0.27 }, { 0.584, 0.875 } },
                                                 { "D5", { 35.656, -0.421, -1.231 }, { 0.751, 0.875 } },
                                                 { "D6", { 20.461, -0.079, -0.973 }, { 0.918, 0.875 } } } };


dt_color_checker_t xrite_24_2014 = {
  .name = "Xrite ColorChecker 24 after 2014",
  .author = "X-Rite",
  .date = "3/28/2015",
  .manufacturer = "X-Rite/Gretag Macbeth",
  .type = COLOR_CHECKER_XRITE_24_2014,
  .radius = 0.055f,
  .ratio = 2.f / 3.f,
  .patches = 24,
  .size = { 4, 6 },
  .middle_grey = 21,
  .white = 18,
  .black = 23,
  .values
  = { { "A1", { 37.54, 14.37, 14.92 }, { 0.087, 0.125 } },   { "A2", { 64.66, 19.27, 17.50 }, { 0.250, 0.125 } },
      { "A3", { 49.32, -03.82, -22.54 }, { 0.417, 0.125 } }, { "A4", { 43.46, -12.74, 22.72 }, { 0.584, 0.125 } },
      { "A5", { 54.94, 09.61, -24.79 }, { 0.751, 0.125 } },  { "A6", { 70.48, -32.26, -00.37 }, { 0.918, 0.125 } },
      { "B1", { 62.73, 35.83, 56.50 }, { 0.087, 0.375 } },   { "B2", { 39.43, 10.75, -45.17 }, { 0.250, 0.375 } },
      { "B3", { 50.57, 48.64, 16.67 }, { 0.417, 0.375 } },   { "B4", { 30.10, 22.54, -20.87 }, { 0.584, 0.375 } },
      { "B5", { 71.77, -24.13, 58.19 }, { 0.751, 0.375 } },  { "B6", { 71.51, 18.24, 67.37 }, { 0.918, 0.375 } },
      { "C1", { 28.37, 15.42, -49.80 }, { 0.087, 0.625 } },  { "C2", { 54.38, -39.72, 32.27 }, { 0.250, 0.625 } },
      { "C3", { 42.43, 51.05, 28.62 }, { 0.417, 0.625 } },   { "C4", { 81.80, 02.67, 80.41 }, { 0.584, 0.625 } },
      { "C5", { 50.63, 51.28, -14.12 }, { 0.751, 0.625 } },  { "C6", { 49.57, -29.71, -28.32 }, { 0.918, 0.625 } },
      { "D1", { 95.19, -01.03, 02.93 }, { 0.087, 0.875 } },  { "D2", { 81.29, -00.57, 00.44 }, { 0.250, 0.875 } },
      { "D3", { 66.89, -00.75, -00.06 }, { 0.417, 0.875 } }, { "D4", { 50.76, -00.13, 00.14 }, { 0.584, 0.875 } },
      { "D5", { 35.63, -00.46, -00.48 }, { 0.751, 0.875 } }, { "D6", { 20.64, 00.07, -00.46 }, { 0.918, 0.875 } } }
};


// dimensions between reference dots : 197 mm width x 135 mm height
// patch width : 26x26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm
dt_color_checker_t spyder_24 = {
  .name = "Datacolor SpyderCheckr 24 before 2018",
  .author = "Aur\303\251lien PIERRE",
  .date = "dec, 9 2016",
  .manufacturer = "DataColor",
  .type = COLOR_CHECKER_SPYDER_24,
  .ratio = 2.f / 3.f,
  .radius = 0.035,
  .patches = 24,
  .size = { 4, 6 },
  .middle_grey = 03,
  .white = 00,
  .black = 05,
  .values
  = { { "A1", { 96.04, 2.16, 2.60 }, { 0.107, 0.844 } },     { "A2", { 80.44, 1.17, 2.05 }, { 0.264, 0.844 } },
      { "A3", { 65.52, 0.69, 1.86 }, { 0.421, 0.844 } },     { "A4", { 49.62, 0.58, 1.56 }, { 0.579, 0.844 } },
      { "A5", { 33.55, 0.35, 1.40 }, { 0.736, 0.844 } },     { "A6", { 16.91, 1.43, -0.81 }, { 0.893, 0.844 } },
      { "B1", { 47.12, -32.50, -28.75 }, { 0.107, 0.615 } }, { "B2", { 50.49, 53.45, -13.55 }, { 0.264, 0.615 } },
      { "B3", { 83.61, 3.36, 87.02 }, { 0.421, 0.615 } },    { "B4", { 41.05, 60.75, 31.17 }, { 0.579, 0.615 } },
      { "B5", { 54.14, -40.80, 34.75 }, { 0.736, 0.615 } },  { "B6", { 24.75, 13.78, -49.48 }, { 0.893, 0.615 } },
      { "C1", { 60.94, 38.21, 61.31 }, { 0.107, 0.385 } },   { "C2", { 37.80, 7.30, -43.04 }, { 0.264, 0.385 } },
      { "C3", { 49.81, 48.50, 15.76 }, { 0.421, 0.385 } },   { "C4", { 28.88, 19.36, -24.48 }, { 0.579, 0.385 } },
      { "C5", { 72.45, -23.60, 60.47 }, { 0.736, 0.385 } },  { "C6", { 71.65, 23.74, 72.28 }, { 0.893, 0.385 } },
      { "D1", { 70.19, -31.90, 1.98 }, { 0.107, 0.155 } },   { "D2", { 54.38, 8.84, -25.71 }, { 0.264, 0.155 } },
      { "D3", { 42.03, -15.80, 22.93 }, { 0.421, 0.155 } },  { "D4", { 48.82, -5.11, -23.08 }, { 0.579, 0.155 } },
      { "D5", { 65.10, 18.14, 18.68 }, { 0.736, 0.155 } },   { "D6", { 36.13, 14.15, 15.78 }, { 0.893, 0.155 } } }
};


// dimensions between reference dots : 197 mm width x 135 mm height
// patch width : 26x26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm
dt_color_checker_t spyder_24_v2 = {
  .name = "Datacolor SpyderCheckr 24 after 2018",
  .author = "Aur\303\251lien PIERRE",
  .date = "dec, 9 2016",
  .manufacturer = "DataColor",
  .type = COLOR_CHECKER_SPYDER_24_V2,
  .ratio = 2.f / 3.f,
  .radius = 0.035,
  .patches = 24,
  .size = { 4, 6 },
  .middle_grey = 03,
  .white = 00,
  .black = 05,
  .values
  = { { "A1", { 96.04, 2.16, 2.60 }, { 0.107, 0.844 } },     { "A2", { 80.44, 1.17, 2.05 }, { 0.264, 0.844 } },
      { "A3", { 65.52, 0.69, 1.86 }, { 0.421, 0.844 } },     { "A4", { 49.62, 0.58, 1.56 }, { 0.579, 0.844 } },
      { "A5", { 33.55, 0.35, 1.40 }, { 0.736, 0.844 } },     { "A6", { 16.91, 1.43, -0.81 }, { 0.893, 0.844 } },
      { "B1", { 47.12, -32.50, -28.75 }, { 0.107, 0.615 } }, { "B2", { 50.49, 53.45, -13.55 }, { 0.264, 0.615 } },
      { "B3", { 83.61, 3.36, 87.02 }, { 0.421, 0.615 } },    { "B4", { 41.05, 60.75, 31.17 }, { 0.579, 0.615 } },
      { "B5", { 54.14, -40.80, 34.75 }, { 0.736, 0.615 } },  { "B6", { 24.75, 13.78, -49.48 }, { 0.893, 0.615 } },
      { "C1", { 60.94, 38.21, 61.31 }, { 0.107, 0.385 } },   { "C2", { 37.80, 7.30, -43.04 }, { 0.264, 0.385 } },
      { "C3", { 49.81, 48.50, 15.76 }, { 0.421, 0.385 } },   { "C4", { 28.88, 19.36, -24.48 }, { 0.579, 0.385 } },
      { "C5", { 72.45, -23.57, 60.47 }, { 0.736, 0.385 } },  { "C6", { 71.65, 23.74, 72.28 }, { 0.893, 0.385 } },
      { "D1", { 70.19, -31.85, 1.98 }, { 0.107, 0.155 } },   { "D2", { 54.38, 8.84, -25.71 }, { 0.264, 0.155 } },
      { "D3", { 42.03, -15.78, 22.93 }, { 0.421, 0.155 } },  { "D4", { 48.82, -5.11, -23.08 }, { 0.579, 0.155 } },
      { "D5", { 65.10, 18.14, 18.68 }, { 0.736, 0.155 } },   { "D6", { 36.13, 14.15, 15.78 }, { 0.893, 0.155 } } }
};


// dimensions between reference dots : 297 mm width x 197 mm height
// patch width : 26x26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm
dt_color_checker_t spyder_48 = {
  .name = "Datacolor SpyderCheckr 48 before 2018",
  .author = "Aur\303\251lien PIERRE",
  .date = "dec, 9 2016",
  .manufacturer = "DataColor",
  .type = COLOR_CHECKER_SPYDER_48,
  .ratio = 2.f / 3.f,
  .radius = 0.035,
  .patches = 48,
  .size = { 8, 6 },
  .middle_grey = 24,
  .white = 21,
  .black = 29,
  .values
  = { { "A1", { 61.35, 34.81, 18.38 }, { 0.071, 0.107 } },   { "A2", { 75.50, 5.84, 50.42 }, { 0.071, 0.264 } },
      { "A3", { 66.82, -25.1, 23.47 }, { 0.071, 0.421 } },   { "A4", { 60.53, -22.6, -20.40 }, { 0.071, 0.579 } },
      { "A5", { 59.66, -2.03, -28.46 }, { 0.071, 0.736 } },  { "A6", { 59.15, 30.83, -5.72 }, { 0.071, 0.893 } },
      { "B1", { 82.68, 5.03, 3.02 }, { 0.175, 0.107 } },     { "B2", { 82.25, -2.42, 3.78 }, { 0.175, 0.264 } },
      { "B3", { 82.29, 2.20, -2.04 }, { 0.175, 0.421 } },    { "B4", { 24.89, 4.43, 0.78 }, { 0.175, 0.579 } },
      { "B5", { 25.16, -3.88, 2.13 }, { 0.175, 0.736 } },    { "B6", { 26.13, 2.61, -5.03 }, { 0.175, 0.893 } },
      { "C1", { 85.42, 9.41, 14.49 }, { 0.279, 0.107 } },    { "C2", { 74.28, 9.05, 27.21 }, { 0.279, 0.264 } },
      { "C3", { 64.57, 12.39, 37.24 }, { 0.279, 0.421 } },   { "C4", { 44.49, 17.23, 26.24 }, { 0.279, 0.579 } },
      { "C5", { 25.29, 7.95, 8.87 }, { 0.279, 0.736 } },     { "C6", { 22.67, 2.11, -1.10 }, { 0.279, 0.893 } },
      { "D1", { 92.72, 1.89, 2.76 }, { 0.384, 0.107 } },     { "D2", { 88.85, 1.59, 2.27 }, { 0.384, 0.264 } },
      { "D3", { 73.42, 0.99, 1.89 }, { 0.384, 0.421 } },     { "D4", { 57.15, 0.57, 1.19 }, { 0.384, 0.579 } },
      { "D5", { 41.57, 0.24, 1.45 }, { 0.384, 0.736 } },     { "D6", { 25.65, 1.24, 0.05 }, { 0.384, 0.893 } },
      { "E1", { 96.04, 2.16, 2.60 }, { 0.616, 0.107 } },     { "E2", { 80.44, 1.17, 2.05 }, { 0.616, 0.264 } },
      { "E3", { 65.52, 0.69, 1.86 }, { 0.616, 0.421 } },     { "E4", { 49.62, 0.58, 1.56 }, { 0.616, 0.579 } },
      { "E5", { 33.55, 0.35, 1.40 }, { 0.616, 0.736 } },     { "E6", { 16.91, 1.43, -0.81 }, { 0.616, 0.893 } },
      { "F1", { 47.12, -32.50, -28.75 }, { 0.721, 0.107 } }, { "F2", { 50.49, 53.45, -13.55 }, { 0.721, 0.264 } },
      { "F3", { 83.61, 3.36, 87.02 }, { 0.721, 0.421 } },    { "F4", { 41.05, 60.75, 31.17 }, { 0.721, 0.579 } },
      { "F5", { 54.14, -40.80, 34.75 }, { 0.721, 0.736 } },  { "F6", { 24.75, 13.78, -49.48 }, { 0.721, 0.893 } },
      { "G1", { 60.94, 38.21, 61.31 }, { 0.825, 0.107 } },   { "G2", { 37.80, 7.30, -43.04 }, { 0.825, 0.264 } },
      { "G3", { 49.81, 48.50, 15.76 }, { 0.825, 0.421 } },   { "G4", { 28.88, 19.36, -24.48 }, { 0.825, 0.579 } },
      { "G5", { 72.45, -23.60, 60.47 }, { 0.825, 0.736 } },  { "G6", { 71.65, 23.74, 72.28 }, { 0.825, 0.893 } },
      { "H1", { 70.19, -31.90, 1.98 }, { 0.929, 0.107 } },   { "H2", { 54.38, 8.84, -25.71 }, { 0.929, 0.264 } },
      { "H3", { 42.03, -15.80, 22.93 }, { 0.929, 0.421 } },  { "H4", { 48.82, -5.11, -23.08 }, { 0.929, 0.579 } },
      { "H5", { 65.10, 18.14, 18.68 }, { 0.929, 0.736 } },   { "H6", { 36.13, 14.15, 15.78 }, { 0.929, 0.893 } } }
};


// dimensions between reference dots : 297 mm width x 197 mm height
// patch width : 26x26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm
dt_color_checker_t spyder_48_v2 = {
  .name = "Datacolor SpyderCheckr 48 after 2018",
  .author = "Aur\303\251lien PIERRE",
  .date = "dec, 9 2016",
  .manufacturer = "DataColor",
  .type = COLOR_CHECKER_SPYDER_48_V2,
  .ratio = 2.f / 3.f,
  .radius = 0.035,
  .patches = 48,
  .size = { 8, 6 },
  .middle_grey = 24,
  .white = 21,
  .black = 29,
  .values
  = { { "A1", { 61.35, 34.81, 18.38 }, { 0.071, 0.107 } },   { "A2", { 75.50, 5.84, 50.42 }, { 0.071, 0.264 } },
      { "A3", { 66.82, -25.1, 23.47 }, { 0.071, 0.421 } },   { "A4", { 60.53, -22.62, -20.40 }, { 0.071, 0.579 } },
      { "A5", { 59.66, -2.03, -28.46 }, { 0.071, 0.736 } },  { "A6", { 59.15, 30.83, -5.72 }, { 0.071, 0.893 } },
      { "B1", { 82.68, 5.03, 3.02 }, { 0.175, 0.107 } },     { "B2", { 82.25, -2.42, 3.78 }, { 0.175, 0.264 } },
      { "B3", { 82.29, 2.20, -2.04 }, { 0.175, 0.421 } },    { "B4", { 24.89, 4.43, 0.78 }, { 0.175, 0.579 } },
      { "B5", { 25.16, -3.88, 2.13 }, { 0.175, 0.736 } },    { "B6", { 26.13, 2.61, -5.03 }, { 0.175, 0.893 } },
      { "C1", { 85.42, 9.41, 14.49 }, { 0.279, 0.107 } },    { "C2", { 74.28, 9.05, 27.21 }, { 0.279, 0.264 } },
      { "C3", { 64.57, 12.39, 37.24 }, { 0.279, 0.421 } },   { "C4", { 44.49, 17.23, 26.24 }, { 0.279, 0.579 } },
      { "C5", { 25.29, 7.95, 8.87 }, { 0.279, 0.736 } },     { "C6", { 22.67, 2.11, -1.10 }, { 0.279, 0.893 } },
      { "D1", { 92.72, 1.89, 2.76 }, { 0.384, 0.107 } },     { "D2", { 88.85, 1.59, 2.27 }, { 0.384, 0.264 } },
      { "D3", { 73.42, 0.99, 1.89 }, { 0.384, 0.421 } },     { "D4", { 57.15, 0.57, 1.19 }, { 0.384, 0.579 } },
      { "D5", { 41.57, 0.24, 1.45 }, { 0.384, 0.736 } },     { "D6", { 25.65, 1.24, 0.05 }, { 0.384, 0.893 } },
      { "E1", { 96.04, 2.16, 2.60 }, { 0.616, 0.107 } },     { "E2", { 80.44, 1.17, 2.05 }, { 0.616, 0.264 } },
      { "E3", { 65.52, 0.69, 1.86 }, { 0.616, 0.421 } },     { "E4", { 49.62, 0.58, 1.56 }, { 0.616, 0.579 } },
      { "E5", { 33.55, 0.35, 1.40 }, { 0.616, 0.736 } },     { "E6", { 16.91, 1.43, -0.81 }, { 0.616, 0.893 } },
      { "F1", { 47.12, -32.50, -28.75 }, { 0.721, 0.107 } }, { "F2", { 50.49, 53.45, -13.55 }, { 0.721, 0.264 } },
      { "F3", { 83.61, 3.36, 87.02 }, { 0.721, 0.421 } },    { "F4", { 41.05, 60.75, 31.17 }, { 0.721, 0.579 } },
      { "F5", { 54.14, -40.80, 34.75 }, { 0.721, 0.736 } },  { "F6", { 24.75, 13.78, -49.48 }, { 0.721, 0.893 } },
      { "G1", { 60.94, 38.21, 61.31 }, { 0.825, 0.107 } },   { "G2", { 37.80, 7.30, -43.04 }, { 0.825, 0.264 } },
      { "G3", { 49.81, 48.50, 15.76 }, { 0.825, 0.421 } },   { "G4", { 28.88, 19.36, -24.48 }, { 0.825, 0.579 } },
      { "G5", { 72.45, -23.57, 60.47 }, { 0.825, 0.736 } },  { "G6", { 71.65, 23.74, 72.28 }, { 0.825, 0.893 } },
      { "H1", { 70.19, -31.85, 1.98 }, { 0.929, 0.107 } },   { "H2", { 54.38, 8.84, -25.71 }, { 0.929, 0.264 } },
      { "H3", { 42.03, -15.78, 22.93 }, { 0.929, 0.421 } },  { "H4", { 48.82, -5.11, -23.08 }, { 0.929, 0.579 } },
      { "H5", { 65.10, 18.14, 18.68 }, { 0.929, 0.736 } },   { "H6", { 36.13, 14.15, 15.78 }, { 0.929, 0.893 } } }
};

typedef struct dt_colorchecker_label_t
{
  gchar *label;
  dt_color_checker_targets type;
  gchar *path;
} dt_colorchecker_label_t;

// Add other type of a CGATS.17 here
static const char *CGATS_types[] = { "CGATS.17", "IT8.7/1" };

typedef enum dt_colorchecker_CGATS_types
{
  CGATS_TYPE_CGATS_17,
  CGATS_TYPE_IT8_7_1,
  CGATS_TYPE_LAST
} dt_colorchecker_CGATS_types;

// This defines charts specifications
typedef struct dt_colorchecker_CGATS_spec_t
{
  float radius;
  float ratio;
  size_t size[2];
  size_t middle_grey;
  size_t white;
  size_t black;

  size_t colums;
  size_t rows;
  float patch_width;
  float patch_height;
  float patch_offset_x;
  float patch_offset_y;
} dt_colorchecker_CGATS_spec_t;

// this type exist, but I don't know values to put yet
dt_colorchecker_CGATS_spec_t CGATS_17 = { .radius = 0.035,
                                          .ratio = 2.f / 3.f,
                                          .size = { 4, 6 },
                                          .middle_grey = 3,
                                          .white = 0,
                                          .black = 5,
                                          .colums = 4,
                                          .rows = 6,
                                          .patch_width = 26.f,
                                          .patch_height = 26.f,
                                          .patch_offset_x = 8.f,
                                          .patch_offset_y = 5.f };

dt_colorchecker_CGATS_spec_t IT8_7_1 = {
  .colums = 22,
  .rows = 12,
  .patch_width = 0.04255f,   // 1.0f / (cols + 1.5f)
  .patch_height = 0.0740f,   // 1.0f / (rows + 1.5f)
  .patch_offset_x = 0.0531f, // 1.25f * patch_size_x
  .patch_offset_y = 0.0925f, // 1.25f * patch_size_y
  .radius = 0.0189f,
  .ratio = 13.f / 23.f,
  .size = { 23, 13 },
  .middle_grey = 273, // GS09
  .white = 263,       // Dmin or GS00)
  .black = 287        // Dmax or GS23)
};


dt_colorchecker_CGATS_types dt_get_CGATS_type_val(const char *type)
{
  dt_colorchecker_CGATS_types t = CGATS_TYPE_CGATS_17;

  while(type && t <= CGATS_TYPE_LAST)
  {
    if(!g_strcmp0(type, CGATS_types[t])) break;
    t++;
  }

  return t;
}

dt_colorchecker_CGATS_spec_t *dt_get_CGATS_spec(const char *type)
{
  if(type)
  {
    dt_colorchecker_CGATS_types t = dt_get_CGATS_type_val(type);

    switch(t)
    {
      case CGATS_TYPE_CGATS_17:
        return &CGATS_17;
      case CGATS_TYPE_IT8_7_1:
        return &IT8_7_1;
      case CGATS_TYPE_LAST:
        fprintf(stderr, "Unknown CGATS type: %s\n", type);
        return &CGATS_17;
    }
  }

  return &CGATS_17;
}

dt_color_checker_t *dt_get_color_checker(const dt_color_checker_targets target_type, GList **reference_file_path);

/**
 * Cleanup functions
 */

static void dt_color_checker_cleanup(void *data)
{
  dt_color_checker_t *checker = (dt_color_checker_t *)data;
  if(checker && checker->type == COLOR_CHECKER_USER_REF)
  {
    // Free ONLY if they were individually allocated.
    // For built-in color checkers, patch names are static and must not be freed.
    // For dynamically allocated (IT8) color checkers, patch names are duplicated and must be freed.
    // Here, we assume that for COLOR_CHECKER_USER_REF, values[i].name was allocated.
    fprintf(stderr, "Freeing color checker %s\n", checker->name);
    fprintf(stderr, "first patch name %s\n", checker->values[0].name);
    g_free(checker->name);
    g_free(checker->author);
    g_free(checker->date);
    g_free(checker->manufacturer);
    if(checker->patches)
      for(size_t i = 0; i < checker->patches; i++) g_free(checker->values[i].name);
  }
}

static void dt_colorchecker_label_cleanup(gpointer data)
{
  dt_colorchecker_label_t *checker_label = (dt_colorchecker_label_t *)data;

  // Only free if not NULL and if dynamically allocated (user reference)
  if(checker_label && checker_label->type == COLOR_CHECKER_USER_REF)
  {
    fprintf(stdout, "Freeing label %s\n", checker_label->label);
    g_free(checker_label->label);
    g_free(checker_label->path);
  }
}

/**
 * helper functions
 */

// get a patch index in the list of values from the coordinates of the patch in the checker array
static inline size_t dt_color_checker_get_index(const dt_color_checker_t *const target_checker,
                                                const size_t coordinates[2])
{
  // patches are stored column-major
  const size_t height = target_checker->size[1];
  return CLAMP(height * coordinates[0] + coordinates[1], 0, target_checker->patches - 1);
}

// get a a patch coordinates of in the checker array from the patch index in the list of values
static inline void dt_color_checker_get_coordinates(const dt_color_checker_t *const target_checker,
                                                    size_t *coordinates, const size_t index)
{
  // patches are stored column-major
  const size_t idx = CLAMP(index, 0, target_checker->patches - 1);
  const size_t height = target_checker->size[1];
  const size_t num_col = idx / height;
  const size_t num_lin = idx - num_col * height;
  coordinates[0] = CLAMP(num_col, 0, target_checker->size[0] - 1);
  coordinates[1] = CLAMP(num_lin, 0, target_checker->size[1] - 1);
}

// find a patch matching a name
static inline const dt_color_checker_patch *
dt_color_checker_get_patch_by_name(const dt_color_checker_t *const target_checker, const char *name, size_t *index)
{
  size_t idx = -1;
  const dt_color_checker_patch *patch = NULL;

  for(size_t k = 0; k < target_checker->patches; k++)
    if(strcmp(name, target_checker->values[k].name) == 0)
    {
      idx = k;
      patch = &target_checker->values[k];
      break;
    }

  if(patch == NULL) fprintf(stderr, "No patch matching name `%s` was found in %s\n", name, target_checker->name);

  if(index) *index = idx;
  return patch;
}

// find a colorchecker name
static inline char *dt_get_builtin_colorchecker_name(const dt_color_checker_targets target_type)
{
  dt_color_checker_t *color_checker = dt_get_color_checker(target_type, NULL);
  gchar *name = g_strdup(color_checker->name);
  dt_color_checker_cleanup(color_checker);
  fprintf(stdout, "dt_get_builtin_colorchecker_name: %s\n", name);
  return name;
}


/**
 * ANSI CGATS.17 is THE standard text file format for exchanging color measurement data.
 * This standard text format (the ASCII version is by far the most common) is the format
 * accepted by most color measurement and profiling applications.
 * They can be used with lcms2.
 *
 * IT8 targets contain 288 patches in total.
 * At the bottom of the chart, there is a grey scale consisting of 22 patches (labeled GS01 to GS22),
 * flanked on each side by a Dmin and a Dmax patch, which are usually
 * labeled as Dmin or GS00, and Dmax or GS23.
 */

/**
 * @brief Test if the file is a CGATS.17 file
 * and if it contains one table of patch only.
 *
 * @param data pointer to the cmsHANDLE
 * @return gboolean TRUE if the file is valid, FALSE otherwise.
 */
gboolean dt_colorchecker_is_CGATS_valid(void *data)
{
  gboolean valid = TRUE;
  cmsHANDLE hIT8 = *(cmsHANDLE *)data;

  if(!hIT8)
  {
    fprintf(stderr, "Error loading IT8 file.\n");
    valid = FALSE;
  }
  else
  {
    const char *CGATS_type = cmsIT8GetProperty(hIT8, "CGATS");
    // Check if the data type can be found in our supported list of CGATS types
    if(dt_get_CGATS_type_val(CGATS_type) == CGATS_TYPE_LAST)
    {
      fprintf(stderr, "Warning: %s is not a supported CGATS.17 type.\n", CGATS_types[CGATS_type]);
      valid = FALSE;
    }

    uint32_t table_count = cmsIT8TableCount(hIT8);
    if(table_count != 1)
    {
      fprintf(stderr,
              "Warning: the CGATS.17 file contains %u table(s) but we only support files with one table at the "
              "moment.\n",
              table_count);
      valid = FALSE;
    }
  }
  return valid;
}

static inline const char *_dt_colorchecker_CGATS_get_author(const cmsHANDLE *hIT8)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return "Unknown Author";
  }
  const char *author = cmsIT8GetProperty(*hIT8, "ORIGINATOR");

  return author ? author : "Unknown Author";
}

static inline char *_dt_colorchecker_CGATS_get_date(const cmsHANDLE *hIT8)
{
  // in CGATS.17, the production date is stored in the format YYYY:MM

  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return "Unknown Date";
  }

  // in CGATS.17, the date in PROD_DATE is stored in the format YYYY:MM
  const char *date = cmsIT8GetProperty(*hIT8, "PROD_DATE");

  // we need date to continue.
  if(!date) return "Unknown Date";

  char *result = NULL;

  gchar **parts = g_strsplit_set(date, ":", 0);
  if(parts && parts[0] && parts[1])
  {
    const char *month[12]
        = { "Jan ", "Feb ", "Mar ", "Apr ", "May ", "Jun ", "Jul ", "Aug ", "Sep ", "Oct ", "Nov ", "Dec " };
    int month_num = atoi(parts[1]);
    if(month_num >= 1 && month_num <= 12)
      parts[1] = g_strdup(month[month_num - 1]);

    // write a new date in the format "Month(short) YYYY"
    gchar *date_fmt = g_strdup_printf("%s%s", parts[1], parts[0]);
    result = g_strdup(date_fmt);
    g_free(date_fmt);
  }
  else
    result = g_strdup(date);

  g_strfreev(parts);

  return result;
}

static inline const char *_dt_colorchecker_CGATS_get_manufacturer(const cmsHANDLE *hIT8)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid IT8 handle provided.\n");
    return "Unknown Manufacturer";
  }
  const char *manufacturer = cmsIT8GetProperty(*hIT8, "MANUFACTURER");
  return manufacturer ? manufacturer : "Unknown Manufacturer";
}


/**
 * @brief build a name for the colorchecker.
 * The returned string must be freed by the caller.
 *
 * @param substitute the file name of the colorchecker with extension
 * @param originator the author of the CGATS file
 * @param date the date of the CGATS file
 * @return gchar* String with the name of the colorchecker
 */
static inline gchar *_dt_colorchecker_ref_build_name(const char *substitute, const char *originator, const char *date)
{
  gchar *name = NULL;
  if(substitute && g_strcmp0(substitute, ""))
  {
    // Find the last '.' for the extension
    const char *dot = g_strrstr(substitute, ".");
    size_t name_len = dot ? (size_t)(dot - substitute) : safe_strlen(substitute);
    gchar *basename = g_strndup(substitute, name_len);

    if(basename && originator && date)
    {
      // Build the name with the format: basename - author - date
      gchar *tmp_originator = NULL;
      gchar *tmp_date = NULL;

      // author if any
      if(originator && g_strcmp0(originator, "")) tmp_originator = g_strdup_printf(" - %s", originator);
      // ate if any
      if(date && g_strcmp0(date, "")) tmp_date = g_strdup_printf(" - %s", date);

      // Compose: filename
      name = g_strdup_printf("%s%s%s", basename, tmp_originator ? tmp_originator : "", tmp_date ? tmp_date : "");

      if(tmp_originator) g_free(tmp_originator);
      if(tmp_date) g_free(tmp_date);
    }
		else
		{
			// If no author or date, just use the substitute
			name = g_strdup(substitute);
		}
    g_free(basename);
  }
  return name;
}

/**
 * @brief Get the name of the colorchecker from the CGATS file.
 * The resulting string must be freed by the caller.
 *
 * @param hIT8 the CGATS file handle
 * @param substitute other name to use if the CGATS file does not contain a name or if the function fails.
 * @return char* String with the name of the colorchecker
 */
static inline char *_dt_colorchecker_CGATS_get_name(const cmsHANDLE *hIT8, const char *substitute)
{
  if(!hIT8)
  {
    fprintf(stderr, "Error: Invalid CGATS handle provided.\n");
    return "Unnamed CGATS";
  }

  gchar *originator = g_strdup(_dt_colorchecker_CGATS_get_author(hIT8));
  gchar *date = g_strdup(_dt_colorchecker_CGATS_get_date(hIT8));
  gchar *name = _dt_colorchecker_ref_build_name(substitute, originator, date);

  if(originator) free(originator);
  if(date) free(date);

  gchar *result = NULL;

  if(!name)
    result = g_strdup(substitute && g_strcmp0(substitute, "") ? substitute : "Unnamed IT8");
  else
  {
    result = g_strdup(name);
    g_free(name);
  }

  return result;
}

/**
 * @brief fills the patch values from the CGATS file, converts to Lab if needed.
 * The number of patches to be filled is given by the CGATS file.
 *
 * @param hIT8 the CGATS file handle
 * @param values the array of patches to be filled
 * @return NULL if error, dt_color_checker_patch* with the colors otherwise.
 */
int _dt_colorchecker_CGATS_fill_patch_values(cmsHANDLE hIT8, dt_color_checker_patch *values)
{
  int error = 0;
  if(!values) error = 1;

  int column_SAMPLE_ID = -1;
  int column_X = -1;
  int column_Y = -1;
  int column_Z = -1;
  int column_L = -1;
  int column_a = -1;
  int column_b = -1;
  char **sample_names = NULL;
  int n_columns = cmsIT8EnumDataFormat(hIT8, &sample_names);
  size_t num_patches = (size_t)cmsIT8GetPropertyDbl(hIT8, "NUMBER_OF_SETS");
  const char *CGATS_type = cmsIT8GetSheetType(hIT8);
  dt_colorchecker_CGATS_spec_t *CGATS_spec = dt_get_CGATS_spec(CGATS_type);

  gboolean use_XYZ = FALSE;
  if(n_columns == -1)
  {
    fprintf(stderr, "error with the IT8 file, can't get column types\n");
    error = 1;
  }

  for(int i = 0; i < n_columns; i++)
  {
    if(!g_strcmp0(sample_names[i], "SAMPLE_ID"))
      column_SAMPLE_ID = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_X"))
      column_X = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_Y"))
      column_Y = i;
    else if(!g_strcmp0(sample_names[i], "XYZ_Z"))
      column_Z = i;
    else if(!g_strcmp0(sample_names[i], "LAB_L"))
      column_L = i;
    else if(!g_strcmp0(sample_names[i], "LAB_A"))
      column_a = i;
    else if(!g_strcmp0(sample_names[i], "LAB_B"))
      column_b = i;
  }

  if(column_SAMPLE_ID == -1)
  {
    fprintf(stderr, "error with the IT8 file, can't find the SAMPLE_ID column\n");
    error = 1;
  }

  int columns[3] = { -1, -1, -1 };
  if(column_L != -1 && column_a != -1 && column_b != -1)
  {
    columns[0] = cmsIT8FindDataFormat(hIT8, "LAB_L");
    columns[1] = cmsIT8FindDataFormat(hIT8, "LAB_A");
    columns[2] = cmsIT8FindDataFormat(hIT8, "LAB_B");
  }
  // In case no Lab column is found, we assume the IT8 file has XYZ data
  else if(column_X != -1 && column_Y != -1 && column_Z != -1)
  {
    use_XYZ = TRUE;
    columns[0] = cmsIT8FindDataFormat(hIT8, "XYZ_X");
    columns[1] = cmsIT8FindDataFormat(hIT8, "XYZ_Y");
    columns[2] = cmsIT8FindDataFormat(hIT8, "XYZ_Z");
  }
  else
  {
    fprintf(stderr, "error with the IT8 file, can't find XYZ or Lab columns\n");
    error = 1;
  }

  // IT8 chart dimensions
  const int cols = CGATS_spec->colums;
  const int rows = CGATS_spec->rows;
  // Patch size in ratio of the chart size
  const float patch_size_x = CGATS_spec->patch_width;
  const float patch_size_y = CGATS_spec->patch_height;

  // Offset ratio of the center of the patch from the border of the chart
  const float patch_offset_x = CGATS_spec->patch_offset_x;
  const float patch_offset_y = CGATS_spec->patch_offset_y;

  for(int patch_iter = 0; patch_iter < num_patches; patch_iter++)
  {
    values[patch_iter].name = (char *)cmsIT8GetDataRowCol(hIT8, patch_iter, 0);
    if(values[patch_iter].name == NULL)
    {
      {
        fprintf(stderr, "error with the CGATS file, can't find sample '%d'\n", patch_iter);
        error = 1;
      }
      continue;
    }

    // IT8 grey scale patches
    if(g_strcmp0(CGATS_type, CGATS_types[CGATS_TYPE_IT8_7_1]) && patch_iter + 1 > cols * rows)
    {
      int grey_patch_iter = (patch_iter + 1) - cols * rows;
      // calculate the patch's horizontal and vertical position in the guide
      values[patch_iter].x = (grey_patch_iter - 0.75f) * patch_size_x;
      values[patch_iter].y = 14.5f * patch_size_y;
    }
    else
    {
      // find the patch's horizontal position in the guide
      values[patch_iter].x = (patch_iter % cols) * patch_size_x;
      values[patch_iter].x += patch_offset_x;

      // find the patch's vertical position in the guide
      values[patch_iter].y = (int)patch_iter / cols; // The result must be an int
      values[patch_iter].y *= patch_size_y;
      values[patch_iter].y += patch_offset_y;
    }

    const double patchdbl[3] = { cmsIT8GetDataRowColDbl(hIT8, patch_iter, columns[0]),
                                 cmsIT8GetDataRowColDbl(hIT8, patch_iter, columns[1]),
                                 cmsIT8GetDataRowColDbl(hIT8, patch_iter, columns[2]) };

    const dt_aligned_pixel_t patch_color = { (float)patchdbl[0], (float)patchdbl[1], (float)patchdbl[2], 0.0f };

    // Convert to Lab when it's in XYZ
    if(use_XYZ)
      dt_XYZ_to_Lab(patch_color, values[patch_iter].Lab);
    else
    {
      values[patch_iter].Lab[0] = patch_color[0];
      values[patch_iter].Lab[1] = patch_color[1];
      values[patch_iter].Lab[2] = patch_color[2];
    }
  }

  return error;
}

dt_color_checker_t *dt_colorchecker_user_ref_create(const char *filename)
{
  int error = 0;
  cmsHANDLE hIT8 = cmsIT8LoadFromFile(NULL, filename);

  if(!dt_colorchecker_is_CGATS_valid(&hIT8))
  {
    fprintf(stderr, "Ansel cannot load the CGATS file '%s'\n", filename);
    error = 1;
    goto end;
  }

  const char *type = cmsIT8GetSheetType(hIT8);
  dt_colorchecker_CGATS_spec_t *CGATS_spec = dt_get_CGATS_spec(type);
  size_t num_patches = (size_t)cmsIT8GetPropertyDbl(hIT8, "NUMBER_OF_SETS");
  size_t total_size = sizeof(dt_color_checker_t) + num_patches * sizeof(dt_color_checker_patch);

  dt_color_checker_t *checker = malloc(total_size);
  if(!checker)
  {
    fprintf(stderr, "Error: can't allocate memory for the color checker from IT8 chart\n");
    error = 1;
    goto end;
  }

  checker->name = _dt_colorchecker_CGATS_get_name(&hIT8, NULL);
  checker->author = g_strdup(_dt_colorchecker_CGATS_get_author(&hIT8));
  checker->date = g_strdup(_dt_colorchecker_CGATS_get_date(&hIT8));
  checker->manufacturer = g_strdup(_dt_colorchecker_CGATS_get_manufacturer(&hIT8));
  checker->type = COLOR_CHECKER_USER_REF;
  checker->radius = CGATS_spec->radius;
  checker->ratio = CGATS_spec->ratio;
  checker->patches = num_patches;
  checker->size[0] = CGATS_spec->size[0];
  checker->size[1] = CGATS_spec->size[1];
  checker->middle_grey = CGATS_spec->middle_grey;
  checker->white = CGATS_spec->white;
  checker->black = CGATS_spec->black;

  _dt_colorchecker_CGATS_fill_patch_values(hIT8, checker->values);

  // checker->middle_grey = le patch dont la valeur L est la plus proche de 50, a et b le plus proche de 0;

  fprintf(stdout, "it8 '%s' done\n", filename);

  /*if(!checker.values)
  {
    fprintf(stderr, "Error: can't allocate memory for the color checker from IT8 chart\n");
    error = 1;
  }*/

end:
  if(hIT8) cmsIT8Free(hIT8);
  return error ? NULL : checker;
}

/*
 * Color Checker getter
 */

int _dt_colorchecker_find_builtin(GList **colorcheckers_label)
{
  int nb = 0;
  for(int k = 0; k < COLOR_CHECKER_USER_REF; k++)
  {
    dt_colorchecker_label_t *builtin_label = malloc(sizeof(dt_colorchecker_label_t));
    *builtin_label = (dt_colorchecker_label_t){ .label = dt_get_builtin_colorchecker_name(k), .type = k, .path = NULL };

    *colorcheckers_label = g_list_append(*colorcheckers_label, builtin_label);
    nb++;
  }
  return nb;
}

/**
 * @brief Find all CGAT files in the user config/color/it8 directory
 *
 * ANSI CGATS.17 is THE standard text file format for exchanging color measurement data.
 * This standard text format (the ASCII version is by far the most common) is the format
 * accepted by most color measurement and profiling applications.
 *
 * @param ref_colorcheckers_files NULL GList that will be populated with found IT8 files
 * @return int Number of found files
 */
int _dt_colorchecker_find_CGAT_reference_files(GList **ref_colorcheckers_files)
{
  int nb = 0;
  char confdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(confdir, sizeof(confdir));
  char *user_it8_dir = g_build_filename(confdir, "color", "it8", NULL);

  GDir *dir = g_dir_open(user_it8_dir, 0, NULL);
  if(dir)
  {
    const gchar *filename;
    while((filename = g_dir_read_name(dir)) != NULL)
    {
      gchar *filepath = g_build_filename(user_it8_dir, filename, NULL);
      if(g_file_test(filepath, G_FILE_TEST_IS_REGULAR))
      {
        cmsHANDLE hIT8 = cmsIT8LoadFromFile(NULL, filepath);

        if(hIT8 && dt_colorchecker_is_CGATS_valid(&hIT8))
        {
          gchar *label = _dt_colorchecker_CGATS_get_name(&hIT8, filename);
          size_t label_size = safe_strlen(label) + safe_strlen(filepath) + sizeof(dt_color_checker_targets);
          dt_colorchecker_label_t *it8_label = malloc(label_size);

          it8_label->label = g_strdup(label);
          it8_label->path = g_strdup(filepath);
          it8_label->type = COLOR_CHECKER_USER_REF;

          g_free(label);

          fprintf(stdout, "it8 '%s', '%s'\n", it8_label->label, it8_label->path);
          *ref_colorcheckers_files = g_list_append(*ref_colorcheckers_files, it8_label);
          nb++;
        }
        cmsIT8Free(hIT8);
      }
      g_free(filepath);
    }
    g_dir_close(dir);
  }
  free(user_it8_dir);

  return nb;
}

/**
 * @brief Find all builtin and CGATS colorcheckers.
 *
 * @param colorcheckers_label the NULL GList that will be populated with found colorcheckers.
 * @return int Number of found colorcheckers.
 */
int dt_colorchecker_find(GList **colorcheckers_label)
{
  int nb = _dt_colorchecker_find_builtin(colorcheckers_label);
  fprintf(stdout, "dt_colorchecker_find: found %d builtin colorcheckers\n", nb);
  nb += _dt_colorchecker_find_CGAT_reference_files(colorcheckers_label);
  fprintf(stdout, "dt_colorchecker_find: found %d CGAT references files\n", nb);
  return nb;
}

/*static inline gboolean _dt_colorchecker_it8_check(const dt_color_checker_targets target_type, GList
**reference_file_path)
{
  if(reference_file_path && *reference_file_path)
  {
    gboolean is_a_user_ref = target_type >= COLOR_CHECKER_USER_REF;

    int user_ref_list_len = g_list_length(*reference_file_path);
    int nth_selected_user_ref = target_type - COLOR_CHECKER_USER_REF;

    gboolean is_too_far = nth_selected_user_ref > user_ref_list_len;
    return is_a_user_ref && !is_too_far;
  }

  return FALSE;
}*/

dt_color_checker_t *dt_get_color_checker(const dt_color_checker_targets target_type, GList **colorchecker_label)
{
  fprintf(stdout, "dt_get_color_checker: colorchecker type %i.\n", target_type);

  dt_color_checker_targets nth_checker = 0;
  dt_colorchecker_label_t *label_data = NULL;
	if(target_type >= COLOR_CHECKER_USER_REF && colorchecker_label != NULL && *colorchecker_label != NULL)
  {
    fprintf(stdout, "dt_get_color_checker: colorchecker type %i is a user reference.\n", target_type);

    // Get the label data from the list
    label_data = g_list_nth_data(*colorchecker_label, target_type);
    nth_checker = COLOR_CHECKER_USER_REF;
  }
  else // it's a builtin colorchecker
    nth_checker = target_type;


  switch(nth_checker)
  {
    case COLOR_CHECKER_XRITE_24_2000:
      return &xrite_24_2000;

    case COLOR_CHECKER_XRITE_24_2014:
      return &xrite_24_2014;

    case COLOR_CHECKER_SPYDER_24:
      return &spyder_24;

    case COLOR_CHECKER_SPYDER_24_V2:
      return &spyder_24_v2;

    case COLOR_CHECKER_SPYDER_48:
      return &spyder_48;

    case COLOR_CHECKER_SPYDER_48_V2:
      return &spyder_48_v2;

    case COLOR_CHECKER_USER_REF:
      if(label_data)
      {
        fprintf(stdout, "COLOR_CHECKER_USER_REF type %i is %s.\n", target_type, label_data->path);
        return dt_colorchecker_user_ref_create(label_data->path);
      }
      else
        fprintf(stderr, "COLOR_CHECKER_USER_REF type %i not found!\n", target_type);

    case COLOR_CHECKER_LAST:
      fprintf(stderr, "dt_get_color_checker: colorchecker type %i not found!\n", target_type);
      return &xrite_24_2014;
  }

  return &xrite_24_2014;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
