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
        xcrun actool "$ASSETS_DIR" --compile ios/xash3d.app --app-icon AppIcon --platform iphoneos --minimum-deployment-target 13.0 2>&1 || echo "Warning: asset catalog compilation failed, icon may be missing"
    else
        echo "Warning: Assets.xcassets not found or xcrun not available, icon will not be compiled"
    fi

    rm -r "$BUILDDIR/ios/Payload/"
    mkdir "$BUILDDIR/ios/Payload"

    cp -r "$BUILDDIR/ios/xash3d.app" ios/Payload/
    rm -r "$BUILDDIR/ios/xash3d.app"
    cd ios || exit 1
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
