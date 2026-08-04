#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <cstdint>
#include <memory>

namespace rdma_dada {
namespace modules {
namespace stokes {

class CudaStokesExecutor {
public:
    virtual ~CudaStokesExecutor() {}

    virtual pipeline::StageStatus Configure(
        int cuda_device, std::uint64_t nbeam) = 0;

    virtual pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t product_group_count,
        const pipeline::BlockExecutionContext& context) = 0;

    virtual pipeline::StageStatus Finish() = 0;
};

std::unique_ptr<CudaStokesExecutor> CreateCudaStokesExecutor();

}  // namespace stokes
}  // namespace modules
}  // namespace rdma_dada
