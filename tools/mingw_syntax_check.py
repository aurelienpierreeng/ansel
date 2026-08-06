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

Fedora does not package json-glib, lensfun or libcurl for mingw64, so files needing
those headers cannot be checked here. They are reported as SKIPPED, never as passing:
a check you did not run is not a check that succeeded.

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
import os
import re
import subprocess
import sys

CC = 'x86_64-w64-mingw32-gcc'
CXX = 'x86_64-w64-mingw32-g++'
PKGS = ['gtk+-3.0', 'lcms2', 'sqlite3', 'libpng', 'libtiff-4', 'libjpeg', 'exiv2']

# Third-party headers with no mingw64 package on Fedora. A file that fails *only*
# because one of these is absent is not a finding.
UNAVAILABLE = re.compile(
    r'(json-glib[/.]|lensfun[/.]|curl/curl\.h|libraw|openexr|OpenEXR|Imath|osmgpsmap|'
    r'librsvg|gmic|libsecret|libavif|libheif|webp|openjpeg|graphicsmagick|magick|'
    r'sentry\.h|CL/cl|lua\.h|libxml|cmark|colord|gphoto2|libsoup|portmidi|'
    r'pugixml|rawspeed|libdeflate|zlib\.h|jasper)', re.I)

MISSING_HEADER = re.compile(r"fatal error: ([^:]+): No such file or directory")


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


def check(path, flags):
    cxx = path.endswith(('.cc', '.cpp'))
    cmd = [CXX if cxx else CC, '-fsyntax-only',
           '-std=gnu++17' if cxx else '-std=gnu11',
           '-I', 'src', '-I', 'build/src', '-DHAVE_CONFIG_H',
           '-Wno-attributes', '-Wno-unknown-pragmas'] + flags + [path]
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
    first = [ln for ln in log.splitlines() if ' error:' in ln][:3]
    return ('fail', path, '\n'.join(first) or log.strip().splitlines()[-1] if log.strip() else '?')


def main():
    if not subprocess.run(['which', CC], capture_output=True).returncode == 0:
        print('%s not found -- install mingw64-gcc (see the docstring)' % CC, file=sys.stderr)
        return 2
    args = sys.argv[1:]
    jobs = int(args[args.index('--jobs') + 1]) if '--jobs' in args else os.cpu_count() or 4
    flags = pkg_cflags()
    files = sources(args)
    print('checking %d files with %s (%d jobs)' % (len(files), CC, jobs))

    ok = skipped = 0
    fails, skips = [], {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for status, path, info in ex.map(lambda f: check(f, flags), files):
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
