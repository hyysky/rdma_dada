#include "rdma_dada/config/packet_format_config.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string WithoutOutputOrder(const std::string& source) {
    std::string result = source;
    const std::string::size_type field = result.find("\"output_order\"");
    if (field == std::string::npos) return result;
    const std::string::size_type line_begin = result.rfind('\n', field);
    const std::string::size_type line_end = result.find('\n', field);
    if (line_begin == std::string::npos || line_end == std::string::npos)
        return result;
    result.erase(line_begin + 1U, line_end - line_begin);
    return result;
}

std::string WithLegacyOutputOrder(const std::string& wire_only) {
    std::string result = wire_only;
    const std::string marker = "\"packed_order\": [\"T\", \"F\", \"P\"],";
    const std::string::size_type position = result.find(marker);
    if (position != std::string::npos) {
        result.insert(position + marker.size(),
                      "\n    \"output_order\": [\"T\", \"F\", \"P\", \"A\"],");
    }
    return result;
}

std::string WithLegacyPayloadBytes(const std::string& wire_only) {
    std::string result = wire_only;
    const std::string marker = "\"application_header_bytes\": 32";
    const std::string::size_type position = result.find(marker);
    if (position != std::string::npos) {
        result.insert(position + marker.size(), ",\n    \"payload_bytes\": 12288");
    }
    return result;
}

std::string WithSchemaVersionOne(const std::string& wire_only) {
    std::string result = wire_only;
    const std::string marker = "\"schema_version\": 2";
    const std::string::size_type position = result.find(marker);
    if (position != std::string::npos) {
        result.replace(position, marker.size(), "\"schema_version\": 1");
    }
    return result;
}

