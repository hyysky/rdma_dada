#pragma once

#include "rdma_dada/simulation/vdif_sender_sim.h"

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace simulation {

struct VdifSenderStats {
    std::uint64_t scheduled_packets;
    std::uint64_t sent_packets;
    std::uint64_t retried_packets;
    std::uint64_t failed_packets;
    std::uint64_t payload_bytes;
    std::uint64_t elapsed_ns;
    std::uint64_t batches;
    std::uint64_t short_batches;
    std::uint64_t overrun_batches;
    std::string backend;
};

bool RunUdpVdifSender(const VdifSenderSimConfig& config,
                      VdifSenderStats* stats,
                      std::string* error);

std::string FormatVdifSenderStatsJson(const VdifSenderSimConfig& config,
                                      const VdifSenderStats& stats);

}  // namespace simulation
}  // namespace rdma_dada
