#!/bin/bash
# Run the com_subjective_user_input client
# Usage: ./run.sh [arguments]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Detect OS and set binary path
case "$(uname -s)" in
    Linux*)   BINARY="$SCRIPT_DIR/build/linux/com_subjective_user_input" ;;
    Darwin*)  BINARY="$SCRIPT_DIR/build/macos/com_subjective_user_input" ;;
    MINGW*|MSYS*|CYGWIN*) BINARY="$SCRIPT_DIR/build/windows/com_subjective_user_input.exe" ;;
    *)        echo "Error: Unsupported OS: $(uname -s)"; exit 1 ;;
esac

if [ ! -f "$BINARY" ]; then
    echo "Error: Binary not found at $BINARY"
    echo "Please build the project first:"
    echo "  ./scripts/build_linux.sh"
    exit 1
fi

exec "$BINARY" "$@"
