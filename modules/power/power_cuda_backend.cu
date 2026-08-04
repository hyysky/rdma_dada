#include "power_backend.h"

#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime.h>

#include <climits>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace rdma_dada {
namespace modules {
namespace power {
namespace {

const unsigned int kThreadsPerBlock = 256;

pipeline::StageStatus CudaError(const char* operation, cudaError_t error) {
    std::ostringstream message;
    message << operation << " failed: " << cudaGetErrorString(error);
    return pipeline::StageStatus::Error(message.str());
}

__global__ void PowerKernel(const pipeline::Complex32* input, float* output,
                            std::uint64_t element_count) {
    const std::uint64_t index =
        static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= element_count) return;
    const pipeline::Complex32 value = input[index];
    output[index] = value.real * value.real + value.imag * value.imag;
}

class KernelPowerExecutor : public CudaPowerExecutor {
public:
    KernelPowerExecutor()
        : configured_(false), cuda_device_(-1), max_grid_x_(0) {}

    ~KernelPowerExecutor() { Finish(); }

    pipeline::StageStatus Configure(int cuda_device) {
        const pipeline::StageStatus finish_status = Finish();
        if (!finish_status.ok()) return finish_status;
        if (cuda_device < 0) {
            return pipeline::StageStatus::Error(
                "CUDA_DEVICE must be non-negative");
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
        max_grid_x_ = static_cast<unsigned int>(properties.maxGridSize[0]);
        configured_ = true;
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t element_count,
        const pipeline::BlockExecutionContext& context) {
        if (!configured_) {
            return pipeline::StageStatus::Error(
                "CUDA power executor is not configured");
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
        if (element_count == 0) {
            return pipeline::StageStatus::Error(
                "CUDA power element count must be non-zero");
        }

        const std::uint64_t block_count =
            1 + (element_count - 1) / kThreadsPerBlock;
        if (block_count > max_grid_x_ || block_count > UINT_MAX) {
            return pipeline::StageStatus::Error(
                "power block exceeds the CUDA one-dimensional grid limit");
        }

        const cudaError_t set_status = cudaSetDevice(cuda_device_);
        if (set_status != cudaSuccess) {
            return CudaError("cudaSetDevice", set_status);
        }
        const cudaStream_t stream =
            reinterpret_cast<cudaStream_t>(context.native_stream);
        PowerKernel<<<static_cast<unsigned int>(block_count),
                      kThreadsPerBlock, 0, stream>>>(
            reinterpret_cast<const pipeline::Complex32*>(input.data),
            reinterpret_cast<float*>(output->data), element_count);
        const cudaError_t launch_status = cudaPeekAtLastError();
        if (launch_status != cudaSuccess) {
            return CudaError("PowerKernel launch", launch_status);
        }
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Finish() {
        // The worker owns stream synchronization and the input/output buffer
        // lifetime. This executor owns no per-block or device allocation.
        configured_ = false;
        cuda_device_ = -1;
        max_grid_x_ = 0;
        return pipeline::StageStatus::Ok();
    }

private:
    bool configured_;
    int cuda_device_;
    unsigned int max_grid_x_;
};

}  // namespace

std::unique_ptr<CudaPowerExecutor> CreateCudaPowerExecutor() {
    return std::unique_ptr<CudaPowerExecutor>(new KernelPowerExecutor);
}

}  // namespace power
}  // namespace modules
}  // namespace rdma_dada
