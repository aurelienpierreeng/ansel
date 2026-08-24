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
  # NOT CPACK_PACKAGE_EXECUTABLES. The shortcut CPack generates from it is created with
  # no working directory, so it inherits $OUTDIR -- which the template last set to
  # $INSTDIR, one level above where every DLL and every module actually lives. Launching
  # Ansel from the Start menu and launching it from a shell in bin/ then differ in the
  # one piece of process state neither of them states out loud, and a user chasing a
  # start-up failure ends up comparing two things that were never the same.
  #
  # Written out by hand instead, so the working directory is part of the shortcut. The
  # label is capitalised for #124. CreateShortCut takes $OUTDIR as the working
  # directory, so SetOutPath immediately before it is the whole mechanism; it is put
  # back afterwards because the template's later sections rely on it.
  # Cleared, not overridden: an empty string is still a one-element list to CPack.
  unset(CPACK_PACKAGE_EXECUTABLES)
  SET(CPACK_NSIS_CREATE_ICONS_EXTRA "
      SetOutPath '$INSTDIR\\\\bin'
      CreateShortCut '$SMPROGRAMS\\\\$STARTMENU_FOLDER\\\\Ansel.lnk' '$INSTDIR\\\\bin\\\\ansel.exe'
      SetOutPath '$INSTDIR'
   ")
  SET(CPACK_NSIS_DELETE_ICONS_EXTRA "
      Delete '$SMPROGRAMS\\\\$MUI_TEMP\\\\Ansel.lnk'
   ")
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


  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

  # register dt in the Windows registry. this is needed for GIMP to find dt.
  SET(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
      WriteRegStr HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel.exe' '' '$INSTDIR\\\\bin\\\\ansel.exe'
      WriteRegStr HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel.exe' 'Path' '$INSTDIR\\\\bin'
      WriteRegStr HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel-cli.exe' '' '$INSTDIR\\\\bin\\\\ansel-cli.exe'
      WriteRegStr HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel-cli.exe' 'Path' '$INSTDIR\\\\bin'
      WriteRegStr HKLM 'SOFTWARE\\\\Classes\\\\Applications\\\\ansel.exe\\\\shell\\\\open\\\\command' '' '\\\"$INSTDIR\\\\bin\\\\ansel.exe\\\" \\\"%1\\\"'
   ")
  # An upgrade must not leave the previous version's files behind, and the uninstaller is the
  # only place in the NSIS script where "before the new files land" is true by construction:
  # .onInit runs the OLD uninstaller to completion, before any section, whenever
  # ENABLE_UNINSTALL_BEFORE_INSTALL is on. Removing our three directories wholesale here
  # covers what CPack's generated per-file Delete list does not -- a module a later build
  # stopped shipping, which the loader would otherwise still find (see
  # cmake/module-manifest.cmake for what such a leftover does).
  #
  # It must NOT be done from the install side. CPACK_NSIS_EXTRA_PREINSTALL_COMMANDS reads as
  # if it ran first, and does not: in the CPack template the component sections
  # (@CPACK_NSIS_COMPONENT_SECTIONS@) are emitted BEFORE Section "-Core installation", which
  # is where that hook lives, so with components -- and Ansel has three -- every file is
  # already extracted by the time it runs. "PREINSTALL" means before CPACK_NSIS_FULL_INSTALL,
  # which is empty in component mode. A wipe there deletes the installation it was meant to
  # protect, silently, and the installer still reports success.
  #
  # Nothing under $INSTDIR is user data: the library, the config and the caches all live under
  # %LOCALAPPDATA% and are untouched.
  SET(CPACK_NSIS_EXTRA_UNINSTALL_COMMANDS "
      DeleteRegKey HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel.exe'
      DeleteRegKey HKLM 'SOFTWARE\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\App Paths\\\\ansel-cli.exe'
      DeleteRegKey HKLM 'SOFTWARE\\\\Classes\\\\Applications\\\\ansel.exe'
      RMDir /r '$INSTDIR\\\\bin'
      RMDir /r '$INSTDIR\\\\lib'
      RMDir /r '$INSTDIR\\\\share'
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
