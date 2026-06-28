#!/bin/bash
# rdma_dada module build script

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "Building rdma_dada module..."
echo "Script directory: $SCRIPT_DIR"

# Create build directory
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# Run CMake
echo "Running CMake..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
echo "Building..."
make -j $(nproc)

echo ""
echo "========================================"
echo "Build complete!"
echo "========================================"
echo ""
echo "Demo executable: ${BUILD_DIR}/Demo_psrdada_online"
echo ""
echo "Usage:"
echo "  cd $SCRIPT_DIR"
echo "  ./build/Demo_psrdada_online --help"
echo ""
