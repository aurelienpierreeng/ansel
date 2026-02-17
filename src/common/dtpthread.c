/*
    This file is part of darktable,
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2016-2017 Roman Lebedev.
    Copyright (C) 2017 Tobias Ellinghaus.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Aurélien PIERRE.
    
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

#define _GNU_SOURCE

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <sched.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>
#include <glib.h>

#ifdef _WIN32
#include "win/dtwin.h"
#endif // _WIN32

int dt_pthread_create(pthread_t *thread, void *(*start_routine)(void *), void *arg, const gboolean realtime)
{
  int ret;

  pthread_attr_t attr;

  ret = pthread_attr_init(&attr);
  if(ret != 0)
  {
    fprintf(stderr, "[dt_pthread_create] error: pthread_attr_init() returned %i\n", ret);
    return ret;
  }

  size_t stacksize;

  ret = pthread_attr_getstacksize(&attr, &stacksize);

  if(ret != 0)
  {
    fprintf(stderr, "[dt_pthread_create] error: pthread_attr_getstacksize() returned %i\n", ret);
  }

  if(ret != 0 || stacksize < WANTED_THREADS_STACK_SIZE /*|| 1*/)
  {
    // looks like we need to bump/set it...

    fprintf(stderr, "[dt_pthread_create] info: bumping pthread's stacksize from %zu to %"PRIuMAX"\n", stacksize,
            (uintmax_t)WANTED_THREADS_STACK_SIZE);

    ret = pthread_attr_setstacksize(&attr, WANTED_THREADS_STACK_SIZE);
    if(ret != 0)
    {
      fprintf(stderr, "[dt_pthread_create] error: pthread_attr_setstacksize() returned %i\n", ret);
    }
  }

  if(ret != 0 && realtime)
  {
    // Set SCHED_RR policy: realtime round robin
    ret = pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    if(ret != 0)
    {
      fprintf(stderr, "setschedpolicy: %s\n", strerror(ret));
      exit(EXIT_FAILURE);
    }

    // Set thread priority (between 1 and 99)
    struct sched_param param = { .sched_priority = 80 };
    ret = pthread_attr_setschedparam(&attr, &param);
    if(ret != 0)
    {
      fprintf(stderr, "setschedparam: %s\n", strerror(ret));
      exit(EXIT_FAILURE);
    }

    // Make sure the thread uses the explicitly set policy and param
    ret = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    if(ret != 0)
    {
      fprintf(stderr, "setinheritsched: %s\n", strerror(ret));
      exit(EXIT_FAILURE);
    }
  }

  ret = pthread_create(thread, &attr, start_routine, arg);

  pthread_attr_destroy(&attr);

  return ret;
}

void dt_pthread_setname(const char *name)
{
#if defined __linux__
  pthread_setname_np(pthread_self(), name);
#elif defined __FreeBSD__ || defined __DragonFly__
  // TODO: is this the right syntax?
  // pthread_setname_np(pthread_self(), name, 0);
#elif defined __NetBSD__
  // TODO: is this the right syntax?
  // pthread_setname_np(pthread_self(), name, NULL);
#elif defined __OpenBSD__
  // TODO: find out if there is pthread_setname_np() on OpenBSD and how to call it
#elif defined __APPLE__
  pthread_setname_np(name);
#elif defined _WIN32
  dtwin_set_thread_name((DWORD)-1, name);
#endif
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
