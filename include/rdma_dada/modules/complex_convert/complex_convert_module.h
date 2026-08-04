#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <memory>

namespace rdma_dada {
namespace modules {
namespace complex_convert {

// Converts signed interleaved integer complex samples to CF32 without
// changing the TFPA tensor shape. The module owns no ring or block buffer.
class ComplexConvertModule : public pipeline::AlgorithmModule {
public:
    ComplexConvertModule();
    ~ComplexConvertModule();

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

}  // namespace complex_convert
}  // namespace modules
}  // namespace rdma_dada
