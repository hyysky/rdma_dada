#pragma once

#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/pipeline_config.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {

struct VdifUnpackConfig {
    std::uint32_t input_key;
    std::uint32_t output_key;
    std::string pipeline_config_path;
    std::string packet_format_path;
    std::uint16_t first_channel_id;
    std::vector<std::uint16_t> antenna_map;
    std::uint32_t window_blocks;
    std::uint64_t max_window_bytes;
    std::string output_memory;
    bool run_once;
};

struct VdifUnpackLayout {
    std::uint64_t raw_record_bytes;
    std::uint64_t records_per_raw_block;
    std::uint64_t group_bytes;
    std::uint64_t window_capacity_groups;
    std::uint64_t window_bytes;
    std::uint64_t compute_block_bytes;
};

bool LoadVdifUnpackConfig(const std::string& path,
                          VdifUnpackConfig* config,
                          std::string* error);

bool ComputeVdifUnpackLayout(const VdifUnpackConfig& config,
                             const PipelineConfig& pipeline,
                             const PacketFormatConfig& packet,
                             VdifUnpackLayout* layout,
                             std::string* error);

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
