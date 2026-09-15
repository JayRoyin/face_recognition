#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# download.sh — fetch libmicrohttpd source tarball.
#
# Usage:
#     ./download.sh
#     LIBMICROHTTPD_VERSION=1.0.1 ./download.sh
# -----------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

LIBMICROHTTPD_VERSION="${LIBMICROHTTPD_VERSION:-1.0.1}"
echo "[libmicrohttpd] Target version : ${LIBMICROHTTPD_VERSION}"

export all_proxy="${all_proxy:-${http_proxy:-${HTTP_PROXY:-}}}"
export https_proxy="${https_proxy:-${HTTPS_PROXY:-$all_proxy}}"

URL="https://ftp.gnu.org/gnu/libmicrohttpd/libmicrohttpd-${LIBMICROHTTPD_VERSION}.tar.gz"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

ARCHIVE="$TMP_DIR/libmicrohttpd.tar.gz"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$ARCHIVE" "$URL"
else
    wget -q -O "$ARCHIVE" "$URL"
fi

if [ ! -s "$ARCHIVE" ]; then
    echo "[libmicrohttpd] ERROR: download failed or empty archive" >&2
    exit 1
fi

tar -xzf "$ARCHIVE" -C "$TMP_DIR"

SRC_DIR="$(find "$TMP_DIR" -maxdepth 1 -type d -name 'libmicrohttpd-*' | head -n1)"
if [ -z "$SRC_DIR" ]; then
    echo "[libmicrohttpd] ERROR: extracted directory not found" >&2
    exit 1
fi

mkdir -p "$SCRIPT_DIR/src"
rm -rf "$SCRIPT_DIR/src/libmicrohttpd"
mv "$SRC_DIR" "$SCRIPT_DIR/src/libmicrohttpd"

echo "[libmicrohttpd] OK — source extracted to $SCRIPT_DIR/src/libmicrohttpd"