# - Try to find JsonGlib-1.0
# Once done, this will define
#
#  JsonGlib_FOUND - system has Glib
#  JsonGlib_INCLUDE_DIRS - the Glib include directories
#  JsonGlib_LIBRARIES - link these to use Glib

find_package(PkgConfig REQUIRED)
pkg_check_modules(PC_JSONGLIB REQUIRED json-glib-1.0)

find_path(JsonGlib_INCLUDE_DIR
  NAMES json-glib/json-glib.h
  HINTS ${PC_JSONGLIB_INCLUDE_DIRS}
)

find_library(JsonGlib_LIBRARY
  NAMES json-glib-1.0
  HINTS ${PC_JSONGLIB_LIBRARY_DIRS}
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(JsonGlib
  REQUIRED_VARS JsonGlib_LIBRARY JsonGlib_INCLUDE_DIR
)

if(JsonGlib_FOUND)
  add_library(JsonGlib::JsonGlib UNKNOWN IMPORTED)
  set_target_properties(JsonGlib::JsonGlib PROPERTIES
    IMPORTED_LOCATION "${JsonGlib_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${JsonGlib_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${PC_JSONGLIB_LIBRARIES}"
  )
endif()
