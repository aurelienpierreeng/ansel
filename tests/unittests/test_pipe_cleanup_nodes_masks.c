/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

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

/** The side-band masks of a graph go with its nodes.
 *
 * A pipe references the raster masks its graph published or reopened, and the detail mask, so
 * that the LRU cannot evict them between two renders. The darkroom's pipes live for the whole
 * session and leave() destroys their nodes only: the references used to be released by the next
 * render or by dt_dev_pixelpipe_cleanup(), so back in the lighttable the masks of the last image
 * stayed referenced, and no memory pressure could free them.
 *
 * Released, a mask can be evicted while the output of its provider survives. For the detail mask,
 * whose provider is the hidden detailmask stage, a cached output then stands for the stage only
 * while its mask is there too: dt_dev_pixelpipe_rawdetail_mask_check_hit().
 *
 * These tests drive a pipe with no node: the masks are lines the test publishes itself, held the
 * way the pipe holds them.
 */

#include "caches/pixelpipe_cache.h"
#include "develop/imageop.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_hb.h"
#include "system/atomic.h"

#include <stdarg.h>
#include <stddef.h>
// cmocka.h declares `extern jmp_buf global_expect_assert_env' at file scope without including
// <setjmp.h> itself. Same suppression as test_metadata_notify.c, and for the same reason.
#include <setjmp.h>  // NOLINT(misc-include-cleaner)
#include <stdint.h>
#include <string.h>
#include <cmocka.h>

#include <glib.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

#define LINE_BYTES (256 * 1024)
#define HASH_RASTER 0x0B0B0B0B0B0B0B0BULL
#define HASH_DETAIL 0x0C0C0C0C0C0C0C0CULL

static int _setup(void **state __attribute__((unused)))
{
  return dt_dev_pixelpipe_cache_init((size_t)64 * 1024 * 1024, FALSE, FALSE) ? 0 : -1;
}

static int _teardown(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_cache_cleanup();
  return 0;
}

/* A written mask, still referenced once by its creator: the reference the pipe takes over when a
 * provider publishes it. */
static dt_pixel_cache_entry_t *_publish(const uint64_t hash)
{
  void *data = NULL;
  dt_pixel_cache_entry_t *entry = NULL;
  assert_int_equal(dt_dev_pixelpipe_cache_get(hash, LINE_BYTES, "test mask", 0, TRUE, &data, &entry), 1);
  assert_non_null(entry);
  assert_non_null(data);
  dt_dev_pixelpipe_cache_wrlock_entry(FALSE, entry);
  return entry;
}

/* A pipe whose graph holds one raster mask and the detail mask, as after a render. */
static void _pipe_holding_masks(dt_dev_pixelpipe_t *pipe)
{
  memset(pipe, 0, sizeof(*pipe));
  pipe->raster_mask_hashes = g_array_new(FALSE, FALSE, sizeof(uint64_t));
  const uint64_t raster = HASH_RASTER;
  _publish(HASH_RASTER);
  g_array_append_val(pipe->raster_mask_hashes, raster);
  _publish(HASH_DETAIL);
  pipe->rawdetail_mask_hash = HASH_DETAIL;
}

/** Destroying the nodes releases what the graph referenced, and forgets it. */
static void _cleanup_nodes_releases_the_masks(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_t pipe;
  _pipe_holding_masks(&pipe);

  dt_dev_pixelpipe_cleanup_nodes(&pipe);

  assert_int_equal(dt_atomic_get_int(&dt_dev_pixelpipe_cache_get_entry(HASH_RASTER)->refcount), 0);
  assert_int_equal(dt_atomic_get_int(&dt_dev_pixelpipe_cache_get_entry(HASH_DETAIL)->refcount), 0);
  assert_int_equal(pipe.raster_mask_hashes->len, 0);
  assert_true(pipe.rawdetail_mask_hash == DT_PIXELPIPE_CACHE_HASH_INVALID);

  g_array_free(pipe.raster_mask_hashes, TRUE);
}

