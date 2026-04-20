#!/bin/bash
#   This file is part of the Ansel project.
#   Copyright (C) 2022-2023, 2025-2026 Aurélien PIERRE.
#   Copyright (C) 2023 Alynx Zhou.
#   
#   Ansel is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#   
#   Ansel is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#   
#   You should have received a copy of the GNU General Public License
#   along with Ansel.  If not, see <http://www.gnu.org/licenses/>.

#   
# Build Ansel within an AppDir directory
# Then package it as an .AppImage
# Call this script from the Ansel root folder like `bash .ci/ci-script-appimage.sh`
# Copyright (c) Aurélien Pierre - 2022
#   
# For local builds, purge and clean build pathes if any
#if [ -d "build" ];
#then yes | rm -R build;
#fi;

if [ -z "${BASH_VERSION:-}" ]; then
  exec bash "$0" "$@"
fi

if [ -d "AppDir" ];
then yes | rm -R AppDir;
fi;

mkdir build
mkdir AppDir
cd build

export CXXFLAGS="-g -O3 -fno-strict-aliasing -ffast-math -fno-finite-math-only"
export CFLAGS="$CXXFLAGS"

## AppImages require us to install everything in /usr, where root is the AppDir
export DESTDIR=../AppDir
cmake .. -DCMAKE_INSTALL_PREFIX=/usr -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBINARY_PACKAGE_BUILD=1 \
  -DRAWSPEED_ENABLE_LTO=ON \
  -DBUILD_NOISE_TOOLS=ON \
  -DCMAKE_INSTALL_LIBDIR=lib64 \
  -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib'
cmake --build . --target install --parallel $(nproc)

