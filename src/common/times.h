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
#ifndef DT_COMMON_TIMES_H
#define DT_COMMON_TIMES_H

/* Wall-clock / CPU-time measurement helpers. The dt_show_times* implementations
 * live in common/darktable.c (they honor the runtime perf-debug flag), but the
 * declarations are application-free: low-level code includes this instead of
 * common/darktable.h. */

#ifdef _WIN32
#include "win/getrusage.h"
#else
#include <sys/resource.h>
#endif
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  double clock;
  double user;
} dt_times_t;

static inline double dt_get_wtime(void)
{
  struct timeval time;
  gettimeofday(&time, NULL);
  return time.tv_sec - 1290608000 + (1.0 / 1000000.0) * time.tv_usec;
}

static inline void dt_get_times(dt_times_t *t)
{
  struct rusage ru;

  getrusage(RUSAGE_SELF, &ru);
  t->clock = dt_get_wtime();
  t->user = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec * (1.0 / 1000000.0);
}

/* Wall-clock time (dt_get_wtime() domain) at which the application started.
 * Written once at startup, never mutated; implemented in common/darktable.c. */
double dt_get_start_wtime(void);

void dt_show_times(const dt_times_t *start, const char *prefix);

void dt_show_times_f(const dt_times_t *start, const char *prefix, const char *suffix, ...) __attribute__((format(printf, 3, 4)));

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_TIMES_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
