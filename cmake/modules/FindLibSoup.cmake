include(FindPackageHandleStandardArgs)

option(LIBSOUP_FORCE_VERSION "Force libsoup version (2 or 3)" OFF)

unset(PC_LIBSOUP3_FOUND CACHE)
unset(LibSoup3_INCLUDE_DIR CACHE)
unset(LibSoup3_LIBRARY CACHE)
unset(LibSoup_FOUND CACHE)

find_package(PkgConfig QUIET)

if(LIBSOUP_FORCE_VERSION STREQUAL "2")
  message(STATUS "Forcing libsoup2 - HARD BLOCKING libsoup3")

  set(ENV{PKG_CONFIG_PATH} "")
  set(ENV{PKG_CONFIG_LIBDIR} "")

  set(PC_LIBSOUP3_FOUND FALSE CACHE INTERNAL "Force libsoup2 - libsoup3 blocked")
elseif(LIBSOUP_FORCE_VERSION STREQUAL "3")
  message(STATUS "Forcing libsoup3")
endif()


if(NOT LIBSOUP_FORCE_VERSION STREQUAL "2")
  pkg_check_modules(PC_LIBSOUP3 QUIET libsoup-3.0)
endif()

if(PC_LIBSOUP3_FOUND AND NOT LIBSOUP_FORCE_VERSION STREQUAL "2")
  find_path(LibSoup3_INCLUDE_DIR libsoup/soup.h HINTS ${PC_LIBSOUP3_INCLUDE_DIRS})
  find_library(LibSoup3_LIBRARY NAMES soup-3.0 HINTS ${PC_LIBSOUP3_LIBRARY_DIRS})
  if(LibSoup3_INCLUDE_DIR AND LibSoup3_LIBRARY)
    set(LibSoup_FOUND TRUE)
    set(LibSoup_INCLUDE_DIRS ${LibSoup3_INCLUDE_DIR})
    set(LibSoup_LIBRARIES ${LibSoup3_LIBRARY})
    set(LibSoup_VERSION ${PC_LIBSOUP3_VERSION})
    set(LIBSOUP_VERSION_MAJOR 3 CACHE STRING "LibSoup major version")
    message(STATUS "Found libsoup3 ${PC_LIBSOUP3_VERSION}")
  endif()
else()
  # libsoup2 Fallback
  find_package(LibSoup2 QUIET)
  if(LibSoup2_FOUND)
    set(LibSoup_FOUND TRUE)
    set(LibSoup_INCLUDE_DIRS ${LibSoup2_INCLUDE_DIRS})
    set(LibSoup_LIBRARIES ${LibSoup2_LIBRARIES})
    set(LibSoup_VERSION ${LibSoup2_VERSION})
    set(LIBSOUP_VERSION_MAJOR 2 CACHE STRING "LibSoup major version")
    message(STATUS "Found libsoup2 ${LibSoup2_VERSION}")
  endif()
endif()

if(LibSoup_FOUND)
  list(APPEND LibSoup_DEFINITIONS -DLIBSOUP_VERSION_MAJOR=${LIBSOUP_VERSION_MAJOR})
  mark_as_advanced(LibSoup_INCLUDE_DIRS LibSoup_LIBRARIES)
endif()

find_package_handle_standard_args(LibSoup
  REQUIRED_VARS LibSoup_LIBRARIES LibSoup_INCLUDE_DIRS
  VERSION_VAR LibSoup_VERSION
)
