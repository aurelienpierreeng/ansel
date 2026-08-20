#!/usr/bin/env python3
#
#   This file is part of Ansel,
#   Copyright (C) 2026 Aurélien PIERRE.
#
#   Ansel is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#
#   Ansel is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with Ansel.  If not, see <http://www.gnu.org/licenses/>.

"""Generate the glue that binds statically-linked IOP modules into the application.

IOP modules used to be one shared object each, discovered by scanning a directory and
bound with g_module_symbol(). That indirection bought nothing -- the module set is fixed
at build time, and src/develop/iop_order.c and src/develop/geometry/geometry.c both carry
a hardcoded census of module names that a module on disk cannot join -- while costing a
dlopen per module at every startup and turning a stale .so left over from an experimental
branch into a refusal to start (see dt_ioppr_check_so_iop_order).

Two things have to be generated to replace it:

  * <module>_present.h -- DT_MODULE_HAS_<fn>, 0 or 1, for EVERY name in the API. This is
    the question g_module_symbol() used to answer at runtime: does this module define this
    entry point at all? It is answered here by running the real compiler's preprocessor
    over the module's real sources with the module's real flags. That is not
    over-engineering: a regex over the raw source gets 90 of 91 modules right and then
    trips over iop/censorize.c, which hides four functions behind `#if FALSE', and writes
    name() with its return type on the previous line. Only the preprocessor knows what
    HAVE_OPENCL and `#if FALSE' resolve to, and it costs one -E pass per source.

  * <module>_static.c -- the function that fills this module's dt_iop_module_so_t, expanded
    from the same X-macro list in iop/iop_api.h that used to drive the g_module_symbol()
    calls, inside the module's own translation unit so the plain API names resolve to THIS
    module's asm-labelled symbols.

plus one registry naming every module, compiled into lib_ansel.
"""

import argparse
import os
import re
import subprocess
import sys

# Emitted into every module's translation unit by DT_MODULE(), not declared through the
# X-macro list, but colliding across modules exactly like the API does.
VERSION_FUNCTIONS = ("dt_module_dt_version", "dt_module_mod_version")


def _usable_flags(flags):
    """Drop what the preprocessor cannot use, keep the rest verbatim.

    CMake's COMPILE_DEFINITIONS carries at least one entry that already includes its own
    `-D' (LIBSOUP_VERSION_MAJOR), which reaches us as `-D-DLIBSOUP_VERSION_MAJOR=2' -- not a
    macro name. Imported targets also leak into INCLUDE_DIRECTORIES as `-I<dir>/LCMS2::LCMS2'.
    Neither changes which functions a module defines, so neither is worth failing over.
    """
    usable = []
    for flag in flags:
        if not flag:
            continue
        if flag.startswith("-D"):
            name = flag[2:].split("=", 1)[0]
            if not name or not (name[0].isalpha() or name[0] == "_"):
                continue
        usable.append(flag)
    return usable


def parse_api(path):
    """Return [(kind, name)] for every entry point declared in an X-macro API header."""
    entries = []
    seen = set()
    pattern = re.compile(r"^\s*(OPTIONAL|REQUIRED|DEFAULT)\(\s*[^,]+,\s*([A-Za-z_]\w*)")
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            match = pattern.match(line)
            if match is None:
                continue
            kind, name = match.group(1), match.group(2)
            if name in seen:
                continue
            seen.add(name)
            entries.append((kind, name))
    if not entries:
        sys.exit(f"generate_iop_static: no API entries found in {path}")
    return entries


def preprocess(compiler, flags, source):
    """Run the compiler's preprocessor over one source, keeping its line markers."""
    command = [compiler, "-E"] + flags + [source]
    result = subprocess.run(command, capture_output=True, text=True, errors="replace")
    if result.returncode != 0:
        sys.stderr.write(result.stderr[-4000:])
        sys.exit(f"generate_iop_static: preprocessing failed for {source}")
    return result.stdout


def own_code(preprocessed, module_dir):
    """Keep only the regions the line markers attribute to the module's own files.

    Without this, a definition in an included header counts as the module's own: two C++
    modules (iop/bilateral.cc, iop/tonemap.cc) pick up an unrelated `init(...)' from a
    header and would claim an init() they do not have.
    """
    kept = []
    keeping = False
    marker = re.compile(r'^#\s+\d+\s+"([^"]*)"')
    module_dir = os.path.realpath(module_dir)
    for line in preprocessed.splitlines():
        match = marker.match(line)
        if match is not None:
            name = match.group(1)
            path = os.path.realpath(name) if os.path.isabs(name) else None
            keeping = (path is not None and path.startswith(module_dir + os.sep)) \
                or os.path.basename(name).startswith("introspection_")
            continue
        if keeping:
            kept.append(line)
    return "\n".join(kept)


def defined_entry_points(body, names):
    """Names among `names' that `body' defines at file scope with external linkage."""
    if not names:
        return set()
    definition = re.compile(
        r"(?<![\w.>])(%s)\s*\([^;{]*\)\s*\{" % "|".join(sorted(names, key=len, reverse=True))
    )
    found = {match.group(1) for match in definition.finditer(body)}
    # A static definition is the module's own business and never crosses the link.
    return {
        name for name in found
        if re.search(r"\bstatic\b[^;{}]*\b%s\s*\(" % re.escape(name), body) is None
    }


