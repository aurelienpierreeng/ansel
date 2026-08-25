# Exiv2 in Ansel

Ansel builds its own Exiv2, from the `src/external/exiv2` submodule, pinned at **v0.27.7**
and linked **statically** into `lib_ansel`. This is the default on every platform.

`-DUSE_BUNDLED_EXIV2=OFF` links whatever the system provides instead, for packagers who need
it. It is supported, but it is not the tested configuration, and it comes with one hard
requirement (ISOBMFF, below) that several distributions do not meet.

---

## Why Ansel builds it

Exiv2 is not an ordinary dependency. Ansel does not merely link it, it inherits its behaviour:

- **Which version** decides what metadata a photo is seen to *have*. Lens identity, makernote
  coverage, XMP round-tripping and the ability to open a file at all all move between releases.
- **Which build options** decide whether whole file formats carry metadata. `EXIV2_ENABLE_BMFF`
  is CR3, AVIF and HEIF. `EXIV2_ENABLE_WIN_UNICODE` is whether a Windows path may contain an
  accent.

Leaving both of those to three packagers produced three different products, and that is what
[issue #474](https://github.com/aurelienpierreeng/ansel/issues/474) turned out to be:

- The Windows `.exe` shipped MSYS2's Exiv2 0.28.8. Every EXIF read, thumbnail extraction and
  XMP sidecar operation failed, silently, for any image below a folder with a non-ASCII
  character in its name.
- The Linux AppImage shipped `ubuntu-22.04`'s 0.27.5 — an end-of-life branch, pinned by the
  choice of CI runner image rather than by any decision, and with whatever options Ubuntu
  chose. Whether the AppImage could read CR3 metadata was, in effect, Ubuntu's call.
- macOS shipped Homebrew's 0.28.x, which worked, because APFS paths are UTF-8 and macOS never
  had the Windows bug at all.

Upstream darktable has the same problem and answers it the same way: their Windows nightly
builds Exiv2 v0.27.7 from source with `-DEXIV2_ENABLE_WIN_UNICODE=ON`, while their Linux
AppImage builds v0.28.8. Their one attempt at a code-level fix
([PR #15899](https://github.com/darktable-org/darktable/pull/15899), "switch Windows to UTF-8
locale entirely", 34 files) was auto-closed unmerged after 300 days.

---

## Options the bundled build sets

In `src/external/CMakeLists.txt`. The ones that are not defaults, and why:

| Option | Value | Why |
|---|---|---|
| `BUILD_SHARED_LIBS` | `OFF` | Static: nothing else in the process can supply a second `libexiv2`, and there is no extra shared object to ship. |
| `EXIV2_ENABLE_BMFF` | `ON` | CR3, AVIF, HEIF. **Required** — see below. Exiv2 0.27's default is `OFF`. |
| `EXIV2_ENABLE_XMP` | `ON` | The whole XMP sidecar layer. Pulls in expat. |
| `EXIV2_ENABLE_PNG` | `ON` | PNG metadata. Pulls in zlib. |
| `EXIV2_ENABLE_LENSDATA` | `ON` | Nikon lens tables. |
| `EXIV2_ENABLE_WIN_UNICODE` | `ON` on Windows | Defines `EXV_UNICODE_PATH`, the wide-path API. Without it, non-ASCII paths do not open. |
| `EXIV2_ENABLE_NLS` | `OFF` | See *Native language support* below. |
| `EXIV2_ENABLE_VIDEO`, `_WEBREADY`, `_CURL`, `_SSH` | `OFF` | Ansel reads metadata from files, never over the network. Keeps libcurl and libssh out of Exiv2's link line. |
| `EXIV2_BUILD_SAMPLES`, `_EXIV2_COMMAND`, `_UNIT_TESTS`, `_FUZZ_TESTS`, `_DOC` | `OFF` | We want the library, not the distribution. |

Build dependencies this leaves us with, and which the `packaging/install-deps-*.sh` scripts
now install in Exiv2's place: **expat** (the bundled XMP SDK) and **zlib** (PNG). Iconv is
used when present.

---

## ISOBMFF is a requirement, not an option

`src/CMakeLists.txt` probes the `EXV_ENABLE_BMFF` symbol in the Exiv2 we are actually linking
and **fails the configure** if it is absent. There is no `HAVE_LIBEXIV2_WITH_ISOBMFF` macro any
more, and no degraded build: `cr3` is unconditionally in `DT_SUPPORTED_EXTENSIONS`, and
`dt_exif_init()` calls `Exiv2::enableBMFF()` unconditionally on 0.27.x.

The reason is what a degraded build looks like from the user's chair. Some distributions —
Fedora among them — still build Exiv2 with `EXIV2_ENABLE_BMFF=OFF` over long-settled patent
worries. Canon CR3 is among the most common file types in Ansel's telemetry. An Ansel linked
against such a build starts perfectly well and then cannot open the user's raws, and nobody
reading that screen concludes that their distribution disabled a flag in a metadata library:
they file a bug against Ansel. A configure-time error costs a packager five minutes. The
alternative costs every one of their users their raw files, and costs us the bug report.

If you hit that error with `-DUSE_BUNDLED_EXIV2=OFF`, the fix is either to rebuild Exiv2 with
`-DEXIV2_ENABLE_BMFF=ON` or — far easier — to drop back to the bundled build.

---

## Windows paths

Every path inside Ansel is UTF-8: that is what GLib hands us, what the database stores, and
what the film-roll scanner produces. On Windows the narrow CRT reads a `const char *` path in
the process ANSI code page instead — CP-1252, CP-850, whatever the machine is set to — so a
path holding a single byte above 0x7F simply does not resolve.

Ansel's answer everywhere is to widen at the library boundary: `libraw_open_wfile()` in
`imageio/imageio_libraw.c`, `TIFFOpenW()` in `imageio/imageio_tiff.c`, `g_fopen()` (which is
`_wfopen()` underneath) in the XMP sidecar writer. Exiv2 is the same case, and `WIDEN()` in
`metadata/exif_internal.h` is where it happens.

Three things there are deliberate:

- **`WIDEN()` lives in one header.** It used to be defined identically in both `metadata/exif.cc`
  and `common/xmp_sidecar.cc`. Two copies of a rule this load-bearing is how they drift.
- **A build that cannot widen is a build error.** Exiv2 0.28 deleted the wide overloads, and
  the old `#else #define WIDEN(s) (s)` fallback meant everything still compiled and Windows
  users lost their EXIF at runtime. That is how #474 shipped twice. There is now an `#error`,
  with `ANSEL_ALLOW_NARROW_EXIV2_PATHS` as the documented way to override it deliberately.
- **Sidecar reads do not go through Exiv2's file I/O at all.** The four `Exiv2::readFile()`
  call sites in `common/xmp_sidecar.cc` are now `_read_xmp_packet()`, which reads through
  `dt_read_file()` → `g_fopen()`. They are small files, Ansel already has a reader that handles
  Windows paths correctly, and this way they do not depend on a capability Exiv2 may not have.

`tools/check_it_runs.sh` and the Windows job in `.github/workflows/ci.yml` both export from a
path named `Épreuve — тест` and fail on `Failed to open the data source`. Every round of #474 —
2023, 2025, twice in 2026 — was found months later by a user with a screenshot. That check is
what would have caught each one the day it landed; do not remove it.

---

## Native language support is off

`EXIV2_ENABLE_NLS=OFF`, and it is not simply a matter of flipping the flag.

Exiv2's `po/CMakeLists.txt` calls CMake's `GETTEXT_CREATE_TRANSLATIONS()`, which runs
`msgmerge --update` against the `.po` files **in the source tree**. In a submodule that means
every build leaves `src/external/exiv2` dirty.

What it costs us: `Exiv2::Metadatum::print()` returns some tag *values* from translated
tables ("Manual", "Auto", …), and those show in the metadata panel. Tag *names* are unaffected —
`dt_exif_set_exiv2_taglist()` reads them straight from `Exiv2::ExifTags::groupList()` and never
goes through gettext.

To get it back, shadow `GETTEXT_CREATE_TRANSLATIONS()` with a version that runs `msgfmt` and
skips the merge, before `add_subdirectory(exiv2)`. That is about fifteen lines and it has to
keep the `.mo` files installing into `${CMAKE_INSTALL_LOCALEDIR}`, because Exiv2 resolves
`EXV_LOCALEDIR` relative to the running binary — which, statically linked, is `ansel` itself.

Note also that `dt_strlcpy_to_utf8()` in `metadata/exif.cc` runs `print()`'s output through
`g_locale_to_utf8()`, which is inherited from darktable and is wrong for anything gettext
returns (gettext gives UTF-8; the locale on Windows generally is not). Turning NLS on without
looking at that first would trade one mojibake for another.

---

## Moving the pin to 0.28.x

**Do not, until the wide-path API is back in a release.** The pin is at 0.27.7 for exactly one
reason: Exiv2 commit `7933ff40`, shipped in 0.28.0 (May 2023), deleted the `std::wstring`
overloads and the `EXIV2_ENABLE_WIN_UNICODE` option.

The state of that, as of this writing:

- [Exiv2 #2637](https://github.com/Exiv2/exiv2/issues/2637), "What happened to wstring support
  in open() APIs?", opened May 2023 — **still open**. The maintainers' answer is that consumers
  should move to UCRT and the UTF-8 active code page.
- [Exiv2 PR #3117](https://github.com/Exiv2/exiv2/pull/3117) was **merged to `main`** in January
  2025. Four files, +95/−2. It restores `FileIo(std::wstring)`, `ImageFactory::open(std::wstring)`,
  `createIo()` and `setPath()` — *and* makes the narrow `std::string` constructor convert from
  UTF-8 and open with `_wfopen()`, so even code that does not widen would work.
- It has **not** been backported to the `0.28.x` maintenance branch, and `main` (the v1.0 track)
  has not produced a release since 0.28.0.

So the condition for the bump is: **#3117, or an equivalent, present in a tagged Exiv2 release.**
Check `include/exiv2/basicio.hpp` in the tag for `FileIo(const std::wstring&)`.

When that day comes:

1. Move the submodule to the tag and update `.gitmodules` if the branch changes.
2. `EXIV2_ENABLE_WIN_UNICODE` no longer exists. `WIDEN()` in `metadata/exif_internal.h` keys off
   `EXV_UNICODE_PATH`, which will not be defined either — give it whatever the new release uses
   to advertise the capability, or define `ANSEL_EXIV2_WIDE_PATH` from CMake once you have
   verified the release actually has it. **Do not just let the `#else` branch take over**; that
   is precisely the silent failure #474 was.
3. 0.28 added `EXIV2_ENABLE_INIH` (on by default), which wants the `inih` library. Either add the
   dependency to `packaging/install-deps-*.sh` and the flatpak manifest, or set it `OFF`. The
   flatpak manifest used to carry an `inih` module for this; it was removed with the Exiv2 module
   and would need restoring.
4. `EXIV2_ENABLE_BMFF` defaults to `ON` in 0.28, so the ISOBMFF probe should keep passing — but it
   is a probe, so it will tell you.
5. Ansel's source already compiles against both branches: `metadata/exif.cc` and
   `common/xmp_sidecar.cc` carry `EXIV2_TEST_VERSION(0,28,0)` guards for `AnyError`/`Error`,
   `toLong`/`toInt64`, `XmpParser::initialize()` and `enableBMFF()`.
6. Re-check the makernote regression the bump would bring with it. Measured on #474 by a user
   with a 2165-shot Canon library: 0.28.8 left 394 lenses unidentified where 0.27.7 left almost
   none. That is an Exiv2 makernote change, not a path issue, and none of the above fixes it.
   The durable answer is `ls_vendor_resolve()` (`src/iop/lens.c`), which takes lens identity away
   from Exiv2 altogether.

---

## Notes for anyone touching `src/external/CMakeLists.txt`

Embedding Exiv2 0.27.7 as a subdirectory needs a handful of things it does not do for itself,
each commented at the site:

- It writes `exv_conf.h` and `exiv2lib_export.h` to `${CMAKE_BINARY_DIR}` — which is the top of
  *our* build tree, not its own. They are copied to a named directory and that is what consumers
  get on their include path.
- It puts `include/exiv2` itself on the PUBLIC include path so its own sources can say
  `#include "basicio.hpp"`. Consumers must not inherit that: it would leave Exiv2's `config.h`
  one `-I` away from every Ansel translation unit that writes `#include "config.h"` and means its
  own.
- Its `findDependencies.cmake` appends `${CMAKE_SOURCE_DIR}/cmake` to `CMAKE_MODULE_PATH` to reach
  its own finders — Ansel's root `cmake/` directory, when embedded. Its `FindIconv.cmake` is then
  never found, CMake's builtin one answers, and `EXV_HAVE_ICONV` silently ends up off.
- Its `mainSetup.cmake` creates an `uninstall` target unless one already exists. Ansel's root
  `CMakeLists.txt` therefore creates its own *before* `add_subdirectory(src)`;
  `src/external/CMakeLists.txt` asserts that, so moving it back fails the configure rather than
  producing a duplicate-target mystery.
- It installs its archive, headers, pkg-config file, CMake export set and man page. `install()` is
  shadowed for the duration so none of that reaches Ansel's install tree — nor, downstream,
  the AppImage or the Windows installer.
- It asks for `cmake_minimum_required(VERSION 3.7.2)`, so `CMAKE_POLICY_VERSION_MINIMUM`,
  `CMAKE_POLICY_DEFAULT_CMP0069` and `CMAKE_POLICY_DEFAULT_CMP0077` are set around it. The last
  matters most: left OLD, Exiv2's own `option()` calls override the variables we set, and we
  quietly get a shared library and Exiv2's default option set instead of ours.
- Three things about the dialect it is compiled in, each of which cost a CI round:
  - **`-Wno-register`.** C++17 *removed* the `register` keyword. Clang rejects `xmpsdk/src/MD5.cpp`
    outright where GCC only warns, and neither `-w` nor `-Wno-error` reaches a hard error.
  - **`CXX_EXTENSIONS ON`**, unlike Ansel's own code. `xmpsdk/include/XMP_Environment.h` picks its
    platform on `#if defined WIN32` — the unprefixed spelling, which GCC and Clang define only in
    GNU mode. Under `-std=c++17` a MinGW build falls through to `UNIX_ENV` and `XMPUtils.cpp`
    reaches for `localtime_r`/`gmtime_r`, which MinGW does not have.
  - **`_LIBCPP_ENABLE_CXX17_REMOVED_AUTO_PTR`**, on Exiv2's own compilation *and* on the interface.
    `Image::AutoPtr` **is** `std::auto_ptr<Image>` at every language standard — `config.h`'s
    `using auto_ptr = std::unique_ptr<T>` sits in the global namespace while all 59 uses write the
    `std::` qualification. libstdc++ keeps `std::auto_ptr` as deprecated in C++17; libc++ deletes
    it, so on macOS neither Exiv2 nor `metadata/exif.cc` compiles without it. The macro also
    restores `unique_ptr`'s converting constructor from `auto_ptr`, which every
    `std::unique_ptr<Exiv2::Image> image(Exiv2::ImageFactory::open(...))` in our tree depends on.
- Its public headers are given to consumers with `INTERFACE_SYSTEM_INCLUDE_DIRECTORIES` (`-isystem`),
  because Ansel builds with `-Werror` and those `std::auto_ptr` uses are deprecation warnings we
  cannot fix from here.
- Its headers typedef `Image::AutoPtr` to `std::auto_ptr` or `std::unique_ptr` depending on
  `__cplusplus`, so the library and every consumer **must** agree on the C++ standard. The block
  sets `CMAKE_CXX_STANDARD 17` explicitly, because `src/CMakeLists.txt` says so further down the
  file than `add_subdirectory(external)` sits.
