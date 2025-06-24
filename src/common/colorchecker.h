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

#include "darktable.h"

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
  const char *name;       // mnemonic name for the patch
  dt_aligned_pixel_t Lab; // reference color in CIE Lab

  // (x, y) position of the patch  center, relatively to the guides (white dots)
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
  dt_color_checker_patch *values;  // pointer to an array of colors
} dt_color_checker_t;

dt_color_checker_patch xrite_24_2000_patches[] = {
                                              { "A1", { 37.986,  13.555,  14.059 }, { 0.087, 0.125}},
                                              { "A2", { 65.711,  18.13,   17.81  }, { 0.250, 0.125}},
                                              { "A3", { 49.927, -04.88,  -21.905 }, { 0.417, 0.125}},
                                              { "A4", { 43.139, -13.095,  21.905 }, { 0.584, 0.125}},
                                              { "A5", { 55.112,  08.844, -25.399 }, { 0.751, 0.125}},
                                              { "A6", { 70.719, -33.397,  -0.199 }, { 0.918, 0.125}},
                                              { "B1", { 62.661,  36.067,  57.096 }, { 0.087, 0.375}},
                                              { "B2", { 40.02,   10.41,  -45.964 }, { 0.250, 0.375}},
                                              { "B3", { 51.124,  48.239,  16.248 }, { 0.417, 0.375}},
                                              { "B4", { 30.325,  22.976, -21.587 }, { 0.584, 0.375}},
                                              { "B5", { 72.532, -23.709,  57.255 }, { 0.751, 0.375}},
                                              { "B6", { 71.941,  19.363,  67.857 }, { 0.918, 0.375}},
                                              { "C1", { 28.778,  14.179, -50.297 }, { 0.087, 0.625}},
                                              { "C2", { 55.261, -38.342,  31.37  }, { 0.250, 0.625}},
                                              { "C3", { 42.101,  53.378,  28.19  }, { 0.417, 0.625}},
                                              { "C4", { 81.733,  04.039,  79.819 }, { 0.584, 0.625}},
                                              { "C5", { 51.935,  49.986, -14.574 }, { 0.751, 0.625}},
                                              { "C6", { 51.038, -28.631, -28.638 }, { 0.918, 0.625}},
                                              { "D1", { 96.539,  -0.425,   1.186 }, { 0.087, 0.875}},
                                              { "D2", { 81.257,  -0.638,  -0.335 }, { 0.250, 0.875}},
                                              { "D3", { 66.766,  -0.734,  -0.504 }, { 0.417, 0.875}},
                                              { "D4", { 50.867,  -0.153,  -0.27  }, { 0.584, 0.875}},
                                              { "D5", { 35.656,  -0.421,  -1.231 }, { 0.751, 0.875}},
                                              { "D6", { 20.461,  -0.079,  -0.973 }, { 0.918, 0.875}} };

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
                                    .values = xrite_24_2000_patches };
                                    
