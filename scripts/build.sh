#!/usr/bin/env bash
# rdma_dada module build script

set -euo pipefail

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"

if [[ "$(uname -s)" == "Darwin" ]]; then
    DEFAULT_BUILD_RDMA_PIPELINE="OFF"
else
    DEFAULT_BUILD_RDMA_PIPELINE="ON"
fi

BUILD_RDMA_PIPELINE="${BUILD_RDMA_PIPELINE:-${DEFAULT_BUILD_RDMA_PIPELINE}}"
USE_CUDA="${USE_CUDA:-OFF}"

echo "Building rdma_dada module..."
echo "Project directory: ${PROJECT_DIR}"
echo "Build directory: ${BUILD_DIR}"
echo "BUILD_RDMA_PIPELINE=${BUILD_RDMA_PIPELINE}"
echo "USE_CUDA=${USE_CUDA}"

# Run CMake
echo "Running CMake..."
cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_RDMA_PIPELINE="${BUILD_RDMA_PIPELINE}" \
    -DUSE_CUDA="${USE_CUDA}"

# Build
echo "Building..."
cmake --build "${BUILD_DIR}" --parallel

echo ""
echo "========================================"
echo "Build complete!"
echo "========================================"
echo ""
if [[ "${BUILD_RDMA_PIPELINE}" == "ON" ]]; then
    echo "RDMA ingest executable: ${BUILD_DIR}/rdma2dada"
else
    echo "RDMA demo skipped (BUILD_RDMA_PIPELINE=OFF)"
fi
echo ""
echo "Usage:"
echo "Linux RDMA build:"
echo "  BUILD_RDMA_PIPELINE=ON USE_CUDA=OFF ${SCRIPT_DIR}/build.sh"
echo "Linux CUDA algorithm-only build:"
echo "  BUILD_RDMA_PIPELINE=OFF USE_CUDA=ON ${SCRIPT_DIR}/build.sh"
echo "Linux CUDA/RDMA build:"
echo "  BUILD_RDMA_PIPELINE=ON USE_CUDA=ON ${SCRIPT_DIR}/build.sh"
echo ""
