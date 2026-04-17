if(APPLE)
  find_program(HOMEBREW_EXISTS brew)
  if(HOMEBREW_EXISTS)
    execute_process(
      COMMAND ${HOMEBREW_EXISTS} --prefix libomp
      OUTPUT_VARIABLE HOMEBREW_LIBOMP_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    if(EXISTS "${HOMEBREW_LIBOMP_PREFIX}/include/omp.h")
      message(STATUS "Using Homebrew libomp from ${HOMEBREW_LIBOMP_PREFIX}")
      set(OpenMP_ROOT "${HOMEBREW_LIBOMP_PREFIX}" CACHE PATH "Homebrew libomp prefix" FORCE)
      set(OpenMP_C_FLAGS "-Xclang -fopenmp")
      set(OpenMP_CXX_FLAGS "-Xclang -fopenmp")
      set(OpenMP_C_LIB_NAMES "omp")
      set(OpenMP_CXX_LIB_NAMES "omp")
      set(OpenMP_omp_LIBRARY "${HOMEBREW_LIBOMP_PREFIX}/lib/libomp.dylib")
    endif()
  endif()
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
  set(_ansel_openmp_version_specifier "-fopenmp-version=51")

  if(NOT CMAKE_C_FLAGS MATCHES "(^| )-fopenmp-version=51($| )")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_ansel_openmp_version_specifier}")
  endif()

  if(NOT CMAKE_CXX_FLAGS MATCHES "(^| )-fopenmp-version=51($| )")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${_ansel_openmp_version_specifier}")
  endif()
endif()

include("${CMAKE_ROOT}/Modules/FindOpenMP.cmake")
