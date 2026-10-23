{
  description = "Ansel build environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      forAllSystems = nixpkgs.lib.genAttrs [ "x86_64-linux" "aarch64-linux" ];
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
        in
        {
          default = pkgs.mkShell {
            # Debug builds compile at -O0 (Ansel disables LTO there, see CMakeLists.txt), and
            # nixpkgs' default hardening adds _FORTIFY_SOURCE unconditionally; glibc's
            # features.h turns that combination into a #warning, which the vendored
            # LensSerious submodule's -Werror=cpp promotes to a build-breaking error.
            hardeningDisable = [ "fortify" ];

            nativeBuildInputs = with pkgs; [
              cmake
              ninja
              pkg-config
              gettext
              intltool
              git
              perl
              po4a
              python3
              python3Packages.jsonschema
              desktop-file-utils
              appstream-glib
              libxslt # xsltproc
              exiftool
            ];

            buildInputs = with pkgs; [
              glib
              gtk3
              cairo
              pango
              atk
              gdk-pixbuf
              librsvg
              colord
              colord-gtk
              lcms2
              libjpeg
              libpng
              libtiff
              libwebp
              libheif
              libavif
              libde265
              libjxl
              openexr
              openjpeg
              pugixml
              json-glib
              libsoup_3
              sqlite
              libxml2
              libxslt
              curl
              expat
              libexif
              lensfun
              osm-gps-map
              x265
              zlib
              icu
              dbus-glib
              cmark
              cmocka
              cups
              pixman
              libraw
              SDL2
              isocodes
              at-spi2-core
              libx11
              libxcb
              libxshmfence
              libxkbcommon
              fuse
              gst_all_1.gstreamer
              gst_all_1.gst-plugins-base
              llvmPackages.openmp
            ];

            shellHook = ''
              # nixpkgs splits gio-unix-2.0's headers (gio/gunixmounts.h, used by
              # src/libs/collect.c) into their own include dir instead of glib's main one;
              # nothing in Ansel's CMake pulls the gio-unix-2.0 pkg-config module in to add it.
              export NIX_CFLAGS_COMPILE="$NIX_CFLAGS_COMPILE -I${pkgs.glib.dev}/include/gio-unix-2.0"

              # A nixpkgs-packaged GTK app normally gets GSETTINGS_SCHEMA_DIR from
              # wrapGAppsHook; running the raw binary out of the build tree skips that, and
              # GTK's GtkFileChooserButton calls g_settings_new("org.gtk.Settings.FileChooser")
              # on realize. g_settings_new() on a missing schema logs at G_LOG_LEVEL_ERROR,
              # which glib treats as unconditionally fatal and abort()s the process.
              export GSETTINGS_SCHEMA_DIR="${pkgs.gtk3}/share/gsettings-schemas/${pkgs.gtk3.name}/glib-2.0/schemas"

              echo "Ansel dev shell ready. Configure with:"
              echo "  cmake -S . -B _build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_LIBDIR=lib"
            '';
          };
        });
    };
}
