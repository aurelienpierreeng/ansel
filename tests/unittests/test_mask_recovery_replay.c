/*
    This file is part of Ansel,
    Copyright (C) 2026 Paolo SANTUCCI.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "common/film.h"
#include "common/image.h"
#include "darktable.h"
#include "database/history_repository.h"
#include "develop/develop.h"
#include "develop/blend.h"
#include "develop/dev_history.h"
#include "develop/dev_pixelpipe.h"
#include "develop/masks.h"
#include "imageio/imageio_jpeg.h"
#include "system/mem_alloc.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <cmocka.h>
#include <glib/gstdio.h>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static char *configdir = NULL;
static char *cachedir = NULL;
static char *tmpdir = NULL;
static char *datadir = NULL;

static void remove_tree(const char *path)
{
  GDir *directory = g_dir_open(path, 0, NULL);
  if(IS_NULL_PTR(directory))
  {
    g_remove(path);
    return;
  }

  const char *name = NULL;
  while((name = g_dir_read_name(directory)))
  {
    char *child = g_build_filename(path, name, NULL);
    remove_tree(child);
    dt_free(child);
  }
  g_dir_close(directory);
  g_rmdir(path);
}

static char *prepare_datadir(const char *root)
{
  char *result = g_build_filename(root, "data", NULL);
  char *rawspeed = g_build_filename(result, "rawspeed", NULL);
  char *destination = g_build_filename(rawspeed, "cameras.xml", NULL);
  char *source = g_build_filename(ANSEL_TEST_SOURCE_DIR, "src", "external", "rawspeed", "data", "cameras.xml", NULL);
  assert_int_equal(g_mkdir(result, 0700), 0);
  assert_int_equal(g_mkdir(rawspeed, 0700), 0);
  GFile *source_file = g_file_new_for_path(source);
  GFile *destination_file = g_file_new_for_path(destination);
  assert_true(g_file_copy(source_file, destination_file, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL));
  g_object_unref(destination_file);
  g_object_unref(source_file);
  dt_free(source);
  dt_free(destination);
  dt_free(rawspeed);
  return result;
}

static dt_iop_module_t *module_for(dt_develop_t *dev, const char *operation)
{
  for(GList *node = dev->iop; !IS_NULL_PTR(node); node = g_list_next(node))
  {
    dt_iop_module_t *module = node->data;
    if(!strcmp(module->op, operation)) return module;
  }
  return NULL;
}

static dt_dev_history_item_t *history_for(dt_develop_t *dev, const char *operation)
{
  for(GList *node = dev->history; !IS_NULL_PTR(node); node = g_list_next(node))
  {
    dt_dev_history_item_t *history = node->data;
    if(!strcmp(history->op_name, operation)) return history;
  }
  return NULL;
}

static void write_history(const int32_t imgid, const int first_num, const dt_iop_module_t *exposure,
                           const dt_iop_module_t *consumer, const dt_iop_module_t *boundary)
{
  assert_true(dt_history_repository_delete_dev_history(imgid));
  dt_develop_blend_params_t exposure_blend = *exposure->default_blendop_params;
  dt_develop_blend_params_t consumer_blend = *consumer->default_blendop_params;
  exposure_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_SHAPE;
  exposure_blend.mask_id = 42;
  consumer_blend.mask_mode = DEVELOP_MASK_ENABLED | DEVELOP_MASK_RASTER;
  g_strlcpy(consumer_blend.raster_mask_source, exposure->op, sizeof(consumer_blend.raster_mask_source));
  assert_true(dt_history_repository_write_item(imgid, first_num, exposure->op, exposure->params, exposure->params_size,
                                                 exposure->version(), TRUE, &exposure_blend, sizeof(exposure_blend),
                                                 dt_develop_blend_version(), 0, ""));
  assert_true(dt_history_repository_write_item(imgid, first_num + 1, consumer->op, consumer->params, consumer->params_size,
                                                 consumer->version(), TRUE, &consumer_blend, sizeof(consumer_blend),
                                                 dt_develop_blend_version(),
                                                 0, ""));
  assert_true(dt_history_repository_write_item(imgid, first_num + 2, boundary->op, boundary->params, boundary->params_size,
                                                 boundary->version(), TRUE, boundary->default_blendop_params,
                                                 sizeof(*boundary->default_blendop_params), dt_develop_blend_version(), 0, ""));

  const dt_masks_node_circle_t circle = { .center = { 0.5f, 0.5f }, .radius = 0.2f, .border = 0.0f };
  const dt_masks_form_group_t group = { .formid = 41, .parentid = 42, .opacity = 1.0f };
  const float source[] = { 0.5f, 0.5f };
  assert_true(dt_history_repository_write_mask_item(imgid, first_num, 41, DT_MASKS_CIRCLE, "circle", dt_masks_version(),
                                                      &circle, sizeof(circle), 1, source, sizeof(source)));
  assert_true(dt_history_repository_write_mask_item(imgid, first_num, 42, DT_MASKS_GROUP, "group", dt_masks_version(),
                                                      &group, sizeof(group), 1, source, sizeof(source)));
  assert_true(dt_history_repository_write_mask_item(imgid, first_num + 2, 0, 0, "", 0, NULL, 0, 0, NULL, 0));
  assert_true(dt_history_repository_set_end(imgid, first_num + 2));
}

static void pop_history(dt_develop_t *dev, const int history_end)
{
  dt_pthread_rwlock_wrlock(&dev->history_mutex);
  dt_dev_set_history_end_ext(dev, history_end);
  dt_dev_pop_history_items_ext(dev);
  dt_pthread_rwlock_unlock(&dev->history_mutex);
  dt_dev_pixelpipe_set_changed(dev->pipe, DT_DEV_PIPE_REMOVE);
  dt_dev_pixelpipe_change(dev->pipe);
}

static void should_recover_masked_module_when_replaying_an_empty_snapshot(void **state G_GNUC_UNUSED)
{
  char *directory = g_dir_make_tmp("ansel-mask-recovery-XXXXXX", NULL);
  char *filename = g_build_filename(directory, "image.jpg", NULL);
  const uint8_t pixels[] = { 8, 8, 8, 255, 8, 8, 8, 255, 8, 8, 8, 255, 8, 8, 8, 255 };
  assert_int_equal(dt_imageio_jpeg_write(filename, pixels, 2, 2, 90, NULL, 0), 0);
  dt_film_t film;
  assert_true(dt_film_new(&film, directory) > 0);
  const int32_t imgid = dt_image_import(film.id, filename, FALSE);
  assert_true(imgid > 0);

  dt_develop_t dev;
  dt_dev_init(&dev, FALSE);
  dev.pipe = g_malloc0(sizeof(dt_dev_pixelpipe_t));
  assert_non_null(dev.pipe);
  assert_true(dt_dev_pixelpipe_init(dev.pipe, &dev));
  assert_int_equal(dt_dev_load_image(&dev, imgid), 0);
  dt_iop_module_t *exposure = module_for(&dev, "exposure");
  dt_iop_module_t *consumer = module_for(&dev, "colorbalancergb");
  dt_iop_module_t *boundary = module_for(&dev, "dither");
  assert_non_null(exposure);
  assert_non_null(consumer);
  assert_non_null(boundary);
  write_history(imgid, g_list_length(dev.history), exposure, consumer, boundary);

  dt_pthread_rwlock_wrlock(&dev.history_mutex);
  dt_dev_read_history_ext(&dev, imgid);
  dt_pthread_rwlock_unlock(&dev.history_mutex);
  dt_dev_history_item_t *exposure_history = history_for(&dev, "exposure");
  dt_dev_history_item_t *consumer_history = history_for(&dev, "colorbalancergb");
  assert_non_null(exposure_history);
  assert_non_null(consumer_history);
  const int valid_history_end = g_list_index(dev.history, consumer_history) + 1;
  const int empty_snapshot_history_end = valid_history_end + 1;
  pop_history(&dev, valid_history_end);
  assert_true(exposure->enabled);
  assert_false(exposure_history->mask_dependencies_invalid);
  const uint64_t valid_hash = exposure_history->effective_hash;
  assert_non_null(dt_dev_pixelpipe_get_module_piece(dev.pipe, exposure));
  assert_non_null(dt_dev_pixelpipe_get_module_piece(dev.pipe, consumer));

  pop_history(&dev, empty_snapshot_history_end);
  exposure_history = history_for(&dev, "exposure");
  assert_false(exposure->enabled);
  assert_true(exposure_history->enabled);
  assert_true(exposure_history->mask_dependencies_invalid);
  assert_true(exposure_history->effective_hash != valid_hash);
  assert_null(dt_dev_pixelpipe_get_module_piece(dev.pipe, exposure));
  assert_null(dt_dev_pixelpipe_get_module_piece(dev.pipe, consumer));

  pop_history(&dev, valid_history_end);
  exposure_history = history_for(&dev, "exposure");
  assert_true(exposure->enabled);
  assert_true(exposure_history->enabled);
  assert_false(exposure_history->mask_dependencies_invalid);
  assert_int_equal(exposure_history->effective_hash, valid_hash);
  assert_non_null(dt_dev_pixelpipe_get_module_piece(dev.pipe, exposure));

  dt_dev_write_history_ext(&dev, imgid);
  dt_pthread_rwlock_wrlock(&dev.history_mutex);
  dt_dev_read_history_ext(&dev, imgid);
  dt_pthread_rwlock_unlock(&dev.history_mutex);
  pop_history(&dev, valid_history_end);
  exposure_history = history_for(&dev, "exposure");
  assert_true(exposure->enabled);
  assert_true(exposure_history->enabled);
  assert_false(exposure_history->mask_dependencies_invalid);
  assert_int_equal(exposure_history->effective_hash, valid_hash);

  dt_dev_cleanup(&dev);
  g_remove(filename);
  dt_free(filename);
  remove_tree(directory);
  dt_free(directory);
}

int main(int argc, char *argv[])
{
  configdir = g_dir_make_tmp("ansel-mask-recovery-config-XXXXXX", NULL);
  cachedir = g_dir_make_tmp("ansel-mask-recovery-cache-XXXXXX", NULL);
  tmpdir = g_dir_make_tmp("ansel-mask-recovery-tmp-XXXXXX", NULL);
  assert_non_null(configdir);
  assert_non_null(cachedir);
  assert_non_null(tmpdir);
  datadir = prepare_datadir(tmpdir);
  char *noiseprofiles = g_build_filename(ANSEL_TEST_SOURCE_DIR, "data", "noiseprofiles.json", NULL);
  char *init_argv[] = { "ansel-test-mask-recovery-replay", "--library", ":memory:", "--datadir", datadir,
                        "--noiseprofiles", noiseprofiles, "--moduledir", ANSEL_TEST_BINARY_DIR "/src",
                        "--configdir", configdir, "--cachedir", cachedir, "--tmpdir", tmpdir,
                        "--disable-opencl", "-t", "1", NULL };
  assert_int_equal(dt_init(G_N_ELEMENTS(init_argv) - 1, init_argv, FALSE, FALSE), 0);
  dt_free(noiseprofiles);

  const struct CMUnitTest tests[] = { cmocka_unit_test(should_recover_masked_module_when_replaying_an_empty_snapshot) };
  const int result = cmocka_run_group_tests(tests, NULL, NULL);
  dt_cleanup();
  remove_tree(configdir);
  remove_tree(cachedir);
  remove_tree(tmpdir);
  dt_free(datadir);
  dt_free(tmpdir);
  dt_free(cachedir);
  dt_free(configdir);
  return result;
}
