include_directories(${CMAKE_CURRENT_BINARY_DIR}/../..)

add_executable(ansel-nn-parity main.c)

set_target_properties(ansel-nn-parity PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(ansel-nn-parity lib_ansel)

if(NOT WIN32)
  set_target_properties(ansel-nn-parity
                        PROPERTIES
                        INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR})
endif(NOT WIN32)

install(TARGETS ansel-nn-parity DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
