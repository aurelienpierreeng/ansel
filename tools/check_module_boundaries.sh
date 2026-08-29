#!/usr/bin/env bash
#
# Four module boundaries this tree relies on, made checkable rather than remembered.
# The third and fourth are ratchets on migrations in progress rather than settled rules.
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
# port sizeable: the widget set has to be movable without the application coming with it. It
# went unnoticed once already: gui/screen_metrics.h reached widgets/draw.h through two helpers
# that turned out to have no callers at all, and only an adversarial read caught it.
#
# The layering checker used to be structurally blind to this -- it put gui/ and widgets/ on the
# same layer (4), so a widgets->gui edge was not an upward include and did not register. Since
# widgets/ moved to 2.5, where its own dependencies already put it, that edge IS an upward
# include and tools/include_graph.py catches it too. This check stays: it is the one that names
# the rule, and it is cheaper to read than a violation count.
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


# 5. src/caches owns its instances, and does not call back up the stack.
#
# The three caches (image, mipmap, pixelpipe) used to hang off darktable_t, so their structs
# were reachable from anything that included darktable.h. Each owns its instance now and
# implements its own accessor.
#
# The second count is the one that matters structurally: a cache is storage, and storage that
# calls dt_control_log() or raises a signal has the dependency backwards. Those go through the
# handlers dt_dev_pixelpipe_cache_set_handlers() installs, so the count is zero and staying
# there is the check. develop/imageop.h and develop/pixelpipe_hb.h are excluded by name: the
# pixelpipe cache genuinely needs both (it reads `module->op`, calls `module->name()`, and keys
# entries on pipe hashes), so counting them would only ever read as noise.
#
# The upcall baseline is 10, not 0, and it is a RATCHET rather than a boundary: image_cache.c
# and mipmap_cache.c arrived from common/ carrying these, and they are the next tranche --
#
#   image_cache.c  : control.h, signal.h, supervisor.h                            (3)
#   mipmap_cache.c : supervisor.h, imageio_core.h, imageio_jpeg.h,
#                    imageio_module.h, control/jobs.h, develop/imageop_math.h     (6)
#
# The mipmap cache decoding images and scheduling jobs is a real design question, not a stray
# include, so it gets its own pass. pixelpipe_cache.c is already at zero here.
caches_global_baseline=0
caches_upcall_baseline=9

caches_global_now=$(grep -rn 'darktable\.\(image_cache\|mipmap_cache\|pixelpipe_cache\)\b' \
                    src/ --include='*.c' --include='*.h' --include='*.cc' 2>/dev/null | wc -l)
caches_upcall_now=$(grep -rnE '^#include "(control|develop|gui|libs|views|iop|imageio)/' src/caches/ \
                    2>/dev/null | grep -cv 'develop/imageop\.h\|develop/pixelpipe_hb\.h')

echo "caches:        ${caches_global_now} references to the caches on darktable_t (baseline ${caches_global_baseline}),"
echo "               ${caches_upcall_now} includes from a higher layer (baseline ${caches_upcall_baseline})."

if [ "${caches_global_now}" -gt "${caches_global_baseline}" ] || [ "${caches_upcall_now}" -gt "${caches_upcall_baseline}" ]; then
  echo "caches: a count ROSE. A cache owns its instance and announces through the handlers"
  echo "        (dt_dev_pixelpipe_cache_set_handlers()); it does not call the control loop."
  findings=$((findings + 1))
elif [ "${caches_global_now}" -lt "${caches_global_baseline}" ] || [ "${caches_upcall_now}" -lt "${caches_upcall_baseline}" ]; then
  echo "caches: a count fell -- lower the baseline in this script, in the same commit."
  findings=$((findings + 1))
fi


