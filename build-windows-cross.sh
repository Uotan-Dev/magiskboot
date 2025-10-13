#!/bin/bash
set -e

TOOLCHAIN_URL="https://github.com/mstorsjo/llvm-mingw/releases/download"
TOOLCHAIN_VER="20250709"
CRT="msvcrt"  
ARCH="i686"  

if [ "$ARCH" = "i686" ]; then
    CMAKE_ARCH="x86"
    CLANG_PREFIX="i686-w64-mingw32"
    STRIP_EXE="i686-w64-mingw32-strip"
    VCPKG_TRIPLET="x86-mingw-static"
    RUST_TARGET="i686-pc-windows-gnu"
    BUILD_RUST_STD="OFF"
    LDLIBS=""
elif [ "$ARCH" = "aarch64" ]; then
    CMAKE_ARCH="aarch64"
    CLANG_PREFIX="aarch64-w64-mingw32"
    STRIP_EXE="aarch64-w64-mingw32-strip"
    VCPKG_TRIPLET="arm64-mingw-static"
    RUST_TARGET="aarch64-pc-windows-gnullvm"
    BUILD_RUST_STD="ON"
    LDLIBS="-lbcrypt"
    CRT="ucrt"
else
    echo "no support: $ARCH"
    echo "Usage: $0 [i686|aarch64]"
    exit 1
fi

PROJECT_ROOT="$(cd "$(dirname "$0")" && pwd)"
LLVM_MINGW_DIR="$PROJECT_ROOT/llvm-mingw"
VCPKG_DIR="$PROJECT_ROOT/vcpkg"
BUILD_DIR="$PROJECT_ROOT/build-windows-${ARCH}"
TARGET_TRIPLE="${CLANG_PREFIX}"

echo "======================================"
echo "build Windows ${ARCH} version magiskboot"
echo "CRT: $CRT"
echo "Rust Target: $RUST_TARGET"
echo "======================================"

echo -e "\n### install necessary tools ###\n"
# sudo apt update
# sudo DEBIAN_FRONTEND=noninteractive apt install -y curl wget tar file \
#     build-essential pkg-config cmake ninja-build git

if [ ! -d "$LLVM_MINGW_DIR" ]; then
    echo -e "\n### download LLVM MinGW toolchain ###\n"
    LLVM_ARCHIVE="llvm-mingw-${TOOLCHAIN_VER}-${CRT}-ubuntu-22.04-x86_64.tar.xz"
    wget "${TOOLCHAIN_URL}/${TOOLCHAIN_VER}/${LLVM_ARCHIVE}" -O /tmp/llvm-mingw.tar.xz
    mkdir -p "$LLVM_MINGW_DIR"
    tar -xf /tmp/llvm-mingw.tar.xz -C "$LLVM_MINGW_DIR" --strip-components=1
    rm -f /tmp/llvm-mingw.tar.xz
else
    echo -e "\n### already exists, skipping download ###\n"
fi

export PATH="$LLVM_MINGW_DIR/bin:$PATH"

if [ ! -d "$VCPKG_DIR" ]; then
    echo -e "\n### clone vcpkg ###\n"
    git clone https://github.com/microsoft/vcpkg.git "$VCPKG_DIR"
fi

if [ ! -f "$VCPKG_DIR/vcpkg" ]; then
    echo -e "\n### bootstrap vcpkg ###\n"
    cd "$VCPKG_DIR"
    ./bootstrap-vcpkg.sh
    cd "$PROJECT_ROOT"
else
    echo -e "\n### vcpkg bootstraped ###\n"
fi

export VCPKG_INSTALLATION_ROOT="$VCPKG_DIR"

echo -e "\n### install Windows cross-compilation dependencies ###\n"
"$VCPKG_DIR/vcpkg" install --host-triplet="$VCPKG_TRIPLET" zlib liblzma lz4 'bzip2[core]'

echo -e "\n### setup Rust environment ###\n"
if [ ! -d "$HOME/.cargo" ]; then
    curl https://sh.rustup.rs -sSf | sh -s -- -y
fi

source "$HOME/.cargo/env"

if [ "$BUILD_RUST_STD" = "ON" ]; then
    echo "Installing nightly Rust toolchain..."
    rustup toolchain install nightly
    rustup default nightly
    rustup component add rust-src
else
    rustup target add "$RUST_TARGET"
fi

mkdir -p "$PROJECT_ROOT/.cargo"
cat > "$PROJECT_ROOT/.cargo/config.toml" << EOF
[target.i686-pc-windows-gnu]
linker = "${LLVM_MINGW_DIR}/bin/i686-w64-mingw32-clang"

