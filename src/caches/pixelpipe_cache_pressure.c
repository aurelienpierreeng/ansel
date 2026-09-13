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

/* When the pixelpipe cache gives memory back, and how much.
 *
 * The cache budget is a plan made at startup, and the available-RAM valve in pixelpipe_cache.c
 * guards a floor of free memory. Neither sees a system that still reports memory available but
 * spends its time reclaiming it: swap full, other applications' pages evicted and faulted straight
 * back in. That stall is what systemd-oomd watches -- on an Ubuntu 24.04 user session, past 50%
 * "full" stall of user@.service for 20 s it kills the cgroup under it reclaiming the most, which
 * on a photo workstation is us -- while MemAvailable is still far above any floor.
 *
 * So every DT_PIXELPIPE_CACHE_PSI_WINDOW_US, measure the share of the window during which every
 * task was stalled on memory, the highest over the system and each cgroup above us. Past
 * DT_PIXELPIPE_CACHE_PSI_SHED, shed a quarter of the cache (half past three times that), hand the
 * pages to the OS, and lower the budget to what is left, so the pipes do not fill it straight
 * back up; the next window says whether that was enough.
 *
 * That measurement runs where the cache is already doing something -- an allocation, the idle
 * shedder's timer -- and neither happens once the stall is severe: a machine thrashing stops the
 * GUI thread's timers and the pipelines alike. Measured: a run went from calm to dead with the
 * last 32 seconds silent, the process frozen with 13.6 GB resident while oomd counted its 20
 * seconds. So the reaction does not wait for the cache to run: a watcher sleeps on the kernel's
 * own PSI triggers (dt_memory_pressure_watch_start()), which the kernel raises as soon as a window
 * is stalled past the same threshold, and sheds from there. It calls back on its own thread, which
 * is why every function here is documented as running under the cache's lock: the cache takes it
 * before calling in, from that thread like from any other.
 *
 * Once windows run calm, the budget climbs back by 1/32 of the plan per window, but only to 7/8 of
 * the footprint the cache had when pressure struck; that mark itself rises by 1/1024 of the plan
 * per calm window, so the size that caused the stall is tried again only after minutes of calm.
 * Climbing straight back to the plan brought the stall back within half a minute, every time, and
 * once to 49%, one point under systemd-oomd's limit.
 *
 * The share comes from PSI's cumulative `total` counters, not from its avg10: that is a 10 s
 * moving average, which keeps reading high for some 20 s after a stall has ended, and shedding on
 * it would drain the whole cache for pressure that was already gone.
 *
 * The arithmetic of the ceiling and the mark is in the header, where a test can reach it. */

#include "caches/pixelpipe_cache_pressure.h"

#include "common/logging.h"
#include "system/macros.h"

#include <string.h>

void dt_pixelpipe_cache_pressure_monitor_init(dt_pixelpipe_cache_pressure_monitor_t *m, const size_t plan)
{
  memset(m->totals, 0, sizeof(m->totals));
  m->levels = 0;
  m->time_us = 0;
  m->shed_time_us = 0;
  m->watch = NULL;
  dt_pixelpipe_cache_pressure_init(&m->budget, plan);
}

/* Full-stall share of the window since the previous read, the highest over the levels
 * dt_memory_pressure_read_full_stall() reports. -1 before `min_window_us` has elapsed, on the
 * first read, and where the platform has no PSI. The kernel-woken path passes a shorter
 * `min_window_us` than the polling ones: it already knows a window was stalled, and only reads
 * the counters to tell a bad stall from a catastrophic one. WARNING: non thread-safe */
static double _window_share(dt_pixelpipe_cache_pressure_monitor_t *m, const gint64 now,
                            const gint64 min_window_us)
{
  if(m->time_us != 0 && now - m->time_us < min_window_us) return -1.;

  uint64_t totals[DT_MEMORY_PRESSURE_MAX_LEVELS] = { 0 };
  const int levels = dt_memory_pressure_read_full_stall(totals, DT_MEMORY_PRESSURE_MAX_LEVELS);
  const gint64 elapsed = now - m->time_us;

  double share = -1.;
  if(m->time_us != 0 && levels > 0 && levels == m->levels)
  {
    share = 0.;
    for(int i = 0; i < levels; i++)
      if(totals[i] >= m->totals[i])
        share = MAX(share, (double)(totals[i] - m->totals[i]) / (double)elapsed);
  }

  memcpy(m->totals, totals, sizeof(m->totals));
  m->levels = levels;
  m->time_us = now;
  return share;
}

