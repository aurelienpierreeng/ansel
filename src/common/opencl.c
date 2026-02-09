/*
    This file is part of darktable,
    Copyright (C) 2010-2022 darktable developers.

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

#ifdef HAVE_OPENCL

#include "common/opencl.h"
#include "common/bilateralcl.h"
#include "common/darktable.h"
#include "common/dlopencl.h"
#include "common/dwt.h"
#include "common/file_location.h"
#include "common/gaussian.h"
#include "common/guided_filter.h"
#include "common/heal.h"
#include "common/interpolation.h"
#include "common/locallaplaciancl.h"
#include "common/nvidia_gpus.h"
#include "common/opencl_drivers_blacklist.h"
#include "common/tea.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/pixelpipe.h"
#include "develop/pixelpipe_cache.h"

#include <assert.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <sys/stat.h>
#include <zlib.h>

static const char *dt_opencl_get_vendor_by_id(unsigned int id);
static float dt_opencl_benchmark_gpu(const int devid, const size_t width, const size_t height, const int count, const float sigma);
static float dt_opencl_benchmark_cpu(const size_t width, const size_t height, const int count, const float sigma);
static char *_ascii_str_canonical(const char *in, char *out, int maxlen);
/** parse a single token of priority string and store priorities in priority_list */
static void dt_opencl_priority_parse(dt_opencl_t *cl, char *configstr, int *priority_list, int *mandatory);
/** set device priorities according to config string */
static void dt_opencl_update_priorities();
/** adjust opencl subsystem according to scheduling profile */
static void dt_opencl_apply_scheduling_profile();
/** set opencl specific synchronization timeout */
static void dt_opencl_set_synchronization_timeout(int value);


int dt_opencl_get_device_info(dt_opencl_t *cl, cl_device_id device, cl_device_info param_name, void **param_value,
                              size_t *param_value_size)
{
  *param_value_size = SIZE_MAX;

  // 1. figure out how much memory is needed
  cl_int err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(device, param_name, 0, NULL, param_value_size);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_get_device_info] could not query the actual size in bytes of info %d: %i\n", param_name, err);
    goto error;
  }

  // 2. did we /actually/ get the size?
  if(*param_value_size == SIZE_MAX || *param_value_size == 0)
  {
    // both of these sizes make no sense. either i failed to parse spec, or opencl implementation bug?
    dt_print(DT_DEBUG_OPENCL,
             "[dt_opencl_get_device_info] ERROR: no size returned, or zero size returned for data %d: %zu\n",
             param_name, *param_value_size);
    err = CL_INVALID_VALUE; // FIXME: anything better?
    goto error;
  }

  // 3. make sure that *param_value points to big-enough memory block
  {
    void *ptr = realloc(*param_value, *param_value_size);
    if(!ptr)
    {
      dt_print(DT_DEBUG_OPENCL,
               "[dt_opencl_get_device_info] memory allocation failed! tried to allocate %zu bytes for data %d: %i",
               *param_value_size, param_name, err);
      err = CL_OUT_OF_HOST_MEMORY;
      goto error;
    }

    // allocation succeeded, update pointer.
    *param_value = ptr;
  }

  // 4. actually get the value
  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(device, param_name, *param_value_size, *param_value, NULL);
  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[dt_opencl_get_device_info] could not query info %d: %i\n", param_name, err);
    goto error;
  }

  return CL_SUCCESS;

error:
  free(*param_value);
  *param_value = NULL;
  *param_value_size = 0;
  return err;
}

int dt_opencl_avoid_atomics(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  return (!cl->inited || devid < 0) ? 0 : cl->dev[devid].avoid_atomics;
}

int dt_opencl_micro_nap(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  return (!cl->inited || devid < 0) ? 0 : cl->dev[devid].micro_nap;
}

gboolean dt_opencl_use_pinned_memory(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;
  return cl->dev[devid].pinned_memory;
}

void dt_opencl_write_device_config(const int devid)
{
  if(devid < 0) return;
  dt_opencl_t *cl = darktable.opencl;
  gchar buf[256] = { 0 };
  gchar key_device[256] = { 0 };
  g_snprintf(key_device, 254, "%s/%i/%s", DT_CLDEVICE_HEAD, devid, cl->dev[devid].cname);

  g_snprintf(buf, sizeof(buf), "%s/avoid_atomics", key_device);
  dt_conf_set_int(buf, cl->dev[devid].avoid_atomics);

  g_snprintf(buf, sizeof(buf), "%s/micro_nap", key_device);
  dt_conf_set_int(buf, cl->dev[devid].micro_nap);

  g_snprintf(buf, sizeof(buf), "%s/pinned_memory", key_device);
  dt_conf_set_int(buf, cl->dev[devid].pinned_memory & (DT_OPENCL_PINNING_ON | DT_OPENCL_PINNING_DISABLED));

  g_snprintf(buf, sizeof(buf), "%s/wd", key_device);
  dt_conf_set_int(buf, cl->dev[devid].clroundup_wd);

  g_snprintf(buf, sizeof(buf), "%s/ht", key_device);
  dt_conf_set_int(buf, cl->dev[devid].clroundup_ht);

  g_snprintf(buf, sizeof(buf), "%s/event_handles", key_device);
  dt_conf_set_int(buf, cl->dev[devid].event_handles);

  g_snprintf(buf, sizeof(buf), "%s/disabled", key_device);
  dt_conf_set_int(buf, cl->dev[devid].disabled & 1);

  g_snprintf(buf, sizeof(buf), "%s/benchmark", key_device);
  dt_conf_set_float(buf, cl->dev[devid].benchmark);

  // Also take care of extended device data, these are not only device specific but also depend on the devid
  // to support systems with two similar cards.

  g_snprintf(buf, sizeof(buf), "%s/id%i/forced_headroom", key_device, devid);
  dt_conf_set_int(buf, cl->dev[devid].forced_headroom);
}

static int _dt_opencl_get_conf_int(const gchar *key_device, const gchar *conf_name, gboolean *safety_ok)
{
  int res = 0;
  gchar *key = g_strconcat(key_device, "/", conf_name, NULL);
  const gboolean existing_device = dt_conf_key_not_empty(key);
  if(existing_device)
    res = dt_conf_get_int(key);
  else
  {
    dt_print(DT_DEBUG_OPENCL, "Warning: conf '%s' not found in anselrc.\n", key);
    *safety_ok = FALSE;
  }

  g_free(key);
  return res;
}

static float _dt_opencl_get_conf_float(const gchar *key_device, const gchar *conf_name, gboolean *safety_ok)
{
  float res = 0.0f;
  gchar *key = g_strconcat(key_device, "/", conf_name, NULL);
  const gboolean existing_device = dt_conf_key_not_empty(key);
  if(existing_device)
    res = dt_conf_get_float(key);
  else
  {
    dt_print(DT_DEBUG_OPENCL, "Warning: conf '%s' not found in anselrc.\n", key);
    *safety_ok = FALSE;
  }

  g_free(key);
  return res;
}

gboolean dt_opencl_read_device_config(const int devid)
{
  if(devid < 0) return FALSE;
  dt_opencl_t *cl = darktable.opencl;
  gchar key_device[256] = { 0 };
  g_snprintf(key_device, 254, "%s/%i/%s", DT_CLDEVICE_HEAD, devid, cl->dev[devid].cname);
  gboolean safety_ok = TRUE;

  int avoid_atomics = _dt_opencl_get_conf_int(key_device, "avoid_atomics", &safety_ok);
  int micro_nap = _dt_opencl_get_conf_int(key_device, "micro_nap", &safety_ok);
  int pinned_memory = _dt_opencl_get_conf_int(key_device, "pinned_memory", &safety_ok);
  int wd = _dt_opencl_get_conf_int(key_device, "wd", &safety_ok);
  int ht = _dt_opencl_get_conf_int(key_device, "ht", &safety_ok);
  int event_handles = _dt_opencl_get_conf_int(key_device, "event_handles", &safety_ok);
  int disabled = _dt_opencl_get_conf_int(key_device, "disabled", &safety_ok);
  float benchmark = _dt_opencl_get_conf_float(key_device, "benchmark", &safety_ok);

  // some rudimentary safety checking if string seems to be ok
  safety_ok |= (wd > 1) && (wd < 513) && (ht > 1) && (ht < 513);

  if(safety_ok)
  {
    cl->dev[devid].avoid_atomics = avoid_atomics;
    cl->dev[devid].micro_nap = micro_nap;
    cl->dev[devid].pinned_memory = pinned_memory;
    cl->dev[devid].clroundup_wd = wd;
    cl->dev[devid].clroundup_ht = ht;
    cl->dev[devid].event_handles = event_handles;
    cl->dev[devid].disabled = disabled;
    cl->dev[devid].benchmark = benchmark;
  }
  else // if there is something wrong with the found conf key reset to defaults
  {
    dt_print(DT_DEBUG_OPENCL, "[dt_opencl_read_device_config] malformed data '%s'\n", key_device);
  }

  // do some safety housekeeping
  cl->dev[devid].avoid_atomics &= 1;
  cl->dev[devid].pinned_memory &= (DT_OPENCL_PINNING_ON | DT_OPENCL_PINNING_DISABLED);
  cl->dev[devid].micro_nap = CLAMP(cl->dev[devid].micro_nap, 250, 1000000);
  if((cl->dev[devid].clroundup_wd < 2) || (cl->dev[devid].clroundup_wd > 512))
    cl->dev[devid].clroundup_wd = 16;
  if((cl->dev[devid].clroundup_ht < 2) || (cl->dev[devid].clroundup_ht > 512))
    cl->dev[devid].clroundup_ht = 16;
  if(cl->dev[devid].event_handles < 0)
    cl->dev[devid].event_handles = 0x40961440;
  cl->dev[devid].benchmark = fminf(1e6, fmaxf(0.0f, cl->dev[devid].benchmark));

  cl->dev[devid].use_events = (cl->dev[devid].event_handles != 0) ? 1 : 0;
  cl->dev[devid].disabled &= 1;

  // Also take care of extended device data, these are not only device specific but also depend on the devid
  g_snprintf(key_device, 254, "%s/%i/%s/id%i/forced_headroom", DT_CLDEVICE_HEAD, devid, cl->dev[devid].cname, devid);
  if(dt_conf_key_not_empty(key_device))
  {
    int forced_headroom = dt_conf_get_int(key_device);
    if(forced_headroom > 0) cl->dev[devid].forced_headroom = forced_headroom;
  }
  else // this is used if updating to 4.0 or fresh installs; see commenting _opencl_get_unused_device_mem()
    cl->dev[devid].forced_headroom = dt_conf_get_int64("memory_opencl_headroom");

  dt_opencl_write_device_config(devid);
  return !safety_ok;
}

static float dt_opencl_device_perfgain(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  const float tcpu = cl->cpubenchmark;
  const float tgpu = cl->dev[devid].benchmark;
  if((tcpu < 1e-8) || (tgpu < 1e-8)) return 1.0f;
  return (tcpu / tgpu);
}

