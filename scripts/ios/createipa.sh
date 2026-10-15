#!/bin/bash
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILDDIR="$REPO_ROOT/build"
APP="$BUILDDIR/ios/svenclient.app"
[ -d "$BUILDDIR/SDL2.framework" ] || { echo "Place SDL2.framework in build/ before packaging."; exit 1; }
[ -d "$BUILDDIR/ios/libs/svencoop" ] || { echo "Build the Sven Co-op client library before packaging."; exit 1; }
rm -rf "$APP"
mkdir -p "$APP"
cp -R "$BUILDDIR/ios/libs/svencoop" "$APP/"
cp -R "$BUILDDIR/SDL2.framework" "$APP/"
cp "$REPO_ROOT/engine/platform/ios/bundle/Info.plist" "$APP/Info.plist"
cd "$REPO_ROOT"
./waf build install --destdir="$APP"
xcrun actool "$REPO_ROOT/ios/Resources/Assets.xcassets" --compile "$APP" \
    --app-icon AppIcon --platform iphoneos --minimum-deployment-target 13.0 \
    --output-partial-info-plist "$APP/ActoolInfo.plist"
/usr/libexec/PlistBuddy -c "Merge $APP/ActoolInfo.plist" "$APP/Info.plist"
rm "$APP/ActoolInfo.plist"
xcrun ibtool --compile "$APP/LaunchScreenStoryboard.storyboardc" \
    "$REPO_ROOT/ios/Xash3D-iOS/view/LaunchScreenStoryboard.storyboard" --minimum-deployment-target 13.0
find "$APP" -name '*.dylib' -type f -exec codesign --force --sign - {} \;
codesign --force --sign - "$APP/SDL2.framework"
codesign --entitlements "$REPO_ROOT/engine/platform/ios/bundle/entitlements.plist" --sign - --force "$APP"
rm -rf "$BUILDDIR/ios/Payload"
mkdir -p "$BUILDDIR/ios/Payload"
mv "$APP" "$BUILDDIR/ios/Payload/"
rm -f "$BUILDDIR/svenclient.ipa"
cd "$BUILDDIR/ios"
zip -q -r ../svenclient.ipa Payload
