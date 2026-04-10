#!/usr/bin/env bash
# This file is part of the Ansel project.
# Copyright (C) 2026 Aurélien PIERRE.

set -euo pipefail

TARGET_ARCH="${1:-native}"

resolve_brew() {
  case "${TARGET_ARCH}" in
    arm64)
      if [[ -x /opt/homebrew/bin/brew ]]; then
        printf '%s\n' /opt/homebrew/bin/brew
      else
        command -v brew || true
      fi
      ;;
    x86_64)
      if [[ -x /usr/local/bin/brew ]]; then
        printf '%s\n' /usr/local/bin/brew
      else
        command -v brew || true
      fi
      ;;
    native)
      command -v brew || true
      ;;
  esac
}

BREW_BIN="${BREW_BIN:-$(resolve_brew)}"

if [[ -z "${BREW_BIN}" || ! -x "${BREW_BIN}" ]]; then
  echo "Homebrew not found for TARGET_ARCH=${TARGET_ARCH}." >&2
  echo "Expected one of:" >&2
  echo "  /opt/homebrew/bin/brew  (Apple Silicon)" >&2
  echo "  /usr/local/bin/brew     (Intel / Rosetta)" >&2
  exit 1
fi

BREW_CMD=("${BREW_BIN}")

# If an Apple Silicon machine is explicitly asked to use Intel Homebrew,
# run brew itself under Rosetta.
if [[ "$(uname -m)" == "arm64" && "${TARGET_ARCH}" == "x86_64" ]]; then
  BREW_CMD=(arch -x86_64 "${BREW_BIN}")
fi

# Force native arm execution when requested on Apple Silicon.
if [[ "$(uname -m)" == "arm64" && "${TARGET_ARCH}" == "arm64" ]]; then
  BREW_CMD=(arch -arm64 "${BREW_BIN}")
fi

echo "Using Homebrew: ${BREW_BIN}"
"${BREW_CMD[@]}" --prefix
"${BREW_CMD[@]}" update

HB_PACKAGES=(
  adwaita-icon-theme
  cmake
  cmark
  pkg-config
  cmocka
  curl
  desktop-file-utils
  exiv2
  gettext
  git
  glib
  gmic
  graphicsmagick
  gtk-mac-integration
  gtk+3
  icu4c
  intltool
  iso-codes
  jpeg-turbo
  jpeg-xl
  json-glib
  lensfun
  libavif
  libheif
  libomp
  libraw
  librsvg
  libsecret
  libsoup@2
  little-cms2
  llvm
  lua
  ninja
  openexr
  openjpeg
  osm-gps-map
  perl
  po4a
  pugixml
  sdl2
  webp
)

brew_install_status=0
if "${BREW_CMD[@]}" install "${HB_PACKAGES[@]}"; then
  :
else
  brew_install_status=$?
fi

missing_packages=()
for package in "${HB_PACKAGES[@]}"; do
  if ! "${BREW_CMD[@]}" list --formula "${package}" >/dev/null 2>&1; then
    missing_packages+=("${package}")
  fi
done

if (( ${#missing_packages[@]} > 0 )); then
  printf 'Missing Homebrew packages after install: %s\n' "${missing_packages[*]}" >&2
  exit "${brew_install_status:-1}"
fi

if (( brew_install_status != 0 )); then
  echo "brew install reported a post-install failure, but all requested packages are present." >&2
fi

# Handle keg-only libs.
"${BREW_CMD[@]}" link --force libomp libsoup@2

echo "Dependency installation complete for ${TARGET_ARCH}"