dt_color_checker_patch xrite_24_2014_patches[] = {
                                              { "A1", { 37.54,   14.37,   14.92 }, { 0.087, 0.125}},
                                              { "A2", { 64.66,   19.27,   17.50 }, { 0.250, 0.125}},
                                              { "A3", { 49.32,  -03.82,  -22.54 }, { 0.417, 0.125}},
                                              { "A4", { 43.46,  -12.74,   22.72 }, { 0.584, 0.125}},
                                              { "A5", { 54.94,   09.61,  -24.79 }, { 0.751, 0.125}},
                                              { "A6", { 70.48,  -32.26,  -00.37 }, { 0.918, 0.125}},
                                              { "A1", { 37.54,   14.37,   14.92 }, { 0.087, 0.125}},
                                              { "A2", { 64.66,   19.27,   17.50 }, { 0.250, 0.125}},
                                              { "A3", { 49.32,  -03.82,  -22.54 }, { 0.417, 0.125}},
                                              { "A4", { 43.46,  -12.74,   22.72 }, { 0.584, 0.125}},
                                              { "A5", { 54.94,   09.61,  -24.79 }, { 0.751, 0.125}},
                                              { "A6", { 70.48,  -32.26,  -00.37 }, { 0.918, 0.125}},
                                              { "B1", { 62.73,   35.83,   56.50 }, { 0.087, 0.375}},
                                              { "B2", { 39.43,   10.75,  -45.17 }, { 0.250, 0.375}},
                                              { "B3", { 50.57,   48.64,   16.67 }, { 0.417, 0.375}},
                                              { "B4", { 30.10,   22.54,  -20.87 }, { 0.584, 0.375}},
                                              { "B5", { 71.77,  -24.13,   58.19 }, { 0.751, 0.375}},
                                              { "B6", { 71.51,   18.24,   67.37 }, { 0.918, 0.375}},
                                              { "C1", { 28.37,   15.42,  -49.80 }, { 0.087, 0.625}},
                                              { "C2", { 54.38,  -39.72,   32.27 }, { 0.250, 0.625}},
                                              { "C3", { 42.43,   51.05,   28.62 }, { 0.417, 0.625}},
                                              { "C4", { 81.80,   02.67,   80.41 }, { 0.584, 0.625}},
                                              { "C5", { 50.63,   51.28,  -14.12 }, { 0.751, 0.625}},
                                              { "C6", { 49.57,  -29.71,  -28.32 }, { 0.918, 0.625}},
                                              { "D1", { 95.19,  -01.03,   02.93 }, { 0.087, 0.875}},
                                              { "D2", { 81.29,  -00.57,   00.44 }, { 0.250, 0.875}},
                                              { "D3", { 66.89,  -00.75,  -00.06 }, { 0.417, 0.875}},
                                              { "D4", { 50.76,  -00.13,   00.14 }, { 0.584, 0.875}},
                                              { "D5", { 35.63,  -00.46,  -00.48 }, { 0.751, 0.875}},
                                              { "D6", { 20.64,   00.07,  -00.46 }, { 0.918, 0.875}} };

dt_color_checker_t xrite_24_2014 = { .name = "Xrite ColorChecker 24 after 2014",
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
                                    .values = xrite_24_2014_patches };


// dimensions between reference dots : 197 mm width x 135 mm height
// patch width : 26x26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm

dt_color_checker_patch spyder_24_patches[] = {
                                              { "A1", { 96.04,	 2.16,	 2.60 }, { 0.107, 0.844 } },
                                              { "A2", { 80.44,	 1.17,	 2.05 }, { 0.264, 0.844 } },
                                              { "A3", { 65.52,	 0.69,	 1.86 }, { 0.421, 0.844 } },
                                              { "A4", { 49.62,	 0.58,	 1.56 }, { 0.579, 0.844 } },
                                              { "A5", { 33.55,	 0.35,	 1.40 }, { 0.736, 0.844 } },
                                              { "A6", { 16.91,	 1.43,	-0.81 }, { 0.893, 0.844 } },
                                              { "B1", { 47.12, -32.50, -28.75 }, { 0.107, 0.615 } },
                                              { "B2", { 50.49,	53.45, -13.55 }, { 0.264, 0.615 } },
                                              { "B3", { 83.61,	 3.36,	87.02 }, { 0.421, 0.615 } },
                                              { "B4", { 41.05,	60.75,	31.17 }, { 0.579, 0.615 } },
                                              { "B5", { 54.14, -40.80,	34.75 }, { 0.736, 0.615 } },
                                              { "B6", { 24.75,	13.78, -49.48 }, { 0.893, 0.615 } },
                                              { "C1", { 60.94,	38.21,	61.31 }, { 0.107, 0.385 } },
                                              { "C2", { 37.80,	 7.30, -43.04 }, { 0.264, 0.385 } },
                                              { "C3", { 49.81,	48.50,	15.76 }, { 0.421, 0.385 } },
                                              { "C4", { 28.88,	19.36, -24.48 }, { 0.579, 0.385 } },
                                              { "C5", { 72.45, -23.60,	60.47 }, { 0.736, 0.385 } },
                                              { "C6", { 71.65,	23.74,	72.28 }, { 0.893, 0.385 } },
                                              { "D1", { 70.19, -31.90,	 1.98 }, { 0.107, 0.155 } },
                                              { "D2", { 54.38,	 8.84, -25.71 }, { 0.264, 0.155 } },
                                              { "D3", { 42.03, -15.80,	22.93 }, { 0.421, 0.155 } },
                                              { "D4", { 48.82,	-5.11, -23.08 }, { 0.579, 0.155 } },
                                              { "D5", { 65.10,	18.14,	18.68 }, { 0.736, 0.155 } },
                                              { "D6", { 36.13,	14.15,	15.78 }, { 0.893, 0.155 } } };

