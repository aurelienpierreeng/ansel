#
# Install fixed sized icons
#
set(FIXED_SIZES "16x16" "22x22" "24x24" "32x32" "48x48" "64x64" "256x256" "scalable")
set(THEME hicolor)
foreach(PIXMAP_SIZE ${FIXED_SIZES})
  # install apps icons
  file(GLOB PIXMAP_FILES ${CMAKE_CURRENT_SOURCE_DIR}/${PIXMAP_SIZE}/*.png ${CMAKE_CURRENT_SOURCE_DIR}/${PIXMAP_SIZE}/*.svg)
  install(FILES ${PIXMAP_FILES} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/${THEME}/${PIXMAP_SIZE}/apps COMPONENT DTApplication)
endforeach(PIXMAP_SIZE)

file(GLOB OTHER_PIXMAPS "*.png" "*.svg")
install(FILES ${OTHER_PIXMAPS} DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/ansel/pixmaps COMPONENT DTApplication)
