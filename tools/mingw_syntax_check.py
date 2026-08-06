#!/usr/bin/env python3
"""Syntax-check translation units with the MinGW cross compiler.

Why: a clean Linux build cannot vouch for code inside `#ifdef _WIN32`, and it cannot
see the legacy macros `windows.h` defines (`near`, `grp2`, `interface`, ...). Every
cross-platform breakage in the darktable.h series was of that shape -- green locally,
broken on MinGW, and usually reported in a file that was not at fault. This runs the
real Windows toolchain over the tree so those are caught before CI, or when CI is
unavailable.

This is `-fsyntax-only`: it compiles nothing and links nothing. That is deliberate --
the goal is the preprocessor and the parser, which is where this class of bug lives.

Some libraries have no Fedora mingw64 package (json-glib, lensfun, libcurl, libraw,
osmgpsmap, openjpeg, sentry, OpenCL ...), so files needing them cannot be checked here.
They are reported as SKIPPED and counted separately, never as passing: a check you did
not run is not a check that succeeded. Keep UNAVAILABLE honest -- an entry for a library
that IS installed would downgrade a real failure to "skipped".

Setup (Fedora):
  sudo dnf install mingw64-gcc mingw64-gcc-c++ mingw64-glib2 mingw64-gtk3 \
                   mingw64-lcms2 mingw64-sqlite mingw64-exiv2 mingw64-libpng \
                   mingw64-libtiff mingw64-libjpeg-turbo

Usage:
  python3 tools/mingw_syntax_check.py                    # every .c/.cc under src/
  python3 tools/mingw_syntax_check.py --changed master   # only what a branch touched
  python3 tools/mingw_syntax_check.py src/iop/lens.cc    # specific files
  python3 tools/mingw_syntax_check.py --jobs 8
"""
import concurrent.futures
import json
import os
import re
import subprocess
import sys

REPO = os.getcwd()
CC = 'x86_64-w64-mingw32-gcc'
CXX = 'x86_64-w64-mingw32-g++'
PKGS = ['gtk+-3.0', 'lcms2', 'sqlite3', 'libpng', 'libtiff-4', 'libjpeg', 'exiv2',
        'librsvg-2.0', 'libxml-2.0', 'libwebp', 'OpenEXR', 'libsoup-2.4']

# Third-party headers genuinely absent from the mingw64 sysroot. A file that fails
# ONLY because one of these is missing is not a finding -- but this list must be kept
# honest: leaving an installed library in here would silently downgrade a REAL failure
# to "skipped", which defeats the purpose of the harness. Verified against
# /usr/x86_64-w64-mingw32/sys-root/mingw/include; re-check after installing packages.
UNAVAILABLE = re.compile(
    r'(json-glib[/.]|lensfun[/.]|curl/curl\.h|libraw|osmgpsmap|osm-gps-map|openjpeg|'
    r'gmic|libsecret|libavif|libheif|graphicsmagick|magick|sentry\.h|CL/cl|lua\.h|'
    r'cmark|colord|gphoto2|portmidi|pugixml|rawspeed|libdeflate|jasper|portaudio)',
    re.I)

# Mirror the defines the real Win64 CI job passes. Without them the harness invents
# failures that do not exist in the actual build -- e.g. localtime_r is only declared
# on MinGW when _POSIX_THREAD_SAFE_FUNCTIONS is set, so omitting it makes every user
# of localtime_r look broken. Taken from the Win64.UCRT64 compile command line.
WIN_DEFINES = ['-DHAVE_CONFIG_H', '-D_POSIX_THREAD_SAFE_FUNCTIONS', '-D_USE_MATH_DEFINES',
               '-D__USE_MINGW_ANSI_STDIO=1', '-DUNICODE', '-D_UNICODE',
               '-D__GDK_KEYSYMS_COMPAT_H__']

MISSING_HEADER = re.compile(r"fatal error: ([^:]+): No such file or directory")


