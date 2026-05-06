#!/bin/sh
set -e

ROOT_DIR=$(cd "$(dirname "$0")" && pwd)
INTERP_DIR="$ROOT_DIR/interpreter"
TARBALL_URL="https://www.ifarchive.org/if-archive/infocom/interpreters/frotz/frotz-2.55.tar.gz"

mkdir -p "$INTERP_DIR"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

printf "Downloading Frotz (GPL-2.0) from %s...\n" "$TARBALL_URL"

if command -v curl >/dev/null 2>&1; then
  curl -L "$TARBALL_URL" -o "$TMPDIR/frotz.tar.gz"
elif command -v wget >/dev/null 2>&1; then
  wget -O "$TMPDIR/frotz.tar.gz" "$TARBALL_URL"
else
  echo "Error: curl or wget is required."
  exit 1
fi

rm -rf "$INTERP_DIR"/*

tar -xzf "$TMPDIR/frotz.tar.gz" -C "$TMPDIR"

# The archive contains a top-level folder like frotz-2.55
EXTRACTED=$(find "$TMPDIR" -maxdepth 1 -type d -name "frotz-*" | head -n 1)
if [ -z "$EXTRACTED" ]; then
  echo "Error: could not find extracted Frotz directory."
  exit 1
fi

cp -R "$EXTRACTED"/* "$INTERP_DIR"/

echo "Frotz source extracted to $INTERP_DIR"
