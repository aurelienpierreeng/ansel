#!/usr/bin/env bash

# Normalize Ansel and darktable darkroom GUI and processing preferences
# for side-by-side benchmarking.
#
# Design notes:
# - We edit rc files directly because the goal is to enforce the same
#   benchmark conditions before either GUI starts.
# - Panel visibility and panel sizes are stored per-view and per-layout,
#   so we write every known darkroom layout explicitly instead of relying on
#   whichever layout happened to be active last.
# - Unknown keys are harmless in rc files. Writing the full darkroom panel set
#   keeps the script robust across minor UI layout differences between forks.

set -eu

ANSEL_RC="${ANSEL_RC:-$HOME/.config/anselrc}"
DARKTABLE_RC="${DARKTABLE_RC:-$HOME/.config/darktablerc}"

SIDEBAR_SIZE="${SIDEBAR_SIZE:-350}"
BOTTOM_PANEL_SIZE="${BOTTOM_PANEL_SIZE:-120}"

timestamp="$(date +%Y%m%d-%H%M%S)"

ensure_rc_file()
{
  local file="$1"
  mkdir -p "$(dirname "$file")"
  touch "$file"
}

backup_rc_file()
{
  local file="$1"
  ensure_rc_file "$file"
  cp -a "$file" "${file}.bak.${timestamp}"
}

set_rc_key()
{
  local file="$1"
  local key="$2"
  local value="$3"
  local tmp

  ensure_rc_file "$file"
  tmp="$(mktemp)"

  awk -v key="$key" -v value="$value" '
    BEGIN { done = 0 }

    index($0, key "=") == 1 {
      if(!done) print key "=" value
      done = 1
      next
    }

    { print }

    END {
      if(!done) print key "=" value
    }
  ' "$file" > "$tmp"

  mv "$tmp" "$file"
}

set_darkroom_panels()
{
  local file="$1"
  local layout="$2"
  local prefix="darkroom/ui/${layout}"

  # Keep both sidebars visible and identically sized so module UIs use the
  # same wrapping and scrolling conditions in both applications.
  set_rc_key "$file" "${prefix}/left_visible" "true"
  set_rc_key "$file" "${prefix}/right_visible" "true"
  set_rc_key "$file" "${prefix}/left_size" "$SIDEBAR_SIZE"
  set_rc_key "$file" "${prefix}/right_size" "$SIDEBAR_SIZE"

  # Hide filmstrip and both toolbars to benchmark the central darkroom view
  # under the same chrome budget.
  set_rc_key "$file" "${prefix}/toolbar_top_visible" "false"
  set_rc_key "$file" "${prefix}/toolbar_bottom_visible" "false"
  set_rc_key "$file" "${prefix}/bottom_visible" "false"
  set_rc_key "$file" "${prefix}/bottom_size" "$BOTTOM_PANEL_SIZE"
  set_rc_key "$file" "${prefix}/panels_collapse_controls" "false"

  # Leave the header enabled so the window stays operable in both forks.
  set_rc_key "$file" "${prefix}/header_visible" "true"
  set_rc_key "$file" "${prefix}/panel_collaps_state" "0"
}

set_ansel_defaults()
{
  local layout

  for layout in 0 1 2
  do
    set_darkroom_panels "$ANSEL_RC" "$layout"
  done

  # Use the highest-quality interpolators available in the shared code path so
  # transforms and final scaling don't benchmark lower-quality shortcuts.
  set_rc_key "$ANSEL_RC" "codepaths/openmp_simd" "false"
  set_rc_key "$ANSEL_RC" "opencl_devid_darkroom" "+0"
  set_rc_key "$ANSEL_RC" "opencl_devid_preview" "+0"
  set_rc_key "$ANSEL_RC" "opencl_devid_export" "+0"
  set_rc_key "$ANSEL_RC" "opencl_devid_thumbnail" "+0"
  set_rc_key "$ANSEL_RC" "plugins/lighttable/export/pixel_interpolator_warp" "lanczos2"
  set_rc_key "$ANSEL_RC" "plugins/lighttable/export/pixel_interpolator" "lanczos3"
}

set_darktable_defaults()
{
  local layout

  for layout in 0 1 2
  do
    set_darkroom_panels "$DARKTABLE_RC" "$layout"
  done

  # Hide darkroom picture margins so the image area matches Ansel as closely
  # as darktable allows from rc settings.
  set_rc_key "$DARKTABLE_RC" "plugins/darkroom/ui/border_size" "0"
  set_rc_key "$DARKTABLE_RC" "full_window/color_assessment" "false"
  set_rc_key "$DARKTABLE_RC" "second_window/color_assessment" "false"

  # Disable preview shortcuts and enable the slow/high-quality export path.
  set_rc_key "$DARKTABLE_RC" "codepaths/openmp_simd" "false"
  set_rc_key "$DARKTABLE_RC" "opencl_device_priority" "+0/+0/+0/+0/+0"
  set_rc_key "$DARKTABLE_RC" "opencl_scheduling_profile" "very fast GPU"
  set_rc_key "$DARKTABLE_RC" "preview_downsampling" "original"
  set_rc_key "$DARKTABLE_RC" "plugins/lighttable/export/high_quality_processing" "true"
  set_rc_key "$DARKTABLE_RC" "plugins/lighttable/export/force_lcms2" "false"

  # Match interpolation quality with Ansel.
  set_rc_key "$DARKTABLE_RC" "plugins/lighttable/export/pixel_interpolator_warp" "lanczos2"
  set_rc_key "$DARKTABLE_RC" "plugins/lighttable/export/pixel_interpolator" "lanczos3"
}

backup_rc_file "$ANSEL_RC"
backup_rc_file "$DARKTABLE_RC"

set_ansel_defaults
set_darktable_defaults

printf 'Updated %s and %s\n' "$ANSEL_RC" "$DARKTABLE_RC"
printf 'Backups: %s.bak.%s and %s.bak.%s\n' "$ANSEL_RC" "$timestamp" "$DARKTABLE_RC" "$timestamp"
