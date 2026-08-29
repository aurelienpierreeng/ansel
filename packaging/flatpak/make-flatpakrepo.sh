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

# Print the .flatpakrepo file that adds the nightly repository as a remote:
#     flatpak remote-add --if-not-exists ansel https://<host>/ansel.flatpakrepo
# Usage: make-flatpakrepo.sh <repo url> [gpg key id]
# With a key id (or GPG_KEY_ID in the environment), the public key is embedded so
# clients verify the repository; without one, they need --no-gpg-verify.
set -euo pipefail
url=${1:?repository url}
key=${2:-${GPG_KEY_ID:-}}
cat <<EOF
[Flatpak Repo]
Title=Ansel nightly
Url=${url}/
Homepage=https://ansel.photos/
Comment=Nightly builds of Ansel, rebuilt every morning from the master branch
Description=Nightly builds of Ansel, rebuilt every morning from the master branch. Not a stable release.
Icon=https://ansel.photos/logo.svg
EOF
if [[ -n "${key}" ]]; then
  echo "GPGKey=$(gpg --export "${key}" | base64 -w0)"
fi