# 6. src/database owns the connection, and hands it out to fewer people every time.
#
# The first two counts here BEGAN as ratchets on a large number -- 353 handles across 42
# files -- rather than boundaries at zero, because the connection could not be sealed in one
# change and this script is what kept the sealing monotonic while it happened. They are now
# both at ZERO. Treat them as boundaries: nothing outside src/database holds the connection,
# and the ratchet has become a wall.
#
#   handle_escapes : call sites of dt_database_get_sqlite3_global() outside src/database.
#                    Each is a translation unit holding a raw `sqlite3 *` the module cannot
#                    account for, cannot serialise against a close, and cannot trace. They
#                    are also exactly what makes swapping workspaces at runtime
#                    unimplementable: nothing can wait for a connection somebody else is
#                    still using. Every query that moves into a repository under
#                    src/database/ takes one off this number.
#
#   sql_consumers  : files including database/sql_debug.h outside src/database. The header
#                    is scaffolding and says so. Now zero, so it can be moved out of the
#                    public include path and its macros made private to the repositories --
#                    a separate change, since it touches every repository.
#
# The last two are real boundaries and are at their floor:
#
#   conf_debug     : dt_conf_*/dt_get_debug_flags() calls inside src/database. Zero. The
#                    maintenance and snapshot policy crosses as dt_database_settings_t and
#                    the SQL trace flag is told to dt_database_open(). A module that reads
#                    a user preference at the point of use has no answerable lifecycle.
#
#   upcalls        : includes from a higher layer. One, and it is named: the v1 -> v2
#                    iop-order schema migration in database.c calls
#                    dt_ioppr_get_iop_order_list_version() to rewrite main.history.iop_order.
#                    That migration genuinely needs the module priority table; it is not a
#                    stray include.
database_handle_escapes_baseline=0
database_sql_consumers_baseline=0
database_conf_debug_baseline=0
database_upcalls_baseline=1

database_handle_escapes_now=$(grep -rn 'dt_database_get_sqlite3_global()' \
                              src/ --include='*.c' --include='*.h' --include='*.cc' 2>/dev/null \
                              | grep -cv '^src/database/')
database_sql_consumers_now=$(grep -rln '#include "database/sql_debug.h"' \
                             src/ --include='*.c' --include='*.h' --include='*.cc' 2>/dev/null \
                             | grep -cv '^src/database/')
database_conf_debug_now=$(grep -rnE 'dt_conf_(get|set)|dt_get_debug_flags\(' src/database/ \
                          --include='*.c' --include='*.h' 2>/dev/null | wc -l)
