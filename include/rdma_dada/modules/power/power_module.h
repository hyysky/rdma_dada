#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <memory>

namespace rdma_dada {
namespace modules {
namespace power {

// Converts beamformed CF32 voltage in TFPB order to F32 power in the same
// order. CPU_REFERENCE is a portable correctness backend; CUDA is the primary
// asynchronous observation backend.
class PowerModule : public pipeline::AlgorithmModule {
public:
    PowerModule();
    ~PowerModule();

    const char* Name() const;

    pipeline::StageStatus ConfigureHeader(
        const pipeline::Metadata& input_header,
        const pipeline::StageParameters& parameters,
        pipeline::Metadata* output_header);

    pipeline::StageStatus ProcessBlock(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        const pipeline::BlockExecutionContext& context);

    pipeline::StageStatus Finish();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace power
}  // namespace modules
}  // namespace rdma_dada
