#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux

