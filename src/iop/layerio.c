/*
 * drawlayer sidecar TIFF subsystem
 *
 * This file is intentionally included from drawlayer.c. It centralizes the
 * sidecar filename, directory scanning, page rewrite, and layer-name handling
 * so drawlayer's higher-level code can express lifecycle rules without
 * duplicating TIFF details.
 */

#include <tiffio.h>

static void _layerio_append_error(GString *errors, const char *message)
{
  if(!errors || !message || message[0] == '\0') return;
  if(errors->len > 0) g_string_append(errors, "; ");
  g_string_append(errors, message);
}

static void _layerio_log_errors(GString *errors)
{
  if(!errors) return;
  if(errors->len > 0) dt_control_log("%s", errors->str);
}

static void _sanitize_requested_layer_name(dt_iop_module_t *self, const char *requested, char *name,
                                           const size_t name_size)
{
  if(!name || name_size == 0) return;

  name[0] = '\0';
  if(requested && requested[0]) g_strlcpy(name, requested, name_size);
  g_strstrip(name);

  if(name[0] == '\0') _default_layer_name(self, name, name_size);
}

static gboolean _icc_blob_from_profile_key(const char *work_profile, uint8_t **icc_data, uint32_t *icc_len)
{
  if(icc_data) *icc_data = NULL;
  if(icc_len) *icc_len = 0;
  if(!work_profile || work_profile[0] == '\0' || !icc_data || !icc_len) return FALSE;

  const char *sep0 = strchr(work_profile, '|');
  if(!sep0) return FALSE;
  const char *sep1 = strchr(sep0 + 1, '|');
  if(!sep1) return FALSE;

  char *endptr = NULL;
  const long type_long = strtol(work_profile, &endptr, 10);
  if(endptr != sep0) return FALSE;

  const dt_colorspaces_color_profile_type_t type = (dt_colorspaces_color_profile_type_t)type_long;
  const char *filename = sep1 + 1;
  const dt_colorspaces_color_profile_t *profile = dt_colorspaces_get_profile(type, filename, DT_PROFILE_DIRECTION_ANY);
  if(!profile || !profile->profile) return FALSE;

  cmsUInt32Number len = 0;
  cmsSaveProfileToMem(profile->profile, NULL, &len);
  if(len == 0) return FALSE;

  uint8_t *buf = g_malloc(len);
  if(!buf) return FALSE;

  if(!cmsSaveProfileToMem(profile->profile, buf, &len) || len == 0)
  {
    g_free(buf);
    return FALSE;
  }

  *icc_data = buf;
  *icc_len = (uint32_t)len;
  return TRUE;
}

static gboolean _get_sidecar_path(const int32_t imgid, char *path, const size_t path_size)
{
  /* The sidecar is stored next to the source image, using the source filename verbatim
   * plus the fixed `.ansel.tiff` suffix (`image.ext.ansel.tiff`). */
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, path, path_size, &from_cache, __FUNCTION__);
  if(path[0] == '\0') return FALSE;
  g_strlcat(path, ".ansel.tiff", path_size);
  return TRUE;
}

static gboolean _set_directory_tags(TIFF *tiff, const uint32_t width, const uint32_t height, const char *name,
                                    const char *work_profile)
{
  /* Every page is written as RGBA half-float with associated alpha.
   * The current working-profile key is stored in IMAGEDESCRIPTION so reopening can warn
   * when the sidecar and current pipeline are no longer in the same space. */
  const uint16_t extrasamples[] = { EXTRASAMPLE_ASSOCALPHA };

  if(!TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 16)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 4)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB)) return FALSE;
  if(!TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, extrasamples)) return FALSE;

  /* Keep sidecar compression conservative and broadly interoperable.
   * What broke previously was not lossless compression itself, but the extra codec
   * tuning. In particular, the floating-point predictor is standard on paper but still
   * poorly supported in external readers for half-float RGBA pages: those files reopen
   * garbled even though libtiff accepts the tag combination. So do not emit a predictor
   * at all here. The safest practical options remain plain standard codecs only:
   *   1. Adobe Deflate
   *   2. legacy Deflate tag
   *   3. LZW
   *   4. uncompressed fallback */
  if(!TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE)
     && !TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_DEFLATE)
     && !TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_LZW))
    TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);

  TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff, 0));
  if(name && name[0]) TIFFSetField(tiff, TIFFTAG_PAGENAME, name);
  if(work_profile && work_profile[0]) TIFFSetField(tiff, TIFFTAG_IMAGEDESCRIPTION, work_profile);
  return TRUE;
}

