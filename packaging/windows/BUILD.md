## Native compile using MSYS2:
How to make an Ansel Windows installer (x64 only):

* Install MSYS2 (instructions and prerequisites can be found on the official website: https://www.msys2.org)

* Start the MSYS terminal and update the base system until no further updates are available by repeating:
    ```bash
    $ pacman -Syu
    ```

* From the MSYS terminal, install x64 developer tools, x86_64 toolchain and git:
    ```bash
    $ pacman -S --needed base-devel intltool git
    $ pacman -S --needed mingw-w64-x86_64-{toolchain,cmake,ninja,nsis}
    ```

* Install required libraries and dependencies for darktable:
    ```bash
    $ pacman -S --needed mingw-w64-x86_64-{exiv2,lcms2,lensfun,dbus-glib,openexr,sqlite3,libxslt,libsoup,libavif,libheif,libwebp,libsecret,lua,graphicsmagick,openjpeg2,gtk3,pugixml,libexif,osm-gps-map,libgphoto2,drmingw,gettext,python3,iso-codes,python3-jsonschema,python3-setuptools}
    ```

* Install optional libraries and dependencies:

    for cLUT
    ```bash
    $ pacman -S --needed mingw-w64-x86_64-gmic
    ```
    for NG input with midi or gamepad devices
    ```bash
    $ pacman -S --needed mingw-w64-x86_64-{portmidi,SDL2}
    ```

* Install optional libraries required for [testing](../../src/tests/unittests/README.md):
    ```bash
    $ pacman -S --needed mingw-w64-x86_64-cmocka
    ```

* Switch to the MINGW64 terminal and update your lensfun database:
    ```bash
    $ lensfun-update-data
    ```
    
* MSYS will initialize a personal Unix-like `/home` folder, by default located in `C:\\msys64\home\USERNAME` where `USERNAME` is your current Windows username. If your username contains non-latin characters, like accentuated letters, the clashes between Windows encoding and Linux encoding will make the compilation fail on directory pathes issues. In that case, create a new directory in `C:\\msys64\home\USERNAME_WITHOUT_ACCENTS`, and in the MSYS terminal, do `cd /home/USERNAME_WITHOUT_ACCENT`.
    
* Modify the `.bash_profile` file in your `$HOME` directory and add the following lines:
    ```bash
    # Added as per http://wiki.gimp.org/wiki/Hacking:Building/Windows
    export PREFIX="/mingw64"
    export LD_LIBRARY_PATH="$PREFIX/lib:$LD_LIBRARY_PATH"
    export PATH="$PREFIX/bin:$PATH"
    ```

* By default CMake will only use one core during the build process. To speed things up you might wish to add a line like:
    ```bash
    export CMAKE_BUILD_PARALLEL_LEVEL="6"
    ```
    to your `~/.bash_profile` file. This would use 6 cores.

* Execute the following command to activate profile changes:
    ```bash
    $ . .bash_profile
    ```

* From the MINGW64 terminal, clone the darktable git repository (in this example into `~/darktable`):
    ```bash
    $ cd ~
    $ git clone https://github.com/Aurelien-Pierre/Ansel.git
    $ cd Ansel/
    $ git submodule init
    $ git submodule update
    ```

* Finally build and install darktable:
    ```bash
    $ mkdir build
    $ cd build
    $ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable ../.
    $ cmake --build .
    $ cmake --install .
    ```
    After this darktable will be installed in `/opt/darktable `directory and can be started by typing `/opt/darktable/bin/darktable.exe` in MSYS2 MINGW64 terminal.

    *NOTE: If you are using the Lua scripts, build the installer and install darktable.
    The Lua scripts check the operating system and see Windows and expect a Windows shell when executing system commands.
    Running darktable from the MSYS2 MINGW64 terminal gives a bash shell and therefore the commands will not work.*

* To build the installer image, which will create darktable-<VERSION>.exe installer in the current build directory, use:
    ```bash
    $ cmake --build . --target package
    ```

    *NOTE: The package created will be optimized for the machine on which it has been built, but it could not run on other PCs with different hardware or different Windows version. If you want to create a "generic" package, change the first cmake command line as follows:*
    ```bash
    $ cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable -DBINARY_PACKAGE_BUILD=ON ../.
    ```

While Ninja offers advantages of reduced build times for incremental builds (builds on Windows are significantly slower than with linux based systems), you can also fall back to more traditional Makefiles should the need arise. You'll need to install Autotools from an MSYS terminal with:

```bash
$ pacman -S --needed mingw-w64-x86_64-autotools
```

Now return to the MINGW64 terminal and use this sequence instead:

```bash
$ mkdir build
$ cd build
$ cmake -G 'MSYS Makefiles' -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/darktable ../.
$ cmake --build .
```

If you are in a hurry you can now run darktable by executing the `darktable.exe` found in the `build/bin` folder, install in `/opt/darktable` as described earlier, or create an install image.

If you like suffering you could also install clang and use that instead of gcc/g++ as the toolchain.
