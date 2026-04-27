add_subdirectory(external)

add_library(ansel_deps INTERFACE)

include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(CheckCSourceCompiles)
include(CheckCXXSymbolExists)

add_custom_command(
  OUTPUT ${ANSEL_AUTHORS_FILE}
  COMMAND ${CMAKE_COMMAND}
    -DANSEL_SOURCE_DIR=${CMAKE_SOURCE_DIR}
    -DANSEL_AUTHORS_FILE=${ANSEL_AUTHORS_FILE}
    -DANSEL_SH_EXECUTABLE=${ANSEL_SH_EXECUTABLE}
    -DANSEL_BASE=release-3.9.0
    -DANSEL_HEAD=HEAD
    -P ${CMAKE_SOURCE_DIR}/tools/generate-authors.cmake
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/release/generate-authors.sh
          ${CMAKE_SOURCE_DIR}/tools/generate-authors.cmake
  COMMENT "Generating AUTHORS file for releases."
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_target(
  generate_authors_file ALL
  DEPENDS ${ANSEL_AUTHORS_FILE}
)

add_custom_command(
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${ANSEL_AUTHORS_FILE}
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/tools/
  COMMAND bash ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${ANSEL_AUTHORS_FILE} ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  COMMENT "Generating authors.h for about dialog."
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_custom_target(
  generate_authors_h ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/authors_h.sh ${ANSEL_AUTHORS_FILE} ${CMAKE_CURRENT_BINARY_DIR}/tools/darktable_authors.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_dependencies(generate_authors_h generate_authors_file)

#
# Add files for libansel
#
FILE(GLOB SOURCE_FILES
  "bauhaus/bauhaus.c"
  "common/act_on.c"
  "common/atomic.c"
  "common/bilateral.c"
  "common/bilateralcl.c"
  "common/box_filters.c"
  "common/cache.c"
  "common/calculator.c"
  "common/collection.c"
  "common/color_picker.c"
  "common/color_vocabulary.c"
  "common/colorlabels.c"
  "common/colorequal_shared.c"
  "common/colorspaces.c"
  "common/curve_tools.c"
  "common/splines.cpp"
  "common/curl_tools.c"
  "common/darktable.c"
  "common/database.c"
  "common/datetime.c"
  "common/dbus.c"
  "common/dtpthread.c"
  "common/eaw.c"
  "common/memory_arena.c"
  "common/exif.cc"
  "common/film.c"
  "common/file_location.c"
  "common/focus_peaking.c"
  "common/fp_mode.c"
  "common/gaussian.c"
  "common/grouping.c"
  "common/guided_filter.c"
  "common/history.c"
  "common/history_actions.c"
  "common/history_merge.c"
  "common/history_merge_gui.c"
  "common/history_snapshot.c"
  "common/topological_sort.c"
  "common/gpx.c"
  "common/image.c"
  "common/image_cache.c"
  "common/imagebuf.c"
  "common/imageio.c"
  "common/imageio_jpeg.c"
  "common/imageio_png.c"
  "common/imageio_module.c"
  "common/imageio_pfm.c"
  "common/imageio_pnm.c"
  "common/imageio_rgbe.c"
  "common/imageio_tiff.c"
  "common/imageio_im.c"
  "common/imageio_gm.c"
  "common/import_session.c"
  "common/import.c"
  "common/interpolation.c"
  "common/locallaplacian.c"
  "common/locallaplaciancl.c"
  "common/l10n.c"
  "common/lut3d.c"
  "common/lut_viewer.c"
  "common/matrices.c"
  "common/metadata.c"
  "common/metadata_export.c"
  "common/mipmap_cache.c"
  "common/module.c"
  "common/noiseprofiles.c"
  "common/nlmeans_core.c"
  "common/pdf.c"
  "common/presets.c"
  "common/styles.c"
  "common/selection.c"
  "common/system_signal_handling.c"
  "common/tags.c"
  "common/map_locations.c"
  "common/utility.c"
  "common/variables.c"
  "common/pwstorage/backend_kwallet.c"
  "common/pwstorage/pwstorage.c"
  "common/opencl.c"
  "common/dynload.c"
  "common/dlopencl.c"
  "common/ratings.c"
  "common/resource_limits.c"
  "common/histogram.c"
  "common/undo.c"
  "common/usermanual_url.c"
  "common/iop_group.c"
  "common/iop_order.c"
  "common/iop_profile.c"
  "common/dng_opcode.c"
  "control/conf.c"
  "control/control.c"
  "control/crawler.c"
  "control/jobs.c"
  "control/jobs/control_jobs.c"
  "control/jobs/develop_jobs.c"
  "control/jobs/film_jobs.c"
  "control/jobs/image_jobs.c"
  "control/jobs/import_jobs.c"
  "control/progress.c"
  "control/signal.c"
  "develop/develop.c"
  "develop/dev_history.c"
  "develop/dev_pixelpipe.c"
  "develop/imageop.c"
  "develop/imageop_math.c"
  "develop/imageop_gui.c"
  "develop/lightroom.c"
  "develop/pixelpipe.c"
  "develop/pixelpipe_cache.c"
  "develop/pixelpipe_cpu.c"
  "develop/pixelpipe_gpu.c"
  "develop/blend.c"
  "develop/blend_gui.c"
  "develop/blends/blendif_lab.c"
  "develop/blends/blendif_raw.c"
  "develop/blends/blendif_rgb_hsl.c"
  "develop/blends/blendif_rgb_jzczhz.c"
  "develop/tiling.c"
  "common/dwt.c"
  "common/heal.c"
  "common/iop-autoset.c"
  "develop/masks/brush.c"
  "develop/masks/circle.c"
  "develop/masks/group.c"
  "develop/masks/ellipse.c"
  "develop/masks/gradient.c"
  "develop/masks/masks.c"
  "develop/masks/masks_gui.c"
  "develop/masks/polygon.c"
  "develop/format.c"
  "dtgtk/button.c"
  "dtgtk/culling.c"
  "dtgtk/drawingarea.c"
  "dtgtk/expander.c"
  "dtgtk/gradientslider.c"
  "dtgtk/icon.c"
  "dtgtk/icon_cell_renderer.c"
  "dtgtk/paint.c"
  "dtgtk/preview_window.c"
  "dtgtk/resetlabel.c"
  "dtgtk/sidepanel.c"
  "dtgtk/thumbnail.c"
  "dtgtk/thumbtable_info.c"
  "dtgtk/thumbnail_btn.c"
  "dtgtk/thumbtable.c"
  "dtgtk/togglebutton.c"
  "gui/accelerators.c"
  "gui/gtkentry.c"
  "gui/guides.c"
  "gui/gtk.c"
  "gui/gui_throttle.c"
  "gui/splash.c"
  "gui/hist_dialog.c"
  "gui/preferences.c"
  "gui/presets.c"
  "gui/styles_dialog.c"
  "gui/color_picker_proxy.c"
  "gui/import_metadata.c"
  "gui/window_manager.c"
  "gui/actions/*.c"
  "libs/lib.c"
  "views/view.c"
  )
FILE(GLOB HEADER_FILES "*.h" "common/*.h" "external/OpenCL/CL/*.h" "control/*.h" "iop/*.h" "libs/*.h" "views/*.h")


if(APPLE)
  list(APPEND SOURCE_FILES "osx/osx.mm")
  list(APPEND HEADER_FILES "osx/osx.h")
  include(CheckLanguage)
  check_language(OBJCXX)
  if(CMAKE_OBJCXX_COMPILER)
    enable_language(OBJCXX)
  else()
    set_source_files_properties(osx/osx.mm PROPERTIES LANGUAGE CXX)
  endif()
endif(APPLE)

if(WIN32)
  list(APPEND SOURCE_FILES "win/strptime.c")
  list(APPEND HEADER_FILES "win/strptime.h")
  list(APPEND SOURCE_FILES "win/statvfs.c")
  list(APPEND HEADER_FILES "win/statvfs.h")

  list(APPEND SOURCE_FILES "win/getrusage.c")
  list(APPEND HEADER_FILES "win/getrusage.h")

  list(APPEND SOURCE_FILES "win/rlimit.c")
  list(APPEND HEADER_FILES "win/rlimit.h")

  list(APPEND SOURCE_FILES "win/getdelim.c")
  list(APPEND HEADER_FILES "win/getdelim.h")

  list(APPEND SOURCE_FILES "win/dtwin.c")
  list(APPEND HEADER_FILES "win/dtwin.h")

  list(APPEND SOURCE_FILES "win/filepath.c")
  list(APPEND HEADER_FILES "win/filepath.h")

  # Use mingw's sprintf instead of windows's
  ADD_DEFINITIONS(-D__USE_MINGW_ANSI_STDIO=1)

  # add it to have proper time.h functions available
  ADD_DEFINITIONS(-D_POSIX_THREAD_SAFE_FUNCTIONS)

  # avoid calling legacy ANSI win api functions
  add_definitions("-DUNICODE -D_UNICODE")

  # add icon, version, ... resources for Windows executable
  configure_file("${CMAKE_SOURCE_DIR}/data/pixmaps/dt_logo_128x128.ico" "${CMAKE_BINARY_DIR}/data/pixmaps/dt_logo_128x128.ico" COPYONLY)
  SET(CMAKE_RC_COMPILE_OBJECT "<CMAKE_RC_COMPILER> -O coff <DEFINES> -i <SOURCE> -o <OBJECT>")
  set(RESOURCE_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/win/ansel.rc")
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/win/ansel.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/win/ansel.rc")
endif(WIN32)

set(SOURCES ${SOURCE_FILES} ${HEADER_FILES})

if(CMAKE_SYSTEM_PROCESSOR STREQUAL "ppc64le")
  include_directories(${CMAKE_CURRENT_SOURCE_DIR}/ppc64le)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${CMAKE_CURRENT_BINARY_DIR})
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/external)
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/external/OpenCL)

# initial compiler flags
add_definitions(-DHAVE_CONFIG_H=1)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable IPO/LTO when supported by the toolchain.
# We keep it target-scoped and selectively disable it for known incompatible files.
include(CheckIPOSupported)
check_ipo_supported(RESULT ANSEL_IPO_SUPPORTED OUTPUT ANSEL_IPO_ERROR)
if(ANSEL_IPO_SUPPORTED)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
else()
  message(STATUS "IPO/LTO not supported: ${ANSEL_IPO_ERROR}")
endif()

if(USE_DARKTABLE_PROFILING)
	add_definitions(-DUSE_DARKTABLE_PROFILING)
	set(SOURCES ${SOURCES} "common/profiling.c")
endif()

##
# Find all other required libraries for building
##

# For GTK3, we grab the PkgConfig definition directly.
# Don't reinvent a worse wheel.
find_package(PkgConfig)
pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0>=3.24)
pkg_check_modules(RSVG2 REQUIRED IMPORTED_TARGET librsvg-2.0)
pkg_check_modules(JSONGLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(LIBXML2 REQUIRED IMPORTED_TARGET libxml-2.0)

# TODO: the following find_package() need to be replaced by pkg_check_modules()
# like above as soon as they will get supported by PkgConfig,
# and their cmake/modules/FindXXX.cmake modules deleted.
# We shouldn't have to maintain our own modules, it's brittle and needs to be updated sometimes.
find_package(LCMS2 REQUIRED)
find_package(JPEG REQUIRED)
find_package(PNG REQUIRED)
find_package(TIFF REQUIRED)
find_package(LensFun REQUIRED)
find_package(Sqlite3 3.24 REQUIRED)
find_package(Exiv2 0.27.4 REQUIRED)
find_package(Pugixml 1.2 REQUIRED)

# CMake provides a `FindCURL.cmake` module, which will set `CURL_LIBRARIES`, 
# however if cURL is built with CMake, it will generate a `CURLConfig.cmake` 
# config, which will not set `CURL_LIBRARIES`. 
# 
# MSYS2 currently build cURL with CMake, and if we do `find_package(CURL)` 
# directly, config will be used, so we need to keep compatibility。 
# 
# See <https://cmake.org/cmake/help/latest/module/FindCURL.html#curl-cmake>. 
# See <https://github.com/msys2/MINGW-packages/issues/21028>.
find_package(CURL REQUIRED)

target_include_directories(ansel_deps INTERFACE
  LCMS2::LCMS2
  PNG::PNG
  JPEG::JPEG
  TIFF::TIFF
  LensFun::LensFun
  Sqlite3::Sqlite3
  Exiv2::Exiv2
  Pugixml::Pugixml
  CURL::libcurl
)
target_link_libraries(ansel_deps INTERFACE
  PkgConfig::GTK3
  PkgConfig::RSVG2
  PkgConfig::JSONGLIB
  PkgConfig::LIBXML2
  LCMS2::LCMS2
  PNG::PNG
  JPEG::JPEG
  TIFF::TIFF
  LensFun::LensFun
  Sqlite3::Sqlite3
  Exiv2::Exiv2
  Pugixml::Pugixml
  CURL::libcurl
)

add_definitions("-DGLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_64")
add_definitions("-DGLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_MIN_REQUIRED")
add_definitions("-DGDK_VERSION_MIN_REQUIRED=GDK_VERSION_3_24")
#add_definitions("-DGDK_VERSION_MAX_ALLOWED=GDK_VERSION_MIN_REQUIRED")

## Optional deps go here

option(WITH_LIBSOUP2 "Use libsoup2 instead of libsoup3" OFF)
option(WITH_LIBSOUP3 "Force libsoup3 (ignore libsoup2)" OFF)

if(WITH_LIBSOUP2)
  set(LIBSOUP_FORCE_VERSION 2 CACHE INTERNAL "")
elseif(WITH_LIBSOUP3)
  set(LIBSOUP_FORCE_VERSION 3 CACHE INTERNAL "")
endif()

# used to launch a tiny http server, which could be used by OAuth services (libsoup2/3)
find_package(LibSoup)
if(LibSoup_FOUND)
  target_link_libraries(ansel_deps INTERFACE LibSoup::LibSoup)
  add_definitions(-DHAVE_HTTP_SERVER=1 -DLIBSOUP_VERSION_MAJOR=${LIBSOUP_VERSION_MAJOR})
  list(APPEND SOURCES "common/http_server.c")
  message(STATUS "HTTP server enabled with LibSoup ${LibSoup_VERSION}")
else()
  message(STATUS "HTTP server DISABLED (no libsoup)")
endif()

if(USE_OPENEXR)
  find_package(OpenEXR CONFIG)
  if(TARGET OpenEXR::OpenEXR)
    set(OpenEXR_LIBRARIES OpenEXR::OpenEXR)
  else()
    find_package(OpenEXR)
  endif()
  if(OpenEXR_FOUND)
    target_link_libraries(ansel_deps INTERFACE OpenEXR::OpenEXR)
    add_definitions(${OpenEXR_DEFINITIONS})
    add_definitions(-DHAVE_OPENEXR=1)
    list(APPEND SOURCES "common/imageio_exr.cc")
    set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} exr CACHE INTERNAL "")
  endif(OpenEXR_FOUND)
