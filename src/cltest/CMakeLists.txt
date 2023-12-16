include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

add_executable(ansel-cltest main.c)

set_target_properties(ansel-cltest PROPERTIES LINKER_LANGUAGE C)
target_link_libraries(ansel-cltest lib_ansel)

if(WIN32)
  # _detach_debuginfo(ansel-cltest bin)
else()
  set_target_properties(ansel-cltest
                        PROPERTIES
                        INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR})
endif(WIN32)

install(TARGETS ansel-cltest DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
