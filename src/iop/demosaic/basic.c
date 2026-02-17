/*
    This file is part of the Ansel project.
    Copyright (C) 2023, 2026 Aurélien PIERRE.
    
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

/* taken from dcraw and demosaic_ppg below */

static void lin_interpolate(float *out, const float *const in, const dt_iop_roi_t *const roi_out,
                            const dt_iop_roi_t *const roi_in, const uint32_t filters,
                            const uint8_t (*const xtrans)[6])
{
  const int colors = (filters == 9) ? 3 : 4;

// border interpolate
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(colors, filters, in, roi_in, roi_out, xtrans) \
  shared(out) \
  schedule(static)
#endif
  for(int row = 0; row < roi_out->height; row++)
    for(int col = 0; col < roi_out->width; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      uint8_t count[4] = { 0 };
      if(col == 1 && row >= 1 && row < roi_out->height - 1) col = roi_out->width - 1;
      // average all the adjoining pixels inside image by color
      for(int y = row - 1; y != row + 2; y++)
        for(int x = col - 1; x != col + 2; x++)
          if(y >= 0 && x >= 0 && y < roi_in->height && x < roi_in->width)
          {
            const int f = fcol(y + roi_in->y, x + roi_in->x, filters, xtrans);
            sum[f] += in[y * roi_in->width + x];
            count[f]++;
          }
      const int f = fcol(row + roi_in->y, col + roi_in->x, filters, xtrans);
      // for current cell, copy the current sensor's color data,
      // interpolate the other two colors from surrounding pixels of
      // their color
      for(int c = 0; c < colors; c++)
      {
        if(c != f && count[c] != 0)
          out[4 * (row * roi_out->width + col) + c] = sum[c] / count[c];
        else
          out[4 * (row * roi_out->width + col) + c] = in[row * roi_in->width + col];
      }
    }

  // build interpolation lookup table which for a given offset in the sensor
  // lists neighboring pixels from which to interpolate:
  // NUM_PIXELS                 # of neighboring pixels to read
  // for (1..NUM_PIXELS):
  //   OFFSET                   # in bytes from current pixel
  //   WEIGHT                   # how much weight to give this neighbor
  //   COLOR                    # sensor color
  // # weights of adjoining pixels not of this pixel's color
  // COLORA TOT_WEIGHT
  // COLORB TOT_WEIGHT
  // COLORPIX                   # color of center pixel

  int(*const lookup)[16][32] = malloc(sizeof(int) * 16 * 16 * 32);

  const int size = (filters == 9) ? 6 : 16;
  for(int row = 0; row < size; row++)
    for(int col = 0; col < size; col++)
    {
      int *ip = &(lookup[row][col][1]);
      int sum[4] = { 0 };
      const int f = fcol(row + roi_in->y, col + roi_in->x, filters, xtrans);
      // make list of adjoining pixel offsets by weight & color
      for(int y = -1; y <= 1; y++)
        for(int x = -1; x <= 1; x++)
        {
          const int weight = 1 << ((y == 0) + (x == 0));
          const int color = fcol(row + y + roi_in->y, col + x + roi_in->x, filters, xtrans);
          if(color == f) continue;
          *ip++ = (roi_in->width * y + x);
          *ip++ = weight;
          *ip++ = color;
          sum[color] += weight;
        }
      lookup[row][col][0] = (ip - &(lookup[row][col][0])) / 3; /* # of neighboring pixels found */
      for(int c = 0; c < colors; c++)
        if(c != f)
        {
          *ip++ = c;
          *ip++ = sum[c];
        }
      *ip = f;
    }

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(colors, in, lookup, roi_in, roi_out, size) \
  shared(out) \
  schedule(static)
#endif
  for(int row = 1; row < roi_out->height - 1; row++)
  {
    float *buf = out + 4 * roi_out->width * row + 4;
    const float *buf_in = in + roi_in->width * row + 1;
    for(int col = 1; col < roi_out->width - 1; col++)
    {
      dt_aligned_pixel_t sum = { 0.0f };
      int *ip = &(lookup[row % size][col % size][0]);
      // for each adjoining pixel not of this pixel's color, sum up its weighted values
      for(int i = *ip++; i--; ip += 3) sum[ip[2]] += buf_in[ip[0]] * ip[1];
      // for each interpolated color, load it into the pixel
      for(int i = colors; --i; ip += 2) buf[*ip] = sum[ip[0]] / ip[1];
      buf[*ip] = *buf_in;
      buf += 4;
      buf_in++;
    }
  }

  free(lookup);
}



