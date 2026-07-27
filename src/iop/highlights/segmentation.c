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
   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

// Connected-component segmentation of the clipped regions (host, both paths). (implementation; see segmentation.h
// for the public API.)

#include "common/darktable.h"
#include "develop/imageop.h"
#include "develop/imageop_math.h"
#include "iop/highlights/segmentation.h"
#include <stdlib.h>
#include <string.h>

int _segment_clipped_regions(const uint8_t *const restrict maskb, const float *const restrict depth,
                             const int width, const int height, const float pad_factor, const int pad_min,
                             const int pad_max, _hl_region_t **regions_out)
{
  const size_t npix = (size_t)width * height;
  int *const restrict label = calloc(npix, sizeof(int));  // 0 = background / unvisited
  int *const restrict stack = malloc(npix * sizeof(int)); // flood-fill work stack
  if(!label || !stack)
  {
    free(label);
    free(stack);
    *regions_out = NULL;
    return 0;
  }

  int capacity = 64, count = 0;
  _hl_region_t *regions = malloc((size_t)capacity * sizeof(_hl_region_t));
  if(!regions)
  {
    free(label);
    free(stack);
    *regions_out = NULL;
    return 0;
  }

  for(size_t pixel_index = 0; pixel_index < npix; pixel_index++)
  {
    // seed on the REAL feather support: the 5x5 box mean's genuine values are >= 1/25, while
    // the CPU running-sum blur leaves ~1e-7 cancellation residue on millions of pixels whose
    // true value is zero -- seeding on > 0 made the region topology depend on float noise
    // (and differ between the CPU and OpenCL gathers, which compute exact zeros)
    if(label[pixel_index] || !maskb[pixel_index]) continue;
    int stack_top = 0;
    stack[stack_top++] = (int)pixel_index;
    label[pixel_index] = count + 1;
    // bounding box of the region, grown pixel by pixel as the flood fill visits them
    int x_min = (int)(pixel_index % (size_t)width);
    int x_max = x_min;
    int y_min = (int)(pixel_index / (size_t)width);
    int y_max = y_min;
    float rmax = depth[pixel_index]; // reconstruction radius = deepest clip-to-valid distance in the region
    while(stack_top > 0)
    {
      const int visited_index = stack[--stack_top];
      const int visited_x = visited_index % width;
      const int visited_y = visited_index / width;

      // grow the region's bounding box to include the visited pixel, and keep the deepest
      // clip-to-valid distance seen so far (it becomes the region's reconstruction radius)
      if(visited_x < x_min) x_min = visited_x;
      if(visited_x > x_max) x_max = visited_x;
      if(visited_y < y_min) y_min = visited_y;
      if(visited_y > y_max) y_max = visited_y;
      if(depth[visited_index] > rmax) rmax = depth[visited_index];

      // push every in-bounds, still-unlabelled clipped neighbour (8-connectivity) onto the
      // flood-fill stack, so connected clipped pixels end up in the same region
      for(int delta_y = -1; delta_y <= 1; delta_y++)
        for(int delta_x = -1; delta_x <= 1; delta_x++)
        {
          if(!delta_x && !delta_y) continue;

          const int neighbour_x = visited_x + delta_x;
          const int neighbour_y = visited_y + delta_y;
          if(neighbour_x < 0 || neighbour_y < 0 || neighbour_x >= width || neighbour_y >= height) continue;

          const size_t neighbour_index = (size_t)neighbour_y * width + neighbour_x;
          if(label[neighbour_index] || !maskb[neighbour_index]) continue;

          label[neighbour_index] = count + 1;
          stack[stack_top++] = (int)neighbour_index;
        }
    }
    if(count >= capacity)
    {
      capacity *= 2;
      _hl_region_t *const tmp = realloc(regions, (size_t)capacity * sizeof(_hl_region_t));
      if(!tmp)
      {
        free(regions);
        free(label);
        free(stack);
        *regions_out = NULL;
        return 0;
      }
      regions = tmp;
    }
    // radius = deepest clip-to-valid distance (distance transform), padded by pad_factor of it
    const int pad = CLAMP((int)(pad_factor * rmax + 0.5f), pad_min, pad_max); // pad = clamp(ceil(pad_factor * R))
    regions[count].x0 = x_min; // clipped-pixel bbox (accumulated over the flood fill)
    regions[count].y0 = y_min;
    regions[count].x1 = x_max;
    regions[count].y1 = y_max;
    regions[count].pad = pad;
    regions[count].radius = rmax; // reconstruction radius R = max_{x in Omega} delta(x)
    regions[count].rx0 = MAX(x_min - pad, 0);
    regions[count].ry0 = MAX(y_min - pad, 0);
    regions[count].rx1 = MIN(x_max + pad, width - 1);
    regions[count].ry1 = MIN(y_max + pad, height - 1);
    count++;
  }
  free(label);
  free(stack);

  // Merge regions whose padded READ boxes overlap. Such regions share reconstruction context, so
  // processing them separately is redundant and leaves a seam where their fills meet (each sees the
  // other only as unreconstructed clip values). Union-find on padded-box intersection, then rebuild
  // one region per group: union of the member CLIPPED bboxes, re-padded from the merged extent.
  if(count > 1)
  {
    int *const restrict parent = malloc((size_t)count * sizeof(int));
    _hl_region_t *const restrict merged = malloc((size_t)count * sizeof(_hl_region_t));
    int *const restrict map = malloc((size_t)count * sizeof(int));
    if(!parent || !merged || !map)
    {
      free(parent);
      free(merged);
      free(map);
      *regions_out = regions;
      return count;
    }
    for(int i = 0; i < count; i++) parent[i] = i;

    // union every pair whose padded read boxes intersect (they share reconstruction context)
    for(int i = 0; i < count; i++)
    {
      for(int j = i + 1; j < count; j++)
      {
        // skip disjoint padded boxes
        if(regions[i].rx0 > regions[j].rx1 || regions[j].rx0 > regions[i].rx1) continue;
        if(regions[i].ry0 > regions[j].ry1 || regions[j].ry0 > regions[i].ry1) continue;

        // find the root of i (path halving)
        int root_i = i;
        while(parent[root_i] != root_i)
        {
          parent[root_i] = parent[parent[root_i]];
          root_i = parent[root_i];
        }

        // find the root of j (path halving)
        int root_j = j;
        while(parent[root_j] != root_j)
        {
          parent[root_j] = parent[parent[root_j]];
          root_j = parent[root_j];
        }

        // link the two components
        if(root_i != root_j) parent[root_j] = root_i;
      }
    }

    for(int i = 0; i < count; i++) map[i] = -1;

    // fold each component into its root: union the clipped bboxes, keep the group's MAX padding
    int mcount = 0;
    for(int i = 0; i < count; i++)
    {
      // find the root (path halving)
      int root_i = i;
      while(parent[root_i] != root_i)
      {
        parent[root_i] = parent[parent[root_i]];
        root_i = parent[root_i];
      }

      if(map[root_i] < 0)
      {
        // first member of this group: seed the merged region with it
        map[root_i] = mcount;
        merged[mcount] = regions[i];
        mcount++;
      }
      else
      {
        // grow the group's bbox and keep the largest reconstruction radius in the group, so the
        // smaller holes inherit enough context (per the merge rule)
        _hl_region_t *const merged_region = &merged[map[root_i]];
        merged_region->x0 = MIN(merged_region->x0, regions[i].x0);
        merged_region->y0 = MIN(merged_region->y0, regions[i].y0);
        merged_region->x1 = MAX(merged_region->x1, regions[i].x1);
        merged_region->y1 = MAX(merged_region->y1, regions[i].y1);
        merged_region->pad = MAX(merged_region->pad, regions[i].pad);
        merged_region->radius = fmaxf(merged_region->radius, regions[i].radius);
      }
    }

    // pad every merged region by the group's largest radius, clamped to the image
    for(int merged_region = 0; merged_region < mcount; merged_region++)
    {
      const int pad = merged[merged_region].pad;
      merged[merged_region].rx0 = MAX(merged[merged_region].x0 - pad, 0);
      merged[merged_region].ry0 = MAX(merged[merged_region].y0 - pad, 0);
      merged[merged_region].rx1 = MIN(merged[merged_region].x1 + pad, width - 1);
      merged[merged_region].ry1 = MIN(merged[merged_region].y1 + pad, height - 1);
    }
    free(parent);
    free(map);
    free(regions);
    *regions_out = merged;
    return mcount;
  }

  *regions_out = regions;
  return count;
}
