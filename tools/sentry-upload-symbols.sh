#!/usr/bin/env bash
#
# Upload debug-information files to Sentry so native crash reports get
# symbolicated server-side. This is the single source of truth: the nightly CI
# workflows (Linux/macOS/Windows) and local testing all call this exact script.
#
# It is intentionally NOT wired into the build (CMake / build.sh / regular CI):
# building never uploads symbols. Symbols are only pushed when this script is run
# explicitly.
#
# The only real secret, the auth token, is read from the environment at runtime:
#   SENTRY_AUTH_TOKEN   (required) token with project:releases + project:write scope
# Project coordinates default to Ansel's Sentry project but stay overridable:
#   SENTRY_ORG          (optional) organization slug   (default: aurelienpierreeng)
#   SENTRY_PROJECT      (optional) project slug         (default: ansel)
# Other optional knobs:
#   SENTRY_CLI              path to an existing sentry-cli (else found on PATH, else downloaded)
#   SENTRY_CLI_DIR          where to download sentry-cli if needed (default: a temp dir)
#   SENTRY_INCLUDE_SOURCES  set to "0" to skip embedding sources (default: include them)
#
# Usage:
#   tools/sentry-upload-symbols.sh [PATH ...]
#     PATH ...  build/install directories to scan for debug files (default: ".")
#
# If any required secret is missing (e.g. a fork without repository secrets), the
# script prints a notice and exits 0 so it never breaks a CI job or local build.

set -euo pipefail

log() { printf '[sentry-upload-symbols] %s\n' "$*"; }

# Directories/files to scan (default to the current directory).
if [ "$#" -gt 0 ]; then
  SCAN_PATHS=("$@")
else
  SCAN_PATHS=(".")
fi

# Project coordinates (slugs) are hardcoded defaults, still overridable via env.
SENTRY_ORG="${SENTRY_ORG:-aurelienpierreeng}"
SENTRY_PROJECT="${SENTRY_PROJECT:-ansel}"

# Only the auth token is a real secret and must come from the environment; skip
# gracefully when it is absent (e.g. forks without repository secrets).
if [ -z "${SENTRY_AUTH_TOKEN:-}" ]; then
  log "SENTRY_AUTH_TOKEN not set - skipping symbol upload."
  exit 0
fi
export SENTRY_AUTH_TOKEN SENTRY_ORG SENTRY_PROJECT

# Locate sentry-cli, or fetch a local copy via the official installer.
cli="${SENTRY_CLI:-}"
if [ -z "$cli" ]; then
  if command -v sentry-cli >/dev/null 2>&1; then
    cli="sentry-cli"
  else
    log "sentry-cli not found on PATH - downloading a local copy..."
    install_dir="${SENTRY_CLI_DIR:-${TMPDIR:-/tmp}/ansel-sentry-cli}"
    mkdir -p "$install_dir"
    INSTALL_DIR="$install_dir" curl -fsSL https://sentry.io/get-cli/ | INSTALL_DIR="$install_dir" bash
    for cand in "$install_dir/sentry-cli" "$install_dir/sentry-cli.exe"; do
      if [ -x "$cand" ]; then cli="$cand"; break; fi
    done
    [ -z "$cli" ] && cli="$(command -v sentry-cli || true)"
  fi
fi
if [ -z "$cli" ]; then
  log "ERROR: could not find or install sentry-cli." >&2
  exit 1
fi
log "using sentry-cli: $cli"
"$cli" --version || true

# Embed source context by default (helps reading frames in the Sentry UI).
extra_args=()
if [ "${SENTRY_INCLUDE_SOURCES:-1}" != "0" ]; then
  extra_args+=(--include-sources)
fi

log "uploading debug files (org=$SENTRY_ORG project=$SENTRY_PROJECT) from: ${SCAN_PATHS[*]}"
# Note: the `${arr[@]+...}` form keeps this safe under bash 3.2 (macOS) with `set -u`.
"$cli" debug-files upload \
  --org "$SENTRY_ORG" \
  --project "$SENTRY_PROJECT" \
  ${extra_args[@]+"${extra_args[@]}"} \
  "${SCAN_PATHS[@]}"

log "done."
