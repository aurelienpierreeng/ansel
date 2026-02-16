#!/usr/bin/env bash
# Created: 2026-02-16
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"${SCRIPT_DIR}"/install-deps-ubuntu.sh "$@"