[target.aarch64-pc-windows-gnullvm]
linker = "${LLVM_MINGW_DIR}/bin/aarch64-w64-mingw32-clang"

[env]
CXXBRIDGE_GEN_TARGET_OS = "windows"
EOF


echo -e "\n### create build directory ###\n"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

find "$PROJECT_ROOT/src" -name "target" -type d -exec rm -rf {} + 2>/dev/null || true
find "$PROJECT_ROOT/src" -name "Cargo.lock" -delete 2>/dev/null || true

rm -f "$PROJECT_ROOT/src/Magisk/native/src/.cargo/config.toml" 2>/dev/null || true

echo -e "\n### configure CMake ###\n"
cd "$BUILD_DIR"

RUSTC_PATH=$(rustup which rustc)
CARGO_PATH=$(rustup which cargo)

export CARGO_TARGET_I686_PC_WINDOWS_GNU_LINKER="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-clang"
export CC_i686_pc_windows_gnu="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-clang"
export CXX_i686_pc_windows_gnu="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-clang++"
export AR_i686_pc_windows_gnu="${LLVM_MINGW_DIR}/bin/llvm-ar"

if [ "$ARCH" = "aarch64" ]; then
    export CARGO_TARGET_AARCH64_PC_WINDOWS_GNULLVM_LINKER="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-clang"
    export CC_aarch64_pc_windows_gnullvm="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-clang"
    export CXX_aarch64_pc_windows_gnullvm="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-clang++"
    export AR_aarch64_pc_windows_gnullvm="${LLVM_MINGW_DIR}/bin/llvm-ar"
fi

export RUSTC_BOOTSTRAP="winsup,base,magiskboot"
export CXXBRIDGE_GEN_TARGET_OS="windows"

export RUSTFLAGS="--cfg mbb_stubs_O_PATH --cfg mbb_stubs_SYS_dup3 --cfg mbb_stubs_sendfile --cfg mbb_stubs___errno -Copt-level=z -A dead_code -A unused_imports -A redundant_semicolons -A unused_unsafe -Clto=yes -Clinker-plugin-lto=yes -Cembed-bitcode=yes"

cmake \
    -GNinja \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DMINGW=TRUE \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_CROSSCOMPILING=TRUE \
    -DCMAKE_C_COMPILER="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-clang" \
    -DCMAKE_CXX_COMPILER="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-clang++" \
    -DCMAKE_RC_COMPILER="${LLVM_MINGW_DIR}/bin/${TARGET_TRIPLE}-windres" \
    -DCMAKE_AR="${LLVM_MINGW_DIR}/bin/llvm-ar" \
    -DCMAKE_RANLIB="${LLVM_MINGW_DIR}/bin/llvm-ranlib" \
    -DCMAKE_CXX_SCAN_FOR_MODULES=OFF \
    -DFULL_RUST_LTO=ON \
    -DTRY_USE_LLD=ON \
    -DRust_COMPILER="${RUSTC_PATH}" \
    -DRust_CARGO="${CARGO_PATH}" \
    -DRust_CARGO_TARGET="${RUST_TARGET}" \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_DIR}/scripts/buildsystems/vcpkg.cmake" \
    -DVCPKG_TARGET_TRIPLET=${VCPKG_TRIPLET} \
    -DVCPKG_INSTALLED_DIR="${VCPKG_DIR}/installed" \
    -DCMAKE_FIND_ROOT_PATH="${LLVM_MINGW_DIR}/${TARGET_TRIPLE}" \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DVCPKG_PREFER_SYSTEM_LIBS=OFF \
    "$PROJECT_ROOT"

echo -e "\n### start building ###\n"

if ! cmake --build "$BUILD_DIR" -j "$(nproc)"; then
    echo -e "\n### build failed ###\n"
    exit 1
fi


echo -e "\n### Strip files ###\n"
"${LLVM_MINGW_DIR}/bin/${STRIP_EXE}" "$BUILD_DIR"/magiskboot*.exe

echo -e "\n### build completed ###\n"
file "$BUILD_DIR"/magiskboot*.exe
ls -lh "$BUILD_DIR"/magiskboot*.exe
echo -e "\n### check DLL dependencies ###\n"
strings "$BUILD_DIR"/magiskboot*.exe | grep -iE '\.dll$' | sort -u || echo "No DLL dependencies found."

echo -e "\n======================================"
echo "build success!"
echo "output file: $BUILD_DIR/magiskboot.exe"
echo "size: $(du -h "$BUILD_DIR"/magiskboot.exe | cut -f1)"
echo "======================================"
