#include "rdma_dada/simulation/vdif_sender_rate.h"

#include <limits>

namespace rdma_dada {
namespace simulation {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

}  // namespace

bool ComputePayloadDeadlineNs(const SenderRatePlan& plan,
                              std::uint64_t cumulative_payload_bytes,
                              std::uint64_t* deadline_ns,
                              std::string* error) {
    if (!deadline_ns) return Fail("deadline output pointer is null", error);
    if (plan.target_payload_bits_per_second == 0)
        return Fail("target payload bit rate must be positive", error);

    const __uint128_t numerator =
        static_cast<__uint128_t>(cumulative_payload_bytes) * 8U *
        UINT64_C(1000000000);
    const __uint128_t delta =
        numerator / plan.target_payload_bits_per_second;
    if (delta > std::numeric_limits<std::uint64_t>::max())
        return Fail("payload deadline delta exceeds uint64 range", error);
    const std::uint64_t delta_ns = static_cast<std::uint64_t>(delta);
    if (plan.start_monotonic_ns >
        std::numeric_limits<std::uint64_t>::max() - delta_ns)
        return Fail("absolute payload deadline exceeds uint64 range", error);
    *deadline_ns = plan.start_monotonic_ns + delta_ns;
    return true;
}

bool ComputeEqualStationPayloadRate(std::uint64_t aggregate_bits_per_second,
                                    std::uint32_t station_count,
                                    std::uint64_t* station_bits_per_second,
                                    std::uint64_t* represented_aggregate,
                                    std::string* error) {
    if (!station_bits_per_second || !represented_aggregate)
        return Fail("equal-rate output pointer is null", error);
    if (aggregate_bits_per_second == 0)
        return Fail("aggregate payload bit rate must be positive", error);
    if (station_count == 0)
        return Fail("Station count must be positive", error);
    const std::uint64_t per_station =
        aggregate_bits_per_second / station_count;
    if (per_station == 0)
        return Fail("aggregate rate is below one bit/s per Station", error);
    *station_bits_per_second = per_station;
    *represented_aggregate = per_station * station_count;
    return true;
}

}  // namespace simulation
}  // namespace rdma_dada
