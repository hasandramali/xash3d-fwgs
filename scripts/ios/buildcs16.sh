#!/bin/bash

#cd into script directory
SCRIPTDIR=${0%/*}
cd $SCRIPTDIR

MODPATH=mod-build/cs16-client
git clone --recursive https://github.com/Velaron/cs16-client mod-build/cs16-client

mkdir -p ../../build/ios/libs
LIBSDIR=$(realpath ../../build/ios/libs)
cd $MODPATH

XCODE_DEV=$(xcode-select -p 2>/dev/null || echo "/Applications/Xcode.app/Contents/Developer")
cmake -DCMAKE_OSX_SYSROOT="$XCODE_DEV/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS.sdk" -DCMAKE_DEVELOPER_ROOT="$XCODE_DEV/Platforms/iPhoneOS.platform/Developer" -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DMAC=0 -DDEBUG=1 -DXASH_COMPAT=1 -DMAINUI_USE_STB=1 -DCMAKE_BUILD_TYPE=Debug -B build -S .
cmake --build build --config Debug
cmake --install build --prefix $LIBSDIR

cd ../../

# Rename dylibs to include arch suffix (engine expects _arm64)
find "$LIBSDIR" -name "*.dylib" -type f | while read f; do
    dir=$(dirname "$f")
    base=$(basename "$f" .dylib)
    # Only rename if no arch suffix already
    if [[ "$base" != *_arm64 ]] && [[ "$base" != *_x86* ]] && [[ "$base" != *_i386 ]]; then
        mv "$f" "$dir/${base}_arm64.dylib"
    fi
done

if [ -d mod-build ]; then
    rm -rf mod-build/
fi

exit 0
