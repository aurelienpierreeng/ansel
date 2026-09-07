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

/** Why anything writing `main.images.flags` behind the image cache has to keep the cache
 * in step.
 *
 * A cache entry holds its own dt_image_t, and releasing it writes that WHOLE struct back --
 * which is how a rating reaches the database (metadata/ratings.c). So a row updated straight
 * through the repository while a stale entry sits in the cache is not durable: the next
 * release of that entry overwrites it, at a moment nothing connects to the update.
 *
 * The XMP crawler is the caller this was found on (control/crawler.c). It takes the entry and
 * edits it when there is one, and writes the row only when there is not; these tests pin both
 * halves of that reasoning -- the hazard, and the sequence that avoids it.
 */

#include "testdb.h"

#include "caches/image_cache.h"
#include "common/conf.h"
#include "common/image.h"
#include "control/crawler.h"
#include "darktable.h"

#include <glib/gstdio.h>
#include <stdlib.h>  // calloc/free, used directly below

// two bits standing in for the crawl's own DT_IMAGE_HAS_TXT / DT_IMAGE_HAS_WAV
#define CRAWL_BIT_A (1 << 4)
#define CRAWL_BIT_B (1 << 5)
#define CRAWL_MASK  (CRAWL_BIT_A | CRAWL_BIT_B)
// a bit standing in for what the user owns and the crawl must never touch
#define USER_BIT    2048

static char *_rcfile = NULL;

static int cache_setup(void **state)
{
  const int rc = testdb_setup(state);
  if(rc) return rc;

  /* Populating a cache entry reads conf: dt_image_repository_load() derives the film-roll
   * name, and dt_image_film_roll_name() asks for `show_folder_levels'. Same fixture as
   * test_conf_value_lifetime.c -- dt_conf_init() writes through the global, so the global
   * must point at the instance being initialised before the call. */
  _rcfile = g_build_filename(g_get_tmp_dir(), "ansel_test_image_cache_flags.rc", NULL);
  g_remove(_rcfile);
  darktable.conf = (dt_conf_t *)calloc(1, sizeof(dt_conf_t));
  dt_conf_init(darktable.conf, _rcfile, NULL);

  dt_image_cache_init(FALSE);
  return 0;
}

static int cache_teardown(void **state)
{
  dt_image_cache_cleanup();

  dt_conf_cleanup(darktable.conf);
  free(darktable.conf);
  darktable.conf = NULL;
  g_remove(_rcfile);
  g_free(_rcfile);
  _rcfile = NULL;

  return testdb_teardown(state);
}

/** Make an image whose row carries @p flags, and leave nothing of it in the cache. */
static int32_t _image_with_flags(const char *folder, const char *name, const int flags)
{
  const int32_t film = testdb_make_film(folder);
  assert_true(film > 0);
  const int32_t img = testdb_make_image(film, name);
  assert_true(img > 0);
  /* An image imported through the application leads a group of its own;
   * dt_image_repository_insert_import() leaves group_id NULL, and the cache's write-through
   * rewrites the whole row -- which a NULL group_id fails as a constraint. */
  assert_true(dt_image_repository_set_group(img, img));
  assert_true(dt_image_repository_set_flags(img, flags));
  return img;
}

static gboolean _row_has(const int32_t imgid, const int flag)
{
  GList *one = g_list_append(NULL, GINT_TO_POINTER(imgid));
  GList *got = dt_image_repository_get_ids_with_flag_among(one, flag);
  const gboolean found = (g_list_length(got) == 1);
  g_list_free(got);
  g_list_free(one);
  return found;
}

/* The hazard itself: a row written behind a cached entry does not survive that entry's next
 * release. If this ever stops being true the crawler's care below is no longer needed -- but
 * until then, nothing may write flags straight to the row for a cached image. */
static void test_cached_entry_overwrites_a_row_written_behind_it(void **state)
{
  (void)state;
  const int32_t img = _image_with_flags("/testdb/behind", "behind.raw", USER_BIT);

  // the user looks at the image: it now has a cache entry, holding flags as the row had them
  dt_image_t *cached = dt_image_cache_get(img, 'r');
  assert_non_null(cached);
  assert_int_equal(cached->flags & CRAWL_MASK, 0);
  dt_image_cache_read_release(cached);

  // something writes the row straight through the repository, cache none the wiser
  assert_true(dt_image_repository_set_flags_masked(img, CRAWL_MASK, CRAWL_BIT_A));
  assert_true(_row_has(img, CRAWL_BIT_A));

  // the user rates the image: the entry is released, and writes its whole word back
  dt_image_t *rated = dt_image_cache_get(img, 'w');
  assert_non_null(rated);
  rated->flags |= 1; // one star
  dt_image_cache_write_release(rated, DT_IMAGE_CACHE_RELAXED);

  // the row update is gone, and nothing said so
  assert_false(_row_has(img, CRAWL_BIT_A));
}

/* The sequence control/crawler.c uses instead: edit the entry when there is one. The rating
 * that follows then carries the crawl's bits rather than reverting them. */
