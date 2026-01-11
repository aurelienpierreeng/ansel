# - Try to find libsoup3
#  LibSoup3_FOUND           True if libsoup3 is found
#  LibSoup3_INCLUDE_DIRS    Include directories for libsoup3
#  LibSoup3_LIBRARIES       Libraries to link against libsoup3
#  LibSoup3_VERSION         Detected libsoup3 version

include(FindPkgConfig)

if(LibSoup3_FIND_REQUIRED)
  set(_pkgconfig_REQUIRED "REQUIRED")
else()
  set(_pkgconfig_REQUIRED "")
endif()

# Try to find libsoup-3.0 using pkg-config
pkg_check_modules(LibSoup3 ${_pkgconfig_REQUIRED} libsoup-3.0)

if(LibSoup3_FOUND)
  set(LibSoup3_INCLUDE_DIRS ${LibSoup3_INCLUDE_DIRS} CACHE INTERNAL "")
  set(LibSoup3_LIBRARIES ${LibSoup3_LIBRARIES} CACHE INTERNAL "")
  set(LibSoup3_VERSION ${LibSoup3_VERSION} CACHE INTERNAL "")
  message(STATUS "Found libsoup3: version=${LibSoup3_VERSION}")
else()
  message(${LibSoup3_FIND_REQUIRED} "Could NOT find libsoup3 (pkg-config package 'libsoup-3.0')")
endif()

mark_as_advanced(LibSoup3_LIBRARIES LibSoup3_INCLUDE_DIRS)
