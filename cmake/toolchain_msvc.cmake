# =============================================================================
# CMake toolchain file for MSVC (Visual Studio) on Windows
# =============================================================================
# Usage:
#   mkdir build_msvc && cd build_msvc
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain_msvc.cmake -G "Visual Studio 17 2022" ..
#
# Or for clang-cl (LLVM with MSVC compatibility):
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain_msvc.cmake -DUSE_CLANG_CL=ON -G "Ninja" ..
# =============================================================================

# Native Windows build: do not force CMAKE_SYSTEM_NAME here,
# otherwise CMake may switch to cross-compiling mode and disable try_run.

# Optional: chain-load vcpkg toolchain when provided by caller.
if(DEFINED CMAKE_TOOLCHAIN_FILE_VCPKG AND EXISTS "${CMAKE_TOOLCHAIN_FILE_VCPKG}")
  include("${CMAKE_TOOLCHAIN_FILE_VCPKG}")
endif()

# Disable CMake's built-in compiler checks that may fail with MSVC
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# =============================================================================
# Compiler selection: MSVC or clang-cl
# =============================================================================
if(USE_CLANG_CL)
  # Use clang-cl (LLVM with MSVC front-end)
  set(CMAKE_C_COMPILER clang-cl)
  set(CMAKE_CXX_COMPILER clang-cl)
  set(CMAKE_RC_COMPILER rc.exe)
  message(STATUS "Using clang-cl compiler")
else()
  # Use native MSVC
  # Note: When using Visual Studio generator, these are optional
  # as the generator handles compiler selection
  set(CMAKE_C_COMPILER cl.exe)
  set(CMAKE_CXX_COMPILER cl.exe)
  set(CMAKE_RC_COMPILER rc.exe)
  message(STATUS "Using MSVC compiler")
endif()

# =============================================================================
# Language standards
# =============================================================================
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Do not append -std flags manually here. Let generator/compiler handle it.

# =============================================================================
# Compiler flags for MSVC
# =============================================================================
# Note: MSVC uses different flags than GCC/Clang:
# - /W4 instead of -Wall
# - /O2 instead of -O2
# - /MD instead of dynamic linking flags

# Enable warnings (but reduce noise)
if(MSVC)
  # Reduce noise from MSVC warnings
  add_compile_options(/W3)  # W4 is too strict for legacy code
  
  # MSVC-specific fixes
  add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS      # Disable MSVC's own deprecation warnings
    _CRT_NONSTDC_NO_WARNINGS
    WIN32_LEAN_AND_MEAN          # Reduce Windows.h bloat
    NOMINMAX                     # Don't define min/max macros
    _USE_MATH_DEFINES            # Required for math.h constants
  )
  
  # Multi-processor compilation + serialized PDB writes (avoids C1041 with /MP)
  add_compile_options(/MP /FS)
  
  # Enable UTF-8 source encoding
  add_compile_options(/utf-8)
  
  # Recommended optimizations for clang-cl
  if(USE_CLANG_CL)
    # clang-cl understands both /O2 and -O2
    add_compile_options(-ffast-math)
  endif()
elseif(CLANG)
  # clang on Windows (without -cl front-end)
  add_compile_options(-Wall -Wextra)
endif()

# =============================================================================
# Disable flags that are GCC/Clang only (these will fail on MSVC)
# =============================================================================
# Note: These are typically used in the main CMakeLists.txt
# The main CMakeLists.txt needs checks like:
#   if(NOT MSVC)
#     CHECK_COMPILER_FLAG_AND_ENABLE_IT(-march=native)
#     CHECK_COMPILER_FLAG_AND_ENABLE_IT(-mtune=native)
#   endif()
# =============================================================================

# =============================================================================
# OpenMP support for MSVC
# =============================================================================
# MSVC has built-in OpenMP support with /openmp flag
if(USE_OPENMP)
  if(MSVC)
    add_compile_options(/openmp)
  elseif(USE_CLANG_CL)
    add_compile_options(-fopenmp=libomp)
  endif()
endif()

# =============================================================================
# Threading library - Windows uses native threading
# =============================================================================
set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_HAVE_THREADS_LIBRARY TRUE)
set(CMAKE_USE_WIN32_THREADS_INIT TRUE)
set(CMAKE_USE_PTHREADS_INIT FALSE)

# =============================================================================
# Find libraries in typical Windows paths
# =============================================================================
set(CMAKE_FIND_LIBRARY_SUFFIXES .lib .a ${CMAKE_FIND_LIBRARY_SUFFIXES})

# =============================================================================
# RPATH disabled on Windows (DLLs don't use RPATH)
# =============================================================================
set(CMAKE_BUILD_WITH_INSTALL_RPATH FALSE)
set(CMAKE_SKIP_RPATH TRUE CACHE BOOL "Skip RPATH" FORCE)

# =============================================================================
# Documentation for main CMakeLists.txt changes needed:
# =============================================================================
# The main CMakeLists.txt needs the following adjustments:
#
# 1. In march-mtune.cmake, add this at the beginning:
#    if(MSVC)
#      # MSVC doesn't support -march and -mtune flags
#      set(MARCH "")
#      return()
#    endif()
#
# 2. In compiler-warnings.cmake, add condition:
#    if(NOT MSVC)
#      CHECK_COMPILER_FLAG_AND_ENABLE_IT(-march=native)
#      # ... other GCC/Clang specific flags
#    endif()
#
# 3. Handle POSIX threads requirement in CMakeLists.txt:
#    if(NOT WIN32)
#      if(NOT CMAKE_USE_PTHREADS_INIT)
#        message(FATAL_ERROR "POSIX threads: not found")
#      endif()
#    endif()
#
# 4. MSVC doesn't support __thread TLS, use __declspec(thread):
#    In ConfigureChecks.cmake, add MSVC check or use:
#    if(MSVC)
#      set(HAVE_TLS TRUE)
#    endif()
# =============================================================================
