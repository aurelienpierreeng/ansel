#!/usr/bin/env bash
#
# Install Homebrew formulae that build with Rust, against the Rust project's own prebuilt
# toolchain instead of Homebrew's `rust' formula -- for Intel macOS, where that formula is a
# source build of rust and of the llvm it depends on.
#
# Homebrew publishes no bottles for Intel macOS any more, so every formula is built there, and
# librsvg is written in Rust: `depends_on "rust" => :build', and `cargo-c' likewise. rust depends
# on llvm (since rust 1.99.0, homebrew-core 2026-10-01), and llvm on z3. Measured on
# macos-15-intel: llvm 4 h 11 on 2026-11-17 and over 4 h 12 unfinished on 11-18, after 1 h 02 and
# 1 h 40 of its own dependencies; rust 1 h 41 on 11-17. A rust or llvm bump therefore cost a run
# of six hours or more, and the keg cache that used to hold them could not always warm in one.
#
# The Rust project publishes the same compiler prebuilt for x86_64-apple-darwin, version for
# version. This script downloads that release -- the one homebrew-core's rust formula is at --
# into $(brew --prefix)/var/ansel, puts each formula's homebrew-core source into a local tap with
# its `depends_on "rust"' removed and that toolchain prepended to its PATH, and installs it from
# there. Homebrew then builds it exactly as it would have, with cargo coming from elsewhere.
#
# Why that removes rust and llvm from every other formula's plan too, read in Homebrew's
# FormulaInstaller#expand_dependencies_for_formula_once on 2026-11-19: a build dependency is
# pruned, with everything below it, once the formula needing it is installed at its current
# version. So adwaita-icon-theme, which needs librsvg, no longer schedules rust once librsvg is
# installed at the version homebrew-core wants. A rust keg merely present would not do it: an
# installed dependency is skipped but its own dependencies are still expanded, and rust's
# include llvm.
#
# Formulae must be given in dependency order: `cargo-c librsvg', since librsvg builds with
# cargo-c, which then resolves to the copy installed here.
#
# The tap stays installed: the kegs record it as their origin, and a later `brew install' of a
# formula needing them reads it back. One consequence, from Homebrew's install/check.rb: an
# explicit `brew install librsvg' now refuses ("Formulae with the same name from different taps
# cannot be installed at the same time"), so install-deps-macos.sh does not name it on Intel.
#
# Usage:
#   tools/brew_install_without_rust.sh cargo-c librsvg

set -euo pipefail

if ! command -v brew >/dev/null 2>&1; then
  echo "note: brew not found -- this runs on a Homebrew host only. Nothing was installed." >&2
  exit 2
fi

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <formula>..." >&2
  exit 2
fi

case "$(uname -m)" in
  x86_64) triple="x86_64-apple-darwin" ;;
  arm64)  triple="aarch64-apple-darwin" ;;
  *) echo "unsupported architecture: $(uname -m)" >&2; exit 2 ;;
esac

prefix="$(brew --prefix)"
cellar="$(brew --cellar)"

# What homebrew-core would install: versions.stable, plus _revision when there is one -- the
# name of the keg directory. Read from `brew info', so it is whatever the last `brew update' saw.
pkg_version() {
  brew info --json=v2 --formula "$1" | python3 -c '
import json, sys
f = json.load(sys.stdin)["formulae"][0]
r = f.get("revision") or 0
print(f["versions"]["stable"] + (f"_{r}" if r else ""))'
}

# The formulae not yet installed at that version; nothing to do, and nothing downloaded, when
# none is left.
todo=()
for formula in "$@"; do
  wanted="$(pkg_version "${formula}")"
  keg="${cellar}/${formula}/${wanted}"
  if [ -d "${keg}" ] && [ -n "$(ls -A "${keg}")" ]; then
    echo "${formula} ${wanted}: already installed"
  else
    todo+=("${formula}")
  fi
done
if [ "${#todo[@]}" -eq 0 ]; then
  exit 0
fi

# --- The toolchain: rustc, cargo and the standard library, as the Rust project ships them. ----
#
# Under the Homebrew prefix and not under $HOME: Homebrew's build sandbox denies reading the home
# directory (Sandbox#deny_read_home), and both a formula's fetch and its install run inside it.
rust_version="$(brew info --json=v2 --formula rust | python3 -c '
import json, sys
print(json.load(sys.stdin)["formulae"][0]["versions"]["stable"])')"
toolchains="${prefix}/var/ansel"
toolchain="${toolchains}/rust-${rust_version}"

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT

if "${toolchain}/bin/cargo" --version >/dev/null 2>&1 \
   && "${toolchain}/bin/rustc" --version >/dev/null 2>&1; then
  echo "Rust ${rust_version}: prebuilt toolchain already in ${toolchain}"
