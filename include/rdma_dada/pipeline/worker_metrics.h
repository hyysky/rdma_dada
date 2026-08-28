#ifndef RDMA_DADA_PIPELINE_WORKER_METRICS_H
#define RDMA_DADA_PIPELINE_WORKER_METRICS_H

#include <cstdint>
#include <mutex>
#include <string>

namespace rdma_dada {
namespace pipeline {

class WorkerMetrics {
public:
    WorkerMetrics();

    void Reset();
    void ConfigureExecution(const std::string& execution_mode,
                            std::uint32_t inflight_blocks,
                            std::uint64_t planned_device_bytes,
                            std::uint64_t planned_pinned_host_bytes);
    void RecordInputRingRegistration(std::uint64_t registered_ring_blocks,
                                     std::uint64_t registered_ring_bytes,
                                     std::uint64_t registration_ns);
    void RecordSubmission(std::uint64_t input_staging_bytes,
                          std::uint64_t input_staging_copy_ns,
                          std::uint64_t slot_wait_ns,
                          std::uint64_t current_inflight,
                          std::uint64_t monotonic_ns);
    void RecordCompletion(bool reordered, std::uint64_t writer_wait_ns);
    void RecordPublication(std::uint64_t output_staging_bytes,
                           std::uint64_t output_staging_copy_ns,
                           std::uint64_t monotonic_ns);
    void SetTransferElapsedNs(std::uint64_t transfer_elapsed_ns);
    void RecordBlock(std::uint64_t input_bytes,
                     std::uint64_t output_bytes,
                     std::uint64_t service_ns,
                     std::uint64_t output_wait_ns,
                     double cuda_h2d_ms,
                     double cuda_algorithm_ms,
                     double cuda_d2h_ms);
    bool WriteJson(const std::string& path, std::string* error) const;

    std::uint64_t blocks() const;
    std::uint64_t transfer_elapsed_ns() const;
    std::uint64_t input_bytes() const;
    std::uint64_t output_bytes() const;
    std::uint64_t service_ns_total() const;
    std::uint64_t service_ns_max() const;
    std::uint64_t output_wait_ns_total() const;
    std::uint64_t output_wait_ns_max() const;
    std::uint64_t submitted_blocks() const;
    std::uint64_t completed_blocks() const;
    std::uint64_t published_blocks() const;
    std::uint64_t max_inflight() const;
    std::uint64_t completion_reorder_count() const;
    std::uint64_t input_staging_bytes() const;
    std::uint64_t output_staging_bytes() const;

private:
    std::uint64_t blocks_;
    std::uint64_t transfer_elapsed_ns_;
    std::uint64_t input_bytes_;
    std::uint64_t output_bytes_;
    std::uint64_t service_ns_total_;
    std::uint64_t service_ns_max_;
    std::uint64_t output_wait_ns_total_;
    std::uint64_t output_wait_ns_max_;
    std::string execution_mode_;
    std::uint32_t inflight_blocks_;
    std::uint64_t submitted_blocks_;
    std::uint64_t completed_blocks_;
    std::uint64_t published_blocks_;
    std::uint64_t max_inflight_;
    std::uint64_t completion_reorder_count_;
    std::uint64_t slot_wait_ns_total_;
    std::uint64_t slot_wait_ns_max_;
    std::uint64_t writer_wait_ns_total_;
    std::uint64_t writer_wait_ns_max_;
    std::uint64_t input_staging_copy_ns_total_;
    std::uint64_t input_staging_copy_ns_max_;
    std::uint64_t output_staging_copy_ns_total_;
    std::uint64_t output_staging_copy_ns_max_;
    std::uint64_t input_staging_bytes_;
    std::uint64_t output_staging_bytes_;
    std::uint64_t planned_device_bytes_;
    std::uint64_t planned_pinned_host_bytes_;
    bool input_ring_cuda_registered_;
    std::uint64_t registered_ring_blocks_;
    std::uint64_t registered_ring_bytes_;
    std::uint64_t input_ring_registration_ns_;
    bool active_started_;
    bool active_finished_;
    std::uint64_t active_start_ns_;
    std::uint64_t active_end_ns_;
    double cuda_h2d_ms_total_;
    double cuda_algorithm_ms_total_;
    double cuda_d2h_ms_total_;
    mutable std::mutex mutex_;
};

}  // namespace pipeline
}  // namespace rdma_dada

#endif
