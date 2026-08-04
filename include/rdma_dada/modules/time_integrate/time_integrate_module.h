#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <memory>

namespace rdma_dada {
namespace modules {
namespace time_integrate {

// Reduces the time axis of an F32 product tensor by a configured integer
// integration length. The module owns no ring, block allocation or stream.
class TimeIntegrateModule : public pipeline::AlgorithmModule {
public:
    TimeIntegrateModule();
    ~TimeIntegrateModule();

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

}  // namespace time_integrate
}  // namespace modules
}  // namespace rdma_dada
