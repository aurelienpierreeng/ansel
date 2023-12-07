# Only build on real unix system (excludes MingW, MSYS, etc.)
if(UNIX AND NOT WIN32)
    add_executable(ansel-noiseprofile noiseprofile.c)
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
