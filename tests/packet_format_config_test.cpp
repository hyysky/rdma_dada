#include "rdma_dada/config/packet_format_config.h"

#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: packet_format_config_test CONFIG\n";
        return 2;
    }

    rdma_dada::PacketFormatConfig config;
    std::string error;
    Expect(rdma_dada::LoadPacketFormatConfig(argv[1], &config, &error),
           "example packet format should load: " + error);
    if (failures == 0) {
        Expect(config.schema_version == 1, "schema version is retained");
        Expect(config.application_header_bytes == 32,
               "Project VDIF v1 header is exactly 32 bytes");
        Expect(config.payload_bytes == 12288,
               "payload byte count is retained");
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
               "all logical TFPA axes are retained");
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
        Expect(rdma_dada::ValidatePacketFormatConfig(ci16, &error),
               "the fixed profile also accepts CI16 payloads: " + error);

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

        rdma_dada::PacketFormatConfig partial_sample = config;
        partial_sample.payload_bytes = 12287;
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(partial_sample, &error),
               "payload bytes must contain complete complex samples");

        rdma_dada::PacketFormatConfig unaligned_frame = config;
        unaligned_frame.payload_bytes = 12286;
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(unaligned_frame, &error),
               "header plus payload must be aligned to eight bytes");

        rdma_dada::PacketFormatConfig frame_overflow = config;
        frame_overflow.payload_bytes =
            std::numeric_limits<std::uint64_t>::max() - 15U;
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(frame_overflow, &error),
               "header plus payload arithmetic must reject overflow");

        rdma_dada::PacketFormatConfig frame_length_overflow = config;
        frame_length_overflow.payload_bytes = UINT64_C(134217696);
        error.clear();
        Expect(!rdma_dada::ValidatePacketFormatConfig(frame_length_overflow,
                                                      &error),
               "frame length must fit the Word 2 24-bit unit field");
    }

    if (failures != 0) return 1;
    std::cout << "packet_format_config_test passed\n";
    return 0;
}
