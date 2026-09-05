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

/** The GtkAccelMap contract _insert_accel() (widgets/accelerators.c) is built on.
 *
 * Shortcuts are reconciled between three sources: the app default declared in the code, the
 * user's keyboardrc, and the GtkAccelMap that sits between them. dt_accels_load_user_config()
 * reads the file first, then every shortcut registers its accel path, and only the difference
 * between the two decides whether a shortcut is at its default, was rebound, or was cleared.
 *
 * That difference is only legible if the app default is registered AS the accel_map entry's
 * default. Registering (path, 0, 0) instead -- what the code did until the purple-label bug --
 * makes "this config predates the shortcut" and "the user cleared this shortcut" arrive
 * identically, as key 0, and _update_shortcut_state() then reads both as a deliberate user
 * override. _a_zero_default_cannot_tell_absent_from_cleared() pins that failure so the reason
 * for the current spelling stays visible.
 *
 * These tests exercise GTK, not Ansel code: they state what accelerators.c assumes, so a GTK
 * whose bookkeeping changed is caught here rather than as shortcuts going quietly dead. Each
 * test uses accel pathes of its own -- the GtkAccelMap is process-global with no reset call,
 * and gtk_accel_map_load() merges into whatever is already there.
 */

#include <gtk/gtk.h>

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as the other tests here, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#define APP_DEFAULT_KEY GDK_KEY_F5

/** Write a keyboardrc holding the given lines, and load it the way the app does. */
static void _load_config(const char *contents)
{
  gchar *path = g_build_filename(g_get_tmp_dir(), "ansel-test-keyboardrc", NULL);
  gchar *file = g_strconcat("; test GtkAccelMap rc-file\n", contents, NULL);
  assert_true(g_file_set_contents(path, file, -1, NULL));
  gtk_accel_map_load(path);
  g_free(file);
  g_free(path);
}

/** The key the map holds for a path once its shortcut has registered its own default. */
static guint _register_and_read(const char *path, guint default_key)
{
  gtk_accel_map_add_entry(path, default_key, 0);
  GtkAccelKey key = { 0 };
  assert_true(gtk_accel_map_lookup_entry(path, &key));
  return key.accel_key;
}

/** A shortcut whose path the config never mentions must come up at its app default.
 *
 * This is the whole bug: a shortcut added by a new version, or one whose accel path moved
 * because the translated menu label it is built from changed, is absent from every config
 * written before it existed. F5 for the purple colour label was lost this way when the French
 * catalogue translated "Purple" as "Violet".
 */
static void _an_absent_path_takes_the_app_default(void **state)
{
  (void)state;
  _load_config("(gtk_accel_path \"<AnselTest1>/known\" \"F1\")\n");
  assert_int_equal(_register_and_read("<AnselTest1>/absent", APP_DEFAULT_KEY), APP_DEFAULT_KEY);
}

/** A shortcut the user cleared must stay cleared, and says so with a live empty entry. */
static void _an_emptied_entry_stays_cleared(void **state)
{
  (void)state;
  _load_config("(gtk_accel_path \"<AnselTest2>/cleared\" \"\")\n");
  assert_int_equal(_register_and_read("<AnselTest2>/cleared", APP_DEFAULT_KEY), 0);
}

/** A shortcut the user rebound keeps the user's keys, not the app's. */
static void _a_user_binding_wins_over_the_default(void **state)
{
  (void)state;
  _load_config("(gtk_accel_path \"<AnselTest3>/rebound\" \"<Control>k\")\n");
  gtk_accel_map_add_entry("<AnselTest3>/rebound", APP_DEFAULT_KEY, 0);
  GtkAccelKey key = { 0 };
  assert_true(gtk_accel_map_lookup_entry("<AnselTest3>/rebound", &key));
  assert_int_equal(key.accel_key, GDK_KEY_k);
  assert_int_equal(key.accel_mods & GDK_CONTROL_MASK, GDK_CONTROL_MASK);
}

/** Why the default is registered rather than (0, 0): with (0, 0) the two cases above are
 * the same value, and the one that must win cannot be told from the one that must not. */
static void _a_zero_default_cannot_tell_absent_from_cleared(void **state)
{
  (void)state;
  _load_config("(gtk_accel_path \"<AnselTest4>/cleared\" \"\")\n");
  assert_int_equal(_register_and_read("<AnselTest4>/cleared", 0), 0);
  assert_int_equal(_register_and_read("<AnselTest4>/absent", 0), 0);
}

/** The other half of the contract, on the way out: gtk_accel_map_save() comments out an entry
 * still sitting at its default and writes every other one as a live line. That is what makes a
 * cleared shortcut survive the next start -- it is saved as a real (path "") line, which is
 * what _an_emptied_entry_stays_cleared() reads back. */
static void _saving_marks_defaults_as_comments_and_changes_as_lines(void **state)
{
  (void)state;
  gtk_accel_map_add_entry("<AnselTest5>/default", APP_DEFAULT_KEY, 0);
  gtk_accel_map_add_entry("<AnselTest5>/cleared", APP_DEFAULT_KEY, 0);
  assert_true(gtk_accel_map_change_entry("<AnselTest5>/cleared", 0, 0, TRUE));

  gchar *path = g_build_filename(g_get_tmp_dir(), "ansel-test-keyboardrc-save", NULL);
  gtk_accel_map_save(path);

  gchar *contents = NULL;
  assert_true(g_file_get_contents(path, &contents, NULL, NULL));
  assert_non_null(g_strstr_len(contents, -1, "; (gtk_accel_path \"<AnselTest5>/default\" \"F5\")"));
  assert_non_null(g_strstr_len(contents, -1, "\n(gtk_accel_path \"<AnselTest5>/cleared\" \"\")"));
  g_free(contents);
  g_free(path);
}

int main(void)
{
  // The accel map is a plain hash table populated by gtk_init(), and unreachable without it.
  // The result is deliberately ignored: a headless runner opens no display and answers FALSE,
  // having initialised everything these tests touch anyway.
  int argc = 0;
  char **argv = NULL;
  gtk_init_check(&argc, &argv);

  const struct CMUnitTest tests[] = {
    cmocka_unit_test(_an_absent_path_takes_the_app_default),
    cmocka_unit_test(_an_emptied_entry_stays_cleared),
    cmocka_unit_test(_a_user_binding_wins_over_the_default),
    cmocka_unit_test(_a_zero_default_cannot_tell_absent_from_cleared),
    cmocka_unit_test(_saving_marks_defaults_as_comments_and_changes_as_lines),
  };

  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
