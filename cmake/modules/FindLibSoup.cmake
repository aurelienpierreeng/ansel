include(FindPackageHandleStandardArgs)

option(LIBSOUP_FORCE_VERSION "Force libsoup version (2 or 3)" OFF)

unset(PC_LIBSOUP3_FOUND CACHE)
unset(LibSoup3_INCLUDE_DIR CACHE)
unset(LibSoup3_LIBRARY CACHE)
unset(LibSoup_FOUND CACHE)

find_package(PkgConfig QUIET)

# osm-gps-map decides which libsoup this process gets, and there is only one seat.
#
# libsoup 2 and libsoup 3 abort on sight of each other: each one's constructor
# looks for the other's symbols and calls g_error("Using libsoup2 and libsoup3 in
# the same process is not supported"). Ansel reaches libsoup twice over --
# common/http_server.c links it directly, and the map and geotagging panels link
# it through osm-gps-map -- so those two cannot disagree.
#
# They could, and did. The probe below prefers libsoup 3 whenever its .pc file is
# installed, while osm-gps-map through 1.2.0 is built against libsoup 2. On a
# machine carrying both, that produced a libansel.so on libsoup 3 and a set of
# lighttable plugins on libsoup 2: it compiled, linked, installed and packaged
# cleanly, and died on the first plugin loaded. It goes unnoticed wherever
# libsoup 3's development files simply are not installed, which is most
# development machines and none of the distributions.
#
# So ask osm-gps-map which libsoup it was built against, and follow it.
set(_libsoup_map_version "")
if(USE_MAP AND PKG_CONFIG_EXECUTABLE)
  execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --print-requires --print-requires-private osmgpsmap-1.0
    OUTPUT_VARIABLE _libsoup_osm_requires
    RESULT_VARIABLE _libsoup_osm_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_libsoup_osm_result EQUAL 0)
    if(_libsoup_osm_requires MATCHES "libsoup-3")
      set(_libsoup_map_version "3")
    elseif(_libsoup_osm_requires MATCHES "libsoup-2")
      set(_libsoup_map_version "2")
    endif()
  endif()
endif()

# Empty means the map is off, or osm-gps-map is not installed (in which case
# src/CMakeLists.txt turns the map off a few hundred lines further down), or its
# .pc file names no libsoup at all. Nothing constrains the choice then.
if(_libsoup_map_version)
  if(NOT LIBSOUP_FORCE_VERSION)
    set(LIBSOUP_FORCE_VERSION "${_libsoup_map_version}")
    message(STATUS "LibSoup: osm-gps-map is built against libsoup${_libsoup_map_version}, matching it")
  elseif(NOT LIBSOUP_FORCE_VERSION STREQUAL _libsoup_map_version)
    message(FATAL_ERROR
            "libsoup${LIBSOUP_FORCE_VERSION} was requested, but osm-gps-map is built against "
            "libsoup${_libsoup_map_version}, and the two cannot share a process -- the build would "
            "succeed and the application would abort as soon as it loaded a lighttable plugin.\n"
            "Either ask for libsoup${_libsoup_map_version} instead, or configure with -DUSE_MAP=OFF "
            "to drop the map and geotagging panels.")
  endif()
endif()

if(LIBSOUP_FORCE_VERSION STREQUAL "2")
  message(STATUS "Forcing libsoup2 - HARD BLOCKING libsoup3")

  # Blocking libsoup3 is done by not probing for it (see the guard on the
  # pkg_check_modules call below) and by pinning PC_LIBSOUP3_FOUND. It must NOT
  # be done by emptying PKG_CONFIG_PATH / PKG_CONFIG_LIBDIR, which is what this
  # used to do: set(ENV{...}) outlives this module, so every later
  # pkg_check_modules in the project -- lensfun, osmgpsmap, colord, pugixml --
  # loses sight of any prefix that is not pkg-config's compiled-in default. On a
  # distribution that puts everything in /usr/lib/pkgconfig nothing appears to
  # happen; in a Flatpak, where the dependencies this build needs live in
  # /app/lib/pkgconfig and are reachable only through PKG_CONFIG_PATH, forcing
  # libsoup2 silently disabled half the optional features instead.
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
  libfind_register_imported_target(LibSoup)
endif()

find_package_handle_standard_args(LibSoup
  REQUIRED_VARS LibSoup_LIBRARIES LibSoup_INCLUDE_DIRS
  VERSION_VAR LibSoup_VERSION
)
