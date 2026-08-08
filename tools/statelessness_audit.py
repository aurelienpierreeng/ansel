#!/usr/bin/env python3
"""Classify every translation unit as stateless or stateful, and say why.

"Stateless" here means: calling it twice with the same arguments does the same thing, because
it reads and writes nothing that outlives the call. That is the property that lets a module be
reused, tested, threaded, or ported without dragging the application with it -- and the whole
point of sorting the tree by it is that checking becomes mechanical. Once a directory is known
stateless, anything built only from it is stateless too, and nobody has to re-derive that.

Measured from the linker's own view, not from reading source: `nm` on the compiled objects.

  DIRECT   the object defines mutable storage at file scope -- `d`/`b` for a static, `D`/`B`
           for a global. Read-only data (`r`/`R`) is not state.
  INDIRECT the object calls a symbol defined by an object that has state. Transitive, so a
           module three hops from a global is still reported, with the chain that gets there.

Needs a build directory whose objects still carry symbol tables: an LTO build (`-flto
-fno-fat-lto-objects`, which this tree uses in Release) emits bytecode that `nm` cannot read.
Use a Debug build.

Usage:
    tools/statelessness_audit.py [--build DIR] [--dir src/system] [--json]
    tools/statelessness_audit.py --chains dt_screen_dpi     # why is this one stateful?
"""

import collections
import json
import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Read the ELF section, not nm's collapsed type letter. `static const char *const KEY = "..."`
# is fully const, but because it holds a pointer the linker must relocate, it is emitted to
# .data.rel.ro -- which nm reports as `d`, indistinguishable from real mutable state. Judging
# by letter therefore accuses every const table of pointers in the tree.
#
# .data.rel.ro* is read-only once the dynamic linker has finished, so it is not state.
MUTABLE_SECTIONS = re.compile(r"^\.(data|bss|sdata|sbss|tdata|tbss)(\.|$)")
READONLY_SECTIONS = re.compile(r"^\.(rodata|data\.rel\.ro)(\.|$)")

# Compiler-emitted symbols that are not program state.
NOISE = re.compile(r"^(__func__|__PRETTY_FUNCTION__|CSWTCH|__gcov|__profc|__profd|__llvm|"
                   r"\.L|_ZZ.*E19__PRETTY_FUNCTION__|__odr_asan|__const_|"
                   # C++ exception-handling bookkeeping the compiler emits per object.
                   r"DW\.ref\.|__gxx_personality|__dso_handle|_ZSt|__cxa_)")


def objects(build_dir):
    out = []
    for dirpath, _, filenames in os.walk(build_dir):
        for fn in filenames:
            if fn.endswith(".o"):
                out.append(os.path.join(dirpath, fn))
    return out


def source_of(obj, build_dir):
    """Map an object path back to its source.

    Two shapes, and getting only the first is how src/widgets -- an entire CMake target --
    silently reported zero translation units:

      build/src/CMakeFiles/lib_ansel.dir/common/bar.c.o        -> src/common/bar.c
      build/src/widgets/CMakeFiles/ansel_widgets.dir/bar.c.o   -> src/widgets/bar.c

    The general rule covers both: drop the build directory, then drop the
    `CMakeFiles/<target>.dir/` component wherever it sits, and what is left is the path
    relative to the project root.
    """
    rel = os.path.relpath(obj, build_dir)
    rel = re.sub(r"CMakeFiles/[^/]*\.dir/", "", rel)
    if rel.endswith(".o"):
        rel = rel[:-2]
    if os.path.isfile(os.path.join(REPO, rel)):
        return rel
    # Generated sources (introspection_*.c) have no counterpart in the tree; ignore them.
    return None


def scan(build_dir):
    defines = {}                                   # symbol -> source file
    state_syms = collections.defaultdict(list)     # source file -> [state symbol]
    calls = collections.defaultdict(set)           # source file -> {symbol}

    objs = objects(build_dir)
    if not objs:
        sys.exit(f"error: no .o files under {build_dir}")

    # objdump -t prints:  value flags section \t size name
    # flags contain spaces, so anchor on the tab that precedes the size.
    # objdump prints a visibility marker before the name for non-default visibility:
    #   ... .data.rel.local.DW.ref...  0000000000000008 .hidden DW.ref.__gxx_personality_v0
    # Capturing the first token there yields ".hidden" as the symbol name.
    row = re.compile(r"^[0-9a-fA-F]+\s+(?P<flags>.{7})\s+(?P<section>\S+)\t[0-9a-fA-F]+\s+"
                     r"(?:\.(?:hidden|protected|internal)\s+)?(?P<name>\S+)")

    for obj in objs:
        src = source_of(obj, build_dir)
        if not src:
            continue
        try:
            out = subprocess.run(["objdump", "-t", obj], capture_output=True, text=True).stdout
        except FileNotFoundError:
            sys.exit("error: objdump not found")
        for line in out.splitlines():
            m = row.match(line)
            if not m:
                continue
            name, section, flags = m.group("name"), m.group("section"), m.group("flags")
            if NOISE.match(name):
                continue
            if section == "*ABS*" or name == section:
                continue          # the file symbol, and objdump's one symbol per section
            if section == "*UND*":
                calls[src].add(name)
            elif READONLY_SECTIONS.match(section):
                # Checked BEFORE the mutable test: `.data.rel.ro.local` starts with `.data`
                # and would otherwise be accused of being state.
                defines.setdefault(name, src)
            elif MUTABLE_SECTIONS.match(section):
                defines[name] = src
                state_syms[src].append(name)
            elif section.startswith(".text"):
                defines.setdefault(name, src)
    return defines, state_syms, calls


