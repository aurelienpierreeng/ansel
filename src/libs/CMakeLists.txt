include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include libs/lib_api.h)


# The modules
set(MODULES export styles tagging collect metadata metadata_view navigation histogram history snapshots modulegroups backgroundjobs masks duplicate ioporder textnotes)

# The tools
set(MODULES ${MODULES} filter)
set(MODULES ${MODULES} lighttable_mode)
set(MODULES ${MODULES} module_toolbox)
set(MODULES ${MODULES} masktoolbar)

# modules
add_library(export MODULE "export.c" "export_metadata.c")
add_library(styles MODULE "styles.c")
add_library(tagging MODULE "tagging.c")
add_library(collect MODULE "collect.c")
add_library(metadata MODULE "metadata.c")
add_library(metadata_view MODULE "metadata_view.c")
add_library(navigation MODULE "navigation.c")
add_library(histogram MODULE "histogram.c")
add_library(history MODULE "history.c")
add_library(snapshots MODULE "snapshots.c")
add_library(modulegroups MODULE "modulegroups.c")
add_library(backgroundjobs MODULE "backgroundjobs.c")
add_library(masks MODULE "masks.c")
add_library(duplicate MODULE "duplicate.c")
add_library(ioporder MODULE "ioporder.c")
add_library(textnotes MODULE "textnotes.c")

if(ANSEL_HAVE_CMARK AND ANSEL_CMARK_TARGET AND TARGET ${ANSEL_CMARK_TARGET})
  target_link_libraries(textnotes ${ANSEL_CMARK_TARGET})
  if(ANSEL_CMARK_INCLUDE_DIR)
    target_include_directories(textnotes PRIVATE "${ANSEL_CMARK_INCLUDE_DIR}")
  endif()
  target_compile_definitions(textnotes PRIVATE HAVE_CMARK=1)
endif()

# tools
add_library(filter MODULE "tools/filter.c")
add_library(lighttable_mode MODULE "tools/lighttable.c")
add_library(module_toolbox MODULE "tools/module_toolbox.c")
add_library(masktoolbar MODULE "tools/mask_toolbar.c")

# the modules specific to map mode
if(USE_MAP)
  add_library(location MODULE "location.c")
  add_library(map_settings MODULE "map_settings.c")
  add_library(map_locations MODULE "map_locations.c")
  set(MODULES ${MODULES} location map_settings map_locations)
endif(USE_MAP)

# the modules specific to geotagging module
add_library(geotagging MODULE "geotagging.c")
set(MODULES ${MODULES} geotagging)

# the module specific to print mode
if(CUPS_FOUND)
  add_library(print_settings MODULE "print_settings.c")
  set(MODULES ${MODULES} print_settings)
endif(CUPS_FOUND)

# Add libs references
foreach(module ${MODULES})
  target_link_libraries(${module} lib_ansel)
endforeach(module)

# Add installation target for each module
# TODO: Modules are now not only lighttable related and should be placed
#       in a better matching structure then just in lighttable directory.
foreach(module ${MODULES})
  if(WIN32)
    # _detach_debuginfo(${module} ${CMAKE_INSTALL_LIBDIR}/ansel/plugins/lighttable)
  else()
    set_target_properties(${module}
                          PROPERTIES
                          INSTALL_RPATH ${RPATH_ORIGIN}/../../..)
  endif(WIN32)
  install(TARGETS ${module} DESTINATION ${CMAKE_INSTALL_LIBDIR}/ansel/plugins/lighttable COMPONENT DTApplication)
endforeach(module)
