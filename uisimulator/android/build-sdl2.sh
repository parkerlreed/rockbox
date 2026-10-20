#!/bin/sh
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Downloads SDL2 and builds it for the Android simulator.
# Called by configure --android-sim; see uisimulator/android/README.
#
# Usage: build-sdl2.sh DEST ANDROID_PLATFORM_VERSION
# Installs to DEST/prefix and keeps the source (with SDL's Java files) in
# DEST/src. Does nothing if DEST already holds this version.

set -e

SDL_VERSION=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
SDL_URL=https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz
ABI=arm64-v8a

DEST=$1
PLATFORM=$2
set -- # from here, extra CMake options
: "${ANDROID_NDK_PATH:?set ANDROID_NDK_PATH}"

if [ "$(cat "$DEST/prefix/.version" 2>/dev/null)" = "$SDL_VERSION" ]; then
    exit 0
fi

# CMake: the system's, or the one the Android SDK installs
CMAKE=$(command -v cmake || true)
if [ -z "$CMAKE" ] && [ -n "$ANDROID_SDK_PATH" ]; then
    CMAKE=$(ls -d "$ANDROID_SDK_PATH"/cmake/*/bin/cmake 2>/dev/null | sort -V | tail -n 1)
fi
if [ -z "$CMAKE" ]; then
    echo "ERROR: building SDL2 needs CMake: install it, or install the Android"
    echo "SDK's (sdkmanager \"cmake;3.22.1\") and set ANDROID_SDK_PATH."
    exit 1
fi
GENERATOR="Unix Makefiles"
NINJA=$(dirname "$CMAKE")/ninja
if command -v ninja >/dev/null; then
    GENERATOR=Ninja
elif [ -x "$NINJA" ]; then
    GENERATOR=Ninja
    set -- -DCMAKE_MAKE_PROGRAM="$NINJA"
fi

echo "Building SDL2 $SDL_VERSION for Android"
mkdir -p "$DEST"
cd "$DEST"

TARBALL=SDL2-$SDL_VERSION.tar.gz
if [ ! -f "$TARBALL" ]; then
    curl -fL -o "$TARBALL.part" "$SDL_URL"
    mv "$TARBALL.part" "$TARBALL"
fi
if ! echo "$SDL_SHA256  $TARBALL" | sha256sum -c - >/dev/null; then
    echo "ERROR: $DEST/$TARBALL doesn't match its checksum, delete it and retry"
    exit 1
fi

rm -rf src build prefix
mkdir src
tar xzf "$TARBALL" -C src --strip-components=1

"$CMAKE" -S src -B build -G "$GENERATOR" "$@" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_PATH/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=$ABI -DANDROID_PLATFORM="$PLATFORM" \
    -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$DEST/prefix" > build.log 2>&1 ||
    { echo "ERROR: configuring SDL2 failed, see $DEST/build.log"; exit 1; }
"$CMAKE" --build build >> build.log 2>&1 &&
"$CMAKE" --install build >> build.log 2>&1 ||
    { echo "ERROR: building SDL2 failed, see $DEST/build.log"; exit 1; }

echo "$SDL_VERSION" > prefix/.version
