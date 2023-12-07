include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

add_executable(ansel-cmstest main.c ${CMAKE_CURRENT_BINARY_DIR}/../version_gen.c)

# cmake implicitly sets GENERATED on the source file in the directory of the custom command
# since this isn't the same directory we do have to manually set it
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/../version_gen.c PROPERTIES GENERATED TRUE)
add_dependencies(ansel-cmstest generate_version)

target_link_libraries(ansel-cmstest ${LCMS2_LIBRARIES})
add_definitions(${LCMS2_DEFINITIONS})

target_link_libraries(ansel-cmstest ${Glib_LIBRARIES})
add_definitions(${Glib_DEFINITIONS})

find_package(Glib REQUIRED)
include_directories(SYSTEM ${GObject_INCLUDE_DIRS})
target_link_libraries(ansel-cmstest ${GObject_LIBRARIES})
add_definitions(${GObject_DEFINITIONS})

find_package(X11)
if(X11_FOUND AND X11_Xrandr_FOUND)
  include_directories(SYSTEM ${X11_INCLUDE_DIRS} ${X11_Xrandr_INCLUDE_PATH})
  target_link_libraries(ansel-cmstest ${X11_LIBRARIES} ${X11_Xrandr_LIB})
  add_definitions(${X11_DEFINITIONS})
  add_definitions("-DHAVE_X11")
endif(X11_FOUND AND X11_Xrandr_FOUND)

if(Colord_FOUND AND NOT ${Colord_VERSION} VERSION_LESS "0.1.32")
  message(STATUS "building ansel-cmstest with colord support. nice.")
  target_link_libraries(ansel-cmstest ${Colord_LIBRARIES})
  include_directories(SYSTEM ${Colord_INCLUDE_DIRS})
  add_definitions(${Colord_DEFINITIONS})
  add_definitions("-DHAVE_COLORD")
else(Colord_FOUND AND NOT ${Colord_VERSION} VERSION_LESS "0.1.32")
  message(STATUS "no colord support for ansel-cmstest. not cool.")
endif(Colord_FOUND AND NOT ${Colord_VERSION} VERSION_LESS "0.1.32")

set_target_properties(ansel-cmstest
                      PROPERTIES LINKER_LANGUAGE C)

if(WIN32)
  # _detach_debuginfo(ansel-cmstest bin)
endif(WIN32)

install(TARGETS ansel-cmstest DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
