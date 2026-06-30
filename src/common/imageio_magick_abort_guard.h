/*
    This file is part of darktable,
    Copyright (C) 2026 Aurélien PIERRE.

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
#pragma once

/*
 * GraphicsMagick/ImageMagick call assert() on malformed input, and the
 * distro-packaged builds we link against do not define NDEBUG, so assert()
 * calls abort() and takes the whole process down with it (Sentry issues
 * 130678348 and 129978857 - the same failure mode hits both the full-image
 * GM/IM loaders in imageio_gm.c/imageio_im.c and the embedded-thumbnail
 * decoder inlined in dt_imageio_large_thumbnail()).
 *
 * This guard turns that abort() into a recoverable error per call site:
 *
 *  - it uses plain ISO C signal()/setjmp(), not the POSIX sigaction()/
 *    sigsetjmp() variants, so it actually compiles on Windows/MinGW;
 *  - the jump buffer is thread-local. Thumbnail/preview generation runs
 *    GM/IM concurrently on worker threads (dt_control_work, mipmap cache),
 *    and a single process-wide buffer would let one thread's recovery
 *    longjmp into a different thread's stack;
 *  - on recovery, callers must NOT call back into the library on the object
 *    that was being built (DestroyImage, DestroyMagickWand, etc.). abort()
 *    means the library hit its own internal assertion, so that object's
 *    state is unknown - touching it again, even to free it, can re-enter
 *    the same broken code path or corrupt memory. Leaking that one object
 *    is the safe trade-off for "this single file is malformed". Buffers
 *    Ansel itself allocated (mipmap/pixelpipe buffers) are unaffected by
 *    this and should still be freed normally by the recovery statement;
 *  - it restores Ansel's own signal handlers afterward
 *    (common/system_signal_handling.c) since GraphicsMagick is known to
 *    silently steal them as a side effect of its calls (see the
 *    InitializeMagick() callers in common/darktable.c).
 *
 * Usage, mirroring the existing goto-based error handling in these files:
 *
 *   DT_MAGICK_ABORT_GUARD("GraphicsMagick_open", filename, goto error);
 *   image = ReadImage(image_info, &exception);
 *   ...
 *   DT_MAGICK_ABORT_GUARD_DISARM();
 *   return DT_IMAGEIO_OK;
 *
 * `recovery` runs with the guard already disarmed and the signal handler
 * already restored; it must be a single statement (typically a `goto` or a
 * `return`) and must not declare variables (it sits inside an `if` body
 * opened by the macro).
 */

#include "common/system_signal_handling.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>

static __thread jmp_buf _dt_magick_abort_jmp;
static __thread int _dt_magick_abort_armed = 0;
static __thread void (*_dt_magick_abort_prev_handler)(int) = NULL;

static void _dt_magick_abort_handler(int sig)
{
  if(_dt_magick_abort_armed)
  {
    _dt_magick_abort_armed = 0;
    longjmp(_dt_magick_abort_jmp, 1);
  }
  // abort() on this thread but outside a guarded call: don't swallow it,
  // let the normal crash-reporting path (Sentry / gdb backtrace) handle it.
  signal(SIGABRT, SIG_DFL);
  raise(SIGABRT);
}

#define DT_MAGICK_ABORT_GUARD(label, fname, recovery)                                                          \
  _dt_magick_abort_prev_handler = signal(SIGABRT, _dt_magick_abort_handler);                                   \
  if(setjmp(_dt_magick_abort_jmp))                                                                              \
  {                                                                                                              \
    fprintf(stderr, "[%s] caught an internal abort() raised by the image library while loading `%s' - "        \
                     "treating it as corrupted\n", (label), (fname));                                           \
    signal(SIGABRT, _dt_magick_abort_prev_handler);                                                             \
    dt_set_signal_handlers();                                                                                   \
    recovery;                                                                                                   \
  }                                                                                                              \
  _dt_magick_abort_armed = 1;

#define DT_MAGICK_ABORT_GUARD_DISARM()                                                                          \
  do                                                                                                             \
  {                                                                                                               \
    _dt_magick_abort_armed = 0;                                                                                  \
    signal(SIGABRT, _dt_magick_abort_prev_handler);                                                              \
  } while(0)

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
