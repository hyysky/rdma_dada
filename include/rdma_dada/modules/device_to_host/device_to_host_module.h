#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <memory>

namespace rdma_dada {
namespace modules {
namespace device_to_host {

// Enqueues a byte-preserving device-to-host transfer on a worker-owned CUDA
// stream. The module owns neither the device allocation nor the host block.
class DeviceToHostModule : public pipeline::AlgorithmModule {
public:
    DeviceToHostModule();
    ~DeviceToHostModule();

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

}  // namespace device_to_host
}  // namespace modules
}  // namespace rdma_dada