def generate_present(args, api_names):
    flags = [flag for flag in args.flags if flag]
    found = set()
    for source in args.sources:
        body = own_code(preprocess(args.cc, flags, source), args.module_dir)
        found |= defined_entry_points(body, api_names)

    missing_required = [name for kind, name in args.api if kind == "REQUIRED" and name not in found]
    if missing_required:
        sys.exit(
            f"generate_iop_static: module '{args.module}' defines no "
            f"{', '.join(missing_required)} -- every module must."
        )

    lines = [
        "/* Auto-generated by tools/generate_iop_static.py -- do not edit.",
        " *",
        f" * Which entry points iop/{args.module} actually defines, as the compiler's own",
        " * preprocessor sees them. Consumed by DT_MODULE_PICK() in common/module_api.h;",
        " * every name in the API appears here, so a name the generator did not consider",
        " * is a compile error rather than a silently NULL function pointer. */",
        "",
        f"#ifndef DT_IOP_{args.module.upper()}_PRESENT_H",
        f"#define DT_IOP_{args.module.upper()}_PRESENT_H",
        "",
    ]
    for _kind, name in args.api:
        lines.append(f"#define DT_MODULE_HAS_{name} {1 if name in found else 0}")
    lines += ["", f"#endif // DT_IOP_{args.module.upper()}_PRESENT_H", ""]
    write_if_changed(args.present, "\n".join(lines))


def generate_fill(args):
    prefix = f"dt_iop_{args.module}__"
    content = f"""/* Auto-generated by tools/generate_iop_static.py -- do not edit.
 *
 * Binds iop/{args.module}'s entry points into its dt_iop_module_so_t. This is compiled
 * as part of the {args.module} object library, so DT_MODULE_SYMBOL_PREFIX is set and the
 * plain API names below carry this module's asm label: `module->process = process'
 * stores the address of {prefix}process and nothing else.
 *
 * The DEFAULT fallbacks are NOT applied here -- default_<fn> is static to
 * develop/imageop.c, which applies them right after calling this. */

#include "develop/imageop.h"
#include "{args.module}_present.h"

void {prefix}fill_so(dt_iop_module_so_t *module)
{{
#define INCLUDE_API_FROM_MODULE_STATIC
#include "iop/iop_api.h"

  /* Not an X-macro entry: DT_MODULE() defines this in every module unconditionally. */
  module->version = dt_module_mod_version;
}}
"""
    write_if_changed(args.fill, content)


def generate_registry(args):
    modules = sorted(args.modules)
    lines = [
        "/* Auto-generated by tools/generate_iop_static.py -- do not edit.",
        " *",
        " * Every IOP module built into this binary. This replaces the directory scan in",
        " * dt_module_load_modules(): the set of modules is decided by src/iop/CMakeLists.txt",
        " * at build time, which is also what develop/iop_order.c's order tables and",
        " * develop/geometry/geometry.c's roster are written against, so discovering it again",
        " * at runtime could only ever disagree with them.",
        " *",
        " * Referencing every fill function from lib_ansel's own sources is also what pulls",
        " * each module's objects into the link. */",
        "",
        '#include "develop/imageop.h"',
        "",
    ]
    for module in modules:
        lines.append(f"extern void dt_iop_{module}__fill_so(dt_iop_module_so_t *module);")
    lines += ["", "const dt_iop_module_static_entry_t dt_iop_static_modules[] = {"]
    for module in modules:
        lines.append(f'  {{ "{module}", dt_iop_{module}__fill_so }},')
    lines += [
        "};",
        "",
        "const int dt_iop_static_modules_count "
        "= (int)(sizeof(dt_iop_static_modules) / sizeof(dt_iop_static_modules[0]));",
        "",
    ]
    write_if_changed(args.out, "\n".join(lines))


def write_if_changed(path, content):
    """Avoid touching an unchanged output, so ninja does not rebuild the world."""
    if os.path.exists(path):
        with open(path, encoding="utf-8") as handle:
            if handle.read() == content:
                return
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(content)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    module_parser = sub.add_parser("module", help="presence header + so-fill source")
    module_parser.add_argument("--module", required=True)
    module_parser.add_argument("--module-dir", required=True)
    module_parser.add_argument("--api", required=True)
    module_parser.add_argument("--present", required=True)
    module_parser.add_argument("--fill", required=True)
    module_parser.add_argument("--cc", required=True)
    module_parser.add_argument("--sources", nargs="+", required=True)

    registry_parser = sub.add_parser("registry", help="the table of every built module")
    registry_parser.add_argument("--out", required=True)
    registry_parser.add_argument("--modules", nargs="+", required=True)

    # Compiler flags come after a `--' sentinel: they are full of tokens starting with a
    # dash, which argparse would read as options of its own.
    argv = sys.argv[1:]
    flags = []
    if "--" in argv:
        cut = argv.index("--")
        argv, flags = argv[:cut], argv[cut + 1:]

    args = parser.parse_args(argv)
    args.flags = _usable_flags(flags)

    if args.command == "registry":
        generate_registry(args)
        return

    args.api = parse_api(args.api)
    api_names = {name for _kind, name in args.api} | set(VERSION_FUNCTIONS)
    generate_present(args, api_names)
    generate_fill(args)


if __name__ == "__main__":
    main()
