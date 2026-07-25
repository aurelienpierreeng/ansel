#!/usr/bin/env bash
#
# Run ansel-cli over a local bank of raw files as a crash/regression smoke test.
#
# The image test bank itself is NOT part of this repository -- point the script at any
# local directory of camera raw files (optionally with sidecar `<raw>.xmp` files
# next to them, same convention as darkroom/ansel-cli) via --bank, $ANSEL_IMAGE_TEST,
# or the `configure` subcommand below.
#
# Usage:
#   tests/image_test.sh [run] [options]
#   tests/image_test.sh configure <bank-dir>
#   tests/image_test.sh update-baseline [options]
#
# Commands:
#   run              Export every raw in the bank through ansel-cli (default).
#   configure <dir>  Remember <dir> as the local image test bank (saved, gitignored,
#                     to tests/image_test/bank_path.conf).
#   update-baseline  Add missing baseline entries from the current run (always exported
#                     at a fixed 1024x1024, --width/--height ignored); never overwrites
#                     an existing entry -- delete it first to force a refresh. Shared
#                     team bank -> baseline lives in the samples submodule (reviewed/
#                     committed there); personal bank -> local
#                     tests/image_test/baseline/ (gitignored).
#
# Options:
#   --bank <dir>          Image test bank directory (default: $ANSEL_IMAGE_TEST, or
#                          path saved by `configure`)
#   --cli <path>           ansel-cli binary under test (default: $ANSEL_CLI, or
#                           auto-detected under build*/install*)
#   --baseline <dir>       Baseline dir for delta-E diff (default: alongside the bank,
#                          see `update-baseline` above)
#   --jobs <n>              Parallel exports (default: nproc / --threads)
#   --threads <n>           OpenMP threads per export (default: 2)
#   --timeout <sec>         Per-image timeout (default: 180)
#   --width <px>            Export width cap (default: 1024)
#   --height <px>           Export height cap (default: 1024)
#   --limit <n>             Only test the first N raws found (0 = all, default: 0)
#   --strict                Shorthand for --strict-cpu and --strict-opencl together
#   --strict-cpu            Zero-tolerance mode: fail on any delta-E vs baseline at all, even
#                            invisible to the eye (default: only dE > 2.3, a change a human
#                            would actually notice, fails; smaller drift is only reported)
#   --opencl                Also render with OpenCL and report the CPU-vs-GPU delta-E.
#                            Never fails just because OpenCL is unavailable, but fails
#                            if more than 5% of pixels exceed the delta-E tolerance (a
#                            real, widespread CPU/GPU divergence, not an edge-pixel blip)
#   --strict-opencl         With --opencl, tighten that 5% share to 0%: any single pixel
#                            over tolerance fails
#   --keep                  Keep this run's outputs/logs even on full success
#   --if-configured         Exit 0 silently instead of erroring when no bank is
#                            configured (used by the pre-commit hook)
#   -q, --quiet             Only print the summary and failures
#   -h, --help              Show this help
#
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STATE_DIR="$SCRIPT_DIR/image_test"
BANK_PATH_FILE="$STATE_DIR/bank_path.conf"
RESULTS_DIR="$STATE_DIR/results"

RAW_EXTENSIONS=(3fr ari arw bay cr2 cr3 crw dc2 dcr dng erf fff ia iiq k25 kc2
                kdc mdc mef mos mrw nef nrw orf pef raf raw rw2 rwl sr2 srf
                srw sti x3f)
SUBMODULE_BANK_DIR="$STATE_DIR/samples"
DELTAE_SCRIPT="$STATE_DIR/deltae"
# Fixed export size for the shared baseline (tests/image_test/samples/baseline/): everyone
# comparing against it must use the same dimensions, or delta-E sees a size mismatch that
# has nothing to do with an actual regression. --width/--height are ignored for
# `update-baseline` on purpose.
BASELINE_WIDTH=1024
BASELINE_HEIGHT=1024
# A high max dE with only a few outlier pixels (edge/border effect) is not worth failing
# over; a widespread one is. Fail unconditionally (not just under --strict-opencl) once more
# than this share of pixels exceeds the 2.3 delta-E tolerance, regardless of max/avg dE.
# --strict-opencl tightens this share to 0%.
MAX_PCT_ABOVE_TOLERANCE=5

