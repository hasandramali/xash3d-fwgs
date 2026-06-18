#!/bin/bash

#cd into script directory
SCRIPTDIR=${0%/*}
cd "$SCRIPTDIR" || exit 1

MODPATH=mod-build/$1
if [ -z "$1" ]; then
    MODPATH=mod-build/hlsdk
    git clone --recursive https://github.com/FWGS/hlsdk-portable -b mobile_hacks "$MODPATH"
else
    git clone --recursive https://github.com/FWGS/hlsdk-portable -b "$1" "$MODPATH"
fi

mkdir -p ../../build/ios/libs || exit 1


LIBSDIR=$(realpath ../../build/ios/libs)
cd "$MODPATH" || exit 1

#compiling hlsdk for ios release is broken, so just build for debug
if [ ! -z $2 ]; then
    cmake -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT=0 -DCMAKE_INSTALL_PREFIX="$LIBSDIR" -DGAMEDIR="$2" -DCMAKE_BUILD_TYPE=Debug -B build -S .
else
    cmake -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 -DCMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT=0 -DCMAKE_INSTALL_PREFIX="$LIBSDIR" -DCMAKE_BUILD_TYPE=Debug -B build -S .
fi
cmake --build build --target install

# Rename dylibs to include arch suffix (engine expects _arm64)
find "$LIBSDIR" -name "*.dylib" -type f | while read f; do
    dir=$(dirname "$f")
    base=$(basename "$f" .dylib)
    # Only rename if no arch suffix already
    if [[ "$base" != *_arm64 ]] && [[ "$base" != *_x86* ]] && [[ "$base" != *_i386 ]]; then
        mv "$f" "$dir/${base}_arm64.dylib"
    fi
done

cd "$SCRIPTDIR"
if [ -d "$MODPATH" ]; then
    rm -rf "$MODPATH"
fi

exit 0
