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

#include "system/memory_pressure.h"
#include "system/macros.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#if defined(__linux__)
#include <errno.h>          // conditional-ok: only the watcher's poll() below reports EINTR
#include <pthread.h>        // conditional-ok: idem -- the thread the watcher sleeps on
#include <fcntl.h>          // conditional-ok: idem -- every PSI file is opened inside this same #ifdef
#include <poll.h>           // conditional-ok: idem -- the watcher sleeps on those descriptors
#include <sys/eventfd.h>    // conditional-ok: idem -- the eventfd that wakes it to stop
#include <unistd.h>         // conditional-ok: idem -- read(), write() and close() on those descriptors

// The `total` of the "full" line of one PSI file. FALSE when the file is absent (a kernel
// without PSI, a cgroup level without the memory controller) or unreadable.
static gboolean _read_full_total(const char *file, uint64_t *total_us)
{
  FILE *f = g_fopen(file, "r");
  if(IS_NULL_PTR(f)) return FALSE;

  gboolean found = FALSE;
  char line[256];
  while(!found && fgets(line, sizeof(line), f))
  {
    unsigned long long total = 0;
    if(sscanf(line, "full avg10=%*f avg60=%*f avg300=%*f total=%llu", &total) == 1)
    {
      *total_us = (uint64_t)total;
      found = TRUE;
    }
  }
  fclose(f);
  return found;
}

// The process's cgroup v2 path, "/user.slice/...", or an empty string when it has none.
static void _own_cgroup_path(char *path, const size_t size)
{
  path[0] = '\0';
  FILE *f = g_fopen("/proc/self/cgroup", "r");
  if(IS_NULL_PTR(f)) return;

  char line[512];
  while(fgets(line, sizeof(line), f))
  {
    // cgroup v2 unified hierarchy entry: "0::/user.slice/..."
    if(!strncmp(line, "0::", 3))
    {
      g_strlcpy(path, line + 3, size);
      char *newline = strchr(path, '\n');
      if(newline) *newline = '\0';
      break;
    }
  }
  fclose(f);
}

// Whether a cgroup path names a level below the root.
static gboolean _cgroup_below_root(const char *path)
{
  return path[0] == '/' && path[1] != '\0';
}

// Moves a cgroup path to its parent, in place. FALSE once it was a top-level cgroup: the root has
// no memory.pressure, and the system-wide file stands for it.
static gboolean _cgroup_parent(char *path)
{
  char *slash = strrchr(path, '/');
  if(IS_NULL_PTR(slash) || slash == path) return FALSE;
  *slash = '\0';
  return TRUE;
}

