#!/bin/bash
# Stage the same patched GNU Make 3.82 sources used by the host port for an
# in-OS GNU Make + GCC rebuild.  No Make object or runtime object is seeded.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-/tmp/icsos-make-gcc}"

rm -rf "$DEST"
mkdir -p "$DEST"
"$ROOT/scripts/stage-make.sh" "$DEST/src"
mkdir -p "$DEST/sdk/include"
cp "$ROOT"/sdk/{tccsdk.c,posix.c,libtcc1.c,crt1.c,setjmp.c,dexsdk.h,time.h} "$DEST/sdk/"
cp -a "$ROOT/sdk/include/." "$DEST/sdk/include/"
cp "$ROOT/contrib/gnumake/Selfhost.mk" "$DEST/Selfhost.mk"

echo "staged GCC/GNU Make native rebuild under $DEST"
