#pragma once

#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {

struct VdifGroupKey {
    std::uint8_t reference_epoch;
    std::uint32_t seconds_from_reference_epoch;
    std::uint32_t frame_number_within_second;
    std::uint16_t first_channel_id;
    std::uint8_t nchan;
    std::uint8_t npol;

    bool operator<(const VdifGroupKey& other) const;
};

struct VdifUnpackStatistics {
    std::uint64_t received_records;
    std::uint64_t accepted_packets;
    std::uint64_t invalid_header_packets;
    std::uint64_t invalid_data_packets;
    std::uint64_t unknown_station_packets;
    std::uint64_t duplicate_packets;
    std::uint64_t late_packets;
    std::uint64_t window_evictions;
    std::uint64_t completed_groups;
    std::uint64_t incomplete_groups;
    std::uint64_t missing_station_packets;
    std::uint64_t expected_station_packets_for_observed_groups;
};

typedef std::function<bool(const VdifGroupKey&, const std::uint8_t*,
                           std::uint64_t, std::string*)> VdifGroupEmitter;

class VdifUnpackEngine {
public:
    VdifUnpackEngine();
    ~VdifUnpackEngine();

    bool Configure(const VdifUnpackConfig& config,
                   const PipelineConfig& pipeline,
                   const VdifUnpackLayout& layout,
                   std::string* error);
    bool ConsumeRawBlock(const std::uint8_t* data, std::uint64_t size,
                         std::uint64_t raw_block_sequence,
                         const VdifGroupEmitter& emit,
                         std::string* error);
    bool Finish(const VdifGroupEmitter& emit, std::string* error);
    const VdifUnpackStatistics& statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