endif(USE_OPENEXR)

if(USE_WEBP)
  find_package(WebP 0.3.0)
  if(WebP_FOUND)
    target_link_libraries(ansel_deps INTERFACE WebP::WebP)
    add_definitions(${WebP_DEFINITIONS})
    add_definitions(-DHAVE_WEBP=1)
    list(APPEND SOURCES "common/imageio_webp.c")
    set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} webp CACHE INTERNAL "")
  endif(WebP_FOUND)
endif(USE_WEBP)

if(USE_AVIF)
  find_package(libavif CONFIG)
  if(libavif_FOUND AND libavif_VERSION VERSION_GREATER_EQUAL 0.8.2 AND TARGET avif)
    target_link_libraries(ansel_deps INTERFACE avif)
    add_definitions(-DHAVE_LIBAVIF=1)
    list(APPEND SOURCES common/imageio_avif.c)
    set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} avif CACHE INTERNAL "")
  else()
    # Fallback path (MSYS2 / older distros)
    pkg_check_modules(AVIF REQUIRED IMPORTED_TARGET libavif)
    if(TARGET PkgConfig::AVIF)
      target_link_libraries(ansel_deps INTERFACE PkgConfig::AVIF)
      target_compile_definitions(ansel_deps INTERFACE HAVE_LIBAVIF=1)
      list(APPEND SOURCES common/imageio_avif.c )
      set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} avif CACHE INTERNAL "")
    endif()
  endif()
