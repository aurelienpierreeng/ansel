#!/usr/bin/env bash
#
# Two module boundaries this tree relies on, made checkable rather than remembered.
#
#   1. src/system must include nothing that could bring state with it.
#
# The point of sorting the tree by statelessness is that the check becomes mechanical: once a
# directory is known stateless, anything built only from it is stateless too, and nobody has to
# re-derive that per file. That inference is only sound while src/system depends on nothing
# that could acquire state behind it -- so this pins what it may include.
#
# Statelessness itself is measured separately, from the linker's view, by
# tools/statelessness_audit.py --dir src/system. That needs a Debug build; this does not, so
# this one runs everywhere and catches the cheap case: an include that opens the door.
#
#   2. src/widgets must not include anything from gui/.
#
# That is what makes widgets/ a separate build target and, per the GTK->Qt goal, what makes a
# port sizeable: the widget set has to be movable without the application coming with it. The
# layering checker is structurally blind to this one -- tools/include_graph.py puts gui and
# widgets on the same layer (4), so a widgets->gui edge is not an upward include and does not
# register. It went unnoticed once already: gui/screen_metrics.h reached widgets/draw.h through
# two helpers that turned out to have no callers at all, and only an adversarial read caught it.
#
# What each module may HOLD, rather than include, is a different question and is not answered
# here: see tools/check_statelessness.sh, which measures it from the compiled objects. A
# textual "is there a file-scope static?" rule was tried and removed -- it cannot tell a
# g_signal_new id cache from real state, and a gate that cries wolf gets switched off.
#
# Usage:
#   tools/check_module_boundaries.sh

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 2

# ---------------------------------------------------------------------------------------
# 1. What src/system may include, and why. Anything else is a finding.
#
#   system/*      itself
#   win/*         the Windows shims for POSIX facilities it wraps
#   external/*    vendored third-party headers -- outside our layering entirely
#   config.h      build configuration, generated
#   <...>         libc, glib and platform headers -- outside the tree, not our layering
#
# There are no exceptions, and that is the point: the two that used to be needed --
# common/macros.h for IS_NULL_PTR and common/dtpthread.h for memory_arena's mutex member --
# were both header-only and both measured stateless, so they moved into src/system rather than
# being excused. An exception list is a slow leak; the first entry is always justified and the
# fifth never is.
#
# dtpthread is the instructive case: the HEADER is pure (types plus static inline wrappers over
# pthread) and moved, but dtpthread.c reads dt_conf_get_int("cpu_fp_mode") on every thread
# creation and traces through dt_print, so the .c stayed in common/. This gate is what caught
# that -- the header had been moved with its .c on the assumption that "stateless header"
# implied "stateless module", and it does not.

findings=0
while IFS= read -r line; do
  file="${line%%:*}"
  rest="${line#*:}"
  inc="$(printf '%s' "$rest" | sed -n 's/.*#[ \t]*include[ \t]*"\([^"]*\)".*/\1/p')"
  [ -n "$inc" ] || continue

  case "$inc" in
    system/*|win/*|external/*|config.h) continue ;;
  esac
  # A bare name resolves next to the including file, i.e. inside src/system.
  case "$inc" in
    */*) ;;
    *) [ -f "src/system/$inc" ] && continue ;;
  esac
  echo "${file}: includes ${inc}"
  findings=$((findings + 1))
done < <(grep -Hn '^[ \t]*#[ \t]*include[ \t]*"' src/system/*.c src/system/*.h 2>/dev/null)

# ---------------------------------------------------------------------------------------
# 2. src/widgets must not reach into gui/.
while IFS= read -r line; do
  file="${line%%:*}"
  echo "${file}: includes gui/ -- widgets must not depend on the application GUI"
  findings=$((findings + 1))
done < <(grep -Hn '^[ \t]*#[ \t]*include[ \t]*"gui/' src/widgets/*.c src/widgets/*.h 2>/dev/null)

echo
if [ "${findings}" -gt 0 ]; then
  cat <<'MSG'
A module boundary was crossed.

src/system is the stateless foundation: code elsewhere infers "this is stateless" from the
fact that it was built only from src/system, without checking. An include that leaves the
directory puts that inference at the mercy of whatever the target does later. Either the
include is not needed -- move it to the .c, or drop it -- or the thing it wants is itself
stateless and belongs in src/system too, which is how system/macros.h and system/dtpthread.h
got there.

src/widgets must not reach into gui/, or the widget set stops being movable without the
application. If a widget needs something the application knows, the application pushes it in
through widgets/widget_settings.h -- that is what those handler slots are for.

Adding an exception to this script should be the last resort, and needs the reason written
next to it.
MSG
  exit 1
fi

echo "OK: src/system is closed, and src/widgets does not reach into gui/."
exit 0
