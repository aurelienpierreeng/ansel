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
# lua system scripts
#
if(USE_LUA)
  install(DIRECTORY "lua" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)
  install(FILES luarc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)
endif(USE_LUA)

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
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.appdata.xml.in
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_SOURCE_DIR}/po ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.appdata.xml.in ${CMAKE_CURRENT_BINARY_DIR}/ansel.appdata.xml"
    COMMAND ${appstream_util_BIN} validate --nonet ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.appdata.xml
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
else()
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.ansel.appdata.xml
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.ansel.appdata.xml.in
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
    COMMAND ${jsonschema_BIN} -i ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.json ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.schema
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.json ${CMAKE_CURRENT_SOURCE_DIR}/noiseprofiles.schema
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of noiseprofiles.json"
  )
endif()
install(FILES noiseprofiles.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

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
