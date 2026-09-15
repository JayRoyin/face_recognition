#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# download.sh — fetch SQLite amalgamation source into this directory.
#
# Usage:
#     ./download.sh                 # use default version
#     SQLITE_VERSION=3460100 ./download.sh
#
# After download, sqlite3.c / sqlite3.h live next to this script.
# -----------------------------------------------------------------------------
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SQLITE_VERSION="${SQLITE_VERSION:-3460100}"   # 3.46.1 — stable, has GEOPOLY + R-Tree
SQLITE_YEAR="${SQLITE_YEAR:-2024}"
URL="https://www.sqlite.org/${SQLITE_YEAR}/sqlite-amalgamation-${SQLITE_VERSION}.zip"

echo "[sqlite3] Target version : ${SQLITE_VERSION}"
echo "[sqlite3] Download URL    : ${URL}"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

# Use proxy if available (mirrors the build.sh convention in this repo)
export all_proxy="${all_proxy:-${http_proxy:-${HTTP_PROXY:-}}}"
export https_proxy="${https_proxy:-${HTTPS_PROXY:-$all_proxy}}"

ARCHIVE="$TMP_DIR/sqlite-amalgamation.zip"
if command -v curl >/dev/null 2>&1; then
    curl -fsSL -o "$ARCHIVE" "$URL"
else
    wget -q -O "$ARCHIVE" "$URL"
fi

if [ ! -s "$ARCHIVE" ]; then
    echo "[sqlite3] ERROR: download failed or empty archive" >&2
    exit 1
fi

EXTRACT="$TMP_DIR/extract"
mkdir -p "$EXTRACT"
unzip -q "$ARCHIVE" -d "$EXTRACT"

# The archive expands to a directory named sqlite-amalgamation-<ver>/
SRC_DIR="$(find "$EXTRACT" -maxdepth 1 -type d -name 'sqlite-amalgamation-*' | head -n1)"
if [ -z "$SRC_DIR" ]; then
    echo "[sqlite3] ERROR: could not locate extracted source directory" >&2
    exit 1
fi

cp -f "$SRC_DIR/sqlite3.c"  "$SCRIPT_DIR/sqlite3.c"
cp -f "$SRC_DIR/sqlite3.h"  "$SCRIPT_DIR/sqlite3.h"

# shellc is the CLI; we don't ship it but keep the option to rebuild CLI locally
if [ -f "$SRC_DIR/shell.c" ]; then
    cp -f "$SRC_DIR/shell.c" "$SCRIPT_DIR/shell.c"
fi

echo "[sqlite3] OK — sqlite3.c / sqlite3.h ready in $SCRIPT_DIR"
ls -lh "$SCRIPT_DIR/sqlite3."* "$SCRIPT_DIR/shell.c" 2>/dev/null || true