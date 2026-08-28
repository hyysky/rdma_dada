#pragma once

#include "rdma_dada/config/resolved_observation_plan.h"

#include <cstdint>
#include <string>

namespace rdma_dada {

const std::uint32_t kInitialGpuDeadlineReservePercent = 20U;

struct GpuPipelineBudget {
    std::string execution_mode;
    std::uint32_t inflight_blocks;
    std::uint32_t deadline_reserve_percent;
    std::uint64_t observation_payload_bits_per_second;
    std::uint64_t budget_target_payload_bits_per_second;
    std::string rate_source;
    std::uint64_t block_interval_ns;
    std::uint64_t service_deadline_ns;
    std::uint64_t compute_block_bytes;
    std::uint64_t converted_block_bytes;
    std::uint64_t beamformed_block_bytes;
    std::uint64_t product_block_bytes;
    std::uint64_t output_block_bytes;
    std::uint64_t required_h2d_bytes_per_second;
    std::uint64_t required_conversion_output_bytes_per_second;
    std::uint64_t required_beamform_output_bytes_per_second;
    std::uint64_t required_product_output_bytes_per_second;
    std::uint64_t required_d2h_bytes_per_second;
    std::uint64_t host_device_transfer_bytes_per_block;
    std::uint64_t required_combined_host_device_bytes_per_second;
    std::uint64_t device_input_bytes;
    std::uint64_t device_converted_bytes;
    std::uint64_t device_scratch_bytes;
    std::uint64_t device_output_bytes;
    std::uint64_t device_weight_bytes;
    std::uint64_t device_bytes_per_slot;
    std::uint64_t slot_device_bytes_total;
    std::uint64_t planned_device_bytes;
    std::uint64_t recommended_free_device_bytes;
    std::uint64_t pinned_input_bytes;
    std::uint64_t pinned_output_bytes;
    std::uint64_t planned_pinned_host_bytes;
};

bool ComputeGpuPipelineBudget(const ResolvedObservationPlan& plan,
                              GpuPipelineBudget* budget,
                              std::string* error);

bool ComputeGpuPipelineBudgetForPayloadRate(
    const ResolvedObservationPlan& plan,
    std::uint64_t target_payload_bits_per_second,
    GpuPipelineBudget* budget,
    std::string* error);

bool ParsePayloadGigabitsPerSecond(const std::string& text,
                                   std::uint64_t* bits_per_second,
                                   std::string* error);

std::string SerializeGpuPipelineBudget(const GpuPipelineBudget& budget);

}  // namespace rdma_dada
