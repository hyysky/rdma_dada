#include "rdma_dada/config/gpu_pipeline_budget.h"

#include <limits>
#include <sstream>

namespace rdma_dada {
namespace {

const std::uint64_t kNanosecondsPerSecond = UINT64_C(1000000000);

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right,
                const char* name, std::uint64_t* output,
                std::string* error) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return Fail(std::string(name) + " exceeds uint64 range", error);
    }
    *output = left + right;
    return true;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     const char* name, std::uint64_t* output,
                     std::string* error) {
    if (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Fail(std::string(name) + " exceeds uint64 range", error);
    }
    *output = left * right;
    return true;
}

bool FloorMultiplyDivide(std::uint64_t value, std::uint64_t multiplier,
                         std::uint64_t divisor, const char* name,
                         std::uint64_t* output, std::string* error) {
    if (divisor == 0U) return Fail(std::string(name) + " divisor is zero", error);
    std::uint64_t whole = 0U;
    std::uint64_t remainder_product = 0U;
    if (!CheckedMultiply(value / divisor, multiplier, name, &whole, error) ||
        !CheckedMultiply(value % divisor, multiplier, name,
                         &remainder_product, error)) {
        return false;
    }
    return CheckedAdd(whole, remainder_product / divisor, name, output, error);
}

bool CeilMultiplyDivide(std::uint64_t value, std::uint64_t multiplier,
                        std::uint64_t divisor, const char* name,
                        std::uint64_t* output, std::string* error) {
    std::uint64_t floor_value = 0U;
    if (!FloorMultiplyDivide(value, multiplier, divisor, name, &floor_value,
                             error)) {
        return false;
    }
    const std::uint64_t remainder = value % divisor;
    std::uint64_t remainder_product = 0U;
    if (!CheckedMultiply(remainder, multiplier, name, &remainder_product,
                         error)) {
        return false;
    }
    if (remainder_product % divisor == 0U) {
        *output = floor_value;
        return true;
    }
    return CheckedAdd(floor_value, 1U, name, output, error);
}

bool RequiredRate(std::uint64_t bytes, std::uint64_t deadline_ns,
                  const char* name, std::uint64_t* output,
                  std::string* error) {
    return CeilMultiplyDivide(bytes, kNanosecondsPerSecond, deadline_ns,
                              name, output, error);
}

bool DecimalDigits(const std::string& text) {
    if (text.empty()) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] < '0' || text[index] > '9') return false;
    }
    return true;
}

bool ParseUint64(const std::string& text, const char* name,
                 std::uint64_t* value, std::string* error) {
    if (!DecimalDigits(text)) {
        return Fail(std::string(name) + " must contain decimal digits", error);
    }
    std::uint64_t result = 0U;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const std::uint64_t digit =
            static_cast<std::uint64_t>(text[index] - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) /
                         10U) {
            return Fail(std::string(name) + " exceeds uint64 range", error);
        }
        result = result * 10U + digit;
    }
    *value = result;
    return true;
}

