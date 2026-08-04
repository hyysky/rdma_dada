#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <cstdint>
#include <memory>

namespace rdma_dada {
namespace modules {
namespace transfer {

enum class TransferDirection {
    kHostToDevice,
    kDeviceToHost
};

class CudaTransferExecutor {
public:
    virtual ~CudaTransferExecutor() {}

    virtual pipeline::StageStatus Configure(int cuda_device) = 0;

    virtual pipeline::StageStatus Copy(
        const std::uint8_t* input, std::uint8_t* output,
        std::uint64_t bytes, TransferDirection direction,
        const pipeline::BlockExecutionContext& context) = 0;

    virtual pipeline::StageStatus Finish() = 0;
};

std::unique_ptr<CudaTransferExecutor> CreateCudaTransferExecutor();

}  // namespace transfer
}  // namespace modules
}  // namespace rdma_dada
