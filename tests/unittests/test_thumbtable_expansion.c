/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "testdb.h"
#include "common/collection.h"
#include "common/conf.h"
#include "common/file_location.h"
#include "caches/image_cache.h"
#include "darktable.h"
#include "common/datetime.h"
#include "gui/dtgtk/thumbtable.h"
#include "imageio/imageio_module.h"
#include "../../src/gui/dtgtk/thumbtable.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static dt_imageio_t _imageio = { 0 };
static dt_conf_t _conf = { 0 };

void dt_control_hinter_message(const dt_control_t *control G_GNUC_UNUSED, const char *message)
{
  assert_non_null(message);
}

static int _setup(void **state)
{
  if(testdb_setup(state)) return -1;

  dt_image_cache_init(FALSE);

  darktable.conf = &_conf;
  dt_conf_init(&_conf, "/dev/null", NULL);
  dt_conf_set_int("plugins/collection/filter_flags", -1);
  dt_conf_set_int("plugins/lighttable/collect/num_rules", 1);
  dt_conf_set_int("plugins/lighttable/collect/mode0", 0);
  dt_conf_set_int("plugins/lighttable/collect/item0", DT_COLLECTION_PROP_FILENAME);
  dt_conf_set_string("plugins/lighttable/collect/string0", "%");
  dt_conf_set_bool("plugins/lighttable/collect/recursive0", FALSE);
  dt_loc_init_datadir(NULL, ANSEL_TEST_DATADIR);
  darktable.imageio = &_imageio;
  dt_collection_init_global();
  return 0;
}

static int _teardown(void **state)
{
  dt_collection_cleanup_global();
  dt_image_cache_cleanup();
  dt_conf_cleanup(&_conf);
  darktable.conf = NULL;
  dt_free(darktable.datadir);
  darktable.datadir = NULL;
  darktable.imageio = NULL;
  return testdb_teardown(state);
}

static void test_expanded_group_toggle_replaces_and_clears(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_t table = { .mode = DT_THUMBTABLE_MODE_FILEMANAGER,
                            .collapse_groups = TRUE,
                            .expanded_group_id = UNKNOWN_IMAGE };

  assert_true(dt_thumbtable_toggle_expanded_group(&table, 10));
  assert_int_equal(table.expanded_group_id, 10);
  assert_true(dt_thumbtable_toggle_expanded_group(&table, 20));
  assert_int_equal(table.expanded_group_id, 20);
  assert_false(dt_thumbtable_toggle_expanded_group(&table, 20));
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);
}

static void test_filmstrip_has_no_expanded_group(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_t table = { .mode = DT_THUMBTABLE_MODE_FILMSTRIP, .expanded_group_id = UNKNOWN_IMAGE };

  assert_false(dt_thumbtable_toggle_expanded_group(&table, 10));
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);
  dt_thumbtable_clear_expanded_group(&table);
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);
}

static void test_uncollapsed_lighttable_cannot_expand_a_group(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_t table = { .mode = DT_THUMBTABLE_MODE_FILEMANAGER,
                            .collapse_groups = FALSE,
                            .expanded_group_id = UNKNOWN_IMAGE };

  assert_false(dt_thumbtable_toggle_expanded_group(&table, 10));
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);
}

