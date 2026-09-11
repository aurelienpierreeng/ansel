# Apple's ld64 does not support -Wl,--wrap, which add_cmocka_mock_test() is built on --
# the mock tests cannot link on macOS, with any cmocka.
if(NOT APPLE)
  add_subdirectory(iop)
endif()

add_cmocka_test(test_sample
                SOURCES test_sample.c
                COMPILE_OPTIONS -Wno-error=deprecated-declarations # cmocka >= 2 deprecates check_expected et al.
                LINK_LIBRARIES ${CMOCKA_LINK_TARGET})

# Windows: main-method requires the wrapper provided by lib-darktable
if(WIN32)
    target_link_libraries(test_sample PRIVATE lib_ansel)
    _copy_required_library(test_sample lib_ansel)
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
  # Not database tests, but they want the same standalone-binary-linking-lib_ansel treatment,
  # and splitting the list to say so would be more ceremony than it is worth.
  test_pipe_cache_policy
  test_backbuf_publish
  test_conf_value_lifetime
  test_xmp_colorlabels
  test_xmp_import_safety
  test_xmp_tags
  test_dtpthread_recursive
  test_history_snapshot
  test_masks_raster_contract
  test_stroke_raster
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
    # Windows has no rpath: the DLLs must sit beside the executable. These tests used to be
    # pointed at the main binary's directory by a relative path, which only worked from
    # src/tests/unittests; from here the neighbouring iop tests' way is both correct and
    # independent of where the test tree sits.
    _copy_required_library(${test} lib_ansel)
  endif()
endforeach()

target_sources(test_tagging_completion PRIVATE
  ${CMAKE_SOURCE_DIR}/src/libs/tagging_completion.c)
target_sources(test_tagging_completion_model PRIVATE
  ${CMAKE_SOURCE_DIR}/src/libs/tagging_completion.c)
target_sources(test_xmp_colorlabels PRIVATE test_xmp_fixture.c)
target_sources(test_xmp_tags PRIVATE test_xmp_fixture.c)
set_tests_properties(test_tagging_completion PROPERTIES SKIP_RETURN_CODE 77)
