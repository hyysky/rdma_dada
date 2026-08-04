#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <memory>

namespace rdma_dada {
namespace modules {
namespace beamform {

// Portable logical representation of CF32. The CUDA backend will validate
// layout compatibility before using cuComplex storage.
struct Complex32 {
    float real;
    float imag;
};

// Beamforming module with a host FP32 reference backend and an optional
// asynchronous CUDA FP32/TF32 backend selected at configuration time.
class BeamformModule : public pipeline::AlgorithmModule {
public:
    BeamformModule();
    ~BeamformModule();

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

}  // namespace beamform
}  // namespace modules
}  // namespace rdma_dada