find_raws() {
  local dir="$1"
  local -a expr=()
  local ext
  for ext in "${RAW_EXTENSIONS[@]}"; do
    expr+=(-iname "*.$ext" -o)
  done
  unset 'expr[${#expr[@]}-1]'
  find "$dir" -type f \( "${expr[@]}" \) -print0
}

dir_has_raws() {
  [ -d "$1" ] || return 1
  local first=""
  IFS= read -r -d '' first < <(find_raws "$1" 2>/dev/null)
  [ -n "$first" ]
}

BANK_DIR="${ANSEL_IMAGE_TEST:-}"
CLI_BIN="${ANSEL_CLI:-}"
BASELINE_DIR="$STATE_DIR/baseline"
BASELINE_DIR_SET=no
FILTER_RAWS=()
JOBS=""
THREADS=2
TIMEOUT=180
WIDTH=1024
HEIGHT=1024
LIMIT=0
STRICT_CPU=no
STRICT_OPENCL=no
OPENCL=no
KEEP=no
QUIET=no
IF_CONFIGURED=no
COMMAND=run

log() { [ "$QUIET" = yes ] || echo "$*"; }
err() { echo "$*" >&2; }

usage() { sed -n '2,54p' "$0" | sed 's/^#$//; s/^# //'; }

if [ -t 1 ]; then
  C_GREEN=$'\033[32m'
  C_ORANGE=$'\033[33m' # ANSI has no orange; yellow is the closest portable approximation
  C_RED=$'\033[31m'
  C_RESET=$'\033[0m'
else
  C_GREEN=""; C_ORANGE=""; C_RED=""; C_RESET=""
fi

colorize_verdict() {
  local line="$1"
  local tag="${line%% *}"
  local rest="${line#* }"
  local color=""
  case "$tag" in
    PASS|DONE) color="$C_GREEN" ;;
    TIMEOUT) color="$C_ORANGE" ;;
    CRASH*|EMPTY|DIFF) color="$C_RED" ;;
  esac
  [ -z "$color" ] && { printf '%s' "$line"; return; }
  printf '%s%s%s %s' "$color" "$tag" "$C_RESET" "$rest"
}

# --- argument parsing --------------------------------------------------

if [ $# -gt 0 ] && [[ "$1" != -* ]]; then
  COMMAND="$1"; shift
  if [ "$COMMAND" = configure ] && [ $# -gt 0 ] && [[ "$1" != -* ]]; then
    BANK_DIR="$1"; shift
  fi
fi

while [ $# -gt 0 ]; do
  case "$1" in
    --bank) BANK_DIR="$2"; shift 2 ;;
    --cli) CLI_BIN="$2"; shift 2 ;;
    --baseline) BASELINE_DIR="$2"; BASELINE_DIR_SET=yes; shift 2 ;;
    --jobs) JOBS="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --width) WIDTH="$2"; shift 2 ;;
    --height) HEIGHT="$2"; shift 2 ;;
    --limit) LIMIT="$2"; shift 2 ;;
    --strict) STRICT_CPU=yes; STRICT_OPENCL=yes; shift ;;
    --strict-cpu) STRICT_CPU=yes; shift ;;
    --strict-opencl) STRICT_OPENCL=yes; shift ;;
    --opencl) OPENCL=yes; shift ;;
    --keep) KEEP=yes; shift ;;
    --if-configured) IF_CONFIGURED=yes; shift ;;
    -q|--quiet) QUIET=yes; shift ;;
    -h|--help) usage; exit 0 ;;
    *) err "unknown argument: $1"; usage; exit 2 ;;
  esac
done

case "$COMMAND" in
  run|configure|update-baseline) ;;
  *) err "unknown command: $COMMAND"; usage; exit 2 ;;
esac

# --- bank path resolution -----------------------------------------------

if [ -z "$BANK_DIR" ] && [ -f "$BANK_PATH_FILE" ]; then
  BANK_DIR="$(cat "$BANK_PATH_FILE")"
fi

# Fall back to the shared team bank (git submodule at tests/image_test/samples,
# see tests/image_test/README.md) if nothing more specific was given and it has
# actually been checked out (submodules aren't populated by a plain git clone).
if [ -z "$BANK_DIR" ] && dir_has_raws "$SUBMODULE_BANK_DIR"; then
  BANK_DIR="$SUBMODULE_BANK_DIR"
fi

