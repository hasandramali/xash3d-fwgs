#!/bin/bash
set -euo pipefail
cd "$GITHUB_WORKSPACE"
git clone --branch "release-$SDL_VERSION" --single-branch https://github.com/libsdl-org/SDL SDL
cd SDL/Xcode/SDL
xcodebuild -scheme xcFramework-iOS -target xcFramework-iOS build -configuration Release
sudo cp -R Products/SDL2.xcframework/ios-arm64/SDL2.framework /Library/Frameworks