// returns 0 if all ok or an error if we failed to init this device
static int dt_opencl_device_init(dt_opencl_t *cl, const int dev, cl_device_id *devices, const int k)
{
  int res;
  cl_int err;

  memset(cl->dev[dev].program, 0x0, sizeof(cl_program) * DT_OPENCL_MAX_PROGRAMS);
  memset(cl->dev[dev].program_used, 0x0, sizeof(int) * DT_OPENCL_MAX_PROGRAMS);
  memset(cl->dev[dev].kernel, 0x0, sizeof(cl_kernel) * DT_OPENCL_MAX_KERNELS);
  memset(cl->dev[dev].kernel_used, 0x0, sizeof(int) * DT_OPENCL_MAX_KERNELS);
  cl->dev[dev].eventlist = NULL;
  cl->dev[dev].eventtags = NULL;
  cl->dev[dev].numevents = 0;
  cl->dev[dev].eventsconsolidated = 0;
  cl->dev[dev].maxevents = 0;
  cl->dev[dev].maxeventslot = 0;
  cl->dev[dev].lostevents = 0;
  cl->dev[dev].totalevents = 0;
  cl->dev[dev].totalsuccess = 0;
  cl->dev[dev].totallost = 0;
  cl->dev[dev].summary = CL_COMPLETE;
  cl->dev[dev].used_global_mem = 0;
  cl->dev[dev].nvidia_sm_20 = 0;
  cl->dev[dev].vendor = NULL;
  cl->dev[dev].name = NULL;
  cl->dev[dev].cname = NULL;
  cl->dev[dev].options = NULL;
  cl->dev[dev].memory_in_use = 0;
  cl->dev[dev].peak_memory = 0;
  cl->dev[dev].used_available = 0;
  // setting sane/conservative defaults at first
  cl->dev[dev].avoid_atomics = 0;
  cl->dev[dev].micro_nap = 250;
  cl->dev[dev].pinned_memory = DT_OPENCL_PINNING_OFF;
  cl->dev[dev].clroundup_wd = 16;
  cl->dev[dev].clroundup_ht = 16;
  cl->dev[dev].benchmark = 0.0f;
  cl->dev[dev].use_events = 1;
  cl->dev[dev].event_handles = 128;
  cl->dev[dev].disabled = 0;
  cl->dev[dev].forced_headroom = 0;
  cl->dev[dev].runtime_error = 0;
  cl_device_id devid = cl->dev[dev].devid = devices[k];

  char *infostr = NULL;
  size_t infostr_size;

  char *cname = NULL;
  size_t cname_size;

  char *vendor = NULL;
  size_t vendor_size;

  char *driverversion = NULL;
  size_t driverversion_size;

  char *deviceversion = NULL;
  size_t deviceversion_size;

  size_t infoint;
  size_t *infointtab = NULL;
  cl_device_type type;
  cl_bool image_support = 0;
  cl_bool device_available = 0;
  cl_uint vendor_id = 0;
  cl_bool little_endian = 0;
  cl_platform_id platform_id = 0;

  char *dtcache = calloc(PATH_MAX, sizeof(char));
  char *cachedir = calloc(PATH_MAX, sizeof(char));
  char *devname = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *drvversion = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *platform_name = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *platform_vendor = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));

  char kerneldir[PATH_MAX] = { 0 };
  char *filename = calloc(PATH_MAX, sizeof(char));
  char *confentry = calloc(PATH_MAX, sizeof(char));
  char *binname = calloc(PATH_MAX, sizeof(char));
  dt_print_nts(DT_DEBUG_OPENCL, "\n[dt_opencl_device_init]\n");

  // test GPU availability, vendor, memory, image support etc:
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_AVAILABLE, sizeof(cl_bool), &device_available, NULL);

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_VENDOR, (void **)&vendor, &vendor_size);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "  *** could not get vendor name of device %d: %i\n", k, err);
    res = -1;
    goto end;
  }

  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_VENDOR_ID, sizeof(cl_uint), &vendor_id, NULL);

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_NAME, (void **)&infostr, &infostr_size);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "  *** could not get device name of device %d: %i\n", k, err);
    res = -1;
    goto end;
  }

  // get the canonical device name
  cname_size = infostr_size;
  cname = malloc(cname_size);
  _ascii_str_canonical(infostr, cname, cname_size);
  cl->dev[dev].name = strdup(infostr);
  cl->dev[dev].cname = strdup(cname);

  // take every detected device into account of checksum
  cl->crc = crc32(cl->crc, (const unsigned char *)infostr, strlen(infostr));

  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_PLATFORM, sizeof(cl_platform_id), &platform_id, NULL);
  if(err != CL_SUCCESS)
  {
    g_strlcpy(platform_vendor, "no platform id", DT_OPENCL_CBUFFSIZE);
    g_strlcpy(platform_name, "no platform id", DT_OPENCL_CBUFFSIZE);
    dt_print_nts(DT_DEBUG_OPENCL, "  *** could not get platform id for device `%s' : %i\n", cl->dev[dev].name, err);
  }
  else
  {
    err = (cl->dlocl->symbols->dt_clGetPlatformInfo)(platform_id, CL_PLATFORM_NAME, DT_OPENCL_CBUFFSIZE, platform_name, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print_nts(DT_DEBUG_OPENCL, "  *** could not get platform name for device `%s' : %i\n", cl->dev[dev].name, err);
      g_strlcpy(platform_name, "???", DT_OPENCL_CBUFFSIZE);
    }

    err = (cl->dlocl->symbols->dt_clGetPlatformInfo)(platform_id, CL_PLATFORM_VENDOR, DT_OPENCL_CBUFFSIZE, platform_vendor, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print_nts(DT_DEBUG_OPENCL, "  *** could not get platform vendor for device `%s' : %i\n", cl->dev[dev].name, err);
      g_strlcpy(platform_vendor, "???", DT_OPENCL_CBUFFSIZE);
    }
  }

  const gboolean newdevice = dt_opencl_read_device_config(dev);
  dt_print_nts(DT_DEBUG_OPENCL, "   DEVICE:                   %d: '%s'%s\n", k, infostr, (newdevice) ? ", NEW" : "" );
  dt_print_nts(DT_DEBUG_OPENCL, "   CANONICAL NAME:           %s\n", cname);
  dt_print_nts(DT_DEBUG_OPENCL, "   PLATFORM NAME & VENDOR:   %s, %s\n", platform_name, platform_vendor);

  err = dt_opencl_get_device_info(cl, devid, CL_DRIVER_VERSION, (void **)&driverversion, &driverversion_size);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** driver version not available *** %i\n", err);
    res = -1;
    cl->dev[dev].disabled |= 1;
    goto end;
  }

  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_VERSION, (void **)&deviceversion, &deviceversion_size);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** device version not available *** %i\n", err);
    res = -1;
    cl->dev[dev].disabled |= 1;
    goto end;
  }

  // take every detected device driver into account of checksum
  cl->crc = crc32(cl->crc, (const unsigned char *)deviceversion, deviceversion_size);

  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_TYPE, sizeof(cl_device_type), &type, NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE_SUPPORT, sizeof(cl_bool), &image_support, NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_HEIGHT, sizeof(size_t),
                                           &(cl->dev[dev].max_image_height), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_IMAGE2D_MAX_WIDTH, sizeof(size_t),
                                           &(cl->dev[dev].max_image_width), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_MEM_ALLOC_SIZE, sizeof(cl_ulong),
                                           &(cl->dev[dev].max_mem_alloc), NULL);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_ENDIAN_LITTLE, sizeof(cl_bool), &little_endian, NULL);

  cl->dev[dev].cltype = (unsigned int)type;


  if(!strncasecmp(vendor, "NVIDIA", 6))
  {
    // very lame attempt to detect support for atomic float add in global memory.
    // we need compute model sm_20, but let's try for all nvidia devices :(
    cl->dev[dev].nvidia_sm_20 = dt_nvidia_gpu_supports_sm_20(infostr);
  }

  const gboolean is_cpu_device = (type & CL_DEVICE_TYPE_CPU) == CL_DEVICE_TYPE_CPU;

  // micro_nap can be made less conservative on current systems at least if not on-CPU
  if(newdevice)
    cl->dev[dev].micro_nap = (is_cpu_device) ? 1000 : 250;

  dt_print_nts(DT_DEBUG_OPENCL, "   DRIVER VERSION:           %s\n", driverversion);
  dt_print_nts(DT_DEBUG_OPENCL, "   DEVICE VERSION:           %s%s\n", deviceversion,
     cl->dev[dev].nvidia_sm_20 ? ", SM_20 SUPPORT" : "");
  dt_print_nts(DT_DEBUG_OPENCL, "   DEVICE_TYPE:              %s%s%s\n",
      ((type & CL_DEVICE_TYPE_CPU) == CL_DEVICE_TYPE_CPU) ? "CPU" : "",
      ((type & CL_DEVICE_TYPE_GPU) == CL_DEVICE_TYPE_GPU) ? "GPU" : "",
      (type & CL_DEVICE_TYPE_ACCELERATOR)                 ? ", Accelerator" : "" );

  if(is_cpu_device && newdevice)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** discarding new device as emulated by CPU ***\n");
    cl->dev[dev].disabled |= 1;
    res = -1;
    goto end;
  }

  if(!device_available)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** device is not available ***\n");
    res = -1;
    goto end;
  }

  if(!image_support)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** The OpenCL driver doesn't provide image support. See also 'clinfo' output ***\n");
    res = -1;
    cl->dev[dev].disabled |= 1;
    goto end;
  }

  if(!little_endian)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** device is not little endian ***\n");
    res = -1;
    cl->dev[dev].disabled |= 1;
    goto end;
  }

  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(cl_ulong),
                                           &(cl->dev[dev].max_global_mem), NULL);
  if(cl->dev[dev].max_global_mem < (uint64_t)512ul * 1024ul * 1024ul)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** insufficient global memory (%" PRIu64 "MB) ***\n",
                                   cl->dev[dev].max_global_mem / 1024 / 1024);
    res = -1;
    cl->dev[dev].disabled |= 1;
    goto end;
  }

  cl->dev[dev].vendor = strdup(dt_opencl_get_vendor_by_id(vendor_id));

  const gboolean is_blacklisted = dt_opencl_check_driver_blacklist(deviceversion);

  // disable device for now if this is the first time detected and blacklisted too.
  if(newdevice && is_blacklisted)
  {
    // To keep installations we look for the old blacklist conf key
    const gboolean old_blacklist = dt_conf_get_bool("opencl_disable_drivers_blacklist");
    cl->dev[dev].disabled |= (old_blacklist) ? 0 : 1;
    if(cl->dev[dev].disabled)
      dt_print_nts(DT_DEBUG_OPENCL, "   *** new device is blacklisted ***\n");
    res = -1;
    goto end;
  }

  dt_print_nts(DT_DEBUG_OPENCL, "   GLOBAL MEM SIZE:          %.0f MB\n", (double)cl->dev[dev].max_global_mem / 1024.0 / 1024.0);
  dt_print_nts(DT_DEBUG_OPENCL, "   MAX MEM ALLOC:            %.0f MB\n", (double)cl->dev[dev].max_mem_alloc / 1024.0 / 1024.0);
  dt_print_nts(DT_DEBUG_OPENCL, "   MAX IMAGE SIZE:           %zd x %zd\n", cl->dev[dev].max_image_width, cl->dev[dev].max_image_height);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_WORK_GROUP_SIZE, sizeof(infoint), &infoint, NULL);
  dt_print_nts(DT_DEBUG_OPENCL, "   MAX WORK GROUP SIZE:      %zu\n", infoint);
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS, sizeof(infoint), &infoint, NULL);
  dt_print_nts(DT_DEBUG_OPENCL, "   MAX WORK ITEM DIMENSIONS: %zu\n", infoint);

  size_t infointtab_size;
  err = dt_opencl_get_device_info(cl, devid, CL_DEVICE_MAX_WORK_ITEM_SIZES, (void **)&infointtab, &infointtab_size);
  if(err == CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   MAX WORK ITEM SIZES:      [ ");
    for(size_t i = 0; i < infoint; i++) dt_print_nts(DT_DEBUG_OPENCL, "%zu ", infointtab[i]);
    free(infointtab);
    infointtab = NULL;
    dt_print_nts(DT_DEBUG_OPENCL, "]\n");
  }
  else
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** could not get maximum work item sizes ***\n");
    res = -1;
    cl->dev[dev].disabled |= 1;
    goto end;
  }

  const gboolean pinning = (cl->dev[dev].pinned_memory & DT_OPENCL_PINNING_ON);
  dt_print_nts(DT_DEBUG_OPENCL, "   PINNED MEMORY TRANSFER:   %s\n", pinning ? "WANTED" : "NO");
  dt_print_nts(DT_DEBUG_OPENCL, "   FORCED HEADROOM:          %lu\n", cl->dev[dev].forced_headroom);
  dt_print_nts(DT_DEBUG_OPENCL, "   AVOID ATOMICS:            %s\n", (cl->dev[dev].avoid_atomics) ? "YES" : "NO");
  dt_print_nts(DT_DEBUG_OPENCL, "   MICRO NAP:                %i\n", cl->dev[dev].micro_nap);
  dt_print_nts(DT_DEBUG_OPENCL, "   ROUNDUP WIDTH:            %i\n", cl->dev[dev].clroundup_wd);
  dt_print_nts(DT_DEBUG_OPENCL, "   ROUNDUP HEIGHT:           %i\n", cl->dev[dev].clroundup_ht);
  dt_print_nts(DT_DEBUG_OPENCL, "   CHECK EVENT HANDLES:      %i\n", cl->dev[dev].event_handles);
  if(cl->dev[dev].benchmark > 0.0f)
    dt_print_nts(DT_DEBUG_OPENCL, "   PERFORMANCE:              %f\n", dt_opencl_device_perfgain(dev));
  dt_print_nts(DT_DEBUG_OPENCL, "   DEFAULT DEVICE:           %s\n", (type & CL_DEVICE_TYPE_DEFAULT) ? "YES" : "NO");

  if(cl->dev[dev].disabled)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** marked as disabled ***\n");
    res = -1;
    goto end;
  }
  dt_print_nts(DT_DEBUG_OPENCL, "   *** Device enabled ***\n");

  dt_pthread_mutex_init(&cl->dev[dev].lock, NULL);

  cl->dev[dev].context = (cl->dlocl->symbols->dt_clCreateContext)(0, 1, &devid, NULL, NULL, &err);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** could not create context *** %i\n", err);
    res = -1;
    goto end;
  }
  // create a command queue for first device the context reported
  cl->dev[dev].cmd_queue = (cl->dlocl->symbols->dt_clCreateCommandQueue)(
      cl->dev[dev].context, devid, (darktable.unmuted & DT_DEBUG_PERF) ? CL_QUEUE_PROFILING_ENABLE : 0, &err);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** could not create command queue *** %i\n", err);
    res = -1;
    goto end;
  }

  dt_loc_get_kerneldir(kerneldir, sizeof(kerneldir));
  dt_print_nts(DT_DEBUG_OPENCL, "   KERNEL SOURCE DIRECTORY:  %s\n", kerneldir);

  double tstart, tend, tdiff;
  dt_loc_get_user_cache_dir(dtcache, PATH_MAX * sizeof(char));

  int len = MIN(strlen(infostr),1024 * sizeof(char));;
  int j = 0;
  // remove non-alphanumeric chars from device name
  for(int i = 0; i < len; i++)
    if(isalnum(infostr[i])) devname[j++] = infostr[i];
  devname[j] = 0;
  len = MIN(strlen(driverversion), 1024 * sizeof(char));
  j = 0;
  // remove non-alphanumeric chars from driver version
  for(int i = 0; i < len; i++)
    if(isalnum(driverversion[i])) drvversion[j++] = driverversion[i];
  drvversion[j] = 0;
  snprintf(cachedir, PATH_MAX * sizeof(char), "%s" G_DIR_SEPARATOR_S "cached_kernels_for_%s_%s", dtcache, devname, drvversion);

  dt_print_nts(DT_DEBUG_OPENCL, "   KERNEL BUILD DIRECTORY:   %s\n", cachedir);

  if(g_mkdir_with_parents(cachedir, 0700) == -1)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "   *** failed to create kernel directory `%s' ***\n", cachedir);
    res = -1;
    goto end;
  }

  snprintf(filename, PATH_MAX * sizeof(char), "%s" G_DIR_SEPARATOR_S "programs.conf", kerneldir);

  char *escapedkerneldir = NULL;
#ifndef __APPLE__
  escapedkerneldir = g_strdup_printf("\"%s\"", kerneldir);
#else
  escapedkerneldir = dt_util_str_replace(kerneldir, " ", "\\ ");
#endif

  gchar* compile_option_name_cname = g_strdup_printf("%s/%i/%s/building", DT_CLDEVICE_HEAD, dev, cl->dev[dev].cname);
  const char* compile_opt = NULL;

  if(dt_conf_key_exists(compile_option_name_cname))
    compile_opt = dt_conf_get_string_const(compile_option_name_cname);
  else
  {
    switch(vendor_id)
    {
      case DT_OPENCL_VENDOR_AMD:
        compile_opt = DT_OPENCL_DEFAULT_COMPILE_AMD;
        break;
      case DT_OPENCL_VENDOR_NVIDIA:
        compile_opt = DT_OPENCL_DEFAULT_COMPILE_NVIDIA;
        break;
      case DT_OPENCL_VENDOR_INTEL:
        compile_opt = DT_OPENCL_DEFAULT_COMPILE_INTEL;
        break;
      default:
        compile_opt = DT_OPENCL_DEFAULT_COMPILE;
    }
  }
  gchar *my_option = g_strdup(compile_opt);
  dt_conf_set_string(compile_option_name_cname, my_option);

  cl->dev[dev].options = g_strdup_printf("-w %s %s -D%s=1 -I%s",
                            my_option,
                            (cl->dev[dev].nvidia_sm_20 ? " -DNVIDIA_SM_20=1" : ""),
                            dt_opencl_get_vendor_by_id(vendor_id), escapedkerneldir);

  dt_print_nts(DT_DEBUG_OPENCL, "   CL COMPILER OPTION:       %s\n", my_option);

  g_free(compile_option_name_cname);
  g_free(my_option);
  g_free(escapedkerneldir);
  escapedkerneldir = NULL;

  const char *clincludes[DT_OPENCL_MAX_INCLUDES] = { "rgb_norms.h", "noise_generator.h", "color_conversion.h", "colorspaces.cl", "colorspace.h", "common.h", NULL };
  char *includemd5[DT_OPENCL_MAX_INCLUDES] = { NULL };
  dt_opencl_md5sum(clincludes, includemd5);

  if(newdevice) // so far the device seems to be ok. Make sure to write&export the conf database to
  {
    dt_opencl_write_device_config(dev);
    dt_conf_save(darktable.conf);
  }

  // now load all darktable cl kernels.
  // TODO: compile as a job?
  tstart = dt_get_wtime();
  FILE *f = g_fopen(filename, "rb");
  if(f)
  {
    while(!feof(f))
    {
      int prog = -1;
      gchar *confline_pattern = g_strdup_printf("%%%zu[^\n]\n", PATH_MAX * sizeof(char) - 1);
      int rd = fscanf(f, confline_pattern, confentry);
      g_free(confline_pattern);
      if(rd != 1) continue;
      // remove comments:
      size_t end = strlen(confentry);
      for(size_t pos = 0; pos < end; pos++)
        if(confentry[pos] == '#')
        {
          confentry[pos] = '\0';
          for(int l = pos - 1; l >= 0; l--)
          {
            if(confentry[l] == ' ')
              confentry[l] = '\0';
            else
              break;
          }
          break;
        }
      if(confentry[0] == '\0') continue;

      const char *programname = NULL, *programnumber = NULL;
      gchar **tokens = g_strsplit_set(confentry, " \t", 2);
      if(tokens)
      {
        programname = tokens[0];
        if(tokens[0])
          programnumber = tokens[1]; // if the 0st wasn't NULL then we have at least the terminating NULL in [1]
      }

      prog = programnumber ? strtol(programnumber, NULL, 10) : -1;

      if(!programname || programname[0] == '\0' || prog < 0)
      {
        dt_print(DT_DEBUG_OPENCL, "[dt_opencl_device_init] malformed entry in programs.conf `%s'; ignoring it!\n", confentry);
        continue;
      }

      snprintf(filename, PATH_MAX * sizeof(char), "%s" G_DIR_SEPARATOR_S "%s", kerneldir, programname);
      snprintf(binname, PATH_MAX * sizeof(char), "%s" G_DIR_SEPARATOR_S "%s.bin", cachedir, programname);
      dt_vprint(DT_DEBUG_OPENCL, "[dt_opencl_device_init] testing program `%s' ..\n", programname);
      int loaded_cached;
      char md5sum[33];
      if(dt_opencl_load_program(dev, prog, filename, binname, cachedir, md5sum, includemd5, &loaded_cached)
         && dt_opencl_build_program(dev, prog, binname, cachedir, md5sum, loaded_cached) != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[dt_opencl_device_init] failed to compile program `%s'!\n", programname);
        fclose(f);
        g_strfreev(tokens);
        res = -1;
        goto end;
      }

      g_strfreev(tokens);
    }

    fclose(f);
    tend = dt_get_wtime();
    tdiff = tend - tstart;
    dt_print_nts(DT_DEBUG_OPENCL, "   KERNEL LOADING TIME:       %2.4lf sec\n", tdiff);
  }
  else
  {
    dt_print_nts(DT_DEBUG_OPENCL, "[dt_opencl_device_init] could not open `%s'!\n", filename);
    res = -1;
    goto end;
  }
  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++) g_free(includemd5[n]);
  res = 0;

end:
  // we always write the device config to keep track of disabled devices
  dt_opencl_write_device_config(dev);

  free(infostr);
  free(cname);
  free(vendor);
  free(driverversion);
  free(deviceversion);

  free(dtcache);
  free(cachedir);
  free(devname);
  free(drvversion);
  free(platform_name);
  free(platform_vendor);

  free(filename);
  free(confentry);
  free(binname);

  return res;
}