if [ "$COMMAND" = configure ]; then
  [ -z "$BANK_DIR" ] && { err "usage: $0 configure <image-test-dir>"; exit 2; }
  [ -d "$BANK_DIR" ] || { err "not a directory: $BANK_DIR"; exit 2; }
  BANK_DIR="$(cd "$BANK_DIR" && pwd)"
  mkdir -p "$STATE_DIR"
  printf '%s\n' "$BANK_DIR" > "$BANK_PATH_FILE"
  log "Image test bank configured: $BANK_DIR"
  log "(saved to $BANK_PATH_FILE, not tracked by git)"
  exit 0
fi

if [ -z "$BANK_DIR" ]; then
  [ "$IF_CONFIGURED" = yes ] && exit 0
  err "No image test bank configured."
  if git -C "$REPO_ROOT" config -f .gitmodules --get-regexp '\.path$' 2>/dev/null \
       | grep -q "tests/image_test/samples\$"; then
    err "The shared team bank is registered but not checked out yet:"
    err "  git submodule update --init --checkout tests/image_test/samples"
  else
    err "Point one with --bank <dir>, \$ANSEL_IMAGE_TEST, or: $0 configure <dir>"
  fi
  exit 1
fi
[ -d "$BANK_DIR" ] || { err "image test bank directory not found: $BANK_DIR"; exit 1; }
BANK_DIR="$(cd "$BANK_DIR" && pwd)"

# Default baseline location follows where the bank came from: the shared, reviewed
# baseline lives inside the samples submodule; a personal/local bank gets its own
# local (gitignored) baseline instead. An explicit --baseline always wins.
if [ "$BASELINE_DIR_SET" = no ]; then
  if [ "$BANK_DIR" = "$SUBMODULE_BANK_DIR" ]; then
    BASELINE_DIR="$SUBMODULE_BANK_DIR/baseline"
  else
    BASELINE_DIR="$STATE_DIR/baseline"
  fi
fi

# --- ansel-cli discovery --------------------------------------------------

find_cli() {
  if [ -n "$CLI_BIN" ]; then
    [ -x "$CLI_BIN" ] || { err "not executable: $CLI_BIN"; return 1; }
    printf '%s\n' "$CLI_BIN"
    return 0
  fi
  # build.sh --install always (re)points /usr/local/bin/ansel-cli at whatever
  # $INSTALL_PREFIX/bin/ansel-cli it just installed, whatever --prefix was used --
  # so PATH reliably resolves to the most recently installed build. Trust that
  # over guessing a repo-relative layout below.
  local from_path
  from_path="$(command -v ansel-cli 2>/dev/null)" && { printf '%s\n' "$from_path"; return 0; }
  # Fallback for a build that was never `--install`ed system-wide (e.g. a
  # relocatable "cmake --install <builddir> --prefix <builddir>" done by hand):
  # ansel-cli resolves its plugin moduledir relative to its own binary path,
  # which only lines up correctly once install has populated <builddir>/bin +
  # <builddir>/lib together. The raw ninja output in build*/src/cli/ansel-cli
  # looks for its bundled plugins (views/libs/imageio backends) in the wrong
  # place and silently fails to init (e.g. "can't init develop system") --
  # kept below only as a last resort.
  local candidates=(
    "$REPO_ROOT/build/bin/ansel-cli"
    "$REPO_ROOT/install/bin/ansel-cli"
    "$REPO_ROOT/build_Release/bin/ansel-cli"
    "$REPO_ROOT/build_Debug/bin/ansel-cli"
    "$REPO_ROOT/build_release/bin/ansel-cli"
    "$REPO_ROOT/build_debug/bin/ansel-cli"
    "$REPO_ROOT/build_ASAN/bin/ansel-cli"
    "$REPO_ROOT/install_ASAN/bin/ansel-cli"
    "$REPO_ROOT/build/src/cli/ansel-cli"
    "$REPO_ROOT/build_Release/src/cli/ansel-cli"
    "$REPO_ROOT/build_Debug/src/cli/ansel-cli"
    "$REPO_ROOT/build_release/src/cli/ansel-cli"
    "$REPO_ROOT/build_debug/src/cli/ansel-cli"
    "$REPO_ROOT/build_ASAN/src/cli/ansel-cli"
  )
  local c
  for c in "${candidates[@]}"; do
    [ -x "$c" ] && { printf '%s\n' "$c"; return 0; }
  done
  return 1
}

