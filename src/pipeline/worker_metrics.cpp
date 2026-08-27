#include "rdma_dada/pipeline/worker_metrics.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>

namespace rdma_dada {
namespace pipeline {

WorkerMetrics::WorkerMetrics() { Reset(); }

void WorkerMetrics::Reset() {
    blocks_ = 0U;
    transfer_elapsed_ns_ = 0U;
    input_bytes_ = 0U;
    output_bytes_ = 0U;
    service_ns_total_ = 0U;
    service_ns_max_ = 0U;
    output_wait_ns_total_ = 0U;
    output_wait_ns_max_ = 0U;
    cuda_h2d_ms_total_ = 0.0;
    cuda_algorithm_ms_total_ = 0.0;
    cuda_d2h_ms_total_ = 0.0;
}

void WorkerMetrics::SetTransferElapsedNs(std::uint64_t transfer_elapsed_ns) {
    transfer_elapsed_ns_ = transfer_elapsed_ns;
}

void WorkerMetrics::RecordBlock(std::uint64_t input_bytes,
                                std::uint64_t output_bytes,
                                std::uint64_t service_ns,
                                std::uint64_t output_wait_ns,
                                double cuda_h2d_ms,
                                double cuda_algorithm_ms,
                                double cuda_d2h_ms) {
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
    output << std::setprecision(17)
           << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"blocks\": " << blocks_ << ",\n"
           << "  \"transfer_elapsed_ns\": " << transfer_elapsed_ns_ << ",\n"
           << "  \"input_payload_gbps\": " << input_payload_gbps << ",\n"
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

}  // namespace pipeline
}  // namespace rdma_dada
