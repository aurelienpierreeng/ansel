/*
    This file is part of darktable,
    Copyright (C) 2016-2018 Peter Budai.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2017 luzpaz.
    Copyright (C) 2017 Tobias Ellinghaus.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Aurélien PIERRE.
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

#include "common/darktable.h"     // for darktable, darktable_t
#include "common/system_signal_handling.h"
#include <errno.h>       // for errno
#include <fcntl.h>       // for O_APPEND, O_CREAT, O_WRONLY, open
#include <glib.h>        // for g_free, g_printerr, g_strdup_printf
#include <glib/gstdio.h> // for g_unlink
#include <signal.h>      // for signal, SIGSEGV, SIG_ERR
#include <stddef.h>      // for NULL
#include <stdio.h>       // for dprintf, fprintf, stderr
#include <string.h>      // for strerror
#include <unistd.h>      // for STDOUT_FILENO, close, execlp, fork

#ifdef __linux__
#include <sys/prctl.h> // for PR_SET_PTRACER, prctl
#endif

#ifndef _WIN32
#include <sys/wait.h> // for waitpid
#endif

#ifdef _WIN32
#include <exchndl.h>
#endif //_WIN32

#if defined(__linux__) && !defined(PR_SET_PTRACER)
#define PR_SET_PTRACER 0x59616d61
#endif

typedef void(dt_signal_handler_t)(int);

#if !defined(__APPLE__) && !defined(_WIN32)
static dt_signal_handler_t *_dt_sigsegv_old_handler = NULL;
#endif

// deer graphicsmagick, please stop messing with the stuff that you should not be touching at all.
// based on GM's InitializeMagickSignalHandlers() and MagickSignalHandlerMessage()
#if !defined(_WIN32)
static const int _signals_to_preserve[] = { SIGHUP,  SIGINT,  SIGQUIT, SIGILL,  SIGABRT, SIGBUS, SIGFPE,
                                            SIGPIPE, SIGALRM, SIGTERM, SIGCHLD, SIGXCPU, SIGXFSZ };
#else
static const int _signals_to_preserve[] = { SIGABRT, SIGFPE, SIGILL, SIGINT, SIGSEGV, SIGTERM };
static LPTOP_LEVEL_EXCEPTION_FILTER _dt_exceptionfilter_old_handler = NULL;
#endif //! defined (_WIN32)

#define _NUM_SIGNALS_TO_PRESERVE (sizeof(_signals_to_preserve) / sizeof(_signals_to_preserve[0]))
static dt_signal_handler_t *_orig_sig_handlers[_NUM_SIGNALS_TO_PRESERVE] = { NULL };

#if(defined(__FreeBSD_version) && (__FreeBSD_version < 800071)) || (defined(OpenBSD) && (OpenBSD < 201305))       \
    || defined(__SUNOS__)
static int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)))
{
  va_list ap;
  FILE *f = fdopen(fd, "a");
  va_start(ap, fmt);
  int rc = vfprintf(f, fmt, ap);
  fclose(f);
  va_end(ap);
  return rc;
}
#endif

#if !defined(__APPLE__) && !defined(_WIN32)
static void _dt_sigsegv_handler(int param)
{
  pid_t pid;
  gchar *name_used;
  int fout;
  gboolean delete_file = FALSE;

  if((fout = g_file_open_tmp("ansel_bt_XXXXXX.txt", &name_used, NULL)) == -1)
    fout = STDOUT_FILENO; // just print everything to stdout

  dprintf(fout, "this is %s reporting a segfault:\n\n", darktable_package_string);

  if(fout != STDOUT_FILENO) close(fout);

  gchar *pid_arg = g_strdup_printf("%d", (int)getpid());
  gchar *exe_arg = g_strdup_printf("/proc/%s/exe", pid_arg);
  gchar *log_file_arg = g_strdup_printf("set logging file %s", name_used);
  const char *log_overwrite_arg = "set logging overwrite on";
  const char *log_redirect_arg = "set logging redirect on";
  const char *log_enabled_arg = "set logging enabled on";
  const char *pagination_arg = "set pagination off";
  const char *confirm_arg = "set confirm off";
  const char *where_arg = "where";
  const char *info_threads_arg = "info threads";
  const char *thread_bt_arg = "thread apply all bt full";
  const char *separator_a_arg = "echo \\n=========\\n\\n";
  const char *separator_b_arg = "echo \\n=========\\n";

  if((pid = fork()) != -1)
  {
    if(pid)
    {
#ifdef __linux__
      // Allow the child to ptrace us
      prctl(PR_SET_PTRACER, pid, 0, 0, 0);
#endif
      waitpid(pid, NULL, 0);
      g_printerr("backtrace written to %s\n", name_used);
    }
    else
    {
      if(fout != STDOUT_FILENO)
      {
        const int log_fd = open(name_used, O_WRONLY | O_APPEND);
        if(log_fd != -1)
        {
          dup2(log_fd, STDOUT_FILENO);
          dup2(log_fd, STDERR_FILENO);
          close(log_fd);
        }
      }

      if(execlp("gdb", "gdb", exe_arg, pid_arg, "-batch", "-ex", pagination_arg, "-ex", confirm_arg, "-ex",
                log_file_arg, "-ex", log_overwrite_arg, "-ex", log_redirect_arg, "-ex", log_enabled_arg, "-ex",
                where_arg, "-ex", separator_a_arg, "-ex", info_threads_arg, "-ex", separator_b_arg, "-ex",
                thread_bt_arg, NULL))
      {
        delete_file = TRUE;
        g_printerr("an error occurred while trying to execute gdb. please check if gdb is installed on your "
                   "system.\n");
      }
    }
  }
  else
  {
    delete_file = TRUE;
    g_printerr("an error occurred while trying to execute gdb.\n");
  }

  if(delete_file) g_unlink(name_used);
  dt_free(pid_arg);
  dt_free(exe_arg);
  dt_free(log_file_arg);
  dt_free(name_used);

  /* pass it further to the old handler*/
  _dt_sigsegv_old_handler(param);
}
#endif

