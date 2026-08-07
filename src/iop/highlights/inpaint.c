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

// Colour-inpainting highlight reconstruction (a1ex's magiclantern idea). (implementation; see inpaint.h
// for the public API.) The per-line colour interpolators live in the LCh TU (interpolate_color /
// interpolate_color_xtrans, lch.h); this TU only drives them along the four axis directions.

#include "system/openmp.h"
#include "iop/highlights/common.h"
#include "system/target_clones.h"
#include "iop/highlights/inpaint.h"
#include "iop/highlights/lch.h"

__DT_CLONE_TARGETS__
void process_inpaint_bayer(const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_out,
                           const float clips[4], const uint32_t filters)
{
  // left/right directions
  __OMP_PARALLEL_FOR__()
  for(int j = 0; j < roi_out->height; j++)
  {
    interpolate_color(ivoid, ovoid, roi_out, 0, 1, j, clips, filters, 0);
    interpolate_color(ivoid, ovoid, roi_out, 0, -1, j, clips, filters, 1);
  }

  // up/down directions
  __OMP_PARALLEL_FOR__()
  for(int i = 0; i < roi_out->width; i++)
  {
    interpolate_color(ivoid, ovoid, roi_out, 1, 1, i, clips, filters, 2);
    interpolate_color(ivoid, ovoid, roi_out, 1, -1, i, clips, filters, 3);
  }
}

__DT_CLONE_TARGETS__
void process_inpaint_xtrans(const void *const ivoid, void *const ovoid, const dt_iop_roi_t *const roi_in,
                            const dt_iop_roi_t *const roi_out, const float clips[4],
                            const uint8_t (*const xtrans)[6])
{
  // left/right directions
  __OMP_PARALLEL_FOR__()
  for(int j = 0; j < roi_out->height; j++)
  {
    interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, 1, j, clips, xtrans, 0);
    interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 0, -1, j, clips, xtrans, 1);
  }

  // up/down directions
  __OMP_PARALLEL_FOR__()
  for(int i = 0; i < roi_out->width; i++)
  {
    interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, 1, i, clips, xtrans, 2);
    interpolate_color_xtrans(ivoid, ovoid, roi_in, roi_out, 1, -1, i, clips, xtrans, 3);
  }
}