static void test_expanded_group_members_include_the_representative(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/thumbtable-expansion/");
  const int32_t representative = testdb_make_image(film_id, "representative.raw");
  const int32_t first_member = testdb_make_image(film_id, "first-member.raw");
  const int32_t second_member = testdb_make_image(film_id, "second-member.raw");
  dt_image_t image;

  dt_image_init(&image);
  image.id = representative;
  image.film_id = film_id;
  image.group_id = representative;
  g_strlcpy(image.filename, "representative.raw", sizeof(image.filename));
  g_strlcpy(image.folder, "/testdb/thumbtable-expansion/", sizeof(image.folder));
  dt_image_repository_store(&image);
  dt_image_init(&image);
  image.id = first_member;
  image.film_id = film_id;
  image.group_id = first_member;
  g_strlcpy(image.filename, "first-member.raw", sizeof(image.filename));
  g_strlcpy(image.folder, "/testdb/thumbtable-expansion/", sizeof(image.folder));
  dt_image_repository_store(&image);
  dt_image_init(&image);
  image.id = second_member;
  image.film_id = film_id;
  image.group_id = second_member;
  g_strlcpy(image.filename, "second-member.raw", sizeof(image.filename));
  g_strlcpy(image.folder, "/testdb/thumbtable-expansion/", sizeof(image.folder));
  dt_image_repository_store(&image);
  assert_true(dt_image_repository_set_group(first_member, representative));
  assert_true(dt_image_repository_set_group(second_member, representative));
  GList *members = dt_image_repository_get_group_members(representative, -1);
  assert_int_equal(g_list_length(members), 3);
  assert_non_null(g_list_find(members, GINT_TO_POINTER(representative)));
  assert_non_null(g_list_find(members, GINT_TO_POINTER(first_member)));
  assert_non_null(g_list_find(members, GINT_TO_POINTER(second_member)));
  g_list_free(members);
}

static void test_expansion_rebuilds_the_production_lut(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/thumbtable-lut/");
  const int32_t representative = testdb_make_image(film_id, "representative.raw");
  const int32_t member = testdb_make_image(film_id, "member.raw");
  dt_thumbtable_t table = { .mode = DT_THUMBTABLE_MODE_FILEMANAGER,
                             .collapse_groups = TRUE,
                             .collection_inited = TRUE,
                             .expanded_group_id = UNKNOWN_IMAGE };
  dt_image_t image;

  assert_true(dt_image_repository_load(representative, &image));
  image.group_id = representative;
  dt_image_repository_store(&image);
  assert_true(dt_image_repository_set_group(member, representative));
  assert_true(dt_image_repository_load(member, &image));
  assert_int_equal(image.group_id, representative);

  const dt_collection_rule_t rule = { .property = DT_COLLECTION_PROP_FILMROLL,
                                      .mode = 0,
                                      .text = "/testdb/thumbtable-lut/" };
  dt_collection_set_rules(dt_collection_get_global(), &rule, 1);
  dt_collection_update(dt_collection_get_global());
  dt_collection_memory_update();
  assert_int_equal(dt_collection_get_count(dt_collection_get_global()), 2);
  table.list = g_hash_table_new(g_direct_hash, g_direct_equal);
  dt_pthread_mutex_init(&table.lock, NULL);

  assert_true(dt_thumbtable_toggle_expanded_group(&table, representative));
  assert_int_equal(table.expanded_group_id, representative);
  assert_int_equal(table.collection_count, 2);
  assert_true(table.lut[0].imgid == representative || table.lut[1].imgid == representative);
  assert_true(table.lut[0].imgid == member || table.lut[1].imgid == member);
  assert_false(dt_thumbtable_toggle_expanded_group(&table, representative));
  assert_int_equal(table.collection_count, 1);
  assert_int_equal(table.lut[0].imgid, representative);

  dt_free(table.lut);
  g_hash_table_destroy(table.list);
  dt_pthread_mutex_destroy(&table.lock);
}

