# Building Ansel with MSVC (Visual Studio)

## MSVC support status

MSVC is **not officially supported** by the Ansel project.  
Partial support has been added (CMake toolchain, header compatibility, preprocessor guards), but a full build is **not yet functional** with plain `cl.exe` due to the heavy use of GCC vector extensions (`__attribute__((vector_size(...)))`) throughout `src/common/darktable.h` and dependent modules.

**Status as of April 28, 2026:**
- ✅ CMake configure completes fully (using `build_msvc_clean` and the options listed below)
- ✅ RawSpeed compiles (`rawspeed_*` libs)
- ✅ External libraw via vcpkg integrated and found
- ❌ `lib_ansel_imageio_rawspeed`: temporarily disabled on MSVC (GCC vector extensions)
- ❌ `lib_ansel_imageio_libraw`: blocked by GCC SIMD/vector layer in `darktable.h`
- ❌ Full `lib_ansel`: not reached

The most pragmatic path to a complete build is **`clang-cl`** (MSVC ABI, Clang compiler). See Method 3.

## Basic setup

### Prerequisites

1. **Visual Studio 2022** (tested with MSVC 19.51 / generator "Visual Studio 18 2026")
2. **CMake 3.19+**
3. **vcpkg** (required — MSYS2/ucrt64 packages **must NOT** be in PATH during MSVC builds)

If you do not have Visual Studio yet, download **Build Tools 2022** from Microsoft. The full IDE is not required.

> **Important**: Never mix MSYS2 headers (e.g. `C:/msys64/ucrt64/include`) into an MSVC build.
> Use a clean build directory and restrict `PKG_CONFIG_LIBDIR` to vcpkg `.pc` files only
> (see the recommended configure command below).

### Installing dependencies (via vcpkg — required)

vcpkg port names do not always match the CMake package names used by the project.  
Example: `find_package(LCMS2)` → port `lcms`; `find_package(LensFun)` → port `lensfun`.

From `x64 Native Tools Command Prompt for VS 2022`:

```bat
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
.\bootstrap-vcpkg.bat

.\vcpkg install ^
  lcms:x64-windows ^
  libjpeg-turbo:x64-windows ^
  libpng:x64-windows ^
  tiff:x64-windows ^
  lensfun:x64-windows ^
  sqlite3:x64-windows ^
  exiv2:x64-windows ^
  pugixml:x64-windows ^
  curl:x64-windows ^
  zlib:x64-windows ^
  libxml2:x64-windows ^
  libxslt:x64-windows ^
  libraw:x64-windows
```

> **Do not install**: `icu` and `libavif` fail to build via vcpkg under MSVC.  
> Disable them in CMake with `-DUSE_ICU=OFF -DUSE_AVIF=OFF`.

## Building

### Method 1: Visual Studio Generator (partially functional)

Requires Developer Command Prompt:

```bat
cd C:\path\to\ansel
mkdir build_msvc_clean
cd build_msvc_clean

set PKG_CONFIG_LIBDIR=C:/path/to/vcpkg/installed/x64-windows/lib/pkgconfig;C:/path/to/vcpkg/installed/x64-windows/share/pkgconfig

cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain_msvc.cmake ^
  -DCMAKE_TOOLCHAIN_FILE_VCPKG="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake" ^
  -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF ^
  -DCMAKE_IGNORE_PREFIX_PATH=C:/msys64/ucrt64 ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  -DUSE_OPENMP=OFF ^
  -DUSE_OPENCL=OFF ^
  -DUSE_ICU=OFF ^
  -DUSE_AVIF=OFF ^
  -DUSE_COLORD=OFF ^
  -DUSE_KWALLET=OFF ^
  -DUSE_LIBRAW=ON ^
  -DUSE_BUNDLED_LIBRAW=OFF ^
  -DBUILD_TESTING=OFF ^
  -G "Visual Studio 18 2026" ^
  -A x64 ^
  ..

cmake --build . --config Release --parallel 4
```

