#!/bin/bash
set -e

# Code formatting script
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "=== Formatting code ==="

# C/C++ source files
find "$PROJECT_DIR/src" "$PROJECT_DIR/include" "$PROJECT_DIR/test" \
    -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.c" \) \
    -exec clang-format -i {} \;

echo "=== Formatting complete ==="
