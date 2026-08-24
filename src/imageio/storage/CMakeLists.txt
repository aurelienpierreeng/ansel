include_directories(${CMAKE_CURRENT_BINARY_DIR}/../..)

include(manage-symbol-visibility)
include(module-manifest)

add_definitions(-include common/module_api.h)
add_definitions(-include imageio/storage/imageio_storage_api.h)

set(MODULES disk gallery)

foreach(module ${MODULES})
  add_library(${module} MODULE "${module}.c")
endforeach(module)

foreach(module ${MODULES})
  target_link_libraries(${module} lib_ansel)
  if(WIN32)
    # _detach_debuginfo(${module} ${CMAKE_INSTALL_LIBDIR}/ansel/plugins/imageio/storage)
  else()
    set_target_properties(${module}
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/../../../..)
  endif(WIN32)
  install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/ansel/plugins/imageio/storage COMPONENT DTApplication)
endforeach(module)

# The set of modules Ansel may load from that directory, written down so it does not
# have to trust whatever shared objects happen to be sitting there. See
# cmake/module-manifest.cmake.
ansel_write_module_manifest("${CMAKE_INSTALL_LIBDIR}/ansel/plugins/imageio/storage" ${MODULES})
