#ifndef RDMA_DADA_TESTING_GPU_PRESSURE_PLAN_H
#define RDMA_DADA_TESTING_GPU_PRESSURE_PLAN_H

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace testing {

struct GpuPressurePlan {
    std::uint64_t block_bytes;
    std::uint64_t target_bits_per_second;
    std::uint64_t duration_seconds;
    std::uint64_t blocks_per_second;
    std::uint64_t block_count;
    std::uint64_t bytes_per_second;
    std::uint64_t total_bytes;
};

bool MakeGpuPressurePlan(std::uint64_t block_bytes,
                         std::uint64_t target_bits_per_second,
                         std::uint64_t duration_seconds,
                         GpuPressurePlan* plan,
                         std::string* error);

std::uint64_t ScheduledOffsetNs(std::uint64_t block_index,
                                std::uint64_t blocks_per_second);

}  // namespace testing
}  // namespace rdma_dada

#endif
