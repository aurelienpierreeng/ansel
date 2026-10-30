/*
 * This file is part of Ansel, Copyright (C) 2026 Paolo SANTUCCI.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "control/jobs/import_sequence.h"
#include "common/utility.h"
#include "system/mem_alloc.h"

int dt_import_sequence_assign(dt_import_sequence_t *sequences, const char *filename)
{
  const char *extension = dt_util_path_get_extension(filename);
  const size_t length = IS_NULL_PTR(extension) ? strlen(filename) : (size_t)(extension - filename - 1);
  char *capture = g_strndup(filename, length);
  gpointer assigned = g_hash_table_lookup(sequences->captures, capture);
  if(IS_NULL_PTR(assigned))
  {
    assigned = GINT_TO_POINTER(++sequences->next);
    g_hash_table_insert(sequences->captures, capture, assigned);
  }
  else
    dt_free(capture);
  return GPOINTER_TO_INT(assigned);
}

char *dt_import_sequence_destination(dt_import_sequence_t *sequences, const dt_import_destination_t *request)
{
  dt_control_import_t *data = request->data;
  int destination_sequence = request->capture_sequence;
  dt_free(data->target_dir);
  data->target_dir = NULL;
  char *destination = dt_build_filename_from_pattern(request->filename, destination_sequence, request->image, data);
  if(IS_NULL_PTR(destination)) return NULL;

  const int reserved = GPOINTER_TO_INT(g_hash_table_lookup(sequences->destinations, destination));
  if(reserved == request->capture_sequence)
  {
    dt_free(destination);
    dt_free(data->target_dir);
    data->target_dir = NULL;
    destination_sequence = ++sequences->next;
    destination = dt_build_filename_from_pattern(request->filename, destination_sequence, request->image, data);
  }

  if(!IS_NULL_PTR(destination) && !g_hash_table_contains(sequences->destinations, destination))
    g_hash_table_insert(sequences->destinations, g_strdup(destination), GINT_TO_POINTER(destination_sequence));
  return destination;
}
