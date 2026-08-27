include(CheckCCompilerFlag)

macro (CHECK_C_COMPILER_FLAG_AND_ENABLE_IT _FLAG)
  # The result variable name is passed to the compiler as -D<name>, so it must be a valid
  # macro name. Interpolating the raw flag left a '-' in it -- "-DC_COMPILER_UNDERSTANDS_-Wall"
  # -- which clang reports as "ISO C99 requires whitespace after the macro name". Harmless on
  # its own, but CMAKE_REQUIRED_FLAGS below carries the project's -Werror, so EVERY probe
  # failed under clang and every flag checked this way was silently never enabled. That
  # included -Wthread-safety, i.e. the entire lock-annotation analysis, on every clean clang
  # build. Sanitise to an identifier.
  string(REGEX REPLACE "[^A-Za-z0-9]" "_" _FLAG_ID "${_FLAG}")
  set(_RESULT "C_COMPILER_UNDERSTANDS_${_FLAG_ID}")

  set(CMAKE_REQUIRED_FLAGS_ORIG "${CMAKE_REQUIRED_FLAGS}")
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_C_FLAGS}")

  CHECK_C_COMPILER_FLAG("${_FLAG}" ${_RESULT})

  if(${${_RESULT}})
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${_FLAG}")
  endif()

  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_ORIG}")
endmacro ()