dt_color_checker_t spyder_24 = {  .name = "Datacolor SpyderCheckr 24 before 2018",
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
                                  .values = spyder_24_patches };


// dimensions between reference dots : 197 mm width x 135 mm height
// patch width : 26x26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm

dt_color_checker_patch spyder_24_v2_patch[] = {{ "A1", { 96.04,   2.16,   2.60 }, { 0.107, 0.844 } },
                                        { "A2", { 80.44,   1.17,   2.05 }, { 0.264, 0.844 } },
                                        { "A3", { 65.52,   0.69,   1.86 }, { 0.421, 0.844 } },
                                        { "A4", { 49.62,   0.58,   1.56 }, { 0.579, 0.844 } },
                                        { "A5", { 33.55,   0.35,   1.40 }, { 0.736, 0.844 } },
                                        { "A6", { 16.91,   1.43,  -0.81 }, { 0.893, 0.844 } },
                                        { "B1", { 47.12, -32.50, -28.75 }, { 0.107, 0.615 } },
                                        { "B2", { 50.49,  53.45, -13.55 }, { 0.264, 0.615 } },
                                        { "B3", { 83.61,   3.36,  87.02 }, { 0.421, 0.615 } },
                                        { "B4", { 41.05,  60.75,  31.17 }, { 0.579, 0.615 } },
                                        { "B5", { 54.14, -40.80,  34.75 }, { 0.736, 0.615 } },
                                        { "B6", { 24.75,  13.78, -49.48 }, { 0.893, 0.615 } },
                                        { "C1", { 60.94,  38.21,  61.31 }, { 0.107, 0.385 } },
                                        { "C2", { 37.80,   7.30, -43.04 }, { 0.264, 0.385 } },
                                        { "C3", { 49.81,  48.50,  15.76 }, { 0.421, 0.385 } },
                                        { "C4", { 28.88,  19.36, -24.48 }, { 0.579, 0.385 } },
                                        { "C5", { 72.45, -23.57,  60.47 }, { 0.736, 0.385 } },
                                        { "C6", { 71.65,  23.74,  72.28 }, { 0.893, 0.385 } },
                                        { "D1", { 70.19, -31.85,   1.98 }, { 0.107, 0.155 } },
                                        { "D2", { 54.38,   8.84, -25.71 }, { 0.264, 0.155 } },
                                        { "D3", { 42.03, -15.78,  22.93 }, { 0.421, 0.155 } },
                                        { "D4", { 48.82,  -5.11, -23.08 }, { 0.579, 0.155 } },
                                        { "D5", { 65.10,  18.14,  18.68 }, { 0.736, 0.155 } },
                                        { "D6", { 36.13,  14.15,  15.78 }, { 0.893, 0.155 } } };

dt_color_checker_t spyder_24_v2 = {  .name = "Datacolor SpyderCheckr 24 after 2018",
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
                                  .values = spyder_24_v2_patch };


// dimensions between reference dots : 297 mm width x 197 mm height
// patch width : 26x26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm

