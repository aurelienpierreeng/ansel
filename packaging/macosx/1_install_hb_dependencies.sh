#!/bin/bash
#
# Script to install required homebrew packages
#

# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Check if brew exists
if ! [ -x "$(command -v brew)" ]; then
    echo 'Homebrew not found. Follow instructions as provided by https://brew.sh/ to install it.' >&2
    exit 1
else
    echo "Found homebrew running in $(uname -m)-based environment."
fi

# Make sure that homebrew is up-to-date
brew update
brew upgrade

# Define homebrew dependencies
# Homebrew disabled jsonschema, which is only needed to verify json during build
# so removed it.
hbDependencies="adwaita-icon-theme \
    cmake \
    pkg-config \
    curl \
    desktop-file-utils \
    exiv2 \
    gettext \
    git \
    glib \
    gmic \
    imagemagick \
    gtk-mac-integration \
    gtk+3 \
    icu4c \
    intltool \
    iso-codes \
    jpeg-turbo \
    jpeg-xl \
    json-glib \
    lensfun \
    libavif \
    libheif \
    libomp \
    libraw \
    librsvg \
    libsecret \
    little-cms2 \
    llvm \
    lua \
    ninja \
    openexr \
    openjpeg \
    osm-gps-map \
    perl \
    po4a \
    pugixml \
    sdl2 \
    webp"

function find_in_array() {
    local element="${1}"
    local array=(${@:2})
    for i in ${array[@]}; do
	if [[ "${i}" == "${element}" ]]; then
	    return 0
	fi
    done
    return 1
}

# Categorize dependency list
standalone=
deps=
notfound=
hbInstalled=$( brew list --formula --quiet )
hbLeaves=$( brew leaves --installed-on-request )
for hbDependency in ${hbDependencies}; do
    if find_in_array "${hbDependency}" ${hbInstalled[@]}; then
        if find_in_array "${hbDependency}" ${hbLeaves[@]}; then
            standalone="${hbDependency} ${standalone}"
        else
            deps="${hbDependency} ${deps}"
        fi
    else
      notfound="${hbDependency} ${notfound}"
    fi
done

# Show installed dependencies
if [ "${standalone}" -o "${deps}" ]; then
    echo
    echo "Installed Dependencies:"
    (
        brew list --formula --quiet --versions ${standalone}
        brew list --formula --quiet --versions ${deps} | sed -e 's/$/ (autoinstalled)/'
    ) | sort
fi

# Install missing dependencies
if [ "${notfound}" ]; then
    echo
    echo "Missing Dependencies:"
    echo "${notfound}"

    brew install ${notfound}
fi

# Dependencies that must be linked
brew link --force libomp libsoup@2
