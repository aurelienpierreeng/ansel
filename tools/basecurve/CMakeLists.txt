find_package(OpenMP)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/modules")
find_library(M_LIBRARY m)
find_package(Exiv2 REQUIRED)
include_directories(${Exiv2_INCLUDE_DIR})

add_executable(ansel-curve-tool ansel-curve-tool.c exif-wrapper.cpp)
if(OPENMP_FOUND)
    set_target_properties(ansel-curve-tool PROPERTIES COMPILE_FLAGS ${OpenMP_CXX_FLAGS})
    if(OpenMP_CXX_INCLUDE_DIRS)
        include_directories(${OpenMP_CXX_INCLUDE_DIRS})
    endif()
endif()
target_link_libraries(ansel-curve-tool ${Exiv2_LIBRARY} ${OpenMP_CXX_LIBRARIES} m)

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