static gboolean _read_scanline_rgba(TIFF *tiff, const uint32_t width, const uint32_t row, uint16_t *out)
{
  /* Read exactly one row as raw half-float RGBA.
   * We intentionally reject any page that is not already 16-bit IEEE float RGBA instead
   * of trying to convert formats here. */
  uint16_t bpp = 0;
  uint16_t spp = 0;
  uint16_t sampleformat = SAMPLEFORMAT_UINT;

  TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bpp);
  TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleformat);

  if(spp != 4) return FALSE;

  const tsize_t scanline = TIFFScanlineSize(tiff);
  tdata_t buffer = _TIFFmalloc((tsize_t)scanline);
  if(!buffer) return FALSE;

  const int ok = TIFFReadScanline(tiff, buffer, row, 0);
  if(ok == -1)
  {
    _TIFFfree(buffer);
    return FALSE;
  }

  if(bpp == 16 && sampleformat == SAMPLEFORMAT_IEEEFP)
    memcpy(out, buffer, (size_t)width * 4 * sizeof(uint16_t));
  else
  {
    _TIFFfree(buffer);
    return FALSE;
  }

  _TIFFfree(buffer);
  return TRUE;
}

static gboolean _write_scanline_rgba(TIFF *tiff, const uint32_t width, const uint32_t row, const uint16_t *in)
{
  return TIFFWriteScanline(tiff, (tdata_t)in, row, 0) != -1;
}

static void _scan_directories(TIFF *tiff, const char *target_name, const int target_order, drawlayer_dir_info_t *info)
{
  /* Find the page referenced by the module parameters.
   * Matching rules:
   * - when both are available, prefer exact (name + order) matches,
   * - otherwise prefer page name when provided (stable identity across reordering),
   * - fall back to page index when name lookup fails. */
  memset(info, 0, sizeof(*info));
  info->index = -1;

  if(!tiff) return;
  if(!TIFFSetDirectory(tiff, 0)) return;

  const gboolean has_target_name = (target_name && target_name[0] != '\0');
  const gboolean has_target_order = (target_order >= 0);
  gboolean exact_found = FALSE;
  gboolean named_found = FALSE;
  gboolean order_found = FALSE;
  drawlayer_dir_info_t exact = { 0 };
  drawlayer_dir_info_t named = { 0 };
  drawlayer_dir_info_t ordered = { 0 };
  exact.index = -1;
  named.index = -1;
  ordered.index = -1;

  int index = 0;
  do
  {
    char *page_name = NULL;
    char *page_profile = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height);
    TIFFGetField(tiff, TIFFTAG_PAGENAME, &page_name);
    TIFFGetField(tiff, TIFFTAG_IMAGEDESCRIPTION, &page_profile);

    const gboolean order_match = (has_target_order && index == target_order);
    const gboolean name_match = (has_target_name && !g_strcmp0(page_name ? page_name : "", target_name));
    const gboolean exact_match = (name_match && order_match);

    if(exact_match && !exact_found)
    {
      exact_found = TRUE;
      exact.found = TRUE;
      exact.index = index;
      exact.width = width;
      exact.height = height;
      g_strlcpy(exact.name, page_name ? page_name : "", sizeof(exact.name));
      g_strlcpy(exact.work_profile, page_profile ? page_profile : "", sizeof(exact.work_profile));
    }

    if(name_match && !named_found)
    {
      named_found = TRUE;
      named.found = TRUE;
      named.index = index;
      named.width = width;
      named.height = height;
      g_strlcpy(named.name, page_name ? page_name : "", sizeof(named.name));
      g_strlcpy(named.work_profile, page_profile ? page_profile : "", sizeof(named.work_profile));
    }

    if(order_match && !order_found)
    {
      order_found = TRUE;
      ordered.found = TRUE;
      ordered.index = index;
      ordered.width = width;
      ordered.height = height;
      g_strlcpy(ordered.name, page_name ? page_name : "", sizeof(ordered.name));
      g_strlcpy(ordered.work_profile, page_profile ? page_profile : "", sizeof(ordered.work_profile));
    }

    index++;
  } while(TIFFReadDirectory(tiff));

  if(exact_found)
  {
    info->found = TRUE;
    info->index = exact.index;
    info->width = exact.width;
    info->height = exact.height;
    g_strlcpy(info->name, exact.name, sizeof(info->name));
    g_strlcpy(info->work_profile, exact.work_profile, sizeof(info->work_profile));
  }
  else if(named_found)
  {
    info->found = TRUE;
    info->index = named.index;
    info->width = named.width;
    info->height = named.height;
    g_strlcpy(info->name, named.name, sizeof(info->name));
    g_strlcpy(info->work_profile, named.work_profile, sizeof(info->work_profile));
  }
  else if(order_found)
  {
    info->found = TRUE;
    info->index = ordered.index;
    info->width = ordered.width;
    info->height = ordered.height;
    g_strlcpy(info->name, ordered.name, sizeof(info->name));
    g_strlcpy(info->work_profile, ordered.work_profile, sizeof(info->work_profile));
  }

  info->count = index;
  TIFFSetDirectory(tiff, 0);
}

