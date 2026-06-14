#!/bin/bash

set -e

cd "${0%/*}" || exit 1

BUILDDIR=$(realpath ../../ios/build)
APP_PATH="$BUILDDIR/Build/Products/Release-iphoneos/Xash3D.app"
ROOT_DIR=$(realpath ../../)

if [ ! -d "$APP_PATH" ]; then
    echo "Couldn't find Xash3D.app at $APP_PATH"
    echo "Make sure xcodebuild succeeded."
    exit 1
fi

cd "$BUILDDIR" || exit 1

rm -rf Payload/
mkdir Payload
cp -r "$APP_PATH" Payload/

# Sign with ad-hoc identity
codesign --entitlements "$ROOT_DIR/engine/platform/ios/bundle/entitlements.plist" \
    --sign "-" --force \
    --timestamp=none \
    "Payload/Xash3D.app" 2>/dev/null || true

OUTPUT_DIR="$ROOT_DIR/build"
mkdir -p "$OUTPUT_DIR"
if [ -f "$OUTPUT_DIR/xash3d.ipa" ]; then
    rm "$OUTPUT_DIR/xash3d.ipa"
fi
zip -q -r "$OUTPUT_DIR/xash3d.ipa" Payload
rm -rf Payload/

echo "Built xash3d.ipa successfully at $OUTPUT_DIR/xash3d.ipa"
exit 0
