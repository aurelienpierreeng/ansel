# - Find the native sqlite3 includes and library
#
# This module defines
#  Rsvg2_INCLUDE_DIR, where to find sqlite3.h, etc.
#  Rsvg2_LIBRARIES, the libraries to link against to use sqlite3.
#  Rsvg2_FOUND, If false, do not try to use sqlite3.
# also defined, but not for general use are
#  Rsvg2_LIBRARY, where to find the sqlite3 library.


find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_RSVG2 REQUIRED librsvg-2.0)

find_path(Rsvg2_INCLUDE_DIR
  NAMES librsvg/rsvg.h
  HINTS ${PC_RSVG2_INCLUDE_DIRS}
)

find_library(Rsvg2_LIBRARY
  NAMES rsvg-2 librsvg-2
  HINTS ${PC_RSVG2_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Rsvg2
  REQUIRED_VARS Rsvg2_LIBRARY Rsvg2_INCLUDE_DIR
)

if(Rsvg2_FOUND)
  add_library(Rsvg2::Rsvg2 UNKNOWN IMPORTED)
  set_target_properties(Rsvg2::Rsvg2 PROPERTIES
    IMPORTED_LOCATION "${Rsvg2_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Rsvg2_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${PC_RSVG2_LIBRARIES}"
  )
endif()