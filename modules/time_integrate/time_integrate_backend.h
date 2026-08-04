#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <cstdint>
#include <memory>

namespace rdma_dada {
namespace modules {
namespace time_integrate {

class CudaTimeIntegrateExecutor {
public:
    virtual ~CudaTimeIntegrateExecutor() {}

    virtual pipeline::StageStatus Configure(
        int cuda_device, std::uint64_t frame_elements,
        std::uint64_t integration_length, bool calculate_mean) = 0;

    virtual pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t output_element_count,
        const pipeline::BlockExecutionContext& context) = 0;

    virtual pipeline::StageStatus Finish() = 0;
};

std::unique_ptr<CudaTimeIntegrateExecutor>
CreateCudaTimeIntegrateExecutor();

}  // namespace time_integrate
}  // namespace modules
}  // namespace rdma_dada
