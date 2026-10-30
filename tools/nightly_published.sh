#!/usr/bin/env bash
#   This file is part of the Ansel project.
#   Copyright (C) 2026 Guillaume Stutin.
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
# Tell a scheduled nightly whether the packages it would build are already published.
#
# Every nightly asks it first -- win, lin, mac, flatpak and docker -- so that all of them agree
# on what "already published" means.
#
# The nightlies run on a cron, whether master moved or not, and used to rebuild the same head
# every night: four nights in a row on f484018567, each of them an hour or more per platform
# for packages identical to the ones already in the release. A package's name carries the
# version string, which ends in the abbreviated commit hash (`0.0.0+5038.gf484018567`), so a
# release asset of the right shape whose hash is a prefix of the head's means this very commit
# was already built and published for that platform, and building it again adds nothing.
#
# The name is matched on its hash, NOT rebuilt from `git describe`. Both halves of that string
# depend on the clone rather than on the commit: the count comes from a shallow history (the
# nightlies' `fetch-depth: 5000` is what makes the retired v0.0.0 tag look like an ancestor at
# all), and the abbreviation grows with the number of objects in the repository -- the same
# commit abbreviates to 9 digits in a blobless clone and to 10 in the build's. Matching the
# hash needs neither, so a depth-1 checkout is enough to run this.
#
# Each argument is an asset name in which `@VERSION@` stands for the version string. Every
# `nightly-YYYY-MM` release is looked in, not only the current month's: a month is only the
# container a night's packages land in, and a head published on the last night of one month is
# as published on the first night of the next. The manifest walks the months newest first, so
# it finds that package where it is.
#
# The commit is HEAD of the working directory, or $GITHUB_SHA outside a repository. Needs
# GH_TOKEN and GITHUB_REPOSITORY, as a workflow step provides them.
#
# Exits 0 when every named asset is published for that commit, 1 when at least one is missing,
# and prints which is which.
#
# Usage:
#   tools/nightly_published.sh 'Ansel-@VERSION@-x86_64.AppImage' 'Ansel-@VERSION@-x86_64.AppImage.zsync'

set -u

sha="$(git rev-parse HEAD 2>/dev/null)" || sha="${GITHUB_SHA:-}"
if [ -z "${sha}" ]; then
  echo "no commit to look for: not in a repository and GITHUB_SHA is unset" >&2
  exit 1
fi
# One line per asset, `<release tag> <asset name>`. nightly-prune.yml keeps twelve months, so
# one page of a hundred releases holds every one of them.
#
# A release list that cannot be read answers "not published", so the nightly builds as it did
# before it asked: a wasted build is cheaper than a night with no package. The warning is what
# keeps that from passing unseen.
# shellcheck disable=SC2016  # `$t' is jq's variable, not the shell's.
if ! published="$(gh api "repos/${GITHUB_REPOSITORY}/releases?per_page=100" \
  --jq '.[] | select(.tag_name | test("^nightly-[0-9]{4}-[0-9]{2}$")) | .tag_name as $t | .assets[] | "\($t) \(.name)"')"; then
  echo "::warning::could not list the nightly releases; every package is taken as missing"
  published=""
fi

# The version string ends in `g<hash>` when a tag describes the commit, and is the bare hash
# when none does (tools/get_git_version_string.sh); whatever follows the last `g' is the hash
# either way, since hex digits never contain one.
_published_for_head()
{
  local head="$1" tail="$2" tag name version hash
  while IFS=' ' read -r tag name; do
    [ -n "${name}" ] || continue
    case "${name}" in
      "${head}"*"${tail}") ;;
      *) continue ;;
    esac
    version="${name#"${head}"}"
    version="${version%"${tail}"}"
    hash="${version##*g}"
    [[ "${hash}" =~ ^[0-9a-f]{7,40}$ ]] || continue
    if [ "${sha#"${hash}"}" != "${sha}" ]; then
      echo "${tag}: ${name}"
      return 0
    fi
  done <<< "${published}"
  return 1
}

status=0
for pattern in "$@"; do
  if found="$(_published_for_head "${pattern%%@VERSION@*}" "${pattern#*@VERSION@}")"; then
    echo "published in ${found}"
  else
    echo "not published: ${pattern} for ${sha:0:10}"
    status=1
  fi
done
exit "${status}"
