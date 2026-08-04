#include "rdma_dada/pipeline/worker_config.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: pipeline_worker_config_inspect CONFIG\n";
        return EXIT_FAILURE;
    }

    rdma_dada::pipeline::WorkerConfig config;
    rdma_dada::pipeline::WorkerBlockGeometry geometry;
    std::string error;
    if (!rdma_dada::pipeline::LoadWorkerConfig(
            argv[1], &config, &error) ||
        !rdma_dada::pipeline::ComputeWorkerBlockGeometry(
            config, &geometry, &error)) {
        std::cerr << "invalid pipeline worker config: " << error << '\n';
        return EXIT_FAILURE;
    }

    std::cout
        << "INPUT_KEY=" << config.input_key_text << '\n'
        << "OUTPUT_KEY=" << config.output_key_text << '\n'
        << "NCHAN=" << config.nchan << '\n'
        << "NANT=" << config.nant << '\n'
        << "NPOL=" << config.npol << '\n'
        << "NBEAM=" << config.nbeam << '\n'
        << "UDP_PAYLOAD_BYTES=" << config.udp_payload_bytes << '\n'
        << "UDP_PACKETS_PER_ANTENNA_PER_BLOCK="
        << config.udp_packets_per_antenna_per_block << '\n'
        << "BLOCK_NTIME=" << geometry.ntime << '\n'
        << "UDP_ANTENNA_GROUP_BYTES="
        << geometry.udp_antenna_group_bytes << '\n'
        << "UDP_GROUP_MULTIPLE=" << geometry.udp_group_multiple << '\n'
        << "INPUT_FRAME_BYTES=" << geometry.input_frame_bytes << '\n'
        << "INPUT_BLOCK_BYTES=" << geometry.input_block_bytes << '\n'
        << "BEAMFORMED_FRAME_BYTES="
        << geometry.beamformed_frame_bytes << '\n'
        << "BEAMFORMED_BLOCK_BYTES="
        << geometry.beamformed_block_bytes << '\n'
        << "INTEGRATION_ENABLED="
        << (config.integration_enabled ? "true" : "false") << '\n'
        << "INTEGRATION_LENGTH=" << config.integration_length << '\n'
        << "INTEGRATION_OPERATION=" << config.integration_operation << '\n'
        << "OUTPUT_PRODUCT="
        << rdma_dada::pipeline::WorkerProductName(config.product) << '\n'
        << "OUTPUT_FRAME_BYTES=" << geometry.output_frame_bytes << '\n'
        << "PRODUCT_BLOCK_BYTES=" << geometry.product_block_bytes << '\n'
        << "OUTPUT_BLOCK_NTIME=" << geometry.output_ntime << '\n'
        << "OUTPUT_BLOCK_BYTES=" << geometry.output_block_bytes << '\n'
        << "SCRATCH_BLOCK_BYTES=" << geometry.scratch_block_bytes << '\n';
    return EXIT_SUCCESS;
}
