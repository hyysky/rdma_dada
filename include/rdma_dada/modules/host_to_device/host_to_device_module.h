#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <memory>

namespace rdma_dada {
namespace modules {
namespace host_to_device {

// Enqueues a byte-preserving host-to-device transfer on a worker-owned CUDA
// stream. The module owns neither the host block nor the device allocation.
class HostToDeviceModule : public pipeline::AlgorithmModule {
public:
    HostToDeviceModule();
    ~HostToDeviceModule();

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

}  // namespace host_to_device
}  // namespace modules
}  // namespace rdma_dada
