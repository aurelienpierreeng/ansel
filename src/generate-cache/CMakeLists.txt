include_directories(${DARKTABLE_BINDIR})
add_executable(ansel-generate-cache main.c)

set_target_properties(ansel-generate-cache PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(ansel-generate-cache lib_ansel whereami)

if (WIN32)
  #_detach_debuginfo (ansel-generate-cache bin)
else()
    set_target_properties(ansel-generate-cache
                          PROPERTIES
                          INSTALL_RPATH ${CMAKE_INSTALL_LIBDIR_RPATH}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif(WIN32)

install(TARGETS ansel-generate-cache DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