/** Released, the masks are lines like any other: the next flush frees them. */
static void _released_masks_go_with_the_flush(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_t pipe;
  _pipe_holding_masks(&pipe);

  dt_dev_pixelpipe_cleanup_nodes(&pipe);
  dt_dev_pixelpipe_cache_flush(-1);

  assert_null(dt_dev_pixelpipe_cache_get_entry(HASH_RASTER));
  assert_null(dt_dev_pixelpipe_cache_get_entry(HASH_DETAIL));

  g_array_free(pipe.raster_mask_hashes, TRUE);
}

#define HASH_OUTPUT 0x0D0D0D0D0D0D0D0DULL

/* A detailmask piece, and its cached output: a written line nobody holds, as after a render. */
static void _detailmask_output(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, const char *op)
{
  memset(module, 0, sizeof(*module));
  memset(piece, 0, sizeof(*piece));
  g_strlcpy(module->op, op, sizeof(module->op));
  piece->module = module;
  piece->global_hash = 0x0E0E0E0E0E0E0E0EULL;
  piece->roi_out.width = 16;
  piece->roi_out.height = 8;
  dt_dev_pixelpipe_cache_unref_entry(_publish(HASH_OUTPUT));
}

/** Its mask there, the output stands, and the pipe holds the mask from then on. */
static void _detail_output_with_its_mask_stands(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_t pipe;
  memset(&pipe, 0, sizeof(pipe));
  pipe.raster_mask_hashes = g_array_new(FALSE, FALSE, sizeof(uint64_t));
  pipe.rawdetail_mask_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  dt_iop_module_t module;
  dt_dev_pixelpipe_iop_t piece;
  _detailmask_output(&module, &piece, "detailmask");
  const uint64_t mask_hash = dt_dev_pixelpipe_rawdetail_mask_hash(&piece);
  dt_dev_pixelpipe_cache_unref_entry(_publish(mask_hash));

  assert_true(dt_dev_pixelpipe_rawdetail_mask_check_hit(&pipe, &piece, HASH_OUTPUT));
  assert_non_null(dt_dev_pixelpipe_cache_get_entry(HASH_OUTPUT));
  assert_true(pipe.rawdetail_mask_hash == mask_hash);
  assert_int_equal(dt_atomic_get_int(&dt_dev_pixelpipe_cache_get_entry(mask_hash)->refcount), 1);
  assert_int_equal(pipe.rawdetail_mask_roi.width, 16);

  dt_dev_pixelpipe_cleanup_nodes(&pipe);
  g_array_free(pipe.raster_mask_hashes, TRUE);
}

/** Its mask evicted, the output goes, so that the stage runs and publishes the mask again. */
static void _detail_output_without_its_mask_goes(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_t pipe;
  memset(&pipe, 0, sizeof(pipe));
  pipe.rawdetail_mask_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  dt_iop_module_t module;
  dt_dev_pixelpipe_iop_t piece;
  _detailmask_output(&module, &piece, "detailmask");

  assert_false(dt_dev_pixelpipe_rawdetail_mask_check_hit(&pipe, &piece, HASH_OUTPUT));
  assert_null(dt_dev_pixelpipe_cache_get_entry(HASH_OUTPUT));
  assert_true(pipe.rawdetail_mask_hash == DT_PIXELPIPE_CACHE_HASH_INVALID);
}

/** Any other stage's output stands as it is. */
static void _other_output_stands(void **state __attribute__((unused)))
{
  dt_dev_pixelpipe_t pipe;
  memset(&pipe, 0, sizeof(pipe));
  pipe.rawdetail_mask_hash = DT_PIXELPIPE_CACHE_HASH_INVALID;
  dt_iop_module_t module;
  dt_dev_pixelpipe_iop_t piece;
  _detailmask_output(&module, &piece, "exposure");

  assert_true(dt_dev_pixelpipe_rawdetail_mask_check_hit(&pipe, &piece, HASH_OUTPUT));
  assert_non_null(dt_dev_pixelpipe_cache_get_entry(HASH_OUTPUT));
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(_cleanup_nodes_releases_the_masks, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_released_masks_go_with_the_flush, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_detail_output_with_its_mask_stands, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_detail_output_without_its_mask_goes, _setup, _teardown),
    cmocka_unit_test_setup_teardown(_other_output_stands, _setup, _teardown),
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}