### Method 2: Ninja + MSVC

```bat
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain_msvc.cmake ^
  -DCMAKE_TOOLCHAIN_FILE_VCPKG="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake" ^
  -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF ^
  -DCMAKE_IGNORE_PREFIX_PATH=C:/msys64/ucrt64 ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  -DUSE_OPENMP=OFF ^
  -DUSE_OPENCL=OFF ^
  -DUSE_ICU=OFF ^
  -DUSE_AVIF=OFF ^
  -DUSE_COLORD=OFF ^
  -DUSE_KWALLET=OFF ^
  -DUSE_LIBRAW=ON ^
  -DUSE_BUNDLED_LIBRAW=OFF ^
  -DBUILD_TESTING=OFF ^
  -G "Ninja" ^
  -DCMAKE_BUILD_TYPE=Release ^
  ..

ninja
```

### Method 3: clang-cl (recommended to go further)

`clang-cl` uses the MSVC ABI but accepts GCC extensions, which removes the SIMD/vector blockers in the core of the project.

```bat
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain_msvc.cmake ^
  -DUSE_CLANG_CL=ON ^
  -DCMAKE_TOOLCHAIN_FILE_VCPKG="C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake" ^
  -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF ^
  -DCMAKE_IGNORE_PREFIX_PATH=C:/msys64/ucrt64 ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  -DUSE_OPENMP=OFF ^
  -DUSE_OPENCL=OFF ^
  -DUSE_ICU=OFF ^
  -DUSE_AVIF=OFF ^
  -DUSE_COLORD=OFF ^
  -DUSE_KWALLET=OFF ^
  -DUSE_LIBRAW=ON ^
  -DUSE_BUNDLED_LIBRAW=OFF ^
  -DBUILD_TESTING=OFF ^
  -G "Ninja" ^
  -DCMAKE_BUILD_TYPE=Release ^
  ..

ninja
```

## Key CMake options for MSVC

| Option | Recommended value | Notes |
|--------|------------------|-------|
| `USE_OPENMP` | `OFF` | Unstable with plain cl.exe for now |
| `USE_OPENCL` | `OFF` | Requires clang to compile OpenCL programs |
| `USE_ICU` | `OFF` | vcpkg icu port fails to build |
| `USE_AVIF` | `OFF` | pkg-config fallback causes a configure error |
| `USE_COLORD` | `OFF` | Linux only |
| `USE_KWALLET` | `OFF` | KDE/Linux only |
| `USE_LIBRAW` | `ON` | Enable libraw (RAW fallback loader) |
| `USE_BUNDLED_LIBRAW` | `OFF` | Use vcpkg libraw (`libraw:x64-windows`) |
| `CMAKE_POLICY_VERSION_MINIMUM` | `3.5` | Required with some recent CMake versions |
| `CMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH` | `OFF` | Prevents CMake from picking up MSYS2 paths |
| `CMAKE_IGNORE_PREFIX_PATH` | `C:/msys64/ucrt64` | Explicitly exclude the MSYS2 toolchain |
| `BUILD_TESTING` | `OFF` | Skip test targets that do not build yet |

## Dependencies — actual status

### ✅ Working via vcpkg
- `LCMS2` (port `lcms`)
- JPEG (`libjpeg-turbo`), PNG, TIFF
- SQLite3
- Exiv2
- curl
- pugixml, zlib
- libxml2, libxslt
- External LibRaw (`libraw:x64-windows` + `USE_BUNDLED_LIBRAW=OFF`)
- OpenMP (native MSVC `/openmp`)
- Threads (native Windows)

### ⚠️ Partially supported
- **RawSpeed**: builds as a standalone library, but `lib_ansel_imageio_rawspeed` is disabled
  on MSVC (uses `dt_aligned_pixel_simd_t` and GCC vector extensions). Works with `clang-cl`.
- **LibRaw imageio loader**: configure finds the library, but compiling `imageio_libraw.c`
  fails with plain `cl.exe` (same SIMD blocker through `darktable.h`).