static gboolean _read_region(const char *path, const char *target_name, const int target_order, const int raw_width,
                             const int raw_height, drawlayer_patch_t *patch)
{
  /* Read only the requested patch from the selected TIFF page into an already allocated
   * float buffer. Missing files/pages are treated as an empty transparent layer. */
  _clear_transparent_float(patch->pixels, (size_t)patch->width * patch->height);

  if(!g_file_test(path, G_FILE_TEST_EXISTS)) return TRUE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;

  drawlayer_dir_info_t info;
  _scan_directories(tiff, target_name, target_order, &info);
  if(!info.found)
  {
    TIFFClose(tiff);
    return TRUE;
  }

  if(!TIFFSetDirectory(tiff, (tdir_t)info.index))
  {
    TIFFClose(tiff);
    return FALSE;
  }

  uint16_t *row = g_malloc_n((gsize)info.width * 4, sizeof(uint16_t));
  if(!row)
  {
    TIFFClose(tiff);
    return FALSE;
  }

  const int offset_x = (raw_width - (int)info.width) / 2;
  const int offset_y = (raw_height - (int)info.height) / 2;

  for(int py = 0; py < patch->height; py++)
  {
    const int raw_y = patch->y + py;
    const int src_y = raw_y - offset_y;
    if(src_y < 0 || src_y >= (int)info.height) continue;
    if(!_read_scanline_rgba(tiff, info.width, (uint32_t)src_y, row))
    {
      g_free(row);
      TIFFClose(tiff);
      return FALSE;
    }

    for(int px = 0; px < patch->width; px++)
    {
      const int raw_x = patch->x + px;
      const int src_x = raw_x - offset_x;
      if(src_x < 0 || src_x >= (int)info.width) continue;
      float *dst = patch->pixels + 4 * ((size_t)py * patch->width + px);
      _load_half_pixel_rgba(row + 4 * src_x, dst);
    }
  }

  g_free(row);
  TIFFClose(tiff);
  return TRUE;
}

