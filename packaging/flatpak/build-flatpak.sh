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
# Requires: flatpak, flatpak-builder, and git installed on the host.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/../.." && pwd)

LOCAL_MANIFEST=${LOCAL_MANIFEST:-"${SCRIPT_DIR}/photos.ansel.ansel.json"}
APP_ID=${APP_ID:-"photos.ansel.app"}
COMMAND=${COMMAND:-"ansel"}
DESKTOP_FILE=${DESKTOP_FILE:-"${APP_ID}.desktop"}
APPDATA_FILE=${APPDATA_FILE:-"${APP_ID}.appdata.xml"}
SOURCE_DIR=${SOURCE_DIR:-"${ROOT_DIR}"}

BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build/flatpak"}
MANIFEST_PATH=${MANIFEST_PATH:-"${BUILD_DIR}/${APP_ID}.json"}
SHARED_MODULES_DIR=${SHARED_MODULES_DIR:-"${BUILD_DIR}/shared-modules"}
FLATHUB_DARKTABLE_DIR=${FLATHUB_DARKTABLE_DIR:-"${BUILD_DIR}/org.darktable.Darktable"}

mkdir -p "${BUILD_DIR}"

TMP_DIR=$(mktemp -d)
cleanup() {
  rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

cp "${LOCAL_MANIFEST}" "${TMP_DIR}/manifest.json"

jq --arg app_id "${APP_ID}" \
   --arg command "${COMMAND}" \
   --arg desktop "${DESKTOP_FILE}" \
   --arg appdata "${APPDATA_FILE}" \
   --arg source_dir "${SOURCE_DIR}" \
   '
   .["app-id"] = $app_id |
   .command = $command |
   (if has("rename-desktop-file") then .["rename-desktop-file"] = $desktop else . end) |
   (if has("rename-appdata-file") then .["rename-appdata-file"] = $appdata else . end) |
   (if has("rename-icon") then .["rename-icon"] = "ansel" else . end) |
   def is_app_module:
     (type == "object") and
     ((has("name") and (.name | test("darktable|ansel"; "i"))) or
      ([.sources[]? | .url? // empty] | any(test("darktable|ansel"; "i"))));
   .modules |= (map(
     if is_app_module then
       .name = "ansel" |
       .sources = [{"type":"dir","path":$source_dir}]
     else
       .
     end
   ))
   ' "${TMP_DIR}/manifest.json" > "${MANIFEST_PATH}"

if ! jq -e --arg source_dir "${SOURCE_DIR}" \
  'any(.modules[];
      if type=="object" then
        (.sources? | if type=="array" then any(.[]?; type=="object" and .type=="dir" and .path==$source_dir) else false end)
      else
        false
      end
    )' \
  "${MANIFEST_PATH}" >/dev/null; then
  echo "ERROR: no local source module found after patching manifest." >&2
  exit 1
fi

flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
if [[ ! -d "${SHARED_MODULES_DIR}" ]]; then
  git clone --depth 1 https://github.com/flathub/shared-modules.git "${SHARED_MODULES_DIR}"
fi
flatpak-builder --user --force-clean --install-deps-from=flathub \
  --repo="${BUILD_DIR}/repo" "${BUILD_DIR}/build" "${MANIFEST_PATH}"

flatpak build-bundle "${BUILD_DIR}/repo" "${BUILD_DIR}/${APP_ID}.flatpak" "${APP_ID}"