- **pkg-config**: worked around by restricting `PKG_CONFIG_LIBDIR` to vcpkg `.pc` files only.
  Without this, CMake may pick up incompatible MSYS2 `.pc` files.

### ❌ Not supported / must be disabled
- **colord**: Linux only → `USE_COLORD=OFF`
- **kwallet**: KDE/Linux only → `USE_KWALLET=OFF`
- **ICU**: vcpkg `icu` port fails to build → `USE_ICU=OFF`
- **AVIF**: pkg-config fallback causes a configure error → `USE_AVIF=OFF`
- **intltool-merge**, **desktop-file-validate**, **appstream-util**: Unix tools
- **GTK3**: Windows version via vcpkg required but untested

## Files modified for MSVC support

### `cmake/toolchain_msvc.cmake` (new file)
- Optional chaining of `CMAKE_TOOLCHAIN_FILE_VCPKG`
- Compiler selection: `cl.exe` or `clang-cl` depending on `USE_CLANG_CL`
- MSVC flags: `/W3 /MP /FS /utf-8`
- Definitions: `_CRT_SECURE_NO_WARNINGS`, `WIN32_LEAN_AND_MEAN`, `NOMINMAX`, `_USE_MATH_DEFINES`
- `/MP /FS` together: parallel compilation + serialized PDB writes (prevents `C1041`)

### `cmake/march-mtune.cmake`
- Early exit for MSVC (MSVC does not support `-march` / `-mtune`)

### `cmake/compiler-warnings.cmake`
- GCC/Clang warning flags wrapped in `if(NOT MSVC)`

### `ConfigureChecks.cmake`
- TLS: MSVC uses `__declspec(thread)` instead of `__thread`

### `src/external/CMakeLists.txt`
- `-Wno-error` and `-w` flags conditional: `if(NOT MSVC)`
- Adds `/Zc:preprocessor /FS` before `add_subdirectory(rawspeed)` when MSVC
  (RawSpeed uses `__VA_OPT__` which requires the conforming preprocessor)

### `src/external/rawspeed/src/config.h.in`
- MSVC shims in a `#if defined(_MSC_VER)` block:
  - `__attribute__(x)` → no-op
  - `__builtin_unreachable()` → `__assume(0)`
  - `__builtin_expect(e,c)` → `(e)`
  - `__PRETTY_FUNCTION__` → `__FUNCSIG__`
  - C++ templates for `__builtin_sadd_overflow` / `__builtin_mul_overflow`
  - `RAWSPEED_NOINLINE` / `RAWSPEED_UNLIKELY_FUNCTION` → `__declspec(noinline)`

### `src/external/rawspeed/cmake/Modules/CheckZLIB.cmake`
- Clang warning flags in compile checks wrapped in `if(NOT MSVC)`

### `src/config.cmake.h`
- `dt_supported_extensions`: `__attribute__((unused))` disabled on MSVC via `#if defined(_MSC_VER)`

### `src/common/fp_mode.h`
- `DT_ALWAYS_INLINE`: `__forceinline` on MSVC, `__attribute__((always_inline)) inline` elsewhere

### `src/win/getrusage.h`
- Replaced `#include <sys/time.h>` with `#include <winsock2.h>` on `_WIN32`

### `src/common/darktable.h`
- POSIX includes conditionally replaced on Windows: `winsock2.h` / `io.h` instead of
  `sys/time.h`, `sys/types.h`, `unistd.h`
- **Remaining blocker**: the GCC SIMD section (`dt_aligned_pixel_simd_t`, `vector_size`,
  `__builtin_memcpy`, etc.) around lines 529–690 does not compile with `cl.exe`

### `src/CMakeLists.txt`
- `lib_ansel_imageio_rawspeed` disabled on MSVC with an explicit warning message
- Link to `lib_ansel_imageio_rawspeed` in `lib_ansel` is conditional via `if(TARGET ...)`

## Known issues and solutions

### 1. "cl.exe not found"

