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

/** The contract behind "undo remove from library" (doc/removal-undo.md).
 *
 * Removing an image deletes its `main.images` row and the foreign keys take its history,
 * masks, tags, colour labels, metadata, module order and history end with it. The undo can
 * only be as good as what was staged first, so what these tests pin is that a snapshot is
 * COMPLETE -- every table comes back, not merely the image row, which is the failure this
 * design is exposed to: a restore that looks right because the image is listed again while
 * its edit is gone.
 *
 * Two properties beyond the round trip get their own tests, because both are invisible in
 * the common case and wrong only at the edges: the group membership of images NOBODY asked
 * to remove (rewritten on the way out, staged separately), and the film roll (swept when its
 * last image goes, and referenced by a foreign key on the way back).
 *
 * The removal itself is spelled out here rather than called: dt_image_remove_undoable()
 * lives in common/image.c and drags the image cache, the mipmap cache and the undo stack in
 * with it. What is under test is the repository, so _remove() below reproduces the only two
 * database effects the job has -- the grouping hand-over and the row delete -- through the
 * repository API, the same way testdb.h requires of every test here.
 *
 * Note what is NOT used: dt_image_repository_load(). It maps the row through
 * dt_image_film_roll_name(), which reads a conf key, and this fixture initialises no conf
 * system. Every assertion below therefore reads its column through a narrower query.
 */

#include "testdb.h"

#include "database/removed_image_repository.h"

// an arbitrary flag word, distinctive enough that a restored row cannot carry it by accident
#define SEEDED_FLAGS 0x51
// likewise for a scalar column that is not the identity
#define SEEDED_VERSION 7

/** Give @p imgid one row in every table dt_removed_image_repository_create() stages. */
static void _seed_every_table(const int32_t imgid, const char *tag_name, const char *meta_value)
{
  assert_true(dt_image_repository_set_flags(imgid, SEEDED_FLAGS));
  assert_true(dt_image_repository_set_version(imgid, SEEDED_VERSION));
  /* An image imported through the application leads a group of its own;
   * dt_image_repository_insert_import() leaves group_id NULL, and an image in no group has
   * no membership to stage or restore -- which would make the group assertions vacuous. */
  assert_true(dt_image_repository_set_group(imgid, imgid));

  assert_true(dt_history_repository_write_item(imgid, 0, "exposure", "\x01\x02", 2, 3, TRUE,
                                               "\x03", 1, 9, 0, "the instance"));
  assert_true(dt_history_repository_write_mask_item(imgid, 0, 7, 4, "circle", 3,
                                                    "\xAA\xBB", 2, 2, "\x00", 1));
  assert_true(dt_history_repository_set_module_order(imgid, 2, "exposure,colorin"));
  assert_true(dt_history_repository_set_end(imgid, 1));

  dt_colorlabel_repository_set(imgid, 1);

  const guint tagid = dt_tag_repository_insert(tag_name);
  assert_true(tagid > 0);
  assert_true(dt_tag_repository_attach(tagid, imgid));

  const dt_metadata_row_t meta = { .imgid = imgid, .keyid = 3, .value = meta_value };
  dt_metadata_repository_add(&meta, 1);
}

/** What the removal job does to the database, and nothing else. */
static void _remove(const int32_t imgid)
{
  /* dt_grouping_remove_from_group(): a departing leader hands the group to its first
   * survivor. A departing member changes nobody else, and this query is empty for it --
   * nothing has a group_id of a non-leader. */
  GList *survivors = dt_image_repository_get_group_members(imgid, imgid);
  if(!IS_NULL_PTR(survivors))
    dt_image_repository_reassign_group(imgid, GPOINTER_TO_INT(survivors->data), imgid);
  g_list_free(survivors);

  assert_true(dt_image_repository_delete(imgid));
}

static gboolean _in_group(const int32_t group_id, const int32_t imgid)
{
  GList *members = dt_image_repository_get_group_members(group_id, -1);
  const gboolean found = !IS_NULL_PTR(g_list_find(members, GINT_TO_POINTER(imgid)));
  g_list_free(members);
  return found;
}

static gboolean _has_flags(const int32_t imgid, const int flags)
{
  GList *one = g_list_prepend(NULL, GINT_TO_POINTER(imgid));
  GList *flagged = dt_image_repository_get_ids_with_flag_among(one, flags);
  const gboolean found = (g_list_length(flagged) == 1);
  g_list_free(flagged);
  g_list_free(one);
  return found;
}

