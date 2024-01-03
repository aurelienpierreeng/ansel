# NOTE: copy of src/external/rawspeed/cmake/compiler-versions.cmake
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "GNU C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. Version 10+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "GNU C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. Version 10+ is required.")
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "LLVM Clang C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. Version 10+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
  message(WARNING "LLVM Clang C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. Version 10+ is required.")
endif()

# XCode 14.2 (apple clang 1400.0.29.202) is based on LLVM14
if(CMAKE_C_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_C_COMPILER_VERSION VERSION_LESS 14.0.0.14000029)
  message(SEND_ERROR "XCode (Apple clang) C compiler version ${CMAKE_C_COMPILER_VERSION} is too old and is unsupported. XCode version 14.2+ is required.")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14.0.0.14000029)
  message(SEND_ERROR "XCode (Apple clang) C++ compiler version ${CMAKE_CXX_COMPILER_VERSION} is too old and is unsupported. XCode version 14.2+ is required.")
endif()

if(CMAKE_OSX_DEPLOYMENT_TARGET AND CMAKE_OSX_DEPLOYMENT_TARGET VERSION_LESS 12.5)
  message(SEND_ERROR "Targeting OSX version ${CMAKE_OSX_DEPLOYMENT_TARGET} older than 12.5 is unsupported.")
endif()