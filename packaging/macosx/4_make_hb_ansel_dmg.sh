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
scriptDir=$(cd "$(dirname "$0")" && pwd)
buildDir="${scriptDir}/../../install"
cd "$buildDir"/

device=""
temp_dmg="pack.${PROGN}.$$.temp.dmg"
trap 'set +e; if [ -n "${device:-}" ]; then hdiutil detach -force "${device}" >/dev/null 2>&1; fi; rm -f "${temp_dmg:-}"' EXIT

# Generate symlink to applications folder for easier drag & drop within dmg
ln -s /Applications package/ || true

# Create a temporary rw image. Use a process-local filename so a stale mounted
# image from a previous interrupted packaging attempt cannot make hdiutil report
# "Resource busy" when the CI runner reuses the same install directory.
#
# Retried, because that is not the only way hdiutil says "Resource busy". What is MEASURED is
# that this command fails intermittently on the same code: once in six nightly DMG builds, the
# other five passing. What is INFERRED, and not observed, is which process held the folder --
# the step before this one ends on `codesign --deep' over the whole bundle, and macOS answers a
# newly signed application by looking at it, Spotlight indexing it and the security machinery
# verifying it. The retry does not depend on that guess being right: it cures any transient
# holder, whichever it was.
#
# A failed attempt leaves nothing behind that matters -- the image is named per process and the
# EXIT trap removes it -- but hdiutil may have written a partial file, so each retry clears it
# first or the next attempt fails on "file already exists" instead. The wait grows, since a
# fixed short one is the version that would still lose to a slow indexer.
dmg_attempts=3
for dmg_attempt in $(seq 1 ${dmg_attempts}); do
	if hdiutil create -srcfolder package -volname "${PROGN}" -fs HFS+ \
		-fsargs "-c c=64,a=16,e=16" -format UDRW "${temp_dmg}"; then
		break
	fi
	if [ "${dmg_attempt}" -eq "${dmg_attempts}" ]; then
		echo "hdiutil create failed ${dmg_attempts} times; giving up." >&2
		exit 1
	fi
	rm -f "${temp_dmg}"
	echo "hdiutil create failed (attempt ${dmg_attempt}/${dmg_attempts}); retrying in $((dmg_attempt * 15)) s..." >&2
	sleep $((dmg_attempt * 15))
done

# Mount image without autoopen to create window style params
device=$(hdiutil attach -readwrite -noverify -autoopen "${temp_dmg}" |
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
hdiutil detach "${device}"
device=""
# Find repo root (a parent directory that contains `tools`) and use its script.
search_dir="${scriptDir}"
while [ ! -d "${search_dir}/tools" ] && [ "${search_dir}" != "/" ]; do
	search_dir=$(dirname "${search_dir}")
done
if [ -d "${search_dir}/tools" ]; then
	tools_script="${search_dir}/tools/get_git_version_string.sh"
else
	tools_script="${scriptDir}/../../tools/get_git_version_string.sh"
fi
OUTPUT_VERSION=$(sh "${tools_script}")
DMG="${PROGN}-${OUTPUT_VERSION}-$(arch)"
echo "Create DMG $DMG"
hdiutil convert "${temp_dmg}" -format UDZO -imagekey zlib-level=9 -o "${DMG}"
rm -f "${temp_dmg}"

# Sign dmg image when a certificate has been provided
if [ -n "$CODECERT" ]; then
    codesign --deep --verbose --force --options runtime -i "photos.ansel" -s "${CODECERT}" "${DMG}".dmg
fi
