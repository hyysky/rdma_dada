#ifndef RDMA_DADA_PIPELINE_WORKER_METRICS_H
#define RDMA_DADA_PIPELINE_WORKER_METRICS_H

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace pipeline {

class WorkerMetrics {
public:
    WorkerMetrics();

    void Reset();
    void SetTransferElapsedNs(std::uint64_t transfer_elapsed_ns);
    void RecordBlock(std::uint64_t input_bytes,
                     std::uint64_t output_bytes,
                     std::uint64_t service_ns,
                     std::uint64_t output_wait_ns,
                     double cuda_h2d_ms,
                     double cuda_algorithm_ms,
                     double cuda_d2h_ms);
    bool WriteJson(const std::string& path, std::string* error) const;

    std::uint64_t blocks() const { return blocks_; }
    std::uint64_t transfer_elapsed_ns() const { return transfer_elapsed_ns_; }
    std::uint64_t input_bytes() const { return input_bytes_; }
    std::uint64_t output_bytes() const { return output_bytes_; }
    std::uint64_t service_ns_total() const { return service_ns_total_; }
    std::uint64_t service_ns_max() const { return service_ns_max_; }
    std::uint64_t output_wait_ns_total() const { return output_wait_ns_total_; }
    std::uint64_t output_wait_ns_max() const { return output_wait_ns_max_; }

private:
    std::uint64_t blocks_;
    std::uint64_t transfer_elapsed_ns_;
    std::uint64_t input_bytes_;
    std::uint64_t output_bytes_;
    std::uint64_t service_ns_total_;
    std::uint64_t service_ns_max_;
    std::uint64_t output_wait_ns_total_;
    std::uint64_t output_wait_ns_max_;
    double cuda_h2d_ms_total_;
    double cuda_algorithm_ms_total_;
    double cuda_d2h_ms_total_;
};

}  // namespace pipeline
}  // namespace rdma_dada

#endif
