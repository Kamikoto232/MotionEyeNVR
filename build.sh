#!/bin/bash
# Builds inside a Debian 13 (trixie) environment so the binary matches the target system.
# Usage (inside the trixie chroot/container, with qt6-base-dev qt6-multimedia-dev cmake g++ installed):
#   ./build.sh            -> build/motioneye-nvr
set -euo pipefail
cd "$(dirname "$0")"
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "$@"
cmake --build build -j"$(nproc)"
strip -o build/motioneye-nvr.stripped build/motioneye-nvr
echo "built: $(pwd)/build/motioneye-nvr"