static gboolean _write_page(TIFF *dst, TIFF *src, const int src_dir, const char *name, const char *work_profile,
                            const drawlayer_patch_t *patch, const int raw_width, const int raw_height)
{
  uint32_t width = (uint32_t)raw_width;
  uint32_t height = (uint32_t)raw_height;
  const char *page_profile = work_profile;
  uint8_t *icc_profile = NULL;
  uint32_t icc_profile_len = 0;

  if(src)
  {
    if(!TIFFSetDirectory(src, (tdir_t)src_dir)) return FALSE;
    TIFFGetField(src, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(src, TIFFTAG_IMAGELENGTH, &height);
    if(!name)
    {
      char *src_name = NULL;
      TIFFGetField(src, TIFFTAG_PAGENAME, &src_name);
      name = src_name;
    }
    if(!page_profile)
    {
      char *src_profile = NULL;
      TIFFGetField(src, TIFFTAG_IMAGEDESCRIPTION, &src_profile);
      page_profile = src_profile;
    }
  }

  if(!_set_directory_tags(dst, width, height, name, page_profile)) return FALSE;
  if(page_profile && page_profile[0] && _icc_blob_from_profile_key(page_profile, &icc_profile, &icc_profile_len))
    TIFFSetField(dst, TIFFTAG_ICCPROFILE, icc_profile_len, icc_profile);

  uint16_t *src_row = g_malloc0_n((gsize)width * 4, sizeof(uint16_t));
  uint16_t *dst_row = g_malloc0_n((gsize)width * 4, sizeof(uint16_t));
  if(!src_row || !dst_row)
  {
    g_free(icc_profile);
    g_free(src_row);
    g_free(dst_row);
    return FALSE;
  }

  const int offset_x = (raw_width - (int)width) / 2;
  const int offset_y = (raw_height - (int)height) / 2;

  for(uint32_t y = 0; y < height; y++)
  {
    _clear_transparent_half(dst_row, (size_t)width);

    if(src)
    {
      if(!_read_scanline_rgba(src, width, y, src_row))
      {
        g_free(icc_profile);
        g_free(src_row);
        g_free(dst_row);
        return FALSE;
      }
      memcpy(dst_row, src_row, (size_t)width * 4 * sizeof(uint16_t));
    }

    if(patch)
    {
      const int raw_y = (int)y + offset_y;
      if(raw_y >= patch->y && raw_y < patch->y + patch->height)
      {
        const float *patch_row = patch->pixels + 4 * (size_t)(raw_y - patch->y) * patch->width;
        for(uint32_t x = 0; x < width; x++)
        {
          const int raw_x = (int)x + offset_x;
          if(raw_x < patch->x || raw_x >= patch->x + patch->width) continue;
          const float *src_pixel = patch_row + 4 * (size_t)(raw_x - patch->x);
          dst_row[4 * x + 0] = _float_to_half(src_pixel[0]);
          dst_row[4 * x + 1] = _float_to_half(src_pixel[1]);
          dst_row[4 * x + 2] = _float_to_half(src_pixel[2]);
          dst_row[4 * x + 3] = _float_to_half(src_pixel[3]);
        }
      }
    }

    if(!_write_scanline_rgba(dst, width, y, dst_row))
    {
      g_free(icc_profile);
      g_free(src_row);
      g_free(dst_row);
      return FALSE;
    }
  }

  g_free(icc_profile);
  g_free(src_row);
  g_free(dst_row);
  return TIFFWriteDirectory(dst) != 0;
}

static gboolean _rewrite_sidecar(const char *path, const char *target_name, const int target_order,
                                 const char *work_profile, const drawlayer_patch_t *patch, const int raw_width,
                                 const int raw_height, const gboolean delete_target, const int insert_order,
                                 int *final_order)
{
  /* Whole-file rewrite strategy:
   * TIFF page insertion/deletion/update is easier and more robust here by rewriting to a
   * temporary file, then atomically renaming it over the original sidecar. */
  if(final_order) *final_order = -1;

  TIFF *src = NULL;
  drawlayer_dir_info_t info;
  memset(&info, 0, sizeof(info));
  info.index = -1;

  if(g_file_test(path, G_FILE_TEST_EXISTS))
  {
    src = TIFFOpen(path, "rb");
    if(!src) return FALSE;
    _scan_directories(src, target_name, target_order, &info);
  }

  if(delete_target && (!src || !info.found))
  {
    if(src) TIFFClose(src);
    return TRUE;
  }

  if(delete_target && src && info.found && info.count == 1)
  {
    TIFFClose(src);
    return g_unlink(path) == 0;
  }

  gchar *tmp_path = g_strdup_printf("%s.tmp", path);
  TIFF *dst = TIFFOpen(tmp_path, "wb");
  if(!dst)
  {
    if(src) TIFFClose(src);
    g_free(tmp_path);
    return FALSE;
  }

  int written_index = 0;
  gboolean ok = TRUE;

  if(src)
  {
    for(int dir = 0; dir < info.count && ok; dir++)
    {
      /* Optional insertion path used by drawlayer's "create background layer"
       * action: when creating a new page (target not found), insert it at the
       * requested page order instead of always appending at the end. */
      if(!delete_target && !info.found && insert_order >= 0 && written_index == insert_order)
      {
        ok = _write_page(dst, NULL, -1, target_name, work_profile, patch, raw_width, raw_height);
        if(ok && final_order) *final_order = written_index;
        if(ok) written_index++;
      }

      const gboolean is_target = info.found && dir == info.index;
      if(is_target && delete_target) continue;

      const drawlayer_patch_t *page_patch = (is_target && !delete_target) ? patch : NULL;
      const char *page_label = is_target ? target_name : NULL;

      ok = _write_page(dst, src, dir, page_label, is_target ? work_profile : NULL, page_patch, raw_width, raw_height);
      if(ok && is_target && !delete_target && final_order) *final_order = written_index;
      if(ok) written_index++;
    }
  }

  if(ok && !delete_target && (!src || !info.found))
  {
    const gboolean append_at_end = (insert_order < 0 || insert_order >= written_index || !src);
    if(append_at_end)
    {
      ok = _write_page(dst, NULL, -1, target_name, work_profile, patch, raw_width, raw_height);
      if(ok && final_order) *final_order = written_index;
    }
  }

  TIFFClose(dst);
  if(src) TIFFClose(src);

  if(ok)
    ok = (g_rename(tmp_path, path) == 0);
  else
    g_unlink(tmp_path);

  g_free(tmp_path);
  return ok;
}

static gboolean _layer_name_exists_in_sidecar(const char *path, const char *candidate, const int ignore_index)
{
  if(!path || !candidate || candidate[0] == '\0' || !g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;

  gboolean exists = FALSE;
  int index = 0;
  if(TIFFSetDirectory(tiff, 0))
  {
    do
    {
      char *page_name = NULL;
      TIFFGetField(tiff, TIFFTAG_PAGENAME, &page_name);
      if(index != ignore_index && !g_strcmp0(page_name ? page_name : "", candidate))
      {
        exists = TRUE;
        break;
      }
      index++;
    } while(TIFFReadDirectory(tiff));
  }

  TIFFClose(tiff);
  return exists;
}

static void _make_unique_layer_name(dt_iop_module_t *self, const char *path, const char *requested, char *name,
                                    const size_t name_size);

static gboolean _layerio_sidecar_path(const int32_t imgid, char *path, const size_t path_size)
{
  return _get_sidecar_path(imgid, path, path_size);
}

static gboolean _layerio_find_layer(const char *path, const char *target_name, const int target_order,
                                    drawlayer_dir_info_t *info)
{
  if(info) memset(info, 0, sizeof(*info));
  if(!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;
  _scan_directories(tiff, target_name, target_order, info);
  TIFFClose(tiff);
  return info && info->found;
}

static gboolean _layerio_load_layer(const char *path, const char *target_name, const int target_order,
                                    const int raw_width, const int raw_height, drawlayer_patch_t *patch)
{
  return _read_region(path, target_name, target_order, raw_width, raw_height, patch);
}

static gboolean _layerio_load_flat_rgba(const char *path, float **pixels, int *width, int *height)
{
  if(pixels) *pixels = NULL;
  if(width) *width = 0;
  if(height) *height = 0;
  if(!path || !pixels || !width || !height || !g_file_test(path, G_FILE_TEST_EXISTS)) return FALSE;

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff) return FALSE;

  uint32_t w = 0, h = 0;
  uint16_t spp = 0, bpp = 0, sampleformat = SAMPLEFORMAT_UINT;
  TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &w);
  TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &h);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLESPERPIXEL, &spp);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_BITSPERSAMPLE, &bpp);
  TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleformat);

  if(w == 0 || h == 0 || spp < 1)
  {
    TIFFClose(tiff);
    return FALSE;
  }

  float *out = _alloc_tracked_temp_buffer((size_t)w * h * 4 * sizeof(float), "drawlayer bg export");
  if(!out)
  {
    TIFFClose(tiff);
    return FALSE;
  }
  _clear_transparent_float(out, (size_t)w * h);

  const tsize_t scanline = TIFFScanlineSize(tiff);
  tdata_t row = _TIFFmalloc((tsize_t)scanline);
  if(!row)
  {
    _free_tracked_temp_buffer((void **)&out, "drawlayer bg export");
    TIFFClose(tiff);
    return FALSE;
  }

  gboolean ok = TRUE;
  for(uint32_t y = 0; y < h && ok; y++)
  {
    if(TIFFReadScanline(tiff, row, y, 0) == -1)
    {
      ok = FALSE;
      break;
    }

    for(uint32_t x = 0; x < w; x++)
    {
      float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;

      if(bpp == 32 && sampleformat == SAMPLEFORMAT_IEEEFP)
      {
        const float *src = (const float *)row + (size_t)x * spp;
        if(spp >= 3)
        {
          r = src[0];
          g = src[1];
          b = src[2];
        }
        else
        {
          r = g = b = src[0];
        }
        if(spp >= 4) a = src[3];
      }
      else if(bpp == 16 && sampleformat == SAMPLEFORMAT_IEEEFP)
      {
        const uint16_t *src = (const uint16_t *)row + (size_t)x * spp;
        if(spp >= 3)
        {
          r = _half_to_float(src[0]);
          g = _half_to_float(src[1]);
          b = _half_to_float(src[2]);
        }
        else
        {
          r = g = b = _half_to_float(src[0]);
        }
        if(spp >= 4) a = _half_to_float(src[3]);
      }
      else if(bpp == 16)
      {
        const uint16_t *src = (const uint16_t *)row + (size_t)x * spp;
        if(spp >= 3)
        {
          r = src[0] / 65535.0f;
          g = src[1] / 65535.0f;
          b = src[2] / 65535.0f;
        }
        else
        {
          r = g = b = src[0] / 65535.0f;
        }
        if(spp >= 4) a = src[3] / 65535.0f;
      }
      else if(bpp == 8)
      {
        const uint8_t *src = (const uint8_t *)row + (size_t)x * spp;
        if(spp >= 3)
        {
          r = src[0] / 255.0f;
          g = src[1] / 255.0f;
          b = src[2] / 255.0f;
        }
        else
        {
          r = g = b = src[0] / 255.0f;
        }
        if(spp >= 4) a = src[3] / 255.0f;
      }
      else
      {
        ok = FALSE;
        break;
      }

      float *dst = out + 4 * ((size_t)y * w + x);
      dst[0] = r;
      dst[1] = g;
      dst[2] = b;
      dst[3] = a;
    }
  }

  _TIFFfree(row);
  TIFFClose(tiff);
  if(!ok)
  {
    _free_tracked_temp_buffer((void **)&out, "drawlayer bg export");
    return FALSE;
  }

  *pixels = out;
  *width = (int)w;
  *height = (int)h;
  return TRUE;
}

