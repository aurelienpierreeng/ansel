add_executable(ansel-chart
               main.c
               colorchart.c
               deltaE.c
               pfm.c
               dtcairo.c
               common.c
               thinplate.c
               tonecurve.c)

target_include_directories(ansel-chart
                           PRIVATE
                              ${CMAKE_CURRENT_BINARY_DIR}/..)

target_compile_options(ansel-chart
                       PRIVATE
                           ${LCMS2_DEFINITIONS}
                           ${GTK3_DEFINITIONS})

target_link_libraries(ansel-chart
                      PRIVATE
                        ${LCMS2_LIBRARIES}
                        ${GTK3_LIBRARIES}
                        lib_ansel
                        m)
if (WIN32)
  set_target_properties(ansel-chart PROPERTIES LINK_FLAGS "-mwindows -Wl,-subsystem,windows")
  #_detach_debuginfo (ansel-chart bin)
else()
    set_target_properties(ansel-chart
                          PROPERTIES
                          INSTALL_RPATH ${CMAKE_INSTALL_LIBDIR_RPATH})
endif(WIN32)

install(TARGETS ansel-chart DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT DTApplication)