lib_dir_for() {
  local bin="$1" bindir up1 up2
  bindir="$(cd "$(dirname "$bin")" 2>/dev/null && pwd)" || return 1
  # Windows CMake install layout: everything flat in <prefix>/bin/ -- the DLL sits
  # right beside the .exe itself, no separate lib/ subfolder at all. This is also
  # why running the installed ansel-cli.exe directly (no PATH tweak) "just works":
  # Windows' DLL search always checks the exe's own directory first, regardless of
  # PATH. It stops being automatic the moment CLI_BIN is a copy/symlink living
  # somewhere else (e.g. /usr/local/bin/ansel-cli), which is exactly when this
  # function's result actually matters.
  if [ -f "$bindir/libansel.dll" ] || [ -f "$bindir/libansel.so" ]; then printf '%s\n' "$bindir"; return 0; fi
  up1="$(cd "$bindir/.." 2>/dev/null && pwd)" || return 1
  up2="$(cd "$bindir/../.." 2>/dev/null && pwd)" || return 1
  # Installed layout: <prefix>/bin/ansel-cli + <prefix>/lib/libansel.{so,dll}
  if [ -f "$up2/lib/libansel.so" ] || [ -f "$up2/lib/libansel.dll" ]; then printf '%s\n' "$up2/lib"; return 0; fi
  if [ -f "$up1/lib/libansel.so" ] || [ -f "$up1/lib/libansel.dll" ]; then printf '%s\n' "$up1/lib"; return 0; fi
  # Raw ninja build layout on Windows: <builddir>/src/cli/ansel-cli.exe next to
  # <builddir>/src/libansel.dll (sibling directory, no lib/ subfolder ever created).
  if [ -f "$up1/libansel.dll" ]; then printf '%s\n' "$up1"; return 0; fi
  return 1
}

toolchain_bin_dir_for() {
  # Windows-only: the compiler's own toolchain bin dir carries the runtime DLLs
  # (libc++.dll, libunwind.dll, ucrtbase api-ms-win-crt-* forwarders...) ansel-cli
  # was linked against, AND a real python3 (the Windows Store "python3" shim that
  # otherwise wins on PATH prints an install nag and exits, breaking the deltae
  # comparison script). A git hook can run from a different MSYS2 install/flavor's
  # shell than the one that actually built the CLI (e.g. Git for Windows' bundled,
  # unrelated /mingw64 vs. a standalone C:\msys64\ucrt64 build) -- guessing a
  # flavor dir name off the current shell's own PATH doesn't reliably land on the
  # right install, so read the compiler path CMake recorded for this exact build.
  local bin="$1" d cache compiler compiler_dir
  d="$(dirname "$bin")"
  while [ "$d" != "/" ] && [ -n "$d" ]; do
    cache="$d/CMakeCache.txt"
    if [ -f "$cache" ]; then
      compiler="$(sed -n 's/^CMAKE_C_COMPILER:FILEPATH=//p' "$cache")"
      [ -z "$compiler" ] && return 1
      compiler_dir="$(dirname "$compiler")"
      command -v cygpath >/dev/null 2>&1 && compiler_dir="$(cygpath -u "$compiler_dir" 2>/dev/null || printf '%s\n' "$compiler_dir")"
      printf '%s\n' "$compiler_dir"
      return 0
    fi
    d="$(dirname "$d")"
  done
  return 1
}

installed_cli_for() {
  # The raw ninja build output (build*/src/cli/ansel-cli) resolves its plugin
  # moduledir relative to its own binary path, which only lines up once a proper
  # "cmake --install" has populated <prefix>/bin + <prefix>/lib together (see the
  # comment on find_cli()'s candidates below) -- otherwise ansel-cli starts (DLLs
  # resolve fine) but fails with "can't init develop system, aborting" because it
  # can't find its views/libs/imageio plugins. If this build was ever installed
  # (CMAKE_INSTALL_PREFIX recorded in its CMakeCache.txt has a populated bin/),
  # prefer that binary over the raw, not-installed one found by find_cli().
  local bin="$1" d cache prefix prefix_dir candidate
  d="$(dirname "$bin")"
  while [ "$d" != "/" ] && [ -n "$d" ]; do
    cache="$d/CMakeCache.txt"
    if [ -f "$cache" ]; then
      prefix="$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$cache")"
      [ -z "$prefix" ] && return 1
      prefix_dir="$prefix"
      command -v cygpath >/dev/null 2>&1 && prefix_dir="$(cygpath -u "$prefix" 2>/dev/null || printf '%s\n' "$prefix")"
      for candidate in "$prefix_dir/bin/ansel-cli" "$prefix_dir/bin/ansel-cli.exe"; do
        [ -x "$candidate" ] && { printf '%s\n' "$candidate"; return 0; }
      done
      return 1
    fi
    d="$(dirname "$d")"
  done
  return 1
}

