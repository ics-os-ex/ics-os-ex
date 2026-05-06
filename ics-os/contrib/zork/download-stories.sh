#!/usr/bin/env sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT_DIR="$SCRIPT_DIR/stories"

mkdir -p "$OUT_DIR"

fetch() {
  url="$1"
  out="$2"

  if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --retry-delay 2 -o "$out" "$url"
  elif command -v wget >/dev/null 2>&1; then
    wget -O "$out" "$url"
  else
    echo "Error: need curl or wget" >&2
    exit 1
  fi
}

fetch "https://www.ifarchive.org/if-archive/games/zcode/cluedo.z5" "$OUT_DIR/cluedo.z5"
fetch "https://www.ifarchive.org/if-archive/games/zcode/Figaro.zblorb" "$OUT_DIR/Figaro.zblorb"
fetch "https://www.ifarchive.org/if-archive/games/zcode/bookvol.z5" "$OUT_DIR/bookvol.z5"

echo "Downloaded story files to $OUT_DIR"
