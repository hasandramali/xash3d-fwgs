#!/bin/bash
set -euo pipefail
REPO_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE="${1:-$REPO_ROOT/ic_launcher.jpg}"
CATALOG="$REPO_ROOT/ios/Resources/Assets.xcassets"
python3 - "$CATALOG/AppIcon.appiconset/Contents.json" <<'PY' | while read -r size filename; do
import json, sys
for image in json.load(open(sys.argv[1]))['images']:
    size = round(float(image['size'].split('x')[0]) * float(image['scale'].rstrip('x')))
    print(size, image['filename'])
PY
    sips -s format png -z "$size" "$size" "$SOURCE" --out "$CATALOG/AppIcon.appiconset/$filename" >/dev/null
done
for entry in '128 Logo-1.png' '256 Logo.png' '384 Logo-2.png'; do
    read -r size filename <<< "$entry"
    sips -s format png -z "$size" "$size" "$SOURCE" --out "$CATALOG/Logo.imageset/$filename" >/dev/null
done
