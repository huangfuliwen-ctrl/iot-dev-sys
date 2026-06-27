#!/bin/bash
set -e

# Build script for dev-sys
# Usage: ./build.sh [debug|release] [host|arm]

BUILD_TYPE="${1:-debug}"
TARGET="${2:-host}"

# Convert to CMake build type
case "$BUILD_TYPE" in
    debug)   CMAKE_BUILD_TYPE="Debug" ;;
    release) CMAKE_BUILD_TYPE="Release" ;;
    *)       echo "Unknown build type: $BUILD_TYPE"; exit 1 ;;
esac

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="${PROJECT_DIR}/build/${BUILD_TYPE}-${TARGET}"

echo "=== Building dev-sys ($CMAKE_BUILD_TYPE / $TARGET) ==="

# Prepare build directory
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# CMake configure
CMAKE_ARGS=(
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE"
)

if [ "$TARGET" = "arm" ]; then
    CMAKE_ARGS+=(-DCMAKE_TOOLCHAIN_FILE="${PROJECT_DIR}/toolchains/arm-linux-gnueabihf.cmake")
    CMAKE_ARGS+=(-DUSE_SYSTEM_OPENSSL=OFF)
    CMAKE_ARGS+=(-DUSE_SYSTEM_CURL=OFF)
    CMAKE_ARGS+=(-DUSE_SYSTEM_SQLITE=OFF)
fi

cmake "${CMAKE_ARGS[@]}" "$PROJECT_DIR"

# Build
cmake --build . -j"$(nproc)"

echo "=== Build complete ==="
echo "Output: $BUILD_DIR/bin/dev-sys"
