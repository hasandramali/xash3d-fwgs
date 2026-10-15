#!/bin/bash
set -euo pipefail
SCRIPTDIR=$(cd "$(dirname "$0")" && pwd)
bash "$SCRIPTDIR/buildhlsdk.sh"
bash "$SCRIPTDIR/createipa.sh"
