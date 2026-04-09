if(USE_XCF)
  add_subdirectory(libxcf)
endif()

add_library(whereami STATIC "${CMAKE_CURRENT_SOURCE_DIR}/whereami/src/whereami.c")
target_include_directories(whereami PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/whereami/src/")

set(BUILD_TESTING_SAVE "${BUILD_TESTING}")

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKING OFF)
set(WITH_OPENMP ${USE_OPENMP} CACHE BOOL "" FORCE)

# Feed RawSpeed's own find_package(OpenMP) with the AppleClang/Homebrew libomp hints.
if(APPLE AND USE_OPENMP AND DEFINED OpenMP_ROOT)
  set(OpenMP_C_INCLUDE_DIR   "${OpenMP_ROOT}/include" CACHE PATH "" FORCE)
  set(OpenMP_CXX_INCLUDE_DIR "${OpenMP_ROOT}/include" CACHE PATH "" FORCE)

  set(OpenMP_C_FLAGS   "-Xclang -fopenmp" CACHE STRING "" FORCE)
  set(OpenMP_CXX_FLAGS "-Xclang -fopenmp" CACHE STRING "" FORCE)

  set(OpenMP_C_LIB_NAMES   "omp" CACHE STRING "" FORCE)
  set(OpenMP_CXX_LIB_NAMES "omp" CACHE STRING "" FORCE)
  set(OpenMP_omp_LIBRARY   "${OpenMP_ROOT}/lib/libomp.dylib" CACHE FILEPATH "" FORCE)
endif()

set(RAWSPEED_PATH "${CMAKE_CURRENT_SOURCE_DIR}/rawspeed")
set(RAWSPEED_BINARY_PREFIX "ansel")
if(USE_CMARK)
  find_package(CMark QUIET)
  if(CMark_FOUND)
    if(TARGET CMark::CMark)
      set(ANSEL_CMARK_TARGET CMark::CMark PARENT_SCOPE)
    elseif(TARGET cmark::cmark)
      set(ANSEL_CMARK_TARGET cmark::cmark PARENT_SCOPE)
    else()
      add_library(ansel_cmark INTERFACE)
      target_include_directories(ansel_cmark INTERFACE ${CMark_INCLUDE_DIRS})
      target_link_libraries(ansel_cmark INTERFACE ${CMark_LIBRARIES})
      set(ANSEL_CMARK_TARGET ansel_cmark PARENT_SCOPE)
    endif()
    set(ANSEL_CMARK_INCLUDE_DIR "${CMark_INCLUDE_DIRS}" PARENT_SCOPE)
    set(ANSEL_HAVE_CMARK TRUE PARENT_SCOPE)
  else()
    message(WARNING "cmark not found. Disabling USE_CMARK.")
    set(USE_CMARK OFF CACHE BOOL "" FORCE)
    set(ANSEL_HAVE_CMARK FALSE PARENT_SCOPE)
  endif()
else()
  set(ANSEL_HAVE_CMARK FALSE PARENT_SCOPE)
endif()

if(NOT(EXISTS "${RAWSPEED_PATH}" AND IS_DIRECTORY "${RAWSPEED_PATH}" AND EXISTS "${RAWSPEED_PATH}/CMakeLists.txt"))
  message(FATAL_ERROR "RawSpeed submodule not found. You probably want to run:\n$ git submodule init\nand then\n$ git submodule update")
endif()

set(OPENCL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/OpenCL")

if(NOT(EXISTS "${OPENCL_PATH}" AND IS_DIRECTORY "${OPENCL_PATH}" AND EXISTS "${OPENCL_PATH}/CL/opencl.h"))
  message("OpenCL submodule not found. You probably want to run:\n$ git submodule init\nand then\n$ git submodule update\nOr let the OS headers to be used.")
endif()

set(CMAKE_INSTALL_DATAROOTDIR_SAVE "${CMAKE_INSTALL_DATAROOTDIR}")

set(CMAKE_C_FLAGS_SAVE "${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS_SAVE "${CMAKE_CXX_FLAGS}")

set(CMAKE_INSTALL_DATAROOTDIR "${CMAKE_INSTALL_DATAROOTDIR}/ansel")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

set(CMAKE_CXX_CLANG_TIDY_SAVE "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE_SAVE "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")

unset(CMAKE_CXX_CLANG_TIDY)
unset(CMAKE_CXX_INCLUDE_WHAT_YOU_USE)

# Keep RawSpeed OpenMP setting aligned with the top-level build option
set(WITH_OPENMP ${USE_OPENMP} CACHE BOOL "Enable OpenMP support in RawSpeed" FORCE)

add_subdirectory(rawspeed)

# Only patch the target when OpenMP is actually enabled
if(USE_OPENMP AND TARGET rawspeed)
  target_link_libraries(rawspeed PUBLIC OpenMP::OpenMP_CXX)

  # AppleClang/Homebrew libomp sometimes still needs the header path spelled out
  if(APPLE AND DEFINED OpenMP_ROOT)
    target_include_directories(rawspeed SYSTEM PUBLIC "${OpenMP_ROOT}/include")
  endif()
endif()

set(CMAKE_INSTALL_DATAROOTDIR "${CMAKE_INSTALL_DATAROOTDIR_SAVE}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_SAVE}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_SAVE}")

set(CMAKE_CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY_SAVE}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE_SAVE}")

set(BUILD_TESTING "${BUILD_TESTING_SAVE}" CACHE BOOL "" FORCE)
