#!/usr/bin/env python3
"""Check that every header compiles on its own.

A header should compile standalone. One that does not is relying on whoever includes
it having pulled something in first, which is the same defect as an unnecessary
include seen from the other side: the dependency is real, and written nowhere. It
breaks the day someone tidies an include in a file that never mentioned this header.

Each header is compiled as a translation unit containing nothing but an include of
itself, reusing the flags a real translation unit is built with, taken from
compile_commands.json. Only the syntax pass runs (-fsyntax-only), so no object code
is produced and the whole sweep is fast.

Headers that are NOT expected to stand alone are skipped, and the reason is recorded
rather than hidden:

  - X-macro headers, re-included several times in one translation unit with different
    macros defined, and expanded inside struct bodies. Compiling one alone is
    meaningless: it has no guard, by design.
  - Headers under a vendored or generated directory, which are not this project's to
    fix.

Usage:
  python3 tools/check_header_selfcontained.py -p build -o selfcontained.json [--jobs N]
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import re
import tempfile
from concurrent.futures import ThreadPoolExecutor

# Re-included on purpose, with different macros defined each time, and expanded inside
# struct bodies. They have no include guard by design, so "does it compile alone?" is
# not a question that applies to them.
X_MACRO_HEADERS = (
    "common/module_api.h",
    "views/view_api.h",
    "libs/lib_api.h",
    "imageio/format/imageio_format_api.h",
    "imageio/storage/imageio_storage_api.h",
    "iop/iop_api.h",
)

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")

SKIP_DIR_PARTS = ("/external/", "/tests/integration/", "/doxygen-awesome-css/",
                  "/build/", "/CMakeFiles/")


def flags_from_database(build_dir):
    """Take the compile flags of a representative C translation unit."""
    path = os.path.join(build_dir, "compile_commands.json")
    with open(path, encoding="utf-8") as fh:
        entries = json.load(fh)
    best = None
    for e in entries:
        f = e.get("file", "")
        if any(p in f for p in SKIP_DIR_PARTS) or not f.endswith(".c"):
            continue
        # prefer a plain, central translation unit over a generated or odd one
        if best is None or ("/common/" in f and "/common/" not in best.get("file", "")):
            best = e
    if best is None:
        raise SystemExit("no usable C entry in compile_commands.json")

    argv = best.get("arguments") or shlex.split(best.get("command", ""))

    # Flags whose value is a SEPARATE argv entry. Keeping the flag and dropping the
    # path that follows it silently removes an include directory: -isystem
    # /usr/include/glib-2.0 becomes a bare -isystem, glib.h stops being findable, and
    # every header that reaches glib fails for a reason that has nothing to do with
    # the header. That produced a "10% self-contained" reading before it was caught.
    TAKES_VALUE = ("-isystem", "-I", "-D", "-U", "-include", "-imacros",
                   "-idirafter", "-iquote", "-iprefix", "-isysroot", "--sysroot")
    DROP_WITH_VALUE = ("-o", "-c", "-MF", "-MT", "-MQ")

    keep, i = [], 1
    while i < len(argv):
        a = argv[i]
        if a in DROP_WITH_VALUE:
            i += 2
            continue
        if a.endswith((".c", ".cc", ".cpp", ".o")) or a in ("-MD", "-MMD"):
            i += 1
            continue
        if a in TAKES_VALUE and i + 1 < len(argv):
            keep.extend([a, argv[i + 1]])
            i += 2
            continue
        if a.startswith(("-I", "-D", "-i", "-std", "-f", "-m", "-U", "-W", "-pthread",
                         "-O", "-g")):
            keep.append(a)
        i += 1
    return argv[0], keep, best.get("directory", build_dir)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--build", required=True, help="dir holding compile_commands.json")
    ap.add_argument("-s", "--source-dir", default="src")
    ap.add_argument("-o", "--out", default="selfcontained.json")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2)))
    args = ap.parse_args()

    compiler, flags, workdir = flags_from_database(args.build)
    sys.stderr.write("selfcontained: %s with %d flags\n" % (compiler, len(flags)))

    headers, skipped = [], []
    for dirpath, dirnames, filenames in os.walk(args.source_dir):
        rel_dir = "/" + dirpath.replace(os.sep, "/").strip("/") + "/"
        if any(p in rel_dir for p in SKIP_DIR_PARTS):
            dirnames[:] = []
            continue
        for name in filenames:
            if not name.endswith((".h", ".hpp")):
                continue
            rel = os.path.join(dirpath, name).replace(os.sep, "/")
            if any(rel.endswith(x) for x in X_MACRO_HEADERS):
                skipped.append({"header": rel, "reason": "X-macro header, no guard by design"})
                continue
            headers.append(rel)
    headers.sort()
    sys.stderr.write("selfcontained: %d headers, %d skipped\n" % (len(headers), len(skipped)))

    def check(rel):
        src = '#include "%s"\n' % os.path.abspath(rel)
        with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as tf:
            tf.write(src)
            tmp = tf.name
        try:
            # LC_ALL=C: the compiler's diagnostics are parsed below, and a localised
            # build reports "erreur:" rather than "error:", which silently turns every
            # extracted message into the wrong line.
            env = dict(os.environ, LC_ALL="C", LANG="C")
            r = subprocess.run([compiler] + flags
                               + ["-fdiagnostics-color=never", "-fsyntax-only", tmp],
                               capture_output=True, text=True, errors="replace",
                               cwd=workdir, check=False, env=env)
            ok = r.returncode == 0
            first = ""
            if not ok:
                # Belt and braces: a build configured with colour forced on ignores
                # -fdiagnostics-color=never, and the escape codes break the match.
                clean = ANSI.sub("", r.stderr)
                for line in clean.splitlines():
                    if ": error:" in line or ": fatal error:" in line:
                        first = line.strip()
                        break
                else:
                    first = (clean.strip().splitlines() or [""])[0]
            return {"header": rel, "ok": ok, "first_error": first}
        finally:
            os.unlink(tmp)

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(check, headers))

    bad = [r for r in results if not r["ok"]]
    payload = {"results": results, "skipped": skipped,
               "headers": len(results), "failing": len(bad)}
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=1)
    sys.stderr.write("selfcontained: %d/%d self-contained (%.1f%%), wrote %s\n"
                     % (len(results) - len(bad), len(results),
                        100.0 * (len(results) - len(bad)) / max(1, len(results)), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
