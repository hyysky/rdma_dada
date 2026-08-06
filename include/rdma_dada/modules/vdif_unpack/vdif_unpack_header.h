#pragma once

#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"
#include "rdma_dada/pipeline/metadata.h"

#include <string>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {

bool BuildVdifUnpackOutputHeader(const pipeline::Metadata& input,
                                 const VdifUnpackConfig& config,
                                 const PipelineConfig& pipeline_config,
                                 const PipelineLayout& pipeline_layout,
                                 const VdifUnpackLayout& unpack_layout,
                                 pipeline::Metadata* output,
                                 std::string* error);

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