bool ComputeForPayloadRate(const ResolvedObservationPlan& plan,
                           std::uint64_t target_payload_bits_per_second,
                           const char* rate_source,
                           GpuPipelineBudget* budget,
                           std::string* error) {
    if (!budget) return Fail("GPU budget output pointer is null", error);
    if (plan.source.modules.empty()) {
        return Fail("GPU budget requires a processing module chain", error);
    }
    if (plan.payload_bytes_per_second == 0U || plan.compute_block_bytes == 0U ||
        plan.converted_block_bytes == 0U ||
        plan.beamformed_block_bytes == 0U || plan.output_block_bytes == 0U) {
        return Fail("GPU budget requires resolved processing geometry", error);
    }
    if (target_payload_bits_per_second == 0U ||
        target_payload_bits_per_second % 8U != 0U) {
        return Fail("GPU budget payload bit rate must be positive and byte-aligned",
                    error);
    }

    GpuPipelineBudget result = GpuPipelineBudget();
    result.deadline_reserve_percent = kInitialGpuDeadlineReservePercent;
    result.rate_source = rate_source;
    result.budget_target_payload_bits_per_second =
        target_payload_bits_per_second;
    std::uint64_t target_payload_bytes_per_second =
        target_payload_bits_per_second / 8U;
    if (!CheckedMultiply(plan.payload_bytes_per_second, 8U,
                         "observation payload bit rate",
                         &result.observation_payload_bits_per_second, error) ||
        !FloorMultiplyDivide(plan.compute_block_bytes, kNanosecondsPerSecond,
                             target_payload_bytes_per_second,
                             "compute block interval",
                             &result.block_interval_ns, error) ||
        !FloorMultiplyDivide(
            result.block_interval_ns,
            100U - kInitialGpuDeadlineReservePercent, 100U,
            "GPU service deadline", &result.service_deadline_ns, error)) {
        return false;
    }
    if (result.service_deadline_ns == 0U) {
        return Fail("GPU service deadline rounds to zero", error);
    }

    result.compute_block_bytes = plan.compute_block_bytes;
    result.converted_block_bytes = plan.converted_block_bytes;
    result.beamformed_block_bytes = plan.beamformed_block_bytes;
    result.product_block_bytes = plan.product_block_bytes;
    result.output_block_bytes = plan.output_block_bytes;
    if (!RequiredRate(plan.compute_block_bytes, result.service_deadline_ns,
                      "required H2D rate",
                      &result.required_h2d_bytes_per_second, error) ||
        !RequiredRate(plan.converted_block_bytes, result.service_deadline_ns,
                      "required conversion output rate",
                      &result.required_conversion_output_bytes_per_second,
                      error) ||
        !RequiredRate(plan.beamformed_block_bytes, result.service_deadline_ns,
                      "required beamform output rate",
                      &result.required_beamform_output_bytes_per_second,
                      error) ||
        !RequiredRate(plan.product_block_bytes, result.service_deadline_ns,
                      "required product output rate",
                      &result.required_product_output_bytes_per_second,
                      error) ||
        !RequiredRate(plan.output_block_bytes, result.service_deadline_ns,
                      "required D2H rate",
                      &result.required_d2h_bytes_per_second, error)) {
        return false;
    }

    if (!CheckedAdd(plan.compute_block_bytes, plan.output_block_bytes,
                    "host/device transfer bytes per block",
                    &result.host_device_transfer_bytes_per_block, error) ||
        !RequiredRate(result.host_device_transfer_bytes_per_block,
                      result.service_deadline_ns,
                      "required combined host/device rate",
                      &result.required_combined_host_device_bytes_per_second,
                      error)) {
        return false;
    }

    result.device_input_bytes = plan.compute_block_bytes;
    result.device_converted_bytes = plan.converted_block_bytes;
    result.device_output_bytes = plan.output_block_bytes;
    if (plan.source.modules.size() == 1U) {
        result.device_scratch_bytes = 0U;
    } else if (plan.source.modules.size() == 2U) {
        result.device_scratch_bytes = plan.beamformed_block_bytes;
    } else {
        if (!CheckedAdd(plan.beamformed_block_bytes, plan.product_block_bytes,
                        "module-chain scratch bytes",
                        &result.device_scratch_bytes, error)) {
            return false;
        }
    }

    std::uint64_t weight_elements = 0U;
    if (!CheckedMultiply(plan.source.nchan, plan.source.npol,
                         "weight F*P", &weight_elements, error) ||
        !CheckedMultiply(weight_elements, plan.nant, "weight F*P*A",
                         &weight_elements, error) ||
        !CheckedMultiply(weight_elements, plan.nbeam, "weight F*P*A*B",
                         &weight_elements, error) ||
        !CheckedMultiply(weight_elements, 8U, "resident CF32 weight bytes",
                         &result.device_weight_bytes, error)) {
        return false;
    }

    std::uint64_t device_buffers = 0U;
    if (!CheckedAdd(result.device_input_bytes, result.device_converted_bytes,
                    "device buffer bytes", &device_buffers, error) ||
        !CheckedAdd(device_buffers, result.device_scratch_bytes,
                    "device buffer bytes", &device_buffers, error) ||
        !CheckedAdd(device_buffers, result.device_output_bytes,
                    "device buffer bytes", &device_buffers, error) ||
        !CheckedAdd(device_buffers, result.device_weight_bytes,
                    "planned device bytes", &result.planned_device_bytes,
                    error)) {
        return false;
    }
    std::uint64_t memory_reserve = 0U;
    if (!CeilMultiplyDivide(result.planned_device_bytes,
                            kInitialGpuDeadlineReservePercent, 100U,
                            "device memory reserve", &memory_reserve, error) ||
        !CheckedAdd(result.planned_device_bytes, memory_reserve,
                    "recommended free device bytes",
                    &result.recommended_free_device_bytes, error)) {
        return false;
    }
    *budget = result;
    return true;
}

}  // namespace

bool ComputeGpuPipelineBudget(const ResolvedObservationPlan& plan,
                              GpuPipelineBudget* budget,
                              std::string* error) {
    std::uint64_t observation_bits_per_second = 0U;
    if (!CheckedMultiply(plan.payload_bytes_per_second, 8U,
                         "observation payload bit rate",
                         &observation_bits_per_second, error)) {
        return false;
    }
    return ComputeForPayloadRate(plan, observation_bits_per_second,
                                 "OBSERVATION", budget, error);
}