static gboolean _layerio_store_layer(const char *path, const char *target_name, const int target_order,
                                     const char *work_profile, const drawlayer_patch_t *patch,
                                     const int raw_width, const int raw_height, const gboolean delete_target,
                                     int *final_order)
{
  if(!target_name || target_name[0] == '\0') return FALSE;
  return _rewrite_sidecar(path, target_name, target_order, work_profile, patch, raw_width, raw_height,
                          delete_target, -1, final_order);
}

static gboolean _layerio_insert_layer(const char *path, const char *target_name, const int insert_after_order,
                                      const char *work_profile, const drawlayer_patch_t *patch,
                                      const int raw_width, const int raw_height, int *final_order)
{
  if(!target_name || target_name[0] == '\0') return FALSE;
  const int insert_order = (insert_after_order >= 0) ? (insert_after_order + 1) : -1;
  return _rewrite_sidecar(path, target_name, -1, work_profile, patch, raw_width, raw_height, FALSE, insert_order,
                          final_order);
}

static void _layerio_make_unique_name(dt_iop_module_t *self, const char *path, const char *requested, char *name,
                                      const size_t name_size)
{
  _make_unique_layer_name(self, path, requested, name, name_size);
}

static gboolean _layerio_layer_name_exists(const char *path, const char *candidate, const int ignore_index)
{
  return _layer_name_exists_in_sidecar(path, candidate, ignore_index);
}

