#!/usr/bin/env bash
#
# Put a formula's external patches in Homebrew's download cache when GitHub now serves them with
# a checksum the formula no longer accepts, so that a source build on Intel macOS can proceed.
#
# Measured on the mac nightly of 2026-10-02: llvm 23.1.2 patches itself with
#   https://github.com/llvm/llvm-project/compare/1381ad49...40a8c7c0.diff
# and the formula expects sha256 f6dafd76..., but GitHub now serves fa40bc0a.... The content is
# unchanged: GitHub abbreviates the blob hashes on the diff's `index' lines, and the abbreviation
# grew from 13 to 14 characters as the repository grew. Truncating them back to 13 gives the
# expected sha256 exactly. A compare diff is generated, not stored, so this can happen to any
# formula patching from one, and only source builds ever download the patch -- on Intel, where
# Homebrew publishes no bottles, that is every build.
#
# Why a stale checksum is fatal and not just a retry: brew fetches everything up front and then
# builds with network access denied (llvm has had `deny_network_access!` since 2026-09-21), so a
# patch rejected at fetch time is fetched again inside the sandbox and dies there on DNS --
# `Could not resolve host: github.com`, which reads like an outage and is not one.
#
# For each external patch, the file brew would download is fetched here, outside the sandbox.
# If it already matches, nothing is done. Otherwise every shorter abbreviation of the `index'
# hashes is tried, and the file is written at the patch's cached location ONLY when one of them
# reproduces the formula's own sha256. brew then finds it there and verifies it itself, so this
# can never feed a build anything Homebrew did not already approve. When nothing matches, a note
# is printed and the build fails exactly as it would have without this script.
#
# Always exits 0: it is a best effort in front of `brew install`, never a gate.
#
# Usage:
#   tools/brew_seed_patches.sh llvm

set -uo pipefail

if ! command -v brew >/dev/null 2>&1; then
  echo "note: brew not found -- this runs on a Homebrew host only. Nothing was seeded." >&2
  exit 0
fi

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

for formula in "$@"; do
  # Loaded from its Ruby source rather than from the JSON API, which does not carry the patches
  # with their URLs and checksums. `brew cat' fetches that source when brew runs from the API.
  src="${work}/${formula}.rb"
  if ! brew cat "${formula}" > "${src}" 2>/dev/null; then
    echo "note: ${formula}: no formula source -- skipped." >&2
    continue
  fi

  # One line per external patch: URL, expected sha256, the path brew looks for it at. The path
  # is brew's own answer, so this script never has to know the cache's naming scheme.
  if ! brew ruby -e "
      f = Formulary.factory('${src}')
      f.stable.patches.select(&:external?).each do |p|
        puts [p.url, p.resource.checksum.hexdigest, p.cached_download].join(\"\t\")
      end" > "${work}/${formula}.patches"; then
    echo "note: ${formula}: could not list its patches -- skipped." >&2
    continue
  fi

  python3 - "${formula}" "${work}/${formula}.patches" <<'EOF'
import hashlib, os, re, sys, urllib.request

formula, listing = sys.argv[1], sys.argv[2]
INDEX = re.compile(rb'^index ([0-9a-f]+)\.\.([0-9a-f]+)', re.M)

for line in open(listing, encoding='utf-8'):
    parts = line.rstrip('\n').split('\t')
    if len(parts) != 3:
        continue
    url, expected, cached = parts
    name = url.rsplit('/', 1)[-1]
    if os.path.exists(cached):
        print(f'{formula}: {name}: already in the cache')
        continue
    try:
        with urllib.request.urlopen(url, timeout=60) as r:
            body = r.read()
    except Exception as e:
        print(f'note: {formula}: {name}: download failed ({e}) -- left to brew', file=sys.stderr)
        continue
    if hashlib.sha256(body).hexdigest() == expected:
        print(f'{formula}: {name}: matches as served -- nothing to do')
        continue
    longest = max((len(m.group(1)) for m in INDEX.finditer(body)), default=0)
    for n in range(longest - 1, 6, -1):
        candidate = INDEX.sub(lambda m: b'index ' + m.group(1)[:n] + b'..' + m.group(2)[:n], body)
        if hashlib.sha256(candidate).hexdigest() == expected:
            os.makedirs(os.path.dirname(cached), exist_ok=True)
            with open(cached, 'wb') as out:
                out.write(candidate)
            print(f'{formula}: {name}: served with {longest}-character index hashes; '
                  f'the {n}-character form matches the formula -- seeded at {cached}')
            break
    else:
        print(f'note: {formula}: {name}: checksum differs and no shorter index abbreviation '
              f'reproduces it -- the content changed; left to brew', file=sys.stderr)
EOF
done

exit 0
