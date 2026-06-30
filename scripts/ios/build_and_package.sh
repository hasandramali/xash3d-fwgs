#!/bin/bash

# Build all game libraries and create IPA
# Usage: ./build_and_package.sh

SCRIPTDIR=${0%/*}
cd "$SCRIPTDIR" || exit 1

echo "=== Building all game libraries (mods) ==="
./buildallmods.sh || exit 1

echo "=== Building CS16 (last) ==="
./buildcs16.sh || exit 1

echo "=== Building bot10 ==="
./buildhlsdk.sh bot10 valve || exit 1

echo "=== Building opfor ==="
./buildhlsdk.sh opfor gearbox || exit 1

echo "=== Creating IPA ==="
./createipa.sh || exit 1

echo "=== Done ==="
exit 0
