#include "rdma_dada/config/pipeline_config.h"

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: pipeline_config_inspect CONFIG\n";
        return EXIT_FAILURE;
    }

    rdma_dada::PipelineConfig config;
    rdma_dada::PipelineLayout layout;
    std::string error;
    if (!rdma_dada::LoadPipelineConfig(argv[1], &config, &error) ||
        !rdma_dada::ComputePipelineLayout(config, &layout, &error)) {
        std::cerr << "invalid pipeline config: " << error << '\n';
        return EXIT_FAILURE;
    }

    // Machine-readable numeric output used by scripts/run_demo.sh. Keeping
    // this list numeric avoids shell evaluation of arbitrary config content.
    std::cout << "RAW_RECORD_BYTES=" << layout.raw_record_bytes << '\n'
              << "RAW_BLOCK_BYTES=" << layout.raw_block_bytes << '\n'
              << "RAW_RING_BLOCKS=" << config.raw_ring_blocks << '\n'
              << "RAW_RING_BYTES=" << layout.raw_ring_bytes << '\n'
              << "RAW_FILE_BYTES=" << layout.raw_file_bytes << '\n'
              << "DBDISK_ENABLED=" << (config.disk_enabled ? 1 : 0) << '\n'
              << "DIRECT_IO=" << (config.direct_io ? 1 : 0) << '\n';
    return EXIT_SUCCESS;
}
