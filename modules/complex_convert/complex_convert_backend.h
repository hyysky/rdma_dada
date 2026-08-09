#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <cstdint>
#include <memory>

namespace rdma_dada {
namespace modules {
namespace complex_convert {

class CudaComplexConvertExecutor {
public:
    virtual ~CudaComplexConvertExecutor() {}

    virtual pipeline::StageStatus Configure(
        int cuda_device, std::uint64_t component_bits, float scale) = 0;

    virtual pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t nant,
        std::uint64_t q,
        const pipeline::BlockExecutionContext& context) = 0;

    virtual pipeline::StageStatus Finish() = 0;
};

std::unique_ptr<CudaComplexConvertExecutor>
CreateCudaComplexConvertExecutor();

}  // namespace complex_convert
}  // namespace modules
}  // namespace rdma_dada
