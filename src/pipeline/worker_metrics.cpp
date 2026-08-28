#include "rdma_dada/pipeline/worker_metrics.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>

namespace rdma_dada {
namespace pipeline {

WorkerMetrics::WorkerMetrics() { Reset(); }

void WorkerMetrics::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    blocks_ = 0U;
    transfer_elapsed_ns_ = 0U;
    input_bytes_ = 0U;
    output_bytes_ = 0U;
    service_ns_total_ = 0U;
    service_ns_max_ = 0U;
    output_wait_ns_total_ = 0U;
    output_wait_ns_max_ = 0U;
    execution_mode_ = "SYNCHRONOUS_DIRECT";
    inflight_blocks_ = 1U;
    submitted_blocks_ = 0U;
    completed_blocks_ = 0U;
    published_blocks_ = 0U;
    max_inflight_ = 0U;
    completion_reorder_count_ = 0U;
    slot_wait_ns_total_ = 0U;
    slot_wait_ns_max_ = 0U;
    writer_wait_ns_total_ = 0U;
    writer_wait_ns_max_ = 0U;
    input_staging_copy_ns_total_ = 0U;
    input_staging_copy_ns_max_ = 0U;
    output_staging_copy_ns_total_ = 0U;
    output_staging_copy_ns_max_ = 0U;
    input_staging_bytes_ = 0U;
    output_staging_bytes_ = 0U;
    planned_device_bytes_ = 0U;
    planned_pinned_host_bytes_ = 0U;
    input_ring_cuda_registered_ = false;
    registered_ring_blocks_ = 0U;
    registered_ring_bytes_ = 0U;
    input_ring_registration_ns_ = 0U;
    active_started_ = false;
    active_finished_ = false;
    active_start_ns_ = 0U;
    active_end_ns_ = 0U;
    cuda_h2d_ms_total_ = 0.0;
    cuda_algorithm_ms_total_ = 0.0;
    cuda_d2h_ms_total_ = 0.0;
}

void WorkerMetrics::RecordInputRingRegistration(
    std::uint64_t registered_ring_blocks,
    std::uint64_t registered_ring_bytes,
    std::uint64_t registration_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    input_ring_cuda_registered_ = true;
    registered_ring_blocks_ = registered_ring_blocks;
    registered_ring_bytes_ = registered_ring_bytes;
    input_ring_registration_ns_ = registration_ns;
}

void WorkerMetrics::ConfigureExecution(
    const std::string& execution_mode, std::uint32_t inflight_blocks,
    std::uint64_t planned_device_bytes,
    std::uint64_t planned_pinned_host_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    execution_mode_ = execution_mode;
    inflight_blocks_ = inflight_blocks;
    planned_device_bytes_ = planned_device_bytes;
    planned_pinned_host_bytes_ = planned_pinned_host_bytes;
}

void WorkerMetrics::RecordSubmission(
    std::uint64_t input_staging_bytes,
    std::uint64_t input_staging_copy_ns,
    std::uint64_t slot_wait_ns,
    std::uint64_t current_inflight,
    std::uint64_t monotonic_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!active_started_) {
        active_started_ = true;
        active_start_ns_ = monotonic_ns;
    }
    ++submitted_blocks_;
    input_staging_bytes_ += input_staging_bytes;
    input_staging_copy_ns_total_ += input_staging_copy_ns;
    input_staging_copy_ns_max_ =
        std::max(input_staging_copy_ns_max_, input_staging_copy_ns);
    slot_wait_ns_total_ += slot_wait_ns;
    slot_wait_ns_max_ = std::max(slot_wait_ns_max_, slot_wait_ns);
    max_inflight_ = std::max(max_inflight_, current_inflight);
}

void WorkerMetrics::RecordCompletion(bool reordered,
                                     std::uint64_t writer_wait_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++completed_blocks_;
    if (reordered) ++completion_reorder_count_;
    writer_wait_ns_total_ += writer_wait_ns;
    writer_wait_ns_max_ = std::max(writer_wait_ns_max_, writer_wait_ns);
}

