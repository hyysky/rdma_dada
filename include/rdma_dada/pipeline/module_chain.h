#pragma once

#include "rdma_dada/pipeline/block.h"
#include "rdma_dada/pipeline/execution_context.h"
#include "rdma_dada/pipeline/metadata.h"
#include "rdma_dada/pipeline/stage.h"
#include "rdma_dada/pipeline/worker_config.h"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rdma_dada {
namespace pipeline {

struct ModuleChainPlan {
    Metadata input_header;
    Metadata output_header;
    std::uint64_t input_frame_bytes;
    std::uint64_t beamformed_frame_bytes;
    std::uint64_t product_frame_bytes;
    std::uint64_t output_frame_bytes;
    std::uint64_t integration_length;
    bool integration_enabled;
    std::size_t module_count;
    MemoryLocation execution_location;
};

// Portable process-level algorithm chain. Ring ownership and host/device
// transfers stay in pipeline_worker; this class validates metadata, configures
// the selected modules and executes them on caller-owned buffers.
class ModuleChain {
public:
    ModuleChain();
    ~ModuleChain();

    StageStatus Configure(const Metadata& ring_input_header,
                          const WorkerConfig& config,
                          Metadata* ring_output_header);

    StageStatus PlanBlock(std::uint64_t input_bytes,
                          std::uint64_t* scratch_bytes,
                          std::uint64_t* output_bytes) const;

    // scratch contains all intermediate buffers. With integration enabled it
    // contains adjacent full-size beamformed and unintegrated product blocks.
    StageStatus ProcessBlock(const InputBlock& input, OutputBlock* output,
                             std::uint8_t* scratch,
                             std::uint64_t scratch_capacity,
                             const BlockExecutionContext& context);

    StageStatus Finish();
    const ModuleChainPlan& plan() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pipeline
}  // namespace rdma_dada