static void test_group_borders_close_every_visual_row(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_cache_t lut[12] = { 0 };
  dt_thumbtable_t table = { .lut = lut, .collection_count = 12, .thumbs_per_row = 5 };
  dt_thumbnail_t thumb = { 0 };
  const dt_thumbnail_border_t all = DT_THUMBNAIL_BORDER_TOP | DT_THUMBNAIL_BORDER_BOTTOM
                                    | DT_THUMBNAIL_BORDER_LEFT | DT_THUMBNAIL_BORDER_RIGHT;
  const struct
  {
    int columns;
    int first;
    int last;
    int row;
    dt_thumbnail_border_t expected;
  } cases[] = {
    { 5, 4, 5, 4, all },
    { 5, 4, 5, 5, all },
    { 5, 1, 2, 1, all & ~DT_THUMBNAIL_BORDER_RIGHT },
    { 5, 1, 2, 2, all & ~DT_THUMBNAIL_BORDER_LEFT },
    { 5, 3, 7, 4, all & ~DT_THUMBNAIL_BORDER_LEFT },
    { 5, 3, 7, 5, all & ~DT_THUMBNAIL_BORDER_RIGHT },
    { 5, 0, 11, 0, DT_THUMBNAIL_BORDER_TOP | DT_THUMBNAIL_BORDER_LEFT },
    { 5, 0, 11, 4, DT_THUMBNAIL_BORDER_TOP | DT_THUMBNAIL_BORDER_RIGHT },
    { 5, 0, 11, 11, DT_THUMBNAIL_BORDER_BOTTOM | DT_THUMBNAIL_BORDER_RIGHT },
    { 1, 0, 2, 1, DT_THUMBNAIL_BORDER_LEFT | DT_THUMBNAIL_BORDER_RIGHT },
    { 5, 4, 10, 9, DT_THUMBNAIL_BORDER_RIGHT | DT_THUMBNAIL_BORDER_BOTTOM
                   | DT_THUMBNAIL_BORDER_INNER_TOP_LEFT },
    { 5, 4, 10, 5, DT_THUMBNAIL_BORDER_LEFT | DT_THUMBNAIL_BORDER_TOP
                   | DT_THUMBNAIL_BORDER_INNER_BOTTOM_RIGHT },
  };
  for(size_t index = 0; index < G_N_ELEMENTS(cases); index++)
  {
    for(int row = 0; row < table.collection_count; row++)
      lut[row].groupid = row >= cases[index].first && row <= cases[index].last ? 100 : row;
    table.thumbs_per_row = cases[index].columns;
    thumb.rowid = cases[index].row;
    thumb.info.group_id = 100;
    dt_thumbnail_border_t borders = DT_THUMBNAIL_BORDER_NONE;
    dt_thumbtable_grid_ops()->group_borders(&table, &thumb, &borders);
    assert_int_equal(borders, cases[index].expected);
  }
}

static void test_group_borders_find_all_inward_corners(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_cache_t lut[9] = { 0 };
  dt_thumbtable_t table = { .lut = lut, .collection_count = 9, .thumbs_per_row = 3 };
  dt_thumbnail_t thumb = { .rowid = 4, .info.group_id = 100 };
  const int diagonals[] = { 0, 2, 6, 8 };
  for(int corner = 0; corner < 4; corner++)
  {
    for(int row = 0; row < 9; row++) lut[row].groupid = 100;
    lut[diagonals[corner]].groupid = 200;
    dt_thumbnail_border_t borders = 0;
    dt_thumbtable_grid_ops()->group_borders(&table, &thumb, &borders);
    assert_int_equal(borders, DT_THUMBNAIL_BORDER_INNER_TOP_LEFT << corner);
  }
}

