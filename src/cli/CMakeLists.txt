include_directories(${DARKTABLE_BINDIR})
add_executable(ansel-cli main.c)

set_target_properties(ansel-cli PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(ansel-cli lib_ansel whereami)

if (WIN32)
  #_detach_debuginfo (ansel-cli bin)
else()
    set_target_properties(ansel-cli
                          PROPERTIES
                          INSTALL_RPATH ${CMAKE_INSTALL_LIBDIR_RPATH}
                          RUNTIME_OUTPUT_DIRECTORY ${DARKTABLE_BINDIR})
endif(WIN32)

install(TARGETS ansel-cli DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