database_upcalls_now=$(grep -rnE '^#include "(control|gui|libs|views|iop|imageio|widgets)/' src/database/ 2>/dev/null | wc -l)
database_upcalls_now=$((database_upcalls_now + $(grep -rcE '^#include "develop/' src/database/*.c src/database/*.h 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')))

echo "database:      ${database_handle_escapes_now} sqlite3 handles handed out (ratchet, baseline ${database_handle_escapes_baseline}),"
echo "               ${database_sql_consumers_now} files writing SQL outside the module (ratchet, baseline ${database_sql_consumers_baseline}),"
echo "               ${database_conf_debug_now} conf/debug reads inside it (baseline ${database_conf_debug_baseline}),"
echo "               ${database_upcalls_now} includes from a higher layer (baseline ${database_upcalls_baseline})."

if [ "${database_handle_escapes_now}" -gt "${database_handle_escapes_baseline}" ] \
   || [ "${database_sql_consumers_now}" -gt "${database_sql_consumers_baseline}" ]; then
  echo "database: a count ROSE. New SQL belongs in a repository under src/database/, behind a"
  echo "          named function -- not at a new call site holding the connection. See"
  echo "          src/database/README.md."
  findings=$((findings + 1))
elif [ "${database_handle_escapes_now}" -lt "${database_handle_escapes_baseline}" ] \
     || [ "${database_sql_consumers_now}" -lt "${database_sql_consumers_baseline}" ]; then
  echo "database: a ratchet fell -- that is the point. Lower the baseline in this script, in"
  echo "          the same commit, so the ground gained cannot be given back."
  findings=$((findings + 1))
fi

if [ "${database_conf_debug_now}" -gt "${database_conf_debug_baseline}" ] \
   || [ "${database_upcalls_now}" -gt "${database_upcalls_baseline}" ]; then
  echo "database: the module started reading conf, the debug flags, or a higher layer. Session"
  echo "          constants are told to dt_database_open(); user preferences cross as"
  echo "          dt_database_settings_t."
  findings=$((findings + 1))
elif [ "${database_conf_debug_now}" -lt "${database_conf_debug_baseline}" ] \
     || [ "${database_upcalls_now}" -lt "${database_upcalls_baseline}" ]; then
  echo "database: a count fell -- lower the baseline in this script, in the same commit."
  findings=$((findings + 1))
fi

# 7. src/metadata is closed: what a photograph says about itself, and nothing about the
#    application showing it.
#
#   upcalls  : includes from a higher layer. ZERO, and it is a boundary, not a ratchet --
#              the module reached this state in the commit that created it. Ratings and
#              colour labels used to call dt_control_log()/dt_toast_log() and tags used to
#              raise DT_SIGNAL_TAG_CHANGED, all of which put control/ (layer 3) inside a
#              layer-1 module. They go out through the handlers in metadata/notify.h now,
#              installed by src/darktable.c. A new one belongs there too; where a message
#              appears is not a decision this module can make, and a module that cannot be
#              built without the control loop cannot be tested without it either.
metadata_upcalls_baseline=0

metadata_upcalls_now=$(grep -rcE '^#include "(control|gui|libs|views|iop|imageio|widgets|develop|apps)/' \
                       src/metadata/ 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')

echo "metadata:      ${metadata_upcalls_now} includes from a higher layer (baseline ${metadata_upcalls_baseline})."

if [ "${metadata_upcalls_now}" -gt "${metadata_upcalls_baseline}" ]; then
  echo "metadata: the module reached up into the application. Outbound notifications go"
  echo "          through metadata/notify.h -- state the fact, let the caller present it."
  findings=$((findings + 1))
fi


# 8. src/history is closed: the history stack, styles and presets. It reached up for three
#    different reasons and each was inverted rather than tolerated -- dt_control_log() and
#    the signals through history/notify.h, dt_lib_presets_can_autoapply() through the
#    resolver in history/presets.h, and dt_iop_get_localized_name() through the one in
#    history/history.h. A history item holds a dt_iop_module_t *, so the pipeline half of
#    this code genuinely belongs at layer 5 and stays there; what is in this directory is
#    the half that does not.
history_upcalls_baseline=0

history_upcalls_now=$(grep -rcE '^#include "(control|gui|libs|views|iop|imageio|widgets|develop|apps)/' \
                      src/history/ 2>/dev/null | awk -F: '{s+=$2} END {print s+0}')

echo "history:       ${history_upcalls_now} includes from a higher layer (baseline ${history_upcalls_baseline})."

if [ "${history_upcalls_now}" -gt "${history_upcalls_baseline}" ]; then
  echo "history: the module reached up into the application. Messages and notifications go"
  echo "         through history/notify.h; a question only the application can answer gets"
  echo "         a resolver, as presets.h and history.h already do."
  findings=$((findings + 1))
fi

# ---------------------------------------------------------------------------------------
# 9. src/develop/masks is NOT closed. This is the ratchet that closes it.
#
# Unlike colorprofiles, opencl, caches and database above, this one starts far from zero and
# is expected to stay non-zero for several releases. It is here because the masks audit
# (issue #1299) found the leak is CONCENTRATED rather than diffuse -- five files account for
# every direct struct access outside the module -- which is what makes draining it tractable,
# and because the same defect kept coming back while nothing counted it: the rule that the
# pipeline must resolve shapes through pipe->forms and never the live dev->forms was violated
# and re-fixed four separate times, once three lines below the comment stating it.
#
# The counts here are what the enclosure plan's later phases drain. Every accessor added to
# the module and every caller moved onto it takes one off a number, and the number may then
# only be lowered, never raised -- so a phase that half-lands cannot silently un-land.
#
# What is counted, and why each is counted THIS way rather than more ambitiously:
#
#   includes       Files outside src/develop/masks/ that include the module's two public
#                  headers. develop/blend.h is one of them, which is how the whole masks
#                  surface reaches every IOP in the tree; that single edge is worth more than
#                  the other twenty put together and it is the one P2 removes.
#
#   members        Direct reads of a masks-owned struct member from outside the module. The
#                  member list is CURATED on purpose: it holds only names that no other struct
#                  in this tree uses (formid, form_dragging, creation_formids, ...) and leaves
#                  out the ambiguous ones a masks form shares with half the codebase (points,
#                  type, name, state, opacity). That undercounts -- the audit's full census
#                  found ~385 by reading declarations, this finds far fewer -- and undercounting is
#                  the right error for a gate. A ratchet that moves when somebody renames an
#                  unrelated `->state` is a ratchet that gets switched off. Every match this
#                  does report is real: they land in exactly five files, and no other.
#
#   writes         The same members, assigned to. A write from outside is the sharper half:
#                  it is a caller mutating a refcounted, copy-on-write object without going
#                  through dt_masks_cow_touch(), which is how a snapshot ends up observing a
#                  half-rewritten form. Every remaining match is a real one: the single false
#                  positive this count used to carry -- supervisor.c's own event struct, whose
#                  `formid` field matched on the left of `e->formid = form->formid` -- is gone,
#                  because that field was renamed when the file was converted.
#
#   allocations    malloc/calloc of a masks type outside the module: four places build a
#                  group-membership entry or a circle node by hand and rely on the module's
#                  free path to release it. An allocator/deallocator pact held together by
#                  convention.
#
#   forms          Direct touches of ->forms / ->allforms outside the module: the live GUI
#                  list, the pipe's refcounted snapshot and the per-history-item snapshot, all
#                  reached as plain GLists. This is the count behind the bug that came back
#                  four times, and it only reaches zero when resolving a shape is something
#                  callers ask the module to do.
#
#   rows           Files outside the module that name dt_masks_form_group_t at all -- the
#                  group-membership row. A row cannot copy-on-write: it is memory owned by the
#                  group, so whoever holds one must have touched the group first, and must not
#                  keep holding it afterwards because the touch replaces it. Every caller that
#                  resolves one is a caller that has to get both halves right by hand, and the
#                  opacity sliders are the proof they do not: they touched once, when the menu
#                  was built, then wrote through the row for the whole drag while committing
#                  history at every step -- so from the second step on they edited the very
#                  snapshots that were supposed to be frozen. The module's own interface headers
#                  are excluded; naming the type there is the design, not the leak.
masks_include_baseline=19
masks_gui_include_baseline=11
masks_member_baseline=83
masks_write_baseline=20
masks_alloc_baseline=1
masks_forms_baseline=76
masks_row_baseline=35

# Members no other struct in the tree uses. Keep it that way: adding an ambiguous name here
# buys a bigger number and loses the gate.
masks_members='formid|parentid|form_dragging|source_dragging|form_selected|border_selected'
masks_members="${masks_members}|source_selected|pivot_selected|group_selected|form_visible"
masks_members="${masks_members}|creation_formids|creation_module|creation_type|guipoints"
masks_members="${masks_members}|guipoints_count|uses_bezier_points_layout|gravity_center_valid"
masks_members="${masks_members}|node_dragging|handle_dragging|seg_dragging|node_selected_idx"
masks_members="${masks_members}|handle_border_selected|handle_border_hovered"

# Drop whole-line comments so a count cannot move because somebody described the code.
masks_strip() { grep -vE ':[0-9]+:[[:space:]]*(\*|//)'; }

masks_include_now=$(grep -rn '^[ \t]*#[ \t]*include[ \t]*"develop/masks\.h"' \
                    src/ --include='*.c' --include='*.h' --include='*.cc' 2>/dev/null \
                    | grep -cv '^src/develop/masks/')
masks_gui_include_now=$(grep -rn '^[ \t]*#[ \t]*include[ \t]*"develop/masks_gui\.h"' \
                        src/ --include='*.c' --include='*.h' --include='*.cc' 2>/dev/null \
                        | grep -cv '^src/develop/masks/')
masks_member_now=$(grep -rnE "\->(${masks_members})\b" src/ --include='*.c' --include='*.cc' 2>/dev/null \
                   | grep -v '^src/develop/masks/' | masks_strip | wc -l)
masks_write_now=$(grep -rnE "\->(${masks_members})[[:space:]]*(=[^=]|\|=|&=|\+=|-=)" \
                  src/ --include='*.c' --include='*.cc' 2>/dev/null \
                  | grep -v '^src/develop/masks/' | masks_strip | wc -l)
masks_alloc_now=$(grep -rnE '\b(malloc|calloc|g_malloc[0-9n]*|g_new[0-9]*)[[:space:]]*\(' \
                  src/ --include='*.c' --include='*.cc' 2>/dev/null \
                  | grep -v '^src/develop/masks/' \
                  | grep -cE 'dt_masks_(form_t|form_gui_t|form_group_t|node_|anchor_|point_)')
masks_forms_now=$(grep -rnE '\->(forms|allforms)\b' src/ --include='*.c' --include='*.cc' 2>/dev/null \
                  | grep -v '^src/develop/masks/' | masks_strip | wc -l)

# The module's own public headers declare the type; only its CONSUMERS are leakage.
masks_own_headers='^src/develop/masks/|^src/develop/masks\.h|^src/develop/masks_types\.h'
masks_own_headers="${masks_own_headers}|^src/develop/masks_group\.h|^src/develop/masks_gui\.h"
masks_row_now=$(grep -rnE '\bdt_masks_form_group_t\b' \
                src/ --include='*.c' --include='*.cc' --include='*.h' 2>/dev/null \
                | grep -vE "${masks_own_headers}" | masks_strip | wc -l)

echo "masks:         ${masks_include_now} include masks.h, ${masks_gui_include_now} include masks_gui.h" \
     "(baselines ${masks_include_baseline}, ${masks_gui_include_baseline}),"
echo "               ${masks_member_now} external struct-member reads, ${masks_write_now} of them writes" \
     "(baselines ${masks_member_baseline}, ${masks_write_baseline}),"
echo "               ${masks_alloc_now} external allocations, ${masks_forms_now} direct ->forms touches" \
     "(baselines ${masks_alloc_baseline}, ${masks_forms_baseline}),"
echo "               ${masks_row_now} external mentions of a membership row" \
     "(baseline ${masks_row_baseline})."

masks_findings=0
masks_check() { # name now baseline
  if [ "$2" -gt "$3" ]; then
    echo "masks: $1 ROSE ($3 -> $2). The module is being enclosed, not extended: ask it for what"
    echo "       you need instead of reaching into a dt_masks_form_t. See issue #1299."
    masks_findings=$((masks_findings + 1))
  elif [ "$2" -lt "$3" ]; then
    echo "masks: $1 fell ($3 -> $2). Lower the baseline in this script, in the same commit, so"
    echo "       the ground you took cannot be given back."
    masks_findings=$((masks_findings + 1))
  fi
}
masks_check "masks.h includers"    "${masks_include_now}"     "${masks_include_baseline}"
masks_check "masks_gui.h includers" "${masks_gui_include_now}" "${masks_gui_include_baseline}"
masks_check "struct-member reads"  "${masks_member_now}"      "${masks_member_baseline}"
masks_check "struct-member writes" "${masks_write_now}"       "${masks_write_baseline}"
masks_check "external allocations" "${masks_alloc_now}"       "${masks_alloc_baseline}"
masks_check "direct ->forms"       "${masks_forms_now}"       "${masks_forms_baseline}"
masks_check "membership rows named externally" "${masks_row_now}" "${masks_row_baseline}"
findings=$((findings + masks_findings))


# ---------------------------------------------------------------------------------------
# 4. How much of the pixel engine still names a toolkit -- the GTK->Qt door.
#
# This measures a DIFFERENT property from tools/check_layering.sh, and the difference is the
# whole reason it exists. That one measures dependency ORDER: who may include whom. This one
# measures toolkit-FREEDOM: whether a translation unit names GtkWidget, GdkEvent, cairo_t or
# their headers at all.
#
# They came apart when widgets/ moved from layer 4 to 2.5. That move is correct on the order
# axis -- widgets/ depends only on system/, common/, metadata/ and pixel/, so it is a leaf
# library that happens to be written against GTK, and the move costs zero new violations. But
# it also makes develop/ -> widgets/ a DOWNWARD include, so 50 edges that the layering ratchet
# used to count stopped counting, without one line of GTK leaving the pixel engine. Left alone
# that is a metric improving while the thing it stood for does not, which is worse than no
# metric. So the property those 50 edges were standing in for gets its own gate here.
#
# It is a ratchet because the tranche that lowers it is the IOP operator/panel split: an IOP is
# currently one file holding both the pixel math and its GTK panel, and 97 of src/iop's 164
# files name a toolkit type. Each split module lowers the iop count by one. src/pixel (0/39),
# src/caches (0/11) and src/database (0/31) are already free and are pinned at zero so they
# stay that way.
#
# Files, not occurrences: the file is the unit that gets split, and one GtkWidget in a header
# is as disqualifying as forty in a callback.
#
# KNOWN BLIND SPOT, stated here so this number is not read as more than it is: every src/iop
# translation unit is compiled with GTK already in scope, because src/iop/CMakeLists.txt:5-6
# does `add_definitions(-include iop/iop_api.h)` and develop/iop_api.h:44-45 includes
# <cairo/cairo.h> and <gtk/gtk.h> under FULL_API_H. No file partition produces a genuinely
# toolkit-free IOP object file until that force-include is dealt with, and tools/include_graph.py
# cannot see it either -- its INCLUDE_RE (:24) matches the quote form only, so angle-bracket
# system includes are outside the graph entirely. So `toolkit: iop/ 0' would mean "no IOP file
# NAMES a toolkit symbol", not "IOP objects no longer link against GTK". Both are worth having;
# they are not the same claim.
toolkit_develop_baseline=20
toolkit_iop_baseline=97
toolkit_imageio_baseline=12
toolkit_pixel_baseline=0
toolkit_caches_baseline=0
toolkit_database_baseline=0

# Comments are stripped first: a file that only MENTIONS GTK in prose is toolkit-free, and the
# doc comments in this tree talk about GTK constantly.
count_toolkit() {
  ${PYTHON:-python3} - "$1" <<'PYEOF'
import os, re, sys
ident = re.compile(r'\b(?:Gtk[A-Z]\w*|Gdk[A-Z]\w*|GTK_[A-Z]\w*|GDK_[A-Z]\w*|cairo_[a-z]\w*|PangoLayout\w*)\b')
inc = re.compile(r'^\s*#\s*include\s*[<"](?:gtk/|gdk/|cairo|pango)', re.M)
hits = 0
for root, _, names in os.walk(sys.argv[1]):
    parts = root.split(os.sep)
    if 'external' in parts or 'attic' in parts:
        continue
    for n in names:
        if not n.endswith(('.c', '.h', '.cc', '.cpp', '.hpp')):
            continue
        try:
            text = open(os.path.join(root, n), encoding='utf-8', errors='replace').read()
        except OSError:
            continue
        text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
        text = re.sub(r'//[^\n]*', '', text)
        if ident.search(text) or inc.search(text):
            hits += 1
print(hits)
PYEOF
}

toolkit_findings=0
for module in develop iop imageio pixel caches database; do
  now=$(count_toolkit "src/${module}")
  eval "base=\${toolkit_${module}_baseline}"
  printf 'toolkit:       %-9s %3d files name a toolkit type (baseline %d).\n' "${module}/" "${now}" "${base}"
  if [ "${now}" -gt "${base}" ]; then
    echo "toolkit: ${module}/ gained a file that names GTK, GDK, cairo or pango. The pixel engine"
    echo "         and the params engine must be portable to another toolkit; put the widget code"
    echo "         in the panel half and keep the operator half free of it."
    toolkit_findings=$((toolkit_findings + 1))
  elif [ "${now}" -lt "${base}" ]; then
    echo "toolkit: ${module}/ fell ${base} -> ${now}. Lower toolkit_${module}_baseline in this"
    echo "         script, in the same commit, so the gain is locked in."
    toolkit_findings=$((toolkit_findings + 1))
  fi
done
findings=$((findings + toolkit_findings))

if [ "${findings}" -gt 0 ]; then
  exit 1
fi

echo "OK: src/system is closed, src/widgets does not reach into gui/; colorprofiles, opencl, caches, database, metadata and history held; masks did not leak further; the pixel engine's toolkit surface did not grow."
exit 0
