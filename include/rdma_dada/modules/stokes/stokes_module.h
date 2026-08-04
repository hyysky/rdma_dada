#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <memory>

namespace rdma_dada {
namespace modules {
namespace stokes {

// Converts dual-polarization beamformed CF32 voltage in TFPB order to the
// four F32 coherency products [AA, BB, AB_REAL, AB_IMAG] in TFBS order.
class StokesModule : public pipeline::AlgorithmModule {
public:
    StokesModule();
    ~StokesModule();

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

}  // namespace stokes
}  // namespace modules
}  // namespace rdma_dada
