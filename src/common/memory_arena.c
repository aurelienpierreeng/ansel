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

#include "common/memory_arena.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

typedef struct dt_free_run_t
{
  uint32_t start;
  uint32_t length;
} dt_free_run_t;

gboolean dt_cache_arena_calc(const dt_cache_arena_t *a,
                             size_t size,
                             uint32_t *out_pages,
                             size_t *out_size)
{
  if(!a || !a->base || !out_pages || a->page_size == 0 || a->num_pages == 0) return FALSE;
  if(size == 0) return FALSE;
  if(size > SIZE_MAX - (a->page_size - 1)) return FALSE;

  const size_t pages = (size + a->page_size - 1) / a->page_size;
  if(pages > a->num_pages || pages > UINT32_MAX) return FALSE;
  if(out_size)
  {
    if(pages > SIZE_MAX / a->page_size) return FALSE;
    *out_size = pages * a->page_size;
  }

  *out_pages = (uint32_t)pages;
  return TRUE;
}

/*
 * Allocate from the arena in page-sized chunks.
 * Uses a best-fit scan over the sorted free-run list, then consumes from
 * the beginning of the selected run. On success, returns a pointer into the
 * arena and writes the page-rounded allocation size to out_size.
 */
void *dt_cache_arena_alloc(dt_cache_arena_t *a,
                           size_t size,
                           size_t *out_size)
{
  if(!a || !a->base || !out_size) return NULL;

  uint32_t pages_needed = 0;
  size_t rounded_size = 0;
  if(!dt_cache_arena_calc(a, size, &pages_needed, &rounded_size)) return NULL;

  dt_pthread_mutex_lock(&a->lock);

  guint best_index = G_MAXUINT;
  uint32_t best_length = UINT32_MAX;

  for(guint i = 0; i < a->free_runs->len; i++)
  {
    dt_free_run_t *r = &g_array_index(a->free_runs, dt_free_run_t, i);
    if(r->length >= pages_needed && r->length < best_length)
    {
      best_index = i;
      best_length = r->length;
      if(best_length == pages_needed) break; // exact fit
    }
  }

  if(best_index == G_MAXUINT)
  {
    dt_pthread_mutex_unlock(&a->lock);
    return NULL;
  }

  dt_free_run_t *r = &g_array_index(a->free_runs, dt_free_run_t, best_index);
  const uint32_t first = r->start;

  /* consume from the front of the run so the list stays sorted */
  r->start += pages_needed;
  r->length -= pages_needed;

  /* remove empty run after consumption */
  if(r->length == 0)
    g_array_remove_index(a->free_runs, best_index);

  dt_pthread_mutex_unlock(&a->lock);

  *out_size = rounded_size;
  return a->base + (size_t)first * a->page_size;
}


/*
 * Return a previously allocated region to the arena.
 * The pointer must refer to the arena base, and size is rounded up to pages.
 * The freed run is inserted in order and coalesced with adjacent runs.
 */
void dt_cache_arena_free(dt_cache_arena_t *a,
                         void *ptr,
                         size_t size)
{
  if(!a || !a->base || !a->free_runs || a->page_size == 0 || a->num_pages == 0)
    return;
  if(!ptr || size == 0) return;

  const uintptr_t base = (uintptr_t)a->base;
  const uintptr_t addr = (uintptr_t)ptr;
  if(addr < base || addr >= base + a->size)
  {
    fprintf(stderr, "[pixelpipe] arena free: pointer out of range\n");
    return;
  }
  if(((addr - base) % a->page_size) != 0)
  {
    fprintf(stderr, "[pixelpipe] arena free: pointer not page-aligned\n");
    return;
  }

  uint32_t pages = 0;
  if(!dt_cache_arena_calc(a, size, &pages, NULL))
  {
    fprintf(stderr, "[pixelpipe] arena free: invalid size\n");
    return;
  }

  const size_t first_sz = (addr - base) / a->page_size;
  if(first_sz >= a->num_pages || pages > a->num_pages - first_sz)
  {
    fprintf(stderr, "[pixelpipe] arena free: range out of bounds\n");
    return;
  }

  const uint32_t first = (uint32_t)first_sz;

  dt_pthread_mutex_lock(&a->lock);

  /* insert a new free run, keeping free_runs sorted by start page */
  guint i = 0;
  while(i < a->free_runs->len &&
        g_array_index(a->free_runs, dt_free_run_t, i).start < first)
    i++;

  if(i > 0)
  {
    dt_free_run_t *prev = &g_array_index(a->free_runs, dt_free_run_t, i - 1);
    if(prev->start + prev->length > first)
    {
      dt_pthread_mutex_unlock(&a->lock);
      fprintf(stderr, "[pixelpipe] arena free: overlap with previous run\n");
      return;
    }
  }
  if(i < a->free_runs->len)
  {
    dt_free_run_t *next = &g_array_index(a->free_runs, dt_free_run_t, i);
    if(first + pages > next->start)
    {
      dt_pthread_mutex_unlock(&a->lock);
      fprintf(stderr, "[pixelpipe] arena free: overlap with next run\n");
      return;
    }
  }

  dt_free_run_t new = { first, pages };
  g_array_insert_val(a->free_runs, i, new);

  /* coalesce with next run if adjacent */
  if(i + 1 < a->free_runs->len)
  {
    dt_free_run_t *cur = &g_array_index(a->free_runs, dt_free_run_t, i);
    dt_free_run_t *next = &g_array_index(a->free_runs, dt_free_run_t, i + 1);
    if(cur->start + cur->length == next->start)
    {
      cur->length += next->length;
      g_array_remove_index(a->free_runs, i + 1);
    }
  }

  /* coalesce with previous run if adjacent */
  if(i > 0)
  {
    dt_free_run_t *prev = &g_array_index(a->free_runs, dt_free_run_t, i - 1);
    dt_free_run_t *cur  = &g_array_index(a->free_runs, dt_free_run_t, i);
    if(prev->start + prev->length == cur->start)
    {
      prev->length += cur->length;
      g_array_remove_index(a->free_runs, i);
    }
  }

  dt_pthread_mutex_unlock(&a->lock);
}

