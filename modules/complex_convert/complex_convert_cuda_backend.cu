#include "complex_convert_backend.h"

#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime.h>

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

namespace rdma_dada {
namespace modules {
namespace complex_convert {
namespace {

const unsigned int kTile = 32;
const unsigned int kBlockRows = 8;

pipeline::StageStatus CudaError(const char* operation, cudaError_t error) {
    std::ostringstream message;
    message << operation << " failed: " << cudaGetErrorString(error);
    return pipeline::StageStatus::Error(message.str());
}

template <typename Component>
__global__ void ConvertTransposeKernel(const Component* input,
                                       pipeline::Complex32* output,
                                       std::uint64_t nant,
                                       std::uint64_t q,
                                       float scale) {
    __shared__ pipeline::Complex32 tile[kTile][kTile + 1];

    const std::uint64_t input_q =
        static_cast<std::uint64_t>(blockIdx.x) * kTile + threadIdx.x;
    const std::uint64_t input_a =
        static_cast<std::uint64_t>(blockIdx.y) * kTile + threadIdx.y;
    for (unsigned int row = 0; row < kTile; row += kBlockRows) {
        const std::uint64_t a = input_a + row;
        if (input_q < q && a < nant) {
            const std::uint64_t source_index = a * q + input_q;
            pipeline::Complex32 value;
            value.real =
                static_cast<float>(input[2 * source_index]) * scale;
            value.imag =
                static_cast<float>(input[2 * source_index + 1]) * scale;
            tile[threadIdx.y + row][threadIdx.x] = value;
        }
    }
    __syncthreads();

    const std::uint64_t output_a =
        static_cast<std::uint64_t>(blockIdx.y) * kTile + threadIdx.x;
    const std::uint64_t output_q =
        static_cast<std::uint64_t>(blockIdx.x) * kTile + threadIdx.y;
    for (unsigned int row = 0; row < kTile; row += kBlockRows) {
        const std::uint64_t q_index = output_q + row;
        if (output_a < nant && q_index < q) {
            output[q_index * nant + output_a] =
                tile[threadIdx.x][threadIdx.y + row];
        }
    }
}

class KernelComplexConvertExecutor : public CudaComplexConvertExecutor {
public:
    KernelComplexConvertExecutor()
        : configured_(false), cuda_device_(-1), component_bits_(0),
          scale_(0.0f), max_grid_x_(0), max_grid_y_(0) {}

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
        if (properties.maxGridSize[0] <= 0 ||
            properties.maxGridSize[1] <= 0) {
            return pipeline::StageStatus::Error(
                "CUDA device reports invalid two-dimensional grid limits");
        }

        cuda_device_ = cuda_device;
        component_bits_ = component_bits;
        scale_ = scale;
        max_grid_x_ =
            static_cast<std::uint64_t>(properties.maxGridSize[0]);
        max_grid_y_ =
            static_cast<std::uint64_t>(properties.maxGridSize[1]);
        configured_ = true;
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t nant,
        std::uint64_t q,
        const pipeline::BlockExecutionContext& context) {
        if (!configured_) {
            return pipeline::StageStatus::Error(
                "CUDA complex conversion executor is not configured");
        }
        if (!output || !input.data || !output->data) {
            return pipeline::StageStatus::Error(
                "CUDA complex conversion requires non-null blocks");
        }
        if (!context.native_stream) {
            return pipeline::StageStatus::Error(
                "CUDA execution requires a worker-owned non-default stream");
        }
        if (context.device_id != cuda_device_) {
            return pipeline::StageStatus::Error(
                "CUDA context device does not match configured CUDA_DEVICE");
        }
        if (nant == 0 || q == 0) {
            return pipeline::StageStatus::Error(
                "CUDA transpose dimensions must be non-zero");
        }

        const std::uint64_t grid_x = 1 + (q - 1) / kTile;
        const std::uint64_t grid_y = 1 + (nant - 1) / kTile;
        if (grid_x > max_grid_x_ || grid_y > max_grid_y_) {
            return pipeline::StageStatus::Error(
                "ATFP transpose exceeds the CUDA two-dimensional grid limit");
        }
        const cudaError_t set_status = cudaSetDevice(cuda_device_);
        if (set_status != cudaSuccess) {
            return CudaError("cudaSetDevice", set_status);
        }

        const dim3 block(kTile, kBlockRows);
        const dim3 grid(static_cast<unsigned int>(grid_x),
                        static_cast<unsigned int>(grid_y));
        const cudaStream_t stream =
            reinterpret_cast<cudaStream_t>(context.native_stream);
        pipeline::Complex32* destination =
            reinterpret_cast<pipeline::Complex32*>(output->data);
        if (component_bits_ == 8) {
            ConvertTransposeKernel<<<grid, block, 0, stream>>>(
                reinterpret_cast<const std::int8_t*>(input.data),
                destination, nant, q, scale_);
        } else {
            ConvertTransposeKernel<<<grid, block, 0, stream>>>(
                reinterpret_cast<const std::int16_t*>(input.data),
                destination, nant, q, scale_);
        }
        const cudaError_t launch_status = cudaPeekAtLastError();
        if (launch_status != cudaSuccess) {
            return CudaError("ATFP transpose kernel launch", launch_status);
        }
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Finish() {
        configured_ = false;
        cuda_device_ = -1;
        component_bits_ = 0;
        scale_ = 0.0f;
        max_grid_x_ = 0;
        max_grid_y_ = 0;
        return pipeline::StageStatus::Ok();
    }

private:
    bool configured_;
    int cuda_device_;
    std::uint64_t component_bits_;
    float scale_;
    std::uint64_t max_grid_x_;
    std::uint64_t max_grid_y_;
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
