#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# download.sh — fetch SpatiaLite source (the project doesn't ship binaries).
#
# Usage:
#     ./download.sh                 # default version
#     SPATIALITE_VERSION=5.1.0 ./download.sh
# -----------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SPATIALITE_VERSION="${SPATIALITE_VERSION:-5.1.0}"
echo "[spatialite] Target version : ${SPATIALITE_VERSION}"

# Use proxy if available
export all_proxy="${all_proxy:-${http_proxy:-${HTTP_PROXY:-}}}"
export https_proxy="${https_proxy:-${HTTPS_PROXY:-$all_proxy}}"

URL="https://www.gaia-gis.it/gaia-sins/libspatialite-${SPATIALITE_VERSION}.tar.gz"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

ARCHIVE="$TMP_DIR/libspatialite.tar.gz"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$ARCHIVE" "$URL"
else
    wget -q -O "$ARCHIVE" "$URL"
fi

if [ ! -s "$ARCHIVE" ]; then
    echo "[spatialite] ERROR: download failed or empty archive" >&2
    exit 1
fi

tar -xzf "$ARCHIVE" -C "$TMP_DIR"

# SpatiaLite archives unpack into a directory like libspatialite-5.1.0/
SRC_DIR="$(find "$TMP_DIR" -maxdepth 1 -type d -name 'libspatialite-*' | head -n1)"
if [ -z "$SRC_DIR" ]; then
    echo "[spatialite] ERROR: extracted directory not found" >&2
    exit 1
fi

mkdir -p "$SCRIPT_DIR/src"
rm -rf "$SCRIPT_DIR/src/libspatialite"
mv "$SRC_DIR" "$SCRIPT_DIR/src/libspatialite"

echo "[spatialite] OK — source extracted to $SCRIPT_DIR/src/libspatialite"