#pragma once

#include "rdma_dada/config/resolved_observation_plan.h"

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace pipeline {

enum class WorkerProduct {
    kBeamformed,
    kPower,
    kStokes
};

struct WorkerConfig {
    std::uint32_t input_key;
    std::uint32_t output_key;
    std::string input_key_text;
    std::string output_key_text;

    std::string execution_backend;
    int cuda_device;
    bool run_once;

    std::uint64_t nchan;
    std::uint64_t nant;
    std::uint64_t npol;
    std::uint64_t udp_payload_bytes;
    std::uint64_t samples_per_udp;
    std::uint64_t udp_packets_per_antenna_per_block;

    double conversion_scale;

    std::string weights_file;
    std::string weights_order;
    std::string weights_id;
    double weights_scale;
    std::uint64_t nbeam;
    std::string compute_mode;

    WorkerProduct product;

    bool integration_enabled;
    std::uint64_t integration_length;
    std::string integration_operation;
};

struct WorkerBlockGeometry {
    std::uint64_t ntime;
    std::uint64_t udp_antenna_group_bytes;
    std::uint64_t udp_group_multiple;
    std::uint64_t input_frame_bytes;
    std::uint64_t input_block_bytes;
    std::uint64_t converted_frame_bytes;
    std::uint64_t converted_block_bytes;
    std::uint64_t beamformed_frame_bytes;
    std::uint64_t beamformed_block_bytes;
    std::uint64_t output_frame_bytes;
    std::uint64_t product_block_bytes;
    std::uint64_t output_ntime;
    std::uint64_t output_block_bytes;
    std::uint64_t scratch_block_bytes;
};

bool LoadWorkerConfig(const std::string& path, WorkerConfig* config,
                      std::string* error);

// Builds the worker runtime contract from the single resolved observation
// plan. No geometry or NBEAM is independently entered by the worker.
bool BuildWorkerConfigFromResolvedPlan(
    const ResolvedObservationPlan& plan,
    WorkerConfig* config,
    WorkerBlockGeometry* geometry,
    std::string* error);

bool LoadWorkerConfigFromResolvedPlan(
    const std::string& path,
    WorkerConfig* config,
    WorkerBlockGeometry* geometry,
    std::string* error);

// Compute full-block geometry from F/A/P, UDP grouping, block-scoped ATFP CI8
// input, converted TFPA CF32, and the configured Beamform/Power/Stokes product.
// product_block_bytes is before optional integration; output_block_bytes is
// final.
bool ComputeWorkerBlockGeometry(const WorkerConfig& config,
                                WorkerBlockGeometry* geometry,
                                std::string* error);

const char* WorkerProductName(WorkerProduct product);

}  // namespace pipeline
}  // namespace rdma_dada
