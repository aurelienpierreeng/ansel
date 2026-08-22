/*
    This file is part of darktable,
    Copyright (C) 2010-2012, 2016 johannes hanika.
    Copyright (C) 2010, 2020-2021 Pascal Obry.
    Copyright (C) 2011 Henrik Andersson.
    Copyright (C) 2011, 2013-2014, 2016 Tobias Ellinghaus.
    Copyright (C) 2011-2017 Ulrich Pegelow.
    Copyright (C) 2012 Michal Babej.
    Copyright (C) 2012 Richard Wonka.
    Copyright (C) 2013 Pascal de Bruijn.
    Copyright (C) 2015, 2019 Dan Torop.
    Copyright (C) 2016 Roman Lebedev.
    Copyright (C) 2017-2019 Edgardo Hoszowski.
    Copyright (C) 2017 luzpaz.
    Copyright (C) 2019 Heiko Bauke.
    Copyright (C) 2019 jakubfi.
    Copyright (C) 2021-2022, 2025-2026 Aurélien PIERRE.
    Copyright (C) 2021 Hubert Kowalski.
    Copyright (C) 2022 Hanno Schwalm.
    Copyright (C) 2022 Martin Bařinka.
    Copyright (C) 2025 Alynx Zhou.
    Copyright (C) 2025 Guillaume Stutin.
    
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

#ifndef DT_COMMON_OPENCL_H
#define DT_COMMON_OPENCL_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define DT_OPENCL_MAX_PLATFORMS 5
#define DT_OPENCL_MAX_PROGRAMS 256
#define DT_OPENCL_MAX_KERNELS 512
#define DT_OPENCL_EVENTLISTSIZE 256
#define DT_OPENCL_EVENTNAMELENGTH 64
#define DT_OPENCL_MAX_ERRORS 5
/* Size of the clincludes[] table in dt_opencl_priority_parse()'s caller, NULL terminator
 * included. dt_opencl_md5sum() walks exactly this many entries, so the table must have
 * room for its terminator: adding an include without raising this reads past the end. */
#define DT_OPENCL_MAX_INCLUDES 8
#define DT_OPENCL_VENDOR_AMD 4098
#define DT_OPENCL_VENDOR_NVIDIA 4318
#define DT_OPENCL_VENDOR_INTEL 0x8086u
#define DT_OPENCL_CBUFFSIZE 1024

// some pseudo error codes in dt opencl usage
#define DT_OPENCL_DEFAULT_ERROR -999
#define DT_OPENCL_SYSMEM_ALLOCATION -998

#include "common/logging.h"

#include <stdint.h>

#ifdef HAVE_OPENCL

#include "common/dlopencl.h"
#include "common/conf.h"

// #pragma GCC diagnostic push
// #pragma GCC diagnostic ignored "-Wcomment"
#include <CL/cl.h>
// #pragma GCC diagnostic

