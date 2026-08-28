#pragma once

#include "rdma_dada/pipeline/metadata.h"
#include "rdma_dada/pipeline/stage.h"
#include "rdma_dada/pipeline/worker_config.h"
#include "rdma_dada/pipeline/worker_metrics.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rdma_dada {
namespace pipeline {

typedef std::function<StageStatus(
    std::uint64_t sequence,
    std::uint8_t** data,
    std::uint64_t* capacity)> AcquireOutputBlockFunction;
typedef std::function<StageStatus(
    std::uint64_t sequence,
    std::uint64_t bytes)> CommitOutputBlockFunction;
typedef std::function<StageStatus(
    std::uint64_t sequence)> AbortOutputBlockFunction;

struct OutputBlockFunctions {
    AcquireOutputBlockFunction acquire;
    CommitOutputBlockFunction commit;
    AbortOutputBlockFunction abort;
};

// Owns CUDA execution resources but not PSRDADA rings. The caller supplies a
// single-writer output-block seam so direct mode can D2H into the ring while
// staged mode can publish pinned output without changing ring ownership.
class GpuBlockPipeline {
public:
    GpuBlockPipeline();
    ~GpuBlockPipeline();

    StageStatus Configure(const WorkerConfig& config,
                          const WorkerBlockGeometry& geometry,
                          const Metadata& input_header,
                          const OutputBlockFunctions& output,
                          Metadata* output_header);
    StageStatus SubmitBlock(std::uint64_t sequence,
                            const std::uint8_t* ring_data,
                            std::uint64_t input_bytes);
    StageStatus PlanBlock(std::uint64_t input_bytes,
                          std::uint64_t* scratch_bytes,
                          std::uint64_t* output_bytes) const;
    void RecordInputRingRegistration(std::uint64_t registered_ring_blocks,
                                     std::uint64_t registered_ring_bytes,
                                     std::uint64_t registration_ns);
    StageStatus Drain();
    StageStatus Abort(const std::string& reason);
    StageStatus Finish();

    const WorkerMetrics& metrics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pipeline
}  // namespace rdma_dada
