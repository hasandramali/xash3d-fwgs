#!/bin/bash
set -euo pipefail
SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPTDIR/../.." && pwd)
SOURCE_DIR="$SCRIPTDIR/mod-build/svencoop"
LIBSDIR="$REPO_ROOT/build/ios/libs"
if [ ! -d "$SOURCE_DIR/.git" ]; then
    git clone --recursive --branch svencoop --single-branch https://github.com/hasandramali/hlsdk-portable "$SOURCE_DIR"
fi
cmake -S "$SOURCE_DIR" -B "$SOURCE_DIR/build-ios" \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_INSTALL_PREFIX="$LIBSDIR" -DCMAKE_BUILD_TYPE=Debug -DGAMEDIR=svencoop -DBUILD_SERVER=OFF -DBUILD_CLIENT=ON \
    -DFREEVGUI_SRC_DIR="$REPO_ROOT/3rdparty/freevgui" -DXASH3D_ENGINE_RUNTIME_DIR="$REPO_ROOT" \
    -DCMAKE_PROJECT_INCLUDE="$SCRIPTDIR/hlsdk_overrides.cmake"
cmake --build "$SOURCE_DIR/build-ios" --target install --parallel
CLIENT_LIBRARY="$LIBSDIR/svencoop/cl_dlls/client_ios_arm64.dylib"
[ -f "$CLIENT_LIBRARY" ] || { echo "Missing iOS client library: $CLIENT_LIBRARY"; exit 1; }
rm -f "$LIBSDIR/svencoop/cl_dlls/client_arm64.dylib"
