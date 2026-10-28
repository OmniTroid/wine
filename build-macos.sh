#!/bin/bash
# Out-of-tree x86_64 build (runs under Rosetta) with 32- and 64-bit Windows support.
# Usage: ./build-macos.sh <build-dir> <install-prefix>
# Needs Xcode, Homebrew bison and mingw-w64. Libraries without an x86_64 Homebrew build are disabled.
set -euo pipefail

src="$(cd "$(dirname "$0")" && pwd)/wine"
build="${1:?build directory}"
prefix="${2:?install prefix}"

export PATH="$(brew --prefix bison)/bin:$PATH"
mkdir -p "$build"
cd "$build"

"$src/configure" -q --prefix="$prefix" \
  'CC=clang -arch x86_64' 'CXX=clang++ -arch x86_64' --host=x86_64-apple-darwin \
  --enable-archs=i386,x86_64 \
  --without-x --without-freetype --without-gstreamer --without-gnutls --without-sdl --without-cups \
  --without-sane --without-gphoto --without-pcap --without-usb --without-v4l2 --without-krb5 \
  --without-netapi --without-opencl --without-inotify --without-capi --without-ffmpeg

make -j"$(sysctl -n hw.ncpu)"
make install
