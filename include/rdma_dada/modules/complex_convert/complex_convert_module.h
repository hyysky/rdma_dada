#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <memory>

namespace rdma_dada {
namespace modules {
namespace complex_convert {

// Converts block-scoped [A,T,F,P] signed integer complex samples into a
// physically contiguous [T,F,P,A] CF32 tensor. The module owns no ring or
// block buffer.
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