endif()

if(USE_HEIF)
  # Try modern CMake package first
  find_package(libheif CONFIG)
  if(NOT libheif_FOUND)
    find_package(libheif 1.13.0 MODULE)
  endif()
  if(libheif_FOUND AND libheif_VERSION VERSION_GREATER_EQUAL 1.13.0 AND TARGET heif)
    target_link_libraries(ansel_deps INTERFACE heif)
    add_definitions(-DHAVE_LIBHEIF=1)
    list(APPEND SOURCES common/imageio_heif.c )
    set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} heif heic hif CACHE INTERNAL "")
    # libheif may also support AVIF
    if(NOT libavif_FOUND)
      list(APPEND DT_SUPPORTED_EXTENSIONS avif)
      set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} CACHE INTERNAL "")
    endif()
  else()
    set(libheif_FOUND NOTFOUND)
  endif()
endif()

# Optionally enable ISOBMFF support in Exiv2
if(USE_ISOBMFF)
  # exiv2 >= 0.27.4 provides optional support for ISOBMFF files.
  # This must be manually enabled during exiv2 build and can be checked with
  # the EXV_ENABLE_BMFF symbol.
  check_symbol_exists(EXV_ENABLE_BMFF
    "${Exiv2_INCLUDE_DIRS}/exiv2/exv_conf.h"
    HAVE_EXV_ENABLE_BMFF)
  if(HAVE_EXV_ENABLE_BMFF)
    add_definitions(-DHAVE_LIBEXIV2_WITH_ISOBMFF=1)
    set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} cr3 CACHE INTERNAL "")
    message(STATUS "Found Exiv2 ${Exiv2_VERSION} with ISOBMFF support (CR3, AVIF, HEIF)")
  else()
    message(STATUS "Found Exiv2 ${Exiv2_VERSION}, but was not compiled with ISOBMFF support (CR3, AVIF, HEIF)")
  endif()
