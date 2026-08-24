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

/** @file ansel-lens-db-update/main.c
 *
 * @brief Rebuild the lens-correction database from this machine's lensfun installation.
 *
 * @details Ansel ships a `lenses.db` built at package time from upstream's published
 * calibrations. That is the right default and it is all most people ever need. What it
 * cannot contain is a profile that did not exist when the package was built: a calibration
 * upstream published since, or -- the case this tool exists for -- one the user measured
 * themselves and wrote into `~/.local/share/lensfun`.
 *
 * Those profiles are only ever present at RUNTIME, on the machine that has them. A build
 * runner has none, so no amount of care at package time can pick them up. Hence a separate
 * command, run by the person who has the profiles.
 *
 * It deliberately makes no attempt to be clever about aggregation. lensfun already defines
 * where profiles live and which wins when two disagree -- the system database, the system
 * updates directory, the user's updates directory, and the user's own hand-written
 * profiles, with the last always overriding -- and `ls_import_run(..., NULL)` calls
 * lensfun's own `lf_db_load()` to apply exactly those rules. Re-deriving them here would be
 * an approximation of a moving target.
 *
 * Nor does it fetch anything. Downloading upstream's newest calibrations is what lensfun's
 * own `lensfun-update-data` is for, and it writes them into one of the directories above.
 * Run that first if that is what you want; this converts whatever lensfun can currently see.
 *
 * @note Close Ansel first. The result is written to a temporary and renamed into place, so a
 * running Ansel can never read a half-written file -- but each of its threads holds its
 * database handle open for the life of the process, so it would go on using the old one
 * until restarted. Nothing breaks; the update simply would not appear.
 */

#include "common/file_location.h"
#include "common/paths.h"   // DT_PATH_MAX
#include "whereami.h"
#include "lensserious_import.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include "osx/osx.h"   // conditional-ok: dt_osx_prepare_environment() is called only
                       // from the matching __APPLE__ block in main()
#endif

#ifdef _WIN32
// This header DEFINES wmain(), which -municode makes the entry point on Windows, and which
// forwards to main() with the arguments converted to UTF-8. Without it the linker finds no
// entry it recognises, falls back to the GUI-subsystem C runtime, and fails on an undefined
// wWinMain. Every other app in src/apps includes it the same way.
#include "win/main_wrapper.h"   // conditional-ok: it defines the Windows entry point itself
#endif

static void usage(const char *progname)
{
  fprintf(stderr,
          "usage: %s [options]\n"
          "\n"
          "Rebuilds Ansel's lens-correction database from the lensfun profiles installed\n"
          "on THIS machine, including any you wrote yourself. The result is written to\n"
          "your configuration directory, where Ansel looks before the one it shipped with.\n"
          "\n"
          "Close Ansel before running this: a running instance keeps its database open and\n"
          "would not see the new file until restarted.\n"
          "\n"
          "options:\n"
          "  --xml-dir <dir>    read exactly this directory of lensfun XML, instead of\n"
          "                     letting lensfun search its four standard locations.\n"
          "                     Skips your own profiles unless they are in <dir>.\n"
          "  --output <path>    write here instead of <configdir>/lenses.db\n"
          "  --schema <path>    the SQL schema; defaults to the installed one\n"
          "  --configdir <dir>  use this configuration directory\n"
          "  --datadir <dir>    use this data directory\n"
          "  --no-baseline      do not start from the calibrations Ansel shipped with, so\n"
          "                     the result holds only what lensfun finds on this machine\n"
          "  -h, --help         this text\n"
          "\n"
          "To pull upstream's newest calibrations first, use lensfun's own updater:\n"
          "  lensfun-update-data\n"
          "then run this to convert what it fetched.\n",
          progname);
}

