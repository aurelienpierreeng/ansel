#!/bin/bash
#   This file is part of darktable,
#   Copyright (C) 2011-2015, 2017 johannes hanika.
#   Copyright (C) 2012 Jean-Sébastien Pédron.
#   Copyright (C) 2014-2016 Tobias Ellinghaus.
#   Copyright (C) 2016-2017 Roman Lebedev.
#   Copyright (C) 2017, 2020 Heiko Bauke.
#   Copyright (C) 2018-2020, 2022 Pascal Obry.
#   Copyright (C) 2022-2023 Aurélien PIERRE.
#   
#   darktable is free software: you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation, either version 3 of the License, or
#   (at your option) any later version.
#   
#   darktable is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#   
#   You should have received a copy of the GNU General Public License
#   along with darktable.  If not, see <http://www.gnu.org/licenses/>.
#   
#   
#   
#   
#   
#   
#   
#   
#   
#   
DT_SRC_DIR=$(dirname "$0")
DT_SRC_DIR=$(cd "$DT_SRC_DIR/../../" && pwd -P)

cd "$DT_SRC_DIR" || exit

git shortlog -sne release-3.9.0..HEAD

echo "are you sure these guys received proper credit in the about dialog?"
echo "HINT: $ tools/generate-authors.sh release-3.1.0 HEAD > AUTHORS"
read -r answer

# prefix rc with ~, so debian thinks its less than
echo "* archiving git tree"

dt_decoration=$(git describe --tags | sed -e 's,^v-,,;s,-,+,;s,-,~,;' -e 's/rc/~rc/')

echo "* * creating root archive"
git archive --format tar HEAD --prefix=ansel-"$dt_decoration"/ -o ansel-"$dt_decoration".tar

echo "* * creating submodule archives"
# for each of git submodules append to the root archive
git submodule foreach --recursive 'git archive --format tar --verbose --prefix="ansel-'"$dt_decoration"'/$path/" HEAD --output "'"$DT_SRC_DIR"'/ansel-sub-$sha1.tar"'

if [ $(ls "$DT_SRC_DIR/ansel-sub-"*.tar | wc -l) != 0  ]; then
  echo "* * appending submodule archives, combining all tars"
  find "$DT_SRC_DIR" -maxdepth 1 -name "ansel-sub-*.tar" -exec tar --concatenate --file "$DT_SRC_DIR/ansel-$dt_decoration.tar" {} \;
  # remove sub tars
  echo "* * removing all sub tars"
  rm -rf "$DT_SRC_DIR/ansel-sub-"*.tar
fi

echo "* * done creating archive"

TMPDIR=$(mktemp -d -t ansel-XXXXXX)
cd "$TMPDIR" || exit

tar xf "$DT_SRC_DIR/ansel-$dt_decoration.tar"

# create version header for non-git tarball:
echo "* creating version header"
"$DT_SRC_DIR/tools/create_version_c.sh" "ansel-$dt_decoration/src/version_gen.c" "$dt_decoration"

# drop integration tests
echo "* removing src/tests/integration"
rm -rf ansel-"$dt_decoration"/src/tests/integration

# drop all git-related stuff
find ansel-"$dt_decoration"/ -iname '.git*' -exec rm -fr {} \;

# ... and also remove RELEASE_NOTES. that file is just for internal use
#echo "* removing RELEASE_NOTES"
#rm -rf ansel-$dt_decoration/RELEASE_NOTES

# wrap it up again
echo "* creating final tarball"
tar cf ansel-$dt_decoration.tar ansel-$dt_decoration/ || exit
rm "$DT_SRC_DIR/ansel-$dt_decoration.tar"
xz -z -v -9 -e "ansel-$dt_decoration.tar"
cp "ansel-$dt_decoration.tar.xz" "$DT_SRC_DIR"

## now test the build:
#echo "* test compiling"
#rm -rf "ansel-$dt_decoration/"
#tar xf "ansel-$dt_decoration.tar.xz"
#cd "ansel-$dt_decoration/" || exit
#./build.sh --prefix "$TMPDIR/ansel/"

#echo
#echo "to actually test this build you should do:"
#echo "cd $TMPDIR/ansel-$dt_decoration/build && make install"
#echo "then run ansel from:"
#echo "$TMPDIR/ansel/bin/ansel"
