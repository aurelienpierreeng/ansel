# - Find the cmark includes and library
#
# This module defines
#  CMark_INCLUDE_DIRS, where to find cmark.h
#  CMark_LIBRARIES, the libraries to link against to use cmark
#  CMark_FOUND, If false, do not try to use cmark
#  CMark_VERSION, version string for cmark
#
# Also defined, but not for general use are
#  CMark_LIBRARY, where to find the cmark library

include(LibFindMacros)

libfind_pkg_check_modules(CMark_PKGCONF cmark)

find_path(CMark_INCLUDE_DIR
  NAMES cmark.h
  HINTS
    ${CMark_PKGCONF_INCLUDEDIR}
    ${CMark_PKGCONF_INCLUDE_DIRS}
)
mark_as_advanced(CMark_INCLUDE_DIR)

set(CMark_NAMES ${CMark_NAMES} cmark libcmark)
find_library(CMark_LIBRARY
  NAMES ${CMark_NAMES}
  HINTS
    ${CMark_PKGCONF_LIBDIR}
    ${CMark_PKGCONF_LIBRARY_DIRS}
)
mark_as_advanced(CMark_LIBRARY)

if(CMark_PKGCONF_VERSION VERSION_LESS CMark_FIND_VERSION)
  message(FATAL_ERROR "cmark version check failed. Version ${CMark_PKGCONF_VERSION} was found, at least version ${CMark_FIND_VERSION} is required")
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(CMark DEFAULT_MSG CMark_LIBRARY CMark_INCLUDE_DIR)

if(CMark_FOUND)
  set(CMark_LIBRARIES ${CMark_LIBRARY})
  set(CMark_INCLUDE_DIRS ${CMark_INCLUDE_DIR})
  set(CMark_VERSION ${CMark_PKGCONF_VERSION})
endif()
