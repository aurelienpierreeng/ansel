# src/widgets/ — reusable GTK widgets, built as their own target.
#
# Self-contained by construction (see README.md): no application state, and nothing included
# from gui/ or above. That is what makes a separate target meaningful here — this list can be
# read as the module's contents rather than as an arbitrary slice of a global glob.
#
# Deliberately an explicit list, not a GLOB: the parent CMakeLists uses FILE(GLOB) patterns,
# where a path matching no file is dropped with no configure error. An explicit list of
# existing files does not have that failure mode.

set(ANSEL_WIDGETS_SOURCES
  accelerators.c
  bauhaus.c
  button.c
  collapsible_section.c
  container.c
  dialog.c
  drawingarea.c
  expander.c
  focus_peaking.c
  gradientslider.c
  gtkentry.c
  icon.c
  icon_cell_renderer.c
  label.c
  notebook.c
  paint.c
  popup.c
  resize_handle.c
  resetlabel.c
  scroll_wrap.c
  sidepanel.c
  thumbnail_btn.c
  togglebutton.c
  widget_settings.c
  widget_style.c
)

add_library(ansel_widgets STATIC ${ANSEL_WIDGETS_SOURCES})

# Built into lib_ansel, which is a shared object.
set_target_properties(ansel_widgets PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

# Includes resolve from src/ (widgets/foo.h) and from the build dir (config.h).
target_include_directories(ansel_widgets
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries(ansel_widgets PUBLIC ansel_deps)
