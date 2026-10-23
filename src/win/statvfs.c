/*
    This file is part of darktable,
    Copyright (C) 2016-2017 Peter Budai.
    Copyright (C) 2017 Tobias Ellinghaus.
    Copyright (C) 2020 Pascal Obry.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2026 Aurélien PIERRE.
    Copyright (C) 2026 Guillaume Stutin.
    
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

/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * statvfs emulation for Windows
 *
 * Copyright 2012 Gerald Richter
 * Copyright 2016 Inuvika Inc.
 * Copyright 2016 David PHAM-VAN <d.phamvan@inuvika.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "system/mem_alloc.h"
#include <glib.h>

#include <string.h>

//#include <winpr/crt.h>
#include "win/win.h"

#include "statvfs.h"

/* GetDiskFreeSpaceExW takes any directory, where GetDiskFreeSpaceW needs a drive root, which cannot
 * be built for a UNC share or a drive mounted as a folder. Its counts are in bytes, so they are
 * reported with f_frsize == 1: f_frsize * f_bavail is exact, and the cluster geometry is not known.
 * Returns 0 on success and -1 on failure, like POSIX.
 */
int statvfs(const char *path, struct statvfs *buf)
{
  if(IS_NULL_PTR(path) || IS_NULL_PTR(buf)) return -1;

  memset(buf, 0, sizeof(*buf));
  buf->f_namemax = 250;

  wchar_t *wpath = g_utf8_to_utf16(path, -1, NULL, NULL, NULL);
  if(IS_NULL_PTR(wpath)) return -1;

  ULARGE_INTEGER free_to_caller = { 0 };
  ULARGE_INTEGER total_bytes = { 0 };
  ULARGE_INTEGER total_free = { 0 };
  const BOOL res = GetDiskFreeSpaceExW(wpath, &free_to_caller, &total_bytes, &total_free);

  dt_free(wpath);

  if(!res) return -1;

  buf->f_bsize = 1;                                  /* file system block size */
  buf->f_frsize = 1;                                 /* fragment size: bytes, see above */
  buf->f_blocks = (fsblkcnt_t)total_bytes.QuadPart;  /* size of fs in f_frsize units */
  buf->f_bfree = (fsblkcnt_t)total_free.QuadPart;    /* # free blocks */
  buf->f_bavail = (fsblkcnt_t)free_to_caller.QuadPart; /* # free blocks for unprivileged users */

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

