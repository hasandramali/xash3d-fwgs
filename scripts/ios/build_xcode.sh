#!/bin/bash
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO_ROOT"
./waf configure --ios --enable-lto
APP="$TARGET_BUILD_DIR/$WRAPPER_NAME"
mkdir -p "$APP"
./waf build install --destdir="$APP"
mv "$APP/xash" "$APP/$EXECUTABLE_NAME"
bash scripts/ios/prepare_game_libs.sh
SDL_FRAMEWORK="${SDL_FRAMEWORK:-/Library/Frameworks/SDL2.framework}"
cp -R "$SDL_FRAMEWORK" "$APP/"
if [ "${CODE_SIGNING_ALLOWED:-YES}" != NO ] && [ -n "${EXPANDED_CODE_SIGN_IDENTITY:-}" ]; then
    find "$APP" -name '*.dylib' -type f -exec codesign --force --sign "$EXPANDED_CODE_SIGN_IDENTITY" {} \;
    codesign --force --sign "$EXPANDED_CODE_SIGN_IDENTITY" "$APP/SDL2.framework"
fi
