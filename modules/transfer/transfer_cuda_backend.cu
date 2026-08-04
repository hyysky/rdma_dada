#include "transfer_backend.h"

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <sstream>

namespace rdma_dada {
namespace modules {
namespace transfer {
namespace {

pipeline::StageStatus CudaError(const char* operation, cudaError_t error) {
    std::ostringstream message;
    message << operation << " failed: " << cudaGetErrorString(error);
    return pipeline::StageStatus::Error(message.str());
}

class RuntimeCudaTransferExecutor : public CudaTransferExecutor {
public:
    RuntimeCudaTransferExecutor() : configured_(false), cuda_device_(-1) {}

    ~RuntimeCudaTransferExecutor() { Finish(); }

    pipeline::StageStatus Configure(int cuda_device) {
        const pipeline::StageStatus finish_status = Finish();
        if (!finish_status.ok()) return finish_status;
        if (cuda_device < 0) {
            return pipeline::StageStatus::Error(
                "CUDA_DEVICE must be non-negative");
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

        configured_ = true;
        cuda_device_ = cuda_device;
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Copy(
        const std::uint8_t* input, std::uint8_t* output,
        std::uint64_t bytes, TransferDirection direction,
        const pipeline::BlockExecutionContext& context) {
        if (!configured_) {
            return pipeline::StageStatus::Error(
                "CUDA transfer executor is not configured");
        }
        if (!input || !output || bytes == 0) {
            return pipeline::StageStatus::Error(
                "CUDA transfer requires non-empty input and output buffers");
        }
        if (context.backend != pipeline::ExecutionBackend::kCuda) {
            return pipeline::StageStatus::Error(
                "CUDA transfer requires a CUDA execution context");
        }
        if (context.device_id != cuda_device_) {
            return pipeline::StageStatus::Error(
                "CUDA context device does not match configured CUDA_DEVICE");
        }
        if (!context.native_stream) {
            return pipeline::StageStatus::Error(
                "CUDA transfer requires a worker-owned non-default stream");
        }
        if (bytes > static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max())) {
            return pipeline::StageStatus::Error(
                "CUDA transfer byte count exceeds addressable size_t");
        }

        cudaError_t status = cudaSetDevice(cuda_device_);
        if (status != cudaSuccess) return CudaError("cudaSetDevice", status);
        const cudaMemcpyKind kind =
            direction == TransferDirection::kHostToDevice ?
                cudaMemcpyHostToDevice : cudaMemcpyDeviceToHost;
        status = cudaMemcpyAsync(
            output, input, static_cast<std::size_t>(bytes), kind,
            reinterpret_cast<cudaStream_t>(context.native_stream));
        if (status != cudaSuccess) {
            return CudaError(
                direction == TransferDirection::kHostToDevice ?
                    "cudaMemcpyAsync host-to-device" :
                    "cudaMemcpyAsync device-to-host",
                status);
        }
        return pipeline::StageStatus::Ok();
    }

    pipeline::StageStatus Finish() {
        // Stream synchronization and buffer lifetime belong to the worker.
        configured_ = false;
        cuda_device_ = -1;
        return pipeline::StageStatus::Ok();
    }

private:
    bool configured_;
    int cuda_device_;
};

}  // namespace

std::unique_ptr<CudaTransferExecutor> CreateCudaTransferExecutor() {
    return std::unique_ptr<CudaTransferExecutor>(
        new RuntimeCudaTransferExecutor);
}

}  // namespace transfer
}  // namespace modules
}  // namespace rdma_dada
