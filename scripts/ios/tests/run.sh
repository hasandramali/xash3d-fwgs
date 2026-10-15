#!/bin/bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror -pthread \
    "$SCRIPT_DIR/SteamWireTests.cpp" -o "$TEST_DIR/steam-wire-tests"
"$TEST_DIR/steam-wire-tests"
"${CC:-clang}" -std=gnu11 -c "$SCRIPT_DIR/../../../3rdparty/miniz/miniz.c" -o "$TEST_DIR/miniz.o"
"${CXX:-clang++}" -std=c++17 -Wall -Wextra -Werror \
    "$SCRIPT_DIR/SteamArchiveTests.cpp" "$TEST_DIR/miniz.o" -lz -o "$TEST_DIR/steam-archive-tests"
"$TEST_DIR/steam-archive-tests"
python3 "$SCRIPT_DIR/build_scripts_test.py"
