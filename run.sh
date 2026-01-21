#!/bin/bash
# Run the input_unified client
# Usage: ./run.sh [arguments]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BINARY="$SCRIPT_DIR/build/linux/input_unified"

if [ ! -f "$BINARY" ]; then
    echo "Error: Binary not found at $BINARY"
    echo "Please build the project first:"
    echo "  ./scripts/build_linux.sh"
    exit 1
fi

exec "$BINARY" "$@"
