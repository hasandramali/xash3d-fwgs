#!/bin/bash
# Called from Xcode build phase. Copies pre-built game libs into .app bundle.
# If libs aren't built yet, builds valve + cs16 (fast, just 2 games).

SCRIPTDIR=${0%/*}
cd "$SCRIPTDIR" || exit 1

LIBSDIR=$(realpath ../../build/ios/libs 2>/dev/null)
if [ -z "$LIBSDIR" ]; then
    LIBSDIR="../../build/ios/libs"
fi

mkdir -p "$LIBSDIR"

# Build valve if missing (from local hlsdk submodule — fast, no clone)
if [ ! -f "$LIBSDIR/valve/cl_dlls/client_arm64.dylib" ] && [ ! -f "$LIBSDIR/valve/cl_dlls/client.dylib" ]; then
    if [ -f "../../hlsdk/CMakeLists.txt" ]; then
        cd "../../hlsdk"
        cmake -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
              -DCMAKE_INSTALL_PREFIX="$LIBSDIR" -DCMAKE_BUILD_TYPE=Debug \
              -B build -S . >/dev/null 2>&1 || true
        cmake --build build --target install >/dev/null 2>&1 || true
        cd "$SCRIPTDIR"
    else
        bash buildhlsdk.sh >/dev/null 2>&1 || true
    fi
fi

# Build CS16 if missing
if [ ! -f "$LIBSDIR/cstrike/cl_dlls/client_arm64.dylib" ] && [ ! -f "$LIBSDIR/cstrike/cl_dlls/client.dylib" ]; then
    bash buildcs16.sh >/dev/null 2>&1 || true
fi

# Rename dylibs to include arch suffix (all mods)
find "$LIBSDIR" -name "*.dylib" -type f | while read f; do
    dir=$(dirname "$f")
    base=$(basename "$f" .dylib)
    if [[ "$base" != *_arm64 ]] && [[ "$base" != *_x86* ]] && [[ "$base" != *_i386 ]]; then
        mv "$f" "$dir/${base}_arm64.dylib" 2>/dev/null || true
    fi
done

# Copy to Xcode app bundle if env vars are set
if [ -n "$BUILT_PRODUCTS_DIR" ] && [ -n "$EXECUTABLE_FOLDER_PATH" ]; then
    mkdir -p "${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}"
    cp -r "$LIBSDIR/"* "${BUILT_PRODUCTS_DIR}/${EXECUTABLE_FOLDER_PATH}/" 2>/dev/null || true
fi

exit 0