CLI_WAS_EXPLICIT=no
[ -n "$CLI_BIN" ] && CLI_WAS_EXPLICIT=yes
CLI_BIN="$(find_cli)" || { err "ansel-cli not found; build it or pass --cli <path>"; exit 1; }
# toolchain_bin_dir_for() below needs the raw build tree (it walks up looking for
# CMakeCache.txt) -- capture it before installed_cli_for() potentially swaps
# CLI_BIN to an installed binary that lives outside any build tree entirely.
RAW_CLI_BIN="$CLI_BIN"
# Never second-guess an explicit --cli/$ANSEL_CLI: only upgrade a CLI we found
# ourselves via the repo-relative fallback candidates above.
if [ "$CLI_WAS_EXPLICIT" = no ] && INSTALLED_CLI="$(installed_cli_for "$CLI_BIN")" && [ -x "$INSTALLED_CLI" ]; then
  CLI_BIN="$INSTALLED_CLI"
fi
LIBDIR="$(lib_dir_for "$CLI_BIN")"
if [ -z "$LIBDIR" ] && [ "$CLI_WAS_EXPLICIT" = no ]; then
  # command -v ansel-cli (find_cli()'s first, most-trusted source) assumes
  # /usr/local/bin/ansel-cli is a real symlink into the install prefix, as
  # `build.sh --install`'s `ln -sfn` intends -- true on Linux/macOS, but creating
  # a symlink on Windows/NTFS without elevated privileges silently degrades to a
  # standalone copy instead (seen in practice: a lone .exe in /usr/local/bin with
  # no adjacent DLLs at all, Links: 1, not a reparse point). lib_dir_for() then
  # has nothing nearby to find. Fall back to this project's own well-known
  # default install prefix (build.sh's INSTALL_PREFIX_DEFAULT) as a last resort.
  for DEFAULT_CLI in /opt/ansel/bin/ansel-cli /opt/ansel/bin/ansel-cli.exe; do
    if [ -x "$DEFAULT_CLI" ] && DEFAULT_LIBDIR="$(lib_dir_for "$DEFAULT_CLI")"; then
      CLI_BIN="$DEFAULT_CLI"
      LIBDIR="$DEFAULT_LIBDIR"
      break
    fi
  done
fi
if [ -n "$LIBDIR" ]; then
  export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  # The Windows PE loader resolves DLLs via PATH, not LD_LIBRARY_PATH.
  export PATH="$LIBDIR:$PATH"
fi
if TOOLCHAIN_BIN="$(toolchain_bin_dir_for "$RAW_CLI_BIN")" && [ -d "$TOOLCHAIN_BIN" ]; then
  export PATH="$TOOLCHAIN_BIN:$PATH"
fi

# --- per-image export + verdict -------------------------------------------

