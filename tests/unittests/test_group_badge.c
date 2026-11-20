/*
    This file is part of Ansel,
    Copyright (C) 2026 Aurélien PIERRE.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "testdb.h"
#include "widgets/paint.h"
#include "../../src/gui/dtgtk/thumbnail.c"

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static int _representative_changes;
static int _grouping_reloads;
static dt_thumbnail_t *_representative_thumb;
static int32_t _expected_expanded_group;
static gboolean _representative_failed;

int dt_grouping_change_representative(const int32_t image_id)
{
  _representative_changes++;
  if(_representative_failed) return UNKNOWN_IMAGE;
  if(!IS_NULL_PTR(_representative_thumb)) _representative_thumb->info.group_id = image_id;
  return image_id;
}

void dt_collection_update_query(const dt_collection_t *collection G_GNUC_UNUSED, dt_collection_change_t change,
                                dt_collection_properties_t property, GList *images)
{
  assert_int_equal(change, DT_COLLECTION_CHANGE_GROUP_REPRESENTATIVE);
  assert_int_equal(property, DT_COLLECTION_PROP_GROUPING);
  assert_null(images);
  if(!IS_NULL_PTR(_representative_thumb))
    assert_int_equal(_representative_thumb->table->expanded_group_id, _expected_expanded_group);
  _grouping_reloads++;
}

static void test_group_badge_labels_exact_counts(void **state)
{
  (void)state;
  char label[4] = { 0 };

  assert_true(dtgtk_grouping_badge_label(2, label));
  assert_string_equal(label, "2");
  assert_true(dtgtk_grouping_badge_label(99, label));
  assert_string_equal(label, "99");
  assert_true(dtgtk_grouping_badge_label(100, label));
  assert_string_equal(label, "99+");
}

static void test_single_image_has_no_group_badge(void **state)
{
  (void)state;
  char label[4] = { 'x', 0 };

  assert_false(dtgtk_grouping_badge_label(1, label));
  assert_string_equal(label, "");
}

static void test_group_badge_paints_at_hidpi(void **state)
{
  (void)state;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 96, 96);
  cairo_surface_set_device_scale(surface, 2.0, 2.0);
  cairo_t *cr = cairo_create(surface);
  guint members = 99;

  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  dtgtk_cairo_paint_grouping(cr, 0, 0, 48, 48, CPF_GROUPING_BADGE, &members);
  cairo_surface_flush(surface);

  const unsigned char *pixels = cairo_image_surface_get_data(surface);
  gboolean painted = FALSE;
  for(int i = 0; i < 96 * 96 * 4; i += 4)
    if(pixels[i + 3] != 0)
    {
      painted = TRUE;
      break;
    }

  assert_true(painted);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

static void test_grouping_glyph_accepts_existing_null_data(void **state)
{
  (void)state;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48, 48);
  cairo_t *cr = cairo_create(surface);

  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  dtgtk_cairo_paint_grouping(cr, 0, 0, 48, 48, CPF_NONE, NULL);
  cairo_surface_flush(surface);

  const unsigned char *pixels = cairo_image_surface_get_data(surface);
  gboolean painted = FALSE;
  for(int i = 0; i < 48 * 48 * 4; i += 4)
    if(pixels[i + 3] != 0)
    {
      painted = TRUE;
      break;
    }

  assert_true(painted);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

static void test_active_group_badge_keeps_cairo_state_and_contrast(void **state)
{
  (void)state;
  cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 64, 64);
  cairo_t *cr = cairo_create(surface);
  guint members = 100;
  cairo_matrix_t matrix;

  cairo_translate(cr, 2.0, 3.0);
  cairo_scale(cr, 0.9, 0.9);
  cairo_set_source_rgba(cr, 0.25, 0.5, 0.75, 1.0);
  cairo_get_matrix(cr, &matrix);
  dtgtk_cairo_paint_grouping(cr, 0, 0, 48, 48, CPF_ACTIVE | CPF_GROUPING_BADGE, &members);

  assert_int_equal(cairo_get_operator(cr), CAIRO_OPERATOR_OVER);
  cairo_matrix_t result;
  cairo_get_matrix(cr, &result);
  assert_memory_equal(&result, &matrix, sizeof(matrix));
  double red = 0.0;
  double green = 0.0;
  double blue = 0.0;
  double alpha = 0.0;
  assert_int_equal(cairo_pattern_get_rgba(cairo_get_source(cr), &red, &green, &blue, &alpha), CAIRO_STATUS_SUCCESS);
  assert_true(red == 0.25 && green == 0.5 && blue == 0.75 && alpha == 1.0);

  cairo_destroy(cr);
  cairo_surface_destroy(surface);

  surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48, 48);
  cr = cairo_create(surface);
  cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
  dtgtk_cairo_paint_grouping(cr, 0, 0, 48, 48, CPF_ACTIVE | CPF_GROUPING_BADGE, &members);
  cairo_surface_flush(surface);
  const unsigned char *pixels = cairo_image_surface_get_data(surface);
  gboolean light = FALSE;
  gboolean dark = FALSE;
  for(int y = 22; y < 38; y++)
    for(int x = 5; x < 33; x++)
    {
      const unsigned char *pixel = pixels + (y * 48 + x) * 4;
      assert_int_equal(pixel[3], 255);
      if(pixel[0] > 200) light = TRUE;
      if(pixel[0] < 50) dark = TRUE;
    }

  assert_true(light);
  assert_true(dark);
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
}

static void test_group_member_rows_report_all_members(void **state G_GNUC_UNUSED)
{
  const int32_t film_id = testdb_make_film("/testdb/group-badge");
  const int32_t representative = testdb_make_image(film_id, "representative.raw");
  const int32_t member = testdb_make_image(film_id, "member.raw");
  assert_true(representative > 0);
  assert_true(member > 0);
  assert_true(dt_image_repository_set_group(representative, representative));
  assert_true(dt_image_repository_set_group(member, representative));
  GList *members = dt_image_repository_get_group_member_rows(representative);
  assert_int_equal(g_list_length(members), 2);
  g_list_free_full(members, dt_image_group_member_free);
}

static void test_group_badge_scales_with_extension_text(void **state G_GNUC_UNUSED)
{
  dt_thumbnail_t thumb = { 0 };
  thumb.widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink(thumb.widget);
  GtkWidget **widgets[] = { &thumb.w_main, &thumb.w_reject, &thumb.w_color, &thumb.w_local_copy,
                            &thumb.w_altered, &thumb.w_group, &thumb.w_audio, &thumb.w_cursor };
  for(size_t index = 0; index < G_N_ELEMENTS(widgets); index++)
  {
    *widgets[index] = gtk_drawing_area_new();
    gtk_box_pack_start(GTK_BOX(thumb.widget), *widgets[index], FALSE, FALSE, 0);
  }
  for(int index = 0; index < MAX_STARS; index++)
  {
    thumb.w_stars[index] = gtk_drawing_area_new();
    gtk_box_pack_start(GTK_BOX(thumb.widget), thumb.w_stars[index], FALSE, FALSE, 0);
  }
  thumb.w_ext = gtk_label_new("RAW");
  gtk_box_pack_start(GTK_BOX(thumb.widget), thumb.w_ext, FALSE, FALSE, 0);
  const int widths[] = { 600, 400, 280, 180, 120, 80, 40 };
  int previous_height = G_MAXINT;
  for(size_t index = 0; index < G_N_ELEMENTS(widths); index++)
  {
    const int icon_size = _thumb_resize_overlays(&thumb, widths[index], widths[index]);
    int width;
    int height;
    gtk_widget_get_size_request(thumb.w_group, &width, &height);
    const int scaled_height = roundf(1.4f * icon_size);
    assert_int_equal(height, MAX(1, scaled_height));
    assert_int_equal(width, MAX(1, roundf(1.4f * scaled_height)));
    assert_true(height <= previous_height);
    previous_height = height;
    PangoAttrIterator *iter = pango_attr_list_get_iterator(gtk_label_get_attributes(GTK_LABEL(thumb.w_ext)));
    PangoAttrInt *size = (PangoAttrInt *)pango_attr_iterator_get(iter, PANGO_ATTR_ABSOLUTE_SIZE);
    assert_non_null(size);
    assert_int_equal(size->value, (int)(icon_size * PANGO_SCALE * 0.9));
    pango_attr_iterator_destroy(iter);
  }
  gtk_widget_destroy(thumb.widget);
  g_object_unref(thumb.widget);
}

static void test_representative_click_reloads_grouping(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_t table = { .mode = DT_THUMBTABLE_MODE_FILEMANAGER, .collapse_groups = TRUE,
                            .expanded_group_id = 10 };
  dt_thumbnail_t thumb = { .info = { .id = 11, .group_id = 10, .group_members = 3 }, .table = &table };
  thumb.widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink(thumb.widget);
  thumb.w_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  thumb.w_top_eb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  thumb.w_group = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_grouping, CPF_GROUPING_BADGE, &thumb.info.group_members);
  gtk_container_add(GTK_CONTAINER(thumb.widget), thumb.w_main);
  gtk_container_add(GTK_CONTAINER(thumb.w_main), thumb.w_top_eb);
  gtk_container_add(GTK_CONTAINER(thumb.w_top_eb), thumb.w_group);
  gtk_widget_show_all(thumb.widget);
  GdkEventButton event = { .button = 1 };
  _representative_changes = 0;
  _grouping_reloads = 0;
  _representative_thumb = &thumb;
  _expected_expanded_group = 11;
  _event_grouping_release(thumb.w_group, &event, &thumb);
  assert_int_equal(_representative_changes, 1);
  assert_int_equal(_grouping_reloads, 1);
  assert_int_equal(table.expanded_group_id, 11);
  _thumb_update_icons(&thumb);
  assert_false(DTGTK_THUMBNAIL_BTN(thumb.w_group)->icon_flags & CPF_GROUPING_BADGE);
  thumb.info.group_id = thumb.info.id;
  _event_grouping_release(thumb.w_group, &event, &thumb);
  assert_int_equal(_representative_changes, 1);
  assert_int_equal(_grouping_reloads, 1);

  thumb.info.group_id = 10;
  table.expanded_group_id = 42;
  _expected_expanded_group = 42;
  _event_grouping_release(thumb.w_group, &event, &thumb);
  assert_int_equal(_grouping_reloads, 2);
  assert_int_equal(table.expanded_group_id, 42);

  thumb.info.group_id = 10;
  table.collapse_groups = FALSE;
  table.expanded_group_id = UNKNOWN_IMAGE;
  _expected_expanded_group = UNKNOWN_IMAGE;
  _event_grouping_release(thumb.w_group, &event, &thumb);
  assert_int_equal(_grouping_reloads, 3);
  assert_int_equal(table.expanded_group_id, UNKNOWN_IMAGE);

  thumb.info.group_id = 10;
  table.collapse_groups = TRUE;
  table.expanded_group_id = 10;
  _representative_failed = TRUE;
  _event_grouping_release(thumb.w_group, &event, &thumb);
  assert_int_equal(_grouping_reloads, 3);
  assert_int_equal(table.expanded_group_id, 10);
  _representative_failed = FALSE;
  _representative_thumb = NULL;
  gtk_widget_destroy(thumb.widget);
  g_object_unref(thumb.widget);
}

static void test_count_visibility_tracks_each_groups_expansion(void **state G_GNUC_UNUSED)
{
  dt_thumbtable_t table = { .mode = DT_THUMBTABLE_MODE_FILEMANAGER, .collapse_groups = TRUE,
                            .expanded_group_id = UNKNOWN_IMAGE };
  dt_thumbnail_t thumb = { .info = { .id = 10, .group_id = 10, .group_members = 3 }, .table = &table };
  thumb.widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  g_object_ref_sink(thumb.widget);
  thumb.w_main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  thumb.w_top_eb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  thumb.w_group = dtgtk_thumbnail_btn_new(dtgtk_cairo_paint_grouping, CPF_GROUPING_THUMBNAIL,
                                          &thumb.info.group_members);
  gtk_container_add(GTK_CONTAINER(thumb.widget), thumb.w_main);
  gtk_container_add(GTK_CONTAINER(thumb.w_main), thumb.w_top_eb);
  gtk_container_add(GTK_CONTAINER(thumb.w_top_eb), thumb.w_group);
  gtk_widget_set_size_request(thumb.w_group, 39, 28);
  const GtkDarktableThumbnailBtn *button = DTGTK_THUMBNAIL_BTN(thumb.w_group);

  for(int overlay = DT_THUMBNAIL_OVERLAYS_NONE; overlay < DT_THUMBNAIL_OVERLAYS_LAST; overlay++)
  {
    thumb.over = overlay;
    table.collapse_groups = TRUE;
    table.expanded_group_id = UNKNOWN_IMAGE;
    thumb.info.id = 10;
    _thumb_update_icons(&thumb);
    assert_true(gtk_widget_get_visible(thumb.w_group));
    assert_true(button->icon_flags & CPF_GROUPING_BADGE);
    assert_true(button->icon_flags & CPF_GROUPING_THUMBNAIL);
    assert_true(gtk_widget_get_state_flags(thumb.w_group) & GTK_STATE_FLAG_ACTIVE);

    table.collapse_groups = FALSE;
    _thumb_update_icons(&thumb);
    assert_false(button->icon_flags & CPF_GROUPING_BADGE);
    assert_true(gtk_widget_get_state_flags(thumb.w_group) & GTK_STATE_FLAG_ACTIVE);
    thumb.info.id = 11;
    _thumb_update_icons(&thumb);
    assert_false(button->icon_flags & CPF_GROUPING_BADGE);
    assert_false(gtk_widget_get_state_flags(thumb.w_group) & GTK_STATE_FLAG_ACTIVE);

    table.collapse_groups = TRUE;
    table.expanded_group_id = 10;
    _thumb_update_icons(&thumb);
    assert_false(button->icon_flags & CPF_GROUPING_BADGE);
    table.expanded_group_id = 20;
    thumb.info.id = 10;
    _thumb_update_icons(&thumb);
    assert_true(button->icon_flags & CPF_GROUPING_BADGE);
    int width;
    int height;
    gtk_widget_get_size_request(thumb.w_group, &width, &height);
    assert_int_equal(width, 39);
    assert_int_equal(height, 28);
  }

  table.mode = DT_THUMBTABLE_MODE_FILMSTRIP;
  table.expanded_group_id = 10;
  _thumb_update_icons(&thumb);
  assert_true(button->icon_flags & CPF_GROUPING_BADGE);
  table.collapse_groups = FALSE;
  _thumb_update_icons(&thumb);
  assert_false(button->icon_flags & CPF_GROUPING_BADGE);
  thumb.table = NULL;
  _thumb_update_icons(&thumb);
  assert_false(button->icon_flags & CPF_GROUPING_BADGE);
  thumb.info.group_members = 1;
  _thumb_update_icons(&thumb);
  assert_false(gtk_widget_get_visible(thumb.w_group));
  gtk_widget_destroy(thumb.widget);
  g_object_unref(thumb.widget);
}

static void test_expanded_leader_and_member_glyphs_have_no_count(void **state G_GNUC_UNUSED)
{
  cairo_surface_t *surfaces[4];
  const gint flags[] = { CPF_GROUPING_THUMBNAIL | CPF_ACTIVE, CPF_GROUPING_THUMBNAIL,
                        CPF_GROUPING_THUMBNAIL | CPF_ACTIVE, CPF_GROUPING_THUMBNAIL | CPF_ACTIVE | CPF_GROUPING_BADGE };
  guint counts[] = { 2, 2, 99, 2 };
  for(size_t index = 0; index < G_N_ELEMENTS(surfaces); index++)
  {
    surfaces[index] = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 80, 60);
    cairo_t *cr = cairo_create(surfaces[index]);
    cairo_set_source_rgb(cr, 1, 1, 1);
    dtgtk_cairo_paint_grouping(cr, 4, 4, 70, 50, flags[index], counts + index);
    assert_int_equal(cairo_status(cr), CAIRO_STATUS_SUCCESS);
    cairo_destroy(cr);
    cairo_surface_flush(surfaces[index]);
  }
  const unsigned char *leader = cairo_image_surface_get_data(surfaces[0]);
  const unsigned char *member = cairo_image_surface_get_data(surfaces[1]);
  const unsigned char *other_count = cairo_image_surface_get_data(surfaces[2]);
  const unsigned char *collapsed = cairo_image_surface_get_data(surfaces[3]);
  const int bytes = cairo_image_surface_get_stride(surfaces[0]) * 60;
  assert_int_equal(((const uint32_t *)leader)[35 * 80 + 32] >> 24, 255);
  assert_int_equal(((const uint32_t *)member)[35 * 80 + 32] >> 24, 0);
  assert_memory_equal(leader, other_count, bytes);
  assert_true(memcmp(leader, collapsed, bytes) != 0);
  for(size_t index = 0; index < G_N_ELEMENTS(surfaces); index++) cairo_surface_destroy(surfaces[index]);
}

int main(int argc, char *argv[])
{
  assert_true(gtk_init_check(&argc, &argv));
  const struct CMUnitTest tests[] = { cmocka_unit_test(test_group_member_rows_report_all_members) };
  const struct CMUnitTest badge_tests[] = {
    cmocka_unit_test(test_group_badge_labels_exact_counts),
    cmocka_unit_test(test_single_image_has_no_group_badge),
    cmocka_unit_test(test_group_badge_paints_at_hidpi),
    cmocka_unit_test(test_grouping_glyph_accepts_existing_null_data),
    cmocka_unit_test(test_active_group_badge_keeps_cairo_state_and_contrast),
    cmocka_unit_test(test_group_badge_scales_with_extension_text),
    cmocka_unit_test(test_representative_click_reloads_grouping),
    cmocka_unit_test(test_count_visibility_tracks_each_groups_expansion),
    cmocka_unit_test(test_expanded_leader_and_member_glyphs_have_no_count),
  };
  return cmocka_run_group_tests_name("group badge", badge_tests, NULL, NULL)
         + cmocka_run_group_tests(tests, testdb_setup, testdb_teardown);
}
