# We don't control the code under here, so don't turn warnings into errors
set(_PARENT_C_FLAGS "${CMAKE_C_FLAGS}")
set(_PARENT_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-error")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-error")

add_library(whereami STATIC "${CMAKE_CURRENT_SOURCE_DIR}/whereami/src/whereami.c")
target_include_directories(whereami PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/whereami/src/")

set(BUILD_TESTING_SAVE "${BUILD_TESTING}")

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_BENCHMARKING OFF)
set(WITH_OPENMP ${USE_OPENMP} CACHE BOOL "" FORCE)

# Feed RawSpeed's own find_package(OpenMP) with the AppleClang/Homebrew libomp hints.
if(APPLE AND USE_OPENMP AND DEFINED OpenMP_ROOT)
  set(OpenMP_C_INCLUDE_DIR   "${OpenMP_ROOT}/include" CACHE PATH "" FORCE)
  set(OpenMP_CXX_INCLUDE_DIR "${OpenMP_ROOT}/include" CACHE PATH "" FORCE)

  set(OpenMP_C_FLAGS   "-Xclang -fopenmp" CACHE STRING "" FORCE)
  set(OpenMP_CXX_FLAGS "-Xclang -fopenmp" CACHE STRING "" FORCE)

  set(OpenMP_C_LIB_NAMES   "omp" CACHE STRING "" FORCE)
  set(OpenMP_CXX_LIB_NAMES "omp" CACHE STRING "" FORCE)
  set(OpenMP_omp_LIBRARY   "${OpenMP_ROOT}/lib/libomp.dylib" CACHE FILEPATH "" FORCE)
endif()

set(RAWSPEED_PATH "${CMAKE_CURRENT_SOURCE_DIR}/rawspeed")
set(RAWSPEED_BINARY_PREFIX "ansel")

if(NOT(EXISTS "${RAWSPEED_PATH}" AND IS_DIRECTORY "${RAWSPEED_PATH}" AND EXISTS "${RAWSPEED_PATH}/CMakeLists.txt"))
  message(FATAL_ERROR "RawSpeed submodule not found. You probably want to run:\n$ git submodule init\nand then\n$ git submodule update")
endif()

set(OPENCL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/OpenCL")

if(NOT(EXISTS "${OPENCL_PATH}" AND IS_DIRECTORY "${OPENCL_PATH}" AND EXISTS "${OPENCL_PATH}/CL/opencl.h"))
  message("OpenCL submodule not found. You probably want to run:\n$ git submodule init\nand then\n$ git submodule update\nOr let the OS headers to be used.")
endif()

set(CMAKE_INSTALL_DATAROOTDIR_SAVE "${CMAKE_INSTALL_DATAROOTDIR}")

set(CMAKE_C_FLAGS_SAVE "${CMAKE_C_FLAGS}")
set(CMAKE_CXX_FLAGS_SAVE "${CMAKE_CXX_FLAGS}")

set(CMAKE_INSTALL_DATAROOTDIR "${CMAKE_INSTALL_DATAROOTDIR}/ansel")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

set(CMAKE_CXX_CLANG_TIDY_SAVE "${CMAKE_CXX_CLANG_TIDY}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE_SAVE "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE}")

unset(CMAKE_CXX_CLANG_TIDY)
unset(CMAKE_CXX_INCLUDE_WHAT_YOU_USE)

# Keep RawSpeed OpenMP setting aligned with the top-level build option
set(WITH_OPENMP ${USE_OPENMP} CACHE BOOL "Enable OpenMP support in RawSpeed" FORCE)

# RawSpeed doesn't do RelWithDebInfo builds, so we need to change that
set(RAWSPEED_BUILD_TYPE ${CMAKE_BUILD_TYPE})
set(_PARENT_BUILD_TYPE ${CMAKE_BUILD_TYPE})

if(RAWSPEED_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(RAWSPEED_BUILD_TYPE "Release")
endif()

# RawSpeed test build will fail on Release without this
set(HAVE_ZLIB_ULONGF 1 CACHE INTERNAL "Force override for broken CheckZLIB on macOS")

set(CMAKE_BUILD_TYPE ${RAWSPEED_BUILD_TYPE} CACHE STRING "" FORCE)
add_subdirectory(rawspeed)
set(CMAKE_BUILD_TYPE ${_PARENT_BUILD_TYPE} CACHE STRING "" FORCE)

if(TARGET rstest)
  target_compile_definitions(rstest PRIVATE -DRAWSPEED_SOURCE_DIR="${RAWSPEED_PATH}")
endif()

if(TARGET rsbench)
  target_compile_definitions(rsbench PRIVATE -DRAWSPEED_SOURCE_DIR="${RAWSPEED_PATH}")
endif()

# Only patch the target when OpenMP is actually enabled
if(USE_OPENMP AND TARGET rawspeed)
  target_link_libraries(rawspeed PUBLIC OpenMP::OpenMP_CXX)

  # AppleClang/Homebrew libomp sometimes still needs the header path spelled out
  if(APPLE AND DEFINED OpenMP_ROOT)
    target_include_directories(rawspeed SYSTEM PUBLIC "${OpenMP_ROOT}/include")
  endif()
endif()

set(CMAKE_INSTALL_DATAROOTDIR "${CMAKE_INSTALL_DATAROOTDIR_SAVE}")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS_SAVE}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS_SAVE}")