# Feature defines whose headers are not in the mingw64 sysroot. Keeping them does not
# make the harness stricter -- it just converts checkable files into skipped ones,
# because the guarded include fails outright. Dropping them checks everything EXCEPT
# those branches, which were unverifiable regardless. HAVE_MAP alone accounted for 110
# skipped files.
UNBUILDABLE_FEATURES = ('HAVE_MAP', 'HAVE_GMIC', 'HAVE_LIBAVIF', 'HAVE_LIBHEIF',
                        'HAVE_GRAPHICSMAGICK', 'HAVE_IMAGEMAGICK', 'HAVE_HTTP_SERVER',
                        'HAVE_SENTRY', 'HAVE_LIBRAW', 'HAVE_OPENJPEG', 'HAVE_WEBP',
                        'HAVE_OPENEXR', 'HAVE_ISO_CODES', 'HAVE_CMARK', 'HAVE_LIBSECRET',
                        'HAVE_OSMGPSMAP_110_OR_NEWER', 'HAVE_OSMGPSMAP_NEWER_THAN_110')


def _drop_unbuildable(flags):
    keep = []
    for f in flags:
        if f.startswith('-D') and any(f[2:].split('=')[0] == n for n in UNBUILDABLE_FEATURES):
            continue
        keep.append(f)
    return keep


def _in_repo(path):
    """Keep project include paths, drop the host toolchain's own -I/-isystem: those are
    Linux headers and must not leak into a MinGW cross check."""
    return not os.path.isabs(path) or REPO in path


def _relevant_flags(argv):
    """The -D / -include / project -I flags from one compile command."""
    flags, i = [], 0
    while i < len(argv):
        a = argv[i]
        if a.startswith('-std='):
            # Take the language standard from the build too: RawSpeed's headers need
            # C++20, and forcing gnu++17 made common/imageio_rawspeed.cc fail inside a
            # submodule header -- a harness artefact, not a defect in our code.
            flags.append(a)
        elif a.startswith('-D'):
            flags.append(a)
        elif a == '-include' and i + 1 < len(argv):
            flags += ['-include', argv[i + 1]]
            i += 1
        elif a in ('-I', '-isystem') and i + 1 < len(argv):
            if _in_repo(argv[i + 1]):
                flags += ['-I', argv[i + 1]]
            i += 1
        elif a.startswith('-I') and len(a) > 2 and _in_repo(a[2:]):
            flags.append(a)
        i += 1
    return flags


def _source_for(entry_file):
    """Map a compdb entry back to the source we care about. Generated
    introspection_X.c carries the flags for the src/iop/X.c it textually includes."""
    base = os.path.basename(entry_file)
    if not base.startswith('introspection_'):
        rel = os.path.relpath(entry_file, REPO) if os.path.isabs(entry_file) else entry_file
        return [os.path.normpath(rel)]
    stem = base[len('introspection_'):]
    return [os.path.normpath(d + stem)
            for d in ('src/iop/', 'src/libs/', 'src/imageio/format/', 'src/imageio/storage/')
            if os.path.exists(d + stem)]


def build_flag_map(compdb='build/compile_commands.json'):
    """Per-file -D/-include/project -I flags, taken from the BUILD SYSTEM.

    Guessing these is how a cross-check invents failures: the real build force-includes
    common/module_api.h and iop/iop_api.h into every module, and gates whole APIs behind
    -DHAVE_MAP / -DBUILD_PRINT / -DHAVE_OPENCL. Without them the harness reports missing
    declarations that exist perfectly well in the real build.

    Generated introspection_X.c entries carry the flags for src/iop/X.c, which is
    textually included by them, so they are mapped back onto the original.
    """
    import shlex
    if not os.path.exists(compdb):
        r = subprocess.run(['ninja', '-C', 'build', '-t', 'compdb'],
                           capture_output=True, text=True)
        if r.returncode != 0:
            return {}
        entries = json.loads(r.stdout)
    else:
        entries = json.load(open(compdb))

    out = {}
    for e in entries:
        f = e.get('file', '')
        cmd = e.get('command', '')
        if not cmd or cmd.lstrip().startswith(':'):
            continue                      # link/utility line, not a compile
        try:
            argv = shlex.split(cmd)
        except ValueError:
            continue
        flags = _relevant_flags(argv)
        for src in _source_for(f):
            out.setdefault(src, flags)
    return out


def pkg_cflags():
    out = []
    for p in PKGS:
        r = subprocess.run(['mingw64-pkg-config', '--cflags', p],
                           capture_output=True, text=True)
        if r.returncode == 0:
            out += r.stdout.split()
    # de-duplicate, keep order
    seen, flags = set(), []
    for f in out:
        if f not in seen:
            seen.add(f)
            flags.append(f)
    return flags


