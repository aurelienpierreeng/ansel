/*
    This file is part of darktable,
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "system/resource_limits.h"

#ifndef _WIN32
#include <assert.h>       // for assert
#include <errno.h>        // for errno
#include <stdint.h>       // for uintmax_t
#include <stdio.h>        // for fprintf, stderr
#include <string.h>       // for strerror
#include <inttypes.h>
#include <sys/resource.h> // for rlimit, RLIMIT_STACK, getrlimit, setrlimit

static void dt_set_rlimits_stack()
{
  // make sure that stack/frame limits are good (musl)

  int ret;

  struct rlimit rlim = { 0 };

  ret = getrlimit(RLIMIT_STACK, &rlim);

  if(ret != 0)
  {
    const int errsv = errno;
    fprintf(stderr, "[dt_set_rlimits_stack] error: getrlimit(RLIMIT_STACK) returned %i: %i (%s)\n", ret, errsv,
            strerror(errsv));
  }

  assert((ret == 0 && (WANTED_STACK_SIZE <= rlim.rlim_max || RLIM_INFINITY == rlim.rlim_max)) || (ret != 0));

  if(ret != 0 || (rlim.rlim_cur < WANTED_STACK_SIZE && rlim.rlim_cur != RLIM_INFINITY) /*|| 1*/)
  {
    // looks like we need to bump/set it...

    fprintf(stderr, "[dt_set_rlimits_stack] info: bumping RLIMIT_STACK rlim_cur from %"PRIuMAX" to %"PRIuMAX"\n",
            (uintmax_t)rlim.rlim_cur, (uintmax_t)WANTED_STACK_SIZE);

    rlim.rlim_cur = WANTED_STACK_SIZE;

    ret = setrlimit(RLIMIT_STACK, &rlim);
    if(ret != 0)
    {
      int errsv = errno;
      fprintf(stderr, "[dt_set_rlimits_stack] error: setrlimit(RLIMIT_STACK) returned %i: %i (%s)\n", ret, errsv,
              strerror(errsv));
    }
  }
}
#endif // !_WIN32

/**
 * @brief Raise the process limits Ansel needs, where the system lets a running process do it.
 *
 * Nothing to do on Windows: a thread's stack size is fixed when it is created, the main thread's
 * at link time from the executable header, and no call changes it afterwards. The emulated
 * `getrlimit()` that used to sit here answered RLIM_INFINITY, so `setrlimit()` was never reached.
 */
void dt_set_rlimits()
{
#ifndef _WIN32
  dt_set_rlimits_stack();
#endif
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

