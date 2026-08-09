#include "rdma_dada/config/packet_format_config.h"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

std::string Join(const std::vector<std::string>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        result += values[index];
    }
    return result;
}

const char* SourceName(rdma_dada::PacketAxisValueSource source) {
    switch (source) {
        case rdma_dada::PacketAxisValueSource::kConstant: return "CONST";
        case rdma_dada::PacketAxisValueSource::kConfig: return "CONFIG";
        case rdma_dada::PacketAxisValueSource::kHeader: return "HEADER";
        case rdma_dada::PacketAxisValueSource::kDerived: return "DERIVED";
        case rdma_dada::PacketAxisValueSource::kLookup: return "LOOKUP";
    }
    return "UNKNOWN";
}

std::string AxisValueText(const rdma_dada::PacketAxisValue& value) {
    std::ostringstream output;
    output << SourceName(value.source) << ':';
    if (value.source == rdma_dada::PacketAxisValueSource::kConstant) {
        output << value.constant;
    } else {
        output << value.reference;
        if (value.source == rdma_dada::PacketAxisValueSource::kLookup) {
            output << ':' << value.input_field;
        }
    }
    return output.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: packet_format_inspect CONFIG\n";
        return EXIT_FAILURE;
    }

    rdma_dada::PacketFormatConfig config;
    std::string error;
    if (!rdma_dada::LoadPacketFormatConfig(argv[1], &config, &error)) {
        std::cerr << "invalid packet format: " << error << '\n';
        return EXIT_FAILURE;
    }
    const std::uint64_t sample_bytes = config.sample_format == "CI8" ? 2U : 4U;
    std::cout << "FORMAT_ID=" << config.format_id << '\n'
              << "APPLICATION_HEADER_BYTES="
              << config.application_header_bytes << '\n'
              << "HEADER_FIELD_COUNT=" << config.header_fields.size() << '\n'
              << "SAMPLE_FORMAT=" << config.sample_format << '\n'
              << "SAMPLE_ENCODING=" << config.sample_encoding << '\n'
              << "COMPONENT_ORDER=" << config.component_order << '\n'
              << "SAMPLE_BYTES=" << sample_bytes << '\n'
              << "PACKED_ORDER=" << Join(config.packed_order) << '\n';
    for (std::size_t index = 0; index < config.axes.size(); ++index) {
        std::cout << "AXIS_" << config.axes[index].name
                  << "_EXTENT=" << AxisValueText(config.axes[index].extent)
                  << '\n'
                  << "AXIS_" << config.axes[index].name
                  << "_ORIGIN=" << AxisValueText(config.axes[index].origin)
                  << '\n';
    }
    return EXIT_SUCCESS;
}
