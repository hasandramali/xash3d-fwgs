#!/bin/bash

#cd into script directory
cd "${0%/*}" || exit 1

BUILDDIR=$(realpath ../../build)

cd "../../engine/platform/ios/bundle" || exit 1

if [ -d "$BUILDDIR" ]; then
    mkdir -p "$BUILDDIR/ios/xash3d.app"

    cp -r "$BUILDDIR/ios/libs/"* "$BUILDDIR/ios/xash3d.app"
    cp Info.plist "$BUILDDIR/ios/xash3d.app"
    if [ ! -d "$BUILDDIR/SDL2.framework" ]; then
        echo "Couldn't find SDL2.framework, place it in the build directory"
        exit 1
    fi
    cp -r "$BUILDDIR/SDL2.framework" "$BUILDDIR/ios/xash3d.app"

    cd ../../../../ || exit 1

    ./waf build install --destdir="$BUILDDIR/ios/xash3d.app"
    #echo "Generating dSYMs"
    #find "$BUILDDIR/ios/xash3d.app" -name "*.dylib" -type f -exec dsymutil {} \;
    #dsymutil "$BUILDDIR/ios/xash3d.app/xash"

    cd "$BUILDDIR" || exit 1

    # Compile asset catalog for app icon
    ASSETS_DIR=$(realpath ../ios/Resources/Assets.xcassets 2>/dev/null)
    if [ -d "$ASSETS_DIR" ] && command -v xcrun &>/dev/null; then
        echo "Compiling asset catalog..."
        xcrun actool "$ASSETS_DIR" --compile ios/xash3d.app --app-icon AppIcon --platform iphoneos --minimum-deployment-target 10.0 --output-partial-info-plist ios/xash3d.app/ActoolInfo.plist 2>&1 || echo "Warning: asset catalog compilation failed, icon may be missing"
        # Merge generated icon entries into Info.plist so iOS recognizes the icon
        if [ -f ios/xash3d.app/ActoolInfo.plist ]; then
            /usr/libexec/PlistBuddy -c "Merge ios/xash3d.app/ActoolInfo.plist" ios/xash3d.app/Info.plist 2>&1 || echo "Warning: could not merge icon entries into Info.plist"
            rm ios/xash3d.app/ActoolInfo.plist
        fi
    else
        echo "Warning: Assets.xcassets not found or xcrun not available, icon will not be compiled"
    fi

    rm -r "$BUILDDIR/ios/Payload/"
    mkdir "$BUILDDIR/ios/Payload"

    cp -r "$BUILDDIR/ios/xash3d.app" ios/Payload/
    rm -r "$BUILDDIR/ios/xash3d.app"
    cd ios || exit 1

    # Patch all Mach-Os to report iOS 10.0 as the minimum OS version.
    # Xcode 26 cannot link below iOS 12, so we build at 12.0 and lower the
    # recorded minimum with vtool so the app installs and runs on iOS 10.
    if command -v vtool &>/dev/null; then
        SDK_VERSION=$(xcrun --sdk iphoneos --show-sdk-version 2>/dev/null || echo "26.0")
        patch_minos() {
            local f="$1"
            vtool -set-build-version ios 10.0 "$SDK_VERSION" -o "$f" "$f" 2>/dev/null || return 1
            vtool -add-version-min 10.0 -o "$f" "$f" 2>/dev/null || true
            echo "Patched $f minimum OS to iOS 10"
        }
        for f in Payload/xash3d.app/xash Payload/xash3d.app/SDL2.framework/SDL2; do
            [ -f "$f" ] && patch_minos "$f"
        done
        while read -r f; do patch_minos "$f"; done < <(find Payload/xash3d.app -name '*.dylib' -type f)
    else
        echo "Warning: vtool not found, app will require iOS 12"
    fi

    codesign --entitlements "$(realpath ../../engine/platform/ios/bundle/entitlements.plist)" --sign "-" --force Payload/xash3d.app
    if [ -e ../xash3d.ipa ]; then
        rm ../xash3d.ipa
    fi
    zip -q -r ../xash3d.ipa Payload
else
    echo "Couldn't find the build directory, compile the engine before running this script!"
    exit 1
fi

exit 0
