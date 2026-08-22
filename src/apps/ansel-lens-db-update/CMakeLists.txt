include_directories(${CMAKE_CURRENT_BINARY_DIR}/../..)

add_executable(ansel-lens-db-update main.c)
set_target_properties(ansel-lens-db-update PROPERTIES LINKER_LANGUAGE C)

# lib_ansel for dt_loc_*, so this resolves the data and configuration directories through
# exactly the code the application uses -- a hand-rolled lookup here is how the two would
# drift the first time a packager moves something.
#
# lensserious_import is the ONLY target in the tree that links liblensfun, and it stops
# here: it is not in lib_ansel and nothing that renders a pixel reaches it.
target_link_libraries(ansel-lens-db-update lib_ansel whereami lensserious_import)

if(WIN32)
  # _detach_debuginfo(ansel-lens-db-update bin)
else()
  set_target_properties(ansel-lens-db-update
                        PROPERTIES
                        INSTALL_RPATH ${RPATH_ORIGIN}/${REL_BIN_TO_LIBDIR})
endif(WIN32)

install(TARGETS ansel-lens-db-update DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