std::string WriteTemporary(const std::string& suffix,
                           const std::string& contents) {
    std::ostringstream path;
    path << "/tmp/rdma_dada_packet_format_" << static_cast<long>(getpid())
         << '_' << suffix << ".json";
    std::ofstream output(path.str().c_str());
    output << contents;
    return path.str();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: packet_format_config_test CONFIG\n";
        return 2;
    }

    std::ifstream source(argv[1]);
    std::ostringstream buffer;
    buffer << source.rdbuf();
    const std::string wire_only = WithoutOutputOrder(buffer.str());
    const std::string wire_path = WriteTemporary("wire", wire_only);
    const std::string legacy_path = WriteTemporary(
        "legacy", WithLegacyOutputOrder(wire_only));
    const std::string payload_path = WriteTemporary(
        "payload", WithLegacyPayloadBytes(wire_only));
    const std::string schema_v1_path = WriteTemporary(
        "schema-v1", WithSchemaVersionOne(WithLegacyPayloadBytes(wire_only)));

    rdma_dada::PacketFormatConfig config;
    std::string error;
    Expect(rdma_dada::LoadPacketFormatConfig(wire_path, &config, &error),
           "wire-only packet format should load without output_order: " +
               error);
    rdma_dada::PacketFormatConfig unpublished = config;
    error.clear();
    Expect(!rdma_dada::LoadPacketFormatConfig(legacy_path, &unpublished,
                                              &error),
           "legacy payload.output_order is rejected as a non-wire field");
    error.clear();
    Expect(!rdma_dada::LoadPacketFormatConfig(payload_path, &unpublished,
                                              &error),
           "observation-specific record.payload_bytes is rejected");
    error.clear();
    Expect(!rdma_dada::LoadPacketFormatConfig(schema_v1_path, &unpublished,
                                              &error),
           "schema v1 profile requires migration");
    Expect(error.find("schema_version 1") != std::string::npos,
           "schema v1 rejection identifies the migration source");
    std::remove(wire_path.c_str());
    std::remove(legacy_path.c_str());
    std::remove(payload_path.c_str());
    std::remove(schema_v1_path.c_str());
    if (failures == 0) {
        Expect(config.schema_version == 2, "wire profile schema version is 2");
        Expect(config.application_header_bytes == 32,
               "Project VDIF v1 header is exactly 32 bytes");
        static const char* const expected_field_names[] = {
            "invalid_data", "legacy_mode", "seconds_from_reference_epoch",
            "word1_reserved", "reference_epoch",
            "frame_number_within_second", "vdif_version",
            "channel_count_code", "frame_length_units_8_bytes", "data_type",
            "component_bits_minus_one", "thread_id", "station_id", "edv",
            "profile_version", "sample_encoding", "flags",
            "first_channel_id", "nchan", "npol", "nsamp_per_packet",
            "word7_reserved"
        };
        static const std::uint32_t expected_offsets[] = {
            0, 0, 0, 4, 4, 4, 8, 8, 8, 12, 12, 12, 12, 16, 16, 16,
            16, 20, 20, 20, 24, 28
        };
        static const std::uint32_t expected_bit_offsets[] = {
            31, 30, 0, 30, 24, 0, 29, 24, 0, 31, 26, 16, 0, 24, 16, 8,
            0, 16, 8, 0, 0, 0
        };
        static const std::uint32_t expected_bit_widths[] = {
            1, 1, 30, 2, 6, 24, 3, 5, 24, 1, 5, 10, 16, 8, 8, 8, 8,
            16, 8, 8, 32, 32
        };
        const std::size_t expected_field_count =
            sizeof(expected_field_names) / sizeof(expected_field_names[0]);
        Expect(config.header_fields.size() == expected_field_count,
               "all 22 Project VDIF v1 fields are declared");
        if (config.header_fields.size() == expected_field_count) {
            for (std::size_t index = 0; index < expected_field_count; ++index) {
                const rdma_dada::ApplicationHeaderField& field =
                    config.header_fields[index];
                Expect(field.name == expected_field_names[index],
                       "field name matches the Project VDIF v1 table");
                Expect(field.offset_bytes == expected_offsets[index],
                       "field word offset matches the Project VDIF v1 table");
                Expect(field.bit_offset == expected_bit_offsets[index],
                       "field bit offset matches the Project VDIF v1 table");
                Expect(field.bit_width == expected_bit_widths[index],
                       "field bit width matches the Project VDIF v1 table");
                Expect(field.type == rdma_dada::PacketFieldType::kUint32,
                       "every v1 field is decoded from a UINT32 storage word");
                Expect(field.endianness == rdma_dada::PacketEndianness::kLittle,
                       "every v1 storage word is little-endian");
            }
        }
        Expect(!rdma_dada::IsSignedPacketFieldType(
                   rdma_dada::PacketFieldType::kUint32),
               "UINT32 is inferred as unsigned");
        Expect(rdma_dada::IsSignedPacketFieldType(
                   rdma_dada::PacketFieldType::kInt16),
               "INT16 is inferred as signed");
        Expect(config.sample_format == "CI8", "sample format is retained");
        Expect(config.sample_encoding == "TWOS_COMPLEMENT",
               "payload uses two's-complement components");
        Expect(config.component_order == "IQ",
               "raw packet components are I then Q");
        Expect(config.packed_order.size() == 3 &&
                   config.packed_order[0] == "T" &&
                   config.packed_order[2] == "P",
               "payload packed order is slowest-to-fastest TFP");
        Expect(config.axes.size() == 4,
               "wire axes and Station-to-A lookup semantics are retained");
        Expect(config.axes[0].extent.source ==
                   rdma_dada::PacketAxisValueSource::kHeader &&
                   config.axes[0].extent.reference == "nsamp_per_packet",
               "T payload extent is supplied by the packet header");
        Expect(config.axes[0].origin.source ==
                   rdma_dada::PacketAxisValueSource::kDerived &&
                   config.axes[0].origin.reference == "vdif_frame_time",
               "T origin is derived from VDIF time fields");
        Expect(config.axes[3].origin.source ==
                   rdma_dada::PacketAxisValueSource::kLookup &&
                   config.axes[3].origin.reference == "antenna_map" &&
                   config.axes[3].origin.input_field == "station_id",
               "Station ID is mapped to the output A index");

        rdma_dada::PacketFormatConfig ci16 = config;
        ci16.sample_format = "CI16";
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(ci16, &error),
               "schema v2 fixes the first wire profile to CI8");

        rdma_dada::PacketFormatConfig overlapping = config;
        rdma_dada::ApplicationHeaderField duplicate =
            overlapping.header_fields[0];
        duplicate.name = "duplicate_station_bits";
        overlapping.header_fields.push_back(duplicate);
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(overlapping, &error),
               "overlapping application-header bit fields are rejected");

        rdma_dada::PacketFormatConfig wrong_format_id = config;
        wrong_format_id.format_id = "other-format";
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(wrong_format_id, &error),
               "schema v1 rejects formats other than Project VDIF v1");

        rdma_dada::PacketFormatConfig missing_field = config;
        missing_field.header_fields.pop_back();
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(missing_field, &error),
               "all 22 header fields are required");

        rdma_dada::PacketFormatConfig reordered_fields = config;
        const rdma_dada::ApplicationHeaderField first_field =
            reordered_fields.header_fields[0];
        reordered_fields.header_fields[0] = reordered_fields.header_fields[1];
        reordered_fields.header_fields[1] = first_field;
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(reordered_fields, &error),
               "header fields must use the canonical v1 order");

        rdma_dada::PacketFormatConfig wrong_field_type = config;
        wrong_field_type.header_fields.back().type =
            rdma_dada::PacketFieldType::kInt32;
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(wrong_field_type, &error),
               "every header field uses UINT32 word storage");

        rdma_dada::PacketFormatConfig wrong_field_endian = config;
        wrong_field_endian.header_fields.back().endianness =
            rdma_dada::PacketEndianness::kBig;
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(wrong_field_endian,
                                                      &error),
               "every header word is little-endian");

        rdma_dada::PacketFormatConfig wrong_semantic = config;
        wrong_semantic.header_fields.back().semantic = "OTHER";
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(wrong_semantic, &error),
               "header field semantics are canonical in Project VDIF v1");

        rdma_dada::PacketFormatConfig wrong_unit = config;
        wrong_unit.header_fields.back().unit = "byte";
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(wrong_unit, &error),
               "header field units are canonical in Project VDIF v1");

        rdma_dada::PacketFormatConfig wrong_t_extent = config;
        wrong_t_extent.axes[0].extent.reference = "nchan";
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(wrong_t_extent, &error),
               "T extent must come from nsamp_per_packet");

        rdma_dada::PacketFormatConfig missing_antenna_lookup = config;
        missing_antenna_lookup.axes[3].origin.source =
            rdma_dada::PacketAxisValueSource::kConstant;
        missing_antenna_lookup.axes[3].origin.constant = 0;
        missing_antenna_lookup.axes[3].origin.reference.clear();
        missing_antenna_lookup.axes[3].origin.input_field.clear();
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(missing_antenna_lookup,
                                                      &error),
               "A origin must map Station ID through antenna_map");

        rdma_dada::PacketFormatConfig big_endian_payload = config;
        big_endian_payload.payload_endianness =
            rdma_dada::PacketEndianness::kBig;
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(big_endian_payload,
                                                      &error),
               "Project VDIF v1 payload is little-endian");

        rdma_dada::PacketFormatConfig duplicate_packed_axis = config;
        duplicate_packed_axis.packed_order[2] = "F";
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(duplicate_packed_axis,
                                                      &error),
               "packed order must contain each TFPA axis exactly once");

        rdma_dada::PacketFormatConfig literal_frequency_extent = config;
        literal_frequency_extent.axes[1].extent.source =
            rdma_dada::PacketAxisValueSource::kConstant;
        literal_frequency_extent.axes[1].extent.constant = 2U;
        literal_frequency_extent.axes[1].extent.reference.clear();
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(
                   literal_frequency_extent, &error),
               "wire profile must not contain an observation F extent");
    }

    if (failures != 0) return 1;
    std::cout << "packet_format_config_test passed\n";
    return 0;
}
