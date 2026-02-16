#!/usr/bin/env bash
# Created: 2026-02-16
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NIX_FILE="${SCRIPT_DIR}/nix/default.nix"

if ! command -v nix-shell >/dev/null 2>&1; then
  echo 'nix-shell not found. Install Nix from https://nixos.org/.' >&2
  exit 1
fi

# Realize the dependencies defined in packaging/nix/default.nix.
# This will populate the Nix store without building ansel itself.
nix-shell "${NIX_FILE}" --run "true"
