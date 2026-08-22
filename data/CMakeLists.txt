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

  configure_file(${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop.in)

  file(GLOB PO_FILES "${CMAKE_SOURCE_DIR}/po/*.po")
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop
    SOURCE ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop.in
    COMMAND sh -c "${intltool_merge_BIN} --desktop-style ${CMAKE_SOURCE_DIR}/po ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop"
    MAIN_DEPENDENCY ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop.in
    DEPENDS ${PO_FILES}
  )
  add_custom_target(ansel.desktop_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop)

  if(VALIDATE_DESKTOP_FILE)
    add_custom_target(
      validate_ansel.desktop_file ALL
      COMMAND ${desktop_file_validate_BIN} --warn-kde ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
      COMMENT "Checking validity of photos.ansel.ansel.desktop"
    )
    add_dependencies(ansel.desktop_file validate_ansel.desktop_file)
  endif()

  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications COMPONENT DTApplication)
endif(NOT WIN32)


if(VALIDATE_APPDATA_FILE)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.appdata.xml
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_SOURCE_DIR}/po ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.appdata.xml.in ${CMAKE_CURRENT_BINARY_DIR}/ansel.appdata.xml"
    COMMAND ${appstream_util_BIN} validate --nonet ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.appdata.xml
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
else()
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.appdata.xml
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_SOURCE_DIR}/po ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.appdata.xml.in ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.appdata.xml"
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
endif()

add_custom_target(ansel.appdata_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.appdata.xml)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.appdata.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo COMPONENT DTApplication)

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
# builds always bundle the current models. -DFETCH_NN_MODELS=OFF for offline
# builds: the rawdenoiseai module hides itself when its model is absent.
#
option(FETCH_NN_MODELS "Download neural denoise models at configure time" ON)
set(NN_MODELS_URL "https://raw.githubusercontent.com/aurelienpierreeng/ansel-denoise/master/models")
set(NN_MODELS_DIR "${CMAKE_CURRENT_BINARY_DIR}/nn-models")
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
        if(_nn_dl_code EQUAL 0)
          message(STATUS "Neural denoise model ready: ${_nn_name}")
          install(FILES "${NN_MODELS_DIR}/${_nn_name}"
                  DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)
        else()
          message(WARNING "Could not fetch neural model ${_nn_name}: ${_nn_dl} — "
                          "it will be missing from this build")
        endif()
      endforeach()
    endif()
  else()
    message(WARNING "Could not fetch neural models manifest (offline?): ${_nn_status} — "
                    "neural denoise models will be missing from this build")
  endif()
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
# (101.7 ms against 0.35 ms measured end to end). The conversion is done ONCE, here, at
# build time -- which is why liblensfun is a BUILD dependency of this file and not a
# runtime dependency of the application.
#
# The XML is FETCHED rather than taken from whatever the build machine has installed, for
# two reasons. It is what upstream's own lensfun-update-data pulls, so a build carries the
# current calibrations rather than the packager's snapshot. And it is what makes this work
# at all on Windows: the msys2 runners have liblensfun but no data package, so importing
# from "the system database" found nothing and failed the build.
#
# Falls back to the system database when the fetch is off or fails, and if neither can
# produce a database the build still succeeds -- iop/lens.c reports the database as
# unavailable and the module disables itself, the same way rawdenoiseai does without its
# model. Shipping no lens corrections is bad; failing a release build over a network hiccup
# is worse.
option(FETCH_LENS_DB "Download the lensfun XML at configure time to build lenses.db" ON)
set(LENS_DB_URL "https://lensfun.github.io/db/version_1.tar.bz2")
set(LENS_XML_DIR "${CMAKE_CURRENT_BINARY_DIR}/lensfun-xml")

set(LENS_XML_SOURCE "")
if(FETCH_LENS_DB)
  file(DOWNLOAD "${LENS_DB_URL}" "${CMAKE_CURRENT_BINARY_DIR}/lensfun-db.tar.bz2"
       TIMEOUT 120 STATUS _lens_dl)
  list(GET _lens_dl 0 _lens_dl_code)
  if(_lens_dl_code EQUAL 0)
    file(REMOVE_RECURSE "${LENS_XML_DIR}")
    file(MAKE_DIRECTORY "${LENS_XML_DIR}")
    file(ARCHIVE_EXTRACT INPUT "${CMAKE_CURRENT_BINARY_DIR}/lensfun-db.tar.bz2"
         DESTINATION "${LENS_XML_DIR}")
    file(GLOB _lens_xml "${LENS_XML_DIR}/*.xml")
    list(LENGTH _lens_xml _lens_xml_n)
    if(_lens_xml_n GREATER 0)
      set(LENS_XML_SOURCE "${LENS_XML_DIR}")
      message(STATUS "Lens database: ${_lens_xml_n} XML files fetched from upstream")
    endif()
  endif()
  if(NOT LENS_XML_SOURCE)
    message(WARNING "Lens database: could not fetch ${LENS_DB_URL}; "
                    "falling back to the system lensfun database")
  endif()
endif()

if(TARGET import_lensfun_xml)
  set(LENSES_DB "${CMAKE_CURRENT_BINARY_DIR}/lenses.db")
  set(LENSES_SCHEMA "${CMAKE_SOURCE_DIR}/src/external/LensSerious/src/schema.sql")
  # The importer writes to a temporary and renames, so a killed build cannot leave a
  # half-written database that looks complete.
  add_custom_command(
    OUTPUT "${LENSES_DB}"
    COMMAND $<TARGET_FILE:import_lensfun_xml> "${LENSES_SCHEMA}" "${LENSES_DB}"
            ${LENS_XML_SOURCE}
    DEPENDS import_lensfun_xml "${LENSES_SCHEMA}"
    COMMENT "Converting the lensfun database to SQLite (lenses.db)"
    VERBATIM)
  add_custom_target(lenses_db ALL DEPENDS "${LENSES_DB}")
  install(FILES "${LENSES_DB}" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel
          COMPONENT DTApplication OPTIONAL)
  # ansel-lens-db-update rebuilds this file on the user's machine and needs the schema to
  # do it, so the schema is part of the installation rather than a build-tree artefact.
  # Renamed on the way in: "schema.sql" says nothing in a directory shared with every other
  # data file Ansel ships.
  install(FILES "${LENSES_SCHEMA}" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel
          RENAME lenses-schema.sql COMPONENT DTApplication)

  # The calibrations themselves, as XML, beside the database built from them.
  #
  # ansel-lens-db-update loads these FIRST and then lets the machine's own lensfun profiles
  # override them, which is what makes an update ADDITIVE: a user who calibrated one lens
  # ends up with that lens on top of the fifteen hundred we ship, rather than a database
  # containing only theirs. Doing the merge here, in lensfun's own loader, is why nothing
  # has to upsert SQL rows or rebuild the corpus-wide token statistics by hand.
  #
  # About 5 MB. Only installed when the fetch succeeded -- with no baseline the tool still
  # runs, it simply cannot be additive, and it says so.
  if(LENS_XML_SOURCE)
    install(DIRECTORY "${LENS_XML_SOURCE}/" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/lensfun-xml
            COMPONENT DTApplication FILES_MATCHING PATTERN "*.xml")
  endif()
else()
  message(WARNING "liblensfun not found: lenses.db will not be built, and the lens "
                  "correction module will report no calibration data")
endif()