dt_color_checker_patch spyder_48_patches[] = { { "A1", { 61.35,  34.81,  18.38 }, { 0.071, 0.107 } },
                                              { "A2", { 75.50 ,  5.84,  50.42 }, { 0.071, 0.264 } },
                                              { "A3", { 66.82,	-25.1,	23.47 }, { 0.071, 0.421 } },
                                              { "A4", { 60.53,	-22.6, -20.40 }, { 0.071, 0.579 } },
                                              { "A5", { 59.66,	-2.03, -28.46 }, { 0.071, 0.736 } },
                                              { "A6", { 59.15,	30.83,  -5.72 }, { 0.071, 0.893 } },
                                              { "B1", { 82.68,	 5.03,	 3.02 }, { 0.175, 0.107 } },
                                              { "B2", { 82.25,	-2.42,	 3.78 }, { 0.175, 0.264 } },
                                              { "B3", { 82.29,	 2.20,	-2.04 }, { 0.175, 0.421 } },
                                              { "B4", { 24.89,	 4.43,	 0.78 }, { 0.175, 0.579 } },
                                              { "B5", { 25.16,	-3.88,	 2.13 }, { 0.175, 0.736 } },
                                              { "B6", { 26.13,	 2.61,	-5.03 }, { 0.175, 0.893 } },
                                              { "C1", { 85.42,	 9.41,	14.49 }, { 0.279, 0.107 } },
                                              { "C2", { 74.28,	 9.05,	27.21 }, { 0.279, 0.264 } },
                                              { "C3", { 64.57,	12.39,	37.24 }, { 0.279, 0.421 } },
                                              { "C4", { 44.49,	17.23,	26.24 }, { 0.279, 0.579 } },
                                              { "C5", { 25.29,	 7.95,	 8.87 }, { 0.279, 0.736 } },
                                              { "C6", { 22.67,	 2.11,	-1.10 }, { 0.279, 0.893 } },
                                              { "D1", { 92.72,	 1.89,	 2.76 }, { 0.384, 0.107 } },
                                              { "D2", { 88.85,	 1.59,	 2.27 }, { 0.384, 0.264 } },
                                              { "D3", { 73.42,	 0.99,	 1.89 }, { 0.384, 0.421 } },
                                              { "D4", { 57.15,	 0.57,	 1.19 }, { 0.384, 0.579 } },
                                              { "D5", { 41.57,	 0.24,	 1.45 }, { 0.384, 0.736 } },
                                              { "D6", { 25.65,	 1.24,	 0.05 }, { 0.384, 0.893 } },
                                              { "E1", { 96.04,	 2.16,	 2.60 }, { 0.616, 0.107 } },
                                              { "E2", { 80.44,	 1.17,	 2.05 }, { 0.616, 0.264 } },
                                              { "E3", { 65.52,	 0.69,	 1.86 }, { 0.616, 0.421 } },
                                              { "E4", { 49.62,	 0.58,	 1.56 }, { 0.616, 0.579 } },
                                              { "E5", { 33.55,	 0.35,	 1.40 }, { 0.616, 0.736 } },
                                              { "E6", { 16.91,	 1.43,	-0.81 }, { 0.616, 0.893 } },
                                              { "F1", { 47.12, -32.50, -28.75 }, { 0.721, 0.107 } },
                                              { "F2", { 50.49,	53.45, -13.55 }, { 0.721, 0.264 } },
                                              { "F3", { 83.61,	 3.36,	87.02 }, { 0.721, 0.421 } },
                                              { "F4", { 41.05,	60.75,	31.17 }, { 0.721, 0.579 } },
                                              { "F5", { 54.14, -40.80,	34.75 }, { 0.721, 0.736 } },
                                              { "F6", { 24.75,	13.78, -49.48 }, { 0.721, 0.893 } },
                                              { "G1", { 60.94,	38.21,	61.31 }, { 0.825, 0.107 } },
                                              { "G2", { 37.80,	 7.30, -43.04 }, { 0.825, 0.264 } },
                                              { "G3", { 49.81,	48.50,	15.76 }, { 0.825, 0.421 } },
                                              { "G4", { 28.88,	19.36, -24.48 }, { 0.825, 0.579 } },
                                              { "G5", { 72.45, -23.60,	60.47 }, { 0.825, 0.736 } },
                                              { "G6", { 71.65,	23.74,	72.28 }, { 0.825, 0.893 } },
                                              { "H1", { 70.19, -31.90,	 1.98 }, { 0.929, 0.107 } },
                                              { "H2", { 54.38,	 8.84, -25.71 }, { 0.929, 0.264 } },
                                              { "H3", { 42.03, -15.80,	22.93 }, { 0.929, 0.421 } },
                                              { "H4", { 48.82,	-5.11, -23.08 }, { 0.929, 0.579 } },
                                              { "H5", { 65.10,	18.14,	18.68 }, { 0.929, 0.736 } },
                                              { "H6", { 36.13,	14.15,	15.78 }, { 0.929, 0.893 } } };

dt_color_checker_t spyder_48 = {  .name = "Datacolor SpyderCheckr 48 before 2018",
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
                                  .values = spyder_48_patches };