#define RUNS 5

// If in GUI, the benchmarking array is running in a separate thread,
// but Gtk widgets need to be updated from the main thread.
// We need to wrap this function into g_main_context_invoke
// to make that happen.
static gboolean _opencl_update_progress(gpointer userdata)
{
  dt_opencl_t *cl = (dt_opencl_t *)userdata;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(cl->progress), cl->step / cl->steps);
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(cl->progress), cl->progress_label);
  return G_SOURCE_REMOVE;
}

static void dt_opencl_benchmark_array(dt_opencl_t *cl, const char *config, int width, int height, gboolean gui, int steps, int step)
{
  // Parametric sweep : N runs for each rounding for each device. CPU only does runs.
  const int inner_steps_for_outer_step = 3 * RUNS * cl->num_devs + RUNS;
  cl->steps = inner_steps_for_outer_step * steps; // total
  cl->step = step * inner_steps_for_outer_step; // current

  // CPU - no params sweep
  cl->cpubenchmark = 0.f;
  for(int _x = 0; _x < RUNS; _x++)
  {
    if(gui)
    {
      g_snprintf(cl->progress_label, sizeof(cl->progress_label), _("Benchmarking CPU @%i×%ipx..."), width, height);
      g_main_context_invoke(NULL, _opencl_update_progress, cl);
    }

    cl->cpubenchmark += dt_opencl_benchmark_cpu(width, height, 3, 100.0f);

    cl->step += 1.;
    if(gui) g_main_context_invoke(NULL, _opencl_update_progress, cl);
  }

  cl->cpubenchmark /= (float)RUNS;

  fprintf(stdout, "[OpenCL benchmark]: %s - CPU %f s\n", config, cl->cpubenchmark);
  dt_conf_set_float("dt_cpubenchmark", cl->cpubenchmark);

  // GPU
  int round_sizes[3] = { 16, 32, 64 };
  for(int n = 0; n < cl->num_devs; n++)
  {
    float results[3];

    for(int l = 0; l < 3; l++)
    {
      cl->dev[n].clroundup_ht = round_sizes[l];
      cl->dev[n].clroundup_wd = round_sizes[l];
      results[l] = 0.f;

      if(gui)
      {
        g_snprintf(cl->progress_label, sizeof(cl->progress_label), _("Benchmarking %s %s @%i×%ipx..."), cl->dev[n].vendor, cl->dev[n].name, width, height);
        g_main_context_invoke(NULL, _opencl_update_progress, cl);
      }

      for(int _x = 0; _x < RUNS; _x++)
      {
        results[l] += dt_opencl_benchmark_gpu(n, width, height, 3, 100.0f);
        cl->step += 1.;
        if(gui) g_main_context_invoke(NULL, _opencl_update_progress, cl);
      }

      results[l] /= (float)RUNS;

      fprintf(stdout, "[OpenCL benchmark]: %s %s %f s for rounding size %i\n", config, cl->dev[n].name,
              results[l], cl->dev[n].clroundup_ht);
    }

    // Find the best run for that device
    cl->dev[n].benchmark = FLT_MAX;

    for(int l = 0; l < 3; l++)
    {
      if(results[l] < cl->dev[n].benchmark)
      {
        cl->dev[n].benchmark = results[l];
        cl->dev[n].clroundup_ht = round_sizes[l];
        cl->dev[n].clroundup_wd = round_sizes[l];
      }
    }
    dt_opencl_write_device_config(n);
  }

  // Find the best run for all devices
  const float tcpu = cl->cpubenchmark;
  float tgpumin = INFINITY;
  float tgpumax = -INFINITY;
  int fastest_device = -1; // Device -1 is CPU
  for(int n = 0; n < cl->num_devs; n++)
  {
    if((cl->dev[n].benchmark > 0.0f) && (cl->dev[n].benchmark < tgpumin))
    {
      tgpumin = cl->dev[n].benchmark;
      fastest_device = n;
    }
    tgpumax = fmaxf(cl->dev[n].benchmark, tgpumax);
  }

  if(tcpu < tgpumin / 1.5f)
  {
    // CPU is much faster than GPU: disable GPU.
    dt_conf_set_string(config, "-1");
  }
  else if(tcpu > tgpumin)
  {
    // GPU is faster than CPU: force enable
    gchar *device_str = g_strdup_printf("+%i", fastest_device);
    dt_conf_set_string(config, device_str);
    g_free(device_str);
  }
  else
  {
    // GPU is on-par or slightly slower than CPU: still suggest it.
    // Reason is the most power-hungry algos are not in the benchmark,
    // and we know for a fact that OpenCL makes them faster.
    gchar *device_str = g_strdup_printf("%i", fastest_device);
    dt_conf_set_string(config, device_str);
    g_free(device_str);
  }

  // Timeouts: wait for available GPU for that amount of time.
  // Say CPU takes 2s to complete and GPU 1s,
  // we'd better wait for at most 1s for the GPU to be available.
  // Timeouts are expressed in increments of 5 ms
  dt_conf_set_int("opencl_mandatory_timeout", CLAMP((tcpu - tgpumin) / 0.005f, 100, 2000));

  // TODO: that should be deleted in the future
  dt_conf_set_int("pixelpipe_synchronization_timeout", 2 * MIN(tcpu, tgpumin) / 0.005f);
}

static gboolean _close_opencl_window(gpointer userdata)
{
  dt_opencl_t *cl = (dt_opencl_t *)userdata;
  gtk_widget_destroy(cl->dialog);
  return G_SOURCE_REMOVE;
}

gpointer dt_opencl_benchmark_sequence(dt_opencl_t *cl)
{
  // Get display size
  int width, height;
  gboolean gui = FALSE;

  if(cl->dialog != NULL)
  {
    // If GUI, use screen size for darkroom pipeline
    GdkDisplay *display = gdk_display_manager_get_default_display(gdk_display_manager_get());
    GdkMonitor *monitor = gdk_display_get_monitor_at_window(display, gtk_widget_get_window(cl->dialog));
    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    width = geometry.width * gdk_monitor_get_scale_factor(monitor);
    height = geometry.height * gdk_monitor_get_scale_factor(monitor);
    gui = TRUE;
  }
  else
  {
    width = 3840;
    height = 2160;
  }

  // Each of these globally overwrites size rounding factors and timeouts
  // So we leave the darkroom one for last since the most perf-critical
  // pipeline is when the user is editing in realtime.
  dt_opencl_benchmark_array(cl, "opencl_devid_thumbnail", 1920, 1200, gui, 4, 0);
  dt_opencl_benchmark_array(cl, "opencl_devid_preview", 1440, 900, gui, 4, 1);
  dt_opencl_benchmark_array(cl, "opencl_devid_export", 6144, 4096, gui, 4, 2);
  dt_opencl_benchmark_array(cl, "opencl_devid_darkroom", width, height, gui, 4, 3);

  if(gui) g_main_context_invoke(NULL, _close_opencl_window, cl);

  return NULL;
}

void dt_opencl_benchmark_window(dt_opencl_t *cl)
{
  // Create the widgets
  cl->dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(cl->dialog), _("Tuning OpenCL parameters..."));
  gtk_window_set_default_size(GTK_WINDOW(cl->dialog), DT_PIXEL_APPLY_DPI(600), DT_PIXEL_APPLY_DPI(400));
  gtk_window_set_icon_name(GTK_WINDOW(cl->dialog), "ansel");
  gtk_window_set_type_hint(GTK_WINDOW(cl->dialog), GDK_WINDOW_TYPE_HINT_DIALOG);

  GtkWindow *win = GTK_WINDOW(dt_ui_main_window(darktable.gui->ui));
  gtk_window_set_transient_for(GTK_WINDOW(cl->dialog), win);
  gtk_window_set_modal(GTK_WINDOW(cl->dialog), TRUE);

  GtkWidget *content_area = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_PIXEL_APPLY_DPI(20));
  gtk_container_add(GTK_CONTAINER(cl->dialog), content_area);

  cl->label = gtk_label_new(_("Ansel is looking for the optimal parameters to configure your GPU. It will take some time.\n\n"
                              "This happens when a new GPU is connected and when an OpenCL driver is updated."));
  gtk_label_set_line_wrap(GTK_LABEL(cl->label), TRUE);
  gtk_box_pack_start(GTK_BOX(content_area), cl->label, TRUE, TRUE, 0);

  cl->progress = gtk_progress_bar_new();
  gtk_progress_bar_set_text(GTK_PROGRESS_BAR(cl->progress), "");
  gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(cl->progress), TRUE);
  gtk_box_pack_start(GTK_BOX(content_area), cl->progress, TRUE, TRUE, 0);

  gtk_widget_show_all(cl->dialog);
  g_signal_connect(GTK_WINDOW(cl->dialog), "destroy", G_CALLBACK(gtk_main_quit), NULL);
  g_thread_new("dt_opencl_benchmark_sequence", (GThreadFunc)dt_opencl_benchmark_sequence, cl);
  gtk_main();
}

void dt_opencl_init(dt_opencl_t *cl, const gboolean exclude_opencl, const gboolean print_statistics)
{
  dt_pthread_mutex_init(&cl->lock, NULL);
  cl->inited = 0;
  cl->enabled = 0;
  cl->stopped = 0;
  cl->error_count = 0;
  cl->print_statistics = print_statistics;
  cl->progress = NULL;
  cl->dialog = NULL;
  cl->label = NULL;

  // work-around to fix a bug in some AMD OpenCL compilers, which would fail parsing certain numerical
  // constants if locale is different from "C".
  // we save the current locale, set locale to "C", and restore the previous setting after OpenCL is
  // initialized
  char *locale = strdup(setlocale(LC_ALL, NULL));
  setlocale(LC_ALL, "C");

  cl->crc = 5781;
  cl->dlocl = NULL;
  cl->dev_priority_image = NULL;
  cl->dev_priority_preview = NULL;
  cl->dev_priority_export = NULL;
  cl->dev_priority_thumbnail = NULL;

  if(exclude_opencl) return;

  cl_platform_id *all_platforms = NULL;
  cl_uint *all_num_devices = NULL;

  char *platform_name = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));
  char *platform_vendor = calloc(DT_OPENCL_CBUFFSIZE, sizeof(char));

  cl->cpubenchmark = dt_conf_get_float("dt_cpubenchmark");

  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] opencl related configuration options:\n");
  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] opencl: %s\n", dt_conf_get_bool("opencl") ? "ON" : "OFF" );
  // look for explicit definition of opencl_runtime library in preferences
  const char *library = dt_conf_get_string_const("opencl_library");
  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] opencl_library: '%s'\n", (strlen(library) == 0) ? "default path" : library);
  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] opencl_mandatory_timeout: %d\n",
           dt_conf_get_int("opencl_mandatory_timeout"));

  // dynamically load opencl runtime
  if((cl->dlocl = dt_dlopencl_init(library)) == NULL)
  {
    dt_print_nts(DT_DEBUG_OPENCL,
             "[opencl_init] no working opencl library found. Continue with opencl disabled\n");
    goto finally;
  }
  else
  {
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] opencl library '%s' found on your system and loaded\n",
             cl->dlocl->library);
  }

  cl_int err;
  all_platforms = malloc(sizeof(cl_platform_id) * DT_OPENCL_MAX_PLATFORMS);
  all_num_devices = malloc(sizeof(cl_uint) * DT_OPENCL_MAX_PLATFORMS);
  cl_uint num_platforms = DT_OPENCL_MAX_PLATFORMS;
  err = (cl->dlocl->symbols->dt_clGetPlatformIDs)(DT_OPENCL_MAX_PLATFORMS, all_platforms, &num_platforms);
  if(err != CL_SUCCESS)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] could not get platforms: %i\n", err);
    goto finally;
  }

  if(num_platforms == 0)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] no opencl platform available\n");
    goto finally;
  }
  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] found %d platform%s\n", num_platforms,
           num_platforms > 1 ? "s" : "");

  for(int n = 0; n < num_platforms; n++)
  {
    cl_platform_id platform = all_platforms[n];
    // get the number of GPU devices available to the platforms
    // the other common option is CL_DEVICE_TYPE_GPU/CPU (but the latter doesn't work with the nvidia drivers)
    err = (cl->dlocl->symbols->dt_clGetDeviceIDs)(platform, CL_DEVICE_TYPE_ALL, 0, NULL, &(all_num_devices[n]));
    if(err != CL_SUCCESS)
    {
      cl_int errv = (cl->dlocl->symbols->dt_clGetPlatformInfo)(platform, CL_PLATFORM_VENDOR, DT_OPENCL_CBUFFSIZE, platform_vendor, NULL);
      cl_int errn = (cl->dlocl->symbols->dt_clGetPlatformInfo)(platform, CL_PLATFORM_NAME, DT_OPENCL_CBUFFSIZE, platform_name, NULL);
      if((errn == CL_SUCCESS) && (errv == CL_SUCCESS))
        dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] no devices found for %s (vendor) - %s (name)\n", platform_vendor, platform_name);
      else
        dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] no devices found for unknown platform\n");

      all_num_devices[n] = 0;
    }
    else
    {
      char profile[64] = { 0 };
      size_t profile_size;
      err = (cl->dlocl->symbols->dt_clGetPlatformInfo)(platform, CL_PLATFORM_PROFILE, 64, profile, &profile_size);
      if(err != CL_SUCCESS)
      {
        all_num_devices[n] = 0;
        dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] could not get profile: %i\n", err);
      }
      else
      {
        // fprintf(stderr, "%s\n", profile);
        if(strcmp("FULL_PROFILE", profile) != 0)
        {
          all_num_devices[n] = 0;
          dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] platform %i is not FULL_PROFILE\n", n);
        }
      }
    }
  }

  cl_uint num_devices = 0;
  for(int n = 0; n < num_platforms; n++) num_devices += all_num_devices[n];

  // create the device list
  cl_device_id *devices = 0;
  if(num_devices)
  {
    cl->dev = (dt_opencl_device_t *)malloc(sizeof(dt_opencl_device_t) * num_devices);
    devices = (cl_device_id *)malloc(sizeof(cl_device_id) * num_devices);
    if(!cl->dev || !devices)
    {
      free(cl->dev);
      cl->dev = NULL;
      free(devices);
      dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] could not allocate memory\n");
      goto finally;
    }
  }

  cl_device_id *devs = devices;
  for(int n = 0; n < num_platforms; n++)
  {
    if(all_num_devices[n])
    {
      cl_platform_id platform = all_platforms[n];
      err = (cl->dlocl->symbols->dt_clGetDeviceIDs)(platform, CL_DEVICE_TYPE_ALL, all_num_devices[n], devs,
                                                    NULL);
      if(err != CL_SUCCESS)
      {
        num_devices -= all_num_devices[n];
        dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] could not get devices list: %i\n", err);
      }
      devs += all_num_devices[n];
    }
  }
  devs = NULL;

  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] found %d device%s\n", num_devices, num_devices > 1 ? "s" : "");
  if(num_devices == 0)
  {
    if(devices) free(devices);
    goto finally;
  }

  int dev = 0;
  for(int k = 0; k < num_devices; k++)
  {
    const int res = dt_opencl_device_init(cl, dev, devices, k);
    if(res != 0)
      continue;
    // increase dev only if dt_opencl_device_init was successful (res == 0)
    ++dev;
  }
  free(devices);
  devices = NULL;

  if(dev > 0)
  {
    cl->num_devs = dev;
    cl->inited = 1;
    cl->enabled = dt_conf_get_bool("opencl");
    memset(cl->mandatory, 0, sizeof(cl->mandatory));
    cl->dev_priority_image = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_preview = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_export = (int *)malloc(sizeof(int) * (dev + 1));
    cl->dev_priority_thumbnail = (int *)malloc(sizeof(int) * (dev + 1));

    // only check successful malloc in debug mode; darktable will crash anyhow sooner or later if mallocs that
    // small would fail
    assert(cl->dev_priority_image != NULL && cl->dev_priority_preview != NULL
           && cl->dev_priority_export != NULL && cl->dev_priority_thumbnail != NULL);

    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] OpenCL successfully initialized.\n");
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] here are the internal numbers and names of OpenCL devices available to Ansel:\n");
    for(int i = 0; i < dev; i++) dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init]\t\t%d\t'%s'\n", i, cl->dev[i].name);
  }
  else
  {
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] no suitable devices found.\n");
  }

