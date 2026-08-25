{ pkgs ? import <nixpkgs> {} }:
# Based on nixpkgs/pkgs/applications/graphics/ansel/default.nix

let stdenv = pkgs.stdenv;
in stdenv.mkDerivation rec {
  name = "ansel-git";

  src = pkgs.lib.cleanSource ../..;

  nativeBuildInputs = [ pkgs.git ];

  buildInputs = with pkgs; with xorg; with gnome2;
    [ atk cairo cmake cmark curl dbus_glib expat glib gtk3
      ilmbase intltool lcms2 lensfun libX11 libexif
      libjpeg libpng
      librsvg libtiff libxcb openexr pixman pkgconfig sqlite libxslt
      colord colord-gtk libxshmfence libxkbcommon at_spi2_core
      osm-gps-map
    ];

  cmakeFlags = [
  ];

  # ansel changed its rpath handling in commit
  # 83c70b876af6484506901e6b381304ae0d073d3c and as a result the
  # binaries can't find libansel.so, so change LD_LIBRARY_PATH in
  # the wrappers:
  preFixup = ''
    gappsWrapperArgs+=(
      --prefix LD_LIBRARY_PATH ":" "$out/lib/ansel"
    )
  '';

  meta = with stdenv.lib; {
    description = "Virtual lighttable and darkroom for photographers";
    homepage = https://www.darktable.org;
    license = licenses.gpl3Plus;
    platforms = platforms.linux;
    maintainers = [ maintainers.goibhniu maintainers.rickynils maintainers.flosse ];
  };
}
