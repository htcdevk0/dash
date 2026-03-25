#!/usr/bin/env bash
set -euo pipefail

URL="https://github.com/htcdevk0/dash/releases/download/LLVM-Pre-build/dash-linux-pre-compiled-llvm-x86_64.tar.gz"
DEST="LLVM-22.1.1"
TMP="$(mktemp -d)"

cleanup() {
    rm -rf "$TMP"
}
trap cleanup EXIT

if [ -d "$DEST" ]; then
    echo "[info] $DEST already exists"
    exit 0
fi

echo "[info] downloading LLVM toolchain"
curl -L "$URL" -o "$TMP/llvm.tar.gz"

echo "[info] extracting"
mkdir -p "$DEST"
tar -xzf "$TMP/llvm.tar.gz" -C "$DEST" --strip-components=1

if [ ! -x "$DEST/bin/llvm-config" ]; then
    echo "[error] invalid LLVM package"
    exit 1
fi

echo "[ok] LLVM installed at $DEST"