#!/usr/bin/env bash
#
# Compare the EXPORTED PIXELS of two commits, not the exported file.
#
# tools/check_it_runs.sh answers "does it survive an export", which is the question that
# catches double frees. It cannot answer "does it still produce the same image", and the
# obvious way to ask that -- sha256 the output file -- is wrong: the PNG carries the build's
# version string in its metadata, so two builds of two commits differ by a byte or two of
# compressed text while every pixel is identical. One such 29558-vs-29557 delta cost a
# double-take during the colorprofiles work.
#
# So: decode both PNGs and compare the pixel arrays. Byte-identical pixels or a report of
# exactly how many differ and by how much.
#
# This is the standing regression check for anything touching colour management, where "it
# still runs" is a very low bar and a one-LSB hue shift is the actual failure mode.
#
# Usage:
#   tools/check_export_pixels.sh <ref-a> <ref-b> [image]
#   tools/check_export_pixels.sh master HEAD
#
# It builds each ref into its own build dir (kept between runs, so the second call is fast),
# stages it, and exports. Needs a clean tree: it checks out other commits.

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 2

REF_A="${1:?need two refs}"
REF_B="${2:?need two refs}"
IMAGE="${3:-${REPO_ROOT}/data/pixmaps/256x256/ansel.png}"

# --ignore-submodules: src/external/* are pinned third-party checkouts whose recorded SHA
# routinely reads as modified; that has nothing to do with our sources.
if [ -n "$(git status --porcelain --ignore-submodules=all -- src/ tools/ 2>/dev/null)" ]; then
  echo "FAILED: working tree is dirty under src/ or tools/."
  echo "        This script checks out other commits; commit or stash first."
  exit 2
fi

PY="$(command -v python3.12 || command -v python3)"
"${PY}" -c "import numpy, PIL" 2>/dev/null || {
  echo "note: need numpy and PIL to decode the PNGs. Skipping."
  exit 0
}

ORIGINAL_REF="$(git symbolic-ref --quiet --short HEAD || git rev-parse HEAD)"
OUT_DIR="$(mktemp -d)"
restore() { git checkout --quiet "${ORIGINAL_REF}" 2>/dev/null; rm -rf "${OUT_DIR}"; }
trap restore EXIT

export_at() {
  local ref="$1" out="$2" builddir="$3"

  git checkout --quiet "${ref}" || { echo "FAILED: cannot check out ${ref}"; exit 2; }
  echo "--- ${ref} ($(git rev-parse --short HEAD))"

  if [ ! -f "${builddir}/build.ninja" ]; then
    cmake -B "${builddir}" -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_INSTALL_PREFIX=/opt/ansel >/dev/null 2>&1 \
      || { echo "FAILED: cmake configure in ${builddir}"; exit 2; }
  fi
  ninja -C "${builddir}" -j4 >/dev/null 2>&1 \
    || { echo "FAILED: build at ${ref}"; exit 2; }
  ( cd "${builddir}" && DESTDIR="$PWD/stage" cmake -P cmake_install.cmake >/dev/null 2>&1 )

  local cli="${builddir}/stage/opt/ansel/bin/ansel-cli"
  [ -x "${cli}" ] || { echo "FAILED: no staged ansel-cli at ${cli}"; exit 2; }

  local work
  work="$(mktemp -d)"
  # --configdir and --library are NOT optional: without them ansel-cli writes into the
  # user's real configuration, which has corrupted a live collection before.
  "${cli}" \
    --width 2048 --height 2048 \
    --apply-custom-presets false \
    "${IMAGE}" "${out}" \
    --core --disable-opencl \
    --configdir "${work}/config" --library "${work}/config/library.db" \
    --conf host_memory_limit=8192 --conf worker_threads=4 -t 4 \
    --conf plugins/lighttable/export/force_lcms2=FALSE \
    --conf plugins/lighttable/export/iccintent=0 \
    >"${work}/log" 2>&1
  local status=$?
  rm -rf "${work}"
  [ ${status} -eq 0 ] || { echo "FAILED: export at ${ref} exited ${status}"; exit 1; }
  [ -s "${out}" ] || { echo "FAILED: export at ${ref} wrote nothing"; exit 1; }
}

export_at "${REF_A}" "${OUT_DIR}/a.png" "build-regress-a"
export_at "${REF_B}" "${OUT_DIR}/b.png" "build-regress-b"

"${PY}" - "${OUT_DIR}/a.png" "${OUT_DIR}/b.png" "${REF_A}" "${REF_B}" <<'PYEOF'
import sys
import numpy as np
from PIL import Image

path_a, path_b, ref_a, ref_b = sys.argv[1:5]
a = np.asarray(Image.open(path_a).convert("RGBA")).astype(np.int32)
b = np.asarray(Image.open(path_b).convert("RGBA")).astype(np.int32)

if a.shape != b.shape:
    print(f"FAILED: geometry changed, {ref_a} is {a.shape}, {ref_b} is {b.shape}")
    sys.exit(1)

diff = np.abs(a - b)
n_diff = int((diff.any(axis=-1)).sum())
total = a.shape[0] * a.shape[1]

if n_diff == 0:
    print(f"OK: {total} pixels identical between {ref_a} and {ref_b}.")
    sys.exit(0)

print(f"PIXELS CHANGED between {ref_a} and {ref_b}:")
print(f"  {n_diff} of {total} pixels differ ({100.0 * n_diff / total:.4f}%)")
print(f"  max abs delta {int(diff.max())}, mean over changed {diff[diff > 0].mean():.4f}")
for i, ch in enumerate("RGBA"):
    d = diff[..., i]
    if d.any():
        print(f"  {ch}: {int((d > 0).sum())} changed, max {int(d.max())}")
print()
print("A one-LSB delta is still a real difference: this pipeline is deterministic, so")
print("nothing should move unless the change intended it. Say which change caused it")
print("and why, in the commit message, or find the bug.")
sys.exit(1)
PYEOF