// dimensions between reference dots : 297 mm width x 197 mm height
// patch width : 26x26 mm
// outer gutter : 8 mm
// internal gutters (gap between patches) : 5 mm

dt_color_checker_patch spyder_48_v2_patch[] = { { "A1", { 61.35,  34.81,  18.38 }, { 0.071, 0.107 } },
                                              { "A2", { 75.50 ,  5.84,  50.42 }, { 0.071, 0.264 } },
                                              { "A3", { 66.82,  -25.1,  23.47 }, { 0.071, 0.421 } },
                                              { "A4", { 60.53, -22.62, -20.40 }, { 0.071, 0.579 } },
                                              { "A5", { 59.66,  -2.03, -28.46 }, { 0.071, 0.736 } },
                                              { "A6", { 59.15,  30.83,  -5.72 }, { 0.071, 0.893 } },
                                              { "B1", { 82.68,   5.03,   3.02 }, { 0.175, 0.107 } },
                                              { "B2", { 82.25,  -2.42,   3.78 }, { 0.175, 0.264 } },
                                              { "B3", { 82.29,   2.20,  -2.04 }, { 0.175, 0.421 } },
                                              { "B4", { 24.89,   4.43,   0.78 }, { 0.175, 0.579 } },
                                              { "B5", { 25.16,  -3.88,   2.13 }, { 0.175, 0.736 } },
                                              { "B6", { 26.13,   2.61,  -5.03 }, { 0.175, 0.893 } },
                                              { "C1", { 85.42,   9.41,  14.49 }, { 0.279, 0.107 } },
                                              { "C2", { 74.28,   9.05,  27.21 }, { 0.279, 0.264 } },
                                              { "C3", { 64.57,  12.39,  37.24 }, { 0.279, 0.421 } },
                                              { "C4", { 44.49,  17.23,  26.24 }, { 0.279, 0.579 } },
                                              { "C5", { 25.29,   7.95,   8.87 }, { 0.279, 0.736 } },
                                              { "C6", { 22.67,   2.11,  -1.10 }, { 0.279, 0.893 } },
                                              { "D1", { 92.72,   1.89,   2.76 }, { 0.384, 0.107 } },
                                              { "D2", { 88.85,   1.59,   2.27 }, { 0.384, 0.264 } },
                                              { "D3", { 73.42,   0.99,   1.89 }, { 0.384, 0.421 } },
                                              { "D4", { 57.15,   0.57,   1.19 }, { 0.384, 0.579 } },
                                              { "D5", { 41.57,   0.24,   1.45 }, { 0.384, 0.736 } },
                                              { "D6", { 25.65,   1.24,   0.05 }, { 0.384, 0.893 } },
                                              { "E1", { 96.04,   2.16,   2.60 }, { 0.616, 0.107 } },
                                              { "E2", { 80.44,   1.17,   2.05 }, { 0.616, 0.264 } },
                                              { "E3", { 65.52,   0.69,   1.86 }, { 0.616, 0.421 } },
                                              { "E4", { 49.62,   0.58,   1.56 }, { 0.616, 0.579 } },
                                              { "E5", { 33.55,   0.35,   1.40 }, { 0.616, 0.736 } },
                                              { "E6", { 16.91,   1.43,  -0.81 }, { 0.616, 0.893 } },
                                              { "F1", { 47.12, -32.50, -28.75 }, { 0.721, 0.107 } },
                                              { "F2", { 50.49,  53.45, -13.55 }, { 0.721, 0.264 } },
                                              { "F3", { 83.61,   3.36,  87.02 }, { 0.721, 0.421 } },
                                              { "F4", { 41.05,  60.75,  31.17 }, { 0.721, 0.579 } },
                                              { "F5", { 54.14, -40.80,  34.75 }, { 0.721, 0.736 } },
                                              { "F6", { 24.75,  13.78, -49.48 }, { 0.721, 0.893 } },
                                              { "G1", { 60.94,  38.21,  61.31 }, { 0.825, 0.107 } },
                                              { "G2", { 37.80,   7.30, -43.04 }, { 0.825, 0.264 } },
                                              { "G3", { 49.81,  48.50,  15.76 }, { 0.825, 0.421 } },
                                              { "G4", { 28.88,  19.36, -24.48 }, { 0.825, 0.579 } },
                                              { "G5", { 72.45, -23.57,  60.47 }, { 0.825, 0.736 } },
                                              { "G6", { 71.65,  23.74,  72.28 }, { 0.825, 0.893 } },
                                              { "H1", { 70.19, -31.85,   1.98 }, { 0.929, 0.107 } },
                                              { "H2", { 54.38,   8.84, -25.71 }, { 0.929, 0.264 } },
                                              { "H3", { 42.03, -15.78,  22.93 }, { 0.929, 0.421 } },
                                              { "H4", { 48.82,  -5.11, -23.08 }, { 0.929, 0.579 } },
                                              { "H5", { 65.10,  18.14,  18.68 }, { 0.929, 0.736 } },
                                              { "H6", { 36.13,  14.15,  15.78 }, { 0.929, 0.893 } } };

