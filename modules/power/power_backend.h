#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <cstdint>
#include <memory>

namespace rdma_dada {
namespace modules {
namespace power {

class CudaPowerExecutor {
public:
    virtual ~CudaPowerExecutor() {}

    virtual pipeline::StageStatus Configure(int cuda_device) = 0;

    virtual pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t element_count,
        const pipeline::BlockExecutionContext& context) = 0;

    virtual pipeline::StageStatus Finish() = 0;
};

std::unique_ptr<CudaPowerExecutor> CreateCudaPowerExecutor();

}  // namespace power
}  // namespace modules
}  // namespace rdma_dada