void dt_cache_arena_stats(dt_cache_arena_t *a,
                          uint32_t *out_total_free_pages,
                          uint32_t *out_largest_free_run_pages)
{
  if(out_total_free_pages) *out_total_free_pages = 0;
  if(out_largest_free_run_pages) *out_largest_free_run_pages = 0;
  if(!a || !a->free_runs) return;

  dt_pthread_mutex_lock(&a->lock);
  uint32_t total = 0;
  uint32_t largest = 0;
  for(guint i = 0; i < a->free_runs->len; i++)
  {
    dt_free_run_t *r = &g_array_index(a->free_runs, dt_free_run_t, i);
    total += r->length;
    if(r->length > largest) largest = r->length;
  }
  dt_pthread_mutex_unlock(&a->lock);

  if(out_total_free_pages) *out_total_free_pages = total;
  if(out_largest_free_run_pages) *out_largest_free_run_pages = largest;
}

void dt_cache_arena_cleanup(dt_cache_arena_t *a)
{
  if(!a) return;

  dt_pthread_mutex_lock(&a->lock);
  g_array_free(a->free_runs, TRUE);
  dt_pthread_mutex_unlock(&a->lock);

  dt_pthread_mutex_destroy(&a->lock);

  /* 5. Release the virtual memory block */
  if(a->base && a->size)
  {
#ifdef _WIN32
    VirtualFree(a->base, 0, MEM_RELEASE);
#else
    munmap(a->base, a->size);
#endif
  }

  /* 6. Poison the struct (defensive) */
  a->base = NULL;
  a->size = 0;
  a->num_pages = 0;
  a->page_size = 0;
}

// return 0 on success 1 on error
int dt_cache_arena_init(dt_cache_arena_t *a, size_t total_size)
{
  const size_t page_size = 64 * 1024; // 64 KiB cache pages
  const size_t pages = total_size / page_size;

#ifdef _WIN32
  a->base = (uint8_t *)VirtualAlloc(NULL, total_size,
                                    MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
  if(!a->base)
  {
    const DWORD err = GetLastError();
    fprintf(stderr, "couldn't alloc map (VirtualAlloc error %lu)\n", (unsigned long)err);
    return 1;
  }
#else
  a->base = mmap(NULL, total_size,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS,
                 -1, 0);

  if(a->base == MAP_FAILED)
  {
    a->base = NULL;
    fprintf(stderr, "couldn't alloc map (mmap error %d: %s)\n", errno, strerror(errno));
    return 1;
  }
#endif

  a->size = total_size;
  a->page_size = page_size;
  a->num_pages = pages;

  a->free_runs = g_array_new(FALSE, FALSE, sizeof(dt_free_run_t));
  if(!a->free_runs)
  {
#ifdef _WIN32
    VirtualFree(a->base, 0, MEM_RELEASE);
#else
    munmap(a->base, a->size);
#endif
    a->base = NULL;
    a->size = 0;
    a->page_size = 0;
    a->num_pages = 0;
    fprintf(stderr, "couldn't alloc free run list\n");
    return 1;
  }

  /* start with one free run covering the whole arena */
  dt_free_run_t full = {
    .start  = 0,
    .length = a->num_pages
  };

  g_array_append_val(a->free_runs, full);

  dt_pthread_mutex_init(&a->lock, NULL);
  return 0;
}

gboolean dt_cache_arena_ptr_in(const dt_cache_arena_t *a, const void *ptr)
{
  if(!a || !a->base || !ptr) return FALSE;
  const uintptr_t base = (uintptr_t)a->base;
  const uintptr_t addr = (uintptr_t)ptr;
  return addr >= base && addr < base + a->size;
}
