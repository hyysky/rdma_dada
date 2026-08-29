#include "rdma_dada/testing/gpu_pressure_plan.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
    using rdma_dada::testing::GpuPressurePlan;
    using rdma_dada::testing::MakeGpuPressurePlan;
    using rdma_dada::testing::ScheduledOffsetNs;

    GpuPressurePlan plan = {};
    std::string error;
    Expect(MakeGpuPressurePlan(52428800U, 30000000000ULL, 60U,
                               &plan, &error),
           "build a 30 Gbps pressure plan");
    Expect(plan.blocks_per_second == 72U,
           "round target rate upward to whole blocks per second");
    Expect(plan.block_count == 4320U, "derive the exact finite block count");
    Expect(plan.bytes_per_second == 3774873600ULL,
           "report the block-aligned byte rate");
    Expect(plan.total_bytes == 226492416000ULL,
           "report the exact finite byte count");
    Expect(ScheduledOffsetNs(0U, 72U) == 0U,
           "first block is scheduled at the epoch");
    Expect(ScheduledOffsetNs(72U, 72U) == 1000000000ULL,
           "rational scheduling has no one-second drift");

    Expect(!MakeGpuPressurePlan(0U, 1U, 1U, &plan, &error),
           "reject a zero block size");
    Expect(!MakeGpuPressurePlan(1U, 0U, 1U, &plan, &error),
           "reject a zero target rate");
    Expect(!MakeGpuPressurePlan(
               std::numeric_limits<std::uint64_t>::max(),
               std::numeric_limits<std::uint64_t>::max(), 2U,
               &plan, &error),
           "reject plan arithmetic overflow");

    if (failures != 0) return 1;
    std::cout << "gpu pressure plan tests passed\n";
    return 0;
}
