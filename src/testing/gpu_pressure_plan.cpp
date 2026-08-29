#include "rdma_dada/testing/gpu_pressure_plan.h"

#include <limits>

namespace rdma_dada {
namespace testing {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result) return false;
    if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

std::uint64_t CeilDivide(std::uint64_t numerator,
                         std::uint64_t denominator) {
    return numerator / denominator + (numerator % denominator != 0U ? 1U : 0U);
}

}  // namespace

bool MakeGpuPressurePlan(std::uint64_t block_bytes,
                         std::uint64_t target_bits_per_second,
                         std::uint64_t duration_seconds,
                         GpuPressurePlan* plan,
                         std::string* error) {
    if (!plan) return Fail("pressure plan output is null", error);
    if (block_bytes == 0U) return Fail("block bytes must be positive", error);
    if (target_bits_per_second == 0U) {
        return Fail("target bit rate must be positive", error);
    }
    if (duration_seconds == 0U) {
        return Fail("duration must be positive", error);
    }

    const std::uint64_t target_bytes_per_second =
        CeilDivide(target_bits_per_second, 8U);
    const std::uint64_t blocks_per_second =
        CeilDivide(target_bytes_per_second, block_bytes);
    std::uint64_t bytes_per_second = 0U;
    std::uint64_t block_count = 0U;
    std::uint64_t total_bytes = 0U;
    if (!CheckedMultiply(blocks_per_second, block_bytes,
                         &bytes_per_second) ||
        !CheckedMultiply(blocks_per_second, duration_seconds,
                         &block_count) ||
        !CheckedMultiply(block_count, block_bytes, &total_bytes)) {
        return Fail("pressure plan arithmetic overflow", error);
    }

    plan->block_bytes = block_bytes;
    plan->target_bits_per_second = target_bits_per_second;
    plan->duration_seconds = duration_seconds;
    plan->blocks_per_second = blocks_per_second;
    plan->block_count = block_count;
    plan->bytes_per_second = bytes_per_second;
    plan->total_bytes = total_bytes;
    return true;
}

std::uint64_t ScheduledOffsetNs(std::uint64_t block_index,
                                std::uint64_t blocks_per_second) {
    if (blocks_per_second == 0U) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t seconds = block_index / blocks_per_second;
    const std::uint64_t remainder = block_index % blocks_per_second;
    std::uint64_t whole_ns = 0U;
    if (!CheckedMultiply(seconds, 1000000000ULL, &whole_ns)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t fraction_ns =
        (remainder * 1000000000ULL) / blocks_per_second;
    if (fraction_ns > std::numeric_limits<std::uint64_t>::max() - whole_ns) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return whole_ns + fraction_ns;
}

}  // namespace testing
}  // namespace rdma_dada
