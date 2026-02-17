#!/bin/bash
#   This file is part of the Ansel project.
#   Copyright (C) 2023 lologor.
#   Copyright (C) 2024 jakehl.
#   Copyright (C) 2025 Miguel Moquillon.
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

# Script to generate DMG image from application bundle

#   
# Exit in case of error
set -e -o pipefail
trap 'echo "${BASH_SOURCE[0]}{${FUNCNAME[0]}}:${LINENO}: Error: command \`${BASH_COMMAND}\` failed with exit code $?"' ERR

# Define application name
PROGN=Ansel

# Go to directory of script
scriptDir=$(dirname "$0")
buildDir="${scriptDir}/../../install"
cd "$buildDir"/

# Generate symlink to applications folder for easier drag & drop within dmg
ln -s /Applications package/ || true

# Create temporary rw image
hdiutil create -srcfolder package -volname "${PROGN}" -fs HFS+ \
	-fsargs "-c c=64,a=16,e=16" -format UDRW pack.temp.dmg

# Mount image without autoopen to create window style params
device=$(hdiutil attach -readwrite -noverify -autoopen "pack.temp.dmg" |
	egrep '^/dev/' | sed 1q | awk '{print $1}')

echo '
 tell application "Finder"
	tell disk "'${PROGN}'"
		set current view of container window to icon view
		set toolbar visible of container window to false
		set statusbar visible of container window to false
		set the bounds of container window to {400, 100, 885, 330}
		set theViewOptions to the icon view options of container window
		set arrangement of theViewOptions to not arranged
		set icon size of theViewOptions to 72
		set position of item "'${PROGN}'" of container window to {100, 100}
		set position of item "Applications" of container window to {375, 100}
		update without registering applications
	end tell
 end tell
' | osascript

# Finalizing creation
chmod -Rf go-w /Volumes/"${PROGN}"
sync
hdiutil detach ${device}
DMG="${PROGN}-$(git describe --tags | sed 's/^release-//;s/-/+/;s/-/~/;s/rc/~rc/')-$(arch)"
echo "Create DMG $DMG"
hdiutil convert "pack.temp.dmg" -format UDZO -imagekey zlib-level=9 -o "${DMG}"
rm -f pack.temp.dmg

# Sign dmg image when a certificate has been provided
if [ -n "$CODECERT" ]; then
    codesign --deep --verbose --force --options runtime -i "photos.ansel" -s "${CODECERT}" "${DMG}".dmg
fi
