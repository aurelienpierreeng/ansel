#
# Install Doc files
#
set(DOC_FILES ../AUTHORS ../LICENSE)
install(FILES ${DOC_FILES} DESTINATION ${CMAKE_INSTALL_DOCDIR} COMPONENT DTDocuments)

# Build manual page
add_subdirectory(man)