static void _layerio_make_unique_name_plain(const char *path, const char *requested, char *name, const size_t name_size)
{
  if(!name || name_size == 0) return;
  name[0] = '\0';
  if(requested) g_strlcpy(name, requested, name_size);
  g_strstrip(name);
  if(name[0] == '\0') return;
  if(!_layer_name_exists_in_sidecar(path, name, -1)) return;

  char base[DRAWLAYER_NAME_SIZE] = { 0 };
  g_strlcpy(base, name, sizeof(base));

  for(int suffix = 2; suffix < 100000; suffix++)
  {
    g_snprintf(name, name_size, "%.*s %d", MAX((int)name_size - 12, 1), base, suffix);
    if(!_layer_name_exists_in_sidecar(path, name, -1)) return;
  }
}

static void _make_unique_layer_name(dt_iop_module_t *self, const char *path, const char *requested, char *name,
                                    const size_t name_size)
{
  _sanitize_requested_layer_name(self, requested, name, name_size);
  if(name[0] == '\0') return;

  if(!_layer_name_exists_in_sidecar(path, name, -1)) return;

  char base[DRAWLAYER_NAME_SIZE] = { 0 };
  g_strlcpy(base, name, sizeof(base));

  for(int suffix = 2; suffix < 100000; suffix++)
  {
    g_snprintf(name, name_size, "%.*s %d", MAX((int)name_size - 12, 1), base, suffix);
    if(!_layer_name_exists_in_sidecar(path, name, -1)) return;
  }
}

