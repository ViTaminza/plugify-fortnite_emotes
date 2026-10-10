#!/bin/bash
# Assembles build/package/fortnite_emotes: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/fortnite_emotes"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/fortnite_emotes/libfortnite_emotes.so" "$OUT/bin/"
cp "$ROOT/build/plugins/fortnite_emotes/fortnite_emotes.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
