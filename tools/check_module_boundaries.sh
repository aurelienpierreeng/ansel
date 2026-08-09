#!/usr/bin/env bash
#
# Three module boundaries this tree relies on, made checkable rather than remembered.
# The third is a ratchet on a migration in progress rather than a settled rule.
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

# ---------------------------------------------------------------------------------------
# 3. src/colorprofiles is closed: nothing outside it names the module's state.
#
# This started as a ratchet on a migration -- 59 places read dt_colorspaces_t's members
# through dt_colorspaces_get_global(), and 4 took its rwlock by hand. Both are zero now,
# so it is a real boundary and the check is that it stays one. A new caller of either is
# a caller that should be asking the module a question instead.
#
# xprofile_lock is counted separately because it is the sharper of the two: a caller that
# holds the module's lock is a caller that can deadlock it or use a handle it frees.
accessor_baseline=0
lock_baseline=0

accessor_now=$(grep -rn 'dt_colorspaces_get_global()' src/ --include='*.c' --include='*.h' --include='*.cc' \
               2>/dev/null | grep -cv '^src/colorprofiles/')
# Acquisitions, not mentions: one number per caller-held region, so it reads as
# "4 places outside the module still take its lock" rather than counting unlocks twice.
lock_now=$(grep -rnE 'pthread_rwlock_(rd|wr|tryrd|trywr)lock[^;]*xprofile_lock' \
           src/ --include='*.c' --include='*.h' --include='*.cc' \
           2>/dev/null | grep -cv '^src/colorprofiles/')

echo "colorprofiles: ${accessor_now} external dt_colorspaces_get_global() (baseline ${accessor_baseline}),"
echo "               ${lock_now} external xprofile_lock (baseline ${lock_baseline})."

if [ "${accessor_now}" -gt "${accessor_baseline}" ] || [ "${lock_now}" -gt "${lock_baseline}" ]; then
  echo "colorprofiles: a count ROSE. New code must go through the module API, not its globals."
  findings=$((findings + 1))
elif [ "${accessor_now}" -lt "${accessor_baseline}" ] || [ "${lock_now}" -lt "${lock_baseline}" ]; then
  echo "colorprofiles: a count fell -- lower the baseline in this script, in the same commit."
  findings=$((findings + 1))
fi

echo
system_widgets_failed=0
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
  system_widgets_failed=1
fi


# 4. src/common/opencl is closed: nothing outside it names the module's state.
#
# `darktable.opencl` used to be a member of the application struct, so dt_opencl_t's device
# array, its per-device locks and nine other subsystems' kernel bundles were one dereference
# away from any file that included darktable.h. The struct is a file-static in
# common/opencl.c now and is not even declared in the header, so both counts are structurally
# zero -- this check is what keeps someone from re-exporting it "just for one caller".
#
# The kernel-bundle count is separate because it is the shape that comes back: a subsystem
# that hands its own state to opencl.c to hold is re-creating the round trip that was removed.
opencl_state_baseline=0
opencl_parked_baseline=0

opencl_state_now=$(grep -rn 'darktable\.opencl\|dt_opencl_get_global' src/ --include='*.c' --include='*.h' --include='*.cc' \
                   2>/dev/null | grep -v '^src/common/opencl' | grep -cv '^\s*[0-9]*:\s*[/ ]\*')
# A member of dt_opencl_t that is really another module's: `struct dt_<x>_cl_global_t *` on it.
# The struct lives in the .c now -- that is the point of the check -- so look for it there,
# and in the header too, so that re-exporting it does not make the count silently zero.
opencl_parked_now=$(cat src/common/opencl.c src/common/opencl.h 2>/dev/null \
                    | sed -n '/^typedef struct dt_opencl_t$/,/^} dt_opencl_t;/p' \
                    | grep -c '_cl_global_t \*')

echo "opencl:        ${opencl_state_now} external references to the module's state (baseline ${opencl_state_baseline}),"
echo "               ${opencl_parked_now} foreign kernel bundles parked on dt_opencl_t (baseline ${opencl_parked_baseline})."

if [ "${opencl_state_now}" -gt "${opencl_state_baseline}" ] || [ "${opencl_parked_now}" -gt "${opencl_parked_baseline}" ]; then
  echo "opencl: a count ROSE. Ask the module a question (dt_opencl_get_num_devices(),"
  echo "        dt_opencl_get_device_name(), dt_opencl_reserve_device_*()) instead of reaching"
  echo "        into its state, and keep your own kernels in your own file."
  findings=$((findings + 1))
elif [ "${opencl_state_now}" -lt "${opencl_state_baseline}" ] || [ "${opencl_parked_now}" -lt "${opencl_parked_baseline}" ]; then
  echo "opencl: a count fell -- lower the baseline in this script, in the same commit."
  findings=$((findings + 1))
fi

if [ "${findings}" -gt 0 ]; then
  exit 1
fi

echo "OK: src/system is closed, src/widgets does not reach into gui/, colorprofiles and opencl held."
exit 0
