#pragma once

#include "rdma_dada/config/observation_config.h"
#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/pipeline_config.h"

#include <cstdint>
#include <string>

namespace rdma_dada {

struct ResolvedObservationPlan {
    ObservationConfig source;
    PacketFormatConfig wire;
    std::uint32_t nant;
    std::uint64_t complex_sample_bytes;
    std::uint64_t payload_bytes;
    std::uint64_t raw_record_bytes;
    std::uint64_t group_period_ps;
    std::uint64_t expected_groups;
    std::uint64_t records_per_block;
    std::uint64_t samples_per_block;
    std::uint64_t raw_block_bytes;
    std::uint64_t compute_block_bytes;
    std::uint64_t window_groups;
    std::uint64_t window_payload_bytes;
    std::uint64_t window_validity_bytes;
    std::uint64_t raw_ring_bytes;
    std::uint64_t compute_ring_bytes;
    std::uint64_t raw_file_bytes;
    std::uint64_t compute_file_bytes;
    std::uint64_t payload_bytes_per_second;
    std::uint64_t raw_bytes_per_second;
    std::uint8_t group_start_reference_epoch;
    std::uint32_t group_start_seconds;
    std::uint32_t group_start_frame;
    std::string config_id;
    std::string geometry_id;
};

bool ResolveObservationPlan(const ObservationConfig& config,
                            const PacketFormatConfig& wire,
                            ResolvedObservationPlan* plan,
                            std::string* error);

bool BuildPipelineRuntimeFromResolvedPlan(
    const ResolvedObservationPlan& plan,
    PipelineConfig* config,
    PipelineLayout* layout,
    std::string* error);

}  // namespace rdma_dada