void WorkerMetrics::RecordPublication(
    std::uint64_t output_staging_bytes,
    std::uint64_t output_staging_copy_ns,
    std::uint64_t monotonic_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++published_blocks_;
    output_staging_bytes_ += output_staging_bytes;
    output_staging_copy_ns_total_ += output_staging_copy_ns;
    output_staging_copy_ns_max_ =
        std::max(output_staging_copy_ns_max_, output_staging_copy_ns);
    active_finished_ = true;
    active_end_ns_ = monotonic_ns;
}

void WorkerMetrics::SetTransferElapsedNs(std::uint64_t transfer_elapsed_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    transfer_elapsed_ns_ = transfer_elapsed_ns;
}

void WorkerMetrics::RecordBlock(std::uint64_t input_bytes,
                                std::uint64_t output_bytes,
                                std::uint64_t service_ns,
                                std::uint64_t output_wait_ns,
                                double cuda_h2d_ms,
                                double cuda_algorithm_ms,
                                double cuda_d2h_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++blocks_;
    input_bytes_ += input_bytes;
    output_bytes_ += output_bytes;
    service_ns_total_ += service_ns;
    service_ns_max_ = std::max(service_ns_max_, service_ns);
    output_wait_ns_total_ += output_wait_ns;
    output_wait_ns_max_ = std::max(output_wait_ns_max_, output_wait_ns);
    cuda_h2d_ms_total_ += cuda_h2d_ms;
    cuda_algorithm_ms_total_ += cuda_algorithm_ms;
    cuda_d2h_ms_total_ += cuda_d2h_ms;
}