// Give memory back for a window `share` of which was fully stalled. Returns the bytes shed.
// WARNING: non thread-safe
static size_t _shed(dt_pixelpipe_cache_pressure_monitor_t *m,
                    const dt_pixelpipe_cache_pressure_sink_t *sink, const double share, const char *origin)
{
  const size_t before = sink->held(sink->user);
  const size_t target = dt_pixelpipe_cache_pressure_shed_target(&m->budget, share, before);
  if(target == DT_PIXELPIPE_CACHE_PRESSURE_KEEP) return 0;

  size_t given_back = 0;
  const size_t after = sink->shed(sink->user, target, &given_back);

  dt_pixelpipe_cache_pressure_after_shed(&m->budget, before, after);
  m->shed_time_us = g_get_monotonic_time();

  const size_t freed = before - after;
  dt_print(DT_DEBUG_MEMORY | DT_DEBUG_PIPECACHE,
           "[pixelpipe_cache] kernel memory pressure (%s): %.0f%% of the window stalled -- shed %" G_GSIZE_FORMAT
           " MiB of cache, returned %" G_GSIZE_FORMAT " MiB to the OS, budget lowered to %" G_GSIZE_FORMAT " MiB\n",
           origin, 100. * share, freed / (1024 * 1024), given_back / (1024 * 1024),
           dt_pixelpipe_cache_pressure_budget(&m->budget) / (1024 * 1024));
  return freed;
}

// Two paths shed: the kernel's wake-up and the windows measured here. Neither needs to repeat what
// the other just did. WARNING: non thread-safe
static inline gboolean _shed_too_soon(const dt_pixelpipe_cache_pressure_monitor_t *m, const gint64 now)
{
  return m->shed_time_us != 0 && now - m->shed_time_us < DT_PIXELPIPE_CACHE_PSI_WINDOW_US / 2;
}

size_t dt_pixelpipe_cache_pressure_react(dt_pixelpipe_cache_pressure_monitor_t *m,
                                         const dt_pixelpipe_cache_pressure_sink_t *sink)
{
  const gint64 now = g_get_monotonic_time();
  const double share = _window_share(m, now, DT_PIXELPIPE_CACHE_PSI_WINDOW_US);
  if(share < 0.) return 0;

  if(share >= DT_PIXELPIPE_CACHE_PSI_SHED)
    return _shed_too_soon(m, now) ? 0 : _shed(m, sink, share, "measured");

  if(share < DT_PIXELPIPE_CACHE_PSI_CALM && dt_pixelpipe_cache_pressure_relax(&m->budget))
    dt_print(DT_DEBUG_MEMORY | DT_DEBUG_PIPECACHE,
             "[pixelpipe_cache] kernel memory pressure calm: budget raised to %" G_GSIZE_FORMAT
             " MiB (at most %" G_GSIZE_FORMAT " MiB until the pressure mark recovers)\n",
             dt_pixelpipe_cache_pressure_budget(&m->budget) / (1024 * 1024),
             dt_pixelpipe_cache_pressure_cap(&m->budget) / (1024 * 1024));
  return 0;
}

/* The kernel raised a trigger: a window was stalled past DT_PIXELPIPE_CACHE_PSI_SHED, so shed
 * without waiting for a window of our own to close. The counters still say HOW stalled it was --
 * over whatever has elapsed since the last read, which the trigger guarantees is recent -- and
 * that is what picks a quarter or a half; when the last read is too close to tell, the threshold
 * the trigger fired at stands in. */
void dt_pixelpipe_cache_pressure_triggered(dt_pixelpipe_cache_pressure_monitor_t *m,
                                           const dt_pixelpipe_cache_pressure_sink_t *sink)
{
  const gint64 now = g_get_monotonic_time();
  if(_shed_too_soon(m, now)) return;

  const double measured = _window_share(m, now, DT_PIXELPIPE_CACHE_PSI_WINDOW_US / 8);
  _shed(m, sink, MAX(measured, DT_PIXELPIPE_CACHE_PSI_SHED), "kernel wake-up");
}

void dt_pixelpipe_cache_pressure_watch_start(dt_pixelpipe_cache_pressure_monitor_t *m,
                                             void (*wake)(void *user), void *user)
{
  m->watch = dt_memory_pressure_watch_start((uint64_t)(DT_PIXELPIPE_CACHE_PSI_SHED
                                                       * DT_PIXELPIPE_CACHE_PSI_WINDOW_US),
                                            (uint64_t)DT_PIXELPIPE_CACHE_PSI_WINDOW_US, wake, user);

  // No watcher: the measured windows still react, just not while nothing of ours runs.
  if(!IS_NULL_PTR(m->watch))
    dt_print(DT_DEBUG_MEMORY | DT_DEBUG_PIPECACHE,
             "[pixelpipe_cache] watching kernel memory pressure on %i level(s)\n",
             dt_memory_pressure_watch_levels(m->watch));
}

void dt_pixelpipe_cache_pressure_watch_stop(dt_pixelpipe_cache_pressure_monitor_t *m)
{
  dt_memory_pressure_watch_stop(&m->watch);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