set(CMAKE_CXX_CLANG_TIDY "${CMAKE_CXX_CLANG_TIDY_SAVE}")
set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${CMAKE_CXX_INCLUDE_WHAT_YOU_USE_SAVE}")

set(BUILD_TESTING "${BUILD_TESTING_SAVE}" CACHE BOOL "" FORCE)

# Restore parent C flags
set(CMAKE_C_FLAGS "${_PARENT_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${_PARENT_CXX_FLAGS}")

#
# sentry-native (crash reporting). Built with the in-process backend so that no
# extra handler executable needs to be shipped, and statically so it links
# straight into lib_ansel. The HTTP transport reuses the libcurl dependency that
# Ansel already requires on Linux/macOS (WinHTTP is used on Windows).
#
if(USE_SENTRY)
  set(SENTRY_PATH "${CMAKE_CURRENT_SOURCE_DIR}/sentry-native")

  if(NOT(EXISTS "${SENTRY_PATH}" AND IS_DIRECTORY "${SENTRY_PATH}" AND EXISTS "${SENTRY_PATH}/CMakeLists.txt"))
    message(FATAL_ERROR "sentry-native submodule not found. You probably want to run:\n$ git submodule update --init src/external/sentry-native\nOr configure with -DUSE_SENTRY=OFF.")
  endif()

  set(_SENTRY_C_FLAGS_SAVE "${CMAKE_C_FLAGS}")
  set(_SENTRY_CXX_FLAGS_SAVE "${CMAKE_CXX_FLAGS}")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w")

  set(_SENTRY_SHARED_SAVE "${BUILD_SHARED_LIBS}")

  set(SENTRY_BACKEND inproc CACHE STRING "" FORCE)
  set(SENTRY_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(SENTRY_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(SENTRY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF)

  add_subdirectory(sentry-native)

  set(BUILD_SHARED_LIBS "${_SENTRY_SHARED_SAVE}")
  set(CMAKE_C_FLAGS "${_SENTRY_C_FLAGS_SAVE}")
  set(CMAKE_CXX_FLAGS "${_SENTRY_CXX_FLAGS_SAVE}")
endif()

#
# Exiv2 (metadata). Vendored as a git submodule and linked statically into lib_ansel.
#
# Ansel does not merely link Exiv2, it inherits its behaviour: which version is built
# decides what metadata an image is seen to have -- lens identity, makernote coverage,
# XMP round-tripping -- and which options it was built with decides whether CR3, AVIF
# and HEIF carry any metadata at all. Leaving that to three packagers produced three
# different products, which is what issue #474 turned out to be. doc/exiv2.md is the
# full account, including what has to be true before the pin can move to 0.28.x.
#
if(USE_BUNDLED_EXIV2)
  set(EXIV2_PATH "${CMAKE_CURRENT_SOURCE_DIR}/exiv2")

  if(NOT(EXISTS "${EXIV2_PATH}" AND IS_DIRECTORY "${EXIV2_PATH}" AND EXISTS "${EXIV2_PATH}/CMakeLists.txt"))
    message(FATAL_ERROR "Exiv2 submodule not found. You probably want to run:\n$ git submodule update --init src/external/exiv2\nOr configure with -DUSE_BUNDLED_EXIV2=OFF to link the system library instead.")
  endif()

  set(_EXIV2_C_FLAGS_SAVE "${CMAKE_C_FLAGS}")
  set(_EXIV2_CXX_FLAGS_SAVE "${CMAKE_CXX_FLAGS}")
  set(_EXIV2_SHARED_SAVE "${BUILD_SHARED_LIBS}")
  set(_EXIV2_MODULE_PATH_SAVE "${CMAKE_MODULE_PATH}")
  set(_EXIV2_TESTING_SAVE "${BUILD_TESTING}")

  # -w silences Exiv2's warnings; -Wno-error keeps Ansel's -Werror off code we do not own.
  # -Wno-register is separate because it is neither: C++17 REMOVED the register keyword, so
  # Clang rejects it outright rather than warning, and neither -w nor -Wno-error reaches a
  # hard error. The Adobe XMP SDK vendored in Exiv2's xmpsdk/ still uses it (MD5.cpp), which
  # is why every Clang and AppleClang job failed here and every GCC one did not.
  #
  # ANSEL_EXIV2_AUTO_PTR is the other half of the same story, and it has to reach Exiv2's
  # three targets AND everything that includes its headers, so it is a flag here and an
  # INTERFACE definition below. C++17 removed std::auto_ptr; libstdc++ keeps it as
  # deprecated, libc++ genuinely deletes it, and Exiv2 0.27's public headers name it 59
  # times -- Image::AutoPtr IS std::auto_ptr<Image>, at every language standard, because
  # config.h's `using auto_ptr = std::unique_ptr<T>` sits in the global namespace and the
  # headers all write the std:: qualification. So on macOS neither Exiv2 nor Ansel's own
  # exif.cc compiles without asking libc++ for it back. The macro also restores
  # unique_ptr's converting constructor from auto_ptr, which is what our
  #   std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(...))
  # call sites rely on. It is a libc++ spelling and a harmless no-op on libstdc++.
  set(ANSEL_EXIV2_AUTO_PTR _LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR)

  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -Wno-error")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -w -Wno-error -Wno-register -D${ANSEL_EXIV2_AUTO_PTR}")

  # Exiv2 0.27.x asks for cmake_minimum_required(VERSION 3.7.2). Nothing in its build
  # depends on the policies that predates; this only stops CMake 4 from refusing the
  # line outright. Older CMake ignores the variable.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.10)

  # ...and because it asks for 3.7.2, every policy introduced later is left unset in its
  # subtree, which costs us two things.
  #
  # CMP0069 (3.9): unset, INTERPROCEDURAL_OPTIMIZATION is silently dropped, so a Release
  # build would link non-LTO Exiv2 objects into an otherwise LTO'd lib_ansel and say so
  # three times in the configure log.
  #
  # CMP0077 (3.13): unset, Exiv2's own option() calls OVERRIDE the normal variables set
  # below rather than defer to them -- which would quietly hand us a shared libexiv2 and
  # Exiv2's default option set instead of ours, on a cache that does not already carry
  # those entries.
  set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)

  # Exiv2's public headers typedef Image::AutoPtr to std::auto_ptr or std::unique_ptr
  # depending on __cplusplus, so the library and every consumer of it MUST agree on the
  # standard. Ansel compiles at C++17, but src/CMakeLists.txt says so further down the
  # file than this directory is added from, and Exiv2 defaults itself to C++98 when
  # nothing else has spoken. Spell it out rather than depend on that ordering.
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # Extensions ON, unlike Ansel's own code, and NOT a copy-paste slip. The Adobe XMP SDK
  # vendored in xmpsdk/ selects its platform in XMP_Environment.h on `#if defined WIN32` --
  # the unprefixed spelling, which GCC and Clang define only in GNU mode. Under -std=c++17
  # a MinGW build therefore falls through to UNIX_ENV and XMPUtils.cpp reaches for
  # localtime_r/gmtime_r, which MinGW does not have. Exiv2's own CMake never touches
  # CXX_EXTENSIONS, so this is what it is built with everywhere else. Verified:
  #   x86_64-w64-mingw32-g++ -std=c++17   -dM -E - | grep WIN32   ->   _WIN32 only
  #   x86_64-w64-mingw32-g++ -std=gnu++17 -dM -E - | grep WIN32   ->   _WIN32 and WIN32
  set(CMAKE_CXX_EXTENSIONS ON)

  # Exiv2's findDependencies.cmake appends ${CMAKE_SOURCE_DIR}/cmake to CMAKE_MODULE_PATH
  # to reach its own finders -- which is ANSEL's root cmake directory once Exiv2 is a
  # subdirectory rather than the top-level project. Its FindIconv.cmake is then never
  # found, CMake's builtin one answers instead and sets Iconv_FOUND where Exiv2 reads
  # ICONV_FOUND, and the build silently ends up with EXV_HAVE_ICONV off: no charset
  # conversion for Exif.Photo.UserComment or Nikon UNICODE comments. Put its own module
  # directory first so its own finder wins.
  set(CMAKE_MODULE_PATH "${EXIV2_PATH}/cmake" ${CMAKE_MODULE_PATH})

  # Exiv2 includes CTest, which turns BUILD_TESTING on.
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

  # Static, so that nothing else in the process can supply a second libexiv2 and so that
  # no shared object needs shipping alongside the binary. CMP0077, forced NEW above, is
  # what makes this normal variable win over Exiv2's own option().
  set(BUILD_SHARED_LIBS OFF)

  # Options Ansel actually depends on.
  set(EXIV2_ENABLE_XMP           ON  CACHE BOOL "" FORCE)  # the whole XMP sidecar layer
  set(EXIV2_ENABLE_PNG           ON  CACHE BOOL "" FORCE)  # PNG metadata (needs zlib)
  set(EXIV2_ENABLE_BMFF          ON  CACHE BOOL "" FORCE)  # CR3, AVIF, HEIF -- REQUIRED, see src/CMakeLists.txt
  set(EXIV2_ENABLE_LENSDATA      ON  CACHE BOOL "" FORCE)  # Nikon lens tables
  set(EXIV2_ENABLE_PRINTUCS2     ON  CACHE BOOL "" FORCE)  # UCS-2 tag values

  # On Windows this is what defines EXV_UNICODE_PATH, which is the only reason a path
  # holding a single non-ASCII character can be opened at all. See doc/exiv2.md.
  if(WIN32)
    set(EXIV2_ENABLE_WIN_UNICODE ON  CACHE BOOL "" FORCE)
  else()
    set(EXIV2_ENABLE_WIN_UNICODE OFF CACHE BOOL "" FORCE)
  endif()

  # Native language support is off deliberately, and it is not simply a matter of
  # flipping this. Exiv2's po/CMakeLists.txt calls GETTEXT_CREATE_TRANSLATIONS(), which
  # runs `msgmerge --update` against the .po files IN THE SOURCE TREE -- so every build
  # would leave the submodule dirty. What it buys is the translation of the tag values
  # Exiv2::Metadatum::print() returns; doc/exiv2.md records what it would take to get
  # that back without the write.
  set(EXIV2_ENABLE_NLS           OFF CACHE BOOL "" FORCE)

  # Ansel reads metadata from files, never over the network, and decodes video with
  # nothing. Off here is also what keeps libcurl and libssh out of Exiv2's link line.
  set(EXIV2_ENABLE_VIDEO         OFF CACHE BOOL "" FORCE)
  set(EXIV2_ENABLE_WEBREADY      OFF CACHE BOOL "" FORCE)
  set(EXIV2_ENABLE_CURL          OFF CACHE BOOL "" FORCE)
  set(EXIV2_ENABLE_SSH           OFF CACHE BOOL "" FORCE)

  set(EXIV2_BUILD_SAMPLES        OFF CACHE BOOL "" FORCE)
  set(EXIV2_BUILD_EXIV2_COMMAND  OFF CACHE BOOL "" FORCE)
  set(EXIV2_BUILD_UNIT_TESTS     OFF CACHE BOOL "" FORCE)
  set(EXIV2_BUILD_FUZZ_TESTS     OFF CACHE BOOL "" FORCE)
  set(EXIV2_BUILD_DOC            OFF CACHE BOOL "" FORCE)
  set(EXIV2_TEAM_PACKAGING       OFF CACHE BOOL "" FORCE)

  # Exiv2's mainSetup.cmake defines an `uninstall` target unless one already exists, and
  # overwrites ${CMAKE_BINARY_DIR}/cmake_uninstall.cmake on the way. Ansel's root
  # CMakeLists.txt creates that target before it descends into src/ precisely so this
  # guard fires; if you ever move it back below add_subdirectory(src), the build dies on
  # a duplicate target here.
  if(NOT TARGET uninstall)
    message(FATAL_ERROR "Ansel's `uninstall` target must be created before add_subdirectory(src), or the bundled Exiv2 will create its own and collide with it.")
  endif()

  # Exiv2 installs its static archive, its public headers, its pkg-config file, its
  # CMake export set and its man page. Ansel links the archive into lib_ansel and ships
  # no Exiv2 development files, so none of that belongs in our install tree -- nor,
  # downstream of it, in the AppImage or the Windows installer. Shadowing install() is
  # the only lever a sub-project offers: the real command stays reachable as _install(),
  # and ANSEL_SUPPRESS_INSTALL is a directory variable, so it is set here and is invisible
  # to every other directory in the tree.
  #
  # A FUNCTION, and never a macro. Overriding a command this way makes the override GLOBAL
  # for the rest of the configure -- it outlives this directory and every install() in the
  # tree goes through it, packaging/CMakeLists.txt included. In a macro, ${ARGV} is textual
  # substitution: the argument is pasted into the body and then PARSED AGAIN, so every
  # ${...} inside a string argument is expanded a second time and every backslash escape is
  # eaten once more. install(CODE "<script>") is exactly the shape that destroys -- it
  # carries \${...} deliberately, to be expanded at install time and not now.
  #
  # That is not hypothetical. It shipped: the Windows installer's third-party DLL collection
  # (packaging/CMakeLists.txt) is an install(CODE) block, and the macro reduced it to a
  # script whose ${filename}, ${FILES} and ${EXTRA_DEPS} were all empty and whose regex had
  # lost its escapes. It copied nothing, the .exe went from 144 MB to 118 MB, and users got
  # an application missing libcurl, libheif, libopenexr and exchndl.dll.
  #
  # In a function ARGV is a real variable; expanding it yields the argument VALUES, and
  # CMake does not re-expand what a variable expansion produced. The forwarding is faithful.
  function(install)
    if(NOT ANSEL_SUPPRESS_INSTALL)
      _install(${ARGV})
    endif()
  endfunction()
  set(ANSEL_SUPPRESS_INSTALL TRUE)

  add_subdirectory(exiv2)

  set(ANSEL_SUPPRESS_INSTALL FALSE)

  # Exiv2 writes exv_conf.h and exiv2lib_export.h to ${CMAKE_BINARY_DIR} -- which is the
  # top of OUR build tree, since it is a subdirectory here and not the top-level project.
  # Copy them somewhere named, so what we hand consumers is a directory that holds those
  # two headers and nothing else.
  set(EXIV2_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/exiv2-generated")
  file(COPY "${CMAKE_BINARY_DIR}/exv_conf.h" "${CMAKE_BINARY_DIR}/exiv2lib_export.h"
       DESTINATION "${EXIV2_GENERATED_INCLUDE_DIR}")

  # Exiv2 puts include/exiv2 itself on its PUBLIC include path so its own sources can say
  # #include "basicio.hpp". Consumers must not inherit that: it would leave Exiv2's
  # config.h one -I away from every Ansel translation unit that writes #include "config.h"
  # and means its own. Ansel only ever includes <exiv2/...>, so the interface needs
  # include/ and the generated-header directory, and nothing else.
  #
  # SYSTEM, i.e. -isystem rather than -I, because we do not own these headers and Ansel
  # builds with -Werror. Exiv2 0.27's public headers name std::auto_ptr, which C++17
  # deprecates, so a Debug build of any translation unit including <exiv2/exiv2.hpp> fails
  # on two dozen -Wdeprecated-declarations errors that nothing on our side can fix. This
  # is precisely what -isystem is for.
  #
  # WIN32_LEAN_AND_MEAN is dropped from the interface for a different reason: Exiv2 marks
  # it PUBLIC, rwlock.hpp is a public header that includes <windows.h>, and Ansel's Windows
  # build has always compiled against that header with the full windows.h. Presenting the
  # bundled build with the same surface as the system one keeps this change about paths.
  set_target_properties(exiv2lib PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES
      "$<BUILD_INTERFACE:${EXIV2_PATH}/include>;$<BUILD_INTERFACE:${EXIV2_GENERATED_INCLUDE_DIR}>"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES
      "$<BUILD_INTERFACE:${EXIV2_PATH}/include>;$<BUILD_INTERFACE:${EXIV2_GENERATED_INCLUDE_DIR}>"
    INTERFACE_COMPILE_DEFINITIONS
      "")

  # generate_export_header() names the static escape hatch exiv2lib_STATIC and defines it
  # on Exiv2's own compilation only. Without it on our side too, EXIV2API expands to
  # __declspec(dllimport) on Windows and every symbol fails to link.
  target_compile_definitions(exiv2lib PUBLIC exiv2lib_STATIC)

  # And the consumers' half of the std::auto_ptr story explained above: every Ansel
  # translation unit that includes <exiv2/exiv2.hpp> needs it too, not just Exiv2's own.
  target_compile_definitions(exiv2lib INTERFACE ${ANSEL_EXIV2_AUTO_PTR})

  add_library(Exiv2::Exiv2 ALIAS exiv2lib)

  # What the rest of the tree reads instead of the variables FindExiv2.cmake would set.
  # project(exiv2 VERSION ...) writes the version into Exiv2's own directory scope, which
  # the parent cannot see, so ask that directory for it rather than restate the pin here.
  # Exiv2_EXV_CONF_H is what src/CMakeLists.txt probes for EXV_ENABLE_BMFF: the header we
  # actually generated, not one guessed from an include prefix.
  get_directory_property(_exiv2_version DIRECTORY "${EXIV2_PATH}" DEFINITION PROJECT_VERSION)
  set(Exiv2_VERSION "${_exiv2_version}" CACHE INTERNAL "")
  set(Exiv2_EXV_CONF_H "${EXIV2_GENERATED_INCLUDE_DIR}/exv_conf.h" CACHE INTERNAL "")

  set(BUILD_TESTING "${_EXIV2_TESTING_SAVE}" CACHE BOOL "" FORCE)
  set(CMAKE_MODULE_PATH "${_EXIV2_MODULE_PATH_SAVE}")
  set(BUILD_SHARED_LIBS "${_EXIV2_SHARED_SAVE}")
  set(CMAKE_C_FLAGS "${_EXIV2_C_FLAGS_SAVE}")
  set(CMAKE_CXX_FLAGS "${_EXIV2_CXX_FLAGS_SAVE}")

  message(STATUS "Using bundled Exiv2 ${Exiv2_VERSION} (static, ISOBMFF on)")
endif()