finally:
  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] FINALLY: opencl is %sAVAILABLE on this system.\n",
           cl->inited ? "" : "NOT ");
  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] initial status of opencl enabled flag is %s.\n",
           cl->enabled ? "ON" : "OFF");

  // check if the list of existing OpenCL devices (indicated by checksum != oldchecksum) has changed
  // If it has, reprofile and update config if needed.
  char checksum[64];
  snprintf(checksum, sizeof(checksum), "%u", cl->crc);
  const char *oldchecksum = dt_conf_get_string_const("opencl_checksum");
  const gboolean manually = strcasecmp(oldchecksum, "OFF") == 0;
  const gboolean newcheck = ((strcmp(oldchecksum, checksum) != 0) || (strlen(oldchecksum) < 1));

  if(cl->inited)
  {
    dt_capabilities_add("opencl");
    cl->blendop = dt_develop_blend_init_cl_global();
    cl->bilateral = dt_bilateral_init_cl_global();
    cl->gaussian = dt_gaussian_init_cl_global();
    cl->interpolation = dt_interpolation_init_cl_global();
    cl->local_laplacian = dt_local_laplacian_init_cl_global();
    cl->dwt = dt_dwt_init_cl_global();
    cl->heal = dt_heal_init_cl_global();
    cl->colorspaces = dt_colorspaces_init_cl_global();
    cl->guided_filter = dt_guided_filter_init_cl_global();
  }

  if((newcheck && !manually) && cl->inited)
  {
    dt_print_nts(DT_DEBUG_OPENCL, "[opencl_init] OpenCL devices changed, we will update the profiling configuration.\n");
    dt_conf_set_string("opencl_checksum", checksum);

    if(darktable.gui) // use the GUI path only if there is a graphical session
      dt_opencl_benchmark_window(cl);
    else
      dt_opencl_benchmark_sequence(cl);
  }

  dt_opencl_apply_scheduling_profile();

  if(!cl->inited)// initialization failed
  {
    for(int i = 0; cl->dev && i < cl->num_devs; i++) dt_opencl_cleanup_device(cl, i);
  }

  free(all_num_devices);
  free(all_platforms);
  free(platform_name);
  free(platform_vendor);

  if(locale)
  {
    setlocale(LC_ALL, locale);
    free(locale);
  }

  return;
}

void dt_opencl_cleanup_device(dt_opencl_t *cl, int i)
{
  dt_pthread_mutex_destroy(&cl->dev[i].lock);
  for(int k = 0; k < DT_OPENCL_MAX_KERNELS; k++)
    if(cl->dev[i].kernel_used[k]) (cl->dlocl->symbols->dt_clReleaseKernel)(cl->dev[i].kernel[k]);
  for(int k = 0; k < DT_OPENCL_MAX_PROGRAMS; k++)
    if(cl->dev[i].program_used[k]) (cl->dlocl->symbols->dt_clReleaseProgram)(cl->dev[i].program[k]);
  (cl->dlocl->symbols->dt_clReleaseCommandQueue)(cl->dev[i].cmd_queue);
  (cl->dlocl->symbols->dt_clReleaseContext)(cl->dev[i].context);

  if(cl->print_statistics && (darktable.unmuted & DT_DEBUG_MEMORY))
  {
    dt_print_nts(DT_DEBUG_OPENCL, " [opencl_summary_statistics] device '%s' (%d): peak memory usage %zu bytes (%.1f MB)\n",
                cl->dev[i].name, i, cl->dev[i].peak_memory, (float)cl->dev[i].peak_memory/(1024*1024));
  }

  if(cl->print_statistics && cl->dev[i].use_events)
  {
    if(cl->dev[i].totalevents)
    {
      dt_print_nts(DT_DEBUG_OPENCL, " [opencl_summary_statistics] device '%s' (%d): %d out of %d events were "
                                "successful and %d events lost. max event=%d%s\n",
        cl->dev[i].name, i, cl->dev[i].totalsuccess, cl->dev[i].totalevents, cl->dev[i].totallost,
        cl->dev[i].maxeventslot, (cl->dev[i].maxeventslot > 1024) ? "\n *** Warning, slots > 1024" : "");
    }
    else
    {
      dt_print_nts(DT_DEBUG_OPENCL, " [opencl_summary_statistics] device '%s' (%d): NOT utilized\n",
                cl->dev[i].name, i);
    }
  }

  if(cl->dev[i].use_events)
  {
    dt_opencl_events_reset(i);

    free(cl->dev[i].eventlist);
    free(cl->dev[i].eventtags);
  }

  free((void *)(cl->dev[i].vendor));
  free((void *)(cl->dev[i].name));
  free((void *)(cl->dev[i].cname));
  free((void *)(cl->dev[i].options));
}

void dt_opencl_cleanup(dt_opencl_t *cl)
{
  if(cl->inited)
  {
    dt_develop_blend_free_cl_global(cl->blendop);
    dt_bilateral_free_cl_global(cl->bilateral);
    dt_gaussian_free_cl_global(cl->gaussian);
    dt_interpolation_free_cl_global(cl->interpolation);
    dt_dwt_free_cl_global(cl->dwt);
    dt_heal_free_cl_global(cl->heal);
    dt_colorspaces_free_cl_global(cl->colorspaces);
    dt_guided_filter_free_cl_global(cl->guided_filter);

    for(int i = 0; i < cl->num_devs; i++)
      dt_opencl_cleanup_device(cl, i);

    free(cl->dev_priority_image);
    free(cl->dev_priority_preview);
    free(cl->dev_priority_export);
    free(cl->dev_priority_thumbnail);
  }

  if(cl->dlocl)
  {
    free(cl->dlocl->symbols);
    g_free(cl->dlocl->library);
    free(cl->dlocl);
  }

  free(cl->dev);
  dt_pthread_mutex_destroy(&cl->lock);
}

static const char *dt_opencl_get_vendor_by_id(unsigned int id)
{
  const char *vendor;

  switch(id)
  {
    case DT_OPENCL_VENDOR_AMD:
      vendor = "AMD";
      break;
    case DT_OPENCL_VENDOR_NVIDIA:
      vendor = "NVIDIA";
      break;
    case DT_OPENCL_VENDOR_INTEL:
      vendor = "INTEL";
      break;
    default:
      vendor = "UNKNOWN";
  }

  return vendor;
}

static float dt_opencl_benchmark_gpu(const int devid, const size_t width, const size_t height, const int count, const float sigma)
{
  const int bpp = 4 * sizeof(float);
  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  cl_mem dev_mem = NULL;
  cl_mem dev_in = NULL;
  cl_mem mem_out = NULL;
  float *buf = NULL;
  dt_gaussian_cl_t *g = NULL;
  dt_guided_filter_cl_global_t *gf = NULL;

  const float Labmax[] = { INFINITY, INFINITY, INFINITY, INFINITY };
  const float Labmin[] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

  unsigned int *const tea_states = alloc_tea_states(darktable.num_openmp_threads);

  // Simulate a 24 Mpx raw
  buf = dt_pixelpipe_cache_alloc_align_cache( 6144 * 4096 * bpp, 0);
  if(buf == NULL) goto error;

  // Write noise in the raw image
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(tea_states, buf)
#endif
  for(size_t j = 0; j < 4096; j++)
  {
    unsigned int *tea_state = get_tea_state(tea_states,dt_get_thread_num());
    tea_state[0] = j + dt_get_thread_num();
    size_t index = j * 4 * 6144;
    for(int i = 0; i < 4 * 6144; i++)
    {
      encrypt_tea(tea_state);
      buf[index + i] = 100.0f * tpdf(tea_state[0]);
    }
  }

  // start timer
  double start = dt_get_wtime();

  // Allocate dev_in buffer & copy fake data from RAM to vRAM
  // We take I/O cost into account in the timer because
  // OpenCL pipelines have a lot of overhead.
  dev_in = dt_opencl_copy_host_to_device(devid, buf, 6144, 4096, bpp);
  if(dev_in == NULL) goto error;

  dev_mem = dt_opencl_alloc_device(devid, width, height, bpp);
  if(dev_mem == NULL) goto error;

  // simulate "demosaicing" a 24 Mpx raw, aka interpolation
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_LANCZOS3);
  dt_iop_roi_t roi_in = { .height = 4096, .width = 6144, .x = 0, .y = 0 };
  dt_iop_roi_t roi_out = { .height = height, .width = width, .x = 0, .y = 0 };
  dt_interpolation_resample_cl(itor, devid, dev_mem, &roi_out, dev_in, &roi_in);

  // prepare gaussian filter
  g = dt_gaussian_init_cl(devid, width, height, 4, Labmax, Labmin, sigma, 0);
  if(!g) goto error;

  // gaussian blur
  for(int n = 0; n < count; n++)
  {
    err = dt_gaussian_blur_cl(g, dev_mem, dev_mem);
    if(err != CL_SUCCESS) goto error;

    // copy back to host
    err = dt_opencl_copy_device_to_host(devid, buf, dev_mem, width, height, bpp);
    if(err != CL_SUCCESS) goto error;
  }

  // cleanup gaussian filter
  dt_gaussian_free_cl(g);
  g = NULL;

  // prepare guided filter
  gf = dt_guided_filter_init_cl_global();
  mem_out = dt_opencl_alloc_device(devid, width, height, bpp);
  if(!mem_out) goto error;

  for(int n = 0; n < count; n++)
  {
    guided_filter_cl(devid, dev_mem, dev_mem, mem_out, width, height, 4, sigma, 1.0, 0.5, 0., 1.0);

    // copy back to host
    err = dt_opencl_copy_device_to_host(devid, buf, mem_out, width, height, bpp);
    if(err != CL_SUCCESS) goto error;
  }

  // cleanup guided filter
  dt_guided_filter_free_cl_global(gf);
  gf = NULL;

  // end timer
  double end = dt_get_wtime();

  // free dev_mem
  dt_pixelpipe_cache_free_align(buf);
  free_tea_states(tea_states);
  dt_opencl_release_mem_object(dev_mem);
  dt_opencl_release_mem_object(mem_out);
  dt_opencl_release_mem_object(dev_in);
  return (end - start);

error:
  if(g) dt_gaussian_free_cl(g);
  if(buf) dt_pixelpipe_cache_free_align(buf);
  free_tea_states(tea_states);

  if(gf) dt_guided_filter_free_cl_global(gf);
  dt_opencl_release_mem_object(dev_mem);
  dt_opencl_release_mem_object(mem_out);
  dt_opencl_release_mem_object(dev_in);
  return INFINITY;
}

static float dt_opencl_benchmark_cpu(const size_t width, const size_t height, const int count, const float sigma)
{
  const int bpp = 4 * sizeof(float);

  float *buf = dt_pixelpipe_cache_alloc_align_cache( width * height * bpp, 0);

  const float Labmax[] = { INFINITY, INFINITY, INFINITY, INFINITY };
  const float Labmin[] = { -INFINITY, -INFINITY, -INFINITY, -INFINITY };

  unsigned int *const tea_states = alloc_tea_states(darktable.num_openmp_threads);

  float *out = dt_pixelpipe_cache_alloc_align_cache( width * height * bpp, 0);

  // Fake raw
  float *in = dt_pixelpipe_cache_alloc_align_cache( 6144 * 4096 * bpp, 0);

  // Write noise in the fake raw
#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(tea_states, in)
#endif
  for(size_t j = 0; j < 4096; j++)
  {
    unsigned int *tea_state = get_tea_state(tea_states,dt_get_thread_num());
    tea_state[0] = j + dt_get_thread_num();
    size_t index = j * 4 * 6144;
    for(int i = 0; i < 4 * 6144; i++)
    {
      encrypt_tea(tea_state);
      in[index + i] = 100.0f * tpdf(tea_state[0]);
    }
  }

  // start timer
  double start = dt_get_wtime();

  // Simulate "demosaicing" a 24 Mpx raw, aka interpolation
  const struct dt_interpolation *itor = dt_interpolation_new(DT_INTERPOLATION_LANCZOS3);
  dt_iop_roi_t roi_in = { .height = 4096, .width = 6144, .x = 0, .y = 0 };
  dt_iop_roi_t roi_out = { .height = height, .width = width, .x = 0, .y = 0 };
  dt_interpolation_resample(itor, out, &roi_out, in, &roi_in);

  // prepare gaussian filter
  dt_gaussian_t *g = dt_gaussian_init(width, height, 4, Labmax, Labmin, sigma, 0);

  for(int n = 0; n < count; n++)
    dt_gaussian_blur(g, buf, buf);

  // cleanup gaussian filter
  dt_gaussian_free(g);

  // guided filter
  for(int n = 0; n < count; n++)
    guided_filter(buf, buf, out, width, height, 4, sigma, 0.05, 0.5, 0., FLT_MAX);

  // end timer
  double end = dt_get_wtime();

  dt_pixelpipe_cache_free_align(buf);
  dt_pixelpipe_cache_free_align(out);
  dt_pixelpipe_cache_free_align(in);
  free_tea_states(tea_states);

  return (end - start);
}

