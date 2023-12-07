include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

add_executable(ansel-test-variables variables.c)
target_link_libraries(ansel-test-variables lib_ansel)

if(WIN32)
  # This tester sets up a darktable instance (of sorts). Hence it expects libraries at LIBDIR
  # Easiest way to comply with this on Windows: Put tester executable in same directory as darktable executable
  set_target_properties(ansel-test-variables PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..)
endif(WIN32)

add_subdirectory(unittests)