bool WorkerMetrics::WriteJson(const std::string& path,
                              std::string* error) const {
    if (path.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string temporary_path = path + ".tmp";
    std::ofstream output(
        temporary_path.c_str(), std::ios::out | std::ios::trunc);
    if (!output) {
        if (error) *error = "cannot open pipeline worker metrics JSON";
        return false;
    }
    const double service_ns_mean = blocks_ == 0U ? 0.0 :
        static_cast<double>(service_ns_total_) / static_cast<double>(blocks_);
    const double output_wait_ns_mean = blocks_ == 0U ? 0.0 :
        static_cast<double>(output_wait_ns_total_) /
        static_cast<double>(blocks_);
    const double input_payload_gbps = transfer_elapsed_ns_ == 0U ? 0.0 :
        static_cast<double>(input_bytes_) * 8.0 /
        static_cast<double>(transfer_elapsed_ns_);
    const std::uint64_t active_elapsed_ns =
        active_started_ && active_finished_ && active_end_ns_ >= active_start_ns_
            ? active_end_ns_ - active_start_ns_
            : 0U;
    const double active_input_payload_gbps = active_elapsed_ns == 0U ? 0.0 :
        static_cast<double>(input_bytes_) * 8.0 /
        static_cast<double>(active_elapsed_ns);
    output << std::setprecision(17)
           << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"execution_mode\": \"" << execution_mode_ << "\",\n"
           << "  \"inflight_blocks\": " << inflight_blocks_ << ",\n"
           << "  \"submitted_blocks\": " << submitted_blocks_ << ",\n"
           << "  \"completed_blocks\": " << completed_blocks_ << ",\n"
           << "  \"published_blocks\": " << published_blocks_ << ",\n"
           << "  \"max_inflight\": " << max_inflight_ << ",\n"
           << "  \"completion_reorder_count\": "
           << completion_reorder_count_ << ",\n"
           << "  \"slot_wait_ns_total\": " << slot_wait_ns_total_ << ",\n"
           << "  \"slot_wait_ns_max\": " << slot_wait_ns_max_ << ",\n"
           << "  \"writer_wait_ns_total\": " << writer_wait_ns_total_ << ",\n"
           << "  \"writer_wait_ns_max\": " << writer_wait_ns_max_ << ",\n"
           << "  \"input_staging_copy_ns_total\": "
           << input_staging_copy_ns_total_ << ",\n"
           << "  \"input_staging_copy_ns_max\": "
           << input_staging_copy_ns_max_ << ",\n"
           << "  \"output_staging_copy_ns_total\": "
           << output_staging_copy_ns_total_ << ",\n"
           << "  \"output_staging_copy_ns_max\": "
           << output_staging_copy_ns_max_ << ",\n"
           << "  \"input_staging_bytes\": " << input_staging_bytes_ << ",\n"
           << "  \"output_staging_bytes\": " << output_staging_bytes_ << ",\n"
           << "  \"planned_device_bytes\": " << planned_device_bytes_ << ",\n"
           << "  \"planned_pinned_host_bytes\": "
           << planned_pinned_host_bytes_ << ",\n"
           << "  \"input_ring_cuda_registered\": "
           << (input_ring_cuda_registered_ ? "true" : "false") << ",\n"
           << "  \"registered_ring_blocks\": "
           << registered_ring_blocks_ << ",\n"
           << "  \"registered_ring_bytes\": "
           << registered_ring_bytes_ << ",\n"
           << "  \"input_ring_registration_ns\": "
           << input_ring_registration_ns_ << ",\n"
           << "  \"blocks\": " << blocks_ << ",\n"
           << "  \"transfer_elapsed_ns\": " << transfer_elapsed_ns_ << ",\n"
           << "  \"input_payload_gbps\": " << input_payload_gbps << ",\n"
           << "  \"active_elapsed_ns\": " << active_elapsed_ns << ",\n"
           << "  \"active_input_payload_gbps\": "
           << active_input_payload_gbps << ",\n"
           << "  \"input_bytes\": " << input_bytes_ << ",\n"
           << "  \"output_bytes\": " << output_bytes_ << ",\n"
           << "  \"service_ns_total\": " << service_ns_total_ << ",\n"
           << "  \"service_ns_max\": " << service_ns_max_ << ",\n"
           << "  \"service_ns_mean\": " << service_ns_mean << ",\n"
           << "  \"output_wait_ns_total\": " << output_wait_ns_total_ << ",\n"
           << "  \"output_wait_ns_max\": " << output_wait_ns_max_ << ",\n"
           << "  \"output_wait_ns_mean\": " << output_wait_ns_mean << ",\n"
           << "  \"cuda_h2d_ms_total\": " << cuda_h2d_ms_total_ << ",\n"
           << "  \"cuda_algorithm_ms_total\": "
           << cuda_algorithm_ms_total_ << ",\n"
           << "  \"cuda_d2h_ms_total\": " << cuda_d2h_ms_total_ << "\n"
           << "}\n";
    output.close();
    if (!output) {
        std::remove(temporary_path.c_str());
        if (error) *error = "cannot write pipeline worker metrics JSON";
        return false;
    }
    if (std::rename(temporary_path.c_str(), path.c_str()) != 0) {
        std::remove(temporary_path.c_str());
        if (error) *error = "cannot finalize pipeline worker metrics JSON";
        return false;
    }
    return true;
}

#define RDMA_DADA_METRIC_GETTER(name)                  \
    std::uint64_t WorkerMetrics::name() const {         \
        std::lock_guard<std::mutex> lock(mutex_);       \
        return name##_;                                 \
    }

RDMA_DADA_METRIC_GETTER(blocks)
RDMA_DADA_METRIC_GETTER(transfer_elapsed_ns)
RDMA_DADA_METRIC_GETTER(input_bytes)
RDMA_DADA_METRIC_GETTER(output_bytes)
RDMA_DADA_METRIC_GETTER(service_ns_total)
RDMA_DADA_METRIC_GETTER(service_ns_max)
RDMA_DADA_METRIC_GETTER(output_wait_ns_total)
RDMA_DADA_METRIC_GETTER(output_wait_ns_max)
RDMA_DADA_METRIC_GETTER(submitted_blocks)
RDMA_DADA_METRIC_GETTER(completed_blocks)
RDMA_DADA_METRIC_GETTER(published_blocks)
RDMA_DADA_METRIC_GETTER(max_inflight)
RDMA_DADA_METRIC_GETTER(completion_reorder_count)
RDMA_DADA_METRIC_GETTER(input_staging_bytes)
RDMA_DADA_METRIC_GETTER(output_staging_bytes)

#undef RDMA_DADA_METRIC_GETTER

}  // namespace pipeline
}  // namespace rdma_dada