static void test_only_expanded_groups_keep_border_classes(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_cache_t lut[2] = { { .groupid = 10 }, { .groupid = 10 } };
  dt_thumbtable_t table = { .lut = lut, .collection_count = 2, .thumbs_per_row = 5,
                            .mode = DT_THUMBTABLE_MODE_FILEMANAGER, .ops = dt_thumbtable_grid_ops(),
                            .draw_group_borders = TRUE, .expanded_group_id = UNKNOWN_IMAGE };
  dt_thumbnail_t thumb = { .rowid = 0, .info = { .id = 10, .group_id = 10, .group_members = 2 } };
  table.grid = gtk_fixed_new();
  g_object_ref_sink(table.grid);
  thumb.widget = gtk_event_box_new();
  g_object_ref_sink(thumb.widget);
  thumb.w_main = gtk_overlay_new();
  gtk_container_add(GTK_CONTAINER(thumb.widget), thumb.w_main);

  _add_thumbnail_group_borders(&table, &thumb);
  assert_true(thumb.group_borders != DT_THUMBNAIL_BORDER_NONE);
  table.collapse_groups = TRUE;
  _add_thumbnail_group_borders(&table, &thumb);
  assert_int_equal(thumb.group_borders, DT_THUMBNAIL_BORDER_NONE);
  assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(thumb.widget), "dt_group_left"));
  assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(thumb.widget), "dt_group_top"));

  table.expanded_group_id = 10;
  _add_thumbnail_group_borders(&table, &thumb);
  assert_true(thumb.group_borders != DT_THUMBNAIL_BORDER_NONE);
  table.expanded_group_id = 20;
  _add_thumbnail_group_borders(&table, &thumb);
  assert_int_equal(thumb.group_borders, DT_THUMBNAIL_BORDER_NONE);
  table.expanded_group_id = 10;
  table.mode = DT_THUMBTABLE_MODE_FILMSTRIP;
  _add_thumbnail_group_borders(&table, &thumb);
  assert_int_equal(thumb.group_borders, DT_THUMBNAIL_BORDER_NONE);

  table.mode = DT_THUMBTABLE_MODE_FILEMANAGER;
  table.collapse_groups = FALSE;
  _add_thumbnail_group_borders(&table, &thumb);
  assert_true(thumb.group_borders != DT_THUMBNAIL_BORDER_NONE);
  table.draw_group_borders = FALSE;
  _add_thumbnail_group_borders(&table, &thumb);
  assert_int_equal(thumb.group_borders, DT_THUMBNAIL_BORDER_NONE);
  table.draw_group_borders = TRUE;
  thumb.info.group_members = 1;
  dt_thumbnail_set_group_border(&thumb, DT_THUMBNAIL_BORDER_LEFT | DT_THUMBNAIL_BORDER_INNER_BOTTOM_RIGHT);
  _add_thumbnail_group_borders(&table, &thumb);
  assert_int_equal(thumb.group_borders, DT_THUMBNAIL_BORDER_NONE);
  assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(thumb.widget), "dt_group_left"));

  gtk_widget_destroy(thumb.widget);
  g_object_unref(thumb.widget);
  g_object_unref(table.grid);
}

extern void _widget_set_size(GtkWidget *widget, int *width, int *height, gboolean update);
extern gboolean _event_expose(GtkWidget *widget, cairo_t *cr, gpointer user_data);

