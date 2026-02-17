/*
    This file is part of darktable,
    Copyright (C) 2014 Moritz Lipp.
    Copyright (C) 2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2022 Martin Bařinka.
    
    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
    
    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <glib.h>

typedef struct backend_libsecret_context_t
{
  int placeholder; // we have to allocate one of these to signal that init didn't fail
} backend_libsecret_context_t;

/**
 * Initializes a new libsecret backend context.
 *
 * @return The libsecret context
 */
const backend_libsecret_context_t *dt_pwstorage_libsecret_new();

/**
 * Destroys the libsecret backend context.
 *
 * @param context The libsecret context
 */
void dt_pwstorage_libsecret_destroy(const backend_libsecret_context_t *context);

/**
 * Store (key,value) pairs.
 *
 * @param context The libsecret context
 * @param slot The name of the slot
 * @param attributes List of (key,value) pairs
 *
 * @return TRUE If function succeeded, otherwise FALSE
 */
gboolean dt_pwstorage_libsecret_set(const backend_libsecret_context_t *context, const gchar *slot,
                                    GHashTable *attributes);

/**
 * Loads (key, value) pairs
 *
 * @param context The libsecret context
 * @param slot The name of the slot
 *
 * @return table List of (key,value) pairs
 */
GHashTable *dt_pwstorage_libsecret_get(const backend_libsecret_context_t *context, const gchar *slot);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