endif()

if(USE_OPENJPEG)
  find_package(OpenJPEG)
  if(OpenJPEG_FOUND)
    target_link_libraries(ansel_deps INTERFACE OpenJPEG::OpenJPEG)
    add_definitions(-DHAVE_OPENJPEG=1)
    list(APPEND SOURCES "common/imageio_j2k.c")
    set(DT_SUPPORTED_EXTENSIONS ${DT_SUPPORTED_EXTENSIONS} j2c j2k jp2 jpc CACHE INTERNAL "")
  endif(OpenJPEG_FOUND)
endif(USE_OPENJPEG)

find_package(IsoCodes 3.66)
if(IsoCodes_FOUND)
  add_definitions(-DHAVE_ISO_CODES=1)
else(IsoCodes_FOUND)
  message(STATUS "no suitable iso-codes, there will be no language names in the preferences")
endif(IsoCodes_FOUND)

#
# Detect compile of optional pwstorage backends
#
if(USE_LIBSECRET)
  find_package(Libsecret)
  if(Libsecret_FOUND)
    if(Libsecret_VERSION VERSION_LESS "0.18")
      message(STATUS "Found libsecret unsupported version ${Libsecret_VERSION}. Please upgrade to version 0.18 or newer.")
      set(Libsecret_FOUND FALSE)
    else()
      target_link_libraries(ansel_deps INTERFACE Libsecret::Libsecret)
      set(SOURCES ${SOURCES} "common/pwstorage/backend_libsecret.c")
      add_definitions(-DHAVE_LIBSECRET=1)
    endif()
  endif(Libsecret_FOUND)