static void test_group_layout_preserves_joins_and_separates_groups(void **state G_GNUC_UNUSED)
{
  GtkCssProvider *css = gtk_css_provider_new();
  assert_true(gtk_css_provider_load_from_path(css, ANSEL_TEST_SOURCE_DIR "/data/themes/ansel.css", NULL));
  gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  GtkWidget *window = gtk_offscreen_window_new();
  GtkWidget *grid = gtk_fixed_new();
  gtk_widget_set_name(grid, "thumbtable-filemanager");
  gtk_container_add(GTK_CONTAINER(window), grid);
  dt_thumbtable_cache_t lut[4] = { { .groupid = 1 }, { .groupid = 1 }, { .groupid = 2 }, { .groupid = 2 } };
  dt_thumbtable_t table = { .lut = lut, .collection_count = 4, .thumbs_per_row = 4, .grid = grid,
                            .mode = DT_THUMBTABLE_MODE_FILEMANAGER, .ops = dt_thumbtable_grid_ops(),
                            .expanded_group_id = UNKNOWN_IMAGE };
  dt_thumbnail_t thumbs[4] = { 0 };
  for(int index = 0; index < 4; index++)
  {
    dt_thumbnail_t *thumb = thumbs + index;
    thumb->widget = gtk_event_box_new();
    thumb->w_main = gtk_overlay_new();
    thumb->w_image = gtk_drawing_area_new();
    thumb->table = &table;
    thumb->width = 120;
    thumb->height = 120;
    dt_gui_add_class(thumb->w_main, "thumb-main");
    dt_gui_add_class(thumb->w_image, "thumb-image");
    gtk_widget_set_halign(thumb->w_main, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(thumb->w_main, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(thumb->w_main), thumb->w_image);
    gtk_container_add(GTK_CONTAINER(thumb->widget), thumb->w_main);
    dt_gui_add_class(thumb->widget, "thumb-cell");
    g_signal_connect_after(thumb->widget, "draw", G_CALLBACK(_event_expose), thumb);
    thumb->rowid = index;
    thumb->info.group_id = lut[index].groupid;
    thumb->info.group_members = 2;
    thumb->x = index * 120;
    dt_thumbtable_grid_ops()->place_child(&table, thumb);
    int width = 120;
    int height = 120;
    _widget_set_size(thumb->widget, &width, &height, TRUE);
    _widget_set_size(thumb->w_main, &width, &height, TRUE);
    _widget_set_size(thumb->w_image, &width, &height, FALSE);
    int image_width;
    int image_height;
    gtk_widget_get_size_request(thumb->w_image, &image_width, &image_height);
    assert_int_equal(image_width, 108);
    assert_int_equal(image_height, 108);
    GtkBorder margin;
    GtkBorder padding;
    GtkBorder border;
    GtkStyleContext *context = gtk_widget_get_style_context(thumb->widget);
    gtk_style_context_get_margin(context, GTK_STATE_FLAG_NORMAL, &margin);
    gtk_style_context_get_padding(context, GTK_STATE_FLAG_NORMAL, &padding);
    gtk_style_context_get_border(context, GTK_STATE_FLAG_NORMAL, &border);
    const GtkBorder original_margin = { -2, -2, -2, -2 };
    const GtkBorder original_padding = { 0 };
    const GtkBorder original_border = { 4, 4, 4, 4 };
    assert_memory_equal(&margin, &original_margin, sizeof(margin));
    assert_memory_equal(&padding, &original_padding, sizeof(padding));
    assert_memory_equal(&border, &original_border, sizeof(border));
  }
  gtk_widget_show_all(window);
  const gint64 deadline = g_get_monotonic_time() + 100000;
  while(g_get_monotonic_time() < deadline)
  {
    while(g_main_context_iteration(NULL, FALSE));
    g_usleep(1000);
  }
  GtkAllocation allocations[4];
  GtkAllocation image_allocations[4];
  for(int index = 0; index < 4; index++)
  {
    gtk_widget_get_allocation(thumbs[index].widget, allocations + index);
    gtk_widget_get_allocation(thumbs[index].w_image, image_allocations + index);
    assert_int_equal(allocations[index].x, index * 120);
  }
  assert_int_equal(dt_thumbtable_thumb_cell_decoration(), 4);

  table.draw_group_borders = TRUE;
  for(int index = 0; index < 4; index++) _add_thumbnail_group_borders(&table, thumbs + index);
  const gint64 paint_deadline = g_get_monotonic_time() + 100000;
  while(g_get_monotonic_time() < paint_deadline)
  {
    while(g_main_context_iteration(NULL, FALSE));
    g_usleep(1000);
  }

  GdkPixbuf *image = gtk_offscreen_window_get_pixbuf(GTK_OFFSCREEN_WINDOW(window));
  assert_non_null(image);
  const guchar *pixels = gdk_pixbuf_read_pixels(image);
  const int stride = gdk_pixbuf_get_rowstride(image);
  const int channels = gdk_pixbuf_get_n_channels(image);
  const int y = allocations[0].y + 2;
  const guchar *border = pixels + y * stride + (allocations[0].x + 10) * channels;
  const guchar *join = pixels + y * stride + allocations[1].x * channels;
  const guchar *gap = pixels + y * stride + 239 * channels;
  assert_memory_equal(border, join, 3);
  assert_true(memcmp(border, gap, 3) != 0);
  assert_true(memcmp(border, gap + channels, 3) != 0);
  assert_memory_equal(border, gap + 2 * channels, 3);
  g_object_unref(image);

  for(int mode = 0; mode < 5; mode++)
  {
    table.collapse_groups = mode == 1 || mode == 2;
    table.expanded_group_id = mode == 2 ? 1 : UNKNOWN_IMAGE;
    table.draw_group_borders = mode != 3;
    for(int index = 0; index < 4; index++)
    {
      thumbs[index].info.group_members = mode == 4 ? 1 : 2;
      _add_thumbnail_group_borders(&table, thumbs + index);
    }
    while(g_main_context_iteration(NULL, FALSE));
    for(int index = 0; index < 4; index++)
    {
      GtkAllocation allocation;
      GtkAllocation image_allocation;
      gtk_widget_get_allocation(thumbs[index].widget, &allocation);
      gtk_widget_get_allocation(thumbs[index].w_image, &image_allocation);
      assert_memory_equal(&allocation, allocations + index, sizeof(allocation));
      assert_memory_equal(&image_allocation, image_allocations + index, sizeof(image_allocation));
    }
  }
  gtk_widget_destroy(window);
  gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css));
  g_object_unref(css);
}