process_one() {
  local raw="$1"
  local rel="${raw#"$BANK_DIR"/}"
  local xmp=""
  [ -f "$raw.xmp" ] && xmp="$raw.xmp"

  local out="$RESULTS_DIR/$rel.png"
  local runlog="$RESULTS_DIR/$rel.log"
  mkdir -p "$(dirname "$out")"

  local cfg cache
  cfg="$(mktemp -d)"
  cache="$(mktemp -d)"

  local -a args=(--width "$WIDTH" --height "$HEIGHT" --apply-custom-presets false "$raw")
  [ -n "$xmp" ] && args+=("$xmp")
  args+=("$out" --core --disable-opencl --configdir "$cfg" --cachedir "$cache"
         --conf host_memory_limit=8192 --conf worker_threads="$THREADS" -t "$THREADS")

  local status
  timeout --kill-after=10 "$TIMEOUT" "$CLI_BIN" "${args[@]}" >"$runlog" 2>&1
  status=$?

  rm -rf "$cfg" "$cache"

  local verdict="PASS"
  [ "$COMMAND" = update-baseline ] && verdict="DONE"
  if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
    verdict="TIMEOUT"
  elif [ "$status" -ne 0 ]; then
    verdict="CRASH($status)"
  elif [ ! -s "$out" ]; then
    verdict="EMPTY"
  fi

  # Extracts Max dE, Avg dE and the "% pixels above tolerance" line from a deltae
  # report: a high max but ~0% above tolerance means a handful of outlier pixels
  # (typically an edge/border effect), not a real widespread visual change.
  extract_de_stats() {
    local report="$1"
    local max avg pct
    max="$(printf '%s\n' "$report" | sed -n 's/.*Max dE *: *\([0-9.]*\).*/\1/p')"
    avg="$(printf '%s\n' "$report" | sed -n 's/.*Avg dE *: *\([0-9.]*\).*/\1/p')"
    pct="$(printf '%s\n' "$report" | sed -n 's/.*Pixels above tolerance *: *\([0-9.]*\).*/\1/p')"
    printf '%s\t%s\t%s' "$max" "$avg" "$pct"
  }

  local diffnote=""
  if [[ "$verdict" = PASS || "$verdict" = DONE ]] && [ -d "$BASELINE_DIR" ]; then
    local base="$BASELINE_DIR/$rel.png"
    if [ -f "$base" ] && [ -x "$DELTAE_SCRIPT" ]; then
      local deltae_out deltae_rc max_de avg_de pct_de
      deltae_out="$("$DELTAE_SCRIPT" "$base" "$out" 2>&1)"
      deltae_rc=$?
      printf '%s\n' "$deltae_out" > "$RESULTS_DIR/$rel.deltae.log"
      IFS=$'\t' read -r max_de avg_de pct_de <<< "$(extract_de_stats "$deltae_out")"
      if [ -z "$max_de" ]; then
        # deltae crashed (e.g. size mismatch vs baseline) rather than computing a
        # real dE -- never a silent PASS, --strict or not.
        diffnote=" (delta-E comparison failed vs baseline, likely a size mismatch)"
        verdict="DIFF"
      else
        # avg dE > 0.767 (2.3/3) is a population-wide problem -- never filtered
        # by pixel count. max dE > 2.3 on its own can be a single dark-shadow
        # pixel where an isolated ±1/255 rounding step (platform/compiler FP
        # noise) produces an outsized CIEDE2000 value purely because Lab's L*
        # compresses luminance non-linearly (the same 1-bit rounding step reads
        # as a much bigger dE near black than at mid/highlight tones) -- so it
        # only counts as a real, visible regression once it affects more than
        # MAX_PCT_ABOVE_TOLERANCE% of pixels, mirroring the opencl-vs-CPU
        # comparison's own tolerance below. Missing/unparseable pct_de fails
        # safe (treated as widespread) rather than silently passing.
        local avg_bad=no max_bad_raw=no max_bad_widespread=no
        awk -v v="$avg_de" 'BEGIN{exit !(v > 2.3/3)}' && avg_bad=yes
        awk -v v="$max_de" 'BEGIN{exit !(v > 2.3)}' && max_bad_raw=yes
        if [ "$max_bad_raw" = yes ]; then
          if [ -z "$pct_de" ]; then
            max_bad_widespread=yes
          else
            awk -v v="$pct_de" -v t="$MAX_PCT_ABOVE_TOLERANCE" 'BEGIN{exit !(v > t)}' && max_bad_widespread=yes
          fi
        fi
        local base_bad=no max_bad=no
        if [ "$avg_bad" = yes ]; then
          verdict="DIFF"; base_bad=yes
        elif [ "$max_bad_widespread" = yes ]; then
          verdict="DIFF"; base_bad=yes; max_bad=yes
        elif [ "$max_bad_raw" = yes ] && [ "$STRICT_CPU" = yes ]; then
          # --strict-cpu is zero-tolerance: even a handful of outlier pixels still fail.
          verdict="DIFF"; base_bad=yes; max_bad=yes
        elif [ "$deltae_rc" -eq 1 ] && [ "$STRICT_CPU" = yes ]; then
          # deltae's own exit(0)-vs-exit(1) boundary: some nonzero drift under
          # both real thresholds (max dE >= 0.01) -- only --strict-cpu cares.
          verdict="DIFF"; base_bad=yes
        fi
        # Color exactly the metric(s) responsible for the failure. A
        # --strict-cpu-only failure with neither real threshold crossed is the
        # deltae_rc=1 case above, so flag max there too when nothing else did.
        if [ "$base_bad" = yes ] && [ "$max_bad" = no ] && [ "$avg_bad" = no ]; then
          max_bad=yes
        fi
        local max_c="" max_r="" avg_c="" avg_r=""
        [ "$max_bad" = yes ] && { max_c="$C_RED"; max_r="$C_RESET"; }
        [ "$avg_bad" = yes ] && { avg_c="$C_RED"; avg_r="$C_RESET"; }
        diffnote=" [CPU vs baseline] avg dE=${avg_c}${avg_de:-n/a}${avg_r}, max dE=${max_c}${max_de}${max_r}, ${pct_de:-n/a}% px>(dE 2.3)"
      fi
    fi
  fi

  local clnote=""
  if [ "$OPENCL" = yes ] && [ "$verdict" = PASS ]; then
    local cl_out="$RESULTS_DIR/$rel.cl.png"
    local cl_runlog="$RESULTS_DIR/$rel.cl.log"
    local cfg2 cache2
    cfg2="$(mktemp -d)"
    cache2="$(mktemp -d)"

    local -a cl_args=(--width "$WIDTH" --height "$HEIGHT" --apply-custom-presets false "$raw")
    [ -n "$xmp" ] && cl_args+=("$xmp")
    cl_args+=("$cl_out" --core --configdir "$cfg2" --cachedir "$cache2"
              --conf host_memory_limit=8192 --conf worker_threads="$THREADS" -t "$THREADS")

    local cl_status
    timeout --kill-after=10 "$TIMEOUT" "$CLI_BIN" "${cl_args[@]}" >"$cl_runlog" 2>&1
    cl_status=$?
    rm -rf "$cfg2" "$cache2"

    if [ "$cl_status" -ne 0 ] || [ ! -s "$cl_out" ]; then
      # Never fails the run on its own: OpenCL may legitimately be unavailable on this
      # machine (no GPU, no runtime).
      clnote=" [opencl: unavailable/failed]"
    elif [ -x "$DELTAE_SCRIPT" ]; then
      local cl_deltae_out cl_max_de cl_avg_de cl_pct_de
      cl_deltae_out="$("$DELTAE_SCRIPT" "$out" "$cl_out" 2>&1)"
      printf '%s\n' "$cl_deltae_out" > "$RESULTS_DIR/$rel.cl.deltae.log"
      IFS=$'\t' read -r cl_max_de cl_avg_de cl_pct_de <<< "$(extract_de_stats "$cl_deltae_out")"
      # A high max dE with a handful of outlier pixels (edge/border effect) is not a real
      # CPU/GPU parity bug; a widespread share above tolerance is. OpenCL being merely
      # unavailable is handled above and never reaches here.
      local cl_bad=no
      local cl_pct_threshold=$MAX_PCT_ABOVE_TOLERANCE
      [ "$STRICT_OPENCL" = yes ] && cl_pct_threshold=0
      if [ -n "$cl_pct_de" ] \
         && awk -v p="$cl_pct_de" -v t="$cl_pct_threshold" 'BEGIN{exit !(p > t)}'; then
        verdict="DIFF"
        cl_bad=yes
      fi
      local cl_c="" cl_r=""
      [ "$cl_bad" = yes ] && { cl_c="$C_RED"; cl_r="$C_RESET"; }
      clnote="[opencl vs CPU]   avg dE=${cl_avg_de:-n/a}, max dE=${cl_max_de:-n/a}, ${cl_c}${cl_pct_de:-n/a}% px>(dE 2.3)${cl_r}"
    fi
  fi

  local record="$verdict $rel"
  if [ -n "$diffnote$clnote" ]; then
    record+=$'\n\t'"${diffnote# }"$'\n\t'"${clnote}"
  fi
  printf '%s\0' "$record"
}
export -f process_one
export CLI_BIN WIDTH HEIGHT TIMEOUT THREADS RESULTS_DIR BASELINE_DIR STRICT_CPU STRICT_OPENCL OPENCL BANK_DIR COMMAND DELTAE_SCRIPT MAX_PCT_ABOVE_TOLERANCE C_RED C_RESET

