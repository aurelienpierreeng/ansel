
IF(WIN32 AND NOT BUILD_MSYS2_INSTALL)

  set(plugin_dest_dir lib/ansel/plugins)
  set(APPS "\${CMAKE_INSTALL_PREFIX}/bin/ansel.exe")  # paths to executables
  get_filename_component(MINGW_PATH ${CMAKE_CXX_COMPILER} PATH )

  find_program(cygcheck_BIN cygcheck)
  if(${cygcheck_BIN} STREQUAL "cygcheck_BIN-NOTFOUND")

    # use the slower fixup_bundle

    set(DIRS ${MINGW_PATH}) # directories to search for prerequisites

    # run Bundle utilities
    INSTALL(CODE "
      file(GLOB_RECURSE DTPLUGINS
        \"\${CMAKE_INSTALL_PREFIX}/${plugin_dest_dir}/*${CMAKE_SHARED_LIBRARY_SUFFIX}\")
      list(APPEND DTPLUGINS \"\${CMAKE_INSTALL_PREFIX}/bin/libansel.dll\")
      include(\"${CMAKE_ROOT}/Modules/BundleUtilities.cmake\")
      set(gp_tool \"objdump\")
      fixup_bundle(\"${APPS}\" \"\${DTPLUGINS}\" \"${DIRS}\")
      " COMPONENT DTApplication)

  else()

    # use the fast cygcheck variant and pray that no dependencies are missed

    string(REPLACE "/" "\\\\\\\\" DIRS "${MINGW_PATH}") # add lots of escaping

    INSTALL(CODE "
      set(EXTRA_DEPS)

      function(process_file filename)
        execute_process(COMMAND \"${cygcheck_BIN}\" \"\${filename}\" OUTPUT_VARIABLE FILES ERROR_QUIET)
        string(REGEX MATCHALL \"${DIRS}[^\\n\\r]*[dD][lL][lL]\" FILES_LIST \"\${FILES}\")
        set(EXTRA_DEPS \${EXTRA_DEPS} \${FILES_LIST} PARENT_SCOPE)
      endfunction()

      file(GLOB_RECURSE DTPLUGINS \"\${CMAKE_INSTALL_PREFIX}/${plugin_dest_dir}/*${CMAKE_SHARED_LIBRARY_SUFFIX}\")
      list(APPEND DTPLUGINS \"\${CMAKE_INSTALL_PREFIX}/bin/libansel.dll\")

      process_file(\"${APPS}\")
      foreach(filename IN LISTS DTPLUGINS)
        process_file(\"\${filename}\")
      endforeach()

      if(EXTRA_DEPS)
        list(REMOVE_DUPLICATES EXTRA_DEPS)
      endif()

      # A Windows build ALWAYS needs third-party DLLs -- GTK alone is a dozen. An empty list
      # here does not mean 'nothing to copy', it means this script did not run as written, and
      # the only symptom otherwise is an installer that is quietly tens of megabytes lighter
      # and an application that will not start on a machine without MSYS2. That shipped once
      # already: a macro override of install() re-expanded this very block and emptied every
      # \${...} in it (see src/external/CMakeLists.txt). Fail the install instead.
      list(LENGTH EXTRA_DEPS _ansel_dep_count)
      if(_ansel_dep_count LESS 10)
        message(FATAL_ERROR
          \"Windows dependency collection found only \${_ansel_dep_count} DLL(s) under ${DIRS}. \"
          \"Expected dozens. The package would be missing its runtime libraries.\")
      endif()
      message(STATUS \"Bundling \${_ansel_dep_count} third-party DLLs\")

      # install(...) doesn't work in here, and file(INSTALL ...) doesn't put anything into installers,
      # so we have to go over the list of files and do it manually
      foreach(filename IN LISTS EXTRA_DEPS)
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy \"\${filename}\" \"\${CMAKE_INSTALL_PREFIX}/bin/\")
      endforeach()

      " COMPONENT DTApplication)

  endif()

ENDIF(WIN32 AND NOT BUILD_MSYS2_INSTALL)
