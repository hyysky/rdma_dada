#!/usr/bin/env bash
# Incremental GPU algorithm/test build for the qths1 development server.
# This script intentionally preserves the build directory between runs.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BUILD_DIR="${RDMA_DADA_GPU_BUILD_DIR:-${PROJECT_DIR}/build-gpu-codex}"
CMAKE_BIN="${CMAKE_BIN:-/home/user/wy/tools/cmake-3.31.12/bin/cmake}"
CTEST_BIN="${CTEST_BIN:-$(dirname "${CMAKE_BIN}")/ctest}"
CUDA_ROOT="${CUDA_ROOT:-/usr/local/cuda}"
CUDA_ARCH="${CUDA_ARCH:-86}"
BUILD_JOBS="${BUILD_JOBS:-12}"

if [[ -f "${PROJECT_DIR}/env.sh" ]]; then
    # env.sh appends to this variable; non-interactive SSH shells may not set it.
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}"
    # shellcheck disable=SC1091
    source "${PROJECT_DIR}/env.sh"
fi

if [[ ! -x "${CMAKE_BIN}" ]]; then
    echo "Error: CMake executable not found: ${CMAKE_BIN}" >&2
    echo "Set CMAKE_BIN to the project-local CMake executable." >&2
    exit 1
fi

if [[ ! -x "${CUDA_ROOT}/bin/nvcc" ]]; then
    echo "Error: CUDA compiler not found: ${CUDA_ROOT}/bin/nvcc" >&2
    echo "Set CUDA_ROOT to the CUDA Toolkit installation prefix." >&2
    exit 1
fi

echo "Configuring incremental GPU test build"
echo "  Project:       ${PROJECT_DIR}"
echo "  Build:         ${BUILD_DIR}"
echo "  CMake:         ${CMAKE_BIN}"
echo "  CUDA:          ${CUDA_ROOT}"
echo "  CUDA arch:     ${CUDA_ARCH}"
echo "  Parallel jobs: ${BUILD_JOBS}"

"${CMAKE_BIN}" \
    -S "${PROJECT_DIR}" \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CUDA_COMPILER="${CUDA_ROOT}/bin/nvcc" \
    -DCMAKE_CUDA_ARCHITECTURES="${CUDA_ARCH}" \
    -DBUILD_RDMA_PIPELINE=OFF \
    -DBUILD_SSD_BENCHMARK=OFF \
    -DBUILD_ALGORITHM_BENCHMARKS=ON \
    -DBUILD_TESTING=ON \
    -DUSE_CUDA=ON

echo "Building GPU modules and tests (existing outputs are preserved)..."
"${CMAKE_BIN}" --build "${BUILD_DIR}" --parallel "${BUILD_JOBS}"

echo "GPU test build complete: ${BUILD_DIR}"
echo "Run CUDA tests with:"
echo "  ${CTEST_BIN} --test-dir ${BUILD_DIR} -L cuda --output-on-failure"
echo "Run the CUDA ATFP transpose benchmark with:"
echo "  ${BUILD_DIR}/complex_convert_transpose_cuda_benchmark CI8 256 65536 200 1.0 0"
echo "Run the CUDA time-integration benchmark with:"
echo "  ${BUILD_DIR}/time_integrate_cuda_benchmark 65536 180 128 200 MEAN 0"