#define SWAP(a, b)                                                                                           \
  {                                                                                                          \
    const float tmp = (b);                                                                                   \
    (b) = (a);                                                                                               \
    (a) = tmp;                                                                                               \
  }

#ifdef _OPENMP
  #pragma omp declare simd aligned(in, out)
#endif
static void pre_median_b(float *out, const float *const in, const dt_iop_roi_t *const roi, const uint32_t filters,
                         const int num_passes, const float threshold)
{
  dt_iop_image_copy_by_size(out, in, roi->width, roi->height, 1);

  // now green:
  const int lim[5] = { 0, 1, 2, 1, 0 };
  for(int pass = 0; pass < num_passes; pass++)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(filters, in, lim, roi, threshold) \
    shared(out) \
    schedule(static)
#endif
    for(int row = 3; row < roi->height - 3; row++)
    {
      float med[9];
      int col = 3;
      if(FC(row, col, filters) != 1 && FC(row, col, filters) != 3) col++;
      float *pixo = out + (size_t)roi->width * row + col;
      const float *pixi = in + (size_t)roi->width * row + col;
      for(; col < roi->width - 3; col += 2)
      {
        int cnt = 0;
        for(int k = 0, i = 0; i < 5; i++)
        {
          for(int j = -lim[i]; j <= lim[i]; j += 2)
          {
            if(fabsf(pixi[roi->width * (i - 2) + j] - pixi[0]) < threshold)
            {
              med[k++] = pixi[roi->width * (i - 2) + j];
              cnt++;
            }
            else
              med[k++] = 64.0f + pixi[roi->width * (i - 2) + j];
          }
        }
        for(int i = 0; i < 8; i++)
          for(int ii = i + 1; ii < 9; ii++)
            if(med[i] > med[ii]) SWAP(med[i], med[ii]);
        pixo[0] = (cnt == 1 ? med[4] - 64.0f : med[(cnt - 1) / 2]);
        // pixo[0] = med[(cnt-1)/2];
        pixo += 2;
        pixi += 2;
      }
    }
  }
}

static void pre_median(float *out, const float *const in, const dt_iop_roi_t *const roi, const uint32_t filters,
                       const int num_passes, const float threshold)
{
  pre_median_b(out, in, roi, filters, num_passes, threshold);
}

#define SWAPmed(I, J)                                                                                        \
  if(med[I] > med[J]) SWAP(med[I], med[J])

static void color_smoothing(float *out, const dt_iop_roi_t *const roi_out, const int num_passes)
{
  const int width4 = 4 * roi_out->width;

  for(int pass = 0; pass < num_passes; pass++)
  {
    for(int c = 0; c < 3; c += 2)
    {
      {
        float *outp = out;
        for(int j = 0; j < roi_out->height; j++)
          for(int i = 0; i < roi_out->width; i++, outp += 4) outp[3] = outp[c];
      }
#ifdef _OPENMP
#pragma omp parallel for default(none) \
      dt_omp_firstprivate(roi_out, width4) \
      shared(out, c) \
      schedule(static)
#endif
      for(int j = 1; j < roi_out->height - 1; j++)
      {
        float *outp = out + (size_t)4 * j * roi_out->width + 4;
        for(int i = 1; i < roi_out->width - 1; i++, outp += 4)
        {
          float med[9] = {
            outp[-width4 - 4 + 3] - outp[-width4 - 4 + 1], outp[-width4 + 0 + 3] - outp[-width4 + 0 + 1],
            outp[-width4 + 4 + 3] - outp[-width4 + 4 + 1], outp[-4 + 3] - outp[-4 + 1],
            outp[+0 + 3] - outp[+0 + 1], outp[+4 + 3] - outp[+4 + 1],
            outp[+width4 - 4 + 3] - outp[+width4 - 4 + 1], outp[+width4 + 0 + 3] - outp[+width4 + 0 + 1],
            outp[+width4 + 4 + 3] - outp[+width4 + 4 + 1],
          };
          /* optimal 9-element median search */
          SWAPmed(1, 2);
          SWAPmed(4, 5);
          SWAPmed(7, 8);
          SWAPmed(0, 1);
          SWAPmed(3, 4);
          SWAPmed(6, 7);
          SWAPmed(1, 2);
          SWAPmed(4, 5);
          SWAPmed(7, 8);
          SWAPmed(0, 3);
          SWAPmed(5, 8);
          SWAPmed(4, 7);
          SWAPmed(3, 6);
          SWAPmed(1, 4);
          SWAPmed(2, 5);
          SWAPmed(4, 7);
          SWAPmed(4, 2);
          SWAPmed(6, 4);
          SWAPmed(4, 2);
          outp[c] = fmaxf(med[4] + outp[1], 0.0f);
        }
      }
    }
  }
}
#undef SWAP