endif()

if(USE_KWALLET)
  add_definitions(-DHAVE_KWALLET=1)
endif(USE_KWALLET)

if(USE_MAC_INTEGRATION)
  find_package(MacIntegration)
  if(TARGET MacIntegration::MacIntegration)
    target_link_libraries(ansel_deps INTERFACE MacIntegration::MacIntegration)
    add_definitions("-DMAC_INTEGRATION")
  elseif(MacIntegration_FOUND)
    # Fallback for legacy FindMacIntegration.cmake
    target_include_directories(ansel_deps INTERFACE ${MacIntegration_INCLUDE_DIRS})
    target_link_libraries(ansel_deps INTERFACE ${MacIntegration_LIBRARIES})
    add_definitions("-DMAC_INTEGRATION")
  endif()
endif()

if(USE_GRAPHICSMAGICK)
  find_package(GraphicsMagick)

  if(TARGET GraphicsMagick::GraphicsMagick)
    target_link_libraries(ansel_deps INTERFACE GraphicsMagick::GraphicsMagick)
  else()
    # pkg-config fallback
    pkg_check_modules(GRAPHICSMAGICK QUIET IMPORTED_TARGET GraphicsMagick)

    if(TARGET PkgConfig::GRAPHICSMAGICK)
      target_link_libraries(ansel_deps INTERFACE PkgConfig::GRAPHICSMAGICK)
    elseif(GraphicsMagick_FOUND)
      # legacy fallback
      target_include_directories(ansel_deps INTERFACE ${GraphicsMagick_INCLUDE_DIRS})
      target_link_libraries(ansel_deps INTERFACE ${GraphicsMagick_LIBRARIES})
    endif()
  endif()

  if(TARGET GraphicsMagick::GraphicsMagick OR TARGET PkgConfig::GRAPHICSMAGICK OR GraphicsMagick_FOUND)
    set(DT_SUPPORTED_EXTENSIONS
        ${DT_SUPPORTED_EXTENSIONS}
        gif jpc jp2 bmp dcm jng miff mng pbm pnm ppm pgm webp
        CACHE INTERNAL "")
    add_definitions(-DHAVE_GRAPHICSMAGICK=1)
  endif()
endif()

if(USE_IMAGEMAGICK AND NOT TARGET GraphicsMagick::GraphicsMagick AND NOT TARGET PkgConfig::GRAPHICSMAGICK)
  pkg_check_modules(IMAGEMAGICK IMPORTED_TARGET MagickWand>=7)

  if(TARGET PkgConfig::IMAGEMAGICK)
    target_link_libraries(ansel_deps INTERFACE PkgConfig::IMAGEMAGICK)
    add_definitions(-DHAVE_IMAGEMAGICK=1)

    # pkg-config CFLAGS_OTHER (rare but needed for ImageMagick)
    if(IMAGEMAGICK_CFLAGS_OTHER)
      target_compile_options(ansel_deps INTERFACE ${IMAGEMAGICK_CFLAGS_OTHER})
    endif()

    set(DT_SUPPORTED_EXTENSIONS
        ${DT_SUPPORTED_EXTENSIONS}
        gif jpc jp2 bmp dcm jng miff mng pam webp
        CACHE INTERNAL "")
  endif()
endif()

if(USE_GMIC)
  find_package(GMIC)
  if(GMIC_FOUND)
    add_definitions(-DHAVE_GMIC=1)
    target_link_libraries(ansel_deps INTERFACE ${GMIC_LIBRARIES})
  endif(GMIC_FOUND)
endif(USE_GMIC)

if(USE_ICU)
  find_package(ICU REQUIRED COMPONENTS uc i18n data)

  if(TARGET ICU::uc)
    target_link_libraries(ansel_deps INTERFACE ICU::uc ICU::i18n ICU::data)
    add_definitions(-DHAVE_ICU=1 -DSQLITE_CORE -DSQLITE_ENABLE_ICU)
    list(APPEND SOURCES common/sqliteicu.c)
  endif()
endif()

if(USE_CMARK)
  find_package(CMark)
  if(CMark_FOUND)
    if(TARGET CMark::CMark)
      target_include_directories(ansel_deps INTERFACE CMark::CMark)
      target_link_libraries(ansel_deps INTERFACE CMark::CMark)
    elseif(TARGET cmark::cmark)
      target_include_directories(ansel_deps INTERFACE cmark::cmark)
      target_link_libraries(ansel_deps INTERFACE cmark::cmark)
    else()
      target_include_directories(ansel_deps INTERFACE ${CMark_INCLUDE_DIRS})
      target_link_libraries(ansel_deps INTERFACE ${CMark_LIBRARIES})
    endif()
    add_definitions(-DHAVE_CMARK=1)
  else()
    message(WARNING "cmark not found. Disabling USE_CMARK.")
    set(USE_CMARK OFF CACHE BOOL "" FORCE)
  endif()
endif()

if(USE_OPENCL)
  add_definitions(-DHAVE_OPENCL=1)
endif(USE_OPENCL)

if(WIN32)
  # Linker flags (apply to final executable/shared targets, not globally)
  target_link_options(ansel_deps INTERFACE
    -Wl,--enable-runtime-pseudo-reloc
    -Wl,--no-undefined
    -Wl,-O1
    -Wl,--as-needed
    -Wl,--sort-common
    -municode
  )

  # Libraries (proper modern form)
  target_link_libraries(ansel_deps INTERFACE
    psapi      # GetProcessMemoryInfo()
    exchndl    # exception handling / backtrace
  )
endif()

#
# Generate config.h
#
string(REPLACE ";" "\", \"" DT_SUPPORTED_EXTENSIONS_STRING "${DT_SUPPORTED_EXTENSIONS}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/config.cmake.h" "${CMAKE_CURRENT_BINARY_DIR}/config.h" @ONLY)

#
# Build external deps
#

# Actual defination of the target is in `external/CMakeLists.txt`.
target_include_directories(whereami PUBLIC external/whereami)

add_library(lib_ansel_imageio_rawspeed STATIC common/imageio_rawspeed.cc)
target_link_libraries(lib_ansel_imageio_rawspeed PRIVATE rawspeed)
target_link_libraries(lib_ansel_imageio_rawspeed PUBLIC ansel_deps)

# Don't do too much, if `USE_BUNDLED_LIBRAW=OFF` but `libraw_FOUND=NOTFOUND`,
# it's considered as packager's mistake. Instead of using bundled LibRaw as
# fallback, just build without LibRaw support in that case so the mistake could
# be easily found.
if(USE_LIBRAW)
  add_library(ansel_libraw INTERFACE)
  if(USE_BUNDLED_LIBRAW)
    target_link_libraries(ansel_libraw INTERFACE libraw::libraw)
    set(LIBRAW_PATH "${CMAKE_CURRENT_SOURCE_DIR}/external/LibRaw" CACHE STRING "Relative path to libraw directory (default=CMAKE_CURRENT_SOURCE_DIR)")
    if(NOT USE_OPENMP)
      set(ENABLE_OPENMP OFF CACHE BOOL "")
    endif()
    add_subdirectory(external/LibRaw-cmake)

    set(libraw_FOUND TRUE)
  else()
    find_package(libraw)

    if(libraw_FOUND)
      if(TARGET libraw::libraw)
        target_link_libraries(ansel_libraw INTERFACE libraw::libraw)
      elseif(TARGET LibRaw::LibRaw)
        target_link_libraries(ansel_libraw INTERFACE LibRaw::LibRaw)
      else()
        # fallback to raw variables (FindLibRaw legacy)
        target_link_libraries(ansel_libraw INTERFACE ${libraw_LIBRARIES})
        target_include_directories(ansel_libraw INTERFACE ${libraw_INCLUDE_DIRS})
      endif()
    endif()
  endif()

  if(libraw_FOUND)
    add_library(lib_ansel_imageio_libraw STATIC common/imageio_libraw.c)
    target_link_libraries(lib_ansel_imageio_libraw PRIVATE ansel_libraw)
    target_link_libraries(lib_ansel_imageio_libraw PUBLIC ansel_deps)
    add_definitions(-DHAVE_LIBRAW=1)
  else()
    message(STATUS "No LibRaw library was found. Canon CR3 support will be disabled.")
  endif()
else()
  message(STATUS "LibRaw support is disabled. Canon CR3 will not be supported.")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" OR CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  # do not error out on "loop not vectorized" clang OpenMP warnings
  add_definitions("-Wno-error=pass-failed")
endif()

# osm gps map library
if(USE_MAP)
  find_package(OSMGpsMap)
  if(OSMGpsMap_FOUND)
    target_link_libraries(ansel_deps INTERFACE OSMGpsMap::OSMGpsMap)
    message(STATUS "Map mode: enabled")
    add_definitions(-DHAVE_MAP=1)
    if(NOT OSMGpsMap_VERSION VERSION_LESS 1.1.0)
      add_definitions(-DHAVE_OSMGPSMAP_110_OR_NEWER=1)
    endif()
    if(OSMGpsMap_VERSION VERSION_GREATER 1.1.0)
      add_definitions(-DHAVE_OSMGPSMAP_NEWER_THAN_110=1)
    endif()
    add_definitions(${OSMGpsMap_DEFINITIONS})
  else(OSMGpsMap_FOUND)
    set(USE_MAP OFF)
    message(STATUS "Map mode: disabled, please install libosmgpsmap")
  endif(OSMGpsMap_FOUND)
else(USE_MAP)
  message(STATUS "Map mode: disabled")
