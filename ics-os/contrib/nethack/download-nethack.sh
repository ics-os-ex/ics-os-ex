#!/usr/bin/env bash
set -euo pipefail

VERSION="3.6.7"
ARCHIVE="nethack-367-src.tgz"
URL="https://www.nethack.org/download/${VERSION}/${ARCHIVE}"

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="${ROOT_DIR}/nethack"

if [[ -d "${DEST_DIR}" ]]; then
  echo "NetHack source already exists at ${DEST_DIR}"
  exit 0
fi

mkdir -p "${DEST_DIR}"

tmpfile="${ROOT_DIR}/${ARCHIVE}"
if command -v curl >/dev/null 2>&1; then
  curl -L -o "${tmpfile}" "${URL}"
elif command -v wget >/dev/null 2>&1; then
  wget -O "${tmpfile}" "${URL}"
else
  echo "Error: curl or wget is required to download NetHack."
  exit 1
fi

tar -xzf "${tmpfile}" -C "${ROOT_DIR}"
rm -f "${tmpfile}"

mv "${ROOT_DIR}/NetHack-${VERSION}" "${DEST_DIR}"

echo "NetHack ${VERSION} downloaded to ${DEST_DIR}"