def sources(args):
    explicit = [a for a in args if a.endswith(('.c', '.cc', '.cpp'))]
    if explicit:
        return explicit
    if '--changed' in args:
        base = args[args.index('--changed') + 1]
        r = subprocess.run(['git', 'diff', '--name-only', '%s..HEAD' % base, '--', 'src'],
                           capture_output=True, text=True)
        return [f for f in r.stdout.split() if f.endswith(('.c', '.cc', '.cpp'))]
    out = []
    for root, dirs, names in os.walk('src'):
        dirs[:] = [d for d in dirs if d != 'external']
        for n in names:
            if n.endswith(('.c', '.cc', '.cpp')):
                out.append(os.path.join(root, n))
    return sorted(out)


def _missing_unavailable_header(stderr):
    """The header name if this failure is only 'we do not have it cross-built'."""
    m = MISSING_HEADER.search(stderr)
    if not m:
        return None
    if UNAVAILABLE.search(m.group(1)) or not os.path.exists(os.path.join('src', m.group(1))):
        return m.group(1)
    return None


def _unverifiable_with_full_flags(cmd, own, fmap, path):
    """Dropping a feature define can hide declarations a feature-only file legitimately
    uses (src/libs/map_locations.c is entirely map code). Retry with the ORIGINAL flags:
    if it then dies on a header we do not have cross-built, the file is unverifiable
    here rather than broken."""
    full = fmap.get(os.path.normpath(path))
    if full is None or full == own or not own:
        return None
    head = cmd[:cmd.index(own[0])]
    tail = cmd[cmd.index(own[0]) + len(own):]
    r = subprocess.run(head + full + tail, capture_output=True, text=True)
    if r.returncode == 0:
        return None
    return _missing_unavailable_header(r.stderr)


def check(path, flags, fmap):
    cxx = path.endswith(('.cc', '.cpp'))
    own = fmap.get(os.path.normpath(path))
    own = _drop_unbuildable(own) if own is not None else None
    if own is None:
        return ('skip', path, '<not compiled by this build>')
    has_std = any(f.startswith('-std=') for f in (own or []))
    cmd = [CXX if cxx else CC, '-fsyntax-only'] + \
          ([] if has_std else ['-std=gnu++17' if cxx else '-std=gnu11']) + [
           '-I', 'src', '-I', 'build/src', '-I', 'src/external/OpenCL'] + own + WIN_DEFINES + [
           '-fopenmp', '-Wno-attributes', '-Wno-unknown-pragmas'] + flags + [path]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode == 0:
        return ('ok', path, '')
    log = r.stderr
    m = MISSING_HEADER.search(log)
    if m and UNAVAILABLE.search(m.group(1)):
        return ('skip', path, m.group(1))
    if m and not os.path.exists(os.path.join('src', m.group(1))):
        # some other header we simply do not have cross-built
        return ('skip', path, m.group(1))
    unverifiable = _unverifiable_with_full_flags(cmd, own, fmap, path)
    if unverifiable:
        return ('skip', path, unverifiable)

    first = [ln for ln in log.splitlines() if ' error:' in ln][:3]
    return ('fail', path, '\n'.join(first) or log.strip().splitlines()[-1] if log.strip() else '?')


def main():
    if subprocess.run(['which', CC], capture_output=True).returncode != 0:
        print('%s not found -- install mingw64-gcc (see the docstring)' % CC, file=sys.stderr)
        return 2
    args = sys.argv[1:]
    jobs = int(args[args.index('--jobs') + 1]) if '--jobs' in args else os.cpu_count() or 4
    flags = pkg_cflags()
    fmap = build_flag_map()
    files = sources(args)
    print('per-file flags recovered for %d translation units' % len(fmap))
    print('checking %d files with %s (%d jobs)' % (len(files), CC, jobs))

    ok = skipped = 0
    fails, skips = [], {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for status, path, info in ex.map(lambda f: check(f, flags, fmap), files):
            if status == 'ok':
                ok += 1
            elif status == 'skip':
                skipped += 1
                skips.setdefault(info, []).append(path)
            else:
                fails.append((path, info))

    if fails:
        print('\n=== FAILURES (%d) ===' % len(fails))
        for p, info in fails:
            print('\n%s\n%s' % (p, info))
    if skips:
        print('\n=== SKIPPED: no mingw64 package for these headers ===')
        for h, ps in sorted(skips.items(), key=lambda kv: -len(kv[1])):
            print('  %-34s %d file(s)' % (h, len(ps)))

    print('\nchecked=%d  ok=%d  skipped=%d  FAILED=%d' % (len(files), ok, skipped, len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