# --- driver -----------------------------------------------------------

run_bank() {
  rm -rf "$RESULTS_DIR"
  mkdir -p "$RESULTS_DIR"

  local -a raws=()
  if [ "${#FILTER_RAWS[@]}" -gt 0 ]; then
    raws=("${FILTER_RAWS[@]}")
  else
    mapfile -d '' -t raws < <(find_raws "$BANK_DIR" | LC_ALL=C sort -z)
  fi
  if [ "${#raws[@]}" -eq 0 ]; then
    err "no raw files found under $BANK_DIR"
    return 1
  fi
  if [ "$LIMIT" -gt 0 ] && [ "${#raws[@]}" -gt "$LIMIT" ]; then
    raws=("${raws[@]:0:$LIMIT}")
  fi

  local jobs="$JOBS"
  if [ -z "$jobs" ]; then
    jobs=$(( $(nproc) / THREADS ))
    [ "$jobs" -lt 1 ] && jobs=1
  fi

  log "Testing ${#raws[@]} raw file(s) from $BANK_DIR"
  log "  cli:      $CLI_BIN"
  log "  jobs:     $jobs (x $THREADS threads)"
  [ -d "$BASELINE_DIR" ] && log "  baseline: $BASELINE_DIR"

  local total=0 pass=0 fail=0
  local -a fails=()
  # NUL-delimited: each record can itself span multiple lines (verdict + an indented
  # delta-E note), which a plain $(...)-captured, newline-delimited read cannot carry --
  # bash strings can't hold an embedded NUL, and a stray note line would otherwise get
  # mis-read as its own bogus PASS/FAIL record.
  while IFS= read -r -d '' record; do
    [ -z "$record" ] && continue
    total=$((total + 1))
    local tag="${record%%[$' \n']*}"
    if [[ "$tag" == PASS* || "$tag" == DONE* ]]; then
      pass=$((pass + 1))
    else
      fail=$((fail + 1))
      fails+=("$record")
    fi
    log "  $(colorize_verdict "$record")"
  done < <(printf '%s\0' "${raws[@]}" | xargs -0 -P "$jobs" -I{} bash -c 'process_one "$1"' _ {})

  log ""
  local summary_color=$C_GREEN
  [ "$fail" -gt 0 ] && summary_color=$C_RED
  log "${summary_color}image_test: $pass/$total OK${C_RESET}"
  log ""

  if [ "$fail" -gt 0 ]; then
    err "image_test: $fail failure(s):"
    local f
    for f in "${fails[@]}"; do err "  $(colorize_verdict "$f")"; done
    err "per-image logs kept in $RESULTS_DIR"
    return 1
  fi

  [ "$KEEP" = no ] && rm -rf "$RESULTS_DIR"
  return 0
}

