#!/bin/bash
set -euo pipefail
SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPTDIR/../.." && pwd)
LIBSDIR="$REPO_ROOT/build/ios/libs"
if [ ! -f "$LIBSDIR/svencoop/cl_dlls/client_arm64.dylib" ]; then
    bash "$SCRIPTDIR/buildhlsdk.sh"
fi
if [ -n "${BUILT_PRODUCTS_DIR:-}" ] && [ -n "${EXECUTABLE_FOLDER_PATH:-}" ]; then
    DEST="$BUILT_PRODUCTS_DIR/$EXECUTABLE_FOLDER_PATH/svencoop"
    mkdir -p "$DEST"
    cp -R "$LIBSDIR/svencoop/." "$DEST/"
fi