static void test_round_trip_restores_every_table(void **state)
{
  const int32_t film = testdb_make_film("/testdb/removal/roundtrip");
  const int32_t imgid = testdb_make_image(film, "a.raw");
  assert_true(film > 0 && imgid > 0);
  _seed_every_table(imgid, "testdb|roundtrip", "a creator");

  const int snap = dt_removed_image_repository_next_id(imgid);
  assert_true(dt_removed_image_repository_create(snap, imgid));
  _remove(imgid);

  /* The cascade really did take these, or the round trip below proves nothing. Only four
   * child tables carry a foreign key on images(id) -- history, masks_history, tagged_images
   * and history_hash. `module_order` and `color_labels` carry none, so their rows outlive
   * the removal; they are staged and restored all the same, because the feature must not
   * depend on that gap staying open, and the restore clears the survivors first so a copy
   * on top of them cannot duplicate. */
  assert_int_equal(dt_image_repository_find_by_film_and_filename(film, "a.raw"), -1);
  assert_int_equal(dt_history_repository_count_items(imgid), 0);
  assert_int_equal(dt_history_repository_count_mask_items(imgid), 0);

  assert_true(dt_removed_image_repository_restore(snap, imgid));

  // the row itself, identity and a scalar column that is not the identity
  assert_int_equal(dt_image_repository_find_by_film_and_filename(film, "a.raw"), imgid);
  assert_true(_has_flags(imgid, SEEDED_FLAGS));
  assert_int_equal(dt_image_repository_get_version(imgid), SEEDED_VERSION);
  assert_true(_in_group(imgid, imgid));

  // and every table that hangs off it
  assert_int_equal(dt_history_repository_count_items(imgid), 1);
  assert_int_equal(dt_history_repository_count_mask_items(imgid), 1);
  assert_int_equal(dt_history_repository_get_end(imgid), 1);

  dt_module_order_row_t order = { 0 };
  assert_true(dt_history_repository_get_module_order(imgid, &order));
  assert_int_equal(order.version, 2);
  assert_string_equal(order.iop_list, "exposure,colorin");
  dt_module_order_row_cleanup(&order);

  /* Exactly one label row, not two: main.color_labels has neither a foreign key nor a unique
   * constraint, so the row survived the removal and an unguarded copy would double it. */
  assert_true(dt_colorlabel_repository_has(imgid, 1));
  GList *labels = dt_colorlabel_repository_get_list(imgid);
  assert_int_equal(g_list_length(labels), 1);
  g_list_free(labels);

  assert_true(dt_tag_repository_is_attached(dt_tag_repository_insert("testdb|roundtrip"), imgid));

  GList *values = dt_metadata_repository_get_values(imgid, 3);
  assert_int_equal(g_list_length(values), 1);
  assert_string_equal((const char *)values->data, "a creator");
  g_list_free_full(values, g_free);
}

static void test_group_leader_restores_membership(void **state)
{
  const int32_t film = testdb_make_film("/testdb/removal/group");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  const int32_t c = testdb_make_image(film, "c.raw");
  assert_true(a > 0 && b > 0 && c > 0);
  assert_true(dt_image_repository_set_group(a, a));
  assert_true(dt_image_repository_set_group(b, a));
  assert_true(dt_image_repository_set_group(c, a));

  const int snap = dt_removed_image_repository_next_id(a);
  assert_true(dt_removed_image_repository_create(snap, a));
  _remove(a);

  // the survivors were handed to a new leader -- that rewrite is what has to be undone, and
  // it touched images nobody asked to remove
  assert_true(_in_group(b, b));
  assert_true(_in_group(b, c));

  assert_true(dt_removed_image_repository_restore(snap, a));

  assert_true(_in_group(a, a));
  assert_true(_in_group(a, b));
  assert_true(_in_group(a, c));
}

static void test_film_roll_comes_back_with_its_last_image(void **state)
{
  const int32_t film = testdb_make_film("/testdb/removal/film");
  const int32_t imgid = testdb_make_image(film, "only.raw");
  assert_true(film > 0 && imgid > 0);

  const int snap = dt_removed_image_repository_next_id(imgid);
  assert_true(dt_removed_image_repository_create(snap, imgid));
  _remove(imgid);

  // dt_film_remove_empty(): the roll's last image took the roll with it
  assert_true(dt_film_repository_delete(film));
  assert_true(dt_film_repository_find_by_folder("/testdb/removal/film") <= 0);

  assert_true(dt_removed_image_repository_restore(snap, imgid));

  /* The roll must come back under its ORIGINAL id: main.images.film_id is a foreign key on
   * it, and a roll re-created with a new id would leave the restored image pointing at
   * nothing. The path below is a join over both tables, so it answers only if it does. */
  assert_int_equal(dt_film_repository_find_by_folder("/testdb/removal/film"), film);

  GList *ids = g_list_prepend(NULL, GINT_TO_POINTER(imgid));
  GList *paths = dt_image_repository_get_full_paths(ids);
  assert_int_equal(g_list_length(paths), 1);
  assert_string_equal((const char *)paths->data, "/testdb/removal/film/only.raw");
  g_list_free_full(paths, g_free);
  g_list_free(ids);
}

