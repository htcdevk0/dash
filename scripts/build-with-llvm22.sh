#!/usr/bin/env bash
set -euo pipefail

LLVM_CONFIG="${LLVM_CONFIG:-$(command -v llvm-config-22 || command -v llvm-config || true)}"
if [[ -z "${LLVM_CONFIG}" ]]; then
    echo "llvm-config was not found. Export LLVM_CONFIG=/path/to/llvm-config" >&2
    exit 1
fi

make LLVM_CONFIG="${LLVM_CONFIG}" "$@"
sudo cp build/dash /bin