add_executable(ansel-cmstest main.c ${CMAKE_CURRENT_BINARY_DIR}/../version_gen.c)
target_include_directories(ansel-cmstest PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/..)

# cmake implicitly sets GENERATED on the source file in the directory of the custom command
# since this isn't the same directory we do have to manually set it
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/../version_gen.c PROPERTIES GENERATED TRUE)
add_dependencies(ansel-cmstest generate_version)

target_link_libraries(ansel-cmstest
  PRIVATE
    ansel_deps
)

find_package(X11)
if(X11_FOUND AND X11_Xrandr_FOUND)
  target_include_directories(ansel-cmstest SYSTEM PRIVATE ${X11_INCLUDE_DIRS} ${X11_Xrandr_INCLUDE_PATH})
  target_link_libraries(ansel-cmstest PRIVATE ${X11_LIBRARIES} ${X11_Xrandr_LIB})
  target_compile_definitions(ansel-cmstest PRIVATE HAVE_X11)
endif(X11_FOUND AND X11_Xrandr_FOUND)

if(Colord_FOUND AND NOT ${Colord_VERSION} VERSION_LESS "0.1.32")
  message(STATUS "building ansel-cmstest with colord support. nice.")
  if(TARGET Colord::Colord)
    target_link_libraries(ansel-cmstest PRIVATE Colord::Colord)
  else()
    target_include_directories(ansel-cmstest SYSTEM PRIVATE ${Colord_INCLUDE_DIRS})
    target_link_libraries(ansel-cmstest PRIVATE ${Colord_LIBRARIES})
  endif()
  target_compile_definitions(ansel-cmstest PRIVATE HAVE_COLORD)
else(Colord_FOUND AND NOT ${Colord_VERSION} VERSION_LESS "0.1.32")
  message(STATUS "no colord support for ansel-cmstest. not cool.")
endif(Colord_FOUND AND NOT ${Colord_VERSION} VERSION_LESS "0.1.32")

set_target_properties(ansel-cmstest
                      PROPERTIES LINKER_LANGUAGE C)

if(WIN32)
  # _detach_debuginfo(ansel-cmstest bin)
endif(WIN32)

install(TARGETS ansel-cmstest DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
