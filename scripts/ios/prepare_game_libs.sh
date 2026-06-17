#!/bin/bash
# Called from Xcode build phase.
# 1) Copies pre-built game libs from build/ios/libs/ into .app bundle
# 2) If missing, builds valve (from hlsdk/ submodule or clone) + cs16 (clone)

set -e

SCRIPTDIR=$(cd "${0%/*}" && pwd)
cd "$SCRIPTDIR"

REPO_ROOT=$(cd ../.. && pwd)
LIBSDIR="$REPO_ROOT/build/ios/libs"
mkdir -p "$LIBSDIR"

echo "prepare_game_libs: LIBSDIR=$LIBSDIR"
echo "prepare_game_libs: Xcode DEST=${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH:-<none>}"

# --- Build valve if missing ---
if [ ! -f "$LIBSDIR/valve/cl_dlls/client_arm64.dylib" ] && [ ! -f "$LIBSDIR/valve/cl_dlls/client.dylib" ]; then
    echo "prepare_game_libs: Building valve hlsdk..."
    if [ -f "$REPO_ROOT/hlsdk/CMakeLists.txt" ]; then
        echo "prepare_game_libs: Using local hlsdk/ submodule"
        cd "$REPO_ROOT/hlsdk"
        cmake -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
              -DCMAKE_INSTALL_PREFIX="$LIBSDIR" -DCMAKE_BUILD_TYPE=Debug \
              -B build -S .
        cmake --build build --target install
    else
        echo "prepare_game_libs: hlsdk/ submodule not found, cloning from GitHub"
        cd "$SCRIPTDIR"
        bash buildhlsdk.sh
    fi
    echo "prepare_game_libs: valve build done"
else
    echo "prepare_game_libs: valve dylibs already exist, skipping build"
fi

# --- Build cs16 if missing ---
if [ ! -f "$LIBSDIR/cstrike/cl_dlls/client_arm64.dylib" ] && [ ! -f "$LIBSDIR/cstrike/cl_dlls/client.dylib" ]; then
    echo "prepare_game_libs: Building cs16-client..."
    cd "$SCRIPTDIR"
    bash buildcs16.sh
    echo "prepare_game_libs: cs16 build done"
else
    echo "prepare_game_libs: cs16 dylibs already exist, skipping build"
fi

# --- Rename dylibs to arch-suffixed names ---
echo "prepare_game_libs: Renaming dylibs to _arm64 suffix..."
find "$LIBSDIR" -name "*.dylib" -type f | while read f; do
    dir=$(dirname "$f")
    base=$(basename "$f" .dylib)
    if [[ "$base" != *_arm64 ]] && [[ "$base" != *_x86* ]] && [[ "$base" != *_i386 ]]; then
        echo "prepare_game_libs:   $f -> ${base}_arm64.dylib"
        mv "$f" "$dir/${base}_arm64.dylib"
    fi
done

# --- Copy to Xcode .app bundle ---
if [ -n "$BUILT_PRODUCTS_DIR" ] && [ -n "$EXECUTABLE_FOLDER_PATH" ]; then
    DEST="${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}"
    echo "prepare_game_libs: Copying libs to $DEST"
    mkdir -p "$DEST"
    cp -r "$LIBSDIR/"* "$DEST/"
    echo "prepare_game_libs: Copy done"
else
    echo "prepare_game_libs: Xcode env vars not set, skip copy to .app"
fi

echo "prepare_game_libs: Done"
exit 0
