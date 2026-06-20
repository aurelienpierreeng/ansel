include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

add_executable(ansel-test-variables variables.c)
target_link_libraries(ansel-test-variables lib_ansel)
target_include_directories(ansel-test-variables PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/src)

add_executable(ansel-test-styles styles.c)
target_link_libraries(ansel-test-styles lib_ansel)
target_include_directories(ansel-test-styles PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/src)
target_compile_definitions(ansel-test-styles PRIVATE
  ANSEL_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  ANSEL_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}")

if(WIN32)
  # This tester sets up a darktable instance (of sorts). Hence it expects libraries at LIBDIR
  # Easiest way to comply with this on Windows: Put tester executable in same directory as darktable executable
  set_target_properties(ansel-test-variables PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..)
  set_target_properties(ansel-test-styles PROPERTIES
                        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/..)
endif(WIN32)

add_subdirectory(unittests)