static void green_equilibration_lavg(float *out, const float *const in, const int width, const int height,
                                     const uint32_t filters, const int x, const int y, const float thr)
{
  const float maximum = 1.0f;

  int oj = 2, oi = 2;
  if(FC(oj + y, oi + x, filters) != 1) oj++;
  if(FC(oj + y, oi + x, filters) != 1) oi++;
  if(FC(oj + y, oi + x, filters) != 1) oj--;

  dt_iop_image_copy_by_size(out, in, width, height, 1);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(height, in, thr, width, maximum) \
  shared(out, oi, oj) \
  schedule(static) collapse(2)
#endif
  for(size_t j = oj; j < height - 2; j += 2)
  {
    for(size_t i = oi; i < width - 2; i += 2)
    {
      const float o1_1 = in[(j - 1) * width + i - 1];
      const float o1_2 = in[(j - 1) * width + i + 1];
      const float o1_3 = in[(j + 1) * width + i - 1];
      const float o1_4 = in[(j + 1) * width + i + 1];
      const float o2_1 = in[(j - 2) * width + i];
      const float o2_2 = in[(j + 2) * width + i];
      const float o2_3 = in[j * width + i - 2];
      const float o2_4 = in[j * width + i + 2];

      const float m1 = (o1_1 + o1_2 + o1_3 + o1_4) / 4.0f;
      const float m2 = (o2_1 + o2_2 + o2_3 + o2_4) / 4.0f;

      // prevent divide by zero and ...
      // guard against m1/m2 becoming too large (due to m2 being too small) which results in hot pixels
      // also m1 must be checked to be positive
      if((m2 > 0.0f) && (m1 > 0.0f) && (m1 / m2 < maximum * 2.0f))
      {
        const float c1 = (fabsf(o1_1 - o1_2) + fabsf(o1_1 - o1_3) + fabsf(o1_1 - o1_4) + fabsf(o1_2 - o1_3)
                          + fabsf(o1_3 - o1_4) + fabsf(o1_2 - o1_4)) / 6.0f;
        const float c2 = (fabsf(o2_1 - o2_2) + fabsf(o2_1 - o2_3) + fabsf(o2_1 - o2_4) + fabsf(o2_2 - o2_3)
                          + fabsf(o2_3 - o2_4) + fabsf(o2_2 - o2_4)) / 6.0f;
        if((in[j * width + i] < maximum * 0.95f) && (c1 < maximum * thr) && (c2 < maximum * thr))
        {
          out[j * width + i] = in[j * width + i] * m1 / m2;
        }
      }
    }
  }
}

static void green_equilibration_favg(float *out, const float *const in, const int width, const int height,
                                     const uint32_t filters, const int x, const int y)
{
  int oj = 0, oi = 0;
  // const float ratio_max = 1.1f;
  double sum1 = 0.0, sum2 = 0.0, gr_ratio;

  if((FC(oj + y, oi + x, filters) & 1) != 1) oi++;
  const int g2_offset = oi ? -1 : 1;
  dt_iop_image_copy_by_size(out, in, width, height, 1);
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(g2_offset, height, in, width) \
  reduction(+ : sum1, sum2) \
  shared(oi, oj) \
  schedule(static) collapse(2)
#endif
  for(size_t j = oj; j < (height - 1); j += 2)
  {
    for(size_t i = oi; i < (width - 1 - g2_offset); i += 2)
    {
      sum1 += in[j * width + i];
      sum2 += in[(j + 1) * width + i + g2_offset];
    }
  }

  if(sum1 > 0.0 && sum2 > 0.0)
    gr_ratio = sum2 / sum1;
  else
    return;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(g2_offset, height, in, width) \
  shared(out, oi, oj, gr_ratio) \
  schedule(static) collapse(2)
#endif
  for(int j = oj; j < (height - 1); j += 2)
  {
    for(int i = oi; i < (width - 1 - g2_offset); i += 2)
    {
      out[(size_t)j * width + i] = in[(size_t)j * width + i] * gr_ratio;
    }
  }
}

#ifdef HAVE_OPENCL