**Solution**: Use the *Developer Command Prompt for VS 2022* or source:
```bat
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
```

### 2. C1041: PDB conflicts during parallel builds

**Cause**: multiple `cl.exe` processes try to write to the same `.pdb` file simultaneously.  
**Solution**: `/MP /FS` together in `toolchain_msvc.cmake` — already applied.

### 3. MSYS2 headers leaking into the MSVC build

**Cause**: `CMAKE_SYSTEM_INCLUDE_PATH` or `PKG_CONFIG_PATH` contains `C:/msys64/ucrt64/include`.  
**Solution**:
```bat
set PKG_CONFIG_LIBDIR=C:/vcpkg/installed/x64-windows/lib/pkgconfig
cmake ... -DCMAKE_FIND_USE_SYSTEM_ENVIRONMENT_PATH=OFF -DCMAKE_IGNORE_PREFIX_PATH=C:/msys64/ucrt64
```

### 4. "LNK2019 unresolved external symbol"

**Cause**: missing vcpkg dependencies, or mixed C++ runtimes (`/MD` vs `/MT`).  
**Solution**:
```bat
cmake ... -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL
```

### 5. ICU or AVIF not found / configure error

**Solution**:
```bat
cmake ... -DUSE_ICU=OFF -DUSE_AVIF=OFF
```

### 6. OpenCL programs "not compiled"

**Solution**:
```bat
cmake ... -DUSE_OPENCL=OFF
```

### 7. GCC SIMD / vector errors in `darktable.h`

**Cause**: `cl.exe` does not support `__attribute__((vector_size(...)))` or GCC vector initializers.  
**Solution**: Use `clang-cl` (`-DUSE_CLANG_CL=ON`). With plain `cl.exe`, a dedicated SIMD
portability layer in `darktable.h` is required.

### 8. `cmake_policy_version_minimum` warning

**Solution**:
```bat
cmake ... -DCMAKE_POLICY_VERSION_MINIMUM=3.5
```

## Toolchain architecture

```
toolchain_msvc.cmake
├── vcpkg chaining (CMAKE_TOOLCHAIN_FILE_VCPKG)
├── Compiler selection (cl.exe or clang-cl via USE_CLANG_CL)
├── C11 / C++17 standards
├── MSVC flags (/W3, /MP, /FS, /utf-8)
├── MSVC definitions (_CRT_SECURE_NO_WARNINGS, NOMINMAX, _USE_MATH_DEFINES…)
└── OpenMP (/openmp for MSVC, -fopenmp for clang-cl)
```

## Current limitations

1. **GCC SIMD/vector extensions**: `darktable.h` makes heavy use of `__attribute__((vector_size(...)))` — incompatible with `cl.exe`
2. **`lib_ansel_imageio_rawspeed`**: disabled on MSVC (same SIMD reason)
3. **ICU, AVIF**: cannot be built via vcpkg under MSVC at this time
4. **Unix build tools**: `intltool-merge`, `desktop-file-validate`, `appstream-util` unavailable
5. **GTK3**: Windows version via vcpkg not tested
6. **Distribution**: install scripts currently target MinGW only

## Recommended next steps

1. ✅ **Base MSVC toolchain**: Done
2. ✅ **External libraw via vcpkg**: Done
3. ✅ **RawSpeed MSVC shims**: Done
4. ✅ **MSYS2 header isolation**: Done
5. ⏳ **SIMD portability layer in `darktable.h` for `cl.exe`**: current blocker — or switch to clang-cl
6. ⏳ **Full build with clang-cl**: to be validated
7. ⏳ **Windows CI/CD tests**: to be implemented
8. ⏳ **MSVC Windows distribution**: to be adapted

---

For more information, see:
- [CMake MSVC Documentation](https://cmake.org/cmake/help/latest/generator/Visual%20Studio%2017%202022.html)
- [vcpkg Documentation](https://vcpkg.io/)
- [clang-cl Documentation](https://clang.llvm.org/docs/MSVCCompatibility.html)
