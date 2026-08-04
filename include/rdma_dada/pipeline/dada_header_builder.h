#pragma once

#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/pipeline/dada_header.h"

#include <string>

namespace rdma_dada {

enum class DataStage {
    kRaw,
    kCompute
};

// Converts the validated pipeline configuration into the versioned metadata
// written to a PSRDADA header block.
bool BuildPipelineDadaHeader(const PipelineConfig& config,
                             const PipelineLayout& layout,
                             DataStage stage,
                             dada_header_t* header,
                             std::string* error);

}  // namespace rdma_dada
