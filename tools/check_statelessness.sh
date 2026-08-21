#!/usr/bin/env bash
#
# What each module may HOLD. The companion to check_module_boundaries.sh, which covers what
# each module may INCLUDE.
#
# Measured from the compiled objects by tools/statelessness_audit.py -- the ELF section a
# symbol lands in, not a reading of the source. That needs a build whose objects still carry
# symbol tables, so this runs where a Debug build exists (CI builds Debug; an LTO Release
# emits bytecode `objdump` cannot read).
#
# Three rules, each earned:
#
#   src/system, src/math   zero state, direct or reached. This is the whole point: once a
#                          directory is known stateless, anything built only from it is
#                          stateless too, and nobody has to re-derive that per file.
#
#   src/widgets            no state outside its two named registries, widget_settings.c and
#                          accelerators.c, EXCEPT GObject type registration. That exception is
#                          not a compromise -- a GObject type must be registered once per
#                          process and its id cached, so a directory containing GTK widget
#                          classes can never be stateless in the linker's sense. Ten of its
#                          files hold nothing else.
#
# Usage:
#   BUILD_DIR=build-debug tools/check_statelessness.sh

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 2
BUILD_DIR="${BUILD_DIR:-build-debug}"

if [ ! -d "${BUILD_DIR}" ]; then
  echo "error: no ${BUILD_DIR} -- set BUILD_DIR to a Debug build directory" >&2
  exit 2
fi
if ! command -v objdump >/dev/null 2>&1; then
  echo "error: objdump not found" >&2
  exit 2
fi

"${PYTHON:-python3}" - "${BUILD_DIR}" <<'PYEOF'
import json, re, subprocess, sys

build = sys.argv[1]
out = subprocess.run([sys.executable, "tools/statelessness_audit.py", "--build", build, "--json"],
                     capture_output=True, text=True)
if out.returncode != 0:
    sys.exit(f"error: statelessness_audit.py failed:\n{out.stderr[:800]}")
data = json.loads(out.stdout)

# A function-local static is named differently by each compiler, and the gate has to see past
# that or it reports GObject boilerplate as real state on one compiler and not the other:
#
#   gcc    static_g_define_type_id.1        <variable>.<n>
#   clang  dt_bh_get_type.static_g_define_type_id     <function>.<variable>
#
# Reduce to the variable name, then match. (Found the hard way: the gate passed on three local
# GCC builds and failed the LLVM job.)
def bare(sym):
    s = re.sub(r"\.\d+$", "", sym)              # gcc's disambiguating suffix
    return s.rsplit(".", 1)[-1]                  # clang's function prefix

# Emitted by G_DEFINE_TYPE / g_type_register_static / g_signal_new. Registering a type once
# per process and caching its id is what defining a GTK widget class is.
GOBJECT = re.compile(r"(^static_g_define_type_id$|_private_offset$|_parent_class$|_type$|"
                     r"_info$|^_?signals?$|_signal$)")
REGISTRIES = {"src/widgets/widget_settings.c", "src/widgets/accelerators.c"}

findings = []
for f, v in sorted(data.items()):
    own = v.get("own_state") or []
    if f.startswith(("src/system/", "src/math/")):
        if v.get("stateful"):
            why = f"holds {', '.join(own[:4])}" if own else \
                  f"reaches state via {' -> '.join((v.get('chain') or ['?'])[1:3])}"
            findings.append(f"{f}: {why}")
    elif f.startswith("src/widgets/") and f not in REGISTRIES:
        real = [s for s in own if not GOBJECT.search(bare(s))]
        if real:
            findings.append(f"{f}: holds {', '.join(real[:4])} "
                            f"(not GObject registration)")

if not findings:
    print("OK: src/system and src/math hold no state; src/widgets holds none outside its two "
          "registries.")
    sys.exit(0)

print("Statelessness rules broken:\n")
for x in findings:
    print(f"  {x}")
print("""
src/system and src/math are the tree's stateless foundation. Code elsewhere infers
statelessness from the directory rather than checking, so one exception costs everyone that
inference.

src/widgets keeps its state in exactly two registries: widget_settings.c for toolkit settings
the application pushes in, accelerators.c for the accel registry. A widget that needs to
remember something belongs in one of those, not in a static of its own.
""")
sys.exit(1)
PYEOF