static int _open_trigger(const char *file, const char *trigger)
{
  const int fd = open(file, O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if(fd < 0) return -1;

  // The kernel reads the trigger up to its terminating NUL, which is written too.
  if(write(fd, trigger, strlen(trigger) + 1) < 0)
  {
    close(fd);
    return -1;
  }
  return fd;
}
#endif

int dt_memory_pressure_read_full_stall(uint64_t *total_us, int max_levels)
{
#if defined(__linux__)
  if(IS_NULL_PTR(total_us) || max_levels <= 0) return 0;

  int levels = 0;
  if(!_read_full_total("/proc/pressure/memory", &total_us[levels])) return 0;
  levels++;

  // A level whose file is missing stays missing, so skipping it keeps the order stable from one
  // read to the next.
  char path[512];
  _own_cgroup_path(path, sizeof(path));
  gboolean more = _cgroup_below_root(path);
  while(more && levels < max_levels)
  {
    char file[600];
    snprintf(file, sizeof(file), "/sys/fs/cgroup%s/memory.pressure", path);
    if(_read_full_total(file, &total_us[levels])) levels++;
    more = _cgroup_parent(path);
  }
  return levels;
#else
  (void)total_us;
  (void)max_levels;
  return 0;
#endif
}

#if defined(__linux__)
/* Arm a trigger -- `stall_us` of full stall within any `window_us` -- on every level that accepts
 * one. Writes up to `max_fds` descriptors to `fds` and returns how many. Levels this process may
 * not write to (root-owned ones) simply refuse it. */
static int _triggers_open(int *fds, int max_fds, uint64_t stall_us, uint64_t window_us)
{
  char trigger[64];
  snprintf(trigger, sizeof(trigger), "full %" PRIu64 " %" PRIu64, stall_us, window_us);

  int count = 0;
  const int system_fd = _open_trigger("/proc/pressure/memory", trigger);
  if(system_fd >= 0) fds[count++] = system_fd;

  char path[512];
  _own_cgroup_path(path, sizeof(path));
  gboolean more = _cgroup_below_root(path);
  while(more && count < max_fds)
  {
    char file[600];
    snprintf(file, sizeof(file), "/sys/fs/cgroup%s/memory.pressure", path);
    const int fd = _open_trigger(file, trigger);
    if(fd >= 0) fds[count++] = fd;
    more = _cgroup_parent(path);
  }
  return count;
}

struct dt_memory_pressure_watch_t
{
  /* Written before the thread starts and after it has joined, read by the thread in between. */
  pthread_t thread;
  int fds[DT_MEMORY_PRESSURE_MAX_LEVELS];
  int count;
  int stop_fd;
  gboolean running;
  void (*stalled)(void *user);
  void *user;
};

static void _watch_close(dt_memory_pressure_watch_t *watch)
{
  if(watch->stop_fd >= 0) close(watch->stop_fd);
  for(int i = 0; i < watch->count; i++) close(watch->fds[i]);
  g_free(watch);
}

static void *_watch_thread(void *arg)
{
  dt_memory_pressure_watch_t *watch = (dt_memory_pressure_watch_t *)arg;
  struct pollfd pfd[DT_MEMORY_PRESSURE_MAX_LEVELS + 1];
  int n = 0;
  for(int i = 0; i < watch->count; i++)
  {
    pfd[n].fd = watch->fds[i];
    pfd[n].events = POLLPRI;
    pfd[n].revents = 0;
    n++;
  }
  const int stop = n;
  pfd[n].fd = watch->stop_fd;
  pfd[n].events = POLLIN;
  pfd[n].revents = 0;
  n++;

  while(TRUE)
  {
    if(poll(pfd, n, -1) < 0)
    {
      if(errno == EINTR) continue;
      break;
    }
    if(pfd[stop].revents) break;

    gboolean fired = FALSE;
    for(int i = 0; i < stop; i++)
    {
      if(pfd[i].revents & POLLPRI) fired = TRUE;
      // That level is gone (its cgroup was removed): poll() skips a negative descriptor, and
      // the stop path still closes the one we opened.
      if(pfd[i].revents & (POLLERR | POLLNVAL)) pfd[i].fd = -1;
    }
    if(fired) watch->stalled(watch->user);
  }
  return NULL;
}
#endif

dt_memory_pressure_watch_t *dt_memory_pressure_watch_start(uint64_t stall_us, uint64_t window_us,
                                                           void (*stalled)(void *user), void *user)
{
#if defined(__linux__)
  if(IS_NULL_PTR(stalled)) return NULL;

  dt_memory_pressure_watch_t *watch = g_malloc0(sizeof(dt_memory_pressure_watch_t));
  watch->stop_fd = -1;
  watch->stalled = stalled;
  watch->user = user;

  watch->count = _triggers_open(watch->fds, DT_MEMORY_PRESSURE_MAX_LEVELS, stall_us, window_us);
  if(watch->count == 0)
  {
    _watch_close(watch);
    return NULL;
  }

  /* pthread_create() rather than dt_pthread_create(): that one reads a conf key to set the
   * thread's FP mode, and src/system holds no state and reaches none. This thread sleeps in
   * poll() and calls back; it does no arithmetic of its own, and the default stack is ample. */
  watch->stop_fd = eventfd(0, EFD_CLOEXEC);
  if(watch->stop_fd < 0 || pthread_create(&watch->thread, NULL, _watch_thread, watch) != 0)
  {
    _watch_close(watch);
    return NULL;
  }

  watch->running = TRUE;
  return watch;
#else
  (void)stall_us;
  (void)window_us;
  (void)stalled;
  (void)user;
  return NULL;
#endif
}

void dt_memory_pressure_watch_stop(dt_memory_pressure_watch_t **watch)
{
  if(IS_NULL_PTR(watch) || IS_NULL_PTR(*watch)) return;

#if defined(__linux__)
  dt_memory_pressure_watch_t *w = *watch;
  if(w->running)
  {
    const uint64_t wake = 1;
    if(write(w->stop_fd, &wake, sizeof(wake)) != (ssize_t)sizeof(wake))
      pthread_cancel(w->thread); // poll() is a cancellation point
    pthread_join(w->thread, NULL);
  }
  _watch_close(w);
#endif

  // Where the platform arms nothing, the handle was never non-NULL and this is unreachable.
  *watch = NULL;
}

int dt_memory_pressure_watch_levels(const dt_memory_pressure_watch_t *watch)
{
#if defined(__linux__)
  return IS_NULL_PTR(watch) ? 0 : watch->count;
#else
  (void)watch;
  return 0;
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
