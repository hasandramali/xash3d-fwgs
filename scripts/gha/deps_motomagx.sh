#!/bin/bash

cd "$GITHUB_WORKSPACE" || exit 1

sudo dpkg --add-architecture i386
sudo apt update
sudo apt install libc6:i386 libstdc++6:i386 gcc-multilib g++-multilib p7zip-full

sudo mkdir -p /opt/toolchains

pushd /opt/toolchains/ || exit 1
sudo git clone https://github.com/a1batross/motomagx_toolchain motomagx --depth=1
popd || exit 1

git clone --depth 1 https://github.com/hasandramali/hlsdk-portable -b svencoop hlsdk
