#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

echo "=== LoadGraph TTY — build (Linux, Release) ==="

if [ ! -d "build_linux" ]; then
    mkdir -p build_linux
fi
cd build_linux

cmake ..
make -j"$(nproc)" --target LoadGraphTTY

echo ""
echo "=== Done. Binary in build_linux/bin/LoadGraphTTY ==="
ls -la bin/LoadGraphTTY