endif(USE_MAP)

# libcolord-gtk:
if(USE_COLORD)
  find_package(Colord)
  if(Colord_FOUND)
    find_package(ColordGTK)
    if(ColordGTK_FOUND)
      target_link_libraries(ansel_deps INTERFACE Colord::Colord ColordGTK::ColordGTK)

      # We might make use of colord directly so link against it explicitly. just in case. move up if it is used without colord-gtk, too.
      add_definitions(${ColordGTK_DEFINITIONS})
      add_definitions("-DUSE_COLORDGTK")
    else(ColordGTK_FOUND)
      message(STATUS "No libcolord-gtk found, this build will lack the ability to get the screen profile from colord")
      set(USE_COLORD OFF)
    endif(ColordGTK_FOUND)
  else(Colord_FOUND)
    message(STATUS "No libcolord found, this build will lack the ability to get the screen profile from colord")
    set(USE_COLORD OFF)
  endif(Colord_FOUND)
endif(USE_COLORD)

if(BUILD_PRINT)
  find_package(Cups)
  if(CUPS_FOUND)
    target_link_libraries(ansel_deps INTERFACE Cups::Cups)
    FILE(GLOB SOURCE_FILES_PRINT
      "common/cups_print.h"
      "common/cups_print.c"
      "common/printprof.c"
      "common/printing.h"
      "common/printing.c"
      )
    set(SOURCES ${SOURCES} ${SOURCE_FILES_PRINT})
    add_definitions(-DHAVE_PRINT=1)
    message(STATUS "Print mode: enabled")
  else(CUPS_FOUND)
    set(BUILD_PRINT OFF)
    message(STATUS "Print mode: disabled, please install CUPS dev package")
  endif(CUPS_FOUND)
else(BUILD_PRINT)
  message(STATUS "Print mode: disabled")
endif(BUILD_PRINT)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  if(HAVE_EXECINFO_H)
    add_definitions("-DDT_HAVE_SIGNAL_TRACE")
    message(STATUS "Signal debug: print-trace possible")
  endif(HAVE_EXECINFO_H)
endif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")

#
# set development flags for our code
#
if(DT_CMAKE_INCLUDE_WHAT_YOU_USE)
  set(CMAKE_C_INCLUDE_WHAT_YOU_USE ${DT_CMAKE_INCLUDE_WHAT_YOU_USE})
  set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE ${DT_CMAKE_INCLUDE_WHAT_YOU_USE})
endif()

#
# Setup generation of preference dialog files..
#
if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_prefs.xsl ${CMAKE_SOURCE_DIR}/data/anselconfig.dtd ${CMAKE_BINARY_DIR}/data/anselconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
    COMMAND ${Xsltproc_BIN} --nonet --stringparam HAVE_OPENCL ${HAVE_OPENCL} ${CMAKE_SOURCE_DIR}/tools/generate_prefs.xsl ${CMAKE_BINARY_DIR}/data/anselconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
    COMMENT "Generating preferences"
  )
  add_custom_command(
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc_conf.xsl ${CMAKE_SOURCE_DIR}/data/anselconfig.dtd ${CMAKE_BINARY_DIR}/data/anselconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/conf_gen.h
    COMMAND ${Xsltproc_BIN} --nonet --stringparam HAVE_OPENCL ${HAVE_OPENCL} ${CMAKE_SOURCE_DIR}/tools/generate_anselrc_conf.xsl ${CMAKE_BINARY_DIR}/data/anselconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/conf_gen.h
    COMMENT "Generating configuration min/max/default/type"
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_prefs.xsl ${CMAKE_SOURCE_DIR}/data/anselconfig.dtd ${CMAKE_BINARY_DIR}/data/anselconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
      COMMAND ${Saxon_BIN} ${CMAKE_BINARY_DIR}/data/anselconfig.xml ${CMAKE_SOURCE_DIR}/tools/generate_prefs.xsl HAVE_OPENCL=${HAVE_OPENCL} > ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
      COMMENT "Generating preferences"
    )
    add_custom_command(
      DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc_conf.xsl ${CMAKE_SOURCE_DIR}/data/anselconfig.dtd ${CMAKE_BINARY_DIR}/data/anselconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/conf_gen.h
      COMMAND ${Saxon_BIN} ${CMAKE_BINARY_DIR}/data/anselconfig.xml ${CMAKE_SOURCE_DIR}/tools/generate_anselrc_conf.xsl HAVE_OPENCL=${HAVE_OPENCL} > ${CMAKE_CURRENT_BINARY_DIR}/conf_gen.h
      COMMENT "Generating configuration min/max/default/type"
    )
  else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(FATAL_ERROR "Don't know how to generate preferences")
  endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

