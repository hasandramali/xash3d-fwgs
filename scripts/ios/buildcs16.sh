#!/bin/bash

#cd into script directory
SCRIPTDIR=${0%/*}
cd $SCRIPTDIR

MODPATH=mod-build/cs16-client
git clone --recursive https://github.com/hasandramali/cs16-client mod-build/cs16-client

# disable yapb build (use if crashes)
#sed -i '' '/add_subdirectory(3rdparty\/yapb)/d; s/set_target_postfix(yapb)/# set_target_postfix(yapb)/' mod-build/cs16-client/CMakeLists.txt

# Disable InstallBotControl on iOS (PAC crash / SIGSEGV in BotPhraseManager::Initialize
# on arm64e A14+). CSTRIKE is not defined in this build, so #ifndef CSTRIKE guard fails.
python3 -c "
import sys
fn = 'mod-build/cs16-client/3rdparty/ReGameDLL_CS/regamedll/dlls/multiplay_gamerules.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = b'#ifndef CSTRIKE' + eol + b'\tInstallBotControl();' + eol + b'#endif' + eol
new = b'#if 0' + eol + b'\tInstallBotControl();' + eol + b'#endif' + eol
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: InstallBotControl disabled (#if 0)')
else:
    print('WARNING: pattern not found in multiplay_gamerules.cpp')
    sys.exit(0)
"

# Fix SIGSEGV at address 0x10 in GetRarityOfKill when a yapb bot kills another bot.
# pVictim->pev can be NULL (probably due to yapb's bot entity state), causing
# &pVictim->pev->origin to compute to 0x10, which crashes when IsLineBlockedBySmoke
# dereferences it. Add a defensive null check for pVictim.
python3 -c "
import sys
fn = 'mod-build/cs16-client/3rdparty/ReGameDLL_CS/regamedll/dlls/multiplay_gamerules.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = (
    b'int CHalfLifeMultiplay::GetRarityOfKill(CBaseEntity *pKiller, CBasePlayer *pVictim, CBasePlayer *pAssister, const char *killerWeaponName, bool bFlashAssist)' + eol +
    b'{' + eol +
    b'\tint iRarity = 0;' + eol
)
new = (
    b'int CHalfLifeMultiplay::GetRarityOfKill(CBaseEntity *pKiller, CBasePlayer *pVictim, CBasePlayer *pAssister, const char *killerWeaponName, bool bFlashAssist)' + eol +
    b'{' + eol +
    b'\tint iRarity = 0;' + eol +
    eol +
    b'\tif (!pVictim)' + eol +
    b'\t\treturn iRarity;' + eol
)
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: pVictim null check in GetRarityOfKill')
    sys.exit(0)
elif new in data:
    print('SKIP: already patched')
    sys.exit(0)
else:
    print('WARNING: pattern not found in multiplay_gamerules.cpp')
    sys.exit(0)
"

# Make TheBotPhrases a global static object instead of a nullptr pointer.
# CCSBotManager (which sets TheBotPhrases) uses C++ operator new and virtual
# functions that trigger PAC IB trap on arm64e (A14+). By pre-allocating
# TheBotPhrases as a static global, the location code (GetPlaceList) works
# without needing to instantiate CCSBotManager.
python3 -c "
import sys
fn = 'mod-build/cs16-client/3rdparty/ReGameDLL_CS/regamedll/dlls/bot/cs_bot_chatter.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = b'BotPhraseManager *TheBotPhrases = nullptr;' + eol
new = (
    b'static BotPhraseManager s_BotPhraseManager;' + eol +
    b'BotPhraseManager *TheBotPhrases = &s_BotPhraseManager;' + eol
)
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: TheBotPhrases is now a global static object')
else:
    print('WARNING: pattern not found in cs_bot_chatter.cpp')
    sys.exit(0)
"

# Fix crash in CBasePlayer::UpdateLocation: TheBotPhrases can be NULL
# TheBotPhrases->GetPlaceList() returns &m_placeList at NULL+offset=0x20
python3 -c "
import sys
fn = 'mod-build/cs16-client/3rdparty/ReGameDLL_CS/regamedll/dlls/player.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = (
    b'\t\tconst BotPhraseList *placeList = TheBotPhrases->GetPlaceList();' + eol +
    b'\t\tfor (auto phrase : *placeList)' + eol +
    b'\t\t{' + eol +
    b'\t\t\tif (phrase->GetID() == playerPlace)' + eol +
    b'\t\t\t{' + eol +
    b'\t\t\t\tplaceName = phrase->GetName();' + eol +
    b'\t\t\t\tbreak;' + eol +
    b'\t\t\t}' + eol +
    b'\t\t}' + eol
)
new = (
    b'\t\tif (TheBotPhrases)' + eol +
    b'\t\t{' + eol +
    b'\t\t\tconst BotPhraseList *placeList = TheBotPhrases->GetPlaceList();' + eol +
    b'\t\t\tfor (auto phrase : *placeList)' + eol +
    b'\t\t\t{' + eol +
    b'\t\t\t\tif (phrase->GetID() == playerPlace)' + eol +
    b'\t\t\t\t{' + eol +
    b'\t\t\t\t\tplaceName = phrase->GetName();' + eol +
    b'\t\t\t\t\tbreak;' + eol +
    b'\t\t\t\t}' + eol +
    b'\t\t\t}' + eol +
    b'\t\t}' + eol
)
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: TheBotPhrases null guard in UpdateLocation (player.cpp)')
else:
    print('WARNING: pattern not found in player.cpp')
    sys.exit(0)
"

# Fix similar crash in client.cpp (same TheBotPhrases->GetPlaceList() call)
python3 -c "
import sys
fn = 'mod-build/cs16-client/3rdparty/ReGameDLL_CS/regamedll/dlls/client.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = (
    b'\t\t\tconst BotPhraseList *placeList = TheBotPhrases->GetPlaceList();' + eol +
    eol +
    b'\t\t\tfor (auto phrase : *placeList)' + eol +
    b'\t\t\t{' + eol +
    b'\t\t\t\tif (phrase->GetID() == playerPlace)' + eol +
    b'\t\t\t\t{' + eol +
    b'\t\t\t\t\tplaceName = phrase->GetName();' + eol +
    b'\t\t\t\t\tbreak;' + eol +
    b'\t\t\t\t}' + eol +
    b'\t\t\t}' + eol
)
new = (
    b'\t\t\tif (TheBotPhrases)' + eol +
    b'\t\t\t{' + eol +
    b'\t\t\t\tconst BotPhraseList *placeList = TheBotPhrases->GetPlaceList();' + eol +
    eol +
    b'\t\t\t\tfor (auto phrase : *placeList)' + eol +
    b'\t\t\t\t{' + eol +
    b'\t\t\t\t\tif (phrase->GetID() == playerPlace)' + eol +
    b'\t\t\t\t\t{' + eol +
    b'\t\t\t\t\t\tplaceName = phrase->GetName();' + eol +
    b'\t\t\t\t\t\tbreak;' + eol +
    b'\t\t\t\t\t}' + eol +
    b'\t\t\t\t}' + eol +
    b'\t\t\t}' + eol
)
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: TheBotPhrases null guard in client.cpp')
else:
    print('WARNING: pattern not found in client.cpp')
    sys.exit(0)
"

# Fix yapb loadCSBinary() on iOS: the relative path "cstrike/dlls/cs_arm64.dylib" doesn't
# resolve from the iOS Documents working directory. Fall back to the yapb library's own
# directory (absolute bundle path) when the relative lookup fails.
python3 -c "
import sys
fn = 'mod-build/cs16-client/3rdparty/yapb/src/engine.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\n'
old = (
    b'      if (path.empty ()) {' + eol +
    b'         path = strings.joinPath (modname, \"dlls\", lib) + kLibrarySuffix;' + eol +
    eol +
    b'         // if we can\\'t read file, skip it' + eol +
    b'         if (!plat.fileExists (path.chars ())) {' + eol +
    b'            continue;' + eol +
    b'         }' + eol +
    b'      }'
)
new = (
    b'      if (path.empty ()) {' + eol +
    b'         path = strings.joinPath (modname, \"dlls\", lib) + kLibrarySuffix;' + eol +
    eol +
    b'         // if we can\\'t read file, skip it' + eol +
    b'         if (!plat.fileExists (path.chars ())) {' + eol +
    b'            // fallback: try the bot library directory (iOS bundle)' + eol +
    b'            auto libpath = SharedLibrary::path (&bstor);' + eol +
    eol +
    b'            if (!libpath.empty ()) {' + eol +
    b'               auto dir = libpath.substr (0, libpath.rfind (kPathSeparator));' + eol +
    b'               path = strings.joinPath (dir, lib) + kLibrarySuffix;' + eol +
    b'            }' + eol +
    eol +
    b'            if (!plat.fileExists (path.chars ())) {' + eol +
    b'               continue;' + eol +
    b'            }' + eol +
    b'         }' + eol +
    b'      }'
)
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: iOS dll path fallback in yapb loadCSBinary()')
    sys.exit(0)
elif new in data:
    print('SKIP: already patched')
    sys.exit(0)
else:
    print('WARNING: pattern not found in engine.cpp')
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