dt_color_checker_t spyder_48_v2 = {  .name = "Datacolor SpyderCheckr 48 after 2018",
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
                                  .values = spyder_48_v2_patch };

typedef struct dt_colorchecker_label_t
{
  gchar *label;
  dt_color_checker_targets type;
  gchar *path;
} dt_colorchecker_label_t;

// Add other supported type of CGATS here
typedef enum dt_colorchecker_CGATS_types
{
  CGATS_TYPE_IT8_7_1,
  CGATS_TYPE_IT8_7_2,
  CGATS_TYPE_UNKOWN
} dt_colorchecker_CGATS_types;

const char *CGATS_types[CGATS_TYPE_UNKOWN] = {
  "IT8.7/1", // transparent 
  "IT8.7/2"  // opaque
};

typedef enum dt_colorchecker_material_types
{
  COLOR_CHECKER_MATERIAL_TRANSPARENT,
  COLOR_CHECKER_MATERIAL_OPAQUE,
  COLOR_CHECKER_MATERIAL_UNKNOWN
} dt_colorchecker_material_types;

const char *colorchecker_material_types[COLOR_CHECKER_MATERIAL_UNKNOWN] = {
  "Transparent",
  "Opaque"
};

typedef struct dt_colorchecker_CGATS_label_name_t 
{
  const char *type;
  const char *originator;
  const char *date; // date in format 'Mon YYYY'
  const char *material;
} dt_colorchecker_CGATS_label_name_t;

// This defines charts specifications
typedef struct dt_colorchecker_chart_spec_t
{
  const gchar *type;
  float radius;       // radius of a patch in ratio of the checker diagonal
  float ratio;        // format ratio of the chart, guide to guide (white dots)
  size_t size[2];     // number of patch along x, y axes
  size_t middle_grey;
  size_t white;
  size_t black;

  int num_patches; // total number of patches
  int colums;
  int rows;
  float patch_width;
  float patch_height;
  float patch_offset_x;
  float patch_offset_y;

  GSList *patches; // list of patches struct, data are dt_color_checker_patch

  // pointer to the buildin chart definition.
  // If set, the struct should not be freed because not allocated.
  void *address;

} dt_colorchecker_chart_spec_t;

dt_colorchecker_chart_spec_t IT8_7 = {
  .type = "IT8",
  .radius = 0.0189f,
  .ratio = 6.f / 11.f,
  .size = { 22, 13 },
  .middle_grey = 273, // GS09
  .white = 263,       // Dmin or GS00
  .black = 287,       // Dmax or GS23

  .num_patches = 288,     // as specified in IT8.7/1 and IT8.7/2
  .colums = 22,
  .rows = 12,
  .patch_width = 0.04255f,   // 1.0f / (cols + 1.5f)
  .patch_height = 0.0740f,   // 1.0f / (rows + 1.5f)
  .patch_offset_x = 0.0531f, // 1.25f * patch_size_x
  .patch_offset_y = 0.0925f,  // 1.25f * patch_size_y
  .address = &IT8_7
};

dt_colorchecker_label_t *dt_colorchecker_label_init(const char *label, const dt_color_checker_targets type, const char *path)
{
  size_t label_size = safe_strlen(label) + safe_strlen(path) + sizeof(dt_color_checker_targets);

  dt_colorchecker_label_t *checker_label = malloc(label_size);
  if(!checker_label) return NULL;

  checker_label->label = g_strdup(label);
  checker_label->type = type;
  checker_label->path = path ? g_strdup(path) : NULL;

  return checker_label;
}