gboolean dt_opencl_finish(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;

  cl_int err = (cl->dlocl->symbols->dt_clFinish)(cl->dev[devid].cmd_queue);

  // take the opportunity to release some event handles, but without printing
  // summary statistics
  cl_int success = dt_opencl_events_flush(devid, 0);

  return (err == CL_SUCCESS && success == CL_COMPLETE);
}

int dt_opencl_enqueue_barrier(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return -1;
  return (cl->dlocl->symbols->dt_clEnqueueBarrier)(cl->dev[devid].cmd_queue);
}

static int _take_from_list(int *list, int value)
{
  int result = -1;

  while(*list != -1 && *list != value) list++;
  result = *list;

  while(*list != -1)
  {
    *list = *(list + 1);
    list++;
  }

  return result;
}


static int _device_by_cname(const char *name)
{
  dt_opencl_t *cl = darktable.opencl;
  int devs = cl->num_devs;
  char tmp[2048] = { 0 };
  int result = -1;

  _ascii_str_canonical(name, tmp, sizeof(tmp));

  for(int i = 0; i < devs; i++)
  {
    if(!strcmp(tmp, cl->dev[i].cname))
    {
      result = i;
      break;
    }
  }

  return result;
}


static char *_ascii_str_canonical(const char *in, char *out, int maxlen)
{
  if(out == NULL)
  {
    maxlen = strlen(in) + 1;
    out = malloc(maxlen);
    if(out == NULL) return NULL;
  }

  int len = 0;

  while(*in != '\0' && len < maxlen - 1)
  {
    int n = strcspn(in, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ");
    in += n;
    if(n != 0) continue;
    out[len] = tolower(*in);
    len++;
    in++;
  }
  out[len] = '\0';

  return out;
}

// parse a single token of priority string and store priorities in priority_list
static void dt_opencl_priority_parse(dt_opencl_t *cl, char *configstr, int *priority_list, int *mandatory)
{
  int devs = cl->num_devs;
  int count = 0;
  int *full = malloc(sizeof(int) * (devs + 1));
  int mnd = 0;

  // NULL or empty configstring?
  if(configstr == NULL || *configstr == '\0')
  {
    priority_list[0] = -1;
    *mandatory = 0;
    free(full);
    return;
  }

  // check if user wants us to force-use opencl device(s)
  if(configstr[0] == '+')
  {
    mnd = 1;
    configstr++;
  }

  // first start with a full list of devices to take from
  for(int i = 0; i < devs; i++) full[i] = i;
  full[devs] = -1;

  gchar **tokens = g_strsplit(configstr, ",", 0);
  gchar **tokens_ptr = tokens;

  while(tokens != NULL && *tokens_ptr != NULL && count < devs + 1 && full[0] != -1)
  {
    gchar *str = *tokens_ptr;
    int not = 0;
    int all = 0;

    switch(*str)
    {
      case '*':
        all = 1;
        break;
      case '!':
        not = 1;
        while(*str == '!') str++;
        break;
    }

    if(all)
    {
      // copy all remaining device numbers from full to priority list
      for(int i = 0; i < devs && full[i] != -1; i++)
      {
        priority_list[count] = full[i];
        count++;
      }
      full[0] = -1; // mark full list as empty
    }
    else if(*str != '\0')
    {
      char *endptr = NULL;

      // first check if str corresponds to an existing canonical device name
      long number = _device_by_cname(str);

      // if not try to convert string into decimal device number
      if(number < 0) number = strtol(str, &endptr, 10);

      // still not found or negative number given? set number to -1
      if(number < 0 || (number == 0 && endptr == str)) number = -1;

      // try to take number out of remaining device list
      int dev_number = _take_from_list(full, number);

      if(!not&&dev_number != -1)
      {
        priority_list[count] = dev_number;
        count++;
      }
    }

    tokens_ptr++;
  }

  g_strfreev(tokens);

  // terminate priority list with -1
  while(count < devs + 1) priority_list[count++] = -1;

  // opencl use can only be mandatory if at least one opencl device is given
  *mandatory = (priority_list[0] != -1) ? mnd : 0;

  free(full);
}

// set device priorities according to config string
static void dt_opencl_update_priorities()
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;

  // Priority parsing iterates over the list of available devices.
  // If !cl->inited, that means we have no available device, so empty list.
  // Exit early of face a segfault
  dt_opencl_priority_parse(cl, dt_conf_get_string("opencl_devid_darkroom"), cl->dev_priority_image, &cl->mandatory[0]);
  dt_opencl_priority_parse(cl, dt_conf_get_string("opencl_devid_preview"), cl->dev_priority_preview, &cl->mandatory[1]);
  dt_opencl_priority_parse(cl, dt_conf_get_string("opencl_devid_export"), cl->dev_priority_export, &cl->mandatory[2]);
  dt_opencl_priority_parse(cl, dt_conf_get_string("opencl_devid_thumbnail"), cl->dev_priority_thumbnail, &cl->mandatory[3]);

  dt_print_nts(DT_DEBUG_OPENCL, "[dt_opencl_update_priorities] these are your device priorities:\n");
  dt_print_nts(DT_DEBUG_OPENCL, "[dt_opencl_update_priorities] \tid |\t\tIMAGE\tPREVIEW\tEXPORT\tTHUMBS\n");
  for(int i = 0; i < cl->num_devs; i++)
    dt_print_nts(DT_DEBUG_OPENCL, "[dt_opencl_update_priorities]\t%i |\t\t%d\t%d\t%d\t%d\n",
                 i, cl->dev_priority_image[i],
                 cl->dev_priority_preview[i], cl->dev_priority_export[i], cl->dev_priority_thumbnail[i]);
  dt_print_nts(DT_DEBUG_OPENCL, "[dt_opencl_update_priorities] show if opencl use is mandatory for a given pixelpipe:\n");
  dt_print_nts(DT_DEBUG_OPENCL, "[dt_opencl_update_priorities] \t\tIMAGE\tPREVIEW\tEXPORT\tTHUMBS\n");
  dt_print_nts(DT_DEBUG_OPENCL, "[dt_opencl_update_priorities]\t\t%s\t%s\t%s\t%s\n", cl->mandatory[0] ? "yes" : "no",
             cl->mandatory[1] ? "yes" : "no", cl->mandatory[2] ? "yes" : "no", cl->mandatory[3] ? "yes" : "no");
}

int dt_opencl_lock_device(const int pipetype)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return -1;


  dt_pthread_mutex_lock(&cl->lock);

  size_t prio_size = sizeof(int) * (cl->num_devs + 1);
  int *priority = (int *)malloc(prio_size);
  int mandatory;

  switch(pipetype)
  {
    case DT_DEV_PIXELPIPE_FULL:
      memcpy(priority, cl->dev_priority_image, prio_size);
      mandatory = cl->mandatory[0];
      break;
    case DT_DEV_PIXELPIPE_PREVIEW:
      memcpy(priority, cl->dev_priority_preview, prio_size);
      mandatory = cl->mandatory[1];
      break;
    case DT_DEV_PIXELPIPE_EXPORT:
      memcpy(priority, cl->dev_priority_export, prio_size);
      mandatory = cl->mandatory[2];
      break;
    case DT_DEV_PIXELPIPE_THUMBNAIL:
      memcpy(priority, cl->dev_priority_thumbnail, prio_size);
      mandatory = cl->mandatory[3];
      break;
    default:
      free(priority);
      priority = NULL;
      mandatory = 0;
  }

  dt_pthread_mutex_unlock(&cl->lock);

  if(priority)
  {
    const int usec = 5000;
    const int nloop = MAX(0, dt_conf_get_int("opencl_mandatory_timeout"));

    // check for free opencl device repeatedly if mandatory is TRUE, else give up after first try
    for(int n = 0; n < nloop; n++)
    {
      const int *prio = priority;

      while(*prio != -1)
      {
        if(!dt_pthread_mutex_BAD_trylock(&cl->dev[*prio].lock))
        {
          int devid = *prio;
          free(priority);
          return devid;
        }
        prio++;
      }

      if(!mandatory)
      {
        free(priority);
        return -1;
      }

      dt_iop_nap(usec);
    }
    dt_print(DT_DEBUG_OPENCL, "[opencl_lock_device] reached opencl_mandatory_timeout trying to lock mandatory device, fallback to CPU\n");
  }
  else
  {
    // only a fallback if a new pipe type would be added and we forget to take care of it in opencl.c
    for(int try_dev = 0; try_dev < cl->num_devs; try_dev++)
    {
      // get first currently unused processor
      if(!dt_pthread_mutex_BAD_trylock(&cl->dev[try_dev].lock)) return try_dev;
    }
  }

  free(priority);

  // no free GPU :(
  // use CPU processing, if no free device:
  return -1;
}

void dt_opencl_unlock_device(const int dev)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;
  if(dev < 0 || dev >= cl->num_devs) return;
  dt_pthread_mutex_BAD_unlock(&cl->dev[dev].lock);
}

static FILE *fopen_stat(const char *filename, struct stat *st)
{
  FILE *f = g_fopen(filename, "rb");
  if(!f)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_fopen_stat] could not open file `%s'!\n", filename);
    return NULL;
  }
  int fd = fileno(f);
  if(fstat(fd, st) < 0)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_fopen_stat] could not stat file `%s'!\n", filename);
    return NULL;
  }
  return f;
}


void dt_opencl_md5sum(const char **files, char **md5sums)
{
  char kerneldir[PATH_MAX] = { 0 };
  char filename[PATH_MAX] = { 0 };
  dt_loc_get_kerneldir(kerneldir, sizeof(kerneldir));

  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++, files++, md5sums++)
  {
    if(!*files)
    {
      *md5sums = NULL;
      continue;
    }

    snprintf(filename, sizeof(filename), "%s" G_DIR_SEPARATOR_S "%s", kerneldir, *files);

    struct stat filestat;
    FILE *f = fopen_stat(filename, &filestat);

    if(!f)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_md5sums] could not open file `%s'!\n", filename);
      *md5sums = NULL;
      continue;
    }

    size_t filesize = filestat.st_size;
    char *file = (char *)malloc(filesize);

    if(!file)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_md5sums] could not allocate buffer for file `%s'!\n", filename);
      *md5sums = NULL;
      fclose(f);
      continue;
    }

    size_t rd = fread(file, sizeof(char), filesize, f);
    fclose(f);

    if(rd != filesize)
    {
      free(file);
      dt_print(DT_DEBUG_OPENCL, "[opencl_md5sums] could not read all of file `%s'!\n", filename);
      *md5sums = NULL;
      continue;
    }

    *md5sums = g_compute_checksum_for_data(G_CHECKSUM_MD5, (guchar *)file, filesize);

    free(file);
  }
}

int dt_opencl_load_program(const int dev, const int prog, const char *filename, const char *binname,
                           const char *cachedir, char *md5sum, char **includemd5, int *loaded_cached)
{
  cl_int err;
  dt_opencl_t *cl = darktable.opencl;

  struct stat filestat, cachedstat;
  *loaded_cached = 0;

  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_source] invalid program number `%d' of file `%s'!\n", prog,
             filename);
    return 0;
  }

  if(cl->dev[dev].program_used[prog])
  {
    dt_print(DT_DEBUG_OPENCL,
             "[opencl_load_source] program number `%d' already in use when loading file `%s'!\n", prog,
             filename);
    return 0;
  }

  FILE *f = fopen_stat(filename, &filestat);
  if(!f) return 0;

  size_t filesize = filestat.st_size;
  char *file = (char *)malloc(filesize + 2048);
  size_t rd = fread(file, sizeof(char), filesize, f);
  fclose(f);
  if(rd != filesize)
  {
    free(file);
    dt_print(DT_DEBUG_OPENCL, "[opencl_load_source] could not read all of file `%s'!\n", filename);
    return 0;
  }

  char *start = file + filesize;
  char *end = start + 2048;
  size_t len;

  cl_device_id devid = cl->dev[dev].devid;
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DRIVER_VERSION, end - start, start, &len);
  start += len;

  cl_platform_id platform;
  (cl->dlocl->symbols->dt_clGetDeviceInfo)(devid, CL_DEVICE_PLATFORM, sizeof(cl_platform_id), &platform, NULL);

  (cl->dlocl->symbols->dt_clGetPlatformInfo)(platform, CL_PLATFORM_VERSION, end - start, start, &len);
  start += len;

  len = g_strlcpy(start, cl->dev[dev].options, end - start);
  start += len;

  /* make sure that the md5sums of all the includes are applied as well */
  for(int n = 0; n < DT_OPENCL_MAX_INCLUDES; n++)
  {
    if(!includemd5[n]) continue;
    len = g_strlcpy(start, includemd5[n], end - start);
    start += len;
  }

  char *source_md5 = g_compute_checksum_for_data(G_CHECKSUM_MD5, (guchar *)file, start - file);
  g_strlcpy(md5sum, source_md5, 33);
  g_free(source_md5);

  file[filesize] = '\0';

  char linkedfile[PATH_MAX] = { 0 };
  ssize_t linkedfile_len = 0;

#if defined(_WIN32)
  // No symlinks on Windows
  // Have to figure out the name using the filename + md5sum
  char dup[PATH_MAX] = { 0 };
  snprintf(dup, sizeof(dup), "%s.%s", binname, md5sum);
  FILE *cached = fopen_stat(dup, &cachedstat);
  g_strlcpy(linkedfile, md5sum, sizeof(linkedfile));
  linkedfile_len = strlen(md5sum);
#else
  FILE *cached = fopen_stat(binname, &cachedstat);
