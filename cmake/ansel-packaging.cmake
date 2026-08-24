set(CPACK_PACKAGE_NAME "${CMAKE_PROJECT_NAME}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "The digital darkroom")
set(CPACK_PACKAGE_CONTACT "https://ansel.photos/")
set(CPACK_PACKAGE_VENDOR "The Ansel project")

set(CPACK_SOURCE_IGNORE_FILES
   "/.gitignore"
   "${CMAKE_BINARY_DIR}/"
   "/.git/"
   "/.deps/"
   "/.build/"
)
set(CPACK_PACKAGE_EXECUTABLES ansel)
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_GENERATOR "TGZ")
SET(CPACK_SOURCE_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")

# Set package for unix
if(UNIX)
  # Try to find architecture
  execute_process(COMMAND uname -m OUTPUT_VARIABLE CPACK_PACKAGE_ARCHITECTURE)
  string(STRIP "${CPACK_PACKAGE_ARCHITECTURE}" CPACK_PACKAGE_ARCHITECTURE)
  # Try to find distro name and distro-specific arch
  execute_process(COMMAND lsb_release -is OUTPUT_VARIABLE LSB_ID)
  execute_process(COMMAND lsb_release -rs OUTPUT_VARIABLE LSB_RELEASE)
  string(STRIP "${LSB_ID}" LSB_ID)
  string(STRIP "${LSB_RELEASE}" LSB_RELEASE)
  set(LSB_DISTRIB "${LSB_ID}${LSB_RELEASE}")
  if(NOT LSB_DISTRIB)
    set(LSB_DISTRIB "unix")
  endif(NOT LSB_DISTRIB)

  if("${LSB_DISTRIB}" MATCHES "Fedora|Mandriva")
    make_directory(${CMAKE_BINARY_DIR}/packaging/rpm)
    set(CPACK_GENERATOR "RPM")
    set(CPACK_RPM_PACKAGE_ARCHITECTURE ${CPACK_PACKAGE_ARCHITECTURE})
    set(CPACK_RPM_PACKAGE_RELEASE "1")

  endif("${LSB_DISTRIB}" MATCHES "Fedora|Mandriva")
endif(UNIX)

# Set package peoperties for Windows
if(WIN32)
  set(CPACK_GENERATOR "NSIS")
  # Second element is the shortcut's label, and it is what the Start menu shows (#124).
  # The first is the executable's base name and must stay as it is spelled on disk.
  set(CPACK_PACKAGE_EXECUTABLES "ansel" "Ansel")
  # Deliberately NOT capitalised: this is $INSTDIR's last component and the uninstall
  # registry key, so changing it moves the install and orphans every existing one --
  # including the uninstaller that ENABLE_UNINSTALL_BEFORE_INSTALL looks for. #124 is
  # about what the user READS; this is what the machine matches on.
  set(CPACK_PACKAGE_INSTALL_DIRECTORY "${CMAKE_PROJECT_NAME}")
  # There is a bug in NSIS that does not handle full unix paths properly. Make
  # sure there is at least one set of four (4) backlasshes.
  #SET(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/data/pixmaps/256x256/ansel.png")
  SET(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/data/pixmaps/dt_logo_128x128.ico")
  SET(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/data/pixmaps/dt_logo_128x128.ico")
  SET(CPACK_NSIS_INSTALLED_ICON_NAME "bin\\\\${CMAKE_PROJECT_NAME}.exe")
  SET(CPACK_NSIS_DISPLAY_NAME "Ansel")
  # Names the installer window, its page headers, and -- because the template defines no
  # MUI_STARTMENUPAGE_DEFAULTFOLDER, so MUI falls back to Name -- the Start menu FOLDER.
  # Left to CPack it derives from CPACK_PACKAGE_INSTALL_DIRECTORY, which is the install
  # directory's name and is deliberately still lowercase.
  SET(CPACK_NSIS_PACKAGE_NAME "Ansel")
  SET(CPACK_NSIS_HELP_LINK "https://ansel.photos/en/doc/install")
  SET(CPACK_NSIS_URL_INFO_ABOUT "https://ansel.photos")
  SET(CPACK_NSIS_MODIFY_PATH OFF)
  SET(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

  # An upgrade must not leave the previous version's files behind.
  #
  # ENABLE_UNINSTALL_BEFORE_INSTALL above runs the old uninstaller first, which is the
  # polite path -- but it only removes what that uninstaller recorded, and it does
  # nothing at all when the previous installation is damaged, was interrupted, or has
  # lost its uninstaller. What survives then is a mixed tree: today's libansel.dll
  # beside a plugin from a build whose struct layouts have since moved. Ansel used to
  # load whatever shared objects it found in its module directories, and the only gate
  # was DT_MODULE_VERSION, a hand-bumped constant that does not move when a struct grows
  # a member -- so such a plugin was accepted, and then read a layout that no longer
  # existed. The manifests (see cmake/module-manifest.cmake) stop it being LOADED; this
  # stops it being LEFT THERE.
  #
  # Only the three directories we own, and only when the target really is an Ansel
  # install: NSIS lets the user pick the directory, and RMDir /r on a hand-typed
  # "C:\\Program Files" would be a catastrophe rather than a cleanup. The second test
  # exists because the first one cannot be relied on -- a damaged install is exactly the
  # case where ansel.exe may be the file that went missing.
  SET(CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS "
      IfFileExists '$INSTDIR\\\\bin\\\\ansel.exe' AnselWipePrevious 0
      IfFileExists '$INSTDIR\\\\lib\\\\ansel\\\\*.*' AnselWipePrevious 0
      Goto AnselWipeDone
      AnselWipePrevious:
        DetailPrint 'Removing the previous installation from $INSTDIR'
        RMDir /r '$INSTDIR\\\\bin'
        RMDir /r '$INSTDIR\\\\lib'
        RMDir /r '$INSTDIR\\\\share'
      AnselWipeDone:
   ")

  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

  # register dt in the Windows registry. this is needed for GIMP to find dt.
  SET(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
      WriteRegStr HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel.exe' '' '$INSTDIR\\\\bin\\\\ansel.exe'
      WriteRegStr HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel-cli.exe' '' '$INSTDIR\\\\bin\\\\ansel-cli.exe'
      WriteRegStr HKLM 'SOFTWARE\\\\Classes\\\\Applications\\\\ansel.exe\\\\shell\\\\open\\\\command' '' '\\\"$INSTDIR\\\\bin\\\\ansel.exe\\\" \\\"%1\\\"'
   ")
  SET(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
      DeleteRegKey HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel.exe'
      DeleteRegKey HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel-cli.exe'
      DeleteRegKey HKLM 'SOFTWARE\\\\Classes\\\\Applications\\\\ansel.exe'
  ")

  # also associate dt with all the supported image file types
  foreach(EXTENSION ${DT_SUPPORTED_EXTENSIONS})
    SET(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "${CPACK_NSIS_EXTRA_INSTALL_COMMANDS}
      WriteRegStr HKLM 'SOFTWARE\\\\Classes\\\\.${EXTENSION}\\\\OpenWithList\\\\ansel.exe' '' ''
    ")
    SET(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "${CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS}
      DeleteRegKey HKLM 'SOFTWARE\\\\Classes\\\\.${EXTENSION}\\\\OpenWithList\\\\ansel.exe'
    ")
  endforeach(EXTENSION)
endif(WIN32)

include(CPack)

# More descriptive names for each of the components
CPACK_ADD_COMPONENT(DTApplication DISPLAY_NAME "ansel main application" REQUIRED)
CPACK_ADD_COMPONENT(DTDebugSymbols DISPLAY_NAME "Debug symbols" REQUIRED)
CPACK_ADD_COMPONENT(DTDocuments DISPLAY_NAME "Documentation and help files")

ADD_CUSTOM_TARGET(pkgsrc
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/src/version_gen.c ${CMAKE_SOURCE_DIR}/src/version_gen.c
  COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target package_source
  COMMAND ${CMAKE_COMMAND} -E remove ${CMAKE_SOURCE_DIR}/src/version_gen.c
)

add_dependencies(pkgsrc generate_version)
