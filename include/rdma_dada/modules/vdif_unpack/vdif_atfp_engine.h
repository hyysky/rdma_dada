#pragma once

#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/modules/vdif_unpack/atfp_block_view.h"
#include "rdma_dada/modules/vdif_unpack/vdif_timeline.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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
    std::uint64_t large_gap_advanced_groups;
    std::uint64_t max_station_ordinal_skew;
    std::uint64_t single_station_raw_blocks;
    std::uint64_t mixed_station_raw_blocks;
    std::uint64_t max_station_records_per_raw_block;
    std::uint64_t max_consecutive_station_records;
    std::uint64_t payload_copy_calls;
    std::uint64_t payload_copy_bytes;
    std::uint64_t emitted_blocks;
    std::uint64_t emitted_bytes;
    std::vector<std::uint64_t> station_observed_packets;
    std::vector<std::uint64_t> station_accepted_packets;
    std::vector<std::uint64_t> station_late_packets;
    std::vector<std::uint64_t> station_highest_ordinals;
    std::vector<std::uint64_t> missing_station_packets_per_second;
};

// One cache-friendly parse result. The raw-record address is reconstructed
// from record_index so descriptors never own pointers or heap storage.
struct ParsedRecordDescriptor {
    std::uint64_t ordinal;
    std::uint32_t record_index;
    std::uint16_t antenna;
    std::uint8_t flags;
    std::uint8_t reserved;
};

typedef std::function<bool(const AtfpBlockView&, std::string*)>
    VdifAtfpBlockEmitter;

class VdifAtfpUnpackEngine {
public:
    VdifAtfpUnpackEngine();
    ~VdifAtfpUnpackEngine();

    bool Prepare(const VdifUnpackConfig& config,
                 const PipelineConfig& pipeline,
                 const VdifUnpackLayout& layout,
                 std::string* error);
    // Ordered CPUs: coordinator, one or more parse/copy workers, writer.
    bool ConfigureThreadCpus(const std::vector<int>& cpus,
                             std::string* error);
    bool BeginTransfer(const VdifTimeline& timeline, std::string* error);
    bool BeginTransfer(const VdifTimeline& timeline,
                       bool collect_missing_per_second,
                       std::string* error);
    bool BeginTransfer(const VdifTimeline& timeline,
                       bool collect_missing_per_second,
                       bool discard_before_timeline_start,
                       std::string* error);
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
    bool ConsumeRawBlockAsync(const std::uint8_t* data,
                              std::uint64_t size,
                              std::uint64_t raw_block_sequence,
                              const VdifAtfpBlockEmitter& emit,
                              std::string* error);
    bool Finish(const VdifAtfpBlockEmitter& emit, std::string* error);
    bool FinishAsync(const VdifAtfpBlockEmitter& emit, std::string* error);
    bool ReleasePublishedBlock(std::uint64_t lease_id, std::string* error);
    const VdifAtfpStatistics& statistics() const;
    bool prepared() const;
    std::uint64_t prepared_window_bytes() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
