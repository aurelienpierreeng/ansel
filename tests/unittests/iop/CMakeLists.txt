add_cmocka_mock_test(test_filmicrgb
                     SOURCES test_filmicrgb.c ../util/testimg.c
                     LINK_LIBRARIES lib_ansel cmocka
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
                     SOURCES test_lens_params_v7.c
                     LINK_LIBRARIES lib_ansel cmocka
                     MOCKS )

add_cmocka_mock_test(test_lens_predicates
                     SOURCES test_lens_predicates.c
                             ${CMAKE_SOURCE_DIR}/src/iop/lens_predicates.cc
                     LINK_LIBRARIES lib_ansel cmocka
                     MOCKS )
target_compile_definitions(test_lens_predicates PRIVATE BUILD_TESTING)

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_filmicrgb lib_ansel)
    _copy_required_library(test_embedded_lens_vendors lib_ansel)
    _copy_required_library(test_lens_params_v7 lib_ansel)
    _copy_required_library(test_lens_predicates lib_ansel)
endif(WIN32)
