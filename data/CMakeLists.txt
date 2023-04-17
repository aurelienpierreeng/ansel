add_subdirectory(pixmaps ${DARKTABLE_DATADIR}/pixmaps)

if(USE_OPENCL)
	add_subdirectory(kernels ${DARKTABLE_DATADIR}/kernels)
endif(USE_OPENCL)

FILE(GLOB THEME_FILES "themes/*.css")
FILE(COPY ${THEME_FILES} DESTINATION "${DARKTABLE_DATADIR}/themes")
install(FILES ${THEME_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/themes COMPONENT DTApplication)

#
# web gallery export support files:
#
FILE(GLOB WEB_FILES "style/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/style COMPONENT DTApplication)

install(DIRECTORY "pswp" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

#
# latex book template support files:
#
FILE(GLOB WEB_FILES "latex/*")
install(FILES ${WEB_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/latex COMPONENT DTApplication)

if(USE_LUA)
#
# lua system scripts
#
FILE(COPY lua luarc DESTINATION "${DARKTABLE_DATADIR}")
install(DIRECTORY "lua" DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)
install(FILES luarc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)
endif(USE_LUA)

#
# Install (and generate when necessary) other system shares
#
if(NOT WIN32)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/ansel.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/ansel.desktop.in)
file(GLOB PO_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../po/*.po")
add_custom_command(
		OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.app.desktop
		SOURCE ${CMAKE_CURRENT_BINARY_DIR}/ansel.desktop.in
		COMMAND sh -c "${intltool_merge_BIN} --desktop-style ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_BINARY_DIR}/ansel.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.app.desktop"
		MAIN_DEPENDENCY ${CMAKE_CURRENT_BINARY_DIR}/ansel.desktop.in
		DEPENDS ${PO_FILES}
)
add_custom_target(photos.ansel.desktop_file ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.app.desktop)
if(${VALIDATE_DESKTOP_FILE})
  add_custom_target(
    validate_photos.ansel.desktop ALL
    COMMAND ${desktop_file_validate_BIN} --warn-kde ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.app.desktop
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.app.desktop
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/
    COMMENT "Checking validity of photos.ansel.app.desktop"
    )
  add_dependencies(photos.ansel.desktop_file validate_photos.ansel.desktop)
endif()
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/photos.ansel.app.desktop DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications COMPONENT DTApplication)
endif(NOT WIN32)

if(${VALIDATE_APPDATA_FILE})
  add_custom_command(
    OUTPUT ${DARKTABLE_SHAREDIR}/metainfo/photos.ansel.app.appdata.xml
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.app.appdata.xml.in
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DARKTABLE_SHAREDIR}/metainfo
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.app.appdata.xml.in ${DARKTABLE_SHAREDIR}/metainfo/ansel.appdata.xml"
    COMMAND ${appstream_util_BIN} validate --nonet ${DARKTABLE_SHAREDIR}/metainfo/photos.ansel.app.appdata.xml
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.app.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
else()
  add_custom_command(
    OUTPUT ${DARKTABLE_SHAREDIR}/metainfo/photos.ansel.app.appdata.xml
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.app.appdata.xml.in
    COMMAND ${CMAKE_COMMAND} -E make_directory ${DARKTABLE_SHAREDIR}/metainfo
    COMMAND sh -c "${intltool_merge_BIN} --xml-style ${CMAKE_CURRENT_SOURCE_DIR}/../po ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.app.appdata.xml.in ${DARKTABLE_SHAREDIR}/metainfo/photos.ansel.app.appdata.xml"
    MAIN_DEPENDENCY ${CMAKE_CURRENT_SOURCE_DIR}/photos.ansel.app.appdata.xml.in
    DEPENDS ${PO_FILES}
  )
endif()

add_custom_target(ansel.appdata_file ALL DEPENDS ${DARKTABLE_SHAREDIR}/metainfo/photos.ansel.app.appdata.xml)

install(FILES ${DARKTABLE_SHAREDIR}/metainfo/photos.ansel.app.appdata.xml DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/metainfo COMPONENT DTApplication)

#
# Install watermarks
#
FILE(GLOB WATERMARKS "watermarks/*.svg")
FILE(COPY ${WATERMARKS} DESTINATION "${DARKTABLE_DATADIR}/watermarks")
install(FILES ${WATERMARKS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/watermarks COMPONENT DTApplication)

#
# Install gdb command file for backtrace generation
#
FILE(COPY gdb_commands DESTINATION "${DARKTABLE_DATADIR}")
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
FILE(COPY noiseprofiles.json DESTINATION "${DARKTABLE_DATADIR}")
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
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/anselconfig.dtd ${DARKTABLE_DATADIR}/anselconfig.dtd COPYONLY)

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
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    COMMENT "Checking validity of ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml"
  )
endif(USE_XMLLINT)

if(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  add_custom_command(
    DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl ${DARKTABLE_DATADIR}/anselconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/anselrc
    COMMAND ${Xsltproc_BIN} --nonet ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml > ${CMAKE_CURRENT_BINARY_DIR}/anselrc
  )
else(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")
  if(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    add_custom_command(
      DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl ${DARKTABLE_DATADIR}/anselconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/anselrc
      COMMAND ${Saxon_BIN} ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl > ${CMAKE_CURRENT_BINARY_DIR}/anselrc
    )
  else(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
    message(FATAL_ERROR "Don't know how to generate anselrc")
  endif(NOT ${Saxon_BIN} STREQUAL "Saxon_BIN-NOTFOUND")
endif(NOT ${Xsltproc_BIN} STREQUAL "Xsltproc_BIN-NOTFOUND")

add_custom_target(
  anselrc_file ALL
  DEPENDS ${CMAKE_SOURCE_DIR}/tools/generate_anselrc.xsl ${DARKTABLE_DATADIR}/anselconfig.dtd ${CMAKE_CURRENT_BINARY_DIR}/anselconfig.xml ${CMAKE_CURRENT_BINARY_DIR}/anselrc
)
if(USE_XMLLINT)
  add_dependencies(anselrc_file validate_anselconfig_xml)
endif(USE_XMLLINT)

# first of all install anselrc file into share as template
# postinst script should copy this into users homedirectory
# $HOME/.config/ansel/ansel.rc
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/anselrc DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)

#
# Install bash completion file. users/packagers can symlink that to /etc/bash_completion.d/
#
install(FILES ansel.bash DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel COMPONENT DTApplication)
