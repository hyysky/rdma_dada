#include "time_integrate_backend.h"

#include <cuda_runtime.h>

#include <climits>
#include <cstdint>
#include <memory>
#include <sstream>

namespace rdma_dada {
namespace modules {
namespace time_integrate {
namespace {

const unsigned int kThreadsPerBlock = 256;

pipeline::StageStatus CudaError(const char* operation, cudaError_t error) {
    std::ostringstream message;
    message << operation << " failed: " << cudaGetErrorString(error);
    return pipeline::StageStatus::Error(message.str());
}

__global__ void TimeIntegrateKernel(
    const float* input, float* output,
    std::uint64_t output_element_count,
    std::uint64_t frame_elements,
    std::uint64_t integration_length,
    float output_scale) {
    const std::uint64_t output_index =
        static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
    if (output_index >= output_element_count) return;

    const std::uint64_t output_time = output_index / frame_elements;
    const std::uint64_t element = output_index % frame_elements;
    const std::uint64_t first_input_time =
        output_time * integration_length;
    float sum = 0.0f;
    for (std::uint64_t integration_index = 0;
         integration_index < integration_length; ++integration_index) {
        const std::uint64_t input_index =
            (first_input_time + integration_index) * frame_elements + element;
        sum += input[input_index];
    }
    output[output_index] = sum * output_scale;
}

class KernelTimeIntegrateExecutor : public CudaTimeIntegrateExecutor {
public:
    KernelTimeIntegrateExecutor()
        : configured_(false), cuda_device_(-1), frame_elements_(0),
          integration_length_(0), output_scale_(1.0f), max_grid_x_(0) {}

    ~KernelTimeIntegrateExecutor() { Finish(); }

    pipeline::StageStatus Configure(
        int cuda_device, std::uint64_t frame_elements,
        std::uint64_t integration_length, bool calculate_mean) {
        const pipeline::StageStatus finish_status = Finish();
        if (!finish_status.ok()) return finish_status;
        if (cuda_device < 0 || frame_elements == 0 ||
            integration_length == 0) {
            return pipeline::StageStatus::Error(
                "invalid CUDA time-integration configuration");
        }

        int device_count = 0;
        cudaError_t status = cudaGetDeviceCount(&device_count);
        if (status != cudaSuccess) {
            return CudaError("cudaGetDeviceCount", status);
        }
        if (cuda_device >= device_count) {
            return pipeline::StageStatus::Error(
                "CUDA_DEVICE is not available on this server");
        }
        status = cudaSetDevice(cuda_device);
        if (status != cudaSuccess) return CudaError("cudaSetDevice", status);

        cudaDeviceProp properties;
        status = cudaGetDeviceProperties(&properties, cuda_device);
        if (status != cudaSuccess) {
            return CudaError("cudaGetDeviceProperties", status);
        }
        if (properties.maxGridSize[0] <= 0) {
            return pipeline::StageStatus::Error(
                "CUDA device reports an invalid maximum grid size");
        }

        configured_ = true;
        cuda_device_ = cuda_device;
        frame_elements_ = frame_elements;
        integration_length_ = integration_length;
        output_scale_ = calculate_mean ?
            1.0f / static_cast<float>(integration_length) : 1.0f;
        max_grid_x_ = static_cast<unsigned int>(properties.maxGridSize[0]);
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t output_element_count,
        const pipeline::BlockExecutionContext& context) {
        if (!configured_) {
            return pipeline::StageStatus::Error(
                "CUDA time-integration executor is not configured");
        }
        if (!output || output_element_count == 0) {
            return pipeline::StageStatus::Error(
                "CUDA time integration requires non-empty output");
        }
        if (context.device_id != cuda_device_ || !context.native_stream) {
            return pipeline::StageStatus::Error(
                "CUDA integration requires the configured device and a "
                "worker-owned non-default stream");
        }

        const std::uint64_t block_count =
            1 + (output_element_count - 1) / kThreadsPerBlock;
        if (block_count > max_grid_x_ || block_count > UINT_MAX) {
            return pipeline::StageStatus::Error(
                "integration output exceeds CUDA one-dimensional grid limit");
        }
        cudaError_t status = cudaSetDevice(cuda_device_);
        if (status != cudaSuccess) return CudaError("cudaSetDevice", status);

        TimeIntegrateKernel<<<static_cast<unsigned int>(block_count),
                              kThreadsPerBlock, 0,
                              reinterpret_cast<cudaStream_t>(
                                  context.native_stream)>>>(
            reinterpret_cast<const float*>(input.data),
            reinterpret_cast<float*>(output->data), output_element_count,
            frame_elements_, integration_length_, output_scale_);
        status = cudaPeekAtLastError();
        if (status != cudaSuccess) {
            return CudaError("TimeIntegrateKernel launch", status);
        }
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Finish() {
        configured_ = false;
        cuda_device_ = -1;
        frame_elements_ = 0;
        integration_length_ = 0;
        output_scale_ = 1.0f;
        max_grid_x_ = 0;
        return pipeline::StageStatus::Ok();
    }

private:
    bool configured_;
    int cuda_device_;
    std::uint64_t frame_elements_;
    std::uint64_t integration_length_;
    float output_scale_;
    unsigned int max_grid_x_;
};

}  // namespace

std::unique_ptr<CudaTimeIntegrateExecutor>
CreateCudaTimeIntegrateExecutor() {
    return std::unique_ptr<CudaTimeIntegrateExecutor>(
        new KernelTimeIntegrateExecutor);
}

}  // namespace time_integrate
}  // namespace modules
}  // namespace rdma_dada
