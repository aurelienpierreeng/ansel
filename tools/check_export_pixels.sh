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
# PASS A RAW. The default image is data/pixmaps/ansel.png, which is convenient and in the
# tree, and it does NOT exercise the paths colour work usually touches: colorin resolves a
# PNG through its embedded/sRGB branch and never reaches the camera-matrix branch that every
# raw takes. A change that moved 747159 exported pixels of a NEF by one LSB passed this
# script twice on the PNG before anyone thought to point it at a raw.
#
# Each ref is exported TWICE: once letting colorout pick the output profile (which, on an
# export naming no colour space, means the image's own input profile) and once forced to
# sRGB. Those are different code paths through commit_params and they fail independently.
#
# Usage:
#   tools/check_export_pixels.sh <ref-a> <ref-b> [image]
#   tools/check_export_pixels.sh master HEAD ~/Pictures/DSC0004.NEF
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

# Resolve BOTH refs to concrete SHAs before touching HEAD. Passing the literal "HEAD" as
# the second ref is the obvious thing to type and is silently wrong: by the time it is
# used, HEAD has already been moved to the first ref, so the script compares that ref with
# itself and reports "identical" -- which is exactly what happened the first time this ran.
SHA_A="$(git rev-parse --verify --quiet "${REF_A}^{commit}")" || true
SHA_B="$(git rev-parse --verify --quiet "${REF_B}^{commit}")" || true
if [ -z "${SHA_A}" ] || [ -z "${SHA_B}" ]; then
  echo "FAILED: cannot resolve ${REF_A} and/or ${REF_B} to commits."
  exit 2
fi
if [ "${SHA_A}" = "${SHA_B}" ]; then
  echo "FAILED: ${REF_A} and ${REF_B} are the same commit (${SHA_A}); nothing to compare."
  exit 2
fi
OUT_DIR="$(mktemp -d)"
restore() {
  # Loud on failure: leaving the tree on someone else's commit and saying nothing is how a
  # later command silently operates on the wrong source.
  git checkout --quiet "${ORIGINAL_REF}" || echo "WARNING: could not restore ${ORIGINAL_REF}; tree is on $(git rev-parse --short HEAD)"
  rm -rf "${OUT_DIR}"
}
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

  # Two exports per ref: the output profile colorout picks for itself, and sRGB forced.
  local variant
  for variant in default srgb; do
    local work target icc=()
    work="$(mktemp -d)"
    target="${out}.${variant}.png"
    [ "${variant}" = "srgb" ] && icc=(--icc-type SRGB)
    rm -f "${target}"
    # --configdir and --library are NOT optional: without them ansel-cli writes into the
    # user's real configuration, which has corrupted a live collection before.
    "${cli}" \
      --width 2048 --height 2048 \
      --apply-custom-presets false \
      "${icc[@]}" \
      "${IMAGE}" "${target}" \
      --core --disable-opencl \
      --configdir "${work}/config" --library "${work}/config/library.db" \
      --conf host_memory_limit=8192 --conf worker_threads=4 -t 4 \
      --conf plugins/lighttable/export/force_lcms2=FALSE \
      --conf plugins/lighttable/export/iccintent=0 \
      >"${work}/log" 2>&1
    local status=$?
    rm -rf "${work}"
    [ ${status} -eq 0 ] || { echo "FAILED: ${variant} export at ${ref} exited ${status}"; exit 1; }
    [ -s "${target}" ] || { echo "FAILED: ${variant} export at ${ref} wrote nothing"; exit 1; }
  done
}

export_at "${SHA_A}" "${OUT_DIR}/a" "build-regress-a"
export_at "${SHA_B}" "${OUT_DIR}/b" "build-regress-b"

echo "image: ${IMAGE}"
"${PY}" - "${OUT_DIR}/a" "${OUT_DIR}/b" "${REF_A} (${SHA_A:0:10})" "${REF_B} (${SHA_B:0:10})" <<'PYEOF'
import sys
import numpy as np
from PIL import Image

stem_a, stem_b, ref_a, ref_b = sys.argv[1:5]
failed = False

for variant in ("default", "srgb"):
    a = np.asarray(Image.open(f"{stem_a}.{variant}.png").convert("RGBA")).astype(np.int32)
    b = np.asarray(Image.open(f"{stem_b}.{variant}.png").convert("RGBA")).astype(np.int32)

    label = "output profile chosen by colorout" if variant == "default" else "output profile forced to sRGB"

    if a.shape != b.shape:
        print(f"FAILED [{label}]: geometry changed, {ref_a} is {a.shape}, {ref_b} is {b.shape}")
        failed = True
        continue

    diff = np.abs(a - b)
    n_diff = int((diff.any(axis=-1)).sum())
    total = a.shape[0] * a.shape[1]

    if n_diff == 0:
        print(f"OK [{label}]: {total} pixels identical between {ref_a} and {ref_b}.")
        continue

    failed = True
    print(f"PIXELS CHANGED [{label}] between {ref_a} and {ref_b}:")
    print(f"  {n_diff} of {total} pixels differ ({100.0 * n_diff / total:.4f}%)")
    print(f"  max abs delta {int(diff.max())}, mean over changed {diff[diff > 0].mean():.4f}")
    for i, ch in enumerate("RGBA"):
        d = diff[..., i]
        if d.any():
            print(f"  {ch}: {int((d > 0).sum())} changed, max {int(d.max())}")

if not failed:
    sys.exit(0)
print()
print("A one-LSB delta is still a real difference: this pipeline is deterministic, so")
print("nothing should move unless the change intended it. Say which change caused it")
print("and why, in the commit message, or find the bug.")
sys.exit(1)
PYEOF