# Grab lensfun database. You should run `sudo lensfun-update-data` before making
# AppImage, we did this in CI.
mkdir -p ../AppDir/usr/share/lensfun
cp -a /var/lib/lensfun-updates/* ../AppDir/usr/share/lensfun

# Deal with errors like:
# Could not load a pixbuf ...
# pixbuf loaders or the mime database could not be found
mkdir -p ../AppDir/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders
cp /usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0/loaders/* \
   ../AppDir/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders/

# Import theme assets
cp -r /usr/share/icons/Adwaita ../AppDir/usr/share/icons/
mkdir -p ../AppDir/usr/share/glib-2.0
cp -r /usr/share/glib-2.0/schemas ../AppDir/usr/share/glib-2.0/
glib-compile-schemas ../AppDir/usr/share/glib-2.0/schemas

# Mime database
cp -r /usr/share/mime ../AppDir/usr/share/

## Get the latest Linuxdeploy and its Gtk plugin to package everything
wget -c "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
wget -c "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
chmod +x linuxdeploy-x86_64.AppImage linuxdeploy-plugin-gtk.sh

export DEPLOY_GTK_VERSION="3"
export LINUXDEPLOY_OUTPUT_VERSION=$(sh ../tools/get_git_version_string.sh)

export LDAI_UPDATE_INFORMATION="gh-releases-zsync|aurelienpierreeng|ansel|v0.0.0|Ansel-*-x86_64.AppImage.zsync"

# Fix https://github.com/linuxdeploy/linuxdeploy/issues/272 on Fedora
export NO_STRIP=true

# Our plugins link against libansel, it's not in system, so tell linuxdeploy
# where to find it. Don't use LD_PRELOAD here, linuxdeploy cannot see preloaded
# libraries.
ANSEL_LIBDIR=""
for candidate in ../AppDir/usr/lib64/ansel ../AppDir/usr/lib/ansel ../AppDir/usr/lib/x86_64-linux-gnu/ansel; do
  if [ -d "${candidate}" ]; then
    ANSEL_LIBDIR="${candidate}"
    break
  fi
done
if [ -z "${ANSEL_LIBDIR}" ]; then
  echo "ERROR: Could not locate installed ansel libraries in AppDir." >&2
  find ../AppDir/usr -maxdepth 4 -type d -name ansel >&2
  exit 1
fi
ANSEL_LIBROOT="$(dirname "${ANSEL_LIBDIR}")"
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:${ANSEL_LIBROOT}/"

# Using `--deploy-deps-only` to tell linuxdeploy also collect dependencies for
# libraries in this dir, but don't copy those libraries. On the contrary,
# `--library` will copy both libraries and their dependencies, which is not what
# we want, we already installed our plugins.
# `--depoly-deps-only` apparently doesn't recurse in subfolders, everything
# needs to be declared.
./linuxdeploy-x86_64.AppImage \
  --appdir ../AppDir \
  --plugin gtk \
  --executable ../AppDir/usr/bin/ansel \
  --executable ../AppDir/usr/bin/ansel-cli \
  --executable ../AppDir/usr/bin/ansel-cltest \
  --executable ../AppDir/usr/bin/ansel-cmstest \
  --exclude-library 'libgomp.so*' \
  --deploy-deps-only "${ANSEL_LIBDIR}" \
  --deploy-deps-only "${ANSEL_LIBDIR}/views" \
  --deploy-deps-only "${ANSEL_LIBDIR}/plugins" \
  --deploy-deps-only "${ANSEL_LIBDIR}/plugins/imageio/format" \
  --deploy-deps-only "${ANSEL_LIBDIR}/plugins/imageio/storage" \
  --deploy-deps-only "${ANSEL_LIBDIR}/plugins/lighttable"

# Keep the exact runtime SONAMEs that the staged executables and modules
# request inside AppDir. This stays generic across distro SONAME revisions:
# compare plugin-only DT_NEEDED entries against the core binaries and what
# linuxdeploy already bundled, then add only the remaining libraries
# explicitly.
RUNTIME_LIBDIR="../AppDir/usr/lib"
mkdir -p "${RUNTIME_LIBDIR}"
APPDIR_LIBDIRS=( ../AppDir/usr/lib ../AppDir/usr/lib64 ../AppDir/usr/lib/x86_64-linux-gnu )
RUNTIME_LIBRARY_ARGS=()

ANSEL_CORELIB=""
for candidate in ../AppDir/usr/lib64/libansel.so ../AppDir/usr/lib/libansel.so ../AppDir/usr/lib/x86_64-linux-gnu/libansel.so; do
  if [ -f "${candidate}" ]; then
    ANSEL_CORELIB="${candidate}"
    break
  fi
done

mapfile -t CORE_NEEDED_LIBRARIES < <(
  {
    readelf -d \
      ../AppDir/usr/bin/ansel \
      ../AppDir/usr/bin/ansel-cli \
      ../AppDir/usr/bin/ansel-cltest \
      ../AppDir/usr/bin/ansel-cmstest 2>/dev/null
    if [ -n "${ANSEL_CORELIB}" ]; then
      readelf -d "${ANSEL_CORELIB}" 2>/dev/null
    fi
  } | awk -F'[][]' '/\(NEEDED\)/ { print $2 }' | sort -u
)

mapfile -t PLUGIN_NEEDED_LIBRARIES < <(
  find "${ANSEL_LIBDIR}" -type f -name '*.so' -exec readelf -d {} + 2>/dev/null \
    | awk -F'[][]' '/\(NEEDED\)/ { print $2 }' | sort -u
)

for soname in "${PLUGIN_NEEDED_LIBRARIES[@]}"; do
  if [ -z "${soname}" ]; then
    continue
  fi

  case "${soname}" in
    libgomp.so.*)
      continue
      ;;
  esac

  if printf '%s\n' "${CORE_NEEDED_LIBRARIES[@]}" | grep -Fxq -- "${soname}"; then
    continue
  fi

  APPDIR_LIBRARY=""
  for appdir_lib in "${APPDIR_LIBDIRS[@]}"; do
    if [ -e "${appdir_lib}/${soname}" ]; then
      APPDIR_LIBRARY="${appdir_lib}/${soname}"
      break
    fi
  done

  if [ -n "${APPDIR_LIBRARY}" ]; then
    continue
  fi

  HOST_LIBRARY="$(ldconfig -p | awk -v so="${soname}" '$1 == so { print $NF; exit }')"
  if [ -z "${HOST_LIBRARY}" ]; then
    HOST_LIBRARY="$(find /lib /lib64 /usr/lib /usr/lib64 /usr/lib/x86_64-linux-gnu -name "${soname}" -print 2>/dev/null | head -n 1)"
  fi
  if [ -z "${HOST_LIBRARY}" ]; then
    echo "ERROR: Could not resolve ${soname} on the build host." >&2
    exit 1
  fi

  cp -avL "${HOST_LIBRARY}" "${RUNTIME_LIBDIR}/"
  RUNTIME_LIBRARY_ARGS+=( --library "${HOST_LIBRARY}" )
done

if [ "${#RUNTIME_LIBRARY_ARGS[@]}" -gt 0 ]; then
  ./linuxdeploy-x86_64.AppImage \
    --appdir ../AppDir \
    --exclude-library 'libgomp.so*' \
    "${RUNTIME_LIBRARY_ARGS[@]}"
fi

# The GTK plugin forces Adwaita inside the AppImage. Keep the bundled theme as
# a fallback, but let the host session pick the actual GTK and icon themes so
# toolbar and symbolic icons match the desktop where possible.
if [ -f ../AppDir/apprun-hooks/linuxdeploy-plugin-gtk.sh ]; then
  cat >> ../AppDir/apprun-hooks/linuxdeploy-plugin-gtk.sh <<'EOF'

# Ansel AppImage runtime overrides.
if [ -z "${ANSEL_APPIMAGE_FORCE_GTK_THEME:-}" ]; then
  unset GTK_THEME
fi

# Host sessions often export GTK_MODULES for optional desktop integrations.
# Those modules are not required for Ansel itself and regularly trigger
# warnings when an AppImage built on one distro runs on another.
if [ -z "${ANSEL_APPIMAGE_ALLOW_GTK_MODULES:-}" ]; then
  unset GTK_MODULES
fi

# The ATK bridge talks to the host accessibility bus. In AppImages this often
# produces ABI mismatches with the bundled GLib/DBus stack, so disable it by
# default and let users opt back in explicitly when they need accessibility
# tooling.
if [ -z "${ANSEL_APPIMAGE_ENABLE_AT_BRIDGE:-}" ]; then
  export NO_AT_BRIDGE=1
fi

# Keep host theme and icon search paths first, and leave the AppImage assets as
# a fallback for icons that the desktop theme does not provide.
export XDG_DATA_DIRS="/usr/local/share:/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}:$APPDIR/usr/share"
EOF
fi

# Drop optional desktop-integration GTK modules from the bundle. They are not
# part of Ansel's feature set and are the main source of cross-distro startup
# warnings such as missing colorreload/unity integrations.
find ../AppDir/usr/lib/gtk-3.0/modules -type f \
  \( -name 'libunity-gtk-module.so' -o -name 'libwindow-decorations-gtk-module.so' \) \
  -print -delete 2>/dev/null || true

# Keep the accessibility bridge on the host side. The AppImage talks to the
# host AT-SPI session bus, and mixing that bus with bundled bridge libraries is
# a recurrent source of runtime warnings and mismatched accessibility behavior.
find ../AppDir/usr -type f \
  \( -name 'libatk-bridge-2.0.so*' -o -name 'libatspi.so*' \) \
  -print -delete

# Keep the AppImage entry point explicit so command-line arguments stay visible.
# If the AppImage is called through a symlink named like one of our tools, run
# that tool. Otherwise, if the first argument names one of our installed
# binaries, route to its applet so argv[0] still points to the selected tool.
# Fall back to the GUI entry point and forward every other argument to it.
CUSTOM_APPRUN_TEMPLATE="${PWD}/AppRun.ansel"
cat > "${CUSTOM_APPRUN_TEMPLATE}" <<'EOF'
#!/bin/sh

set -eu

APPDIR="${APPDIR:-$(dirname "$(readlink -f "$0")")}"
BINDIR="${APPDIR}/usr/bin"
TOOLDIR="${APPDIR}/usr/libexec/ansel/tools"
APPLET="$(basename "$0")"

if [ -f "${APPDIR}/apprun-hooks/linuxdeploy-plugin-gtk.sh" ]; then
  # linuxdeploy's GTK hook prepares loader, immodule and GTK search paths.
  # Source it here so the final AppImage can keep Ansel's custom AppRun rather
  # than being replaced by linuxdeploy's generated wrapper.
  # shellcheck disable=SC1091
  . "${APPDIR}/apprun-hooks/linuxdeploy-plugin-gtk.sh"
fi

# Hard override: NEVER fall back to host first
export LD_LIBRARY_PATH="$APPDIR/usr/lib:$APPDIR/usr/lib64:$APPDIR/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export GDK_PIXBUF_MODULE_FILE="$APPDIR/usr/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
export GSETTINGS_SCHEMA_DIR="$APPDIR/usr/share/glib-2.0/schemas"
export XDG_DATA_DIRS="$APPDIR/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"

if [ "${APPLET}" != "AppRun" ] && [ -x "${BINDIR}/${APPLET}" ]; then
  exec "${BINDIR}/${APPLET}" "$@"
fi

if [ "${APPLET}" != "AppRun" ] && [ -x "${TOOLDIR}/${APPLET}" ]; then
  exec "${TOOLDIR}/${APPLET}" "$@"
fi

if [ "$#" -gt 0 ] && [ "$1" != "ansel" ] && [ -x "${APPDIR}/$1" ]; then
  BINARY="$1"
  shift
  exec env --argv0="${APPDIR}/${BINARY}" "${APPDIR}/${BINARY}" "$@"
fi

if [ "$#" -gt 0 ] && [ "$1" != "ansel" ] && [ -x "${TOOLDIR}/$1" ]; then
  BINARY="$1"
  shift
  exec env --argv0="${TOOLDIR}/${BINARY}" "${TOOLDIR}/${BINARY}" "$@"
fi

if [ "$#" -gt 0 ] && [ "$1" = "ansel" ] && [ -x "${BINDIR}/ansel" ]; then
  shift
  exec env --argv0="${BINDIR}/ansel" "${BINDIR}/ansel" "$@"
fi

exec "${BINDIR}/ansel" "$@"
EOF
chmod +x "${CUSTOM_APPRUN_TEMPLATE}"
install -m 0755 "${CUSTOM_APPRUN_TEMPLATE}" ../AppDir/AppRun

# Map every installed executable to AppRun so AppImage applets can dispatch to
# the matching binary by argv[0] without hiding the selection logic elsewhere.
for binary_path in ../AppDir/usr/bin/*; do
  if [ ! -x "${binary_path}" ] || [ -d "${binary_path}" ]; then
    continue
  fi

  binary_name="$(basename "${binary_path}")"
  if [ "${binary_name}" = "ansel" ]; then
    continue
  fi

  ln -sf AppRun "../AppDir/${binary_name}"
done

# Noise profiling tools are installed in libexec because they are auxiliary
# binaries and scripts, but the AppImage still needs top-level applets so they
# can be called directly from the command line.
for binary_path in ../AppDir/usr/libexec/ansel/tools/*; do
  if [ ! -x "${binary_path}" ] || [ -d "${binary_path}" ]; then
    continue
  fi

  binary_name="$(basename "${binary_path}")"
  ln -sf AppRun "../AppDir/${binary_name}"
done

# Keep using the host OpenMP runtime. linuxdeploy may collect libgomp in either
# lib or lib64 depending on the host layout, and mixing a bundled OpenMP runtime
# with host GPU drivers is one of the AppImage-specific crash suspects here.
find ../AppDir/usr -type f -name 'libgomp.so*' -print -delete

./linuxdeploy-x86_64.AppImage \
  --appdir ../AppDir \
  --custom-apprun "${CUSTOM_APPRUN_TEMPLATE}" \
  --exclude-library 'libgomp.so*' \
  --output appimage
