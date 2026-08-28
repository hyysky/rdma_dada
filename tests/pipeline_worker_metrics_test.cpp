#include "rdma_dada/pipeline/worker_metrics.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main() {
    rdma_dada::pipeline::WorkerMetrics metrics;
    metrics.ConfigureExecution("STAGED_PIPELINE", 3U, 900U, 120U);
    metrics.RecordInputRingRegistration(8U, 419430400U, 75000000U);
    metrics.RecordSubmission(100U, 11U, 7U, 1U, 1000U);
    metrics.RecordSubmission(200U, 13U, 5U, 2U, 1500U);
    metrics.RecordCompletion(false, 17U);
    metrics.RecordCompletion(true, 19U);
    metrics.RecordPublication(40U, 3U, 2000U);
    metrics.RecordPublication(80U, 4U, 3400U);
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
        contents.str().find("\"active_elapsed_ns\": 2400") ==
            std::string::npos ||
        contents.str().find("\"active_input_payload_gbps\": 1") ==
            std::string::npos ||
        contents.str().find("\"service_ns_mean\": 80") ==
            std::string::npos ||
        contents.str().find("\"output_wait_ns_mean\": 7.5") ==
            std::string::npos ||
        contents.str().find("\"cuda_algorithm_ms_total\": 3") ==
            std::string::npos ||
        contents.str().find("\"execution_mode\": \"STAGED_PIPELINE\"") ==
            std::string::npos ||
        contents.str().find("\"inflight_blocks\": 3") == std::string::npos ||
        contents.str().find("\"submitted_blocks\": 2") == std::string::npos ||
        contents.str().find("\"completed_blocks\": 2") == std::string::npos ||
        contents.str().find("\"published_blocks\": 2") == std::string::npos ||
        contents.str().find("\"max_inflight\": 2") == std::string::npos ||
        contents.str().find("\"completion_reorder_count\": 1") ==
            std::string::npos ||
        contents.str().find("\"input_staging_copy_ns_total\": 24") ==
            std::string::npos ||
        contents.str().find("\"output_staging_copy_ns_total\": 7") ==
            std::string::npos ||
        contents.str().find("\"planned_device_bytes\": 900") ==
            std::string::npos ||
        contents.str().find("\"planned_pinned_host_bytes\": 120") ==
            std::string::npos ||
        contents.str().find("\"input_ring_cuda_registered\": true") ==
            std::string::npos ||
        contents.str().find("\"registered_ring_blocks\": 8") ==
            std::string::npos ||
        contents.str().find("\"registered_ring_bytes\": 419430400") ==
            std::string::npos ||
        contents.str().find("\"input_ring_registration_ns\": 75000000") ==
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
