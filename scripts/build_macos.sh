#!/usr/bin/env bash
set -euo pipefail

# macOS build script for subjective_user_input_c

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root_dir"

echo "=== Building subjective_user_input_c (macOS) ==="

# Check for Homebrew
if ! command -v brew &> /dev/null; then
    echo "ERROR: Homebrew is required. Install from https://brew.sh"
    exit 1
fi

# Check/install dependencies
echo "Checking dependencies..."

if ! brew list openssl@3 &> /dev/null && ! brew list openssl@1.1 &> /dev/null; then
    echo "Installing OpenSSL..."
    brew install openssl@3
fi

if ! brew list libwebsockets &> /dev/null; then
    echo "Installing libwebsockets..."
    brew install libwebsockets
fi

if ! command -v cmake &> /dev/null; then
    echo "Installing CMake..."
    brew install cmake
fi

# Set up OpenSSL paths for CMake
if brew list openssl@3 &> /dev/null; then
    OPENSSL_ROOT=$(brew --prefix openssl@3)
elif brew list openssl@1.1 &> /dev/null; then
    OPENSSL_ROOT=$(brew --prefix openssl@1.1)
else
    OPENSSL_ROOT=$(brew --prefix openssl)
fi

export OPENSSL_ROOT_DIR="$OPENSSL_ROOT"
export PKG_CONFIG_PATH="$OPENSSL_ROOT/lib/pkgconfig:$(brew --prefix libwebsockets)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "Using OpenSSL from: $OPENSSL_ROOT"

# Configure and build
cmake -S . -B build/macos \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DOPENSSL_ROOT_DIR="$OPENSSL_ROOT"

cmake --build build/macos

echo ""
echo "Build complete!"
echo "Output: $root_dir/build/macos/input_unified"
