#!/usr/bin/env sh
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT_DIR="$SCRIPT_DIR/busybox"
TMP_DIR="$SCRIPT_DIR/.tmp-busybox"

# BusyBox release tarball (GPL-2.0). Update version as needed.
BUSYBOX_VER=1.36.1
BUSYBOX_TARBALL="busybox-$BUSYBOX_VER.tar.bz2"
BUSYBOX_URL="https://busybox.net/downloads/$BUSYBOX_TARBALL"

mkdir -p "$TMP_DIR"

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

fetch "$BUSYBOX_URL" "$TMP_DIR/$BUSYBOX_TARBALL"

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"

( cd "$TMP_DIR" && tar -xjf "$BUSYBOX_TARBALL" )

mv "$TMP_DIR/busybox-$BUSYBOX_VER"/* "$OUT_DIR/"

rm -rf "$TMP_DIR"

echo "BusyBox source extracted to $OUT_DIR"
