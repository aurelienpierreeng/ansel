/*
 * This file is part of Ansel, Copyright (C) 2026 Paolo SANTUCCI.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include "control/jobs/import_jobs.h"

/** @brief Job-local numbering and reservation state; table lifetime belongs to the worker. */
typedef struct dt_import_sequence_t
{
  GHashTable *captures;
  GHashTable *destinations;
  int next;
} dt_import_sequence_t;

/**
 * @brief Assign a capture number, retaining it even when subsequent expansion fails.
 * @return The positive sequence assigned to the full source path without its extension.
 */
int dt_import_sequence_assign(dt_import_sequence_t *sequences, const char *filename);

/** @brief Borrowed inputs for one destination expansion. */
typedef struct dt_import_destination_t
{
  const char *filename;
  dt_image_t *image;
  dt_control_import_t *data;
  int capture_sequence;
} dt_import_destination_t;

/**
 * @brief Expand and reserve a destination, retrying a sibling collision exactly once.
 *
 * Reservations retain their first owner for the whole job. An unchanged or
 * already-reserved fallback reaches the filesystem conflict policy as-is.
 * Replaces data->target_dir; the worker owns the returned path and frees it.
 * @return Newly allocated destination, or NULL when expansion fails.
 */
char *dt_import_sequence_destination(dt_import_sequence_t *sequences, const dt_import_destination_t *request);
