#pragma once

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace simulation {

struct SenderRatePlan {
    std::uint64_t target_payload_bits_per_second;
    std::uint64_t start_monotonic_ns;
};

bool ComputePayloadDeadlineNs(const SenderRatePlan& plan,
                              std::uint64_t cumulative_payload_bytes,
                              std::uint64_t* deadline_ns,
                              std::string* error);

bool ComputeEqualStationPayloadRate(std::uint64_t aggregate_bits_per_second,
                                    std::uint32_t station_count,
                                    std::uint64_t* station_bits_per_second,
                                    std::uint64_t* represented_aggregate,
                                    std::string* error);

}  // namespace simulation
}  // namespace rdma_dada
