#include "stokes_backend.h"

#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime.h>

#include <climits>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace rdma_dada {
namespace modules {
namespace stokes {
namespace {

const unsigned int kThreadsPerBlock = 256;
const std::uint64_t kPolarizationCount = 2;
const std::uint64_t kProductCount = 4;

pipeline::StageStatus CudaError(const char* operation, cudaError_t error) {
    std::ostringstream message;
    message << operation << " failed: " << cudaGetErrorString(error);
    return pipeline::StageStatus::Error(message.str());
}

__global__ void StokesKernel(const pipeline::Complex32* input, float* output,
                             std::uint64_t product_group_count,
                             std::uint64_t nbeam) {
    const std::uint64_t group =
        static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (group >= product_group_count) return;

    const std::uint64_t tf = group / nbeam;
    const std::uint64_t beam = group % nbeam;
    const std::uint64_t x_index = tf * kPolarizationCount * nbeam + beam;
    const std::uint64_t y_index = x_index + nbeam;
    const pipeline::Complex32 x = input[x_index];
    const pipeline::Complex32 y = input[y_index];
    const std::uint64_t output_index = group * kProductCount;
    output[output_index] = x.real * x.real + x.imag * x.imag;
    output[output_index + 1] = y.real * y.real + y.imag * y.imag;
    output[output_index + 2] = x.real * y.real + x.imag * y.imag;
    output[output_index + 3] = x.imag * y.real - x.real * y.imag;
}

class KernelStokesExecutor : public CudaStokesExecutor {
public:
    KernelStokesExecutor()
        : configured_(false), cuda_device_(-1), nbeam_(0), max_grid_x_(0) {}

    ~KernelStokesExecutor() { Finish(); }

    pipeline::StageStatus Configure(
        int cuda_device, std::uint64_t nbeam) {
        const pipeline::StageStatus finish_status = Finish();
        if (!finish_status.ok()) return finish_status;
        if (cuda_device < 0) {
            return pipeline::StageStatus::Error(
                "CUDA_DEVICE must be non-negative");
        }
        if (nbeam == 0) {
            return pipeline::StageStatus::Error("NBEAM must be non-zero");
        }

        int device_count = 0;
        cudaError_t cuda_status = cudaGetDeviceCount(&device_count);
        if (cuda_status != cudaSuccess) {
            return CudaError("cudaGetDeviceCount", cuda_status);
        }
        if (cuda_device >= device_count) {
            return pipeline::StageStatus::Error(
                "CUDA_DEVICE is not available on this server");
        }
        cuda_status = cudaSetDevice(cuda_device);
        if (cuda_status != cudaSuccess) {
            return CudaError("cudaSetDevice", cuda_status);
        }

        cudaDeviceProp properties;
        cuda_status = cudaGetDeviceProperties(&properties, cuda_device);
        if (cuda_status != cudaSuccess) {
            return CudaError("cudaGetDeviceProperties", cuda_status);
        }
        if (properties.maxGridSize[0] <= 0) {
            return pipeline::StageStatus::Error(
                "CUDA device reports an invalid maximum grid size");
        }

        cuda_device_ = cuda_device;
        nbeam_ = nbeam;
        max_grid_x_ = static_cast<unsigned int>(properties.maxGridSize[0]);
        configured_ = true;
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t product_group_count,
        const pipeline::BlockExecutionContext& context) {
        if (!configured_) {
            return pipeline::StageStatus::Error(
                "CUDA stokes executor is not configured");
        }
        if (!output) {
            return pipeline::StageStatus::Error("null output block");
        }
        if (!context.native_stream) {
            return pipeline::StageStatus::Error(
                "CUDA execution requires a worker-owned non-default stream");
        }
        if (context.device_id != cuda_device_) {
            return pipeline::StageStatus::Error(
                "CUDA context device does not match configured CUDA_DEVICE");
        }
        if (product_group_count == 0) {
            return pipeline::StageStatus::Error(
                "CUDA stokes product group count must be non-zero");
        }

        const std::uint64_t block_count =
            1 + (product_group_count - 1) / kThreadsPerBlock;
        if (block_count > max_grid_x_ || block_count > UINT_MAX) {
            return pipeline::StageStatus::Error(
                "stokes block exceeds the CUDA one-dimensional grid limit");
        }

        const cudaError_t set_status = cudaSetDevice(cuda_device_);
        if (set_status != cudaSuccess) {
            return CudaError("cudaSetDevice", set_status);
        }
        const cudaStream_t stream =
            reinterpret_cast<cudaStream_t>(context.native_stream);
        StokesKernel<<<static_cast<unsigned int>(block_count),
                       kThreadsPerBlock, 0, stream>>>(
            reinterpret_cast<const pipeline::Complex32*>(input.data),
            reinterpret_cast<float*>(output->data), product_group_count,
            nbeam_);
        const cudaError_t launch_status = cudaPeekAtLastError();
        if (launch_status != cudaSuccess) {
            return CudaError("StokesKernel launch", launch_status);
        }
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Finish() {
        // The worker owns stream synchronization and all block buffers. This
        // executor has no persistent device allocation.
        configured_ = false;
        cuda_device_ = -1;
        nbeam_ = 0;
        max_grid_x_ = 0;
        return pipeline::StageStatus::Ok();
    }

private:
    bool configured_;
    int cuda_device_;
    std::uint64_t nbeam_;
    unsigned int max_grid_x_;
};

}  // namespace

std::unique_ptr<CudaStokesExecutor> CreateCudaStokesExecutor() {
    return std::unique_ptr<CudaStokesExecutor>(new KernelStokesExecutor);
}

}  // namespace stokes
}  // namespace modules
}  // namespace rdma_dada