#ifdef __cplusplus
extern "C" {
#endif

#define ROUNDUP(a, n) ((a) % (n) == 0 ? (a) : ((a) / (n)+1) * (n))

// use per device roundups here
#define ROUNDUPDWD(a, b) dt_opencl_dev_roundup_width(a, b)
#define ROUNDUPDHT(a, b) dt_opencl_dev_roundup_height(a, b)

#define DT_OPENCL_BPP_TAG_RGBA8 (1u << 30)
#define DT_OPENCL_BPP_ENCODE_RGBA8(bpp) ((int)((unsigned int)(bpp) | DT_OPENCL_BPP_TAG_RGBA8))
#define DT_OPENCL_BPP_IS_RGBA8(bpp) ((((unsigned int)(bpp)) & DT_OPENCL_BPP_TAG_RGBA8) != 0u)
#define DT_OPENCL_BPP_DECODE(bpp) ((int)(((unsigned int)(bpp)) & ~DT_OPENCL_BPP_TAG_RGBA8))

/* Per-vendor kernel build options. The per-device conf key
 * `cldevice_v4/<n>/<cname>/building` overrides these and is what a user edits in anselrc; these
 * are only the value written there on first sight of a device.
 *
 * `-cl-unsafe-math-optimizations` (and `-cl-fast-relaxed-math`, which implies it) lets the
 * driver substitute a low-precision implementation for any libm function. That is a per-vendor
 * gamble, not a uniform speedup, and Intel loses it badly: measured on an HD Graphics P630
 * (NEO 24.35, max relative error of erf() against a double-precision host reference over
 * x in [-8, 8])
 *
 *     safe flags   1.39e-07      correct to float precision
 *     unsafe       1.00e+00      erf() returns exactly 0.0 for |x| < ~1e-3, and is 22 % low
 *                                at 1e-2
 *
 * `iop/rawdenoiseai.c`'s GELU is 0.5*x*(1+erf(x/sqrt(2))) and a convolutional net spends most
 * of its activations in exactly that small-|x| band, so the whole denoiser drifts: the same
 * export differs from the CPU path by 0.78 % mean / 3.1 % p99 with the flag, and by 0.0023 %
 * without it -- the latter being bit-for-bit what NVIDIA produces. It is visible, and it is
 * what users report as a grid or mesh on X-Trans (discussion #1104).
 *
 * And it buys nothing to pay for that. Measured wall-clock, same image, kernels already cached:
 *
 *     P630, denoiser export          96.8 s unsafe   93.9 s safe
 *     Quadro M2200, denoiser export   8.10 s unsafe   8.14 s safe
 *     Quadro M2200, whole pipeline
 *       with the denoiser OFF, so
 *       every other module dominates  5.38/4.81/4.84 unsafe   5.27/4.92/4.89 safe
 *
 * i.e. a 0.4 % spread, inside run-to-run noise, on hardware old enough to show a real
 * difference if there were one. Dropping the flag also barely moves the existing NVIDIA render
 * (0.00024 % mean over the whole pipeline), so this is not a visual change for users who were
 * already getting correct output.
 *
 * So every vendor gets the same conservative set. `-cl-mad-enable` keeps the fused multiply-add
 * that actually matters for convolution throughput, `-cl-no-signed-zeros` is free, and neither
 * licenses a substitute libm. NVIDIA happens to keep erf() exact under the unsafe flag
 * (7.97e-08) but still degrades log(), and AMD has never been measured here at all -- there is
 * no reason to keep taking a per-driver gamble for a speedup that does not show up.
 *
 * The per-vendor macros are kept separate rather than collapsed: they are the natural place to
 * record a future measurement that justifies diverging again. Run tools/opencl-math-accuracy.c
 * to regenerate every number above on any machine; see doc/opencl-math-accuracy.md. */
#define DT_OPENCL_DEFAULT_COMPILE_INTEL ("-cl-mad-enable -cl-no-signed-zeros")
#define DT_OPENCL_DEFAULT_COMPILE_AMD ("-cl-mad-enable -cl-no-signed-zeros")
#define DT_OPENCL_DEFAULT_COMPILE_NVIDIA ("-cl-mad-enable -cl-no-signed-zeros")
#define DT_OPENCL_DEFAULT_COMPILE ("-cl-mad-enable -cl-no-signed-zeros")
#define DT_CLDEVICE_HEAD ("cldevice_v4")

typedef enum dt_opencl_memory_t
{
  OPENCL_MEMORY_ADD,
  OPENCL_MEMORY_SUB
} dt_opencl_memory_t;

/**
 * Accounting information used for OpenCL events.
 */
typedef struct dt_opencl_eventtag_t
{
  cl_int retval;
  cl_ulong timelapsed;
  char tag[DT_OPENCL_EVENTNAMELENGTH];
} dt_opencl_eventtag_t;

typedef enum dt_opencl_pinmode_t
{
  DT_OPENCL_PINNING_OFF = 0,
  DT_OPENCL_PINNING_ON = 1,
  DT_OPENCL_PINNING_DISABLED = 2
} dt_opencl_pinmode_t;

// Why an image did (or did not) fit on an OpenCL device. Each non-OK reason maps
// to a *different* limit, so a caller logging "needs X but limit is Y" must report
// the quantities that were actually compared for the returned reason -- see
// dt_opencl_image_fits_device_reason().
typedef enum dt_opencl_fit_reason_t
{
  DT_OPENCL_FIT_OK = 0,       // the image fits, the device can process it
  DT_OPENCL_FIT_DIMENSION,    // width/height exceed the device 2D image limits
  DT_OPENCL_FIT_ALLOC_LIMIT,  // a single buffer exceeds CL_DEVICE_MAX_MEM_ALLOC_SIZE
  DT_OPENCL_FIT_AVAILABLE,    // not enough free vRAM headroom for the factored allocation
  DT_OPENCL_FIT_UNINITED      // OpenCL unavailable / invalid device
} dt_opencl_fit_reason_t;

/**
 * to support multi-gpu and mixed systems with cpu support,
 * we encapsulate devices and use separate command queues.
 */
typedef struct dt_opencl_device_t
{
  dt_pthread_mutex_t lock;
  cl_device_id devid;
  cl_context context;
  cl_command_queue cmd_queue;
  size_t max_image_width;
  size_t max_image_height;
  cl_ulong max_mem_alloc;
  cl_ulong max_global_mem;
  cl_ulong used_global_mem;
  cl_program program[DT_OPENCL_MAX_PROGRAMS];
  cl_kernel kernel[DT_OPENCL_MAX_KERNELS];
  int program_used[DT_OPENCL_MAX_PROGRAMS];
  int kernel_used[DT_OPENCL_MAX_KERNELS];
  cl_event *eventlist;
  dt_opencl_eventtag_t *eventtags;
  int numevents;
  int eventsconsolidated;
  int maxevents;
  int lostevents;
  int totalevents;
  int totalsuccess;
  int totallost;
  int maxeventslot;
  int nvidia_sm_20;
  const char *vendor;
  const char *name;
  const char *cname;
  const char *options;
  const char *options_md5;
  cl_int summary;
  size_t memory_in_use;
  size_t peak_memory;
  size_t used_available;

  // flags detected errors
  int runtime_error;
  // if set to TRUE darktable will not use OpenCL kernels which contain atomic operations (example bilateral).
  // pixelpipe processing will be done on CPU for the affected modules.
  // useful (only for very old devices) if your OpenCL implementation freezes/crashes on atomics or if
  // they are processed with a bad performance.
  int avoid_atomics;

  // pause OpenCL processing for this number of microseconds from time to time
  int micro_nap;

  // During tiling huge amounts of memory need to be transferred between host and device.
  // For some OpenCL implementations direct memory transfers give a drastic performance penalty,
  // this can often be avoided by using indirect transfers via pinned memory,
  // other devices have more efficient direct memory transfer implementations.
  // We can't predict on solid grounds if a device belongs to the first or second group,
  // also pinned mem transfer requires slightly more ram.
  // this holds a bitmask defined by dt_opencl_pinmode_t
  // the device specific conf key might hold
  // 0 -> disabled by default; might be switched on by tune for performance
  // 1 -> enabled by default
  // 2 -> disabled under all circumstances. This could/should be used if we give away / ship specific keys for buggy systems
  int pinned_memory;

  // in OpenCL processing round width/height of global work groups to a multiple of these values.
  // reasonable values are powers of 2. this parameter can have high impact on OpenCL performance.
  int clroundup_wd;
  int clroundup_ht;

  // A bitfield that identifies the type of OpenCL device required to test for on-CPU and more.
  unsigned int cltype;

  // This defines how often should dt_opencl_events_get_slot do a dt_opencl_events_flush.
  // It should definitely le lower than the number of events that can be handled by the device/driver.
  // FIXME we should be able to test for that with using >= OpenCl 2.0
  int event_handles;

  // opencl_events enabled for the device, set internally via event_handles
  int use_events;

  // a device might be turned off by force by setting this value to 1
  // also used for blacklisted drivers
  int disabled;

  // CL_DEVICE_HOST_UNIFIED_MEMORY: TRUE for integrated GPUs that share the system's RAM
  // instead of owning dedicated vRAM. On these devices, "available memory" as reported by
  // the driver (max_global_mem, max_mem_alloc) and tracked by our own memory_in_use
  // bookkeeping is a much less reliable estimate of what can actually be allocated: the
  // whole OS, desktop compositor and every other process compete for that same pool, and we
  // have no visibility into that competition. Used to pick a larger default forced_headroom
  // (see dt_opencl_read_device_config()) so a size that our own accounting says "fits" is
  // less likely to still exceed real availability and abort inside the driver instead of
  // failing cleanly with CL_OUT_OF_RESOURCES.
  gboolean host_unified_memory;

  // Some devices are known to be unused by other apps so there is no need to test for available memory at all.
  // Also some devices might behave badly with the checking code, in this case we could enforce a headroom here.
  size_t forced_headroom;
} dt_opencl_device_t;

typedef struct dt_opencl_detected_device_t
{
  int config_id;
  char *name;
  char *cname;
  unsigned int cltype;
  int disabled;
  int pinned_memory;
  size_t forced_headroom;
  // Mirrors dt_opencl_device_t.host_unified_memory -- lets GUI code (preferences) label the
  // headroom setting accurately for integrated GPUs without reaching into the live device array.
  gboolean host_unified_memory;
} dt_opencl_detected_device_t;

struct dt_bilateral_cl_global_t;
struct dt_local_laplacian_cl_global_t;
struct dt_dwt_cl_global_t; // wavelet decompose
struct dt_heal_cl_global_t; // healing
struct dt_colorspaces_cl_global_t; // colorspaces transform
struct dt_guided_filter_cl_global_t;

/**
 * main struct, private to common/opencl.c.
 * holds pointers to all
 */
/* dt_opencl_t is PRIVATE to common/opencl.c.
 *
 * It used to be defined here and instantiated on the application god-struct, so its device
 * array, its lock and nine other subsystems' kernel bundles were reachable from any file that
 * included darktable.h. Callers ask questions now -- dt_opencl_get_num_devices(),
 * dt_opencl_get_device_name(), dt_opencl_get_device_max_image_size() -- and reserve devices
 * through dt_opencl_reserve_device_for_pipe() / _by_id(). Nothing outside the module needs
 * the layout, so nothing outside the module has it. */
typedef struct dt_opencl_t dt_opencl_t;

/** description of memory requirements of local buffer
  * local buffer size will be calculated as:
  * (xoffset + xfactor * x) * (yoffset + yfactor * y) * cellsize + overhead; */
typedef struct dt_opencl_local_buffer_t
{
  const int xoffset;
  const int xfactor;
  const int yoffset;
  const int yfactor;
  const size_t cellsize;
  const size_t overhead;
  int sizex;  // initial value and final values after optimization
  int sizey;  // initial value and final values after optimization
} dt_opencl_local_buffer_t;

/** internally calls dt_clGetDeviceInfo, and takes care of memory allocation
 * afterwards, *param_value will point to memory block of size at least *param_value
 * which needs to be g_free()'d manually */


/** inits the opencl subsystem. */
void dt_opencl_init(const gboolean exclude_opencl, const gboolean print_statistics);

/** cleans up the opencl subsystem. */
void dt_opencl_cleanup(void);

/** cleans up the i-th device in the cl->dev list */


/** both finish functions return TRUE in case of success */
/** cleans up command queue. */
int dt_opencl_finish(const int devid);

/** enqueues a synchronization point. */
int dt_opencl_enqueue_barrier(const int devid);

/** locks a device for your thread's exclusive use */
/**
 * @brief Reserve a device for a pipe run: choose a free one by the pipe's priority list and
 * take its lock. Blocks or gives up per the `opencl_mandatory_timeout` conf key.
 *
 * @param pipetype a ::dt_dev_pixelpipe_type_t; selects which priority list applies.
 * @return the reserved device id, or -1 if none could be had. Release with
 *         dt_opencl_release_device().
 *
 * @note There is ONE lock per device and this is it -- reserving a device and locking it are
 * the same act. dt_opencl_reserve_device_by_id() takes the same lock when the caller already
 * knows which device it needs. This used to be called `dt_opencl_lock_device()`, which took a
 * PIPE TYPE while its `dt_opencl_unlock_device()` counterpart took a DEVICE ID -- two
 * different ints, one of them silently wrong if the pair were ever read as symmetric. The old
 * names are gone rather than redefined, so a stale caller fails to compile.
 */
int dt_opencl_reserve_device_for_pipe(const int pipetype);

/**
 * @brief Reserve a device the caller has already identified, blocking until it is free.
 * @param devid device id; out-of-range ids are ignored.
 */
void dt_opencl_reserve_device_by_id(const int devid);

/**
 * @brief Reserve a device only if it is free right now.
 * @param devid device id.
 * @return 0 when the device was reserved (pthread convention), non-zero when it was busy or
 *         the id was out of range. Never waits -- callers use this on paths that must not
 *         block behind a pipe that is mid-run.
 */
int dt_opencl_try_reserve_device_by_id(const int devid);

/** @brief Number of usable OpenCL devices; 0 when OpenCL is unavailable. */
int dt_opencl_get_num_devices(void);

/**
 * @brief Human-readable device name, owned by the OpenCL module.
 * @param devid device id.
 * @return the name, or NULL for an out-of-range id or a device with no name. Valid until
 *         cleanup; do not free.
 */
const char *dt_opencl_get_device_name(const int devid);

/**
 * @brief Largest 2D image the device will accept, which is what bounds tile size.
 * @param devid device id.
 * @param width receives the maximum width. Not written for an out-of-range id.
 * @param height receives the maximum height. Same.
 * @return TRUE when both were written.
 */
gboolean dt_opencl_get_device_max_image_size(const int devid, int *width, int *height);

/** @brief Total device memory in bytes, or 0 for an out-of-range id. */
size_t dt_opencl_get_device_max_global_mem(const int devid);

/**
 * @brief Record that a pipe run failed on OpenCL, and decide whether to give up on it.
 *
 * @details The count and the give-up threshold belong to the OpenCL module, not to the
 * pipeline: the pipeline's job is to report the failure and be told what to do next. Crossing
 * the threshold also drops the "opencl" capability, so this is the one place that decides
 * OpenCL is finished for the session.
 *
 * @return 1 for "this run failed, retry on CPU", 2 for "too many failures, OpenCL is off for
 *         the rest of the session".
 */
int dt_opencl_report_pipe_error(void);

/** done with your command queue. */
/** @brief Release a device reserved by either reserve function. */
void dt_opencl_release_device(const int devid);

/** calculates md5sums for a list of CL include files. */
void dt_opencl_md5sum(const char **files, char **md5sums);

/** loads the given .cl file and returns a reference to an internal program. */
int dt_opencl_load_program(const int dev, const int prog, const char *filename, const char *binname,
                           const char *cachedir, char *md5sum, char **includemd5, int *loaded_cached);

/** builds the given program. */
int dt_opencl_build_program(const int dev, const int prog, const char *binname, const char *cachedir,
                            char *md5sum, int loaded_cached);

/** inits a kernel. returns the index or -1 if fail. */
int dt_opencl_create_kernel(const int program, const char *name);

/** releases kernel resources again. */
void dt_opencl_free_kernel(const int kernel);

/** return max size in sizes[3]. */
int dt_opencl_get_max_work_item_sizes(const int dev, size_t *sizes);

/** return max size per dimension in sizes[3] and max total size in workgroupsize */
int dt_opencl_get_work_group_limits(const int dev, size_t *sizes, size_t *workgroupsize,
                                    unsigned long *localmemsize);

/** return max workgroup size for a specific kernel */
int dt_opencl_get_kernel_work_group_size(const int dev, const int kernel, size_t *kernelworkgroupsize);

/** attach arg. */
int dt_opencl_set_kernel_arg(const int dev, const int kernel, const int num, const size_t size,
                             const void *arg);

/** launch kernel! */
int dt_opencl_enqueue_kernel_2d(const int dev, const int kernel, const size_t *sizes);

/** launch kernel with defined local size! */
int dt_opencl_enqueue_kernel_2d_with_local(const int dev, const int kernel, const size_t *sizes,
                                           const size_t *local);

/** check if opencl is inited */
int dt_opencl_is_inited(void);


/** check if opencl is enabled */
int dt_opencl_is_enabled(void);

/** disable opencl */
void dt_opencl_disable(void);

/** update enabled flag and profile with value from preferences, returns enabled flag */
int dt_opencl_update_settings(void);

/** HAVE_OPENCL mode only: copy and alloc buffers. */
int dt_opencl_copy_device_to_host(const int devid, void *host, void *device, const int width,
                                  const int height, const int bpp);

int dt_opencl_read_host_from_device(const int devid, void *host, void *device, const int width,
                                    const int height, const int bpp);

int dt_opencl_read_host_from_device_rowpitch(const int devid, void *host, void *device, const int width,
                                             const int height, const int rowpitch);

int dt_opencl_read_host_from_device_non_blocking(const int devid, void *host, void *device, const int width,
                                                 const int height, const int bpp);

int dt_opencl_read_host_from_device_rowpitch_non_blocking(const int devid, void *host, void *device,
                                                          const int width, const int height,
                                                          const int rowpitch);

int dt_opencl_read_host_from_device_raw(const int devid, void *host, void *device, const size_t *origin,
                                        const size_t *region, const int rowpitch, const int blocking);

int dt_opencl_write_host_to_device(const int devid, void *host, void *device, const int width,
                                   const int height, const int bpp);

int dt_opencl_write_host_to_device_rowpitch(const int devid, void *host, void *device, const int width,
                                            const int height, const int rowpitch);

int dt_opencl_write_host_to_device_non_blocking(const int devid, void *host, void *device, const int width,
                                                const int height, const int bpp);

int dt_opencl_write_host_to_device_rowpitch_non_blocking(const int devid, void *host, void *device,
                                                         const int width, const int height,
                                                         const int rowpitch);

int dt_opencl_write_host_to_device_raw(const int devid, const void *host, void *device, const size_t *origin,
                                       const size_t *region, const int rowpitch, const int blocking);

void *dt_opencl_copy_host_to_device(const int devid, void *host, const int width, const int height,
                                    const int bpp);

void *dt_opencl_copy_host_to_device_rowpitch(const int devid, void *host, const int width, const int height,
                                             const int bpp, const int rowpitch);

void *dt_opencl_copy_host_to_device_constant(const int devid, const size_t size, void *host);

int dt_opencl_enqueue_copy_image(const int devid, cl_mem src, cl_mem dst, size_t *orig_src, size_t *orig_dst,
                                 size_t *region);

void *dt_opencl_alloc_device(const int devid, const int width, const int height, const int bpp);

void *dt_opencl_alloc_device_use_host_pointer(const int devid, const int width, const int height,
                                              const int bpp, void *host, const int flags);

int dt_opencl_enqueue_copy_image_to_buffer(const int devid, cl_mem src_image, cl_mem dst_buffer,
                                           size_t *origin, size_t *region, size_t offset);

int dt_opencl_enqueue_copy_buffer_to_image(const int devid, cl_mem src_buffer, cl_mem dst_image,
                                           size_t offset, size_t *origin, size_t *region);

int dt_opencl_enqueue_copy_buffer_to_buffer(const int devid, cl_mem src_buffer, cl_mem dst_buffer,
                                            size_t srcoffset, size_t dstoffset, size_t size);

int dt_opencl_read_buffer_from_device(const int devid, void *host, void *device, const size_t offset,
                                      const size_t size, const int blocking);

int dt_opencl_write_buffer_to_device(const int devid, void *host, void *device, const size_t offset,
                                     const size_t size, const int blocking);

void *dt_opencl_alloc_device_buffer(const int devid, const size_t size);

void *dt_opencl_alloc_device_buffer_with_flags(const int devid, const size_t size, const int flags, void *host_ptr);

void dt_opencl_release_mem_object(cl_mem mem);

void *dt_opencl_map_buffer(const int devid, cl_mem buffer, const int blocking, const int flags, size_t offset,
                           size_t size);

void *dt_opencl_map_image(const int devid, cl_mem buffer, const int blocking, const int flags, size_t width, size_t height, int bpp);

int dt_opencl_unmap_mem_object(const int devid, cl_mem mem_object, void *mapped_ptr);

size_t dt_opencl_get_mem_object_size(cl_mem mem);

int dt_opencl_get_image_width(cl_mem mem);

int dt_opencl_get_image_height(cl_mem mem);

int dt_opencl_get_image_element_size(cl_mem mem);

int dt_opencl_get_mem_context_id(cl_mem mem);
cl_mem_flags dt_opencl_get_mem_flags(cl_mem mem);

// Track a cl_mem allocation/release in the per-device memory accounting.
// On OPENCL_MEMORY_ADD, pass the byte size that was requested for `mem` on device
// `devid`; it is recorded so the matching OPENCL_MEMORY_SUB can undo it exactly
// without ever asking the driver about the object (see mem_sizes in dt_opencl_t).
// On OPENCL_MEMORY_SUB, `devid` and `size` are ignored -- the recorded values win.
void dt_opencl_memory_statistics(int devid, cl_mem mem, size_t size, dt_opencl_memory_t action);

/** check if image size fit into limits given by OpenCL runtime */
gboolean dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height, const unsigned bpp,
                                const float factor, const size_t overhead);
