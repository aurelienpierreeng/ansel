#!/usr/bin/env bash

# Collect OpenCL build logs and cached binaries for Ansel in one shot.
# This uses an isolated cache directory, enables `-d opencl,demosaic`, and
# gathers the resulting logs plus kernel cache artifacts into one output folder.

set -u
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_LIB_DIR="${REPO_ROOT}/build/src"
ANSEL_GUI="/opt/ansel/bin/ansel"
ANSEL_CLI="/opt/ansel/bin/ansel-cli"

usage()
{
  cat <<'EOF'
Usage:
  tools/collect_opencl_artifacts.sh [options] --gui
  tools/collect_opencl_artifacts.sh [options] <raw> [<xmp>] [<output>]

Options:
  --gui                 Launch GUI mode and collect artifacts after you quit.
  --result-dir <dir>    Output directory for logs and copied binaries.
                        Default: /tmp/ansel-opencl-artifacts
  --cache-dir <dir>     Isolated cache directory passed to Ansel.
                        Default: <result-dir>/cache
  --help                Show this help.

CLI mode:
  <raw>                 Input image path.
  <xmp>                 Optional sidecar path.
  <output>              Optional exported output path.
                        Default: <result-dir>/ansel-opencl-out.tif

Examples:
  tools/collect_opencl_artifacts.sh --gui
  tools/collect_opencl_artifacts.sh /path/file.raw
  tools/collect_opencl_artifacts.sh /path/file.raw /path/file.xmp /tmp/out.tif
EOF
}

require_file()
{
  local path="$1"
  local label="$2"
  if [[ ! -e "$path" ]]; then
    printf 'Missing %s: %s\n' "$label" "$path" >&2
    exit 1
  fi
}

MODE="cli"
RESULT_DIR="/tmp/ansel-opencl-artifacts"
CACHE_DIR=""
RAW=""
XMP=""
OUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --gui)
      MODE="gui"
      shift
      ;;
    --result-dir)
      RESULT_DIR="$2"
      shift 2
      ;;
    --cache-dir)
      CACHE_DIR="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --*)
      printf 'Unknown option: %s\n' "$1" >&2
      usage >&2
      exit 1
      ;;
    *)
      if [[ -z "$RAW" ]]; then
        RAW="$1"
      elif [[ -z "$XMP" && "$1" == *.xmp ]]; then
        XMP="$1"
      elif [[ -z "$OUT" ]]; then
        OUT="$1"
      else
        printf 'Unexpected argument: %s\n' "$1" >&2
        usage >&2
        exit 1
      fi
      shift
      ;;
  esac
done

if [[ -z "$CACHE_DIR" ]]; then
  CACHE_DIR="${RESULT_DIR}/cache"
fi

if [[ "$MODE" == "cli" ]]; then
  if [[ -z "$RAW" ]]; then
    printf 'CLI mode requires an input RAW path.\n' >&2
    usage >&2
    exit 1
  fi
  if [[ -z "$OUT" ]]; then
    OUT="${RESULT_DIR}/ansel-opencl-out.tif"
  fi
fi

require_file "$ANSEL_GUI" "GUI executable"
require_file "$ANSEL_CLI" "CLI executable"
if [[ "$MODE" == "cli" ]]; then
  require_file "$RAW" "RAW input"
  if [[ -n "$XMP" ]]; then
    require_file "$XMP" "XMP input"
  fi
fi

LOG_FILE="${RESULT_DIR}/ansel-opencl.log"
SUMMARY_FILE="${RESULT_DIR}/ansel-opencl-summary.txt"
CLINFO_FILE="${RESULT_DIR}/clinfo.txt"
CACHE_FILES_FILE="${RESULT_DIR}/ansel-cache-files.txt"
BINARIES_FILE="${RESULT_DIR}/ansel-binaries.txt"
BINARIES_HEX_FILE="${RESULT_DIR}/ansel-binaries-hex.txt"
COMMAND_FILE="${RESULT_DIR}/command.txt"
STATUS_FILE="${RESULT_DIR}/exit-status.txt"
BIN_COPY_DIR="${RESULT_DIR}/binaries"

