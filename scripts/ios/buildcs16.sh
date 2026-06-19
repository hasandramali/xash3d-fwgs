#!/bin/bash

#cd into script directory
SCRIPTDIR=${0%/*}
cd $SCRIPTDIR

MODPATH=mod-build/cs16-client
git clone --recursive https://github.com/hasandramali/cs16-client mod-build/cs16-client

# PAC crash on arm64e (A14+): CCSBotManager C++ vtables trigger
# pointer authentication trap IB on arm64e HW. Disable built-in bots on iOS.
sed -i '' 's/#ifndef CSTRIKE/#if !defined(CSTRIKE) \&\& !(defined(__APPLE__) \&\& defined(__arm64__))/' mod-build/cs16-client/3rdparty/ReGameDLL_CS/regamedll/dlls/multiplay_gamerules.cpp

# Skip yapb build on iOS (not needed)
sed -i '' '/add_subdirectory(3rdparty\/yapb)/d; s/set_target_postfix(yapb)/# set_target_postfix(yapb)/' mod-build/cs16-client/CMakeLists.txt

# Fix crash in UpdateLocation: TheBotPhrases can be NULL on iOS (bots disabled)
# TheBotPhrases->GetPlaceList() returns &m_placeList at NULL+offset=0x20 when ptr is NULL
python3 -c "
import sys
fn = 'mod-build/cs16-client/3rdparty/ReGameDLL_CS/regamedll/dlls/player.cpp'
with open(fn, 'r') as f:
    c = f.read()
old = '''\t\tconst BotPhraseList *placeList = TheBotPhrases->GetPlaceList();
\t\tfor (auto phrase : *placeList)
\t\t{
\t\t\tif (phrase->GetID() == playerPlace)
\t\t\t{
\t\t\t\tplaceName = phrase->GetName();
\t\t\t\tbreak;
\t\t\t}
\t\t}'''
new = '''\t\tif (TheBotPhrases)
\t\t{
\t\t\tconst BotPhraseList *placeList = TheBotPhrases->GetPlaceList();
\t\t\tfor (auto phrase : *placeList)
\t\t\t{
\t\t\t\tif (phrase->GetID() == playerPlace)
\t\t\t\t{
\t\t\t\t\tplaceName = phrase->GetName();
\t\t\t\t\tbreak;
\t\t\t\t}
\t\t\t}
\t\t}'''
if old in c:
    c = c.replace(old, new, 1)
    with open(fn, 'w') as f:
        f.write(c)
    print('PATCHED: TheBotPhrases null guard in UpdateLocation')
else:
    print('WARNING: pattern not found in player.cpp')
    sys.exit(0)
"

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
