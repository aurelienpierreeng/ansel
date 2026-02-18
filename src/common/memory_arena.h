/*
    This file is part of Ansel
    Copyright (C) 2026 - Aurélien PIERRE

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include "common/dtpthread.h"
#include <glib.h>
#include <stdint.h>

/*
 * Arena allocator for cache buffers:
 * - We reserve one big contiguous block of virtual memory (the arena).
 * - The arena is split into fixed-size pages (page_size).
 * - free_runs is a sorted list of "free stretches" of pages.
 *   Each run says "from page N, K pages are free".
 * This avoids many small malloc/free calls: we just carve out page ranges
 * and put them back into the list when done.
 */
typedef struct dt_cache_arena_t
{
  uint8_t *base;
  size_t   size;

  size_t   page_size;
  uint32_t num_pages;

  GArray *free_runs; // sorted list of free page runs (start page + length in pages)

  dt_pthread_mutex_t lock;
} dt_cache_arena_t;

gboolean dt_cache_arena_calc(const dt_cache_arena_t *a,
                             size_t size,
                             uint32_t *out_pages,
                             size_t *out_size);

void *dt_cache_arena_alloc(dt_cache_arena_t *a,
                           size_t size,
                           size_t *out_size);

void dt_cache_arena_free(dt_cache_arena_t *a,
                         void *ptr,
                         size_t size);

int dt_cache_arena_init(dt_cache_arena_t *a, size_t total_size);
void dt_cache_arena_cleanup(dt_cache_arena_t *a);

gboolean dt_cache_arena_ptr_in(const dt_cache_arena_t *a, const void *ptr);