#endif

  if(cached)
  {
#if !defined(_WIN32)
    linkedfile_len = readlink(binname, linkedfile, sizeof(linkedfile) - 1);
#endif // !defined(_WIN32)
    if(linkedfile_len > 0)
    {
      linkedfile[linkedfile_len] = '\0';

      if(strncmp(linkedfile, md5sum, 33) == 0)
      {
        // md5sum matches, load cached binary
        size_t cached_filesize = cachedstat.st_size;

        unsigned char *cached_content = (unsigned char *)malloc(cached_filesize + 1);
        rd = fread(cached_content, sizeof(char), cached_filesize, cached);
        if(rd != cached_filesize)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_load_program] could not read all of file '%s' MD5: %s!\n", binname, md5sum);
        }
        else
        {
          cl->dev[dev].program[prog] = (cl->dlocl->symbols->dt_clCreateProgramWithBinary)(
              cl->dev[dev].context, 1, &(cl->dev[dev].devid), &cached_filesize,
              (const unsigned char **)&cached_content, NULL, &err);
          if(err != CL_SUCCESS)
          {
            dt_print(DT_DEBUG_OPENCL,
                     "[opencl_load_program] could not load cached binary program from file '%s' MD5: '%s'! (%i)\n",
                     binname, md5sum, err);
          }
          else
          {
            cl->dev[dev].program_used[prog] = 1;
            *loaded_cached = 1;
          }
        }
        free(cached_content);
      }
    }
    fclose(cached);
  }


  if(*loaded_cached == 0)
  {
    // if loading cached was unsuccessful for whatever reason,
    // try to remove cached binary & link
#if !defined(_WIN32)
    if(linkedfile_len > 0)
    {
      char link_dest[PATH_MAX] = { 0 };
      snprintf(link_dest, sizeof(link_dest), "%s" G_DIR_SEPARATOR_S "%s", cachedir, linkedfile);
      g_unlink(link_dest);
    }
    g_unlink(binname);
#else
    // delete the file which contains the MD5 name
    g_unlink(dup);
#endif //!defined(_WIN32)

    dt_print(DT_DEBUG_OPENCL,
             "[opencl_load_program] could not load cached binary program, trying to compile source\n");

    cl->dev[dev].program[prog] = (cl->dlocl->symbols->dt_clCreateProgramWithSource)(
        cl->dev[dev].context, 1, (const char **)&file, &filesize, &err);
    free(file);
    if((err != CL_SUCCESS) || (cl->dev[dev].program[prog] == NULL))
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_load_source] could not create program from file `%s'! (%i)\n",
               filename, err);
      return 0;
    }
    else
    {
      cl->dev[dev].program_used[prog] = 1;
    }
  }
  else
  {
    free(file);
    dt_vprint(DT_DEBUG_OPENCL, "[opencl_load_program] loaded cached binary program from file '%s' MD5: '%s' \n", binname, md5sum);
  }

  dt_vprint(DT_DEBUG_OPENCL, "[opencl_load_program] successfully loaded program from '%s' MD5: '%s'\n", filename, md5sum);

  return 1;
}

int dt_opencl_build_program(const int dev, const int prog, const char *binname, const char *cachedir,
                            char *md5sum, int loaded_cached)
{
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
  dt_opencl_t *cl = darktable.opencl;
  cl_program program = cl->dev[dev].program[prog];
  cl_int err = (cl->dlocl->symbols->dt_clBuildProgram)(program, 1, &(cl->dev[dev].devid), cl->dev[dev].options, 0, 0);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] could not build program: %i\n", err);
  else
    dt_vprint(DT_DEBUG_OPENCL, "[opencl_build_program] successfully built program\n");

  cl_build_status build_status;
  (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_STATUS,
                                                 sizeof(cl_build_status), &build_status, NULL);
  dt_vprint(DT_DEBUG_OPENCL, "[opencl_build_program] BUILD STATUS: %d\n", build_status);

  char *build_log;
  size_t ret_val_size;
  (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_LOG, 0, NULL,
                                                 &ret_val_size);
  if(ret_val_size != SIZE_MAX)
  {
    build_log = (char *)malloc(sizeof(char) * (ret_val_size + 1));
    if(build_log)
    {
      (cl->dlocl->symbols->dt_clGetProgramBuildInfo)(program, cl->dev[dev].devid, CL_PROGRAM_BUILD_LOG,
                                                     ret_val_size, build_log, NULL);

      build_log[ret_val_size] = '\0';

      dt_vprint(DT_DEBUG_OPENCL, "BUILD LOG:\n");
      dt_vprint(DT_DEBUG_OPENCL, "%s\n", build_log);

      free(build_log);
    }
  }

  if(err != CL_SUCCESS)
    return err;
  else
  {
    if(!loaded_cached)
    {
      dt_vprint(DT_DEBUG_OPENCL, "[opencl_build_program] saving binary\n");

      cl_uint numdev = 0;
      err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_NUM_DEVICES, sizeof(cl_uint),
                                                      &numdev, NULL);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] CL_PROGRAM_NUM_DEVICES failed: %i\n", err);
        return CL_SUCCESS;
      }

      cl_device_id *devices = malloc(sizeof(cl_device_id) * numdev);
      err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_DEVICES,
                                                      sizeof(cl_device_id) * numdev, devices, NULL);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] CL_PROGRAM_DEVICES failed: %i\n", err);
        free(devices);
        return CL_SUCCESS;
      }

      size_t *binary_sizes = malloc(sizeof(size_t) * numdev);
      err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_BINARY_SIZES,
                                                      sizeof(size_t) * numdev, binary_sizes, NULL);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] CL_PROGRAM_BINARY_SIZES failed: %i\n", err);
        free(binary_sizes);
        free(devices);
        return CL_SUCCESS;
      }

      unsigned char **binaries = malloc(sizeof(unsigned char *) * numdev);
      for(int i = 0; i < numdev; i++) binaries[i] = (unsigned char *)malloc(binary_sizes[i]);
      err = (cl->dlocl->symbols->dt_clGetProgramInfo)(program, CL_PROGRAM_BINARIES,
                                                      sizeof(unsigned char *) * numdev, binaries, NULL);
      if(err != CL_SUCCESS)
      {
        dt_print(DT_DEBUG_OPENCL, "[opencl_build_program] CL_PROGRAM_BINARIES failed: %i\n", err);
        goto ret;
      }

      for(int i = 0; i < numdev; i++)
        if(cl->dev[dev].devid == devices[i])
        {
          // save opencl compiled binary as md5sum-named file
          char link_dest[PATH_MAX] = { 0 };
          snprintf(link_dest, sizeof(link_dest), "%s" G_DIR_SEPARATOR_S "%s", cachedir, md5sum);
          FILE *f = g_fopen(link_dest, "wb");
          if(!f) goto ret;
          size_t bytes_written = fwrite(binaries[i], sizeof(char), binary_sizes[i], f);
          if(bytes_written != binary_sizes[i]) goto ret;
          fclose(f);

          // create link (e.g. basic.cl.bin -> f1430102c53867c162bb60af6c163328)
          char cwd[PATH_MAX] = { 0 };
          if(!getcwd(cwd, sizeof(cwd))) goto ret;
          if(chdir(cachedir) != 0) goto ret;
          char dup[PATH_MAX] = { 0 };
          g_strlcpy(dup, binname, sizeof(dup));
          char *bname = basename(dup);
#if defined(_WIN32)
          //CreateSymbolicLink in Windows requires admin privileges, which we don't want/need
          //store has using a simple filerename
          char finalfilename[PATH_MAX] = { 0 };
          snprintf(finalfilename, sizeof(finalfilename), "%s" G_DIR_SEPARATOR_S "%s.%s", cachedir, bname, md5sum);
          rename(link_dest, finalfilename);
#else
          if(symlink(md5sum, bname) != 0) goto ret;
#endif //!defined(_WIN32)
          if(chdir(cwd) != 0) goto ret;
        }

    ret:
      for(int i = 0; i < numdev; i++) free(binaries[i]);
      free(binaries);
      free(binary_sizes);
      free(devices);
    }
    return CL_SUCCESS;
  }
}

int dt_opencl_create_kernel(const int prog, const char *name)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return -1;
  if(prog < 0 || prog >= DT_OPENCL_MAX_PROGRAMS) return -1;
  dt_pthread_mutex_lock(&cl->lock);
  int k = 0;
  for(int dev = 0; dev < cl->num_devs; dev++)
  {
    cl_int err;
    for(; k < DT_OPENCL_MAX_KERNELS; k++)
      if(!cl->dev[dev].kernel_used[k])
      {
        cl->dev[dev].kernel_used[k] = 1;
        cl->dev[dev].kernel[k]
            = (cl->dlocl->symbols->dt_clCreateKernel)(cl->dev[dev].program[prog], name, &err);
        if(err != CL_SUCCESS)
        {
          dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] could not create kernel `%s'! (%i)\n", name, err);
          cl->dev[dev].kernel_used[k] = 0;
          goto error;
        }
        else
          break;
      }
    if(k < DT_OPENCL_MAX_KERNELS)
    {
      dt_vprint(DT_DEBUG_OPENCL, "[opencl_create_kernel] successfully loaded kernel `%s' (%d) for device %d\n",
               name, k, dev);
    }
    else
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_create_kernel] too many kernels! can't create kernel `%s'\n", name);
      goto error;
    }
  }
  dt_pthread_mutex_unlock(&cl->lock);
  return k;
error:
  dt_pthread_mutex_unlock(&cl->lock);
  return -1;
}

void dt_opencl_free_kernel(const int kernel)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited) return;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return;
  dt_pthread_mutex_lock(&cl->lock);
  for(int dev = 0; dev < cl->num_devs; dev++)
  {
    cl->dev[dev].kernel_used[kernel] = 0;
    (cl->dlocl->symbols->dt_clReleaseKernel)(cl->dev[dev].kernel[kernel]);
  }
  dt_pthread_mutex_unlock(&cl->lock);
}

int dt_opencl_get_max_work_item_sizes(const int dev, size_t *sizes)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  return (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid, CL_DEVICE_MAX_WORK_ITEM_SIZES,
                                                  sizeof(size_t) * 3, sizes, NULL);
}

int dt_opencl_get_work_group_limits(const int dev, size_t *sizes, size_t *workgroupsize,
                                    unsigned long *localmemsize)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  cl_ulong lmemsize;
  cl_int err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid, CL_DEVICE_LOCAL_MEM_SIZE,
                                                 sizeof(cl_ulong), &lmemsize, NULL);
  if(err != CL_SUCCESS) return err;

  *localmemsize = lmemsize;

  err = (cl->dlocl->symbols->dt_clGetDeviceInfo)(cl->dev[dev].devid, CL_DEVICE_MAX_WORK_GROUP_SIZE,
                                                 sizeof(size_t), workgroupsize, NULL);
  if(err != CL_SUCCESS) return err;

  return dt_opencl_get_max_work_item_sizes(dev, sizes);
}


int dt_opencl_get_kernel_work_group_size(const int dev, const int kernel, size_t *kernelworkgroupsize)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;

  return (cl->dlocl->symbols->dt_clGetKernelWorkGroupInfo)(cl->dev[dev].kernel[kernel], cl->dev[dev].devid,
                                                           CL_KERNEL_WORK_GROUP_SIZE, sizeof(size_t),
                                                           kernelworkgroupsize, NULL);
}


int dt_opencl_set_kernel_arg(const int dev, const int kernel, const int num, const size_t size,
                             const void *arg)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;
  return (cl->dlocl->symbols->dt_clSetKernelArg)(cl->dev[dev].kernel[kernel], num, size, arg);
}

int dt_opencl_enqueue_kernel_2d(const int dev, const int kernel, const size_t *sizes)
{
  return dt_opencl_enqueue_kernel_2d_with_local(dev, kernel, sizes, NULL);
}


int dt_opencl_enqueue_kernel_2d_with_local(const int dev, const int kernel, const size_t *sizes,
                                           const size_t *local)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || dev < 0) return -1;
  if(kernel < 0 || kernel >= DT_OPENCL_MAX_KERNELS) return -1;

  char buf[256];
  buf[0] = '\0';
  if(darktable.unmuted & DT_DEBUG_OPENCL)
    (cl->dlocl->symbols->dt_clGetKernelInfo)(cl->dev[dev].kernel[kernel], CL_KERNEL_FUNCTION_NAME, 256, buf, NULL);
  cl_event *eventp = dt_opencl_events_get_slot(dev, buf);
  cl_int err = (cl->dlocl->symbols->dt_clEnqueueNDRangeKernel)(cl->dev[dev].cmd_queue, cl->dev[dev].kernel[kernel],
                                                        2, NULL, sizes, local, 0, NULL, eventp);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[dt_opencl_enqueue_kernel_2d_with_local] kernel %i (%s) on device %d: %i\n", kernel, buf, dev, err);

  return err;
}

int dt_opencl_copy_device_to_host(const int devid, void *host, void *device, const int width,
                                  const int height, const int bpp)
{
  return dt_opencl_read_host_from_device(devid, host, device, width, height, bpp);
}

int dt_opencl_read_host_from_device(const int devid, void *host, void *device, const int width,
                                    const int height, const int bpp)
{
  return dt_opencl_read_host_from_device_rowpitch(devid, host, device, width, height, bpp * width);
}

int dt_opencl_read_host_from_device_rowpitch(const int devid, void *host, void *device, const int width,
                                             const int height, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // blocking.
  return dt_opencl_read_host_from_device_raw(devid, host, device, origin, region, rowpitch, CL_TRUE);
}

int dt_opencl_read_host_from_device_non_blocking(const int devid, void *host, void *device, const int width,
                                                 const int height, const int bpp)
{
  return dt_opencl_read_host_from_device_rowpitch_non_blocking(devid, host, device, width, height,
                                                               bpp * width);
}

int dt_opencl_read_host_from_device_rowpitch_non_blocking(const int devid, void *host, void *device,
                                                          const int width, const int height,
                                                          const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // non-blocking.
  return dt_opencl_read_host_from_device_raw(devid, host, device, origin, region, rowpitch, CL_FALSE);
}


int dt_opencl_read_host_from_device_raw(const int devid, void *host, void *device, const size_t *origin,
                                        const size_t *region, const int rowpitch, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Read Image (from device to host)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueReadImage)(darktable.opencl->dev[devid].cmd_queue,
                                                                   device, blocking ? CL_TRUE : CL_FALSE, origin, region, rowpitch,
                                                                   0, host, 0, NULL, eventp);
}

int dt_opencl_write_host_to_device(const int devid, void *host, void *device, const int width,
                                   const int height, const int bpp)
{
  return dt_opencl_write_host_to_device_rowpitch(devid, host, device, width, height, width * bpp);
}

int dt_opencl_write_host_to_device_rowpitch(const int devid, void *host, void *device, const int width,
                                            const int height, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // blocking.
  return dt_opencl_write_host_to_device_raw(devid, host, device, origin, region, rowpitch, CL_TRUE);
}

int dt_opencl_write_host_to_device_non_blocking(const int devid, void *host, void *device, const int width,
                                                const int height, const int bpp)
{
  return dt_opencl_write_host_to_device_rowpitch_non_blocking(devid, host, device, width, height, width * bpp);
}

int dt_opencl_write_host_to_device_rowpitch_non_blocking(const int devid, void *host, void *device,
                                                         const int width, const int height,
                                                         const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  const size_t origin[] = { 0, 0, 0 };
  const size_t region[] = { width, height, 1 };
  // non-blocking.
  return dt_opencl_write_host_to_device_raw(devid, host, device, origin, region, rowpitch, CL_FALSE);
}

int dt_opencl_write_host_to_device_raw(const int devid, void *host, void *device, const size_t *origin,
                                       const size_t *region, const int rowpitch, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Write Image (from host to device)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueWriteImage)(darktable.opencl->dev[devid].cmd_queue,
                                                                    device, blocking ? CL_TRUE : CL_FALSE, origin, region,
                                                                    rowpitch, 0, host, 0, NULL, eventp);
}

int dt_opencl_enqueue_copy_image(const int devid, cl_mem src, cl_mem dst, size_t *orig_src, size_t *orig_dst,
                                 size_t *region)
{
  if(!darktable.opencl->inited || devid < 0) return -1;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Image (on device)]");
  cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImage)(
      darktable.opencl->dev[devid].cmd_queue, src, dst, orig_src, orig_dst, region, 0, NULL, eventp);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl copy_image] could not copy image on device %d: %i\n", devid, err);
  return err;
}

int dt_opencl_enqueue_copy_image_to_buffer(const int devid, cl_mem src_image, cl_mem dst_buffer,
                                           size_t *origin, size_t *region, size_t offset)
{
  if(!darktable.opencl->inited) return -1;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Image to Buffer (on device)]");
  cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyImageToBuffer)(
      darktable.opencl->dev[devid].cmd_queue, src_image, dst_buffer, origin, region, offset, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl copy_image_to_buffer] could not copy image on device %d: %i\n", devid, err);
  return err;
}

int dt_opencl_enqueue_copy_buffer_to_image(const int devid, cl_mem src_buffer, cl_mem dst_image,
                                           size_t offset, size_t *origin, size_t *region)
{
  if(!darktable.opencl->inited) return -1;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Buffer to Image (on device)]");
  cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyBufferToImage)(
      darktable.opencl->dev[devid].cmd_queue, src_buffer, dst_image, offset, origin, region, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl copy_buffer_to_image] could not copy buffer on device %d: %i\n", devid, err);
  return err;
}