/** Like dt_opencl_image_fits_device() but also reports *why* the image (didn't) fit.
 * `needed` and `limit` are filled with the two byte quantities that were actually compared
 * for the returned reason (both 0 for DIMENSION/UNINITED), so a caller can log a message
 * consistent with the real check instead of guessing a limit. Either out-param may be NULL. */
dt_opencl_fit_reason_t dt_opencl_image_fits_device_reason(const int devid, const size_t width,
                                const size_t height, const unsigned bpp, const float factor,
                                const size_t overhead, size_t *needed, size_t *limit);
/** get available memory for the device */
cl_ulong dt_opencl_get_device_available(const int devid);

/** check tuning settings and available memory for the device */
void dt_opencl_check_tuning(const int devid);

/** get size of allocatable single buffer */
cl_ulong dt_opencl_get_device_memalloc(const int devid);

/** round size to a multiple of the value given in the device specifig config parameter for opencl_size_roundup */
int dt_opencl_dev_roundup_width(int size, const int devid);
int dt_opencl_dev_roundup_height(int size, const int devid);

/** get next free slot in eventlist and manage size of eventlist */
cl_event *dt_opencl_events_get_slot(const int devid, const char *tag);

/** reset eventlist to empty state */
void dt_opencl_events_reset(const int devid);

