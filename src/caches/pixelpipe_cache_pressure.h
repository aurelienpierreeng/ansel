/*
    This file is part of Ansel,
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

#ifndef DT_CACHES_PIXELPIPE_CACHE_PRESSURE_H
#define DT_CACHES_PIXELPIPE_CACHE_PRESSURE_H

/* How much the pixelpipe cache may hold while the kernel reports memory pressure.
 *
 * Pure arithmetic, separated from pixelpipe_cache.c the way develop/pipe_cache_policy.h is:
 * everything here is a decision nothing else can observe -- it changes no pixel and no hash,
 * only how much cache survives a stalling machine -- so the only way to hold it to its rules is
 * to state them where a test can call them. tests/unittests/test_pipe_cache_pressure.c pins
 * both defects this policy was written around, each found on a live run:
 *
 *   - a ceiling that climbed straight back to the plan brought the stall back within half a
 *     minute, five times in five minutes, once at 49% -- one point under systemd-oomd's limit;
 *   - a stall while the cache still held nothing (four kernel wake-ups in the first 12 s of a
 *     run) recorded a pressure mark of 0 and pinned the budget at the floor, where it stayed
 *     for the next quarter of an hour.
 *
 * The caller owns the measurement and the eviction; this owns only the numbers. */

#include <glib.h>
#include <stddef.h>
#include <stdint.h>  // SIZE_MAX

/* The window the stall share is measured over, and what the kernel's own trigger is armed at.
 * Unprivileged PSI triggers require a multiple of 2 s. */
#define DT_PIXELPIPE_CACHE_PSI_WINDOW_US ((gint64)2 * 1000 * 1000)
/* Share of a window spent fully stalled that makes the cache give memory back. systemd-oomd
 * kills at 50% over 20 s, so this leaves room to act before it does. */
#define DT_PIXELPIPE_CACHE_PSI_SHED 0.10
/* ... and under which the budget may start climbing back. */
#define DT_PIXELPIPE_CACHE_PSI_CALM 0.02

/* "Hold what you have": the answer of dt_pixelpipe_cache_pressure_shed_target() when the cache
 * is not what the machine is short of. Distinct from a target of 0, which means "shed it all". */
#define DT_PIXELPIPE_CACHE_PRESSURE_KEEP SIZE_MAX

typedef struct dt_pixelpipe_cache_pressure_t
{
  /** The startup budget: what the cache may hold when nothing is stalling. */
  size_t plan;
  /** What allocations evict down to right now -- the plan, or less under pressure. */
  size_t ceiling;
  /** The footprint the cache had when pressure last struck. The ceiling may not climb past 7/8
   *  of it, and it recovers slowly, so the size that caused a stall is tried again only after
   *  minutes of calm. */
  size_t mark;
} dt_pixelpipe_cache_pressure_t;

/** Below an eighth of the plan, shedding frees nothing worth having and the stall is somebody
 *  else's: the cache neither sheds nor lowers its budget. */
static inline size_t dt_pixelpipe_cache_pressure_floor(const dt_pixelpipe_cache_pressure_t *p)
{
  return p->plan / 8;
}

/** The mark tops out here rather than at the plan, so that 7/8 of it MEETS the plan instead of
 *  the ceiling jumping the last eighth the moment the mark is full. The `+ 8` covers the integer
 *  division: without it, 7/8 of a full mark lands a few hundred bytes under the plan and the
 *  ceiling can never quite come home. Written this way, and not as `plan * 8 / 7`, because the
 *  multiplication overflows a 32-bit size_t for a plan of a few GiB. */
static inline size_t dt_pixelpipe_cache_pressure_mark_top(const dt_pixelpipe_cache_pressure_t *p)
{
  return p->plan / 7 * 8 + 8;
}

static inline void dt_pixelpipe_cache_pressure_init(dt_pixelpipe_cache_pressure_t *p, const size_t plan)
{
  p->plan = plan;
  p->ceiling = plan;
  p->mark = dt_pixelpipe_cache_pressure_mark_top(p);
}

/** What allocations must evict down to. */
static inline size_t dt_pixelpipe_cache_pressure_budget(const dt_pixelpipe_cache_pressure_t *p)
{
  return MIN(p->plan, p->ceiling);
}

/** The highest the ceiling may climb to until the mark recovers further. */
static inline size_t dt_pixelpipe_cache_pressure_cap(const dt_pixelpipe_cache_pressure_t *p)
{
  return MIN(p->plan, p->mark / 8 * 7);
}

/** The budget to report outward (tiling plans against it): never under what is held, since the
 *  readers compute `max - current` unsigned. */
static inline size_t dt_pixelpipe_cache_pressure_reported(const dt_pixelpipe_cache_pressure_t *p,
                                                          const size_t current)
{
  return MAX(dt_pixelpipe_cache_pressure_budget(p), current);
}

/** The footprint to evict down to for a window `share` of which was fully stalled, while holding
 *  `current` bytes -- a quarter of it, half when the stall is severe, and at least 256 MiB so a
 *  nearly-empty cache is not shed one byte at a time. DT_PIXELPIPE_CACHE_PRESSURE_KEEP when the
 *  stall is under the threshold, or when the cache holds too little to be the cause. */
static inline size_t dt_pixelpipe_cache_pressure_shed_target(const dt_pixelpipe_cache_pressure_t *p,
                                                             const double share, const size_t current)
{
  if(share < DT_PIXELPIPE_CACHE_PSI_SHED) return DT_PIXELPIPE_CACHE_PRESSURE_KEEP;
  if(current <= dt_pixelpipe_cache_pressure_floor(p)) return DT_PIXELPIPE_CACHE_PRESSURE_KEEP;

  const size_t step = MAX(current / ((share >= 3. * DT_PIXELPIPE_CACHE_PSI_SHED) ? 2 : 4),
                          (size_t)256 * 1024 * 1024);
  return (current > step) ? current - step : 0;
}

/** Record what a shed achieved: eviction took the footprint from `before` to `after`. The mark
 *  is where pressure struck, and the ceiling follows what is actually left -- never under the
 *  floor, since the ceiling only steers allocations and shedding itself still goes lower while
 *  the pressure lasts. */
static inline void dt_pixelpipe_cache_pressure_after_shed(dt_pixelpipe_cache_pressure_t *p,
                                                          const size_t before, const size_t after)
{
  p->mark = before;
  p->ceiling = MIN(p->ceiling, MAX(after, dt_pixelpipe_cache_pressure_floor(p)));
}

/** One calm window: the mark recovers by 1/1024 of the plan and the ceiling by 1/32, up to the
 *  cap. TRUE when the ceiling actually moved, which is the only thing worth logging. */
static inline gboolean dt_pixelpipe_cache_pressure_relax(dt_pixelpipe_cache_pressure_t *p)
{
  p->mark = MIN(dt_pixelpipe_cache_pressure_mark_top(p), p->mark + p->plan / 1024);

  const size_t cap = dt_pixelpipe_cache_pressure_cap(p);
  if(p->ceiling >= cap) return FALSE;

  p->ceiling = MIN(cap, p->ceiling + p->plan / 32);
  return TRUE;
}

#endif // DT_CACHES_PIXELPIPE_CACHE_PRESSURE_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
