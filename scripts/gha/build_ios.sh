#!/bin/bash

. scripts/lib.sh

cd "$GITHUB_WORKSPACE" || die

./waf configure --enable-lto --ios build install --destdir=build/ios || die_configure

cp -vr /Library/Frameworks/SDL2.framework ./build

LIBSDIR=$(realpath build/ios/libs)
mkdir -p "$LIBSDIR"

# Build valve from local hlsdk submodule
pushd hlsdk || die
cmake -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_INSTALL_PREFIX="$LIBSDIR" -DCMAKE_BUILD_TYPE=Debug -B build -S .
cmake --build build --target install || die
popd || die

# Rename valve dylibs to arch suffix (engine expects _arm64)
find "$LIBSDIR" -name "*.dylib" -type f | while read f; do
    dir=$(dirname "$f")
    base=$(basename "$f" .dylib)
    if [[ "$base" != *_arm64 ]] && [[ "$base" != *_x86* ]] && [[ "$base" != *_i386 ]]; then
        mv "$f" "$dir/${base}_arm64.dylib"
    fi
done

# Build CS16 client
bash scripts/ios/buildcs16.sh || echo "Warning: cs16 build failed, continuing"

./scripts/ios/createipa.sh

mkdir -p artifacts/
mv "build/xash3d.ipa" "artifacts/xash3d-fwgs-ios-arm64.ipa"
