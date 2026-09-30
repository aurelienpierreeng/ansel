# Apple's ld64 does not support -Wl,--wrap, which add_cmocka_mock_test() is built on --
# the mock tests cannot link on macOS, with any cmocka.
if(NOT APPLE)
  add_subdirectory(iop)
endif()

add_cmocka_test(test_sample
                SOURCES test_sample.c
                COMPILE_OPTIONS -Wno-error=deprecated-declarations # cmocka >= 2 deprecates check_expected et al.
                LINK_LIBRARIES ${CMOCKA_LINK_TARGET})

# Windows has no rpath: libansel.dll must sit beside the executables. Every test below
# shares this one directory, so the copy is ONE target they all depend on -- a POST_BUILD
# copy per test had up to 23 links writing the same file at once, and Windows answered
# "Permission denied" to whichever lost, failing a random handful of links per build.
if(WIN32)
  add_custom_target(unittests_lib_ansel_dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:lib_ansel> ${CMAKE_CURRENT_BINARY_DIR})
  add_dependencies(unittests_lib_ansel_dll lib_ansel)
endif()

# Windows: main-method requires the wrapper provided by lib-darktable
if(WIN32)
    target_include_directories(test_sample PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(test_sample PRIVATE lib_ansel)
    add_dependencies(test_sample unittests_lib_ansel_dll)
endif(WIN32)

# CMocka unit tests for the sealed modules under src/.
#
# Each test is a standalone binary linking lib_ansel. The database tests open the one
# connection on ":memory:" and drive ALL their setup through the repository APIs -- no raw
# SQL and no sqlite3 handle, which keeps tools/check_module_boundaries.sh at its zero
# baselines. A test that cannot seed its rows through the public API has found a hole in
# the API, not a reason to reach for the connection.

set(LIB_ANSEL_UNIT_TESTS
  test_image_repository
  test_history_repository
  test_preset_repository
  test_removed_image_repository
  test_tag_selection_metadata
  test_tagging_completion
  test_tagging_completion_model
  test_metadata_notify
  test_accel_map_defaults
  test_image_cache_flags_writeback
  # Not database tests, but they want the same standalone-binary-linking-lib_ansel treatment,
  # and splitting the list to say so would be more ceremony than it is worth.
  test_pipe_cache_policy
  test_pipe_cache_pressure
  test_pipe_cache_host_stale
  test_pipe_cache_auto_destroy
  test_pipe_cache_alloc_refusals
  test_backbuf_publish
  test_conf_value_lifetime
  test_loc_cache_dir
  test_dtpthread_recursive
  test_history_snapshot
  test_masks_raster_contract
  test_stroke_raster
  test_drawlayer_batch_raster
  test_import_jobs
)

foreach(test ${LIB_ANSEL_UNIT_TESTS})
  add_cmocka_test(${test}
                  SOURCES ${test}.c
                  COMPILE_OPTIONS -Wno-error=deprecated-declarations # cmocka >= 2 deprecates check_expected et al.
                  LINK_LIBRARIES ${CMOCKA_LINK_TARGET} lib_ansel)
  target_include_directories(${test} PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/src
    ${CMAKE_BINARY_DIR})
  # The project builds everything with the INSTALL rpath (CMAKE_SKIP_BUILD_RPATH at the
  # top level), so binaries only resolve libansel.so once installed. Tests must run from
  # the build tree under ctest -- give them a build rpath pointing at the just-built lib.
  set_target_properties(${test} PROPERTIES
    SKIP_BUILD_RPATH FALSE
    BUILD_WITH_INSTALL_RPATH FALSE
    BUILD_RPATH "$<TARGET_FILE_DIR:lib_ansel>")
  if(WIN32)
    add_dependencies(${test} unittests_lib_ansel_dll)
  endif()
endforeach()

# brush.c and paint.c live in the drawlayer IOP plugin, not in lib_ansel. Both are written to
# be independent of the module internals, so the test compiles them directly rather than
# reaching into the plugin.
target_sources(test_drawlayer_batch_raster PRIVATE
  ${CMAKE_SOURCE_DIR}/src/iop/drawlayer/brush.c
  ${CMAKE_SOURCE_DIR}/src/iop/drawlayer/paint.c)
target_compile_definitions(test_import_jobs PRIVATE
  ANSEL_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  ANSEL_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}")

target_sources(test_tagging_completion PRIVATE
  ${CMAKE_SOURCE_DIR}/src/libs/tagging_completion.c)
target_sources(test_tagging_completion_model PRIVATE
  ${CMAKE_SOURCE_DIR}/src/libs/tagging_completion.c)
set_tests_properties(test_tagging_completion PROPERTIES SKIP_RETURN_CODE 77)
