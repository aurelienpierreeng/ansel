#!/usr/bin/env bash
#
# Ratchet on the include graph: the number of layering violations may go down, never up, and
# the graph must stay acyclic.
#
# This is the gate that catches the class tools/check_unused_includes.sh cannot see. That one
# analyses translation units, so it is blind to headers -- and the case that prompted both
# checks was a header: common/metadata.h included gui/gtk.h, used no GTK symbol, and pushed
# the GUI into all 16 of its includers. As a layering violation it is visible here.
#
# A ratchet rather than a threshold: the tree carries a few hundred inherited violations, so
# demanding zero would mean turning the check off. Demanding "no worse than yesterday" costs
# nothing to comply with and cannot be quietly eroded.
#
# Usage:
#   tools/check_layering.sh            # check against tools/include_baseline.txt
#   tools/check_layering.sh --update   # record the current numbers as the new baseline

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 2
BASELINE="tools/include_baseline.txt"
PYTHON="${PYTHON:-python3}"

summary="$(${PYTHON} tools/include_graph.py --summary 2>/dev/null)" || {
  echo "error: tools/include_graph.py failed" >&2; exit 2; }

get() { printf '%s\n' "$summary" | awk -v k="$1" '$1==k {print $2}'; }
cycles="$(get cycles)"
violations="$(get layering_violations)"

if [ -z "${cycles}" ] || [ -z "${violations}" ]; then
  echo "error: could not read cycles/layering_violations from the summary" >&2
  exit 2
fi

if [ "${1:-}" = "--update" ]; then
  {
    echo "# Include-graph ratchet baseline. Regenerate with tools/check_layering.sh --update."
    echo "# Lower these by fixing includes; never raise them to make CI pass."
    echo "cycles ${cycles}"
    echo "layering_violations ${violations}"
  } > "${BASELINE}"
  echo "Baseline updated: cycles ${cycles}, layering_violations ${violations}"
  exit 0
fi

if [ ! -f "${BASELINE}" ]; then
  echo "error: ${BASELINE} missing -- run tools/check_layering.sh --update" >&2
  exit 2
fi

base_violations="$(awk '$1=="layering_violations" {print $2}' "${BASELINE}")"

status=0

# Cycles are absolute, not ratcheted. The include guards documented in CLAUDE.md exist so a
# cycle is a hard error rather than something #pragma once absorbs silently; a baseline here
# would give that back.
if [ "${cycles}" -ne 0 ]; then
  echo "FAIL: ${cycles} include cycle(s). The graph must stay acyclic -- see CLAUDE.md on why"
  echo "      this repository uses explicit include guards instead of #pragma once."
  status=1
fi

if [ "${violations}" -gt "${base_violations}" ]; then
  echo "FAIL: layering violations rose ${base_violations} -> ${violations} (+$((violations - base_violations)))."
  echo
  echo "Something now includes a header from a higher layer. Run:"
  echo "    python3 tools/include_graph.py"
  echo "for the list, and give the file the specific lower-layer header it needs -- or invert"
  echo "the dependency with a handler the upper layer registers, as common/film.c and"
  echo "common/database.c do."
  echo
  echo "Do NOT raise the baseline to make this pass."
  status=1
elif [ "${violations}" -lt "${base_violations}" ]; then
  echo "Layering violations fell ${base_violations} -> ${violations}. Please run:"
  echo "    tools/check_layering.sh --update"
  echo "and commit ${BASELINE} so the improvement is locked in."
  status=1
else
  echo "OK: cycles 0, layering violations ${violations} (baseline ${base_violations})."
fi

exit ${status}
