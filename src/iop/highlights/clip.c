/*
   This file is part of darktable,
   Copyright (C) 2010 Bruce Guenter.
   Copyright (C) 2010-2011 Henrik Andersson.
   Copyright (C) 2010-2014, 2016 johannes hanika.
   Copyright (C) 2010 Stuart Henderson.
   Copyright (C) 2011 Antony Dovgal.
   Copyright (C) 2011 Robert Bieber.
   Copyright (C) 2011-2014, 2016, 2019 Tobias Ellinghaus.
   Copyright (C) 2011-2012, 2014, 2016-2017 Ulrich Pegelow.
   Copyright (C) 2012, 2015 Edouard Gomez.
   Copyright (C) 2012 Jérémy Rosen.
   Copyright (C) 2012 Richard Wonka.
   Copyright (C) 2013, 2020 Aldric Renaudin.
   Copyright (C) 2014, 2016 Dan Torop.
   Copyright (C) 2014-2016 Roman Lebedev.
   Copyright (C) 2015-2016 Pedro Côrte-Real.
   Copyright (C) 2017 Heiko Bauke.
   Copyright (C) 2017 luzpaz.
   Copyright (C) 2018, 2020-2026 Aurélien PIERRE.
   Copyright (C) 2018 Edgardo Hoszowski.
   Copyright (C) 2018 Maurizio Paglia.
   Copyright (C) 2018-2020, 2022 Pascal Obry.
   Copyright (C) 2018 rawfiner.
   Copyright (C) 2019 Andreas Schneider.
   Copyright (C) 2019 Diederik ter Rahe.
   Copyright (C) 2019-2020, 2022 Hanno Schwalm.
   Copyright (C) 2020 Chris Elston.
   Copyright (C) 2020, 2022 Diederik Ter Rahe.
   Copyright (C) 2020-2021 Ralf Brown.
   Copyright (C) 2021 Hubert Kowalski.
   Copyright (C) 2022 Martin Bařinka.
   Copyright (C) 2022 Philipp Lutz.
   Copyright (C) 2022 Victor Forsiuk.
   Copyright (C) 2023 Alynx Zhou.
   Copyright (C) 2023 Guillaume Stutin.
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

// Simple highlight-clip and the raw-clip visualization mode. (implementation; see clip.h for the public API.)

#include "common/openmp.h"
#include "common/target_clones.h"
#include "develop/imageop_math.h"
#include "iop/highlights/clip.h"
#include <string.h>

__DT_CLONE_TARGETS__
void process_clip(const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out, const float clip)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;

  if(piece->dsc_in.filters)
  { // raw mosaic
    __OMP_PARALLEL_FOR_SIMD__()
    for(size_t k = 0; k < (size_t)roi_out->width * roi_out->height; k++)
    {
      out[k] = MIN(clip, in[k]);
    }
  }
  else
  {
    const int ch = piece->dsc_in.channels;
    __OMP_PARALLEL_FOR_SIMD__()
    for(size_t k = 0; k < (size_t)ch * roi_out->width * roi_out->height; k++)
    {
      out[k] = MIN(clip, in[k]);
    }
  }
}

__DT_CLONE_TARGETS__
void process_visualize(const dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *const ovoid,
                       const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out, const uint32_t filters,
                       dt_iop_highlights_data_t *data)
{
  const float *const in = (const float *const)ivoid;
  float *const out = (float *const)ovoid;
  const size_t width = roi_out->width;
  const size_t height = roi_out->height;
  const float clips[4] = { 0.995f * data->clip * piece->dsc_in.processed_maximum[0],
                           0.995f * data->clip * piece->dsc_in.processed_maximum[1],
                           0.995f * data->clip * piece->dsc_in.processed_maximum[2], data->clip };
  __OMP_FOR_SIMD__(aligned(in, out : 64))
  for(size_t row = 0; row < height; row++)
  {
    for(size_t col = 0, i = row * width; col < width; col++, i++)
    {
      const int c = FC(row, col, filters);
      const float ival = in[i];
      out[i] = (ival < clips[c]) ? 0.2f * ival : 1.0f;
    }
  }
}
