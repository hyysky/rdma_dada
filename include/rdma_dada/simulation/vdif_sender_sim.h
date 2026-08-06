#pragma once

#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rdma_dada {
namespace simulation {

struct VdifSenderSimConfig {
    std::string destination_ip;
    std::uint16_t destination_port;
    std::uint32_t path_mtu;
    std::uint16_t station_id;
    modules::vdif_unpack::ProjectVdifGeometry geometry;
    std::uint8_t reference_epoch;
    std::uint32_t start_seconds;
    std::uint64_t sample_interval_ps;
    std::uint64_t group_count;
    std::string mode;
    std::string start_utc;
    std::vector<std::uint64_t> drop_groups;
    std::vector<std::uint64_t> duplicate_groups;
    std::vector<std::uint64_t> invalid_header_groups;
};

bool LoadVdifSenderSimConfig(const std::string& path,
                             VdifSenderSimConfig* config,
                             std::string* error);

bool BuildVdifSenderRecord(const VdifSenderSimConfig& config,
                           std::uint64_t group_index,
                           std::vector<std::uint8_t>* record,
                           std::string* error);

}  // namespace simulation
}  // namespace rdma_dada
