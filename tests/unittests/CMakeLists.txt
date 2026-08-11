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