else
  echo "Rust ${rust_version}: installing the prebuilt toolchain for ${triple} into ${toolchain}"
  rm -rf "${toolchain}"
  for component in rustc cargo rust-std; do
    name="${component}-${rust_version}-${triple}"
    url="https://static.rust-lang.org/dist/${name}.tar.xz"
    curl -fsSL --retry 3 -o "${work}/${name}.tar.xz" "${url}"
    curl -fsSL --retry 3 -o "${work}/${name}.tar.xz.sha256" "${url}.sha256"
    (cd "${work}" && shasum -a 256 -c "${name}.tar.xz.sha256")
    tar -xJf "${work}/${name}.tar.xz" -C "${work}"
    "${work}/${name}/install.sh" --prefix="${toolchain}" --disable-ldconfig >/dev/null
  done
  "${toolchain}/bin/rustc" --version
  "${toolchain}/bin/cargo" --version
  # One toolchain is enough: the previous release's is only the size of a stale download.
  find "${toolchains}" -mindepth 1 -maxdepth 1 -name 'rust-*' ! -name "rust-${rust_version}" \
       -exec rm -rf {} +
fi

# --- The tap, holding homebrew-core's own sources minus their rust dependency. ----------------
#
# A formula can only be installed from a tap ("Homebrew requires formulae to be in a tap"), and a
# local one is refused as untrusted until `brew trust' is run on it -- both measured on the mac
# CI of 2026-10-02 with the same mechanism, in the since-removed tools/brew_seed_patches.sh.
tap="ansel/prebuilt-rust"
if [ ! -d "$(brew --repository "${tap}")" ]; then
  brew tap-new --no-git "${tap}" >/dev/null
fi
tap_formulae="$(brew --repository "${tap}")/Formula"
mkdir -p "${tap_formulae}"
brew trust "${tap}" </dev/null >/dev/null 2>&1 || true

for formula in "${todo[@]}"; do
  src="${work}/${formula}.rb"
  brew cat --formula "${formula}" > "${src}"

  # Strict on purpose: a source no longer shaped as expected stops here, with its name, rather
  # than falling back to a build that schedules rust and llvm and runs into the job's ceiling.
  python3 - "${formula}" "${src}" "${tap_formulae}/${formula}.rb" "${toolchain}/bin" <<'EOF'
import re, sys

formula, src, dst, bindir = sys.argv[1:5]
lines = open(src, encoding="utf-8").read().splitlines(keepends=True)

rust = re.compile(r'^\s*depends_on "rust" => (:build|\[:build, :test\])\s*$')
if sum(1 for l in lines if rust.match(l)) != 1:
    sys.exit(f'{formula}: expected exactly one `depends_on "rust" => :build` in its source -- '
             'homebrew-core changed it; nothing was installed')
lines = [l for l in lines if not rust.match(l)]

# The bottle block names homebrew-core's bottles, which this tap does not serve.
out, skipping = [], False
for l in lines:
    if not skipping and re.match(r'^  bottle do\s*$', l):
        skipping = True
        continue
    if skipping:
        skipping = not re.match(r'^  end\s*$', l)
        continue
    out.append(l)
lines = out

# The toolchain first in PATH for both phases: `fetch' runs `cargo fetch' with the network, then
# `install' builds offline, each in a sandbox whose PATH is rebuilt from the dependencies alone.
path_line = f'    ENV.prepend_path "PATH", "{bindir}"\n'
out, methods = [], set()
for l in lines:
    out.append(l)
    m = re.match(r'^  def (fetch|install)\s*$', l)
    if m:
        out.append(path_line)
        methods.add(m.group(1))
if "install" not in methods:
    sys.exit(f'{formula}: no `def install` in its source; nothing was installed')

header = (f'# homebrew-core\'s {formula}, built against the prebuilt Rust toolchain in {bindir}\n'
          '# instead of its `rust` dependency. Written by tools/brew_install_without_rust.sh.\n')
open(dst, "w", encoding="utf-8").write(header + "".join(out))
print(f'{formula}: tap formula written, without rust, PATH set in {sorted(methods)}')
EOF

  # A keg at another version -- an older one, or homebrew-core's own -- is removed first, since
  # Homebrew refuses two formulae of one name from different taps. Its dependents keep their
  # links to the opt path, which the new keg takes over.
  if [ -d "${cellar}/${formula}" ]; then
    brew uninstall --ignore-dependencies --formula "${formula}"
  fi

  # The plan is read before anything is built: rust or llvm in it would be a six-hour build,
  # the very thing this script exists to avoid, so it stops here and names them.
  # A name is matched between whitespace, not at line start: the Homebrew of 2026-11-19 prints
  # one formula per line, name first, but older ones listed a plan's names on one line.
  plan="$(brew install --dry-run --formula "${tap}/${formula}" 2>&1)"
  echo "${plan}"
  if grep -qE '(^|[[:space:]])(rust|llvm|llvm@[0-9]+)([[:space:]]|$)' <<< "${plan}"; then
    echo "${formula}: Homebrew still plans rust or llvm (above) -- refusing to build it." >&2
    exit 1
  fi

  # brew's status is not the verdict, the Cellar is, as in install-deps-macos.sh: a failing
  # post-install hook exits non-zero over a formula that is installed.
  status=0
  brew install --formula "${tap}/${formula}" || status=$?
  wanted="$(pkg_version "${formula}")"
  if [ ! -d "${cellar}/${formula}/${wanted}" ]; then
    echo "${formula} ${wanted}: missing after brew install" >&2
    exit "$(( status ? status : 1 ))"
  fi
  if [ "${status}" -ne 0 ]; then
    echo "brew install exited ${status}, but ${formula} ${wanted} is installed." >&2
  fi
  echo "${formula} ${wanted}: installed from ${tap}"
done
