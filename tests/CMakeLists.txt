include_directories(${CMAKE_CURRENT_BINARY_DIR}/..)

if(USE_OPENCL)
  # HAVE_OPENCL changes the LAYOUT of public structs -- it gates `process_cl` and its siblings
  # in dt_iop_module_t -- so anything linking lib_ansel must see the same value as the library,
  # or it reads the library's fields at its own offsets. The tests were compiling a 1088-byte
  # dt_iop_module_t against the library's 1104, and a `module->dev` set by a test read back as
  # NULL inside lib_ansel. Nothing warned; the tests simply exercised a different struct.
  #
  # add_definitions() is DIRECTORY-scoped and src/ sets it for itself, but the test tree is a
  # SIBLING of src/, not a child, so it inherited nothing. Setting it here covers this directory
  # and unittests/ below it.
  #
  # Do NOT express this as `target_compile_definitions(ansel_deps INTERFACE ...)' in src/: that
  # reaches every consumer of ansel_deps, including targets with no OpenCL include path, and
  # common/opencl.h includes <CL/cl.h> inside this very guard. src/widgets then failed to build
  # on Windows and macOS while Linux happened to resolve the header anyway.
  add_definitions(-DHAVE_OPENCL=1)
endif(USE_OPENCL)

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

add_executable(ansel-test-masks-geometry masks/masks_geometry.c)
target_link_libraries(ansel-test-masks-geometry lib_ansel)
target_include_directories(ansel-test-masks-geometry PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_BINARY_DIR}/src)
# The project builds with the INSTALL rpath, so binaries only resolve libansel.so once
# installed. Give this one a build rpath, same as tests/unittests does and for the same reason.
set_target_properties(ansel-test-masks-geometry PROPERTIES
  SKIP_BUILD_RPATH FALSE
  BUILD_WITH_INSTALL_RPATH FALSE
  BUILD_RPATH "$<TARGET_FILE_DIR:lib_ansel>")
target_compile_definitions(ansel-test-masks-geometry PRIVATE
  ANSEL_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  ANSEL_TEST_BINARY_DIR="${CMAKE_BINARY_DIR}")
add_test(NAME masks_geometry
         COMMAND ansel-test-masks-geometry ${CMAKE_BINARY_DIR}/masks-geometry)

add_subdirectory(unittests)
