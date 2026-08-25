find_package(OpenMP)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/modules")
find_library(M_LIBRARY m)

# src/external/CMakeLists.txt already defines Exiv2::Exiv2 when the submodule is used,
# and tools/ is descended into after src/, so the alias is visible here. Only reach for
# the system library when Ansel itself is not linking the bundled one.
if(NOT TARGET Exiv2::Exiv2)
  find_package(Exiv2 REQUIRED)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND Exiv2_VERSION VERSION_GREATER_EQUAL "0.28.0")
    set(CMAKE_CXX_STANDARD 17)
endif()

add_executable(ansel-curve-tool ansel-curve-tool.c exif-wrapper.cpp)
if(OPENMP_FOUND)
    set_target_properties(ansel-curve-tool PROPERTIES COMPILE_FLAGS ${OpenMP_CXX_FLAGS})
    if(OpenMP_CXX_INCLUDE_DIRS)
        include_directories(${OpenMP_CXX_INCLUDE_DIRS})
    endif()
endif()
target_link_libraries(ansel-curve-tool Exiv2::Exiv2 ${OpenMP_CXX_LIBRARIES} m)

configure_file(README.cmake.md README.md)

install(TARGETS
            ansel-curve-tool
        RUNTIME DESTINATION
            ${CMAKE_INSTALL_LIBEXECDIR}/ansel/tools)

install(PROGRAMS
            ansel-curve-tool-helper
        DESTINATION
            ${CMAKE_INSTALL_LIBEXECDIR}/ansel/tools)


install(FILES
            plot.basecurve plot.tonecurve
        DESTINATION
            ${CMAKE_INSTALL_DATAROOTDIR}/ansel/tools/basecurve/)
install(FILES
            ${CMAKE_CURRENT_BINARY_DIR}/README.md
        RENAME
            README.tools.basecurve.md
        DESTINATION
            ${CMAKE_INSTALL_DOCDIR}/)
