add_cmocka_mock_test(test_filmicrgb
                     SOURCES test_filmicrgb.c ../util/testimg.c
                     COMPILE_OPTIONS -Wno-error=deprecated-declarations # cmocka >= 2 deprecates check_expected et al.
                     LINK_LIBRARIES lib_ansel ${CMOCKA_LINK_TARGET} m
                     MOCKS dt_iop_color_picker_reset)

add_cmocka_mock_test(test_embedded_lens_vendors
                     SOURCES test_embedded_lens_vendors.c
                             ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/embedded_lens.cc
                             ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/embedded_lens_vendors.cc
                             ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/sony.cc
                             ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/fuji.cc
                             ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/dng.cc
                             ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/olympus.cc
                     LINK_LIBRARIES lib_ansel cmocka
                     MOCKS )

add_cmocka_mock_test(test_lens_params_v7
                     SOURCES test_lens_params_v7.cc
                     LINK_LIBRARIES lib_ansel cmocka
                     MOCKS )
set_target_properties(test_lens_params_v7 PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

add_cmocka_mock_test(test_lens_predicates
                     SOURCES test_lens_predicates.c
                             ${CMAKE_SOURCE_DIR}/src/iop/lens_predicates.cc
                     LINK_LIBRARIES lib_ansel cmocka
                     MOCKS )
target_compile_definitions(test_lens_predicates PRIVATE BUILD_TESTING)

# The project builds with the INSTALL rpath (CMAKE_SKIP_BUILD_RPATH at the top level);
# a test that must run from the build tree needs a build rpath to the just-built lib.
set_target_properties(test_filmicrgb test_embedded_lens_vendors test_lens_params_v7 test_lens_predicates test_lens_process test_lens_process_mutate_tca_gate test_lens_process_mutate_rb_route
  PROPERTIES
  SKIP_BUILD_RPATH FALSE
  BUILD_WITH_INSTALL_RPATH FALSE
  BUILD_RPATH "$<TARGET_FILE_DIR:lib_ansel>")

set(_lens_process_sources test_lens_process.cc
                          test_develop_context.cc
                          ${CMAKE_SOURCE_DIR}/src/iop/lens.cc
                          ${CMAKE_SOURCE_DIR}/src/iop/lens_predicates.cc
                          ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/embedded_lens.cc
                          ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/embedded_lens_vendors.cc
                          ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/sony.cc
                          ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/fuji.cc
                          ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/dng.cc
                          ${CMAKE_SOURCE_DIR}/src/iop/embedded_lens/olympus.cc)

add_cmocka_mock_test(test_lens_process SOURCES ${_lens_process_sources} LINK_LIBRARIES lib_ansel cmocka MOCKS )
set_target_properties(test_lens_process PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_definitions(test_lens_process PRIVATE BUILD_TESTING)

add_cmocka_mock_test(test_lens_process_mutate_tca_gate SOURCES ${_lens_process_sources} LINK_LIBRARIES lib_ansel cmocka MOCKS )
set_target_properties(test_lens_process_mutate_tca_gate PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_definitions(test_lens_process_mutate_tca_gate PRIVATE BUILD_TESTING LENS_PROCESS_MUTATE_TCA_GATE TEST_LENS_PROCESS_EXPECT_MUTATION)

add_cmocka_mock_test(test_lens_process_mutate_rb_route SOURCES ${_lens_process_sources} LINK_LIBRARIES lib_ansel cmocka MOCKS )
set_target_properties(test_lens_process_mutate_rb_route PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_definitions(test_lens_process_mutate_rb_route PRIVATE BUILD_TESTING LENS_PROCESS_MUTATE_RB_ROUTE TEST_LENS_PROCESS_EXPECT_MUTATION)

add_cmocka_mock_test(test_dng_opcode
                      SOURCES test_dng_opcode.c
                              ${CMAKE_SOURCE_DIR}/src/common/dng_opcode.c
                      LINK_LIBRARIES lib_ansel cmocka
                      MOCKS dt_print)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_filmicrgb lib_ansel)
    _copy_required_library(test_embedded_lens_vendors lib_ansel)
    _copy_required_library(test_lens_params_v7 lib_ansel)
    _copy_required_library(test_lens_predicates lib_ansel)
    _copy_required_library(test_lens_process lib_ansel)
    _copy_required_library(test_lens_process_mutate_tca_gate lib_ansel)
    _copy_required_library(test_lens_process_mutate_rb_route lib_ansel)
    _copy_required_library(test_dng_opcode lib_ansel)
endif(WIN32)