dt_color_checker_patch *dt_color_checker_patch_array_init(const size_t num_patches)
{
  dt_color_checker_patch *patches = (dt_color_checker_patch *)dt_alloc_align(num_patches * sizeof(dt_color_checker_patch));
  if(!patches) return NULL;

  // Initialize the patches
  for(size_t i = 0; i < num_patches; i++)
  {
    patches[i].name = NULL;
    patches[i].x = 0.0f;
    patches[i].y = 0.0f;
    patches[i].Lab[0] = 0.0f;
    patches[i].Lab[1] = 0.0f;
    patches[i].Lab[2] = 0.0f;
  }

  return patches;
}

void dt_color_checker_patch_cleanup(const dt_color_checker_patch *patch)
{
  if(!patch) return;
  if(!patch->name) return;

  g_free(patch->name);
}

// This one is to fully free GSList of dt_color_checker_patch
void dt_color_checker_patch_cleanup_list(void *_patch)
{
  dt_color_checker_patch *patch = (dt_color_checker_patch *)_patch;
  if(!patch) return;

  // Free the name if it was allocated
  if(patch->name) g_free(patch->name);

  free(patch);
}

dt_color_checker_t *dt_colorchecker_init()
{                  
  dt_color_checker_t *checker = (dt_color_checker_t*)malloc(sizeof(dt_color_checker_t));

  return checker ? checker : NULL;
}

void dt_color_checker_cleanup(dt_color_checker_t *checker)
{
  if (!checker) return;

  if(checker->name) g_free(checker->name);
  if(checker->author) g_free(checker->author);
  if(checker->date) g_free(checker->date);
  if(checker->manufacturer) g_free(checker->manufacturer);

  if(checker->patches > 0 && checker->values)
  {
    for(size_t i = 0; i < checker->patches; i++)
    {
      dt_color_checker_patch_cleanup(&checker->values[i]);
    }

    dt_free_align(checker->values);
  }
  free(checker);
  checker = NULL;
}

void dt_colorchecker_label_free(gpointer data)
{
  dt_colorchecker_label_t *checker_label = (dt_colorchecker_label_t *)data;

  if(!checker_label) return;

  // Only free if not NULL and if dynamically allocated (user reference)
  if(checker_label->type == COLOR_CHECKER_USER_REF)
  {
    g_free(checker_label->label);
    if(checker_label->path) g_free(checker_label->path); // Builtin checker doesn't use this char dynamically
  }
}

void dt_colorchecker_label_list_cleanup(GList **colorcheckers)
{
  if(!colorcheckers) return;

  g_list_free_full(g_steal_pointer(colorcheckers), dt_colorchecker_label_free);
  *colorcheckers = NULL;
}

void dt_colorchecker_def_list_cleanup(GList **cht)
{
  if(!cht) return;

  g_list_free_full(g_steal_pointer(cht), dt_colorchecker_label_free);
  *cht = NULL;
}

/**
 * @brief Creates a color checker from a user reference file (CGATS format).
 *
 * @param filename the path to the CGATS file.
 * @param cht_filename the path to the .cht file (optional, can be NULL).
 * @return dt_color_checker_t* the filled color checker.
 */
dt_color_checker_t *dt_colorchecker_user_ref_create(const char *filename, const char *cht_filename);

/**
 * @brief Find all .cht files in the user config/color/it8 directory
 * 
 * @param chts NULL GList that will be populated with found IT8 files
 * @return int Number of found files
 */
int dt_colorchecker_find_cht_files(GList **chts);

/**
 * @brief Find all CGAT files in the user config/color/it8 directory
 *
 * @param ref_colorcheckers_files NULL GList that will be populated with found IT8 files
 * @return int Number of found files
 */
int dt_colorchecker_find_CGAT_reference_files(GList **ref_colorcheckers_files);

int dt_colorchecker_find_builtin(GList **colorcheckers_label);

void dt_color_checker_copy(dt_color_checker_t *dest, const dt_color_checker_t *src);


