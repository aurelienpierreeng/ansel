#
# Install Doc files
#
set(DOC_FILES ${ANSEL_AUTHORS_FILE} ../LICENSE)
install(FILES ${DOC_FILES} DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT DTDocuments)

# Build manual page
add_subdirectory(man)
