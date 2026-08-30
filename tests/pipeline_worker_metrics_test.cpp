#include "rdma_dada/pipeline/worker_metrics.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main() {
    rdma_dada::pipeline::WorkerMetrics metrics;
    metrics.ConfigureExecution("STAGED_PIPELINE", 3U, 900U, 120U);
    metrics.ConfigureCudaStreamTopology(1U, 3U, 3U);
    metrics.ConfigureCudaSubmissionPolicy("ONE_BLOCK_H2D_LOOKAHEAD");
    metrics.RecordInputRingRegistration(8U, 419430400U, 75000000U);
    metrics.RecordSubmission(100U, 11U, 7U, 1U, 1000U);
    metrics.RecordStagedSubmissionTiming(0U, 2U, 3U, false, 0U);
    metrics.RecordSubmission(200U, 13U, 5U, 2U, 1500U);
    metrics.RecordStagedSubmissionTiming(1U, 4U, 6U, true, 17U);
    metrics.RecordStagedSubmissionTiming(2U, 8U, 9U, true, 23U);
    metrics.RecordH2dComputeOverlap(1500000U);
    metrics.RecordH2dComputeOverlap(2500000U);
    metrics.RecordH2dLookaheadSubmission(false);
    metrics.RecordH2dLookaheadSubmission(false);
    metrics.RecordH2dLookaheadSubmission(true);
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
        contents.str().find("\"cuda_h2d_stream_count\": 1") ==
            std::string::npos ||
        contents.str().find("\"cuda_compute_stream_count\": 3") ==
            std::string::npos ||
        contents.str().find("\"cuda_d2h_stream_count\": 3") ==
            std::string::npos ||
        contents.str().find(
            "\"cuda_submission_policy\": \"ONE_BLOCK_H2D_LOOKAHEAD\"") ==
            std::string::npos ||
        contents.str().find("\"h2d_lookahead_submission_count\": 2") ==
            std::string::npos ||
        contents.str().find("\"h2d_lookahead_eod_flush_count\": 1") ==
            std::string::npos ||
        contents.str().find("\"h2d_compute_overlap_sample_count\": 2") ==
            std::string::npos ||
        contents.str().find("\"h2d_compute_overlap_ns_total\": 4000000") ==
            std::string::npos ||
        contents.str().find("\"h2d_compute_overlap_ns_max\": 2500000") ==
            std::string::npos ||
        contents.str().find("\"slot_submission_counts\": [1, 1, 1]") ==
            std::string::npos ||
        contents.str().find(
            "\"submit_return_to_next_entry_ns_sample_count\": 2") ==
            std::string::npos ||
        contents.str().find(
            "\"submit_return_to_next_entry_ns_total\": 40") ==
            std::string::npos ||
        contents.str().find(
            "\"submit_return_to_next_entry_ns_max\": 23") ==
            std::string::npos ||
        contents.str().find("\"slot_acquire_wait_ns_total\": 14") ==
            std::string::npos ||
        contents.str().find("\"slot_acquire_wait_ns_max\": 8") ==
            std::string::npos ||
        contents.str().find("\"h2d_lease_wait_ns_total\": 18") ==
            std::string::npos ||
        contents.str().find("\"h2d_lease_wait_ns_max\": 9") ==
            std::string::npos ||
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
