#!/bin/bash
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# Packages an --android-sim simulator build as rockbox-sim.apk, together
# with the other models the Android simulator supports, which it builds in
# models/<model>. Run from the build directory after "make" and
# "make install", which also holds the SDL2 that configure built.
# See uisimulator/android/README.

set -e


SRCDIR=$(cd "$(dirname "$0")" && pwd)
ROOTDIR=$(cd "$SRCDIR/../.." && pwd)
BUILDDIR=$(pwd)
SDL2=$BUILDDIR/sdl2-android

# The SDK and NDK configure found, unless overridden
makevar() { sed -n "s/^export $1=//p" "$BUILDDIR/Makefile" 2>/dev/null; }
ANDROID_SDK_PATH=${ANDROID_SDK_PATH:-$(makevar ANDROID_SDK_PATH)}
ANDROID_NDK_PATH=${ANDROID_NDK_PATH:-$(makevar ANDROID_NDK_PATH)}
: "${ANDROID_SDK_PATH:?set ANDROID_SDK_PATH, or re-run configure}"
: "${ANDROID_NDK_PATH:?set ANDROID_NDK_PATH, or re-run configure}"
OUT=$BUILDDIR/apk
# 64-bit ARM unless configure was run with ANDROID_ABI=armeabi-v7a
ABI=$(makevar ANDROID_ABI)
ABI=${ABI:-arm64-v8a}
PLATFORM=android-35

