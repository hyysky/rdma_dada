#include "complex_convert_backend.h"

#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime.h>

#include <climits>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace rdma_dada {
namespace modules {
namespace complex_convert {
namespace {

const unsigned int kThreadsPerBlock = 256;

pipeline::StageStatus CudaError(const char* operation, cudaError_t error) {
    std::ostringstream message;
    message << operation << " failed: " << cudaGetErrorString(error);
    return pipeline::StageStatus::Error(message.str());
}

__global__ void ConvertCi8Kernel(const std::int8_t* input,
                                 pipeline::Complex32* output,
                                 std::uint64_t sample_count,
                                 float scale) {
    const std::uint64_t index =
        static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= sample_count) return;
    output[index].real = static_cast<float>(input[2 * index]) * scale;
    output[index].imag = static_cast<float>(input[2 * index + 1]) * scale;
}

__global__ void ConvertCi16Kernel(const std::int16_t* input,
                                  pipeline::Complex32* output,
                                  std::uint64_t sample_count,
                                  float scale) {
    const std::uint64_t index =
        static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (index >= sample_count) return;
    output[index].real = static_cast<float>(input[2 * index]) * scale;
    output[index].imag = static_cast<float>(input[2 * index + 1]) * scale;
}

class KernelComplexConvertExecutor : public CudaComplexConvertExecutor {
public:
    KernelComplexConvertExecutor()
        : configured_(false), cuda_device_(-1), component_bits_(0),
          scale_(0.0f), max_grid_x_(0) {}

    ~KernelComplexConvertExecutor() { Finish(); }

    pipeline::StageStatus Configure(
        int cuda_device, std::uint64_t component_bits, float scale) {
        const pipeline::StageStatus finish_status = Finish();
        if (!finish_status.ok()) return finish_status;
        if (cuda_device < 0) {
            return pipeline::StageStatus::Error(
                "CUDA_DEVICE must be non-negative");
        }
        if (component_bits != 8 && component_bits != 16) {
            return pipeline::StageStatus::Error(
                "CUDA complex conversion supports CI8 or CI16");
        }
        if (!(scale > 0.0f)) {
            return pipeline::StageStatus::Error(
                "CONVERSION_SCALE must be positive");
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
        component_bits_ = component_bits;
        scale_ = scale;
        max_grid_x_ = static_cast<unsigned int>(properties.maxGridSize[0]);
        configured_ = true;
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t sample_count,
        const pipeline::BlockExecutionContext& context) {
        if (!configured_) {
            return pipeline::StageStatus::Error(
                "CUDA complex conversion executor is not configured");
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
        if (sample_count == 0) {
            return pipeline::StageStatus::Error(
                "CUDA complex conversion sample count must be non-zero");
        }

        const std::uint64_t block_count =
            1 + (sample_count - 1) / kThreadsPerBlock;
        if (block_count > max_grid_x_ || block_count > UINT_MAX) {
            return pipeline::StageStatus::Error(
                "complex conversion block exceeds the CUDA grid limit");
        }
        const cudaError_t set_status = cudaSetDevice(cuda_device_);
        if (set_status != cudaSuccess) {
            return CudaError("cudaSetDevice", set_status);
        }

        const cudaStream_t stream =
            reinterpret_cast<cudaStream_t>(context.native_stream);
        pipeline::Complex32* destination =
            reinterpret_cast<pipeline::Complex32*>(output->data);
        if (component_bits_ == 8) {
            ConvertCi8Kernel<<<static_cast<unsigned int>(block_count),
                               kThreadsPerBlock, 0, stream>>>(
                reinterpret_cast<const std::int8_t*>(input.data),
                destination, sample_count, scale_);
        } else {
            ConvertCi16Kernel<<<static_cast<unsigned int>(block_count),
                                kThreadsPerBlock, 0, stream>>>(
                reinterpret_cast<const std::int16_t*>(input.data),
                destination, sample_count, scale_);
        }
        const cudaError_t launch_status = cudaPeekAtLastError();
        if (launch_status != cudaSuccess) {
            return CudaError("complex conversion kernel launch", launch_status);
        }
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Finish() {
        configured_ = false;
        cuda_device_ = -1;
        component_bits_ = 0;
        scale_ = 0.0f;
        max_grid_x_ = 0;
        return pipeline::StageStatus::Ok();
    }

private:
    bool configured_;
    int cuda_device_;
    std::uint64_t component_bits_;
    float scale_;
    unsigned int max_grid_x_;
};

}  // namespace

std::unique_ptr<CudaComplexConvertExecutor>
CreateCudaComplexConvertExecutor() {
    return std::unique_ptr<CudaComplexConvertExecutor>(
        new KernelComplexConvertExecutor);
}

}  // namespace complex_convert
}  // namespace modules
}  // namespace rdma_dada
