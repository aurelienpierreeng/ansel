add_subdirectory(pixmaps)

if(USE_OPENCL)
  add_subdirectory(kernels)
endif(USE_OPENCL)

file(GLOB THEME_FILES "themes/*.css")
install(FILES ${THEME_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/themes COMPONENT DTApplication)

#
# web gallery export support files:
#
file(GLOB WEB_FILES "style/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/style COMPONENT DTApplication)

install(DIRECTORY "pswp" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

#
# latex book template support files:
#
file(GLOB WEB_FILES "latex/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/latex COMPONENT DTApplication)

#
# Install (and generate when necessary) other system shares
#
if(NOT WIN32)
  # If we are not installing ansel to `/usr`, we use absolute path so it could
  # find the correct binary and icon. Otherwise, use general name to let system
  # find the correct file.
  set(DESKTOP_ANSEL_BINARY "ansel")
  set(DESKTOP_ANSEL_ICON "ansel")
  if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/usr")
    set(DESKTOP_ANSEL_BINARY "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/ansel")
    # `hicolor` is always available.
    set(DESKTOP_ANSEL_ICON "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps/ansel.svg")
  endif()

  # Append supported MIME types dynamically.
  if("avif" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/avif)
  endif()
  if("cr3" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/x-canon-cr3)
  endif()
  if("exr" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/x-exr image/aces)
  endif()
  if("heif" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/heif image/heic)
  endif()
  if("jp2" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/jp2)
  endif()
  if("webp" IN_LIST DT_SUPPORTED_EXTENSIONS)
    list(APPEND DESKTOP_MIME_TYPES image/webp)
  endif()

  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.Ansel.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop.in)

  file(GLOB PO_FILES "${CMAKE_SOURCE_DIR}/po/*.po")
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop
    SOURCE ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop.in
    COMMAND sh -c "${intltool_merge_BIN} --desktop-style ${CMAKE_SOURCE_DIR}/po ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop"
    MAIN_DEPENDENCY ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop.in
    DEPENDS ${PO_FILES}
  )
  add_custom_target(ansel.desktop_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop)

  if(VALIDATE_DESKTOP_FILE)
    add_custom_target(
      validate_ansel.desktop_file ALL
      COMMAND ${desktop_file_validate_BIN} --warn-kde ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Checking validity of photos.ansel.Ansel.desktop"
    )
    add_dependencies(ansel.desktop_file validate_ansel.desktop_file)
  endif()

  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications COMPONENT DTApplication)
endif(NOT WIN32)


if(VALIDATE_APPDATA_FILE)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.appdata.xml
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_SOURCE_DIR}/po ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.Ansel.appdata.xml.in ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.appdata.xml"
    COMMAND ${appstream_util_BIN} validate --nonet ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.appdata.xml
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.Ansel.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
else()
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.appdata.xml
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_SOURCE_DIR}/po ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.Ansel.appdata.xml.in ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.appdata.xml"
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.Ansel.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
endif()

add_custom_target(ansel.appdata_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.appdata.xml)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.Ansel.appdata.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo COMPONENT DTApplication)

