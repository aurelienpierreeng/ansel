#!/usr/bin/env bash
#   This file is part of the Ansel project.
#   Copyright (C) 2026 Aurélien PIERRE.
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

# Created: 2026-02-16

# Version modified to support both opensuse Leap and Tumbleweed.
# As of May 2026,the immutable distros available from opensuse are made for servers,
# the desktop versions are still in alpha state(Aeon desktop/Kalpa).

####################################################################################
#Functions used to install dependancies
set -euo pipefail
#1: Tumbleweed function
function tumbleweed_function
{
    if [[ "${EUID:-$(id -u)}" -ne 0 ]]
    then
	SUDO=(sudo)
    else
	SUDO=()
    fi

    ZYPPER_PACKAGES=(Mesa-devel 
	OpenEXR-devel
	SDL2-devel
	atk-devel
	cairo-devel
	clang
	cmake
	cmark-devel
	libcolord-devel
	libcolord-gtk-devel
	libcmocka0
	libcmocka-devel
	dbus-1-glib-devel
	desktop-file-utils
	doxygen
	fdupes
	fuse
	gcc-c++
	gdb
	gettext-tools
	git
	gstreamer
	GraphicsMagick
	gnome-keyring-devel
	graphviz
	GraphicsMagick-devel
	gdk-pixbuf-devel
	gtk2-devel
	gtk3-devel
	intltool
	iso-codes
	json-glib-devel
	lensfun-devel
	libavif-devel
	libcurl-devel
	libexiv2-devel
	libexif-devel
	libheif-devel
	libheif1
	libicu-devel
	libjpeg-devel
	libjxl-devel
	liblcms2-devel
	libomp-devel
	libpng16-devel
	libraw-devel
	librsvg-devel
	libsecret-devel
	libsoup2-devel
	libtiff-devel
	libwebp-devel
	libX11-devel
	libxcb-devel
	libxkbcommon-devel
	libxml2-devel
	libxslt-devel
	libxshmfence-devel
	llvm-devel
	libgomp1
	lua53-devel
	make
	ninja
	ocl-icd-devel
	opencl-headers
	openjpeg2-devel
	libosmgpsmap-1_0-1
	libosmgpsmap-devel
	pango-devel
	perl
	libpixman-1-0
	libpixman-1-0-devel
	pkg-config
	po4a
	pugixml-devel
	python3
	python3-jsonschema
	python3-pip
	saxon10
	sqlite3-devel
	squashfs
	update-desktop-files )
    "${SUDO[@]}" zypper --non-interactive install --no-recommends "${ZYPPER_PACKAGES[@]}"
	return 0
}

##########################################################################################
#2: Leap function
function leap_function
{
    if [[ "${EUID:-$(id -u)}" -ne 0 ]]
    then
	SUDO=(sudo)
    else
	SUDO=()
    fi

    ZYPPER_PACKAGES=( 
	Mesa-devel
	OpenEXR-devel
	SDL2-devel
	atk-devel
	cairo-devel
	clang
	cmake
	cmark-devel
	libcolord-devel
	libcolord-gtk-devel
	libcmocka0
	libcmocka-devel
	dbus-1-glib-devel
	desktop-file-utils
	doxygen
	fdupes
	fuse
	gcc-c++
	gdb
	gettext-tools
	git
	gstreamer
	GraphicsMagick
	gnome-keyring-devel
	graphviz
	GraphicsMagick-devel
	gdk-pixbuf-devel
	gtk2-devel
	gtk3-devel
	intltool
	iso-codes
	json-glib-devel
	lensfun-devel
	libavif-devel
	libcurl-devel
	libexiv2-devel
	libexif-devel
	libheif-devel
	libicu-devel
	libjpeg-devel
	libjxl-devel
	liblcms2-devel
	libomp-devel
	libpng16-devel
	libraw-devel
	librsvg-devel
	libsecret-devel
	libsoup2-devel
	libtiff-devel
	libwebp-devel
	libX11-devel
	libxcb-devel
	libxkbcommon-devel
	libxml2-devel
	libxslt-devel
	libxshmfence-devel
	llvm-devel
	libgomp1
	lua53-devel
	make
	ninja
	ocl-icd-devel
	opencl-headers
	openjpeg2-devel
	libosmgpsmap-1_0-1
	libosmgpsmap-devel
	pango-devel
	perl
	libpixman-1-0
	libpixman-1-0-devel
	pkg-config
	po4a
	pugixml-devel
	python3
	python3-jsonschema
	python3-pip
	saxon10
	sqlite3-devel
	squashfs
	update-desktop-files
	libx265-215
    )
    "${SUDO[@]}" zypper --non-interactive install --no-recommends "${ZYPPER_PACKAGES[@]}"
	return 0
}

########################################################################################
#source /etc/os-release
#distro="$ID"
#searching the distro name: grep distro id from /etc/os-relase,then cut
#distro=$(grep '^ID=' /etc/os-release | cut -d= -f2 | tr -d '""')
# distro and ID
distro=$(grep '^ID=' /etc/os-release | cut -d= -f2 | tr -d '"')
ID="$distro"

# a case should make adding the immutable distros simpler,a function for them as well.
case "$ID" in
    openSUSE-tumbleweed|opensuse-tumbleweed)
        echo "OpenSUSE Tumbleweed detected"
	#exec tumbleweed function
        tumbleweed_function
        exit 0
        ;;
    openSUSE-leap|opensuse-leap)
        echo "OpenSUSE Leap detected"
	#exec leap function
        leap_function
        exit 0
        ;;
    #if none of them
    *)
      echo "Unsupported distro: $distro"
      exit 1
        ;;
esac
#######################################################################################