if [ "$COMMAND" = update-baseline ]; then
  KEEP=yes
  if [ "$WIDTH" -ne "$BASELINE_WIDTH" ] || [ "$HEIGHT" -ne "$BASELINE_HEIGHT" ]; then
    log "Ignoring --width/--height: the shared baseline is always ${BASELINE_WIDTH}x${BASELINE_HEIGHT}"
  fi
  WIDTH="$BASELINE_WIDTH"
  HEIGHT="$BASELINE_HEIGHT"
  mkdir -p "$BASELINE_DIR"

  # Only export raws that don't already have a baseline entry -- no point re-running
  # ansel-cli on the rest just to throw the result away in the copy step below.
  while IFS= read -r -d '' raw; do
    rel="${raw#"$BANK_DIR"/}"
    [ -f "$BASELINE_DIR/$rel.png" ] || FILTER_RAWS+=("$raw")
  done < <(find_raws "$BANK_DIR" | LC_ALL=C sort -z)

  if [ "${#FILTER_RAWS[@]}" -eq 0 ]; then
    log "Baseline: nothing missing, $BASELINE_DIR is already complete"
    exit 0
  fi

  target_baseline="$BASELINE_DIR"
  BASELINE_DIR="$STATE_DIR/.no-baseline-during-update"
  run_bank
  rc=$?
  BASELINE_DIR="$target_baseline"
  if [ "$rc" -ne 0 ]; then
    err "update-baseline: run failed, baseline left untouched"
    exit "$rc"
  fi
  added=0
  while IFS= read -r -d '' f; do
    mkdir -p "$BASELINE_DIR/$(dirname "$f")"
    cp "$RESULTS_DIR/$f" "$BASELINE_DIR/$f"
    added=$((added + 1))
  done < <(cd "$RESULTS_DIR" && find . -name '*.png' -print0)
  log "Baseline: $added new entr$([ "$added" = 1 ] && echo y || echo ies) -- in $BASELINE_DIR"
  exit 0
fi

run_bank
exit $?
