#pragma once

namespace rdma_dada {
namespace pipeline {

enum class ExecutionBackend {
    kHost,
    kCuda
};

// Non-owning execution resources supplied by pipeline_worker for one block.
// native_stream is null for host execution and is a cudaStream_t, represented
// opaquely to keep pipeline core independent of CUDA headers, for CUDA.
struct BlockExecutionContext {
    ExecutionBackend backend;
    int device_id;
    void* native_stream;
};

}  // namespace pipeline
}  // namespace rdma_dada
