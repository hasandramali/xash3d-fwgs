# Xash3D iOS Project Memory

## Problem: Black screen after Launch on iOS
- Launcher UI works (xash_ios.log shows complete flow)
- engine.log stops at "Adding directory: ./"
- My host.c Con_Printf logging changes NOT appearing in engine.log
- Root cause: Xcode doesn't compile engine C files; `libxash.a` must be rebuilt via `./waf build` first
- GameDataDownloader's `initLogPath` was truncating xash_ios.log - FIXED by removing the truncation line

## Files Modified in This Session

### iOS Launcher Files (compiled by Xcode - changes DO appear):
1. `ios/Xash3D-iOS/view/FileBrowserViewController.mm` - Added verbose IOS_Log calls for Launch flow
2. `ios/Xash3D-iOS/model/launchdialog.mm` - Added logging around run loop exit, window hiding; changed fopen("a") to "w" for truncation
3. `ios/Xash3D-iOS/model/GameDataDownloader.mm` - REMOVED initLogPath file truncation (was clearing launcher logs)

### Engine C Files (need waf rebuild - changes NOT yet visible):
4. `engine/common/launcher.c` - Added logging between IOS_LaunchDialog return and Host_Main call
5. `engine/common/host.c` - Added step-by-step Con_Printf logging through Host_InitCommon and Host_Main
6. `engine/platform/sdl2/sys_sdl2.c` - Added SDL_Init result logging
7. `engine/platform/sdl2/vid_sdl2.c` - Added window creation, GL context, showing logging

## Build System
- Engine C files: built by waf into `libxash.a` (static library)
- iOS UI (.mm files): built by Xcode project at `ios/Xash3D-iOS.xcodeproj`
- Xcode links against pre-built `libxash.a` from DerivedData
- `./waf configure --ios && ./waf build` must be run BEFORE Xcode build to update engine
- `scripts/ios/createipa.sh` packages final .ipa

## User's cmdfilter.ini Feature
- Added to `engine/common/cmd.c`
- Loads commands to filter from cmdfilter.ini
- Works on Android but untested on iOS

## Pending: Migrate to wscript-based iOS build
- Goal: Build iOS UI files via waf instead of separate Xcode project
- xash3d-broken/ios/ has same source files
- Need to create ios/wscript for compiling .mm files
- Enable LAUNCHER on iOS in main wscript
- Need to handle: UIKit, Foundation linking, .app bundle creation
