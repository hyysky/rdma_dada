#pragma once

#include "rdma_dada/modules/beamform/beamform_module.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace rdma_dada {
namespace modules {
namespace beamform {

enum class BeamformComputeMode {
    kFp32,
    kTf32
};

struct BeamformGeometry {
    std::uint64_t nchan;
    std::uint64_t npol;
    std::uint64_t nant;
    std::uint64_t nbeam;
};

class CudaBeamformExecutor {
public:
    virtual ~CudaBeamformExecutor() {}

    virtual pipeline::StageStatus Configure(
        const BeamformGeometry& geometry,
        const std::vector<Complex32>& weights,
        int cuda_device,
        BeamformComputeMode compute_mode) = 0;

    virtual pipeline::StageStatus Process(
        const pipeline::InputBlock& input,
        pipeline::OutputBlock* output,
        std::uint64_t ntime,
        const pipeline::BlockExecutionContext& context) = 0;

    virtual pipeline::StageStatus Finish() = 0;
};

std::unique_ptr<CudaBeamformExecutor> CreateCudaBeamformExecutor();

}  // namespace beamform
}  // namespace modules
}  // namespace rdma_dada
