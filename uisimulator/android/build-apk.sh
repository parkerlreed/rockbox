#!/bin/bash
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Packages an --android-sim simulator build as rockbox-sim.apk.
# Run from the build directory after "make" and "make install".
# See uisimulator/android/README.

set -e

: "${ANDROID_SDK_PATH:?set ANDROID_SDK_PATH}"
: "${ANDROID_NDK_PATH:?set ANDROID_NDK_PATH}"
: "${ANDROID_SDL2_PREFIX:?set ANDROID_SDL2_PREFIX}"
: "${ANDROID_SDL2_SOURCE:?set ANDROID_SDL2_SOURCE to the SDL2 source tree}"

SRCDIR=$(cd "$(dirname "$0")" && pwd)
ROOTDIR=$(cd "$SRCDIR/../.." && pwd)
BUILDDIR=$(pwd)
OUT=$BUILDDIR/apk
ABI=arm64-v8a
PLATFORM=android-35

ANDROID_JAR=$ANDROID_SDK_PATH/platforms/$PLATFORM/android.jar
BUILD_TOOLS=$(ls -d "$ANDROID_SDK_PATH"/build-tools/* | sort -V | tail -n 1)
STRIP=$ANDROID_NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
KEYSTORE=${ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}

if [ ! -f "$BUILDDIR/libmain.so" ] || [ ! -d "$BUILDDIR/simdisk/.rockbox" ]; then
    echo "Run this from an --android-sim build dir after make && make install"
    exit 1
fi
if [ ! -f "$ANDROID_JAR" ]; then
    echo "$PLATFORM is missing, install it with sdkmanager \"platforms;$PLATFORM\""
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/lib/$ABI" "$OUT/classes" "$OUT/assets" "$OUT/res"

echo "Native libraries"
"$STRIP" --strip-unneeded -o "$OUT/lib/$ABI/libmain.so" "$BUILDDIR/libmain.so"
"$STRIP" --strip-unneeded -o "$OUT/lib/$ABI/libSDL2.so" "$ANDROID_SDL2_PREFIX/lib/libSDL2.so"

# Codecs and plugins go in as libraries, named the way
# android_lib_path() in uisimulator/common/load_code-sim.c expects.
# The simdisk keeps empty placeholders so they still show up in menus.
mkdir -p "$OUT/files"
cp -a "$BUILDDIR/simdisk" "$OUT/files/simdisk"
# Skins without the red keypad hints, made with clean-skin.py
MODELNAME=$(sed -n 's/^export MODELNAME=//p' "$BUILDDIR/Makefile")
if [ -f "$SRCDIR/bitmaps/UI-$MODELNAME.bmp" ]; then
    cp "$SRCDIR/bitmaps/UI-$MODELNAME.bmp" "$OUT/files/UI256.bmp"
else
    cp "$BUILDDIR/UI256.bmp" "$OUT/files/"
fi
rm -f "$OUT/files/simdisk/.rockbox/libmain.so"
(cd "$OUT/files/simdisk" && find .rockbox -name '*.rock' -o -name '*.codec' -o -name '*.ovl') |
while read -r f; do
    lib=lib$(echo "${f#.rockbox/}" | tr '/.' '__').so
    "$STRIP" --strip-unneeded -o "$OUT/lib/$ABI/$lib" "$OUT/files/simdisk/$f"
    : > "$OUT/files/simdisk/$f"
done

echo "Assets"
(cd "$OUT/files" && zip -qr "$OUT/assets/files.zip" .)

echo "Java"
javac -nowarn --release 11 -Xlint:-options -classpath "$ANDROID_JAR" -d "$OUT/classes" \
    $(find "$ANDROID_SDL2_SOURCE/android-project/app/src/main/java" "$SRCDIR/src" -name '*.java')
"$BUILD_TOOLS/d8" --min-api 26 --lib "$ANDROID_JAR" --output "$OUT" \
    $(find "$OUT/classes" -name '*.class')

echo "Resources"
for d in ldpi mdpi hdpi; do
    "$BUILD_TOOLS/aapt2" compile -o "$OUT/res" "$ROOTDIR/android/res/drawable-$d/launcher.png"
done
"$BUILD_TOOLS/aapt2" link -o "$OUT/unaligned.apk" -I "$ANDROID_JAR" \
    --manifest "$SRCDIR/AndroidManifest.xml" \
    --min-sdk-version 26 --target-sdk-version 35 --debug-mode \
    -A "$OUT/assets" "$OUT"/res/*.flat
(cd "$OUT" && zip -qr unaligned.apk classes.dex lib)

echo "Signing"
if [ ! -f "$KEYSTORE" ]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US"
fi
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android \
    --out "$BUILDDIR/rockbox-sim.apk" "$OUT/aligned.apk"

echo "Created $BUILDDIR/rockbox-sim.apk"