static void _reload_group_cache(const int32_t leader, const int32_t member)
{
  const dt_image_t *image = dt_image_cache_get_reload(leader, 'r');
  dt_image_cache_read_release(image);
  image = dt_image_cache_get_reload(member, 'r');
  dt_image_cache_read_release(image);
}

static void test_group_reload_does_not_clone_stale_filmstrip(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/group-refresh/");
  const int32_t leader = testdb_make_image(film_id, "leader.raw");
  const int32_t member = testdb_make_image(film_id, "member.raw");
  assert_true(dt_image_repository_set_group(leader, leader));
  assert_true(dt_image_repository_set_group(member, member));
  const dt_collection_rule_t rule = { .property = DT_COLLECTION_PROP_FILMROLL,
                                      .mode = 0, .text = "/testdb/group-refresh/" };
  dt_collection_set_rules(dt_collection_get_global(), &rule, 1);
  dt_collection_update(dt_collection_get_global());
  dt_collection_memory_update();
  dt_conf_set_bool("ui_last/grouping", TRUE);

  dt_thumbtable_t table = { .mode = DT_THUMBTABLE_MODE_FILEMANAGER,
                            .ops = dt_thumbtable_grid_ops(), .collapse_groups = TRUE,
                            .expanded_group_id = UNKNOWN_IMAGE, .thumb_height = 100, .thumbs_per_row = 5 };
  dt_thumbtable_t filmstrip = { .mode = DT_THUMBTABLE_MODE_FILMSTRIP, .collapse_groups = TRUE,
                                .expanded_group_id = UNKNOWN_IMAGE };
  dt_pthread_mutex_init(&table.lock, NULL);
  dt_pthread_mutex_init(&filmstrip.lock, NULL);
  table.list = g_hash_table_new(g_direct_hash, g_direct_equal);
  table.scroll_window = gtk_scrolled_window_new(NULL, NULL);
  g_object_ref_sink(table.scroll_window);
  table.v_scrollbar = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(table.scroll_window));
  table.h_scrollbar = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(table.scroll_window));
  _dt_collection_get_hash(&table);
  _dt_collection_get_hash(&filmstrip);
  _dt_collection_lut(&table);
  _dt_collection_lut(&filmstrip);
  assert_int_equal(table.collection_count, 2);
  dt_ui_t ui = { .thumbtable_lighttable = &table, .thumbtable_filmstrip = &filmstrip };
  dt_gui_gtk_t gui = { .ui = &ui };
  darktable.gui = &gui;

  assert_true(dt_image_repository_set_group(member, leader));
  _reload_group_cache(leader, member);
  _dt_collection_changed_callback(NULL, DT_COLLECTION_CHANGE_BACKGROUND_SYNC,
                                   DT_COLLECTION_PROP_GROUPING, NULL, -1, &table);
  assert_int_equal(table.collection_count, 1);
  assert_int_equal(table.lut[0].imgid, leader);
  assert_int_equal(filmstrip.collection_count, 2);

  assert_true(dt_image_repository_set_group(member, member));
  _reload_group_cache(leader, member);
  _dt_collection_changed_callback(NULL, DT_COLLECTION_CHANGE_BACKGROUND_SYNC,
                                   DT_COLLECTION_PROP_GROUPING, NULL, -1, &table);
  assert_int_equal(table.collection_count, 2);
  assert_true(dt_image_repository_set_group(member, leader));
  _reload_group_cache(leader, member);
  _dt_collection_changed_callback(NULL, DT_COLLECTION_CHANGE_BACKGROUND_SYNC,
                                   DT_COLLECTION_PROP_GROUPING, NULL, -1, &table);
  assert_int_equal(table.collection_count, 1);
  dt_conf_set_bool("ui_last/grouping", FALSE);
  _dt_collection_changed_callback(NULL, DT_COLLECTION_CHANGE_BACKGROUND_SYNC,
                                   DT_COLLECTION_PROP_GROUPING, NULL, -1, &table);
  assert_int_equal(table.collection_count, 2);

  assert_true(dt_image_repository_set_group(leader, member));
  assert_true(dt_image_repository_set_group(member, member));
  _reload_group_cache(leader, member);
  _dt_collection_changed_callback(NULL, DT_COLLECTION_CHANGE_BACKGROUND_SYNC,
                                   DT_COLLECTION_PROP_GROUPING, NULL, -1, &table);
  assert_int_equal(table.lut[0].groupid, member);
  assert_int_equal(table.lut[1].groupid, member);
  dt_thumbnail_t thumb = { .rowid = 0, .info.group_id = member };
  dt_thumbnail_border_t borders = 0;
  table.ops->group_borders(&table, &thumb, &borders);
  assert_false(borders & DT_THUMBNAIL_BORDER_RIGHT);

  dt_conf_set_bool("ui_last/grouping", TRUE);
  table.collapse_groups = TRUE;
  table.expanded_group_id = member;
  assert_true(dt_image_repository_set_group(leader, leader));
  assert_true(dt_image_repository_set_group(member, leader));
  _reload_group_cache(leader, member);
  table.expanded_group_id = leader;
  _dt_collection_changed_callback(NULL, DT_COLLECTION_CHANGE_GROUP_REPRESENTATIVE,
                                   DT_COLLECTION_PROP_GROUPING, NULL, -1, &table);
  assert_int_equal(table.expanded_group_id, leader);
  assert_int_equal(table.collection_count, 2);
  assert_int_equal(table.lut[0].groupid, leader);
  assert_int_equal(table.lut[1].groupid, leader);
  thumb.info.group_id = leader;
  borders = 0;
  table.ops->group_borders(&table, &thumb, &borders);
  assert_false(borders & DT_THUMBNAIL_BORDER_RIGHT);
  g_source_remove(table.focus_idle_id);
  table.focus_idle_id = 0;

  _dt_collection_changed_callback(NULL, DT_COLLECTION_CHANGE_BACKGROUND_SYNC,
                                   DT_COLLECTION_PROP_GROUPING, NULL, -1, &table);
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);
  assert_int_equal(table.collection_count, 1);

  darktable.gui = NULL;
  g_object_unref(table.scroll_window);
  g_hash_table_destroy(table.list);
  dt_free(table.lut);
  dt_free(filmstrip.lut);
  dt_pthread_mutex_destroy(&table.lock);
  dt_pthread_mutex_destroy(&filmstrip.lock);
}

int main(int argc, char *argv[])
{
  assert_true(gtk_init_check(&argc, &argv));
  dt_datetime_init();
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_expanded_group_toggle_replaces_and_clears),
    cmocka_unit_test(test_filmstrip_has_no_expanded_group),
    cmocka_unit_test(test_uncollapsed_lighttable_cannot_expand_a_group),
    cmocka_unit_test(test_expanded_group_members_include_the_representative),
    cmocka_unit_test(test_expansion_rebuilds_the_production_lut),
    cmocka_unit_test(test_group_borders_close_every_visual_row),
    cmocka_unit_test(test_group_borders_find_all_inward_corners),
    cmocka_unit_test(test_only_expanded_groups_keep_border_classes),
    cmocka_unit_test(test_group_layout_preserves_joins_and_separates_groups),
    cmocka_unit_test(test_group_reload_does_not_clone_stale_filmstrip),
  };
  return cmocka_run_group_tests(tests, _setup, _teardown);
}