static void _populate_layer_list(dt_iop_module_t *self)
{
  dt_iop_drawlayer_gui_data_t *g = (dt_iop_drawlayer_gui_data_t *)self->gui_data;
  dt_iop_drawlayer_params_t *params = (dt_iop_drawlayer_params_t *)self->params;
  if(!g) return;
  _ensure_layer_name(self, params);

  while(dt_bauhaus_combobox_length(g->layer_select) > 0)
    dt_bauhaus_combobox_remove_at(g->layer_select, dt_bauhaus_combobox_length(g->layer_select) - 1);

  if(!self->dev)
  {
    dt_bauhaus_combobox_add(g->layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->layer_select, 0);
    return;
  }

  char path[PATH_MAX] = { 0 };
  if(!_get_sidecar_path(self->dev->image_storage.id, path, sizeof(path)) || !g_file_test(path, G_FILE_TEST_EXISTS))
  {
    dt_bauhaus_combobox_add(g->layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->layer_select, 0);
    return;
  }

  TIFF *tiff = TIFFOpen(path, "rb");
  if(!tiff)
  {
    dt_bauhaus_combobox_add(g->layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->layer_select, 0);
    return;
  }

  int active = -1;
  int index = 0;
  if(TIFFSetDirectory(tiff, 0))
  {
    do
    {
      char *page_name = NULL;
      TIFFGetField(tiff, TIFFTAG_PAGENAME, &page_name);
      dt_bauhaus_combobox_add(g->layer_select, page_name ? page_name : "");

      if(params->layer_order == index
         || (params->layer_name[0] && !g_strcmp0(page_name ? page_name : "", params->layer_name)))
        active = index;
      index++;
    } while(TIFFReadDirectory(tiff));
  }

  TIFFClose(tiff);
  if(index == 0)
  {
    dt_bauhaus_combobox_add(g->layer_select, params->layer_name);
    dt_bauhaus_combobox_set(g->layer_select, 0);
  }
  else if(active >= 0)
  {
    dt_bauhaus_combobox_set(g->layer_select, active);
  }
  else
  {
    dt_bauhaus_combobox_set(g->layer_select, 0);
  }
}
