/*
    This file is part of darktable,
    Copyright (C) 2020 Aurélien PIERRE.
    Copyright (C) 2023 Luca Zulberti.
    Copyright (C) 2026 Guillaume Stutin.
    
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


kernel void
negadoctor (read_only image2d_t in, write_only image2d_t out, int width, int height,
            const float4 Dmin, const float4 wb_high, const float4 offset,
            const float exposure, const float black, const float gamma, const float soft_clip, const float soft_clip_comp,
            const float4 luminance, const float color_gamma)
{
  const unsigned int x = get_global_id(0);
  const unsigned int y = get_global_id(1);

  if(x >= width || y >= height) return;

  float4 i = read_imagef(in, sampleri, (int2)(x, y));
  float4 o;

  // Convert transmission to density using Dmin as a fulcrum
  o = -native_log10(Dmin / fmax(i, (float4)2.3283064365386963e-10f)); // threshold to -32 EV

  // Correct density in log space
  o = wb_high * o + offset;

  // Print density on paper : ((1 - 10^corrected_de + black) * exposure)^gamma rewritten for FMA
  o = fmax(-((float4)exposure * native_exp10(o) + (float4)black), (float4)0.0f);

  // The paper grade raises the luminance of the print to its power, and the ratios of the channels to the
  // luminance to color_gamma: at color_gamma = gamma this is each channel raised to gamma.
  if(color_gamma != gamma)
  {
    const float grey = luminance.x * o.x + luminance.y * o.y + luminance.z * o.z;
    o = (grey > 0.0f) ? native_powr(grey, gamma) * ((color_gamma > 0.0f) ? native_powr(o / grey, color_gamma) : (float4)1.0f)
                      : (float4)0.0f;
  }
  else
    o = native_powr(o, gamma); // note : this is always > 0

  // Compress highlights and clip negatives. from https://lists.gnu.org/archive/html/openexr-devel/2005-03/msg00009.html
  o = (o > (float4)soft_clip) ? soft_clip + ((float4)1.0f - native_exp(-(o - (float4)soft_clip) / (float4)soft_clip_comp)) * (float4)soft_clip_comp
                              : o;

  // Copy alpha
  o.w = i.w;

  write_imagef(out, (int2)(x, y), o);
}
