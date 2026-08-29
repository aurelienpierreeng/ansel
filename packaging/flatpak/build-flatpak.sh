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
# Requires: flatpak, flatpak-builder, git and jq installed on the host.
#
# Builds the Flatpak from the local working tree and produces two things in
# ${BUILD_DIR}:
#
#   repo/                        an OSTree repository, ready to be served over
#                                HTTP as a Flatpak remote (this is what gives
#                                users `flatpak update`)
#   Ansel-<version>-x86_64.flatpak   a single-file bundle, for one-off installs
#
# Signing the repository is opt-in: set GPG_KEY_ID (and optionally GPG_HOMEDIR)
# and both the repo and the bundle are signed. An unsigned repo still works,
# but every client has to add it with --no-gpg-verify.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "${SCRIPT_DIR}/../.." && pwd)

LOCAL_MANIFEST=${LOCAL_MANIFEST:-"${SCRIPT_DIR}/photos.ansel.Ansel.json"}

# The app id is read from the manifest rather than hardcoded here. It has to
# agree with the id in data/photos.ansel.Ansel.appdata.xml.in and with the name
# of the .desktop file data/CMakeLists.txt installs, and three independent
# copies of it is how they drifted apart in the first place.
APP_ID=${APP_ID:-"$(jq -r '.["app-id"]' "${LOCAL_MANIFEST}")"}
SOURCE_DIR=${SOURCE_DIR:-"${ROOT_DIR}"}
VERSION=${VERSION:-"$(sh "${ROOT_DIR}/tools/get_git_version_string.sh")"}
BUNDLE_NAME=${BUNDLE_NAME:-"Ansel-${VERSION}-x86_64.flatpak"}

BUILD_DIR=${BUILD_DIR:-"${ROOT_DIR}/build/flatpak"}
# The manifest is built into a directory of its own, holding a copy of
# everything that sits next to the original. flatpak-builder resolves every
# relative path in a manifest -- an included module file, a "type": "file"
# source, a script a module runs -- against the directory the manifest is in,
# so a manifest generated somewhere else silently loses its companions.
MANIFEST_DIR=${MANIFEST_DIR:-"${BUILD_DIR}/manifest"}
MANIFEST_PATH=${MANIFEST_PATH:-"${MANIFEST_DIR}/${APP_ID}.json"}
REPO_DIR=${REPO_DIR:-"${BUILD_DIR}/repo"}
SHARED_MODULES_DIR=${SHARED_MODULES_DIR:-"${MANIFEST_DIR}/shared-modules"}

GPG_KEY_ID=${GPG_KEY_ID:-""}
GPG_HOMEDIR=${GPG_HOMEDIR:-""}

if [[ -z "${APP_ID}" || "${APP_ID}" == "null" ]]; then
  echo "ERROR: could not read app-id from ${LOCAL_MANIFEST}." >&2
  exit 1
fi

mkdir -p "${MANIFEST_DIR}"
cp -a "${SCRIPT_DIR}/." "${MANIFEST_DIR}/"

# Point the app module at this working tree. Everything else in the manifest is
# used verbatim.
jq --arg source_dir "${SOURCE_DIR}" \
   '.modules |= map(
      if (type == "object") and (.name == "ansel") then
        .sources = [{"type": "dir", "path": $source_dir}]
      else
        .
      end
    )' \
   "${LOCAL_MANIFEST}" > "${MANIFEST_PATH}"

if ! jq -e --arg source_dir "${SOURCE_DIR}" \
  'any(.modules[];
       (type == "object")
       and (.name == "ansel")
       and ((.sources // []) | any(.type == "dir" and .path == $source_dir)))' \
  "${MANIFEST_PATH}" >/dev/null; then
  echo "ERROR: no local source module found after patching manifest." >&2
  exit 1
fi

flatpak remote-add --if-not-exists --user flathub https://flathub.org/repo/flathub.flatpakrepo
if [[ ! -d "${SHARED_MODULES_DIR}" ]]; then
  git clone --depth 1 https://github.com/flathub/shared-modules.git "${SHARED_MODULES_DIR}"
fi

GPG_ARGS=()
if [[ -n "${GPG_KEY_ID}" ]]; then
  GPG_ARGS+=("--gpg-sign=${GPG_KEY_ID}")
  if [[ -n "${GPG_HOMEDIR}" ]]; then
    GPG_ARGS+=("--gpg-homedir=${GPG_HOMEDIR}")
  fi
fi

flatpak-builder --user --force-clean --install-deps-from=flathub \
  --repo="${REPO_DIR}" "${GPG_ARGS[@]}" \
  "${BUILD_DIR}/build" "${MANIFEST_PATH}"

# Generate the summary file clients need to see the app, and drop the objects
# no ref points at any more so a repo republished every night does not grow
# without bound.
flatpak build-update-repo --prune "${GPG_ARGS[@]}" "${REPO_DIR}"

flatpak build-bundle "${GPG_ARGS[@]}" \
  "${REPO_DIR}" "${BUILD_DIR}/${BUNDLE_NAME}" "${APP_ID}"

echo "Repository: ${REPO_DIR}"
echo "Bundle:     ${BUILD_DIR}/${BUNDLE_NAME}"
