#include "rdma_dada/pipeline/worker_metrics.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main() {
    rdma_dada::pipeline::WorkerMetrics metrics;
    metrics.SetTransferElapsedNs(1000U);
    metrics.RecordBlock(100U, 40U, 70U, 10U, 1.0, 2.0, 3.0);
    metrics.RecordBlock(200U, 80U, 90U, 5U, 0.5, 1.0, 1.5);
    if (metrics.blocks() != 2U || metrics.transfer_elapsed_ns() != 1000U ||
        metrics.input_bytes() != 300U ||
        metrics.output_bytes() != 120U ||
        metrics.service_ns_total() != 160U ||
        metrics.service_ns_max() != 90U ||
        metrics.output_wait_ns_total() != 15U ||
        metrics.output_wait_ns_max() != 10U) {
        std::cerr << "worker metrics aggregation failed\n";
        return 1;
    }
    const std::string path = "pipeline_worker_metrics_test.json";
    std::string error;
    if (!metrics.WriteJson(path, &error)) {
        std::cerr << error << '\n';
        return 1;
    }
    std::ifstream input(path.c_str());
    std::ostringstream contents;
    contents << input.rdbuf();
    std::remove(path.c_str());
    if (contents.str().find("\"blocks\": 2") == std::string::npos ||
        contents.str().find("\"transfer_elapsed_ns\": 1000") ==
            std::string::npos ||
        contents.str().find("\"input_payload_gbps\": 2.399") ==
            std::string::npos ||
        contents.str().find("\"service_ns_mean\": 80") ==
            std::string::npos ||
        contents.str().find("\"output_wait_ns_mean\": 7.5") ==
            std::string::npos ||
        contents.str().find("\"cuda_algorithm_ms_total\": 3") ==
            std::string::npos) {
        std::cerr << "worker metrics JSON failed\n";
        return 1;
    }
    std::ifstream temporary((path + ".tmp").c_str());
    if (temporary.good()) {
        std::cerr << "worker metrics temporary file remains\n";
        return 1;
    }
    std::cout << "pipeline_worker_metrics_test passed\n";
    return 0;
}
