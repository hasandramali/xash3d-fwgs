#!/bin/bash

cd "$GITHUB_WORKSPACE" || exit 1

# Allow deploying to iOS < 12 (older devices stuck on iOS 10/11).
# Apple's newer Xcode SDKs hard-floor deployment targets at iOS 12; SDL documents
# extending this by adding older versions to SDKSettings.plist
# (see https://wiki.libsdl.org/SDL2/README-ios#deploying-to-older-versions-of-ios).
SDK=$(xcrun --sdk iphoneos --show-sdk-path 2>/dev/null || true)
if [ -n "$SDK" ] && [ -f "$SDK/SDKSettings.plist" ]; then
    echo "Patching $SDK/SDKSettings.plist to accept iOS 10 deployment target"
    for VER in 11.0 10.0; do
        /usr/libexec/PlistBuddy -c "Add :DefaultProperties:DEPLOYMENT_TARGET_SUGGESTED_VALUES:0 string $VER" "$SDK/SDKSettings.plist" 2>/dev/null || true
    done
else
    echo "Warning: could not patch iOS SDK deployment targets, min target may be limited to iOS 12"
fi

git clone https://github.com/libsdl-org/SDL -b "release-$SDL_VERSION"

cd SDL/Xcode/SDL || exit 1
xcodebuild -scheme xcFramework-iOS -target xcFramework-iOS build -configuration Release
sudo cp -vr Products/SDL2.xcframework/ios-arm64/SDL2.framework /Library/Frameworks

cd "$GITHUB_WORKSPACE" || exit 1

git clone https://github.com/FWGS/hlsdk-portable hlsdk -b mobile_hacks --depth=1
