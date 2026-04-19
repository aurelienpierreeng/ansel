# Make Windows behave like Mac and Linux regarding flag support detection
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

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
    set(MARCH "-march=native -mtune=native")
    add_definitions("-DNATIVE_ARCH")
  endif()

# -----------------------------------------------------------------------------
# Packaged builds or universal builds
# -----------------------------------------------------------------------------
else()

  message(STATUS "Using generic CPU tuning for binary distribution")

  if(DT_IS_X86)
    # Our baseline is Intel Sandy Bridge / AMD Bulldozer arch (~2011)
    CHECK_C_COMPILER_FLAG("-march=x86-64-v2" HAS_X86_V2)
    if(HAS_X86_V2)
      set(MARCH "-march=x86-64-v2 -mtune=core-avx2")
    else()
      # Last resort
      # mtune=generic is universally supported on GCC/Clang
      set(MARCH "-march=generic -mtune=core-avx2")
    endif()
  else()
    # Non-x86 architectures (ARM, etc.)
    message(STATUS "Non-x86 architecture detected, relying on compiler defaults")
  endif()

endif()