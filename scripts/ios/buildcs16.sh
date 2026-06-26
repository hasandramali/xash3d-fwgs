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

# Fix SIGSEGV at 0x10 in GetRarityOfKill: TheCSBots() returns nullptr because
# ReGameDLL_CS bot system is not initialized when yapb manages bots.
# Accessing this->m_activeGrenadeList.begin() on a null CCSBotManager pointer
# dereferences __end_.__next_ at offset 0x10, causing the crash.
python3 -c "
import sys
fn = 'mod-build/cs16-client/3rdparty/ReGameDLL_CS/regamedll/dlls/multiplay_gamerules.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = (
    b'\t\t\tif (TheCSBots()->IsLineBlockedBySmoke(&inEyePos, &pVictim->pev->origin))' + eol
)
new = (
    b'\t\t\tif (TheCSBots() != nullptr && TheCSBots()->IsLineBlockedBySmoke(&inEyePos, &pVictim->pev->origin))' + eol
)
if old in data:
    count = data.count(old)
    if count > 1:
        print(f'WARNING: {count} matches, expect 1. Patching anyway.')
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: TheCSBots null check in GetRarityOfKill')
    sys.exit(0)
elif new in data:
    print('SKIP: already patched')
    sys.exit(0)
else:
    print('WARNING: pattern not found')
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

# Disable radar by default. Player can type "drawradar" to enable it.
python3 -c "
import sys
fn = 'mod-build/cs16-client/cl_dll/hud/radar.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = b'void CHudRadar::InitHUDData( void )' + eol + b'{' + eol + b'\tUserCmd_ShowRadar();' + eol + b'\tReset();' + eol + b'}'
new = b'void CHudRadar::InitHUDData( void )' + eol + b'{' + eol + b'\tUserCmd_HideRadar();' + eol + b'\tReset();' + eol + b'}'
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: radar disabled by default (use drawradar to enable)')
else:
    print('WARNING: pattern not found in radar.cpp')
    sys.exit(0)
"

# Disable sniper scope arc background and default crosshair.
# Keep only the CS 1.6 zoom sprite (sniper_scope.spr) from ammo.cpp.
python3 -c "
import sys
fn = 'mod-build/cs16-client/cl_dll/hud/sniperscope.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = (
    b'int CHudSniperScope::Draw(float flTime)' + eol +
    b'{' + eol +
    b'\tif(gHUD.m_iFOV > 40)' + eol +
    b'\t\treturn 1;' + eol +
    eol +
    b'\tgEngfuncs.pTriAPI->RenderMode(kRenderTransColor);' + eol +
    b'\tgEngfuncs.pTriAPI->Brightness(1.0);' + eol +
    b'\tgEngfuncs.pTriAPI->Color4ub(0, 0, 0, 255);' + eol +
    b'\tgEngfuncs.pTriAPI->CullFace(TRI_NONE);' + eol +
    eol +
    b'\tgRenderAPI.GL_SelectTexture(0);' + eol +
    eol +
    b'\tDrawTexture( m_iScopeArc[0], left, 0, centerx, centery );' + eol +
    b'\tDrawTexture( m_iScopeArc[1], centerx, 0, right, centery );' + eol +
    b'\tDrawTexture( m_iScopeArc[2], centerx, centery, right, TrueHeight );' + eol +
    b'\tDrawTexture( m_iScopeArc[3], left, centery, centerx, TrueHeight );' + eol +
    eol +
    b'\tgRenderAPI.GL_Bind( 0, gHUD.m_WhiteTex );' + eol +
    b'\t// gEngfuncs.pTriAPI->Begin( TRI_QUADS );' + eol +
    b'\t\tDrawUtils::Draw2DQuad( 0, 0, left + 2, TrueHeight );' + eol +
    b'\t\tDrawUtils::Draw2DQuad( right, 0, right + ( TrueWidth - right ), TrueHeight );' + eol +
    b'\t' + eol +
    b'\t// default crosshair pixel perfect lines' + eol +
    b'\t\tDrawUtils::Draw2DQuad( left, centery + 1, right, centery + 2 );' + eol +
    b'\t\tDrawUtils::Draw2DQuad( centerx - 1, 0, centerx, TrueHeight );' + eol +
    b'\t// gEngfuncs.pTriAPI->End();' + eol +
    eol +
    b'\treturn 0;' + eol +
    b'}'
)
new = (
    b'int CHudSniperScope::Draw(float flTime)' + eol +
    b'{' + eol +
    b'\t// disabled - only CS 1.6 zoom sprite (sniper_scope.spr) is used' + eol +
    b'\treturn 1;' + eol +
    b'}'
)
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: sniper scope arcs and default crosshair disabled')
else:
    print('WARNING: pattern not found in sniperscope.cpp')
    sys.exit(0)
"

# Enable numericalmenu on iOS (was Android-only).
python3 -c "
import sys
fn = 'mod-build/cs16-client/cl_dll/menu.cpp'
with open(fn, 'rb') as f:
    data = f.read()
eol = b'\r\n' if b'\r\n' in data else b'\n'
old = b'#ifdef __ANDROID__' + eol + b'\t\tszCmd = \"exec touch/numerical_menu.cfg\";' + eol + b'\t\tbreak;' + eol + b'#else' + eol + b'\t\treturn;' + eol + b'#endif'
new = b'#if defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IOS)' + eol + b'\t\tszCmd = \"exec touch/numerical_menu.cfg\";' + eol + b'\t\tbreak;' + eol + b'#else' + eol + b'\t\treturn;' + eol + b'#endif'
if old in data:
    data = data.replace(old, new, 1)
    with open(fn, 'wb') as f:
        f.write(data)
    print('PATCHED: numericalmenu enabled on iOS')
else:
    print('WARNING: pattern not found in menu.cpp')
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
