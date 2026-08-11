add_cmocka_mock_test(test_filmicrgb
                     SOURCES test_filmicrgb.c ../util/testimg.c
                     LINK_LIBRARIES lib_ansel cmocka m
                     MOCKS dt_iop_color_picker_reset)

# The project builds with the INSTALL rpath (CMAKE_SKIP_BUILD_RPATH at the top level);
# a test that must run from the build tree needs a build rpath to the just-built lib.
set_target_properties(test_filmicrgb PROPERTIES
  SKIP_BUILD_RPATH FALSE
  BUILD_WITH_INSTALL_RPATH FALSE
  BUILD_RPATH "$<TARGET_FILE_DIR:lib_ansel>")

# Windows: libs have to be copied next to the executable
if(WIN32)
    _copy_required_library(test_filmicrgb lib_ansel)
endif(WIN32)