#
# Install watermarks
#
file(GLOB WATERMARKS "watermarks/*.svg")
install(FILES ${WATERMARKS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/watermarks COMPONENT DTApplication)

#
# Install gdb command file for backtrace generation
#
install(FILES gdb_commands DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

#
# Install noiseprofiles
#
if(${VALIDATE_JSON})
  add_custom_target(
    validate_noiseprofiles_json ALL
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/validate_json_schema.py ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.json ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.schema
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.json ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.schema ${CMAKE_SOURCE_DIR}/tools/validate_json_schema.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of noiseprofiles.json"
  )
endif()
install(FILES noiseprofiles.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

#
# Neural raw-denoise models (rawdenoiseai-<version>-<variant>.anselnn).
#
# Models are trained and distributed by the ansel-denoise repo; they are
# fetched at configure time from its models/ directory, verified against the
# sha256 in its manifest.json. An upstream override changes the manifest hash,
# so a stale cached file fails verification and is re-downloaded — nightly
# builds always bundle the current models.
#
# What is installed is whatever ends up in NN_MODELS_DIR, which is the fetch's
# destination and is also settable: point it at models a packager placed there and
# set -DFETCH_NN_MODELS=OFF to build with no network. The rawdenoiseai module hides
# itself when its model is absent, so an empty directory costs a feature, not a build.
option(FETCH_NN_MODELS "Download neural denoise models at configure time" ON)
set(NN_MODELS_URL "https://raw.githubusercontent.com/aurelienpierreeng/ansel-denoise/master/models")
set(NN_MODELS_DIR "${CMAKE_CURRENT_BINARY_DIR}/nn-models" CACHE PATH
    "Directory holding the neural denoise models (*.anselnn)")
if(FETCH_NN_MODELS)
  file(DOWNLOAD "${NN_MODELS_URL}/manifest.json" "${NN_MODELS_DIR}/manifest.json"
       TIMEOUT 30 STATUS _nn_status)
  list(GET _nn_status 0 _nn_code)
  if(_nn_code EQUAL 0)
    file(READ "${NN_MODELS_DIR}/manifest.json" _nn_manifest)
    string(JSON _nn_models_obj GET "${_nn_manifest}" models)
    string(JSON _nn_count LENGTH "${_nn_models_obj}")
    if(_nn_count GREATER 0)
      math(EXPR _nn_last "${_nn_count} - 1")
      foreach(_nn_i RANGE ${_nn_last})
        string(JSON _nn_name MEMBER "${_nn_models_obj}" ${_nn_i})
        string(JSON _nn_sha GET "${_nn_models_obj}" "${_nn_name}" sha256)
        file(DOWNLOAD "${NN_MODELS_URL}/${_nn_name}" "${NN_MODELS_DIR}/${_nn_name}"
             TIMEOUT 600 EXPECTED_HASH SHA256=${_nn_sha} STATUS _nn_dl)
        list(GET _nn_dl 0 _nn_dl_code)
        if(NOT _nn_dl_code EQUAL 0)
          message(WARNING "Could not fetch neural model ${_nn_name}: ${_nn_dl}")
        endif()
      endforeach()
    endif()
  else()
    message(WARNING "Could not fetch neural models manifest (offline?): ${_nn_status} — "
                    "falling back to whatever is already in ${NN_MODELS_DIR}")
  endif()
endif()

file(GLOB NN_MODELS "${NN_MODELS_DIR}/*.anselnn")
if(NN_MODELS)
  list(LENGTH NN_MODELS _nn_installed)
  message(STATUS "Neural denoise models: ${_nn_installed} from ${NN_MODELS_DIR}")
  install(FILES ${NN_MODELS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel
          COMPONENT DTApplication)
else()
  message(WARNING "Neural denoise models: none in ${NN_MODELS_DIR} — the neural raw "
                  "denoiser will hide itself in this build")
endif()
# Local developer override: a model dropped in data/ wins over the fetched one
# (same destination filename, installed last).
file(GLOB RAWDENOISEAI_MODELS "${CMAKE_CURRENT_SOURCE_DIR}/denoise-*.anselnn")
if(RAWDENOISEAI_MODELS)
  install(FILES ${RAWDENOISEAI_MODELS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)
endif()

# Install AUTHORS file for splash screen credits
install(SCRIPT "${CMAKE_BINARY_DIR}/cmake/install_generate_authors.cmake")
install(FILES ${ANSEL_AUTHORS_FILE} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

#
# Transform anselconfig.xml into anselrc
#

set(DEFCONFIG_OPENCL "true")
if(APPLE)
  # OpenCL support on OS X isn't stable enough to enable it by default
  set(DEFCONFIG_OPENCL "false")
endif(APPLE)
set(DEFCONFIG_AUDIOPLAYER "aplay")
if(APPLE)
  # OS X doesn't have aplay, but afplay should do
  set(DEFCONFIG_AUDIOPLAYER "afplay")
endif(APPLE)

# The `anselconfig.xml` file has reference to this, so we need to copy it.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/anselconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.dtd COPYONLY)

# get_property(DT_PLUGIN_IOPS GLOBAL PROPERTY DT_PLUGIN_IOPS)
get_property(DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT GLOBAL PROPERTY DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT)

set(DARKTABLECONFIG_IOP_ENTRIES "")
foreach(DT_PLUGIN_IOP IN LISTS DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT)
  set(DARKTABLECONFIG_IOP_ENTRIES "${DARKTABLECONFIG_IOP_ENTRIES}
  <dtconfig>
    <name>plugins/darkroom/${DT_PLUGIN_IOP}/visible</name>
    <type>bool</type>
    <default>true</default>
    <shortdescription/>
    <longdescription/>
  </dtconfig>")
endforeach()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/anselconfig.xml.in ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml)

if(USE_XMLLINT)
  add_custom_target(
    validate_anselconfig_xml ALL
    COMMAND ${Xmllint_BIN} --nonet --valid --noout ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Checking validity of ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml"
  )
endif(USE_XMLLINT)

if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl ${CMAKE_CURRENT_SOURCE_DIR}/anselconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/anselrc
    COMMAND ${Xsltproc_BIN} --nonet ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/anselrc
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl ${CMAKE_CURRENT_SOURCE_DIR}/anselconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/anselrc
      COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl > ${CMAKE_CURRENT_BINARY_DIR}/anselrc
    )
  else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(FATAL_ERROR "Don't know how to generate anselrc")
  endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

add_custom_target(
  anselrc_file ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl ${CMAKE_CURRENT_SOURCE_DIR}/anselconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/anselrc
)
if(USE_XMLLINT)
  add_dependencies(anselrc_file validate_anselconfig_xml)
endif(USE_XMLLINT)

# first of all install anselrc file into share as template
# postinst script should copy this into users homedirectory
# $HOME/.config/ansel/anselrc
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/anselrc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

#
# Install bash completion file. users/packagers can symlink that to /etc/bash_completion.d/
#
install(FILES ansel.bash DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

# The lens calibration database (lenses.db).
#
# LensSerious reads calibrations from SQLite, not from XML: parsing the 8.4 MB upstream
# database costs ~100 ms per process and is the single largest term in resolving a lens
# (101.7 ms against 0.35 ms measured end to end). The conversion is done ONCE -- but not
# here any more.
#
# It used to happen at configure time: download lensfun's rolling version_1.tar.bz2 and run
# the importer over whatever came back. That made liblensfun a build dependency of this
# file, needed the network in the middle of a build, and pinned nothing -- the URL serves
# whatever upstream regenerated last, and the download carried no EXPECTED_HASH, so two
# builds of one commit could ship different calibrations and neither could say which. A
# sandboxed build (Flatpak, and any offline distribution build) could not do it at all: the
# only inputs such a build gets are the ones declared and hashed up front.
#
# LensSerious publishes the conversion instead, daily and with its hashes -- see its
# .github/workflows/lens-db.yml. Two files are fetched, both named in a manifest:
#
#   lenses.db            what iop/lens.c reads.
#   lensfun-xml.tar.xz   the calibrations it was built from, installed beside it because
#                        ansel-lens-db-update starts from them. That is what makes an update
#                        on the user's machine ADDITIVE: someone who calibrates one lens
#                        ends up with that lens on top of the fifteen hundred we ship,
#                        rather than a database containing only theirs.
#
# The manifest is addressed by the schema version the PINNED LensSerious reads, taken from
# its own schema.sql rather than hardcoded here. ls_db_open() refuses a database whose
# PRAGMA user_version it does not know, and LensSerious publishes each schema in its own
# directory, so a submodule bump and a database bump stay independent instead of breaking
# each other.
#
# LENS_DB_DIR is where the two files are expected. Point it at payloads a packager placed
# there and set -DFETCH_LENS_DB=OFF to build with no network at all; that is the whole
# offline story, and FETCH_NN_MODELS above has the same one.
#
# Failure is not fatal: iop/lens.c reports the database as unavailable and the module
# disables itself, the same way rawdenoiseai does without its model. Shipping no lens
# corrections is bad; failing a release build over a network hiccup is worse.
option(FETCH_LENS_DB "Download the lens database at configure time" ON)
set(LENS_DB_DIR "${CMAKE_CURRENT_BINARY_DIR}/lens-db" CACHE PATH
    "Directory holding lenses.db and lensfun-xml.tar.xz")
set(LENSES_SCHEMA "${CMAKE_SOURCE_DIR}/src/external/LensSerious/src/schema.sql")

set(LENS_DB_SCHEMA "")
file(READ "${LENSES_SCHEMA}" _lens_schema_sql)
string(REGEX MATCH "PRAGMA[ \t]+user_version[ \t]*=[ \t]*([0-9]+)" _lens_schema_match "${_lens_schema_sql}")
if(_lens_schema_match)
  set(LENS_DB_SCHEMA "${CMAKE_MATCH_1}")
endif()
set(LENS_DB_URL "https://raw.githubusercontent.com/aurelienpierreeng/LensSerious/main/db/v${LENS_DB_SCHEMA}")

if(NOT LENS_DB_SCHEMA)
  message(WARNING "Lens database: no PRAGMA user_version in ${LENSES_SCHEMA}, so there is no "
                  "way to tell which published database this build can read")
elseif(FETCH_LENS_DB)
  file(MAKE_DIRECTORY "${LENS_DB_DIR}")
  file(DOWNLOAD "${LENS_DB_URL}/manifest.json" "${LENS_DB_DIR}/manifest.json"
       TIMEOUT 30 STATUS _lens_status)
  list(GET _lens_status 0 _lens_code)
  if(_lens_code EQUAL 0)
    file(READ "${LENS_DB_DIR}/manifest.json" _lens_manifest)
    string(JSON _lens_files GET "${_lens_manifest}" files)
    string(JSON _lens_count LENGTH "${_lens_files}")
    if(_lens_count GREATER 0)
      math(EXPR _lens_last "${_lens_count} - 1")
      foreach(_lens_i RANGE ${_lens_last})
        string(JSON _lens_name MEMBER "${_lens_files}" ${_lens_i})
        string(JSON _lens_sha GET "${_lens_files}" "${_lens_name}" sha256)
        file(DOWNLOAD "${LENS_DB_URL}/${_lens_name}" "${LENS_DB_DIR}/${_lens_name}"
             TIMEOUT 600 EXPECTED_HASH SHA256=${_lens_sha} STATUS _lens_dl)
        list(GET _lens_dl 0 _lens_dl_code)
        if(NOT _lens_dl_code EQUAL 0)
          message(WARNING "Could not fetch ${_lens_name}: ${_lens_dl}")
        endif()
      endforeach()
    endif()
  else()
    message(WARNING "Could not fetch the lens database manifest (offline?): ${_lens_status} — "
                    "falling back to whatever is already in ${LENS_DB_DIR}")
  endif()
endif()

if(EXISTS "${LENS_DB_DIR}/lenses.db")
  install(FILES "${LENS_DB_DIR}/lenses.db" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel
          COMPONENT DTApplication)
  message(STATUS "Lens database: schema v${LENS_DB_SCHEMA}, from ${LENS_DB_DIR}")
else()
  message(WARNING "Lens database: no lenses.db in ${LENS_DB_DIR} — lens corrections will be "
                  "unavailable in this build")
endif()

# The calibrations themselves, as XML, beside the database built from them. About 5 MB
# unpacked. Only installed when the archive is there -- with no baseline
# ansel-lens-db-update still runs, it simply cannot be additive, and it says so.
if(EXISTS "${LENS_DB_DIR}/lensfun-xml.tar.xz")
  file(REMOVE_RECURSE "${LENS_DB_DIR}/lensfun-xml")
  file(MAKE_DIRECTORY "${LENS_DB_DIR}/lensfun-xml")
  file(ARCHIVE_EXTRACT INPUT "${LENS_DB_DIR}/lensfun-xml.tar.xz"
       DESTINATION "${LENS_DB_DIR}/lensfun-xml")
  install(DIRECTORY "${LENS_DB_DIR}/lensfun-xml/"
          DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/lensfun-xml
          COMPONENT DTApplication FILES_MATCHING PATTERN "*.xml")
endif()

# ansel-lens-db-update rebuilds the database on the user's machine and needs the schema to
# do it, so the schema is part of the installation rather than a build-tree artefact.
# Renamed on the way in: "schema.sql" says nothing in a directory shared with every other
# data file Ansel ships.
install(FILES "${LENSES_SCHEMA}" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel
        RENAME lenses-schema.sql COMPONENT DTApplication)
