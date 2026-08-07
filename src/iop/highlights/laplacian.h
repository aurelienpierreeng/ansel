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

#ifndef DT_IOP_HIGHLIGHTS_LAPLACIAN_H
#define DT_IOP_HIGHLIGHTS_LAPLACIAN_H

// Guided-laplacian (2021 a-trous) highlight reconstruction, CPU + OpenCL.
// Public API of this highlights mode (a compiled TU); internals are static in the .c.

#include "develop/imageop.h"

// Single CPU driver for guided-laplacian reconstruction, for Bayer, X-Trans and already-demosaiced
// (non-raw / sRAW) input alike. The wavelet reconstruction is CFA-agnostic; only the disposable-demosaic
// gather and the remosaic scatter branch on the sensor layout, selected internally via _hl_cfa_strategy().
int process_laplacian(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                      const dt_dev_pixelpipe_iop_t *piece, const void *const restrict ivoid,
                      void *const restrict ovoid, const dt_iop_roi_t *const roi_in,
                      const dt_iop_roi_t *const roi_out, const dt_aligned_pixel_t clips);

#ifdef HAVE_OPENCL
cl_int process_laplacian_bayer_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                  const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                  const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                  const dt_aligned_pixel_t clips);

cl_int process_laplacian_xtrans_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                   const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                   const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                   const dt_aligned_pixel_t clips);

// Non-raw / sRAW guided-laplacian on the GPU: the disposable-demosaic gather is a device plane copy
// (already-RGB input) and the remosaic a per-channel device composite; the wavelet reconstruction in
// between is the same CFA-agnostic device path as the Bayer/X-Trans drivers. Runs fully on-device.
cl_int process_laplacian_passthrough_cl(struct dt_iop_module_t *self, const dt_dev_pixelpipe_t *pipe,
                                        const dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
                                        const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out,
                                        const dt_aligned_pixel_t clips);
#endif // HAVE_OPENCL
#endif // DT_IOP_HIGHLIGHTS_LAPLACIAN_H