// color smoothing step by multiple passes of median filtering
static int color_smoothing_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                              cl_mem dev_out, const dt_iop_roi_t *const roi_out, const int passes)
{
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_out->width;
  const int height = roi_out->height;

  cl_int err = -999;

  cl_mem dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float) * 4);
  if(dev_tmp == NULL) goto error;

  dt_opencl_local_buffer_t locopt
    = (dt_opencl_local_buffer_t){ .xoffset = 2*1, .xfactor = 1, .yoffset = 2*1, .yfactor = 1,
                                  .cellsize = 4 * sizeof(float), .overhead = 0,
                                  .sizex = 1 << 8, .sizey = 1 << 8 };

  if(!dt_opencl_local_buffer_opt(devid, gd->kernel_color_smoothing, &locopt))
    goto error;

  // two buffer references for our ping-pong
  cl_mem dev_t1 = dev_out;
  cl_mem dev_t2 = dev_tmp;

  for(int pass = 0; pass < passes; pass++)
  {
    size_t sizes[] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[] = { locopt.sizex, locopt.sizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 0, sizeof(cl_mem), &dev_t1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 1, sizeof(cl_mem), &dev_t2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_color_smoothing, 4,
                               sizeof(float) * 4 * (locopt.sizex + 2) * (locopt.sizey + 2), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_color_smoothing, sizes, local);
    if(err != CL_SUCCESS) goto error;

    // swap dev_t1 and dev_t2
    cl_mem t = dev_t1;
    dev_t1 = dev_t2;
    dev_t2 = t;
  }

  // after last step we find final output in dev_t1.
  // let's see if this is in dev_tmp1 and needs to be copied to dev_out
  if(dev_t1 == dev_tmp)
  {
    // copy data from dev_tmp -> dev_out
    size_t origin[] = { 0, 0, 0 };
    size_t region[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, region);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(dev_tmp);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic_color_smoothing] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

