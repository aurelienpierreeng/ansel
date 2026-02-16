#    This file is part of darktable.
#    copyright (c) 2016-2020 Roman Lebedev.
#
#    darktable is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    darktable is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with darktable.  If not, see <http://www.gnu.org/licenses/>.

# Build from the repo root so packaging/ is in the build context:
# docker build -t ansel -f .ci/Dockerfile .

FROM ubuntu:24.04

# needed at least for python-based jsonschema :(
# see https://github.com/Julian/jsonschema/issues/299
# and https://github.com/docker-library/python/issues/13
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8
ENV LC_MESSAGES=C.UTF-8
ENV LANGUAGE=C.UTF-8
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y bash

COPY packaging/install-deps-ubuntu.sh /tmp/install-deps-ubuntu.sh
RUN chmod +x /tmp/install-deps-ubuntu.sh

# pls keep sorted :)
RUN export GCC_VER=14 && /bin/bash /tmp/install-deps-ubuntu.sh && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

RUN ./build.sh --install \
    rm -rf /src/build /src/src
