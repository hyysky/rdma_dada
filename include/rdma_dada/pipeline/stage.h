#pragma once

#include "rdma_dada/pipeline/block.h"
#include "rdma_dada/pipeline/execution_context.h"
#include "rdma_dada/pipeline/metadata.h"

#include <string>

namespace rdma_dada {
namespace pipeline {

class StageStatus {
public:
    static StageStatus Ok() { return StageStatus(true, std::string()); }
    static StageStatus Error(const std::string& message) {
        return StageStatus(false, message);
    }

    bool ok() const { return ok_; }
    const std::string& message() const { return message_; }

private:
    StageStatus(bool ok, const std::string& message)
        : ok_(ok), message_(message) {}

    bool ok_;
    std::string message_;
};

// One AlgorithmModule transforms metadata and data but does not own a ring.
// Multiple modules may be composed inside one worker process. StageRunner owns
// only the two rings at the ends of that configured module chain.
class AlgorithmModule {
public:
    virtual ~AlgorithmModule() {}

    virtual const char* Name() const = 0;

    virtual StageStatus ConfigureHeader(const Metadata& input_header,
                                        const StageParameters& parameters,
                                        Metadata* output_header) = 0;

    // Host modules complete before returning. CUDA modules may only enqueue
    // work on context.native_stream; pipeline_worker records the completion
    // event and keeps all input/output leases alive until that event finishes.
    virtual StageStatus ProcessBlock(const InputBlock& input,
                                     OutputBlock* output,
                                     const BlockExecutionContext& context) = 0;

    virtual StageStatus Finish() { return StageStatus::Ok(); }
};

// Transitional alias while the process-level ModuleChain is introduced.
using Stage = AlgorithmModule;

}  // namespace pipeline
}  // namespace rdma_dada
