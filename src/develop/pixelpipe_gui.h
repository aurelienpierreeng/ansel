/*
    This file is part of the Ansel project.
    Copyright (C) 2026 Aurélien PIERRE.

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

#pragma once

#include "develop/develop.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize GUI-side histogram sampling callbacks.
 *
 * @details
 * Histogram sampling is performed on the GUI thread after the preview pipe
 * finished and published its cachelines. This initializer connects the preview
 * finished signal to that direct cache sampling path.
 */
void dt_dev_pixelpipe_gui_init(void);

/**
 * @brief Disconnect GUI-side histogram sampling callbacks.
 */
void dt_dev_pixelpipe_gui_cleanup(void);

/**
 * @brief Return one of the develop-owned global histogram backbuffers.
 *
 * @details
 * Global histogram widgets keep stable `dt_backbuf_t` containers in
 * `dt_develop_t`, while the actual cacheline hash they point to is refreshed
 * from the preview pipe on every completed recompute.
 */
dt_backbuf_t *dt_dev_get_histogram_backbuf(dt_develop_t *dev, const char *op);

/**
 * @brief Refresh all GUI-visible histogram sources from the preview cache.
 *
 * @details
 * This updates:
 * - the three global histogram stage backbuffers (`demosaic`, `colorout`, `gamma`),
 * - every module-local histogram requested on the preview pipe.
 *
 * All reads happen directly from preview cachelines identified through the
 * immutable piece contracts currently present in `dev->preview_pipe->nodes`.
 */
void dt_dev_refresh_preview_histograms(dt_develop_t *dev);

/**
 * @brief Refresh the histogram owned by one module from the preview cache.
 *
 * @return TRUE if a fresh histogram was sampled from cache, FALSE otherwise.
 */
gboolean dt_dev_refresh_module_histogram(dt_develop_t *dev, dt_iop_module_t *module);

/**
 * @brief Tell whether the module output itself feeds one global histogram stage.
 */
gboolean dt_dev_module_requires_global_histogram_output_cache(const dt_dev_pixelpipe_t *pipe,
                                                              const dt_iop_module_t *module);

/**
 * @brief Tell whether the previous module output feeds one global histogram stage through this module.
 *
 * @details
 * `gamma` is sampled through its input cacheline because its output is `uint8_t`
 * while the histogram code expects float host buffers.
 */
gboolean dt_dev_module_requires_global_histogram_input_cache(const dt_dev_pixelpipe_t *pipe,
                                                             const dt_iop_module_t *module);

#ifdef __cplusplus
}
#endif
