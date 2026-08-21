#pragma once

#include "develop/develop.h"
#include "develop/pixelpipe_hb.h"

typedef struct dt_test_develop_context_t
{
  dt_develop_t develop;
  dt_dev_pixelpipe_t pipe;
  char *config_directory;
  char *config_file;
  int32_t saved_num_openmp_threads;
  gboolean config_initialized;
  gboolean plugin_mutex_initialized;
  gboolean cache_initialized;
  gboolean points_initialized;
  gboolean develop_initialized;
  gboolean pipe_initialized;
} dt_test_develop_context_t;

int dt_test_develop_context_init(dt_test_develop_context_t *context);
void dt_test_develop_context_cleanup(dt_test_develop_context_t *context);