int dt_opencl_enqueue_copy_buffer_to_buffer(const int devid, cl_mem src_buffer, cl_mem dst_buffer,
                                            size_t srcoffset, size_t dstoffset, size_t size)
{
  if(!darktable.opencl->inited) return -1;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Copy Buffer to Buffer (on device)]");
  cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueCopyBuffer)(darktable.opencl->dev[devid].cmd_queue,
                                                                   src_buffer, dst_buffer, srcoffset,
                                                                   dstoffset, size, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl copy_buffer_to_buffer] could not copy buffer on device %d: %i\n", devid, err);
  return err;
}

int dt_opencl_read_buffer_from_device(const int devid, void *host, void *device, const size_t offset,
                                      const size_t size, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Read Buffer (from device to host)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueReadBuffer)(
      darktable.opencl->dev[devid].cmd_queue, device, blocking ? CL_TRUE : CL_FALSE, offset, size, host, 0, NULL, eventp);
}

int dt_opencl_write_buffer_to_device(const int devid, void *host, void *device, const size_t offset,
                                     const size_t size, const int blocking)
{
  if(!darktable.opencl->inited) return -1;

  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Write Buffer (from host to device)]");

  return (darktable.opencl->dlocl->symbols->dt_clEnqueueWriteBuffer)(
      darktable.opencl->dev[devid].cmd_queue, device, blocking ? CL_TRUE : CL_FALSE, offset, size, host, 0, NULL, eventp);
}


void *dt_opencl_copy_host_to_device_constant(const int devid, const size_t size, void *host)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer)(
      darktable.opencl->dev[devid].context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, size, host, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_host_to_device_constant] could not alloc buffer on device %d: %i\n", devid, err);

  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}

void *dt_opencl_copy_host_to_device(const int devid, void *host, const int width, const int height,
                                    const int bpp)
{
  return dt_opencl_copy_host_to_device_rowpitch(devid, host, width, height, bpp, 0);
}

void *dt_opencl_copy_host_to_device_rowpitch(const int devid, void *host, const int width, const int height,
                                             const int bpp, const int rowpitch)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else
    return NULL;

  // TODO: if fmt = uint16_t, blow up to 4xuint16_t and copy manually!
  cl_mem dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D)(
      darktable.opencl->dev[devid].context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, &fmt, width, height,
      rowpitch, host, &err);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl copy_host_to_device] could not alloc/copy img buffer on device %d: %i\n", devid, err);

  dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}


void dt_opencl_release_mem_object(cl_mem mem)
{
  if(!darktable.opencl->inited) return;

  // the OpenCL specs are not absolutely clear if clReleaseMemObject(NULL) is a no-op. we take care of the
  // case in a centralized way at this place
  if(mem == NULL) return;

  dt_opencl_memory_statistics(-1, mem, OPENCL_MEMORY_SUB);

  (darktable.opencl->dlocl->symbols->dt_clReleaseMemObject)(mem);
}

void *dt_opencl_map_buffer(const int devid, cl_mem buffer, const int blocking, const int flags, size_t offset,
                           size_t size)
{
  if(!darktable.opencl->inited) return NULL;
  cl_int err;
  void *ptr;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Map Buffer]");
  ptr = (darktable.opencl->dlocl->symbols->dt_clEnqueueMapBuffer)(
      darktable.opencl->dev[devid].cmd_queue, buffer, blocking ? CL_TRUE : CL_FALSE, flags, offset, size, 0, NULL, eventp, &err);
  if(err != CL_SUCCESS) dt_print(DT_DEBUG_OPENCL, "[opencl map buffer] could not map buffer on device %d: %i\n", devid, err);
  return ptr;
}


void *dt_opencl_map_image(const int devid, cl_mem buffer, const int blocking, const int flags, size_t width, size_t height, int bpp)
{
  if(!darktable.opencl->inited) return NULL;
  cl_int err;
  void *ptr;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Map Image 2D]");
  size_t origin[3] = {0, 0, 0};
  size_t region[3] = {width, height, 1};
  size_t mapped_row_pitch;

  ptr = (darktable.opencl->dlocl->symbols->dt_clEnqueueMapImage)(
      darktable.opencl->dev[devid].cmd_queue, buffer, blocking ? CL_TRUE : CL_FALSE, flags, origin, region,
      &mapped_row_pitch, NULL, 0, NULL, eventp, &err);

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl map buffer] could not map image on device %d: %i\n", devid, err);
  return ptr;
}


int dt_opencl_unmap_mem_object(const int devid, cl_mem mem_object, void *mapped_ptr)
{
  if(!darktable.opencl->inited) return -1;
  cl_event *eventp = dt_opencl_events_get_slot(devid, "[Unmap Mem Object]");
  cl_int err = (darktable.opencl->dlocl->symbols->dt_clEnqueueUnmapMemObject)(
      darktable.opencl->dev[devid].cmd_queue, mem_object, mapped_ptr, 0, NULL, eventp);
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl unmap mem object] could not unmap mem object on device %d: %i\n", devid, err);
  return err;
}

void *dt_opencl_alloc_device(const int devid, const int width, const int height, const int bpp)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else if(bpp == sizeof(uint8_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT8 };
  else
    return NULL;

  cl_mem dev = NULL;
  for(int attempt = 0; attempt < 2; attempt++)
  {
    dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D)(
        darktable.opencl->dev[devid].context, CL_MEM_READ_WRITE, &fmt, width, height, 0, NULL, &err);
    if(err == CL_SUCCESS) break;
    if(attempt == 0 && (err == CL_MEM_OBJECT_ALLOCATION_FAILURE || err == CL_OUT_OF_RESOURCES))
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl alloc_device] out of memory on device %d, flushing cached pinned buffers and retrying\n",
               devid);
      dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);
      continue;
    }
    break;
  }

  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl alloc_device] could not alloc img buffer on device %d: %i\n", devid,
             err);

  if(err == CL_SUCCESS) dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}


void *dt_opencl_alloc_device_use_host_pointer(const int devid, const int width, const int height,
                                              const int bpp, void *host, const int flags)
{
  if(!darktable.opencl->inited || devid < 0) return NULL;
  cl_int err;
  cl_image_format fmt;
  // guess pixel format from bytes per pixel
  if(bpp == 4 * sizeof(float))
    fmt = (cl_image_format){ CL_RGBA, CL_FLOAT };
  else if(bpp == sizeof(float))
    fmt = (cl_image_format){ CL_R, CL_FLOAT };
  else if(bpp == sizeof(uint16_t))
    fmt = (cl_image_format){ CL_R, CL_UNSIGNED_INT16 };
  else
    return NULL;

  cl_mem dev = NULL;
  for(int attempt = 0; attempt < 2; attempt++)
  {
    dev = (darktable.opencl->dlocl->symbols->dt_clCreateImage2D)(
        darktable.opencl->dev[devid].context, flags, &fmt, width, height, 0,
        host, &err);
    if(err == CL_SUCCESS) break;
    if(attempt == 0 && (err == CL_MEM_OBJECT_ALLOCATION_FAILURE || err == CL_OUT_OF_RESOURCES))
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl alloc_device_use_host_pointer] out of memory on device %d, flushing cached pinned buffers and retrying\n",
               devid);
      dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);
      continue;
    }
    break;
  }
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL,
             "[opencl alloc_device_use_host_pointer] could not alloc img buffer on device %d: %i\n", devid,
             err);

  if(err == CL_SUCCESS) dt_opencl_memory_statistics(devid, dev, OPENCL_MEMORY_ADD);

  return dev;
}

void *dt_opencl_alloc_device_buffer_with_flags(const int devid, const size_t size, const int flags)
{
  if(!darktable.opencl->inited) return NULL;
  cl_int err;
  cl_mem buf = NULL;
  for(int attempt = 0; attempt < 2; attempt++)
  {
    buf = (darktable.opencl->dlocl->symbols->dt_clCreateBuffer)(darktable.opencl->dev[devid].context,
                                                               flags, size, NULL, &err);
    if(err == CL_SUCCESS) break;
    if(attempt == 0 && (err == CL_MEM_OBJECT_ALLOCATION_FAILURE || err == CL_OUT_OF_RESOURCES))
    {
      dt_print(DT_DEBUG_OPENCL,
               "[opencl alloc_device_buffer] out of memory on device %d, flushing cached pinned buffers and retrying\n",
               devid);
      dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);
      continue;
    }
    break;
  }
  if(err != CL_SUCCESS)
    dt_print(DT_DEBUG_OPENCL, "[opencl alloc_device_buffer] could not alloc buffer on device %d: %d\n", devid,
             err);

  if(err == CL_SUCCESS) dt_opencl_memory_statistics(devid, buf, OPENCL_MEMORY_ADD);

  return buf;
}


void *dt_opencl_alloc_device_buffer(const int devid, const size_t size)
{
  return dt_opencl_alloc_device_buffer_with_flags(devid, size,  CL_MEM_READ_WRITE);
}


size_t dt_opencl_get_mem_object_size(cl_mem mem)
{
  size_t size;
  if(mem == NULL) return 0;

  cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetMemObjectInfo)(mem, CL_MEM_SIZE, sizeof(size), &size, NULL);

  return (err == CL_SUCCESS) ? size : 0;
}

int dt_opencl_get_mem_context_id(cl_mem mem)
{
  cl_context context;
  if(mem == NULL) return -1;

  cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetMemObjectInfo)(mem, CL_MEM_CONTEXT, sizeof(context), &context, NULL);
  if(err != CL_SUCCESS)
    return -1;

  for(int devid = 0; devid < darktable.opencl->num_devs; devid++)
  {
    if(darktable.opencl->dev[devid].context == context)
      return devid;
  }

  return -1;
}

cl_mem_flags dt_opencl_get_mem_flags(cl_mem mem)
{
  if(!darktable.opencl->inited || mem == NULL) return 0;
  cl_mem_flags flags = 0;
  cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetMemObjectInfo)(mem, CL_MEM_FLAGS, sizeof(flags), &flags, NULL);
  if(err != CL_SUCCESS) return 0;
  return flags;
}

int dt_opencl_get_image_width(cl_mem mem)
{
  size_t size;
  if(mem == NULL) return 0;

  cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetImageInfo)(mem, CL_IMAGE_WIDTH, sizeof(size), &size, NULL);
  if(size > INT_MAX) size = 0;

  return (err == CL_SUCCESS) ? (int)size : 0;
}

int dt_opencl_get_image_height(cl_mem mem)
{
  size_t size;
  if(mem == NULL) return 0;

  cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetImageInfo)(mem, CL_IMAGE_HEIGHT, sizeof(size), &size, NULL);
  if(size > INT_MAX) size = 0;

  return (err == CL_SUCCESS) ? (int)size : 0;
}

int dt_opencl_get_image_element_size(cl_mem mem)
{
  size_t size;
  if(mem == NULL) return 0;

  cl_int err = (darktable.opencl->dlocl->symbols->dt_clGetImageInfo)(mem, CL_IMAGE_ELEMENT_SIZE, sizeof(size), &size,
                                                              NULL);
  if(size > INT_MAX) size = 0;

  return (err == CL_SUCCESS) ? (int)size : 0;
}

void dt_opencl_memory_statistics(int devid, cl_mem mem, dt_opencl_memory_t action)
{
  if(devid < 0)
    devid = dt_opencl_get_mem_context_id(mem);

  if(devid < 0)
    return;

  const size_t size = dt_opencl_get_mem_object_size(mem);
  if(action == OPENCL_MEMORY_ADD)
    darktable.opencl->dev[devid].memory_in_use += size;
  else
    darktable.opencl->dev[devid].memory_in_use =
      (darktable.opencl->dev[devid].memory_in_use > size)
        ? (darktable.opencl->dev[devid].memory_in_use - size)
        : 0;

  darktable.opencl->dev[devid].peak_memory = MAX(darktable.opencl->dev[devid].peak_memory,
                                                 darktable.opencl->dev[devid].memory_in_use);

  if((darktable.unmuted & DT_DEBUG_MEMORY) && (darktable.unmuted & DT_DEBUG_OPENCL))
    dt_print(DT_DEBUG_OPENCL,
              "[opencl memory] device %d: %zu bytes (%.1f MB) in use\n", devid, darktable.opencl->dev[devid].memory_in_use,
                                      (float)darktable.opencl->dev[devid].memory_in_use/(1024*1024));
}

void dt_opencl_check_tuning(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;

  // Take the max of the device-specific and global param
  size_t headroom = MAX(dt_conf_get_int64("memory_opencl_headroom"), cl->dev[devid].forced_headroom);

  cl->dev[devid].used_available = MAX(0ul, cl->dev[devid].max_global_mem - headroom * 1024 * 1024);

  dt_print(DT_DEBUG_OPENCL | DT_DEBUG_MEMORY,
      "[dt_opencl_check_tuning] use %lu MiB on device `%s' id=%i\n",
      cl->dev[devid].used_available / (1024 * 1024),
      cl->dev[devid].name, devid);
}

cl_ulong dt_opencl_get_device_available(const int devid)
{
  if(!darktable.opencl->inited || devid < 0) return 0;
  const cl_ulong limit = darktable.opencl->dev[devid].used_available;
  const size_t in_use = darktable.opencl->dev[devid].memory_in_use;
  return (limit > in_use) ? (limit - in_use) : 0;
}

static cl_ulong _opencl_get_device_memalloc(const int devid)
{
  return darktable.opencl->dev[devid].max_mem_alloc;
}

cl_ulong dt_opencl_get_device_memalloc(const int devid)
{
  if(!darktable.opencl->inited || devid < 0) return 0;
  return _opencl_get_device_memalloc(devid);
}

gboolean dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height, const unsigned bpp,
                                const float factor, const size_t overhead)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;

  const size_t required  = width * height * bpp;
  const double total = (double)required * (double)factor + (double)overhead;

  if(cl->dev[devid].max_image_width < width || cl->dev[devid].max_image_height < height)
    return FALSE;

  if(_opencl_get_device_memalloc(devid) < required)      return FALSE;

  if((double)dt_opencl_get_device_available(devid) >= total) return TRUE;

  dt_print(DT_DEBUG_OPENCL,
           "[dt_opencl_image_fits_device] not enough memory on device %d, flushing cached pinned buffers and retrying\n",
           devid);
  dt_dev_pixelpipe_cache_flush_clmem(darktable.pixelpipe_cache, devid);

  return ((double)dt_opencl_get_device_available(devid) >= total);
}

/** round size to a multiple of the value given in the device specifig config parameter clroundup_wd/ht */
int dt_opencl_dev_roundup_width(int size, const int devid)
{
  const int roundup = darktable.opencl->dev[devid].clroundup_wd;
  return (size % roundup == 0 ? size : (size / roundup + 1) * roundup);
}
int dt_opencl_dev_roundup_height(int size, const int devid)
{
  const int roundup = darktable.opencl->dev[devid].clroundup_ht;
  return (size % roundup == 0 ? size : (size / roundup + 1) * roundup);
}

/** check if opencl is inited */
int dt_opencl_is_inited(void)
{
  return darktable.opencl->inited;
}


/** check if opencl is enabled */
int dt_opencl_is_enabled(void)
{
  if(!darktable.opencl->inited) return FALSE;
  return darktable.opencl->enabled;
}


/** disable opencl */
void dt_opencl_disable(void)
{
  if(!darktable.opencl->inited) return;
  darktable.opencl->enabled = FALSE;
  dt_conf_set_bool("opencl", FALSE);
}


