include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

add_executable(ansel-generate-cache main.c)

set_target_properties(ansel-generate-cache PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(ansel-generate-cache lib_ansel whereami)

if(WIN32)
  # _detach_debuginfo(ansel-generate-cache bin)
else()
  set_target_properties(ansel-generate-cache
                        PROPERTIES
                        INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR})
endif(WIN32)

install(TARGETS ansel-generate-cache DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
