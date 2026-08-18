/*
    This file is part of Ansel.
    Copyright (C) 2026 Ansel developers.

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

/* Measures how accurate each OpenCL device's math library is under each kernel build option,
 * and prints the `building` line to paste into anselrc for any device that needs one.
 *
 * Why this exists: `-cl-unsafe-math-optimizations` (and `-cl-fast-relaxed-math`, which implies
 * it) permits the driver to substitute a low-precision implementation for any libm function.
 * Whether that is harmless or catastrophic is a property of the driver, not of the flag, so it
 * cannot be decided in the source -- it has to be measured on the machine that will run it.
 * On an Intel HD Graphics P630 the flag makes erf() return exactly 0.0 for |x| < 1e-3, which
 * silently corrupts the neural raw denoiser's GELU. On NVIDIA the same flag leaves erf() exact
 * but costs four orders of magnitude on log(). See doc/opencl-math-accuracy.md.
 *
 * Build (needs only an OpenCL ICD loader and libm; it does not link against Ansel):
 *
 *     gcc -O2 -o opencl-math-accuracy tools/opencl-math-accuracy.c -lOpenCL -lm
 *
 * If your distribution ships no libOpenCL.so development symlink, link the runtime directly:
 *
 *     gcc -O2 -o opencl-math-accuracy tools/opencl-math-accuracy.c /lib64/libOpenCL.so.1 -lm
 *
 * Run it with no arguments. The CPU row is the control: it is this program's own host
 * arithmetic, so building the tool at -O2 and again at -O3 -ffast-math also answers "does the
 * CPU path lose accuracy at higher optimisation levels" for the same expressions.
 */

#define CL_TARGET_OPENCL_VERSION 300
#include <CL/cl.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Samples spanning the range a convolutional network's activations actually occupy. The small
// |x| end is the interesting one: that is where a relaxed erf() implementation flushes to zero
// and where most activations live.
#define NSAMPLES 200000
#define XMIN (-8.0f)
#define XMAX (8.0f)

// A device is reported as failing if any probed function exceeds this relative error. Correct
// single precision lands around 1e-7; a genuinely relaxed-but-usable implementation lands
// around 1e-5. Anything past 1e-4 is a different function, not a rounding difference.
#define FAIL_THRESHOLD 1e-4
// Past this the implementation is not merely relaxed, it is returning something else entirely
// (Intel's relaxed erf() flushes small arguments to exactly 0.0 and scores ~1.0 here).
#define BROKEN_THRESHOLD 1e-2

static const char *KERNEL_SRC =
    "__kernel void probe(__global const float *in, __global float *out, const int op)\n"
    "{\n"
    "  const int i = get_global_id(0);\n"
    "  const float x = in[i];\n"
    "  float r;\n"
    "  switch(op)\n"
    "  {\n"
    "    case 0: r = erf(x); break;\n"
    "    case 1: r = 0.5f * x * (1.0f + erf(x * 0.70710678118654752f)); break;\n"
    "    case 2: r = exp(x); break;\n"
    "    case 3: r = log(fabs(x) + 1.0f); break;\n"
    "    case 4: r = tanh(x); break;\n"
    "    case 5: r = pow(fabs(x) + 1.0f, 2.4f); break;\n"
    "    case 6: r = sqrt(fabs(x)); break;\n"
    "    case 7: r = 1.0f / (fabs(x) + 1.0f); break;\n"
    "    default: r = x; break;\n"
    "  }\n"
    "  out[i] = r;\n"
    "}\n";

typedef enum { OP_ERF = 0, OP_GELU, OP_EXP, OP_LOG, OP_TANH, OP_POW, OP_SQRT, OP_DIV, N_OPS } op_t;

static const char *OP_NAME[N_OPS] = { "erf", "GELU", "exp", "log", "tanh", "pow", "sqrt", "divide" };

// Reference in double precision. Kept in its own function so that a fast-math build of this
// tool relaxes the probe and the reference alike -- which is exactly what we want to see when
// the tool is used to compare CPU optimisation levels.
static double reference(const op_t op, const double x)
{
  switch(op)
  {
    case OP_ERF:  return erf(x);
    case OP_GELU: return 0.5 * x * (1.0 + erf(x * 0.70710678118654752));
    case OP_EXP:  return exp(x);
    case OP_LOG:  return log(fabs(x) + 1.0);
    case OP_TANH: return tanh(x);
    case OP_POW:  return pow(fabs(x) + 1.0, 2.4);
    case OP_SQRT: return sqrt(fabs(x));
    case OP_DIV:  return 1.0 / (fabs(x) + 1.0);
    default:      return x;
  }
}

