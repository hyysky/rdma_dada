#pragma once

#include <cstdint>
#include <string>

namespace rdma_dada {

// Pipeline configuration is portable and has no PSRDADA/RDMA dependency.
// Version 1 describes one raw record as a 64-byte application header followed
// by one payload. The compute record contains the payload only.
struct PipelineConfig {
    std::uint32_t nant;
    std::uint32_t nchan;
    std::uint32_t npol;
    std::string payload_order;

    std::uint64_t packet_header_bytes;
    std::uint64_t packet_payload_bytes;
    std::uint64_t packet_samples;
    std::uint32_t packet_nbit;
    double sample_interval_us;

    std::uint64_t records_per_block;
    std::uint64_t raw_ring_blocks;
    std::uint64_t compute_ring_blocks;
    std::uint64_t file_blocks;
    bool disk_enabled;
    bool direct_io;

    std::string utc_start;
};

struct PipelineLayout {
    // records_per_block is the total packet count across all antennas.
    // One complete time group contains exactly NANT packets.
    std::uint64_t packets_per_antenna_per_block;
    std::uint64_t samples_per_block;
    std::uint64_t raw_record_bytes;
    std::uint64_t compute_record_bytes;
    std::uint64_t raw_resolution;
    std::uint64_t compute_resolution;

    std::uint64_t raw_block_bytes;
    std::uint64_t compute_block_bytes;
    std::uint64_t raw_ring_bytes;
    std::uint64_t compute_ring_bytes;
    std::uint64_t raw_file_bytes;
    std::uint64_t compute_file_bytes;

    // BYTES_PER_SECOND uses this payload-only rate. raw_bytes_per_second is
    // retained separately for network and raw-ring capacity monitoring.
    std::uint64_t payload_bytes_per_second;
    std::uint64_t raw_bytes_per_second;
};

bool LoadPipelineConfig(const std::string& path,
                        PipelineConfig* config,
                        std::string* error);

bool ComputePipelineLayout(const PipelineConfig& config,
                           PipelineLayout* layout,
                           std::string* error);

}  // namespace rdma_dada
