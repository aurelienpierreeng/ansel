#!/usr/bin/env bash
# Created: 2026-02-16
set -euo pipefail

if [ "${EUID:-$(id -u)}" -ne 0 ]; then
  SUDO=(sudo)
else
  SUDO=()
fi

APT_PACKAGES=(
  adwaita-icon-theme
  appstream-util
  at-spi2-core
  build-essential
  cmake
  curl
  debianutils
  desktop-file-utils
  doxygen
  gdb
  gettext
  git
  graphviz
  gstreamer1.0-tools
  intltool
  iso-codes
  libatk1.0-dev
  libavif-dev
  libavifile-0.7-dev
  libcairo2-dev
  libcolord-dev
  libcolord-gtk-dev
  libcmocka-dev
  libcmark-dev
  libcups2-dev
  libcurl4-gnutls-dev
  libdbus-glib-1-dev
  libde265-dev
  libexiv2-dev
  libexif-dev
  libfuse2
  libgdk-pixbuf2.0-dev
  libglib2.0-dev
  libgmic-dev
  libgraphicsmagick1-dev
  libgomp1
  libgtk-3-dev
  libheif-dev
  libicu-dev
  libimage-exiftool-perl
  libinih-dev
  libjpeg-dev
  libjxl-dev
  libjson-glib-dev
  liblcms2-dev
  liblensfun-bin
  liblensfun-data-v1
  liblensfun-dev
  liblensfun1
  liblua5.2-dev
  liblua5.3-dev
  libopenexr-dev
  libopenjp2-7-dev
  libosmgpsmap-1.0-dev
  libpango1.0-dev
  libpixman-1-dev
  libpng-dev
  libpugixml-dev
  libraw-dev
  librsvg2-dev
  libsaxon-java
  libsecret-1-dev
  libsdl2-dev
  libsoup2.4-dev
  libsqlite3-dev
  libtiff5-dev
  libwebp-dev
  libx11-dev
  libx265-dev
  libxcb1-dev
  libxkbcommon-dev
  libxml2-dev
  libxml2-utils
  libxshmfence-dev
  libxslt1-dev
  ninja-build
  ocl-icd-opencl-dev
  opencl-headers
  perl
  pkg-config
  po4a
  python3
  python3-jsonschema
  python3-pip
  squashfs-tools
  xsltproc
  zlib1g-dev
)

remove_pkg() {
  local remove="$1"
  local new=()
  for pkg in "${APT_PACKAGES[@]}"; do
    if [ "${pkg}" != "${remove}" ]; then
      new+=("${pkg}")
    fi
  done
  APT_PACKAGES=("${new[@]}")
}

# Read OS metadata so we can handle Ubuntu version-specific repositories.
if [ -r /etc/os-release ]; then
  . /etc/os-release
fi

version_lt() {
  local a="$1"
  local b="$2"
  if command -v dpkg >/dev/null 2>&1; then
    dpkg --compare-versions "${a}" lt "${b}"
  else
    [ "$(printf '%s\n' "${a}" "${b}" | sort -V | head -n 1)" != "${b}" ]
  fi
}

maybe_enable_backports() {
  # libjxl-dev is only in Ubuntu backports for 22.04 (jammy). For older Ubuntu
  # releases we also need backports to satisfy this critical dependency.
  if [ "${ID:-}" = "ubuntu" ] && [ -n "${VERSION_ID:-}" ] && version_lt "${VERSION_ID}" "24.04"; then
    if [ -n "${VERSION_CODENAME:-}" ]; then
      local backports_list="/etc/apt/sources.list.d/${VERSION_CODENAME}-backports.list"
      if [ ! -f "${backports_list}" ]; then
        echo "deb http://archive.ubuntu.com/ubuntu ${VERSION_CODENAME}-backports main universe" | "${SUDO[@]}" tee "${backports_list}" >/dev/null
      fi
    fi
  fi
}

enable_noble_jxl_repo() {
  # As a last resort on Ubuntu < 24.04, allow pulling libjxl* from Noble with
  # strict apt pinning so we don't upgrade unrelated packages.
  local list_file="/etc/apt/sources.list.d/ansel-noble-jxl.list"
  local pref_file="/etc/apt/preferences.d/ansel-noble-jxl.pref"

  if [ ! -f "${list_file}" ]; then
    {
      echo "deb http://archive.ubuntu.com/ubuntu noble main universe"
      echo "deb http://archive.ubuntu.com/ubuntu noble-updates main universe"
      echo "deb http://security.ubuntu.com/ubuntu noble-security main universe"
    } | "${SUDO[@]}" tee "${list_file}" >/dev/null
  fi

  if [ ! -f "${pref_file}" ]; then
    {
      echo "Package: *"
      echo "Pin: release n=noble"
      echo "Pin-Priority: 100"
      echo ""
      echo "Package: libjxl* jpeg-xl"
      echo "Pin: release n=noble"
      echo "Pin-Priority: 1001"
    } | "${SUDO[@]}" tee "${pref_file}" >/dev/null
  fi
}

if [ -n "${LLVM_VER:-}" ]; then
  APT_PACKAGES+=(
    "clang-${LLVM_VER}"
    "libc++-${LLVM_VER}-dev"
    "libclang-common-${LLVM_VER}-dev"
    "libomp-${LLVM_VER}-dev"
    "llvm-${LLVM_VER}-dev"
  )
else
  # Fallback to the distro default OpenMP runtime when LLVM_VER is not pinned.
  APT_PACKAGES+=(
    "clang"
    "libclang-dev"
    "libomp-dev"
    "llvm"
  )
fi

if [ -n "${GCC_VER:-}" ]; then
  APT_PACKAGES+=(
    "gcc-${GCC_VER}"
    "g++-${GCC_VER}"
    "libomp-dev"
    # LLVM/CLang is still required for OpenCL test-build
    "llvm"
    "clang"
  )
fi

maybe_enable_backports
"${SUDO[@]}" apt-get update

# JPEG XL support is a hard requirement. We only accept system packages to keep
# installs reproducible and aligned with distro security updates.
if ! apt-cache show libjxl-dev >/dev/null 2>&1; then
  if [ "${ID:-}" = "ubuntu" ] && [ -n "${VERSION_ID:-}" ] && version_lt "${VERSION_ID}" "24.04" && [ "${JXL_ALLOW_NOBLE:-0}" -eq 1 ]; then
    enable_noble_jxl_repo
    "${SUDO[@]}" apt-get update
  fi
  if ! apt-cache show libjxl-dev >/dev/null 2>&1; then
    echo "ERROR: libjxl-dev not available in apt repositories. Enable backports or upgrade to Ubuntu 24.04+." >&2
    echo "Set JXL_ALLOW_NOBLE=1 to allow pulling libjxl from Ubuntu 24.04 (Noble) with apt pinning." >&2
    exit 1
  fi
fi

"${SUDO[@]}" apt-get install -y "${APT_PACKAGES[@]}"