// The host control, mirroring the kernel expression by expression.
static void host_probe(const float *const in, float *const out, const int n, const op_t op)
{
  for(int i = 0; i < n; i++)
  {
    const float x = in[i];
    switch(op)
    {
      case OP_ERF:  out[i] = erff(x); break;
      case OP_GELU: out[i] = 0.5f * x * (1.0f + erff(x * 0.70710678118654752f)); break;
      case OP_EXP:  out[i] = expf(x); break;
      case OP_LOG:  out[i] = logf(fabsf(x) + 1.0f); break;
      case OP_TANH: out[i] = tanhf(x); break;
      case OP_POW:  out[i] = powf(fabsf(x) + 1.0f, 2.4f); break;
      case OP_SQRT: out[i] = sqrtf(fabsf(x)); break;
      case OP_DIV:  out[i] = 1.0f / (fabsf(x) + 1.0f); break;
      default:      out[i] = x; break;
    }
  }
}

/* Candidate build option sets, in the order a reader should think about them: nothing, then one
 * flag at a time, then the two compound flags, then what Ansel actually ships. `safe` marks the
 * sets we would be willing to fall back to. */
typedef struct
{
  const char *flags;
  const char *label;
  int is_ansel_default;
} option_set_t;

static const option_set_t OPTION_SETS[] = {
  { "", "(none)", 0 },
  { "-cl-mad-enable", "-cl-mad-enable", 0 },
  { "-cl-no-signed-zeros", "-cl-no-signed-zeros", 0 },
  { "-cl-denorms-are-zero", "-cl-denorms-are-zero", 0 },
  { "-cl-finite-math-only", "-cl-finite-math-only", 0 },
  { "-cl-unsafe-math-optimizations", "-cl-unsafe-math-optimizations", 0 },
  { "-cl-fast-relaxed-math", "-cl-fast-relaxed-math", 0 },
  { "-cl-mad-enable -cl-no-signed-zeros", "SAFE SET (mad-enable + no-signed-zeros)", 0 },
  { "-cl-fast-relaxed-math -cl-no-signed-zeros -cl-unsafe-math-optimizations",
    "ANSEL LEGACY DEFAULT (fast-relaxed + unsafe)", 1 },
};
#define N_OPTION_SETS ((int)(sizeof(OPTION_SETS) / sizeof(OPTION_SETS[0])))

// Same rule as _ascii_str_canonical() in src/common/opencl.c: keep alphanumerics, lowercase.
// Reproduced here so the tool can print the exact anselrc key without linking against Ansel.
static void canonical_name(const char *in, char *out, const size_t maxlen)
{
  size_t len = 0;
  for(; *in != '\0' && len + 1 < maxlen; in++)
    if(isalnum((unsigned char)*in)) out[len++] = (char)tolower((unsigned char)*in);
  out[len] = '\0';
}

/* Error relative to the reference, but with the denominator floored at a thousandth of the
 * function's own peak over the sweep.
 *
 * A plain relative error is unusable for GELU: 0.5*x*(1+erf(x/sqrt(2))) is a difference of two
 * nearly equal numbers once erf saturates to -1, so for x well below zero the result is ~1e-9
 * built out of cancellation and ANY implementation, including a perfect one, scores a huge
 * relative error there. Flooring the denominator says "an absolute error this far below the
 * function's working range is not interesting", which suppresses that artifact.
 *
 * It deliberately does NOT suppress the failure we are hunting. erf peaks at 1, so its floor is
 * 1e-3; Intel's relaxed erf returns exactly 0 where the true value is 1.13e-3, which still
 * scores ~1.0. Sensitivity is kept exactly where the mechanism lives. */
static double max_error(const float *const got, const float *const in, const int n, const op_t op)
{
  double peak = 0.0;
  for(int i = 0; i < n; i++)
  {
    const double mag = fabs(reference(op, (double)in[i]));
    if(mag > peak) peak = mag;
  }
  const double floor_mag = peak * 1e-3;

  double worst = 0.0;
  for(int i = 0; i < n; i++)
  {
    const double ref = reference(op, (double)in[i]);
    const double denom = fmax(fabs(ref), floor_mag);
    if(denom <= 0.0) continue;
    const double rel = fabs((double)got[i] - ref) / denom;
    if(rel > worst) worst = rel;
  }
  return worst;
}

static void print_header(void)
{
  printf("%-46s", "build options");
  for(int o = 0; o < N_OPS; o++) printf("%11s", OP_NAME[o]);
  printf("\n");
  for(int i = 0; i < 46 + 11 * N_OPS; i++) putchar('-');
  printf("\n");
}

