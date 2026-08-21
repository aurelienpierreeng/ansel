#include "test_develop_context.h"

#include "common/conf.h"
#include "common/points.h"
#include "darktable.h"
#include "develop/pixelpipe_cache.h"

int dt_test_develop_context_init(dt_test_develop_context_t *const context)
{
  if(!IS_NULL_PTR(darktable.conf) || !IS_NULL_PTR(darktable.pixelpipe_cache)
     || !IS_NULL_PTR(darktable.points) || !IS_NULL_PTR(darktable.develop)) return 1;

  context->saved_num_openmp_threads = darktable.num_openmp_threads;
  if(dt_pthread_mutex_init(&darktable.plugin_threadsafe, NULL)) return 1;
  context->plugin_mutex_initialized = TRUE;

  context->config_directory = g_dir_make_tmp("ansel-test-XXXXXX", NULL);
  if(IS_NULL_PTR(context->config_directory))
  {
    dt_test_develop_context_cleanup(context);
    return 1;
  }

  context->config_file = g_build_filename(context->config_directory, "anselrc", NULL);
  if(IS_NULL_PTR(context->config_file))
  {
    dt_test_develop_context_cleanup(context);
    return 1;
  }

  darktable.conf = static_cast<dt_conf_t *>(calloc(1, sizeof(dt_conf_t)));
  if(IS_NULL_PTR(darktable.conf))
  {
    dt_test_develop_context_cleanup(context);
    return 1;
  }
  dt_conf_init(darktable.conf, context->config_file, NULL);
  context->config_initialized = TRUE;
  dt_conf_set_string("plugins/lighttable/export/pixel_interpolator_warp", "mitchell");
  darktable.num_openmp_threads = 1;

  darktable.pixelpipe_cache = dt_dev_pixelpipe_cache_init((size_t)64 * 1024 * 1024);
  if(IS_NULL_PTR(darktable.pixelpipe_cache))
  {
    dt_test_develop_context_cleanup(context);
    return 1;
  }
  context->cache_initialized = TRUE;

  darktable.points = static_cast<dt_points_t *>(calloc(1, sizeof(dt_points_t)));
  if(IS_NULL_PTR(darktable.points))
  {
    dt_test_develop_context_cleanup(context);
    return 1;
  }
  dt_points_init(darktable.points, darktable.num_openmp_threads);
  if(darktable.points->num != static_cast<unsigned int>(darktable.num_openmp_threads))
  {
    dt_free(darktable.points);
    darktable.points = NULL;
    dt_test_develop_context_cleanup(context);
    return 1;
  }
  context->points_initialized = TRUE;

  dt_dev_init(&context->develop, FALSE);
  context->develop_initialized = TRUE;

  if(!dt_dev_pixelpipe_init(&context->pipe, &context->develop))
  {
    dt_test_develop_context_cleanup(context);
    return 1;
  }
  context->pipe_initialized = TRUE;
  return 0;
}

void dt_test_develop_context_cleanup(dt_test_develop_context_t *const context)
{
  if(context->pipe_initialized)
  {
    dt_dev_pixelpipe_cleanup(&context->pipe);
    context->pipe_initialized = FALSE;
  }
  if(context->develop_initialized)
  {
    dt_dev_cleanup(&context->develop);
    context->develop_initialized = FALSE;
  }
  if(context->points_initialized)
  {
    dt_points_cleanup(darktable.points);
    dt_free(darktable.points);
    darktable.points = NULL;
    context->points_initialized = FALSE;
  }
  if(context->cache_initialized)
  {
    dt_dev_pixelpipe_cache_cleanup(darktable.pixelpipe_cache);
    dt_free(darktable.pixelpipe_cache);
    darktable.pixelpipe_cache = NULL;
    context->cache_initialized = FALSE;
  }
  if(context->config_initialized)
  {
    dt_conf_cleanup(darktable.conf);
    dt_free(darktable.conf);
    darktable.conf = NULL;
    context->config_initialized = FALSE;
  }
  if(context->plugin_mutex_initialized)
  {
    dt_pthread_mutex_destroy(&darktable.plugin_threadsafe);
    darktable.num_openmp_threads = context->saved_num_openmp_threads;
    context->plugin_mutex_initialized = FALSE;
  }
  if(!IS_NULL_PTR(context->config_file))
  {
    g_remove(context->config_file);
    g_free(context->config_file);
    context->config_file = NULL;
  }
  if(!IS_NULL_PTR(context->config_directory))
  {
    g_rmdir(context->config_directory);
    g_free(context->config_directory);
    context->config_directory = NULL;
  }
}
