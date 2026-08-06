#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"

#include <iostream>
#include <fstream>
#include <limits>
#include <cstdio>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {
int failures = 0;
void Expect(bool condition, const std::string& message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: vdif_unpack_config_test CONFIG\n";
        return 2;
    }
    namespace unpack = rdma_dada::modules::vdif_unpack;
    unpack::VdifUnpackConfig config = {};
    std::string error;
    Expect(unpack::LoadVdifUnpackConfig(argv[1], &config, &error),
           "example config loads: " + error);
    Expect(config.input_key == 0xdadaU && config.output_key == 0xcacaU,
           "hexadecimal ring keys parse");
    Expect(config.first_channel_id == 100U, "first channel parses");
    Expect(config.antenna_map.size() == 4U &&
           config.antenna_map[0] == 10U && config.antenna_map[3] == 13U,
           "Station IDs preserve antenna order");
    Expect(config.window_blocks == 2U, "two raw-block reorder horizon");
    Expect(!config.run_once, "example waits for consecutive transfers");

    {
        std::ifstream source(argv[1]);
        std::ostringstream contents;
        contents << source.rdbuf();
        std::string invalid_json = contents.str();
        const std::string marker = "\"schema_version\": 1,";
        const std::string::size_type position = invalid_json.find(marker);
        Expect(position != std::string::npos, "example contains schema marker");
        if (position != std::string::npos) {
            invalid_json.insert(position + marker.size(),
                                "\n  \"unexpected\": true,");
            std::ostringstream temp_name;
            temp_name << "/tmp/rdma_dada_vdif_unpack_config_"
                      << static_cast<long>(getpid()) << ".json";
            {
                std::ofstream temporary(temp_name.str().c_str());
                temporary << invalid_json;
            }
            unpack::VdifUnpackConfig unpublished = config;
            error.clear();
            Expect(!unpack::LoadVdifUnpackConfig(temp_name.str(),
                                                 &unpublished, &error),
                   "unknown JSON field is rejected");
            std::remove(temp_name.str().c_str());
        }
    }

    rdma_dada::PipelineConfig pipeline = {};
    rdma_dada::PacketFormatConfig packet = {};
    Expect(rdma_dada::LoadPipelineConfig(config.pipeline_config_path,
                                         &pipeline, &error),
           "referenced pipeline config loads: " + error);
    Expect(rdma_dada::LoadPacketFormatConfig(config.packet_format_path,
                                             &packet, &error),
           "referenced packet format loads: " + error);
    unpack::VdifUnpackLayout layout = {};
    error.clear();
    Expect(unpack::ComputeVdifUnpackLayout(config, pipeline, packet,
                                           &layout, &error),
           "unpack layout computes: " + error);
    if (failures == 0) {
        Expect(layout.raw_record_bytes == 12320U, "raw record includes header");
        Expect(layout.records_per_raw_block == 16U, "records per raw block");
        Expect(layout.group_bytes == 49152U, "one all-antenna payload group");
        Expect(layout.window_capacity_groups == 8U, "window group capacity");
        Expect(layout.window_bytes == 393216U,
               "window holds payload only, not Project VDIF headers");
        Expect(layout.compute_block_bytes == 196608U,
               "compute block is one raw-block payload equivalent");
    }

    unpack::VdifUnpackConfig invalid = config;
    invalid.antenna_map[3] = invalid.antenna_map[0];
    Expect(!unpack::ComputeVdifUnpackLayout(invalid, pipeline, packet,
                                            &layout, &error),
           "duplicate Station IDs rejected");
    invalid = config;
    invalid.antenna_map.pop_back();
    Expect(!unpack::ComputeVdifUnpackLayout(invalid, pipeline, packet,
                                            &layout, &error),
           "Station map length must equal NANT");
    invalid = config;
    invalid.window_blocks = 1;
    Expect(!unpack::ComputeVdifUnpackLayout(invalid, pipeline, packet,
                                            &layout, &error),
           "cross-block window requires at least two raw blocks");
    invalid = config;
    invalid.max_window_bytes = 393215U;
    Expect(!unpack::ComputeVdifUnpackLayout(invalid, pipeline, packet,
                                            &layout, &error),
           "configured allocation cap is enforced");
    rdma_dada::PacketFormatConfig mismatch = packet;
    mismatch.payload_bytes += 8U;
    Expect(!unpack::ComputeVdifUnpackLayout(config, pipeline, mismatch,
                                            &layout, &error),
           "packet profile mismatch rejected");
    invalid = config;
    invalid.window_blocks = std::numeric_limits<std::uint32_t>::max();
    invalid.max_window_bytes = std::numeric_limits<std::uint64_t>::max();
    rdma_dada::PipelineConfig huge = pipeline;
    huge.records_per_block = std::numeric_limits<std::uint64_t>::max() - 3U;
    Expect(!unpack::ComputeVdifUnpackLayout(invalid, huge, packet,
                                            &layout, &error),
           "window arithmetic overflow rejected");

    if (failures) return 1;
    std::cout << "vdif_unpack_config_test passed\n";
    return 0;
}
