#!/bin/bash
set -euo pipefail
. scripts/lib.sh
cd "$GITHUB_WORKSPACE" || die
bash scripts/ios/tests/run.sh
./waf configure --enable-lto --ios build install --destdir=build/ios || die_configure
cp -R /Library/Frameworks/SDL2.framework ./build
bash scripts/ios/buildhlsdk.sh
bash scripts/ios/createipa.sh
mkdir -p artifacts
mv build/svenclient.ipa artifacts/svenclient-ios-arm64.ipa