rm -rf "$RESULT_DIR"
mkdir -p "$RESULT_DIR" "$CACHE_DIR" "$BIN_COPY_DIR"

if command -v clinfo >/dev/null 2>&1; then
  clinfo >"$CLINFO_FILE" 2>&1 || true
else
  printf 'clinfo not found in PATH\n' >"$CLINFO_FILE"
fi

if [[ "$MODE" == "gui" ]]; then
  CMD=( "$ANSEL_GUI" --cachedir "$CACHE_DIR" -d opencl -d perf )
else
  CMD=( "$ANSEL_CLI" "$RAW" )
  if [[ -n "$XMP" ]]; then
    CMD+=( "$XMP" )
  fi
  CMD+=( "$OUT" --core --cachedir "$CACHE_DIR" -d opencl -d perf)
fi

{
  printf 'PWD=%s\n' "$REPO_ROOT"
  printf 'LD_LIBRARY_PATH=%s\n' "$BUILD_LIB_DIR"
  printf 'CACHE_DIR=%s\n' "$CACHE_DIR"
  printf 'RESULT_DIR=%s\n' "$RESULT_DIR"
  printf 'MODE=%s\n' "$MODE"
  printf 'COMMAND='
  printf '%q ' "${CMD[@]}"
  printf '\n'
} >"$COMMAND_FILE"

(
  cd "$REPO_ROOT" || exit 1
  env LD_LIBRARY_PATH="$BUILD_LIB_DIR" "${CMD[@]}"
) 2>&1 | tee "$LOG_FILE"
status=${PIPESTATUS[0]}
printf '%d\n' "$status" >"$STATUS_FILE"

grep -nE "DEVICE:|DRIVER VERSION|DEVICE VERSION|KERNEL BUILD DIRECTORY|CL COMPILER OPTION|BUILD LOG|saving binary|loaded cached binary|successfully built program|guided laplacian|demosaic|opencl_init" \
  "$LOG_FILE" >"$SUMMARY_FILE" || true

find "$CACHE_DIR" \( -type f -o -type l \) | sort >"$CACHE_FILES_FILE"

{
  while IFS= read -r entry; do
    [[ -z "$entry" ]] && continue
    real="$(readlink -f "$entry" 2>/dev/null || realpath "$entry" 2>/dev/null || printf '%s' "$entry")"
    printf '===== %s\n' "$entry"
    ls -l "$entry" 2>/dev/null || true
    if [[ -f "$real" ]]; then
      file "$real" 2>/dev/null || true
      strings -a "$real" 2>/dev/null | head -n 200 || true
    fi
    printf '\n'
  done < <(find "$CACHE_DIR" -name '*.bin' | sort)
} >"$BINARIES_FILE"

{
  while IFS= read -r entry; do
    [[ -z "$entry" ]] && continue
    real="$(readlink -f "$entry" 2>/dev/null || realpath "$entry" 2>/dev/null || printf '%s' "$entry")"
    printf '===== %s\n' "$entry"
    if [[ -f "$real" ]]; then
      xxd -g 1 -l 256 "$real" 2>/dev/null || true
    fi
    printf '\n'
  done < <(find "$CACHE_DIR" -name '*.bin' | sort)
} >"$BINARIES_HEX_FILE"

while IFS= read -r entry; do
  [[ -z "$entry" ]] && continue
  real="$(readlink -f "$entry" 2>/dev/null || realpath "$entry" 2>/dev/null || printf '%s' "$entry")"
  if [[ -f "$real" ]]; then
    cp -f "$real" "${BIN_COPY_DIR}/$(basename "$entry")"
  fi
done < <(find "$CACHE_DIR" -name '*.bin' | sort)

cat <<EOF
Artifacts written to:
  $RESULT_DIR

Most useful files:
  $CLINFO_FILE
  $SUMMARY_FILE
  $CACHE_FILES_FILE
  $BINARIES_FILE
  $BINARIES_HEX_FILE
  $COMMAND_FILE
  $STATUS_FILE
EOF

exit "$status"
