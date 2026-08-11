# CMocka unit tests for the sealed modules under src/.
#
# Each test is a standalone binary linking lib_ansel. The database tests open the one
# connection on ":memory:" and drive ALL their setup through the repository APIs -- no raw
# SQL and no sqlite3 handle, which keeps tools/check_module_boundaries.sh at its zero
# baselines. A test that cannot seed its rows through the public API has found a hole in
# the API, not a reason to reach for the connection.

set(DATABASE_UNIT_TESTS
  test_image_repository
  test_history_repository
  test_preset_repository
  test_tag_selection_metadata
  test_metadata_notify
)

foreach(test ${DATABASE_UNIT_TESTS})
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
    # lib_ansel and its DLLs live beside the main executable; run the tests from there
    set_target_properties(${test} PROPERTIES
                          RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/../..)
  endif()
endforeach()
