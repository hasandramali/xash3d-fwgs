#!/bin/bash

cd "$GITHUB_WORKSPACE" || exit 1

git clone https://github.com/libsdl-org/SDL -b "release-$SDL_VERSION"

cd SDL/Xcode/SDL || exit 1
xcodebuild -scheme xcFramework-iOS -target xcFramework-iOS build -configuration Release
sudo cp -vr Products/SDL2.xcframework/ios-arm64/SDL2.framework /Library/Frameworks

cd "$GITHUB_WORKSPACE" || exit 1

git clone --depth 1 https://github.com/hasandramali/hlsdk-portable -b svencoop hlsdk