static int green_equilibration_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in,
                                  cl_mem dev_out, const dt_iop_roi_t *const roi_in)
{
  dt_iop_demosaic_data_t *data = (dt_iop_demosaic_data_t *)piece->data;
  dt_iop_demosaic_global_data_t *gd = (dt_iop_demosaic_global_data_t *)self->global_data;

  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  cl_mem dev_tmp = NULL;
  cl_mem dev_m = NULL;
  cl_mem dev_r = NULL;
  cl_mem dev_in1 = NULL;
  cl_mem dev_out1 = NULL;
  cl_mem dev_in2 = NULL;
  cl_mem dev_out2 = NULL;
  float *sumsum = NULL;

  cl_int err = -999;

  if(data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    dev_tmp = dt_opencl_alloc_device(devid, width, height, sizeof(float));
    if(dev_tmp == NULL) goto error;
  }

  switch(data->green_eq)
  {
    case DT_IOP_GREEN_EQ_FULL:
      dev_in1 = dev_in;
      dev_out1 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_LOCAL:
      dev_in2 = dev_in;
      dev_out2 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_BOTH:
      dev_in1 = dev_in;
      dev_out1 = dev_tmp;
      dev_in2 = dev_tmp;
      dev_out2 = dev_out;
      break;
    case DT_IOP_GREEN_EQ_NO:
    default:
      goto error;
  }

  if(data->green_eq == DT_IOP_GREEN_EQ_FULL || data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    dt_opencl_local_buffer_t flocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = 2 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 4, .sizey = 1 << 4 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_favg_reduce_first, &flocopt))
      goto error;

    const size_t bwidth = ROUNDUP(width, flocopt.sizex);
    const size_t bheight = ROUNDUP(height, flocopt.sizey);

    const int bufsize = (bwidth / flocopt.sizex) * (bheight / flocopt.sizey);

    dev_m = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * bufsize);
    if(dev_m == NULL) goto error;

    size_t fsizes[3] = { bwidth, bheight, 1 };
    size_t flocal[3] = { flocopt.sizex, flocopt.sizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_first, 0, sizeof(cl_mem), &dev_in1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_first, 1, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_first, 2, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_first, 3, sizeof(cl_mem), &dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_first, 4, sizeof(uint32_t), (void *)&piece->pipe->dsc.filters);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_first, 5, sizeof(int), &roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_first, 6, sizeof(int), &roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_first, 7,
                             sizeof(float) * 2 * flocopt.sizex * flocopt.sizey, NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_favg_reduce_first, fsizes,
                                                 flocal);
    if(err != CL_SUCCESS) goto error;

    dt_opencl_local_buffer_t slocopt
      = (dt_opencl_local_buffer_t){ .xoffset = 0, .xfactor = 1, .yoffset = 0, .yfactor = 1,
                                    .cellsize = sizeof(float) * 2, .overhead = 0,
                                    .sizex = 1 << 16, .sizey = 1 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_favg_reduce_second, &slocopt))
      goto error;

    const int reducesize = MIN(REDUCESIZE, ROUNDUP(bufsize, slocopt.sizex) / slocopt.sizex);

    dev_r = dt_opencl_alloc_device_buffer(devid, sizeof(float) * 2 * reducesize);
    if(dev_r == NULL) goto error;

    size_t ssizes[3] = { (size_t)reducesize * slocopt.sizex, 1, 1 };
    size_t slocal[3] = { slocopt.sizex, 1, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_second, 0, sizeof(cl_mem), &dev_m);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_second, 1, sizeof(cl_mem), &dev_r);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_second, 2, sizeof(int), &bufsize);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_reduce_second, 3, sizeof(float) * 2 * slocopt.sizex, NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_favg_reduce_second, ssizes,
                                                 slocal);
    if(err != CL_SUCCESS) goto error;

    sumsum = dt_pixelpipe_cache_alloc_align_float((size_t)2 * reducesize, piece->pipe);
    if(sumsum == NULL) goto error;
    err = dt_opencl_read_buffer_from_device(devid, (void *)sumsum, dev_r, 0,
                                            sizeof(float) * 2 * reducesize, CL_TRUE);
    if(err != CL_SUCCESS) goto error;

    float sum1 = 0.0f, sum2 = 0.0f;
    for(int k = 0; k < reducesize; k++)
    {
      sum1 += sumsum[2 * k];
      sum2 += sumsum[2 * k + 1];
    }

    const float gr_ratio = (sum1 > 0.0f && sum2 > 0.0f) ? sum2 / sum1 : 1.0f;

    size_t asizes[3] = { ROUNDUPDWD(width, devid), ROUNDUPDHT(height, devid), 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_apply, 0, sizeof(cl_mem), &dev_in1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_apply, 1, sizeof(cl_mem), &dev_out1);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_apply, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_apply, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_apply, 4, sizeof(uint32_t), (void *)&piece->pipe->dsc.filters);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_apply, 5, sizeof(int), &roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_apply, 6, sizeof(int), &roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_favg_apply, 7, sizeof(float), &gr_ratio);
    err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_green_eq_favg_apply, asizes);
    if(err != CL_SUCCESS) goto error;
  }

  if(data->green_eq == DT_IOP_GREEN_EQ_LOCAL || data->green_eq == DT_IOP_GREEN_EQ_BOTH)
  {
    const dt_image_t *img = &self->dev->image_storage;
    const float threshold = 0.0001f * img->exif_iso;

    dt_opencl_local_buffer_t locopt
      = (dt_opencl_local_buffer_t){ .xoffset = 2*2, .xfactor = 1, .yoffset = 2*2, .yfactor = 1,
                                    .cellsize = 1 * sizeof(float), .overhead = 0,
                                    .sizex = 1 << 8, .sizey = 1 << 8 };

    if(!dt_opencl_local_buffer_opt(devid, gd->kernel_green_eq_lavg, &locopt))
      goto error;

    size_t sizes[3] = { ROUNDUP(width, locopt.sizex), ROUNDUP(height, locopt.sizey), 1 };
    size_t local[3] = { locopt.sizex, locopt.sizey, 1 };
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 0, sizeof(cl_mem), &dev_in2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 1, sizeof(cl_mem), &dev_out2);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 2, sizeof(int), &width);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 3, sizeof(int), &height);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 4, sizeof(uint32_t), (void *)&piece->pipe->dsc.filters);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 5, sizeof(int), &roi_in->x);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 6, sizeof(int), &roi_in->y);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 7, sizeof(float), (void *)&threshold);
    dt_opencl_set_kernel_arg(devid, gd->kernel_green_eq_lavg, 8,
                           sizeof(float) * (locopt.sizex + 4) * (locopt.sizey + 4), NULL);
    err = dt_opencl_enqueue_kernel_2d_with_local(devid, gd->kernel_green_eq_lavg, sizes, local);
    if(err != CL_SUCCESS) goto error;
  }

  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_pixelpipe_cache_free_align(sumsum);
  return TRUE;

error:
  dt_opencl_release_mem_object(dev_tmp);
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_pixelpipe_cache_free_align(sumsum);
  dt_print(DT_DEBUG_OPENCL, "[opencl_demosaic_green_equilibration] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}

#endif // HAVE_OPENCL

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
