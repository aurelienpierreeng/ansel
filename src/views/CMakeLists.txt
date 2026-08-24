include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

include(manage-symbol-visibility)
include(module-manifest)

add_definitions(-include common/module_api.h)
add_definitions(-include views/view_api.h)

set(MODULES darkroom lighttable slideshow studio_capture)

add_library(darkroom MODULE "darkroom.c")
add_library(lighttable MODULE "lighttable.c")
add_library(slideshow MODULE "slideshow.c")
add_library(studio_capture MODULE "studio_capture.c")

if(USE_MAP)
  add_library(map MODULE "map.c")
  set(MODULES ${MODULES} map)
endif(USE_MAP)

if(CUPS_FOUND)
  add_library(print MODULE "print.c")
  set(MODULES ${MODULES} print)
endif(CUPS_FOUND)

foreach(module ${MODULES})
  target_link_libraries(${module} lib_ansel)
endforeach(module)

foreach(module ${MODULES})
  if(WIN32)
    # _detach_debuginfo(${module} ${CMAKE_INSTALL_LIBDIR}/ansel/views)
  else()
    set_target_properties(${module}
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/../..)
  endif(WIN32)

  install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/ansel/views COMPONENT DTApplication)
endforeach(module)

# The set of modules Ansel may load from that directory, written down so it does not
# have to trust whatever shared objects happen to be sitting there. See
# cmake/module-manifest.cmake.
ansel_write_module_manifest("${CMAKE_INSTALL_LIBDIR}/ansel/views" ${MODULES})