def split_header_defined(state_syms):
    """A mutable symbol defined in more than one object came from a header.

    `static const char *dt_supported_extensions[]` in config.h and `loaders_info[]` in
    common/image.h are emitted into every translation unit that includes them, so they show up
    as "own state" for 170 files that never heard of them. They are still mutable storage --
    `const char *` makes the pointee const, not the array -- and each copy is independently
    writable, which is worth fixing at the header. But they are the header's problem, not each
    includer's, and counting them per-includer buries every real finding.
    """
    seen = collections.Counter()
    for syms in state_syms.values():
        seen.update(set(syms))

    # Appearing in several objects is necessary but not sufficient: `_handler` and
    # `_module_usage` are just names two unrelated files both chose for a static. Confirm
    # against the headers, so only a symbol actually defined in one is excused.
    header_text = []
    for dirpath, dirnames, filenames in os.walk(os.path.join(REPO, "src")):
        dirnames[:] = [d for d in dirnames if d not in ("external", "build")]
        for fn in filenames:
            if fn.endswith((".h", ".hpp", ".cmake.h")):
                try:
                    with open(os.path.join(dirpath, fn), errors="ignore") as fh:
                        header_text.append(fh.read())
                except OSError:
                    pass
    headers = "\n".join(header_text)

    header_defined = set()
    for sym, n in seen.items():
        if n < 2:
            continue
        bare = re.sub(r"^_ZL\d+", "", sym)          # C++ mangling for a file-local
        bare = re.sub(r"\.\d+$", "", bare)          # gcc's suffix for a function-local static
        patterns = [
            # static const char *name[] = ... / static int name = ...
            r"^\s*(?:static\s+)?[A-Za-z_][\w \t*]*\b" + re.escape(bare) + r"\s*(?:\[|=)",
            # } name[N] = ...  -- a struct-array definition closing a typedef
            r"^\s*}\s*" + re.escape(bare) + r"\s*\[",
        ]
        if any(re.search(pat, headers, re.M) for pat in patterns):
            header_defined.add(sym)
    own = {src: [s for s in syms if s not in header_defined]
           for src, syms in state_syms.items()}
    return {k: v for k, v in own.items() if v}, sorted(header_defined)


def propagate(defines, state_syms, calls):
    """Stateful = has state, or reaches something that has. Returns file -> chain."""
    stateful = {src: [src] for src in state_syms}
    changed = True
    while changed:
        changed = False
        for src, syms in calls.items():
            if src in stateful:
                continue
            for sym in syms:
                owner = defines.get(sym)
                if owner and owner in stateful and owner != src:
                    stateful[src] = [src] + stateful[owner]
                    changed = True
                    break
    return stateful


def main():
    build = "build-debug"
    if "--build" in sys.argv:
        build = sys.argv[sys.argv.index("--build") + 1]
    only = None
    if "--dir" in sys.argv:
        only = sys.argv[sys.argv.index("--dir") + 1]

    defines, raw_state, calls = scan(os.path.join(REPO, build))
    state_syms, header_defined = split_header_defined(raw_state)
    stateful = propagate(defines, state_syms, calls)

    files = sorted(set(list(calls) + list(state_syms)))
    if only:
        files = [f for f in files if f.startswith(only)]

    if "--json" in sys.argv:
        print(json.dumps({f: {"stateful": f in stateful,
                              "own_state": sorted(state_syms.get(f, [])),
                              "chain": stateful.get(f, [])} for f in files}, indent=2))
        return 0

    direct = [f for f in files if state_syms.get(f)]
    indirect = [f for f in files if f in stateful and not state_syms.get(f)]
    clean = [f for f in files if f not in stateful]

    scope = only or "src/"
    print(f"{len(files)} translation unit(s) under {scope}: "
          f"{len(clean)} stateless, {len(direct)} with own state, {len(indirect)} reaching state\n")

    if header_defined and not only:
        print(f"--- MUTABLE DATA DEFINED IN HEADERS ({len(header_defined)}) ---")
        print("    One writable copy per including translation unit. Not any single file's")
        print("    state; fix at the header (usually a missing second const).")
        for sym in header_defined:
            print(f"  {sym}")
        print()

    if direct:
        print("--- OWN STATE (mutable storage at file scope) ---")
        for f in direct:
            syms = state_syms[f]
            print(f"  {f}  ({len(syms)}): {', '.join(sorted(syms)[:6])}"
                  + (" ..." if len(syms) > 6 else ""))
        print()
    if indirect:
        print("--- REACHES STATE (through what it calls) ---")
        for f in indirect:
            chain = stateful[f]
            print(f"  {f}")
            print(f"      via {' -> '.join(chain[1:4])}" + (" ..." if len(chain) > 4 else ""))
        print()
    if clean:
        print(f"--- STATELESS ({len(clean)}) ---")
        for f in clean:
            print(f"  {f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