bool ComputeGpuPipelineBudgetForPayloadRate(
    const ResolvedObservationPlan& plan,
    std::uint64_t target_payload_bits_per_second,
    GpuPipelineBudget* budget,
    std::string* error) {
    return ComputeForPayloadRate(plan, target_payload_bits_per_second,
                                 "PERFORMANCE_OVERRIDE", budget, error);
}

bool ParsePayloadGigabitsPerSecond(const std::string& text,
                                   std::uint64_t* bits_per_second,
                                   std::string* error) {
    if (!bits_per_second) {
        return Fail("payload bit rate output pointer is null", error);
    }
    const std::string::size_type decimal = text.find('.');
    if (decimal != std::string::npos &&
        text.find('.', decimal + 1U) != std::string::npos) {
        return Fail("budget payload Gbps contains multiple decimal points",
                    error);
    }
    const std::string whole = decimal == std::string::npos ?
        text : text.substr(0U, decimal);
    std::string fraction = decimal == std::string::npos ?
        std::string() : text.substr(decimal + 1U);
    if (!DecimalDigits(whole) ||
        (decimal != std::string::npos && !DecimalDigits(fraction))) {
        return Fail("budget payload Gbps must be a positive decimal", error);
    }
    if (fraction.size() > 9U) {
        return Fail("budget payload Gbps supports at most 9 decimal places",
                    error);
    }
    while (fraction.size() < 9U) fraction.push_back('0');
    std::uint64_t whole_value = 0U;
    std::uint64_t fraction_value = 0U;
    std::uint64_t whole_bits = 0U;
    if (!ParseUint64(whole, "budget payload Gbps", &whole_value, error) ||
        !ParseUint64(fraction, "budget payload Gbps fraction",
                     &fraction_value, error) ||
        !CheckedMultiply(whole_value, UINT64_C(1000000000),
                         "budget payload bit rate", &whole_bits, error) ||
        !CheckedAdd(whole_bits, fraction_value, "budget payload bit rate",
                    bits_per_second, error)) {
        return false;
    }
    if (*bits_per_second == 0U) {
        return Fail("budget payload Gbps must be positive", error);
    }
    return true;
}

std::string SerializeGpuPipelineBudget(const GpuPipelineBudget& budget) {
    std::ostringstream output;
    output << '{'
           << "\"deadline_reserve_percent\":"
           << budget.deadline_reserve_percent << ','
           << "\"observation_payload_bits_per_second\":"
           << budget.observation_payload_bits_per_second << ','
           << "\"budget_target_payload_bits_per_second\":"
           << budget.budget_target_payload_bits_per_second << ','
           << "\"rate_source\":\"" << budget.rate_source << "\","
           << "\"block_interval_ns\":" << budget.block_interval_ns << ','
           << "\"service_deadline_ns\":" << budget.service_deadline_ns << ','
           << "\"block_bytes\":{"
           << "\"compute\":" << budget.compute_block_bytes << ','
           << "\"converted\":" << budget.converted_block_bytes << ','
           << "\"beamformed\":" << budget.beamformed_block_bytes << ','
           << "\"product\":" << budget.product_block_bytes << ','
           << "\"output\":" << budget.output_block_bytes << "},"
           << "\"required_rates_bytes_per_second\":{"
           << "\"required_h2d_bytes_per_second\":"
           << budget.required_h2d_bytes_per_second << ','
           << "\"required_conversion_output_bytes_per_second\":"
           << budget.required_conversion_output_bytes_per_second << ','
           << "\"required_beamform_output_bytes_per_second\":"
           << budget.required_beamform_output_bytes_per_second << ','
           << "\"required_product_output_bytes_per_second\":"
           << budget.required_product_output_bytes_per_second << ','
           << "\"required_d2h_bytes_per_second\":"
           << budget.required_d2h_bytes_per_second << ','
           << "\"host_device_transfer_bytes_per_block\":"
           << budget.host_device_transfer_bytes_per_block << ','
           << "\"required_combined_host_device_bytes_per_second\":"
           << budget.required_combined_host_device_bytes_per_second << "},"
           << "\"device_memory\":{"
           << "\"device_input_bytes\":" << budget.device_input_bytes << ','
           << "\"device_converted_bytes\":"
           << budget.device_converted_bytes << ','
           << "\"device_scratch_bytes\":" << budget.device_scratch_bytes
           << ',' << "\"device_output_bytes\":"
           << budget.device_output_bytes << ','
           << "\"device_weight_bytes\":" << budget.device_weight_bytes
           << ',' << "\"planned_device_bytes\":"
           << budget.planned_device_bytes << ','
           << "\"recommended_free_device_bytes\":"
           << budget.recommended_free_device_bytes << ','
           << "\"excludes_cuda_runtime_and_library_workspace\":true}"
           << '}';
    return output.str();
}

}  // namespace rdma_dada