static int _times_handlers_were_set = 0;

#if defined(_WIN32)

static LONG WINAPI dt_toplevel_exception_handler(PEXCEPTION_POINTERS pExceptionInfo)
{
  gchar *name_used;
  int fout;
  BOOL ok;

  // Find a filename for the backtrace file
  if((fout = g_file_open_tmp("ansel_bt_XXXXXX.txt", &name_used, NULL)) == -1)
    fout = STDOUT_FILENO; // just print everything to stdout

  FILE *fd = fdopen(fout, "wb");
  fprintf(fd, "this is %s reporting an exception:\n\n", darktable_package_string);
  fclose(fd);

  if(fout != STDOUT_FILENO) close(fout);


  // Set up logfile name
  ok = ExcHndlSetLogFileNameA(name_used);
  if(!ok)
  {
    g_printerr("backtrace logfile cannot be set to %s\n", name_used);
  }
  else
  {
    gchar *exception_message = g_strdup_printf("An unhandled exception occurred.\nBacktrace will be written to: %s "
                                               "after you click on the OK button.\nIf you report this issue, "
                                               "please share this backtrace with the developers.\n",
                                               name_used);
    wchar_t *wexception_message = g_utf8_to_utf16(exception_message, -1, NULL, NULL, NULL);
    MessageBoxW(0, wexception_message, L"Error!", MB_OK);
    dt_free(exception_message);
    dt_free(wexception_message);
  }

  dt_free(name_used);

  // finally call the original exception handler (which should be drmingw's exception handler)
  return _dt_exceptionfilter_old_handler(pExceptionInfo);
}

void dt_set_unhandled_exception_handler_win()
{
  // Set up drming's exception handler
  ExcHndlInit();
}
#endif // defined(_WIN32)


void dt_set_signal_handlers()
{
  _times_handlers_were_set++;

  dt_signal_handler_t *prev;

  if(1 == _times_handlers_were_set)
  {
    // save original handlers
    for(int i = 0; i < _NUM_SIGNALS_TO_PRESERVE; i++)
    {
      const int signum = _signals_to_preserve[i];

      prev = signal(signum, SIG_DFL);

      if(SIG_ERR == prev) prev = SIG_DFL;

      _orig_sig_handlers[i] = prev;
    }
  }

  // restore handlers
  for(int i = 0; i < _NUM_SIGNALS_TO_PRESERVE; i++)
  {
    const int signum = _signals_to_preserve[i];

    (void)signal(signum, _orig_sig_handlers[i]);
  }

#if !defined(__APPLE__) && !defined(_WIN32)
  // now, set our SIGSEGV handler.
  // FIXME: what about SIGABRT?
  prev = signal(SIGSEGV, &_dt_sigsegv_handler);

  if(SIG_ERR != prev)
  {
    // we want the most original previous signal handler.
    if(1 == _times_handlers_were_set) _dt_sigsegv_old_handler = prev;
  }
  else
  {
    const int errsv = errno;
    fprintf(stderr, "[dt_set_signal_handlers] error: signal(SIGSEGV) returned SIG_ERR: %i (%s)\n", errsv,
            strerror(errsv));
  }
#elif !defined(__APPLE__)
  /*
  Set up exception handler for backtrace on Windows
  Works when there is NO SIGSEGV handler installed

  SetUnhandledExceptionFilter handler must be saved on the first invocation
  as GraphicsMagick is overwriting SetUnhandledExceptionFilter and all other signals in InitializeMagick()
  Eventually InitializeMagick() should be fixed upstream not to ignore existing exception handlers
  */

  dt_set_unhandled_exception_handler_win();
  if(1 == _times_handlers_were_set)
  {
    // Save UnhandledExceptionFilter handler which just has been set up
    // This should be drmingw's exception handler
    _dt_exceptionfilter_old_handler = SetUnhandledExceptionFilter(dt_toplevel_exception_handler);
  }
  // Restore our UnhandledExceptionFilter handler no matter what GM is doing
  SetUnhandledExceptionFilter(dt_toplevel_exception_handler);

#endif //!defined(__APPLE__) && !defined(_WIN32)
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
