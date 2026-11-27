# Vendored Wayland protocol descriptions

`color-management-v1.xml` is `staging/color-management/color-management-v1.xml` from
[wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols), **tag 1.46**,
unmodified (sha256 `11b9d0b535d73981…`). It is the last release that describes **version 1** of
the protocol, which is the version `src/gui/wayland_color.c` binds and was written against;
1.47 moved the file to version 2.

`wayland-scanner` turns it into `color-management-v1-client-protocol.h` and
`color-management-v1-protocol.c` in the build directory (see `src/CMakeLists.txt`).

It is vendored rather than taken from the system because the distributions the nightly is built
on do not have it: Ubuntu 22.04 ships wayland-protocols 1.25, and the protocol appeared in 1.41.
A newer description would still generate code this client can use -- a client only receives the
events of the version it binds -- so replacing the file is safe, but changes nothing until
`wayland_color.c` binds a higher version.

Its licence (MIT) is in the file's own header.
