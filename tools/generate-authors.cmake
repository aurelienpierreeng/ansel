cmake_minimum_required(VERSION 3.18)

if(NOT DEFINED ANSEL_SOURCE_DIR)
  message(FATAL_ERROR "ANSEL_SOURCE_DIR is required")
endif()
if(NOT DEFINED ANSEL_AUTHORS_FILE)
  message(FATAL_ERROR "ANSEL_AUTHORS_FILE is required")
endif()
if(NOT DEFINED ANSEL_SH_EXECUTABLE)
  set(ANSEL_SH_EXECUTABLE "")
endif()
if(NOT DEFINED ANSEL_BASE)
  set(ANSEL_BASE "release-3.9.0")
endif()
if(NOT DEFINED ANSEL_HEAD)
  set(ANSEL_HEAD "HEAD")
endif()

get_filename_component(_authors_dir "${ANSEL_AUTHORS_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${_authors_dir}")

set(_script "${ANSEL_SOURCE_DIR}/tools/release/generate-authors.sh")
set(_authors_result "not-run")
set(_shell "")

if(EXISTS "${_script}")
  if(ANSEL_SH_EXECUTABLE AND EXISTS "${ANSEL_SH_EXECUTABLE}")
    set(_shell "${ANSEL_SH_EXECUTABLE}")
  else()
    find_program(_bash_exe bash)
    if(_bash_exe)
      set(_shell "${_bash_exe}")
    else()
      find_program(_sh_exe sh)
      if(_sh_exe)
        set(_shell "${_sh_exe}")
      endif()
    endif()
  endif()
endif()

if(_shell)
  execute_process(
    COMMAND "${_shell}" "${_script}" "${ANSEL_BASE}" "${ANSEL_HEAD}"
    WORKING_DIRECTORY "${ANSEL_SOURCE_DIR}"
    OUTPUT_FILE "${ANSEL_AUTHORS_FILE}"
    RESULT_VARIABLE _authors_result
  )
endif()

if(_authors_result EQUAL 0)
  return()
endif()

set(_static_authors "${ANSEL_SOURCE_DIR}/AUTHORS")
if(EXISTS "${_static_authors}")
  message(WARNING "Failed to generate AUTHORS (exit code ${_authors_result}); using static ${_static_authors}")
  file(READ "${_static_authors}" _authors_contents)
  file(WRITE "${ANSEL_AUTHORS_FILE}" "${_authors_contents}")
else()
  message(WARNING "Failed to generate AUTHORS (exit code ${_authors_result}); creating placeholder AUTHORS")
  file(WRITE "${ANSEL_AUTHORS_FILE}" "AUTHORS\n\nGenerated without git/bash availability during build.\n")
endif()