/** Wait for events in eventlist to terminate -> this is a blocking synchronization point
    Does not flush eventlist */
void dt_opencl_events_wait_for(const int devid);

/** Wait for events in eventlist to terminate, check for return status of events and
    report summary success info (CL_COMPLETE or last error code) */
cl_int dt_opencl_events_flush(const int devid, const int reset);

/** display OpenCL profiling information. If summary is not 0, try to generate summarized info for kernels */
void dt_opencl_events_profiling(const int devid, const int aggregated);

/** utility function to calculate optimal work group dimensions for a given kernel */
int dt_opencl_local_buffer_opt(const int devid, const int kernel, dt_opencl_local_buffer_t *factors);

/** utility functions handling device specific properties */
void dt_opencl_write_device_config(const int devid);
gboolean dt_opencl_read_device_config(const int devid);
/**
 * Number of GPU devices detected during OpenCL probing, including devices disabled by user preference.
 */
int dt_opencl_get_detected_device_count(void);
/**
 * Return metadata for a detected GPU, or NULL when the detected-device index is invalid.
 */
const dt_opencl_detected_device_t *dt_opencl_get_detected_device(const int detected);
/**
 * Return whether a detected GPU is enabled by its per-device OpenCL preference.
 */
gboolean dt_opencl_detected_device_enabled(const int detected);
/**
 * Persist the per-device OpenCL preference and derive the global OpenCL switch from all GPU states.
 */