add_custom_target(
  generate_preferences ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_prefs.xsl ${CMAKE_SOURCE_DIR}/data/anselconfig.dtd ${CMAKE_BINARY_DIR}/data/anselconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_custom_target(
  generate_conf ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc_conf.xsl ${CMAKE_SOURCE_DIR}/data/anselconfig.dtd ${CMAKE_BINARY_DIR}/data/anselconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/conf_gen.h
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
if(USE_XMLLINT)
  add_dependencies(generate_preferences validate_anselconfig_xml generate_conf)
endif(USE_XMLLINT)

#
# build libansel
#
add_library(lib_ansel SHARED ${CMAKE_CURRENT_BINARY_DIR}/preferences_gen.h ${CMAKE_CURRENT_BINARY_DIR}/version_gen.c ${SOURCES})

# cmake implicitly sets GENERATED on the source file in the directory of the custom command
# since this isn't the same directory we do have to manually set it
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/version_gen.c PROPERTIES GENERATED TRUE)

add_dependencies(lib_ansel generate_conf)
add_dependencies(lib_ansel generate_version)
add_dependencies(lib_ansel generate_preferences)
add_dependencies(lib_ansel generate_authors_h)

if(APPLE)
  set_target_properties(lib_ansel PROPERTIES MACOSX_RPATH TRUE)
endif()

set_target_properties(lib_ansel PROPERTIES OUTPUT_NAME ansel)
set_target_properties(lib_ansel PROPERTIES LINKER_LANGUAGE C)

#
# Link external libs
#
target_link_libraries(lib_ansel PUBLIC ansel_deps)
target_link_libraries(lib_ansel PRIVATE lib_ansel_imageio_rawspeed)
target_link_libraries(lib_ansel PRIVATE lib_ansel_imageio_libraw)
target_link_libraries(lib_ansel PRIVATE whereami)

if(OpenMP_C_FOUND)
  target_link_libraries(lib_ansel PUBLIC OpenMP::OpenMP_C)
endif()

if(OpenMP_CXX_FOUND)
  target_link_libraries(lib_ansel PUBLIC OpenMP::OpenMP_CXX)
endif()

if(LIBS)
  message(FATAL_ERROR "LIBS is deprecated and must not be used: ${LIBS}")
endif()
if(STATIC_LIBS)
  message(FATAL_ERROR "STATIC_LIBS is deprecated and must not be used: ${STATIC_LIBS}")
endif()

#get_target_property(_ansel_deps_libs ansel_deps INTERFACE_LINK_LIBRARIES)
#if(_ansel_deps_libs)
#  list(REMOVE_DUPLICATES _ansel_deps_libs)
#  set_property(TARGET ansel_deps PROPERTY INTERFACE_LINK_LIBRARIES "${_ansel_deps_libs}")
#endif()

message(STATUS "Ansel deps:" ${_ansel_deps_libs})

#
# Install libansel
#
if(WIN32)
  # Windows needs its DLLs in the same directory as the executable to find them.
  install(TARGETS lib_ansel RUNTIME DESTINATION bin COMPONENT DTApplication)
else()
  install(TARGETS lib_ansel DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# Compile views modules
add_subdirectory(views)

# Compile lighttable modules
add_subdirectory(libs)

# Compile darkroom modules
add_subdirectory(iop)

# Compile export modules
add_subdirectory(imageio)

if(HAVE_OPENCL)
  # have a small test program, to be run as root on some systems to enable the driver to run cl stuff
  add_subdirectory(cltest)
endif(HAVE_OPENCL)

# have a command line interface
add_subdirectory(cli)

# have a command line utility to generate all the thumbnails
add_subdirectory(generate-cache)

# have a small test program that verifies your color management setup
if(BUILD_CMSTEST)
  add_subdirectory(cmstest)
endif(BUILD_CMSTEST)

# have some test programs for unit testing, ...
if(BUILD_TESTING)
  add_subdirectory(tests)
endif(BUILD_TESTING)

add_library(help_menu "gui/actions/help.c")
add_dependencies(help_menu generate_authors_h)
target_include_directories(help_menu PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(help_menu PRIVATE ansel_deps)

#
# build ansel executable
#
add_executable(ansel ${SUBSYSTEM_MACOSX} ${SUBSYSTEM_WIN32} main.c ${RESOURCE_OBJECT})
set_target_properties(ansel PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(ansel PRIVATE lib_ansel)

if(APPLE)
  set_target_properties(lib_ansel PROPERTIES LINK_FLAGS "-framework Carbon -framework AppKit")
endif(APPLE)

if(WIN32)
  set_target_properties(ansel PROPERTIES LINK_FLAGS "-mconsole -mwindows -Wl,-subsystem,console")
  # _detach_debuginfo(ansel bin)
  # _detach_debuginfo(lib_ansel bin)
else()
  set_target_properties(ansel
                        PROPERTIES
                        INSTALL_RPATH "${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR}")
endif()

install(TARGETS ansel DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)

InstallDependencyFiles()

# Tell CPack about the components and group the data components together (CPACK_COMPONENT_${COMPONENT_NAME_ALL_CAPS}_GROUP).
set(CPACK_COMPONENTS_ALL DTApplication DTDebugSymbols DTDocuments)