int main(void)
{
  float *in = (float *)malloc(sizeof(float) * NSAMPLES);
  float *out = (float *)malloc(sizeof(float) * NSAMPLES);
  if(!in || !out) { fprintf(stderr, "out of memory\n"); return 1; }
  for(int i = 0; i < NSAMPLES; i++)
    in[i] = XMIN + (XMAX - XMIN) * (float)i / (float)(NSAMPLES - 1);

  printf("Ansel OpenCL math accuracy probe\n");
  printf("max relative error against a double-precision reference, x in [%.0f, %.0f]\n"
         "a correct single-precision implementation scores ~1e-7; anything above %.0e is a\n"
         "different function, not a rounding difference\n\n", (double)XMIN, (double)XMAX, FAIL_THRESHOLD);

  // ---- host control -------------------------------------------------------------------
  printf("================ CPU (this tool's own build) ================\n");
  print_header();
  printf("%-46s", "host libm");
  for(int o = 0; o < N_OPS; o++)
  {
    host_probe(in, out, NSAMPLES, (op_t)o);
    printf("%11.2e", max_error(out, in, NSAMPLES, (op_t)o));
  }
  printf("\n\nRebuild this tool at -O2, at -O3, and at -O3 -ffast-math to compare the CPU path\n"
         "across optimisation levels; the row above reflects whichever was used.\n");

  // ---- devices ------------------------------------------------------------------------
  cl_platform_id platforms[16];
  cl_uint n_platforms = 0;
  if(clGetPlatformIDs(16, platforms, &n_platforms) != CL_SUCCESS || n_platforms == 0)
  {
    printf("\nNo OpenCL platform found.\n");
    free(in); free(out);
    return 0;
  }

  int device_index = 0;   // Ansel numbers usable devices in enumeration order
  for(cl_uint p = 0; p < n_platforms; p++)
  {
    cl_device_id devices[8];
    cl_uint n_devices = 0;
    if(clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_GPU, 8, devices, &n_devices) != CL_SUCCESS) continue;

    for(cl_uint d = 0; d < n_devices; d++)
    {
      char name[256] = { 0 };
      clGetDeviceInfo(devices[d], CL_DEVICE_NAME, sizeof(name), name, NULL);

      cl_int err = CL_SUCCESS;
      cl_context ctx = clCreateContext(NULL, 1, &devices[d], NULL, NULL, &err);
      if(err != CL_SUCCESS) { printf("\n%s: cannot create a context (%d), skipping\n", name, err); continue; }
      cl_command_queue queue = clCreateCommandQueueWithProperties(ctx, devices[d], NULL, &err);
      cl_mem d_in = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                   sizeof(float) * NSAMPLES, in, &err);
      cl_mem d_out = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, sizeof(float) * NSAMPLES, NULL, &err);

      printf("\n================ device %d: %s ================\n", device_index, name);
      print_header();

      int best_safe = -1;             // richest option set that stayed accurate
      int default_fails = 0;
      for(int f = 0; f < N_OPTION_SETS; f++)
      {
        cl_program program = clCreateProgramWithSource(ctx, 1, &KERNEL_SRC, NULL, &err);
        if(clBuildProgram(program, 1, &devices[d], OPTION_SETS[f].flags, NULL, NULL) != CL_SUCCESS)
        {
          printf("%-46s  build failed\n", OPTION_SETS[f].label);
          clReleaseProgram(program);
          continue;
        }
        cl_kernel kernel = clCreateKernel(program, "probe", &err);

        printf("%-46s", OPTION_SETS[f].label);
        int fails = 0, broken = 0;
        for(int o = 0; o < N_OPS; o++)
        {
          clSetKernelArg(kernel, 0, sizeof(cl_mem), &d_in);
          clSetKernelArg(kernel, 1, sizeof(cl_mem), &d_out);
          clSetKernelArg(kernel, 2, sizeof(int), &o);
          size_t global = NSAMPLES;
          clEnqueueNDRangeKernel(queue, kernel, 1, NULL, &global, NULL, 0, NULL, NULL);
          clFinish(queue);
          clEnqueueReadBuffer(queue, d_out, CL_TRUE, 0, sizeof(float) * NSAMPLES, out, 0, NULL, NULL);

          const double rel = max_error(out, in, NSAMPLES, (op_t)o);
          if(rel > FAIL_THRESHOLD) fails++;
          if(rel > BROKEN_THRESHOLD) broken++;
          printf("%11.2e", rel);
        }
        printf("%s\n", broken ? "   <-- BROKEN" : (fails ? "   <-- degraded" : ""));

        if(!fails) best_safe = f;
        if(OPTION_SETS[f].is_ansel_default && broken) default_fails = 1;

        clReleaseKernel(kernel);
        clReleaseProgram(program);
      }

      char cname[256];
      canonical_name(name, cname, sizeof(cname));
      printf("\n");
      if(default_fails)
      {
        printf("VERDICT: this device's math library is NOT accurate under Ansel's legacy default.\n");
        printf("Put this in anselrc (it is per device, and Ansel rewrites it only if absent):\n\n");
        printf("    cldevice_v4/%d/%s/building=%s\n\n", device_index, cname,
               best_safe >= 0 ? OPTION_SETS[best_safe].flags : "-cl-mad-enable -cl-no-signed-zeros");
        printf("then delete %s's cached kernels so they rebuild:\n", name);
        printf("    rm -rf ~/.cache/ansel/cached_kernels_for_*\n");
      }
      else
      {
        printf("VERDICT: accurate under every option set probed; no anselrc change needed.\n");
        printf("Current key would be: cldevice_v4/%d/%s/building=...\n", device_index, cname);
      }

      clReleaseMemObject(d_in);
      clReleaseMemObject(d_out);
      clReleaseCommandQueue(queue);
      clReleaseContext(ctx);
      device_index++;
    }
  }

  free(in);
  free(out);
  return 0;
}