static void test_partial_undo_repoints_a_departed_leader(void **state)
{
  const int32_t film = testdb_make_film("/testdb/removal/partial");
  const int32_t a = testdb_make_image(film, "a.raw");
  const int32_t b = testdb_make_image(film, "b.raw");
  assert_true(dt_image_repository_set_group(a, a));
  assert_true(dt_image_repository_set_group(b, a));

  /* b leaves first, so ITS snapshot still names a as the group leader. Then a leaves too.
   * Undoing only b is the case the restore has to survive: the group_id it wants to write
   * back names an image that is not coming with it. */
  const int snap_b = dt_removed_image_repository_next_id(b);
  assert_true(dt_removed_image_repository_create(snap_b, b));
  _remove(b);

  const int snap_a = dt_removed_image_repository_next_id(a);
  assert_true(dt_removed_image_repository_create(snap_a, a));
  _remove(a);

  assert_true(dt_removed_image_repository_restore(snap_b, b));

  // b is back, in a group of its own rather than pointing at an image that no longer exists
  assert_int_equal(dt_image_repository_find_by_film_and_filename(film, "b.raw"), b);
  assert_int_equal(dt_image_repository_find_by_film_and_filename(film, "a.raw"), -1);
  assert_true(_in_group(b, b));
  assert_false(_in_group(a, b));
}

static void test_clear_drops_the_snapshot(void **state)
{
  const int32_t film = testdb_make_film("/testdb/removal/clear");
  const int32_t imgid = testdb_make_image(film, "a.raw");
  _seed_every_table(imgid, "testdb|clear", "gone");

  const int snap = dt_removed_image_repository_next_id(imgid);
  assert_true(dt_removed_image_repository_create(snap, imgid));
  _remove(imgid);

  // this is what makes a removal permanent: the undo record was discarded
  dt_removed_image_repository_clear(snap, imgid);

  /* Restoring a dropped snapshot copies nothing. It is not an error -- every statement
   * succeeds over an empty set -- so the check is that the image stayed gone. */
  assert_true(dt_removed_image_repository_restore(snap, imgid));
  assert_int_equal(dt_image_repository_find_by_film_and_filename(film, "a.raw"), -1);
  assert_int_equal(dt_history_repository_count_items(imgid), 0);
}

static void test_successive_snapshots_do_not_collide(void **state)
{
  const int32_t film = testdb_make_film("/testdb/removal/twice");
  const int32_t imgid = testdb_make_image(film, "a.raw");
  assert_true(dt_image_repository_set_flags(imgid, SEEDED_FLAGS));
  assert_true(dt_image_repository_set_group(imgid, imgid));

  const int first = dt_removed_image_repository_next_id(imgid);
  assert_true(dt_removed_image_repository_create(first, imgid));
  _remove(imgid);
  assert_true(dt_removed_image_repository_restore(first, imgid));

  /* Remove and restore the SAME image again. next_id() must not hand back the id the first
   * snapshot still occupies: two copies of the row under one key would leave the primary key
   * to decide which one the restore inserts. */
  const int second = dt_removed_image_repository_next_id(imgid);
  assert_int_not_equal(second, first);
  assert_true(dt_removed_image_repository_create(second, imgid));
  _remove(imgid);
  assert_true(dt_removed_image_repository_restore(second, imgid));

  assert_int_equal(dt_image_repository_find_by_film_and_filename(film, "a.raw"), imgid);
  assert_true(_has_flags(imgid, SEEDED_FLAGS));
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_round_trip_restores_every_table),
    cmocka_unit_test(test_group_leader_restores_membership),
    cmocka_unit_test(test_film_roll_comes_back_with_its_last_image),
    cmocka_unit_test(test_partial_undo_repoints_a_departed_leader),
    cmocka_unit_test(test_clear_drops_the_snapshot),
    cmocka_unit_test(test_successive_snapshots_do_not_collide),
  };
  return cmocka_run_group_tests(tests, testdb_setup, testdb_teardown);
}