int dt_opencl_set_detected_device_enabled(const int detected, const gboolean enabled);
/**
 * Return whether pinned memory transfer is enabled for a detected GPU.
 */
gboolean dt_opencl_detected_device_pinned_memory(const int detected);
/**
 * Persist the per-device pinned memory transfer preference.
 */
int dt_opencl_set_detected_device_pinned_memory(const int detected, const gboolean enabled);
/**
 * Return the per-device GPU vRAM headroom in MiB.
 */
size_t dt_opencl_detected_device_headroom(const int detected);
/**
 * Persist the per-device GPU vRAM headroom in MiB.
 */
int dt_opencl_set_detected_device_headroom(const int detected, const size_t headroom);
int dt_opencl_avoid_atomics(const int devid);
int dt_opencl_micro_nap(const int devid);
gboolean dt_opencl_use_pinned_memory(const int devid);
gboolean dt_opencl_is_pinned_memory(cl_mem mem);

#ifdef __cplusplus
}
#endif

#else

#include "common/conf.h"
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque here too, so the type name means the same thing in both configurations: private to
 * common/opencl.c, and impossible to instantiate anywhere else. In this build there is no
 * state at all -- every query below is a constant. */
typedef struct dt_opencl_t dt_opencl_t;
typedef struct dt_opencl_detected_device_t
{
  int config_id;
  char *name;
  char *cname;
  unsigned int cltype;
  int disabled;
  int pinned_memory;
  size_t forced_headroom;
  // Mirrors dt_opencl_device_t.host_unified_memory -- lets GUI code (preferences) label the
  // headroom setting accurately for integrated GPUs without reaching into the live device array.
  gboolean host_unified_memory;
} dt_opencl_detected_device_t;
static inline void dt_opencl_init(const gboolean exclude_opencl, const gboolean print_statistics)
{
  /* There is no state to initialise in this build: the queries below are constants. */
  dt_conf_set_bool("opencl", FALSE);
  dt_print(DT_DEBUG_OPENCL, "[opencl_init] this version of darktable was built without opencl support\n");
}
static inline void dt_opencl_cleanup(void)
{
}
static inline gboolean dt_opencl_finish(const int devid)
{
  return -1;
}
static inline int dt_opencl_enqueue_barrier(const int devid)
{
  return -1;
}
static inline int dt_opencl_reserve_device_for_pipe(const int pipetype)
{
  return -1;
}
static inline void dt_opencl_reserve_device_by_id(const int devid)
{
}
static inline int dt_opencl_try_reserve_device_by_id(const int devid)
{
  return 1; // never free: there are no devices
}
static inline void dt_opencl_release_device(const int devid)
{
}
static inline int dt_opencl_get_num_devices(void)
{
  return 0;
}
static inline const char *dt_opencl_get_device_name(const int devid)
{
  return NULL;
}
static inline gboolean dt_opencl_get_device_max_image_size(const int devid, int *width, int *height)
{
  return FALSE;
}
static inline size_t dt_opencl_get_device_max_global_mem(const int devid)
{
  return 0;
}
static inline int dt_opencl_report_pipe_error(void)
{
  return 2;
}
static inline int dt_opencl_load_program(const int dev, const char *filename)
{
  return -1;
}
static inline int dt_opencl_build_program(const int dev, const int program)
{
  return -1;
}
static inline int dt_opencl_create_kernel(const int program, const char *name)
{
  return -1;
}
static inline void dt_opencl_free_kernel(const int kernel)
{
}
static inline int dt_opencl_get_max_work_item_sizes(const int dev, size_t *sizes)
{
  return -1;
}
static inline int dt_opencl_get_work_group_limits(const int dev, size_t *sizes, size_t *workgroupsize,
                                                  unsigned long *localmemsize)
{
  return -1;
}
static inline int dt_opencl_get_kernel_work_group_size(const int dev, const int kernel,
                                                       size_t *kernelworkgroupsize)
{
  return -1;
}
static inline int dt_opencl_set_kernel_arg(const int dev, const int kernel, const size_t size, const void *arg)
{
  return -1;
}
static inline int dt_opencl_enqueue_kernel_2d(const int dev, const int kernel, const size_t *sizes)
{
  return -1;
}
static inline int dt_opencl_enqueue_kernel_2d_with_local(const int dev, const int kernel, const size_t *sizes,
                                                         const size_t *local)
{
  return -1;
}
static inline int dt_opencl_is_inited(void)
{
  return 0;
}
static inline int dt_opencl_is_enabled(void)
{
  return 0;
}
static inline void dt_opencl_disable(void)
{
}
static inline int dt_opencl_update_settings(void)
{
  return 0;
}
static inline int dt_opencl_get_detected_device_count(void)
{
  return 0;
}
static inline const dt_opencl_detected_device_t *dt_opencl_get_detected_device(const int detected)
{
  return NULL;
}
static inline gboolean dt_opencl_detected_device_enabled(const int detected)
{
  return FALSE;
}
static inline int dt_opencl_set_detected_device_enabled(const int detected, const gboolean enabled)
{
  return -1;
}
static inline gboolean dt_opencl_detected_device_pinned_memory(const int detected)
{
  return FALSE;
}
static inline int dt_opencl_set_detected_device_pinned_memory(const int detected, const gboolean enabled)
{
  return -1;
}
static inline size_t dt_opencl_detected_device_headroom(const int detected)
{
  return 0;
}
static inline int dt_opencl_set_detected_device_headroom(const int detected, const size_t headroom)
{
  return -1;
}
static inline gboolean dt_opencl_image_fits_device(const int devid, const size_t width, const size_t height,
                                              const unsigned bpp, const float factor, const size_t overhead)
{
  return FALSE;
}
static inline size_t dt_opencl_get_device_available(const int devid)
{
  return 0;
}
static inline void dt_opencl_check_tuning(const int devid)
{
  return;
}
static inline size_t dt_opencl_get_device_memalloc(const int devid)
{
  return 0;
}
static inline unsigned long dt_opencl_get_mem_flags(void *mem)
{
  return 0;
}
static inline void dt_opencl_release_mem_object(void *mem)
{
}
static inline void *dt_opencl_events_get_slot(const int devid, const char *tag)
{
  return NULL;
}
static inline void dt_opencl_events_reset(const int devid)
{
}
static inline void dt_opencl_events_wait_for(const int devid)
{
}
static inline int dt_opencl_events_flush(const int devid, const int reset)
{
  return 0;
}
static inline void dt_opencl_events_profiling(const int devid, const int aggregated)
{
}

#ifdef __cplusplus
}
#endif

#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
}
#endif

#endif // DT_COMMON_OPENCL_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
