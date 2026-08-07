#include "rdma_dada/simulation/vdif_sender_rate.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

namespace sim = rdma_dada::simulation;
int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void TestExactDeadline() {
    const sim::SenderRatePlan plan = {UINT64_C(10000000000), UINT64_C(1000)};
    std::uint64_t deadline = 0;
    std::string error;
    Expect(sim::ComputePayloadDeadlineNs(plan, 125U, &deadline, &error),
           "125-byte deadline computes: " + error);
    Expect(deadline == 1100U,
           "125 bytes at 10 Gbps consume exactly 100 ns");
    Expect(sim::ComputePayloadDeadlineNs(plan, 1250U, &deadline, &error),
           "1250-byte deadline computes");
    Expect(deadline == 2000U,
           "1250 bytes at 10 Gbps consume exactly 1000 ns");
}

void TestEqualRateUsesNoWeights() {
    std::uint64_t station_rate = 0;
    std::uint64_t represented = 0;
    std::string error;
    Expect(sim::ComputeEqualStationPayloadRate(10U, 3U, &station_rate,
                                                &represented, &error),
           "non-divisible aggregate rate computes: " + error);
    Expect(station_rate == 3U && represented == 9U,
           "all Stations receive the same integer rate and remainder is reported");
    Expect(!sim::ComputeEqualStationPayloadRate(10U, 0U, &station_rate,
                                                 &represented, &error),
           "zero Station count is rejected");
}

void TestInvalidAndOverflow() {
    std::uint64_t deadline = 0;
    std::string error;
    const sim::SenderRatePlan zero_rate = {0U, 0U};
    Expect(!sim::ComputePayloadDeadlineNs(zero_rate, 1U, &deadline, &error),
           "zero target rate is rejected");
    const sim::SenderRatePlan overflowing = {
        UINT64_C(10000000000),
        std::numeric_limits<std::uint64_t>::max() - 10U
    };
    Expect(!sim::ComputePayloadDeadlineNs(overflowing, 125U, &deadline, &error),
           "absolute deadline overflow is rejected");
    Expect(!sim::ComputePayloadDeadlineNs(overflowing, 1U, NULL, &error),
           "null deadline output is rejected");
}

}  // namespace

int main() {
    TestExactDeadline();
    TestEqualRateUsesNoWeights();
    TestInvalidAndOverflow();
    if (failures) return 1;
    std::cout << "vdif_sender_rate_test passed\n";
    return 0;
}
