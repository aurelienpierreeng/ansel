# -----------------------------------------------------------------------------
# Detect Apple universal builds (multiple archs)
# -----------------------------------------------------------------------------
set(DT_APPLE_UNIVERSAL_BUILD OFF)
if(APPLE AND CMAKE_OSX_ARCHITECTURES MATCHES ";")
  set(DT_APPLE_UNIVERSAL_BUILD ON)
endif()

# -----------------------------------------------------------------------------
# Architecture detection
# -----------------------------------------------------------------------------
set(DT_IS_X86 OFF)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i[3-6]86")
  set(DT_IS_X86 ON)
endif()

# -----------------------------------------------------------------------------
# Default: no flags
# -----------------------------------------------------------------------------
set(MARCH "")

# -----------------------------------------------------------------------------
# Native builds (non-packaged, non-universal)
# -----------------------------------------------------------------------------
if(NOT BINARY_PACKAGE_BUILD AND NOT DT_APPLE_UNIVERSAL_BUILD)

  message(STATUS "Checking for native CPU optimization support")

  # ---------------------------------------------------------------------------
  # AppleClang (macOS)
  # ---------------------------------------------------------------------------
  if(APPLE AND CMAKE_C_COMPILER_ID STREQUAL "AppleClang")

    CHECK_C_COMPILER_FLAG("-mcpu=native" HAS_MCPU_NATIVE)
    if(HAS_MCPU_NATIVE)
      set(MARCH "-mcpu=native")
      add_definitions("-DNATIVE_ARCH")
    else()
      message(WARNING "AppleClang does not support -mcpu=native, falling back to defaults")
    endif()

  # ---------------------------------------------------------------------------
  # GCC / Clang (Linux, MinGW, etc.)
  # ---------------------------------------------------------------------------
  else()

    CHECK_C_COMPILER_FLAG("-march=native" HAS_MARCH_NATIVE)
    if(HAS_MARCH_NATIVE)
      set(MARCH "-march=native")
      add_definitions("-DNATIVE_ARCH")
    else()
      CHECK_C_COMPILER_FLAG("-mtune=native" HAS_MTUNE_NATIVE)
      if(HAS_MTUNE_NATIVE)
        set(MARCH "-mtune=native")
      else()
        message(WARNING "No native tuning flags available, using defaults")
      endif()
    endif()

  endif()

# -----------------------------------------------------------------------------
# Packaged builds or universal builds
# -----------------------------------------------------------------------------
else()

  message(STATUS "Using generic CPU tuning for binary distribution")

  if(DT_IS_X86)

    # Prefer modern x86 baseline (AVX2 class)
    CHECK_C_COMPILER_FLAG("-march=x86-64-v3" HAS_X86_V3)
    if(HAS_X86_V3)
      # Binaries will require CPUs roughly ≥ Haswell (2013).
      set(MARCH "-march=x86-64-v3 -mtune=generic")
    else()
      # Fallback to slightly older baseline
      CHECK_C_COMPILER_FLAG("-march=x86-64-v2" HAS_X86_V2)
      if(HAS_X86_V2)
        set(MARCH "-march=x86-64-v2 -mtune=generic")
      else()
        # Last resort
        CHECK_C_COMPILER_FLAG("-mtune=generic" HAS_MTUNE_GENERIC)
        if(HAS_MTUNE_GENERIC)
          set(MARCH "-mtune=generic")
        else()
          message(WARNING "No suitable CPU tuning flags found, using compiler defaults")
        endif()
      endif()
    endif()

  else()
    # Non-x86 architectures (ARM, etc.)
    message(STATUS "Non-x86 architecture detected, relying on compiler defaults")
  endif()

endif()