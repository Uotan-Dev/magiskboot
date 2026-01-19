#!/bin/bash

# fail fast
set -e

# Get number of cores
if [ "$(uname)" = "Darwin" ]; then
    NPROC=$(sysctl -n hw.ncpu)
else
    NPROC=$(nproc)
fi

set -x
git submodule update --init --depth=1 --single-branch -j $NPROC "$@" \
    src/{Magisk,android_{core,fmtlib,libbase,logging},corrosion,mman-win32,msvc_getline}/
git -C src/Magisk submodule update --init --depth=1 --single-branch -j $NPROC "$@" native/src/external/cxx-rs/
set +x