static dt_color_checker_t *dt_get_color_checker(const dt_color_checker_targets target_type, GList **colorchecker_label, const char *cht_filename)
{
  dt_color_checker_t *checker_dest = NULL;
  // cleanup and initialize the destination checker
  checker_dest = dt_colorchecker_init();
  if(!checker_dest) return NULL;

  // check if the target type is a user reference
  dt_color_checker_targets nth_checker = COLOR_CHECKER_LAST;
  const dt_colorchecker_label_t *label_data = NULL;
  if(target_type >= COLOR_CHECKER_USER_REF && colorchecker_label != NULL && *colorchecker_label != NULL)
  {
    dt_print(DT_DEBUG_VERBOSE, _("dt_get_color_checker: colorchecker type %i is a user reference.\n"), target_type);

    // Get the label data from the list
    label_data = g_list_nth_data(*colorchecker_label, target_type);
    nth_checker = COLOR_CHECKER_USER_REF;
  }
  else // it's a builtin colorchecker
    nth_checker = target_type;

  // Copy the color checker data from the predefined checkers
  switch(nth_checker)
  {
    case COLOR_CHECKER_XRITE_24_2000:
      dt_color_checker_copy(checker_dest, &xrite_24_2000);
      break;
    case COLOR_CHECKER_XRITE_24_2014:
      dt_color_checker_copy(checker_dest, &xrite_24_2014);
      break;
    case COLOR_CHECKER_SPYDER_24:
      dt_color_checker_copy(checker_dest,&spyder_24);
      break;
    case COLOR_CHECKER_SPYDER_24_V2:
      dt_color_checker_copy(checker_dest, &spyder_24_v2);
      break;
    case COLOR_CHECKER_SPYDER_48:
      dt_color_checker_copy(checker_dest, &spyder_48);
      break;
    case COLOR_CHECKER_SPYDER_48_V2:
      dt_color_checker_copy(checker_dest, &spyder_48_v2);
      break;
    case COLOR_CHECKER_USER_REF:
      if(label_data)
      {
        dt_color_checker_t *p = dt_colorchecker_user_ref_create(label_data->path, cht_filename);
        if(p)
        {
          dt_color_checker_copy(checker_dest, p);
          dt_color_checker_cleanup(p);
        }
      }
      break;
      
    case COLOR_CHECKER_LAST:
      fprintf(stderr, "dt_get_color_checker: colorchecker type %i not found!\n", target_type);
      dt_color_checker_copy(checker_dest, &xrite_24_2014);
  }

  return checker_dest;
}

/**
 * helper functions
 */

// get a patch index in the list of values from the coordinates of the patch in the checker array
static inline size_t dt_color_checker_get_index(const dt_color_checker_t *const target_checker, const size_t coordinates[2])
{
  // patches are stored column-major
  const size_t height = target_checker->size[1];
  return CLAMP(height * coordinates[0] + coordinates[1], 0, target_checker->patches - 1);
}

// get a a patch coordinates of in the checker array from the patch index in the list of values
static inline void dt_color_checker_get_coordinates(const dt_color_checker_t *const target_checker, size_t *coordinates, const size_t index)
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
static inline const dt_color_checker_patch *dt_color_checker_get_patch_by_name(const dt_color_checker_t *const target_checker, const char *name, size_t *index)
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

/**
 * @brief Find all builtin and CGATS colorcheckers.
 *
 * @param colorcheckers_label the NULL GList that will be populated with found colorcheckers.
 * @return int Number of found colorcheckers.
 */
int dt_colorchecker_find(GList **colorcheckers_label)
{
  int total = dt_colorchecker_find_builtin(colorcheckers_label);
  dt_print(DT_DEBUG_VERBOSE, _("dt_colorchecker_find: found %d builtin colorcheckers\n"), total);
  int b_nb = total;
  total += dt_colorchecker_find_CGAT_reference_files(colorcheckers_label);
  if (total) dt_print(DT_DEBUG_VERBOSE, _("dt_colorchecker_find: found %d CGAT references files\n"), total - b_nb);
  return total;
}

int dt_colorchecker_find_cht(GList **cht)
{
  if(!cht) return 0;

  // Find CGATS files
  const int total = dt_colorchecker_find_cht_files(cht);

  if (total) dt_print(DT_DEBUG_VERBOSE, _("dt_colorchecker_find_cht: found %d .cht files\n"), total);

  return total;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
