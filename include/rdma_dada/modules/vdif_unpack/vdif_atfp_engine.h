#pragma once

#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/modules/vdif_unpack/atfp_block_view.h"
#include "rdma_dada/modules/vdif_unpack/vdif_timeline.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {

struct VdifAtfpStatistics {
    std::uint64_t received_records;
    std::uint64_t accepted_packets;
    std::uint64_t invalid_header_packets;
    std::uint64_t invalid_data_packets;
    std::uint64_t unknown_station_packets;
    std::uint64_t duplicate_packets;
    std::uint64_t late_packets;
    std::uint64_t out_of_range_packets;
    std::uint64_t completed_groups;
    std::uint64_t incomplete_groups;
    std::uint64_t fully_missing_groups;
    std::uint64_t missing_station_packets;
    std::uint64_t expected_station_packets;
    std::uint64_t large_gap_advances;
    std::uint64_t payload_copy_calls;
    std::uint64_t payload_copy_bytes;
    std::uint64_t emitted_blocks;
    std::uint64_t emitted_bytes;
};

typedef std::function<bool(const AtfpBlockView&, std::string*)>
    VdifAtfpBlockEmitter;

class VdifAtfpUnpackEngine {
public:
    VdifAtfpUnpackEngine();
    ~VdifAtfpUnpackEngine();

    bool Configure(const VdifUnpackConfig& config,
                   const PipelineConfig& pipeline,
                   const VdifUnpackLayout& layout,
                   const VdifTimeline& timeline,
                   std::string* error);
    bool ConsumeRawBlock(const std::uint8_t* data,
                         std::uint64_t size,
                         std::uint64_t raw_block_sequence,
                         const VdifAtfpBlockEmitter& emit,
                         std::string* error);
    bool Finish(const VdifAtfpBlockEmitter& emit, std::string* error);
    const VdifAtfpStatistics& statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
