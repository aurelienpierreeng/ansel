#!/usr/bin/env bash
# Incremental rebuild of the already-configured build/ tree and staged install into
# build/stage, so the running app is never taken from the working tree (its share/ is empty).
#
# Run from the MSYS2 MINGW64 shell, from anywhere:
#   ./rebuild.sh
#
# See "Building locally (Windows / MSYS2)" in AGENTS.md.
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cmake --build "${ROOT}/build" -j"$(nproc)"
cmake --install "${ROOT}/build" --prefix "${ROOT}/build/stage"

echo "Staged: ${ROOT}/build/stage/bin/ansel.exe"