int main(int argc, char *argv[])
{
#ifdef __APPLE__
  dt_osx_prepare_environment();
#endif

  const char *xml_dir = NULL;
  const char *out_override = NULL;
  const char *schema_override = NULL;
  const char *configdir = NULL;
  const char *datadir = NULL;
  int no_baseline = 0;

  for(int k = 1; k < argc; k++)
  {
    if(!strcmp(argv[k], "-h") || !strcmp(argv[k], "--help"))
    {
      usage(argv[0]);
      return 0;
    }

    if(!strcmp(argv[k], "--no-baseline"))
    {
      no_baseline = 1;
      continue;
    }

    /* Every option below takes a value, so a missing one is an error rather than a
     * silently ignored flag -- the difference between "read your own profiles" and "read
     * one directory" is exactly the kind of thing that must not be decided by a typo. */
    const char *const opts[] = { "--xml-dir", "--output", "--schema", "--configdir", "--datadir" };
    const char **const dests[] = { &xml_dir, &out_override, &schema_override, &configdir, &datadir };
    int matched = 0;
    for(size_t o = 0; o < sizeof(opts) / sizeof(*opts); o++)
    {
      if(strcmp(argv[k], opts[o])) continue;
      if(k + 1 >= argc)
      {
        fprintf(stderr, "%s: %s needs a value\n", argv[0], opts[o]);
        return 2;
      }
      *dests[o] = argv[++k];
      matched = 1;
      break;
    }
    if(matched) continue;

    fprintf(stderr, "%s: unknown argument `%s'\n", argv[0], argv[k]);
    usage(argv[0]);
    return 2;
  }

  /* Resolves the same paths the application resolves, from the same code, so this writes
   * where Ansel will actually look. Doing the lookup by hand here is how the two would
   * drift the first time a packager moves something.
   *
   * Only the two directories this tool actually reads, though, rather than dt_loc_init()'s
   * eight. Each initialiser ends in dt_check_opendir(), which exit()s the process when a
   * directory is missing and could not be created -- and dt_loc_init_generic() creates with
   * mode 0700, which a normal user cannot do under a system prefix. Asking for the module,
   * locale, kernel, cache, share and temporary directories would make this tool die over
   * any of the six it never opens. */
  char *application_directory = NULL;
  int dirname_length = 0;
  const int length = wai_getExecutablePath(NULL, 0, &dirname_length);
  if(length > 0)
  {
    application_directory = (char *)malloc(length + 1);
    if(application_directory)
    {
      wai_getExecutablePath(application_directory, length, &dirname_length);
      application_directory[dirname_length] = '\0';
    }
  }
  dt_loc_init_datadir(application_directory, datadir);
  dt_loc_init_user_config_dir(configdir);
  free(application_directory);

  char schema_path[DT_PATH_MAX] = { 0 };
  if(schema_override)
    g_strlcpy(schema_path, schema_override, sizeof(schema_path));
  else
    snprintf(schema_path, sizeof(schema_path), "%s/lenses-schema.sql", dt_loc_datadir());

  if(!g_file_test(schema_path, G_FILE_TEST_IS_REGULAR))
  {
    fprintf(stderr,
            "%s: no schema at `%s'.\n"
            "This file is installed with Ansel; if it is missing, the installation is\n"
            "incomplete. Pass --schema to point at one explicitly.\n",
            argv[0], schema_path);
    return 1;
  }

  char out_path[DT_PATH_MAX] = { 0 };
  if(out_override)
    g_strlcpy(out_path, out_override, sizeof(out_path));
  else
    snprintf(out_path, sizeof(out_path), "%s/lenses.db", dt_loc_configdir());

  /* The calibrations Ansel shipped, loaded first so everything below merely overrides
   * them. This is what makes the update additive rather than a replacement -- see
   * ls_import_run(). */
  char base_dir[DT_PATH_MAX] = { 0 };
  const char *base = NULL;
  if(!no_baseline)
  {
    snprintf(base_dir, sizeof(base_dir), "%s/lensfun-xml", dt_loc_datadir());
    if(g_file_test(base_dir, G_FILE_TEST_IS_DIR))
      base = base_dir;
    else
      fprintf(stderr,
              "warning: no shipped calibrations at `%s'.\n"
              "The result will hold ONLY what lensfun finds on this machine, which may be\n"
              "far less than Ansel came with.\n",
              base_dir);
  }

  if(xml_dir)
    printf("Reading lensfun XML from `%s'.\n", xml_dir);
  else
    printf("Reading every lensfun profile this machine has: the system database, the\n"
           "system and user update directories, and your own profiles in\n"
           "~/.local/share/lensfun (which override the rest).\n");

  /* Written aside and only then moved into place, because the result has to be JUDGED
   * before it is allowed to shadow anything.
   *
   * lf_db_load() reads lensfun's own directories and knows nothing about the database
   * Ansel ships. On a machine with no system-wide lensfun -- the normal case on Windows
   * and macOS -- it can therefore come back with nothing but the handful of profiles the
   * user wrote themselves. That file is not wrong, but installing it WOULD be: iop/lens.c
   * prefers the configuration directory and never falls back once it opens something, so
   * a three-lens database would silently replace the fifteen hundred that shipped. */
  char staged_path[DT_PATH_MAX] = { 0 };
  snprintf(staged_path, sizeof(staged_path), "%s.incoming", out_path);

  const int rc = ls_import_run(schema_path, staged_path, base, xml_dir);
  if(rc)
  {
    g_unlink(staged_path);
    fprintf(stderr, "%s: the database was NOT updated; the previous one is untouched.\n",
            argv[0]);
    return rc;
  }

  if(g_rename(staged_path, out_path) != 0)
  {
    fprintf(stderr, "%s: cannot move `%s' to `%s'; nothing was changed.\n",
            argv[0], staged_path, out_path);
    g_unlink(staged_path);
    return 1;
  }

  printf("\nWrote `%s'.\n"
         "Ansel reads this in preference to the database it shipped with, so the new\n"
         "profiles are available next time you start it. Delete this file to go back to\n"
         "the shipped one.\n",
         out_path);
  return 0;
}
