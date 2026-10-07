/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "testdb.h"
#include "common/conf.h"
#include "common/selection.h"
#include "darktable.h"

#include "../../src/gui/actions/display.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_conf_t _conf = { 0 };
static dt_view_manager_t _view_manager = { 0 };
static dt_view_t _lighttable_view = { .module_name = "lighttable" };

static int _setup(void **state)
{
  if(testdb_setup(state)) return -1;

  darktable.conf = &_conf;
  darktable.view_manager = &_view_manager;
  _view_manager.current_view = &_lighttable_view;
  dt_conf_init(&_conf, "/dev/null", NULL);
  dt_selection_init_global();
  return 0;
}

static int _teardown(void **state)
{
  dt_selection_cleanup_global();
  dt_conf_cleanup(&_conf);
  _view_manager.current_view = NULL;
  darktable.view_manager = NULL;
  darktable.conf = NULL;
  return testdb_teardown(state);
}

static void test_single_image_group_has_one_member(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/display-expansion");
  const int32_t imgid = testdb_make_image(film_id, "image.raw");
  dt_image_t image;

  dt_image_init(&image);
  image.id = imgid;
  image.film_id = film_id;
  image.group_id = imgid;
  g_strlcpy(image.filename, "image.raw", sizeof(image.filename));
  dt_image_repository_store(&image);
  GList *members = dt_image_repository_get_group_members(imgid, -1);
  assert_int_equal(g_list_length(members), 1);
  assert_int_equal(GPOINTER_TO_INT(members->data), imgid);
  g_list_free(members);
}

static void test_expand_selected_group_is_insensitive_without_a_selection_or_gui(void **state G_GNUC_UNUSED)
{
  GtkWidget *const item = gtk_menu_item_new_with_label("placeholder");

  dt_conf_set_bool("ui_last/grouping", TRUE);

  assert_false(expand_selected_group_sensitive_callback(item));
  assert_string_equal(gtk_menu_item_get_label(GTK_MENU_ITEM(item)), "Expand selected group");
  assert_true(expand_selected_group_callback(NULL, NULL, 0, 0, NULL));
  gtk_widget_destroy(item);
}

int main(int argc, char *argv[])
{
  assert_true(gtk_init_check(&argc, &argv));
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_single_image_group_has_one_member),
    cmocka_unit_test(test_expand_selected_group_is_insensitive_without_a_selection_or_gui),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}
