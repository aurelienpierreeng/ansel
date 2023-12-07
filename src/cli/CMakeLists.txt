include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

add_executable(ansel-cli main.c)

set_target_properties(ansel-cli PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(ansel-cli lib_ansel whereami)

if(WIN32)
  # _detach_debuginfo(ansel-cli bin)
else()
  set_target_properties(ansel-cli
                        PROPERTIES
                        INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR})
endif(WIN32)

install(TARGETS ansel-cli DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
