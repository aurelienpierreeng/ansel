#!/usr/bin/env bash
#
# Resolve the Homebrew versions the mac nightly's keg cache is keyed on.
#
# Homebrew publishes no bottles for Intel macOS any more -- it prints "You are using macOS on
# Intel x86_64" and builds from source -- and two formulae dominate the dependency step there.
# Measured on the nightlies of 2026-09-10 to 09-14, against a 6-hour job limit: llvm 3h43 to
# 4h12, rust 1h05 to 1h29 (pulled in by librsvg, which is written in Rust), everything else
# about 50 minutes. Four nightlies in a row were killed at the wall with nothing published.
# Those kegs are cached between runs instead, and this script produces the keys so that the
# nightly and mac-brew-cache.yml cannot drift apart on what "the same cache" means.
#
# A keg is cacheable without relocation because a given architecture's prefix is identical on
# every runner (/usr/local on Intel, /opt/homebrew on arm64), so a restored keg sits at the
# very paths it was built for. brew decides a formula is installed by reading
# INSTALL_RECEIPT.json inside the keg, and that file travels with it.
#
# The key carries the version brew WOULD install, never the one installed: keying on the
# latter restores a keg that a formula bump has already made useless, which wastes the cache
# quota and hides the bump. `brew info --json=v2` reports both, so only versions.stable and
# revision are read -- the installed list changes between runs and would make the key unstable.
#
# Run `brew update` first, or the versions are whatever the last update saw.
#
# Writes `<formula>_version=` and `<formula>_key=` to $GITHUB_OUTPUT when that is set, and to
# stdout otherwise. A versioned formula's "@" is spelled "_" in the name, since a step output
# cannot carry it: llvm@22 -> llvm_22_key.
#
# Usage:
#   tools/brew_cache_key.sh llvm llvm@22 rust

set -uo pipefail

if ! command -v brew >/dev/null 2>&1; then
  echo "note: brew not found -- this runs on a Homebrew host only. Nothing was resolved." >&2
  exit 2
fi

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <formula>..." >&2
  exit 2
fi

# Cache identity beyond the formula itself: a keg built for one architecture is useless on the
# other, and a macOS major bump changes what brew builds against.
arch="$(uname -m)"
osmajor="$(sw_vers -productVersion 2>/dev/null | cut -d. -f1)"

out="${GITHUB_OUTPUT:-/dev/stdout}"

for formula in "$@"; do
  # versions.stable plus revision is what brew would install. "none" for a formula this
  # Homebrew does not know, which still makes a stable key -- it simply never hits.
  version="$(brew info --json=v2 --formula "${formula}" 2>/dev/null | python3 -c '
import json, sys
try:
    f = json.load(sys.stdin)["formulae"]
except Exception:
    f = []
if not f:
    print("none")
else:
    v = f[0]["versions"]["stable"] or "none"
    r = f[0].get("revision") or 0
    print(f"{v}_{r}")
')"
  [ -n "${version}" ] || version="none"

  # An output name takes no "@".
  name="${formula//@/_}"
  printf '%s_version=%s\n' "${name}" "${version}" >> "${out}"
  printf '%s_key=brew-keg-%s-%s-%s-%s\n' "${name}" "${arch}" "${osmajor}" "${formula//@/_}" "${version}" >> "${out}"
done
