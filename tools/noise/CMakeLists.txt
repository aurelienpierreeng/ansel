# Only build on real unix system (excludes MingW, MSYS, etc.)
if(UNIX AND NOT WIN32)
    add_executable(ansel-noiseprofile noiseprofile.c)
    target_include_directories(ansel-noiseprofile SYSTEM PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${PROJECT_BINARY_DIR}/src
        ${Glib_INCLUDE_DIRS}
        ${GIO_INCLUDE_DIRS}
        ${GThread_INCLUDE_DIRS}
        ${GModule_INCLUDE_DIRS}
        ${JsonGlib_INCLUDE_DIRS}
        ${GTK3_INCLUDE_DIRS}
        ${Rsvg2_INCLUDE_DIRS}
        ${Sqlite3_INCLUDE_DIR}
        ${Lua54_INCLUDE_DIRS}
    )
    target_link_libraries(ansel-noiseprofile m)

    install(TARGETS
                ansel-noiseprofile
            DESTINATION
                ${CMAKE_INSTALL_LIBEXECDIR}/ansel/tools
            COMPONENT
                DTApplication)

    install(PROGRAMS
                ansel-gen-noiseprofile
            DESTINATION
                ${CMAKE_INSTALL_LIBEXECDIR}/ansel/tools
            COMPONENT
                DTApplication)

    install(FILES
                subr.sh
                profiling-shot.xmp
            DESTINATION
               ${CMAKE_INSTALL_LIBEXECDIR}/ansel/tools
            COMPONENT
                DTApplication)
endif()
