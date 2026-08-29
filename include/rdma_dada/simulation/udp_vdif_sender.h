#pragma once

#include "rdma_dada/simulation/vdif_sender_sim.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rdma_dada {
namespace simulation {

struct VdifSenderStats {
    std::uint64_t scheduled_packets;
    std::uint64_t sent_packets;
    std::uint64_t retried_packets;
    std::uint64_t failed_packets;
    std::uint64_t payload_bytes;
    std::uint64_t elapsed_ns;
    std::uint64_t pacing_start_monotonic_ns;
    std::uint64_t first_send_monotonic_ns;
    std::uint64_t last_send_monotonic_ns;
    std::uint64_t batches;
    std::uint64_t short_batches;
    std::uint64_t overrun_batches;
    std::vector<std::uint16_t> station_ids;
    std::vector<std::uint64_t> station_scheduled_packets;
    std::vector<std::uint64_t> station_sent_packets;
    std::string backend;
    std::string payload_prefix_hex;
};

bool RunUdpVdifSender(const VdifSenderSimConfig& config,
                      VdifSenderStats* stats,
                      std::string* error);

std::string FormatVdifSenderStatsJson(const VdifSenderSimConfig& config,
                                      const VdifSenderStats& stats);

}  // namespace simulation
}  // namespace rdma_dada