/** update enabled flag and profile with value from preferences, returns enabled flag */
int dt_opencl_update_settings(void)
{
  dt_opencl_t *cl = darktable.opencl;
  // FIXME: This pulls in prefs every time the pixelpipe runs. Instead have a callback for DT_SIGNAL_PREFERENCES_CHANGE?
  if(!cl->inited) return FALSE;
  const int prefs = dt_conf_get_bool("opencl");

  if(cl->enabled != prefs)
  {
    cl->enabled = prefs;
    cl->stopped = 0;
    cl->error_count = 0;
    dt_print(DT_DEBUG_OPENCL, "[opencl_update_enabled] enabled flag set to %s\n", prefs ? "ON" : "OFF");
  }

  return (cl->enabled && !cl->stopped);
}


/** set opencl specific synchronization timeout */
static void dt_opencl_set_synchronization_timeout(int value)
{
  darktable.opencl->opencl_synchronization_timeout = value;
  dt_print_nts(DT_DEBUG_OPENCL, "[opencl_synchronization_timeout] synchronization timeout set to %d\n", value);
}

/** adjust opencl subsystem according to scheduling profile */
static void dt_opencl_apply_scheduling_profile()
{
  dt_pthread_mutex_lock(&darktable.opencl->lock);
  dt_opencl_update_priorities();
  dt_opencl_set_synchronization_timeout(dt_conf_get_int("pixelpipe_synchronization_timeout"));
  dt_pthread_mutex_unlock(&darktable.opencl->lock);
}


/** the following eventlist functions assume that affected structures are locked upstream */

/** get next free slot in eventlist (and manage size of eventlist) */
cl_event *dt_opencl_events_get_slot(const int devid, const char *tag)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return NULL;
  if(!cl->dev[devid].use_events) return NULL;

  static const cl_event zeroevent[1]; // implicitly initialized to zero
  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *maxevents = &(cl->dev[devid].maxevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totalevents = &(cl->dev[devid].totalevents);
  int *totallost = &(cl->dev[devid].totallost);
  int *maxeventslot = &(cl->dev[devid].maxeventslot);
  // if first time called: allocate initial buffers
  if(*eventlist == NULL)
  {
    int newevents = DT_OPENCL_EVENTLISTSIZE;
    *eventlist = calloc(newevents, sizeof(cl_event));
    *eventtags = calloc(newevents, sizeof(dt_opencl_eventtag_t));
    if(!*eventlist || !*eventtags)
    {
      free(*eventlist);
      free(*eventtags);
      *eventlist = NULL;
      *eventtags = NULL;
      dt_print(DT_DEBUG_OPENCL, "[dt_opencl_events_get_slot] NO eventlist for device %i\n", devid);
      return NULL;
    }
    *maxevents = newevents;
  }

  // check if currently highest event slot was actually consumed. If not use it again
  if(*numevents > 0 && !memcmp((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event)))
  {
    (*lostevents)++;
    (*totallost)++;
    if(tag != NULL)
    {
      g_strlcpy((*eventtags)[*numevents - 1].tag, tag, DT_OPENCL_EVENTNAMELENGTH);
    }
    else
    {
      (*eventtags)[*numevents - 1].tag[0] = '\0';
    }

    (*totalevents)++;
    return (*eventlist) + *numevents - 1;
  }

  // check if we would exceed the number of available event handles. In that case first flush existing handles
  if((*numevents - *eventsconsolidated + 1 > cl->dev[devid].event_handles) || (*numevents == *maxevents))
    (void)dt_opencl_events_flush(devid, 0);

  // if no more space left in eventlist: grow buffer
  if(*numevents == *maxevents)
  {
    int newevents = *maxevents + DT_OPENCL_EVENTLISTSIZE;
    cl_event *neweventlist = calloc(newevents, sizeof(cl_event));
    dt_opencl_eventtag_t *neweventtags = calloc(newevents, sizeof(dt_opencl_eventtag_t));
    if(!neweventlist || !neweventtags)
    {
      dt_print(DT_DEBUG_OPENCL, "[dt_opencl_events_get_slot] NO new eventlist with size %i for device %i\n",
         newevents, devid);
      free(neweventlist);
      free(neweventtags);
      return NULL;
    }
    memcpy(neweventlist, *eventlist, sizeof(cl_event) * *maxevents);
    memcpy(neweventtags, *eventtags, sizeof(dt_opencl_eventtag_t) * *maxevents);
    free(*eventlist);
    free(*eventtags);
    *eventlist = neweventlist;
    *eventtags = neweventtags;
    *maxevents = newevents;
  }

  // init next event slot and return it
  (*numevents)++;
  memcpy((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event));
  if(tag != NULL)
  {
    g_strlcpy((*eventtags)[*numevents - 1].tag, tag, DT_OPENCL_EVENTNAMELENGTH);
  }
  else
  {
    (*eventtags)[*numevents - 1].tag[0] = '\0';
  }

  (*totalevents)++;
  *maxeventslot = MAX(*maxeventslot, *numevents - 1);
  return (*eventlist) + *numevents - 1;
}


/** reset eventlist to empty state */
void dt_opencl_events_reset(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->dev[devid].use_events) return;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *maxevents = &(cl->dev[devid].maxevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  cl_int *summary = &(cl->dev[devid].summary);

  if(*eventlist == NULL || *numevents == 0) return; // nothing to do

  // release all remaining events in eventlist, not to waste resources
  for(int k = *eventsconsolidated; k < *numevents; k++)
  {
    (cl->dlocl->symbols->dt_clReleaseEvent)((*eventlist)[k]);
  }

  memset(*eventtags, 0, sizeof(dt_opencl_eventtag_t) * *maxevents);
  *numevents = 0;
  *eventsconsolidated = 0;
  *lostevents = 0;
  *summary = CL_COMPLETE;
  return;
}


/** Wait for events in eventlist to terminate -> this is a blocking synchronization point!
    Does not flush eventlist. Side effect: might adjust numevents. */
void dt_opencl_events_wait_for(const int devid)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->dev[devid].use_events) return;

  static const cl_event zeroevent[1]; // implicitly initialized to zero
  cl_event **eventlist = &(cl->dev[devid].eventlist);
  int *numevents = &(cl->dev[devid].numevents);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totallost = &(cl->dev[devid].totallost);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);

  if(*eventlist == NULL || *numevents == 0) return; // nothing to do

  // check if last event slot was actually used and correct numevents if needed
  if(!memcmp((*eventlist) + *numevents - 1, zeroevent, sizeof(cl_event)))
  {
    (*numevents)--;
    (*lostevents)++;
    (*totallost)++;
  }

  if(*numevents == *eventsconsolidated) return; // nothing to do

  assert(*numevents > *eventsconsolidated);

  // now wait for all remaining events to terminate
  // Risk: might never return in case of OpenCL blocks or endless loops
  // TODO: run clWaitForEvents in separate thread and implement watchdog timer
  cl_int err = (cl->dlocl->symbols->dt_clWaitForEvents)(*numevents - *eventsconsolidated,
                                           (*eventlist) + *eventsconsolidated);
  if((err != CL_SUCCESS) && (err != CL_INVALID_VALUE))
    dt_vprint(DT_DEBUG_OPENCL, "[dt_opencl_events_wait_for] reported %i for device %i\n",
       err, devid);
}


/** Wait for events in eventlist to terminate, check for return status and profiling
info of events.
If "reset" is TRUE report summary info (would be CL_COMPLETE or last error code) and
print profiling info if needed.
If "reset" is FALSE just store info (success value, profiling) from terminated events
and release events for re-use by OpenCL driver. */
cl_int dt_opencl_events_flush(const int devid, const int reset)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;
  if(!cl->dev[devid].use_events) return FALSE;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);
  int *totalsuccess = &(cl->dev[devid].totalsuccess);

  cl_int *summary = &(cl->dev[devid].summary);

  if(*eventlist == NULL || *numevents == 0) return CL_COMPLETE; // nothing to do, no news is good news

  // Wait for command queue to terminate (side effect: might adjust *numevents)
  dt_opencl_events_wait_for(devid);

  // now check return status and profiling data of all newly terminated events
  for(int k = *eventsconsolidated; k < *numevents; k++)
  {
    cl_int err;
    char *tag = (*eventtags)[k].tag;
    cl_int *retval = &((*eventtags)[k].retval);

    // get return value of event
    err = (cl->dlocl->symbols->dt_clGetEventInfo)((*eventlist)[k], CL_EVENT_COMMAND_EXECUTION_STATUS,
                                                  sizeof(cl_int), retval, NULL);
    if(err != CL_SUCCESS)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_events_flush] could not get event info for '%s': %i\n",
               tag[0] == '\0' ? "<?>" : tag, err);
    }
    else if(*retval != CL_COMPLETE)
    {
      dt_print(DT_DEBUG_OPENCL, "[opencl_events_flush] execution of '%s' %s: %d\n",
               tag[0] == '\0' ? "<?>" : tag, *retval == CL_COMPLETE ? "was successful" : "failed", *retval);
      *summary = *retval;
    }
    else
      (*totalsuccess)++;

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      // get profiling info of event (only if darktable was called with '-d perf')
      cl_ulong start;
      cl_ulong end;
      cl_int errs = (cl->dlocl->symbols->dt_clGetEventProfilingInfo)(
          (*eventlist)[k], CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
      cl_int erre = (cl->dlocl->symbols->dt_clGetEventProfilingInfo)((*eventlist)[k], CL_PROFILING_COMMAND_END,
                                                                   sizeof(cl_ulong), &end, NULL);
      if(errs == CL_SUCCESS && erre == CL_SUCCESS)
      {
        (*eventtags)[k].timelapsed = end - start;
      }
      else
      {
        (*eventtags)[k].timelapsed = 0;
        (*lostevents)++;
      }
    }
    else
      (*eventtags)[k].timelapsed = 0;

    // finally release event to be re-used by driver
    (cl->dlocl->symbols->dt_clReleaseEvent)((*eventlist)[k]);
    (*eventsconsolidated)++;
  }

  cl_int result = *summary;

  // do we want to get rid of all stored info?
  if(reset)
  {
    // output profiling info if wanted
    if(darktable.unmuted & DT_DEBUG_PERF) dt_opencl_events_profiling(devid, 1);

    // reset eventlist structures to empty state
    dt_opencl_events_reset(devid);
  }

  return result == CL_COMPLETE ? 0 : result;
}


/** display OpenCL profiling information. If "aggregated" is TRUE, try to generate summarized info for each
 * kernel */
void dt_opencl_events_profiling(const int devid, const int aggregated)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return;
  if(!cl->dev[devid].use_events) return;

  cl_event **eventlist = &(cl->dev[devid].eventlist);
  dt_opencl_eventtag_t **eventtags = &(cl->dev[devid].eventtags);
  int *numevents = &(cl->dev[devid].numevents);
  int *eventsconsolidated = &(cl->dev[devid].eventsconsolidated);
  int *lostevents = &(cl->dev[devid].lostevents);

  if(*eventlist == NULL || *numevents == 0 || *eventtags == NULL || *eventsconsolidated == 0)
    return; // nothing to do

  char **tags = malloc(sizeof(char *) * (*eventsconsolidated + 1));
  float *timings = malloc(sizeof(float) * (*eventsconsolidated + 1));
  int items = 1;
  tags[0] = "";
  timings[0] = 0.0f;

  // get profiling info and arrange it
  for(int k = 0; k < *eventsconsolidated; k++)
  {
    // if aggregated is TRUE, try to sum up timings for multiple runs of each kernel
    if(aggregated)
    {
      // linear search: this is not efficient at all but acceptable given the limited
      // number of events (ca. 10 - 20)
      int tagfound = -1;
      for(int i = 0; i < items; i++)
      {
        if(!strncmp(tags[i], (*eventtags)[k].tag, DT_OPENCL_EVENTNAMELENGTH))
        {
          tagfound = i;
          break;
        }
      }

      if(tagfound >= 0) // tag was already detected before
      {
        // sum up timings
        timings[tagfound] += (*eventtags)[k].timelapsed * 1e-9;
      }
      else // tag is new
      {
        // make new entry
        items++;
        tags[items - 1] = (*eventtags)[k].tag;
        timings[items - 1] = (*eventtags)[k].timelapsed * 1e-9;
      }
    }

    else // no aggregated info wanted -> arrange event by event
    {
      items++;
      tags[items - 1] = (*eventtags)[k].tag;
      timings[items - 1] = (*eventtags)[k].timelapsed * 1e-9;
    }
  }

  // now display profiling info
  dt_print(DT_DEBUG_OPENCL,
           "[opencl_profiling] profiling device %d ('%s'):\n", devid, cl->dev[devid].name);

  float total = 0.0f;
  for(int i = 1; i < items; i++)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds in %s\n", (double)timings[i],
             tags[i][0] == '\0' ? "<?>" : tags[i]);
    total += timings[i];
  }
  // aggregated timing info for items without tag (if any)
  if(timings[0] != 0.0f)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_profiling] spent %7.4f seconds (unallocated)\n", (double)timings[0]);
    total += timings[0];
  }

  dt_print(DT_DEBUG_OPENCL,
           "[opencl_profiling] spent %7.4f seconds totally in command queue (with %d event%s missing)\n",
           (double)total, *lostevents, *lostevents == 1 ? "" : "s");

  free(timings);
  free(tags);

  return;
}

static int nextpow2(int n)
{
  int k = 1;
  while (k < n)
    k <<= 1;
  return k;
}

// utility function to calculate optimal work group dimensions for a given kernel
// taking device specific restrictions and local memory limitations into account
int dt_opencl_local_buffer_opt(const int devid, const int kernel, dt_opencl_local_buffer_t *factors)
{
  dt_opencl_t *cl = darktable.opencl;
  if(!cl->inited || devid < 0) return FALSE;

  size_t maxsizes[3] = { 0 };     // the maximum dimensions for a work group
  size_t workgroupsize = 0;       // the maximum number of items in a work group
  unsigned long localmemsize = 0; // the maximum amount of local memory we can use
  size_t kernelworkgroupsize = 0; // the maximum amount of items in work group for this kernel

  int *blocksizex = &factors->sizex;
  int *blocksizey = &factors->sizey;

  // initial values must be supplied in sizex and sizey.
  // we make sure that these are a power of 2 and lie within reasonable limits.
  *blocksizex = CLAMP(nextpow2(*blocksizex), 1, 1 << 16);
  *blocksizey = CLAMP(nextpow2(*blocksizey), 1, 1 << 16);

  if(dt_opencl_get_work_group_limits(devid, maxsizes, &workgroupsize, &localmemsize) == CL_SUCCESS
     && dt_opencl_get_kernel_work_group_size(devid, kernel, &kernelworkgroupsize) == CL_SUCCESS)
  {
    while(maxsizes[0] < *blocksizex || maxsizes[1] < *blocksizey
       || localmemsize < ((factors->xfactor * (*blocksizex) + factors->xoffset) *
                          (factors->yfactor * (*blocksizey) + factors->yoffset)) * factors->cellsize + factors->overhead
       || workgroupsize < (size_t)(*blocksizex) * (*blocksizey) || kernelworkgroupsize < (size_t)(*blocksizex) * (*blocksizey))
    {
      if(*blocksizex == 1 && *blocksizey == 1) return FALSE;

      if(*blocksizex > *blocksizey)
        *blocksizex >>= 1;
      else
        *blocksizey >>= 1;
    }
  }
  else
  {
    dt_print(DT_DEBUG_OPENCL, "[dt_opencl_local_buffer_opt] can not identify resource limits for device %d\n", devid);
    return FALSE;
  }

  return TRUE;
}


#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
