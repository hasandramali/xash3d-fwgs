#!/bin/bash

. scripts/lib.sh

cd "$GITHUB_WORKSPACE" || die

# Build engine and 3rdparty libs with cmake + ios-cmake toolchain
# -B ios/cmake-build so the Xcode project finds the static libraries
cmake -G Xcode \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="${PWD}/3rdparty/ios-cmake/ios.toolchain.cmake" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DPLATFORM=OS64 \
    -DXASH_STATIC_GAMELIB=1 \
    -DXASH_GLES=1 \
    -DDEPLOYMENT_TARGET=13.0 \
    -B ios/cmake-build \
    -S . || die

cmake --build ios/cmake-build --config Release || die

# Flatten CMake-built static libraries for the hand-written launcher project.
# Xcode's generated product path changes between generators/toolchains, so the
# launcher links from this stable directory instead of guessing per-target paths.
rm -rf ios/cmake-build/libs || die
mkdir -p ios/cmake-build/libs || die
find ios/cmake-build -path ios/cmake-build/libs -prune -o -name 'lib*.a' -exec cp -f {} ios/cmake-build/libs/ \; || die

# Build hlsdk game libraries (bundled for reference, downloaded at runtime via GameLibDownloader)
pushd hlsdk || die
mkdir -p ../ios/libs || die
cmake -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX=$(realpath ../ios/libs) \
    -DCMAKE_BUILD_TYPE=Debug -B build -S .
cmake --build build --target install || die
popd || die

# Ensure SDL2.framework is available for Xcode packaging and embedding.
# Try system frameworks first, then repo root. Copy it to ./build so createipa.sh finds it.
if [ -d "/Library/Frameworks/SDL2.framework" ]; then
    mkdir -p build
    cp -R "/Library/Frameworks/SDL2.framework" "build/SDL2.framework"
elif [ -d "./SDL2.framework" ]; then
    mkdir -p build
    cp -R "./SDL2.framework" "build/SDL2.framework"
else
    echo "SDL2.framework not found in /Library/Frameworks or repo root."
    echo "Place SDL2.framework in the runner's /Library/Frameworks or add it to the repository root as SDL2.framework."
    exit 1
fi

# Build the iOS launcher Xcode project
# The Xcode project references the cmake sub-project at ios/cmake-build/
# and links all engine static libs into the final app
xcodebuild \
    -project ios/Xash3D-iOS.xcodeproj \
    -scheme Xash3D-iOS \
    -configuration Release \
    -sdk iphoneos \
    -derivedDataPath ios/build \
    CODE_SIGN_IDENTITY="" \
    CODE_SIGNING_REQUIRED=NO \
    CODE_SIGNING_ALLOWED=NO \
    build || die

# Embed SDL2.framework into the app bundle (required for @rpath loading at runtime)
APP_PATH="ios/build/Build/Products/Release-iphoneos/Xash3D.app"
FRAMEWORKS_DIR="$APP_PATH/Frameworks"
if [ -d "build/SDL2.framework" ]; then
    mkdir -p "$FRAMEWORKS_DIR"
    cp -Rf "build/SDL2.framework" "$FRAMEWORKS_DIR/"
    # Re-sign the embedded framework (ad-hoc)
    codesign --sign "-" --force "$FRAMEWORKS_DIR/SDL2.framework" 2>/dev/null || true
    echo "Embedded SDL2.framework into $FRAMEWORKS_DIR"
fi

# Package into .ipa
./scripts/ios/createipa.sh

mkdir -p artifacts/
mv "build/xash3d.ipa" "artifacts/xash3d-fwgs-ios-arm64.ipa"
