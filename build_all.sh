#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"

echo "=== LoadGraph — full build (Linux, Release) ==="

if [ ! -d "build_linux" ]; then
    mkdir -p build_linux
fi
cd build_linux

cmake ..
make -j"$(nproc)"

echo ""
echo "=== Done. Binaries in build_linux/bin/ ==="
ls -la bin/
