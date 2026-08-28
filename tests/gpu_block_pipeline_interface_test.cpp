#include "rdma_dada/pipeline/gpu_block_pipeline.h"

#include <iostream>

int main() {
    rdma_dada::pipeline::GpuBlockPipeline pipeline;
    rdma_dada::pipeline::WorkerConfig config = {};
    config.execution_backend = "CUDA";
    config.cuda_pipeline_mode =
        rdma_dada::CudaPipelineMode::kSynchronousDirect;
    config.cuda_inflight_blocks = 1U;
    rdma_dada::pipeline::WorkerBlockGeometry geometry = {};
    rdma_dada::pipeline::Metadata input_header;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::OutputBlockFunctions output;
    const rdma_dada::pipeline::StageStatus status = pipeline.Configure(
        config, geometry, input_header, output, &output_header);
    if (status.ok()) {
        std::cerr << "FAIL: incomplete configuration must be rejected\n";
        return 1;
    }
    if (!pipeline.Finish().ok()) {
        std::cerr << "FAIL: finish after rejected configure must be safe\n";
        return 1;
    }
    std::cout << "gpu_block_pipeline_interface_test passed\n";
    return 0;
}
