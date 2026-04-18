# - Find LCMS2 (Little CMS 2)
#
# This module defines:
#   LCMS2_FOUND
#   LCMS2_INCLUDE_DIRS
#   LCMS2_LIBRARIES
#   LCMS2::LCMS2 (imported target)

find_path(LCMS2_INCLUDE_DIR
  NAMES lcms2.h
  HINTS
    ENV LCMS2_INCLUDE_DIR
  PATH_SUFFIXES lcms2
)

find_library(LCMS2_LIBRARY
  NAMES lcms2 liblcms2
  HINTS
    ENV LCMS2_LIBDIR
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LCMS2
  REQUIRED_VARS LCMS2_LIBRARY LCMS2_INCLUDE_DIR
)

if(LCMS2_FOUND)
  set(LCMS2_LIBRARIES ${LCMS2_LIBRARY})
  set(LCMS2_INCLUDE_DIRS ${LCMS2_INCLUDE_DIR})

  if(NOT TARGET LCMS2::LCMS2)
    add_library(LCMS2::LCMS2 UNKNOWN IMPORTED)
    set_target_properties(LCMS2::LCMS2 PROPERTIES
      IMPORTED_LOCATION "${LCMS2_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${LCMS2_INCLUDE_DIR}"
    )
  endif()
endif()