static void test_editing_the_entry_survives_the_next_rating(void **state)
{
  (void)state;
  const int32_t img = _image_with_flags("/testdb/entry", "entry.raw", USER_BIT);

  dt_image_t *cached = dt_image_cache_get(img, 'r');
  assert_non_null(cached);
  dt_image_cache_read_release(cached);

  // the crawl finds an entry, so it edits the entry rather than the row
  dt_image_t *edited = dt_image_cache_testget(img, 'w');
  assert_non_null(edited);
  edited->flags = (edited->flags & ~CRAWL_MASK) | CRAWL_BIT_A;
  dt_image_cache_write_release(edited, DT_IMAGE_CACHE_RELAXED);
  assert_true(_row_has(img, CRAWL_BIT_A));

  // the user rates it afterwards
  dt_image_t *rated = dt_image_cache_get(img, 'w');
  assert_non_null(rated);
  rated->flags |= 1;
  dt_image_cache_write_release(rated, DT_IMAGE_CACHE_RELAXED);

  // both survive: the crawl's bit and the bit the user owns
  assert_true(_row_has(img, CRAWL_BIT_A));
  assert_true(_row_has(img, USER_BIT));
}

/* testget() must not create an entry: a crawl over the whole library calls it once per image,
 * and an allocating answer would pull the whole library into a 50 MiB cache. */
static void test_testget_does_not_create_an_entry(void **state)
{
  (void)state;
  const int32_t img = _image_with_flags("/testdb/uncached", "uncached.raw", USER_BIT);

  assert_null(dt_image_cache_testget(img, 'w'));

  // so the crawl writes the row, and that write stands
  assert_true(dt_image_repository_set_flags_masked(img, CRAWL_MASK, CRAWL_BIT_B));
  assert_true(_row_has(img, CRAWL_BIT_B));
  assert_true(_row_has(img, USER_BIT));
}

/* The crawl itself, over a real directory, on an image the user has already looked at.
 *
 * This is the sequence the fix exists for: the crawl notices a companion .txt, and the entry
 * sitting in the cache must come away carrying that -- otherwise the next rating writes the
 * entry's stale word back over the row. */
static void test_crawl_keeps_a_cached_entry_in_step(void **state)
{
  (void)state;
  gchar *dir = g_dir_make_tmp("ansel_test_crawl_XXXXXX", NULL);
  assert_non_null(dir);
  gchar *raw = g_build_filename(dir, "shot.raw", NULL);
  gchar *txt = g_build_filename(dir, "shot.txt", NULL);
  /* Version 0 takes no `_NN' suffix, so the sidecar is `<name>.xmp'. It has to exist at all:
   * _crawl_image() gives up on an image with no sidecar before it ever looks for companion
   * files, which is the behaviour the per-file stat() version had too. */
  gchar *xmp = g_build_filename(dir, "shot.raw.xmp", NULL);
  assert_true(g_file_set_contents(raw, "", 0, NULL));
  assert_true(g_file_set_contents(txt, "note", 4, NULL));
  assert_true(g_file_set_contents(xmp, "<x/>", 4, NULL));

  const int32_t img = _image_with_flags(dir, "shot.raw", USER_BIT);
  assert_false(_row_has(img, DT_IMAGE_HAS_TXT));

  /* Date the row after the sidecar, so the crawl has nothing to report and the only thing it
   * does on this image is the companion-flags update this test is about. */
  assert_true(dt_image_repository_set_write_timestamp(img, (int64_t)g_get_real_time() / 1000000 + 86400));

  // the user has looked at this image, so it has a cache entry -- holding no HAS_TXT
  dt_image_t *looked = dt_image_cache_get(img, 'r');
  assert_non_null(looked);
  assert_int_equal(looked->flags & DT_IMAGE_HAS_TXT, 0);
  dt_image_cache_read_release(looked);

  // no XMP beside the raw, so the crawl reports nothing and only updates the companion flags
  GList *changed = dt_control_crawler_run();
  assert_null(changed);

  assert_true(_row_has(img, DT_IMAGE_HAS_TXT));

  // and the entry knows it too, which is the whole point: rating the image now keeps it
  dt_image_t *rated = dt_image_cache_get(img, 'w');
  assert_non_null(rated);
  assert_int_equal(rated->flags & DT_IMAGE_HAS_TXT, DT_IMAGE_HAS_TXT);
  rated->flags |= 1; // one star
  dt_image_cache_write_release(rated, DT_IMAGE_CACHE_RELAXED);

  assert_true(_row_has(img, DT_IMAGE_HAS_TXT));
  assert_true(_row_has(img, USER_BIT));

  g_remove(xmp);
  g_remove(txt);
  g_remove(raw);
  g_remove(dir);
  g_free(xmp);
  g_free(txt);
  g_free(raw);
  g_free(dir);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_cached_entry_overwrites_a_row_written_behind_it),
    cmocka_unit_test(test_editing_the_entry_survives_the_next_rating),
    cmocka_unit_test(test_testget_does_not_create_an_entry),
    cmocka_unit_test(test_crawl_keeps_a_cached_entry_in_step),
  };
  return cmocka_run_group_tests(tests, cache_setup, cache_teardown);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
