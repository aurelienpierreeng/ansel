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

/** The backbuffer publication is one fact, and a consumer must read it as one.
 *
 * `hash' names a cacheline; `width'/`height' say what shape the pixels in it are. A consumer
 * that pairs a hash from one publication with dimensions from the next computes a cairo stride
 * from a width the data does not have and paints the diagonal striping of a stride error. The
 * cacheline cannot help: it records how many BYTES it holds and nothing about their layout, and
 * it is routinely LARGER than width x height x bpp (aligned allocation, pool reuse), so a size
 * check passes.
 *
 * The first test reproduces the cross-pairing against the fields read one at a time -- the way
 * every display path read them -- and fails the day it stops being reproducible, which is the
 * point: it is what says the second test is measuring something. The second asserts that a
 * snapshot never crosses. Both run the same writer.
 *
 * This is a race, so the first test is written to be overwhelmingly likely rather than certain:
 * a writer flipping between two shapes as fast as it can, against a reader that reads the hash,
 * does a little work, and then reads the dimensions -- the same shape as
 * "resolve the cacheline, then compute the stride". If it ever goes quiet on some machine,
 * do not delete it: it is reporting that the window closed on that scheduler, not that the
 * hazard is gone.
 */

#include "develop/develop.h"   // dt_dev_set_backbuf()
#include "develop/pixelpipe_hb.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_metadata_notify.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <cmocka.h>

#include <glib.h>

/* Two publications a clipping module's edit mode swaps between: the cropped frame, and the full
 * transformed one it neutralises the crop to show. Different shape, different cacheline. */
#define SHAPE_A_W 4032
#define SHAPE_A_H 3024
#define SHAPE_A_HASH 0x1111111111111111ull
#define SHAPE_B_W 5104
#define SHAPE_B_H 3880
#define SHAPE_B_HASH 0x2222222222222222ull

typedef struct publisher_t
{
  dt_backbuf_t backbuf;
  volatile gint stop;
  guint64 publications;
} publisher_t;

static gpointer _publisher_main(gpointer user_data)
{
  publisher_t *p = (publisher_t *)user_data;
  while(!g_atomic_int_get(&p->stop))
  {
    dt_dev_set_backbuf(&p->backbuf, SHAPE_A_W, SHAPE_A_H, 4, SHAPE_A_HASH, 1);
    dt_dev_set_backbuf(&p->backbuf, SHAPE_B_W, SHAPE_B_H, 4, SHAPE_B_HASH, 2);
    p->publications += 2;
  }
  return NULL;
}

/** @brief Is this (hash, width, height) triple one that was ever published together? */
static gboolean _pairing_is_coherent(const uint64_t hash, const size_t width, const size_t height)
{
  if(hash == SHAPE_A_HASH) return width == SHAPE_A_W && height == SHAPE_A_H;
  if(hash == SHAPE_B_HASH) return width == SHAPE_B_W && height == SHAPE_B_H;
  return TRUE;   // the seed value; not a publication either side authored
}

/* How long to hunt. Long enough that a miss means something on an idle machine, short enough
 * that the suite stays a suite. */
#define HUNT_MICROSECONDS 400000

static void test_field_by_field_read_crosses_publications(void **state)
{
  (void)state;
  publisher_t p = { { 0 } };
  dt_dev_set_backbuf(&p.backbuf, SHAPE_A_W, SHAPE_A_H, 4, SHAPE_A_HASH, 1);

  GThread *writer = g_thread_new("backbuf-publisher", _publisher_main, &p);
  assert_non_null(writer);

  gint64 crossed = 0;
  gint64 reads = 0;
  const gint64 deadline = g_get_monotonic_time() + HUNT_MICROSECONDS;
  while(g_get_monotonic_time() < deadline && crossed == 0)
  {
    /* The display path's shape: take the hash, resolve a cacheline from it, and only then ask
     * how wide the pixels are. The resolve is what makes the window wide enough to matter; a
     * compiler barrier stands in for it. */
    const uint64_t hash = dt_dev_backbuf_get_hash(&p.backbuf);
    __asm__ __volatile__("" ::: "memory");
    const size_t width = p.backbuf.width;
    const size_t height = p.backbuf.height;

    reads++;
    if(!_pairing_is_coherent(hash, width, height)) crossed++;
  }

  g_atomic_int_set(&p.stop, 1);
  g_thread_join(writer);

  print_message("field-by-field: %" G_GINT64_FORMAT " reads, %" G_GINT64_FORMAT " crossed pairings\n", reads,
                crossed);
  assert_true(crossed > 0);
}

static void test_snapshot_never_crosses_publications(void **state)
{
  (void)state;
  publisher_t p = { { 0 } };
  dt_dev_set_backbuf(&p.backbuf, SHAPE_A_W, SHAPE_A_H, 4, SHAPE_A_HASH, 1);

  GThread *writer = g_thread_new("backbuf-publisher", _publisher_main, &p);
  assert_non_null(writer);

  gint64 crossed = 0;
  gint64 reads = 0;
  const gint64 deadline = g_get_monotonic_time() + HUNT_MICROSECONDS;
  while(g_get_monotonic_time() < deadline)
  {
    const dt_backbuf_state_t published = dt_dev_backbuf_snapshot(&p.backbuf);
    reads++;
    if(!_pairing_is_coherent(published.hash, published.width, published.height)) crossed++;
  }

  g_atomic_int_set(&p.stop, 1);
  g_thread_join(writer);

  print_message("snapshot: %" G_GINT64_FORMAT " reads, %" G_GINT64_FORMAT " crossed pairings\n", reads, crossed);
  assert_int_equal(crossed, 0);
}

/** @brief A single-field setter must publish coherently too -- the histogram backbuffers are
 *  invalidated that way when a view leaves. */
static void test_single_field_setter_is_a_publication(void **state)
{
  (void)state;
  dt_backbuf_t backbuf = { 0 };
  dt_dev_set_backbuf(&backbuf, SHAPE_A_W, SHAPE_A_H, 4, SHAPE_A_HASH, 1);

  dt_dev_backbuf_set_hash(&backbuf, DT_PIXELPIPE_CACHE_HASH_INVALID);

  const dt_backbuf_state_t published = dt_dev_backbuf_snapshot(&backbuf);
  assert_int_equal(published.hash, DT_PIXELPIPE_CACHE_HASH_INVALID);
  assert_int_equal(published.width, SHAPE_A_W);
  assert_int_equal(published.height, SHAPE_A_H);
}

int main(void)
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_field_by_field_read_crosses_publications),
    cmocka_unit_test(test_snapshot_never_crosses_publications),
    cmocka_unit_test(test_single_field_setter_is_a_publication),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