ANDROID_JAR=$ANDROID_SDK_PATH/platforms/$PLATFORM/android.jar
BUILD_TOOLS=$(ls -d "$ANDROID_SDK_PATH"/build-tools/* | sort -V | tail -n 1)
STRIP=$ANDROID_NDK_PATH/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip
# Signed with the debug key unless ANDROID_KEYSTORE names another, whose
# password is in ANDROID_KEYSTORE_PASS in apksigner's form (pass:<password>,
# file:<path> or env:<variable>)
DEBUG_KEYSTORE=$HOME/.android/debug.keystore
KEYSTORE=${ANDROID_KEYSTORE:-$DEBUG_KEYSTORE}
KEYSTORE_PASS=${ANDROID_KEYSTORE_PASS:-pass:android}

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
"$STRIP" --strip-unneeded -o "$OUT/lib/$ABI/libSDL2.so" "$SDL2/prefix/lib/libSDL2.so"

# The APK holds every model the Android simulator supports; the user picks
# one in Settings > Android > Model. The model this dir was configured for
# comes first and is the default; the others are configured and built in
# models/<model> here.
PRIMARY=$(makevar MODELNAME)
# Target 203 is the iPod mini 2G in 3D, an app of its own, with just it
SIM_3D=$(makevar ANDROID_SIM_3D)
# The oldest Android the native code was built for (configure)
MIN_SDK=$(makevar ANDROID_PLATFORM_VERSION)
MIN_SDK=${MIN_SDK:-23}
if [ "$SIM_3D" = 1 ]; then
    DEFAULT_MODELS=ipodmini2g
else
    DEFAULT_MODELS="ipodvideo ipodmini2g sansaclipzip gigabeatfx"
fi
MODELS="$PRIMARY $(echo ${ANDROID_SIM_MODELS:-$DEFAULT_MODELS} | tr ' ' '\n' | grep -vx "$PRIMARY" | tr '\n' ' ')"

# The iPod mini 2G as a 3D model (window-3d.c), made by ipodmini2g-3d.py
# from the model's glTF (materials) and OBJ (shape) in MINI3D_MODEL and
# MINI3D_OBJ; without them, it's the 2D skin
MINI3D_MODEL=${MINI3D_MODEL:-$ROOTDIR/945950-gltf-extended.gltf.zip}
MINI3D_OBJ=${MINI3D_OBJ:-$ROOTDIR/iPod_Mini_Blue_obj.zip}
# and the dock cable that slides in when the phone charges, if it's there
MINI3D_CABLE=${MINI3D_CABLE:-$ROOTDIR/30-pin-connector-to-usb-cable-1-meter-3-feet-1.snapshot.5.zip}
MINI3D=$BUILDDIR/mini3d
if [ "$SIM_3D" = 1 ] && [ -f "$MINI3D_MODEL" ] && [ -f "$MINI3D_OBJ" ] && { [ ! -f "$MINI3D/model.bin" ] ||
        [ "$MINI3D_MODEL" -nt "$MINI3D/model.bin" ] ||
        [ "$MINI3D_OBJ" -nt "$MINI3D/model.bin" ] ||
        { [ -f "$MINI3D_CABLE" ] && [ "$MINI3D_CABLE" -nt "$MINI3D/model.bin" ]; } ||
        [ "$ROOTDIR/uisimulator/skins/ipodmini2g-3d.py" -nt "$MINI3D/model.bin" ]; }; then
    echo "3D model"
    rm -rf "$MINI3D"
    python3 "$ROOTDIR/uisimulator/skins/ipodmini2g-3d.py" "$MINI3D_MODEL" "$MINI3D_OBJ" "$MINI3D" \
        $([ -f "$MINI3D_CABLE" ] && echo "$MINI3D_CABLE")
fi

# The 3D app is separate from the 2D simulator's, so both can be installed
MANIFEST=$SRCDIR/AndroidManifest.xml
if [ "$SIM_3D" = 1 ]; then
    PACKAGE=${ANDROID_SIM_PACKAGE:-org.rockbox.sim3d}
    # its name, and GL ES 3 for window-3d.c
    MANIFEST=$BUILDDIR/AndroidManifest-3d.xml
    sed -e 's/android:label="Rockbox"/android:label="Rockbox 3D"/' \
        -e 's/android:glEsVersion="0x00020000"/android:glEsVersion="0x00030000"/' \
        "$SRCDIR/AndroidManifest.xml" > "$MANIFEST"
else
    PACKAGE=${ANDROID_SIM_PACKAGE:-org.rockbox.sim}
fi

model_title() {
    case $1 in
        ipodvideo)  echo "iPod Video" ;;
        ipodmini2g) echo "iPod mini 2G" ;;
        sansaclipzip) echo "Sansa Clip Zip" ;;
        gigabeatfx) echo "Gigabeat F/X" ;;
        *)          echo "$1" ;;
    esac
}

build_model() {
    dir=$BUILDDIR/models/$1
    mkdir -p "$dir"
    if [ ! -f "$dir/Makefile" ]; then
        echo "  configuring $1"
        ln -sfn "$SDL2" "$dir/sdl2-android" # reuse this dir's SDL2
        (cd "$dir" && ANDROID_SDK_PATH="$ANDROID_SDK_PATH" \
            ANDROID_NDK_PATH="$ANDROID_NDK_PATH" ANDROID_ABI="$ABI" \
            "$ROOTDIR/tools/configure" --target="$1" --type=R) > "$dir/configure.log" 2>&1 ||
            { echo "configuring $1 failed, see $dir/configure.log"; exit 1; }
    fi
    echo "  building $1"
    (cd "$dir" && make -j"$(nproc)" && make install) > "$dir/build.log" 2>&1 ||
        { echo "building $1 failed, see $dir/build.log"; exit 1; }
}

# Codecs and plugins go in as libraries, named the way
# android_lib_path() in uisimulator/common/load_code-sim.c expects.
# The simdisk keeps empty placeholders so they still show up in menus.
pack_model() {
    model=$1 dir=$2 dest=$OUT/files/models/$1
    echo "  packing $model"
    mkdir -p "$dest"
    "$STRIP" --strip-unneeded -o "$OUT/lib/$ABI/libmain_$model.so" "$dir/libmain.so"
    cp -a "$dir/simdisk" "$dest/simdisk"
    rm -f "$dest/simdisk/.rockbox/libmain.so"
    # Skins without the red keypad hints, made with clean-skin.py
    if [ -f "$SRCDIR/bitmaps/UI-$model.bmp" ]; then
        cp "$SRCDIR/bitmaps/UI-$model.bmp" "$dest/UI256.bmp"
    else
        cp "$dir/UI256.bmp" "$dest/"
    fi
    # Vector device image, drawn instead where there is one (window-sdl.c)
    if [ -f "$ROOTDIR/uisimulator/skins/$model.svg" ]; then
        cp "$ROOTDIR/uisimulator/skins/$model.svg" "$dest/skin.svg"
    fi
    if [ "$SIM_3D" = 1 ] && [ "$model" = ipodmini2g ] && [ -f "$MINI3D/model.bin" ]; then
        cp -r "$MINI3D" "$dest/mini3d"
    fi
    (cd "$dest/simdisk" && find .rockbox -name '*.rock' -o -name '*.codec' -o -name '*.ovl') |
    while read -r f; do
        lib=lib${model}_$(echo "${f#.rockbox/}" | tr '/.' '__').so
        "$STRIP" --strip-unneeded -o "$OUT/lib/$ABI/$lib" "$dest/simdisk/$f"
        : > "$dest/simdisk/$f"
    done
    printf '%s\t%s\n' "$model" "$(model_title "$model")" >> "$OUT/assets/models.txt"
}

for model in $MODELS; do
    if [ "$model" = "$PRIMARY" ]; then
        pack_model "$model" "$BUILDDIR"
    else
        build_model "$model"
        pack_model "$model" "$BUILDDIR/models/$model"
    fi
done

echo "Assets"
(cd "$OUT/files" && zip -qr "$OUT/assets/files.zip" models)
# SIM_DEBUG_BUTTONS=1 outlines the buttons' touch areas (--debugbuttons)
if [ -n "$SIM_DEBUG_BUTTONS" ]; then
    : > "$OUT/assets/debug_buttons"
fi

echo "Java"
javac -nowarn --release 11 -Xlint:-options -classpath "$ANDROID_JAR" -d "$OUT/classes" \
    $(find "$SDL2/src/android-project/app/src/main/java" "$SRCDIR/src" -name '*.java')
"$BUILD_TOOLS/d8" --min-api "$MIN_SDK" --lib "$ANDROID_JAR" --output "$OUT" \
    $(find "$OUT/classes" -name '*.class')

echo "Resources"
for d in ldpi mdpi hdpi; do
    "$BUILD_TOOLS/aapt2" compile -o "$OUT/res" "$ROOTDIR/android/res/drawable-$d/launcher.png"
done
"$BUILD_TOOLS/aapt2" link -o "$OUT/unaligned.apk" -I "$ANDROID_JAR" \
    --manifest "$MANIFEST" --rename-manifest-package "$PACKAGE" \
    --min-sdk-version "$MIN_SDK" --target-sdk-version 35 --debug-mode \
    -A "$OUT/assets" "$OUT"/res/*.flat
(cd "$OUT" && zip -qr unaligned.apk classes.dex lib)

echo "Signing"
if [ ! -f "$KEYSTORE" ] && [ "$KEYSTORE" = "$DEBUG_KEYSTORE" ]; then
    mkdir -p "$(dirname "$KEYSTORE")"
    keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US"
fi
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$OUT/unaligned.apk" "$OUT/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-pass "$KEYSTORE_PASS" \
    --out "$BUILDDIR/rockbox-sim.apk" "$OUT/aligned.apk"

echo "Created $BUILDDIR/rockbox-sim.apk"
