#pragma once

#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/pipeline/metadata.h"

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {

struct VdifTimeline {
    std::uint64_t group_period_ps;
    std::uint8_t start_reference_epoch;
    std::uint32_t start_seconds;
    std::uint32_t start_frame;
    std::uint64_t expected_groups;
};

bool ParseVdifTimeline(const pipeline::Metadata& header,
                       const PipelineConfig& pipeline,
                       VdifTimeline* timeline,
                       std::string* error);

bool VdifOrdinalToTime(const VdifTimeline& timeline,
                       std::uint64_t ordinal,
                       std::uint32_t* seconds,
                       std::uint32_t* frame,
                       std::string* error);

bool VdifTimeToOrdinal(const VdifTimeline& timeline,
                       std::uint8_t reference_epoch,
                       std::uint32_t seconds,
                       std::uint32_t frame,
                       std::uint64_t* ordinal,
                       std::string* error);

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
