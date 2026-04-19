add_subdirectory(iop)

add_cmocka_test(test_sample
                SOURCES test_sample.c
                LINK_LIBRARIES cmocka)

# Windows: main-method requires the wrapper provided by lib-darktable
if(WIN32)
    target_link_libraries(test_sample PRIVATE lib_ansel)
    _copy_required_library(test_sample lib_ansel)
endif(WIN32)
