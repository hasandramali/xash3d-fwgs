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
    -DXASH_GL4ES=1 \
    -DXASH_GLES=1 \
    -DDEPLOYMENT_TARGET=13.0 \
    -B ios/cmake-build \
    -S . || die

cmake --build ios/cmake-build --config Release || die

# Build hlsdk game libraries (bundled for reference, downloaded at runtime via GameLibDownloader)
pushd hlsdk || die
mkdir -p ../ios/libs || die
cmake -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_INSTALL_PREFIX=$(realpath ../ios/libs) \
    -DCMAKE_BUILD_TYPE=Debug -B build -S .
cmake --build build --target install || die
popd || die

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

# Package into .ipa
./scripts/ios/createipa.sh

mkdir -p artifacts/
mv "build/xash3d.ipa" "artifacts/xash3d-fwgs-ios-arm64.ipa"
