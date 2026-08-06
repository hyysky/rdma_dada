#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rdma_dada {

enum class PacketEndianness {
    kLittle,
    kBig
};

enum class PacketFieldType {
    kUint8,
    kUint16,
    kUint32,
    kUint64,
    kInt8,
    kInt16,
    kInt32,
    kInt64
};

bool IsSignedPacketFieldType(PacketFieldType type);

enum class PacketAxisValueSource {
    kConstant,
    kConfig,
    kHeader,
    kDerived,
    kLookup
};

struct PacketAxisValue {
    PacketAxisValueSource source;
    std::uint64_t constant;
    std::string reference;
    std::string input_field;
};

struct ApplicationHeaderField {
    std::string name;
    std::string semantic;
    std::uint32_t offset_bytes;
    PacketFieldType type;
    PacketEndianness endianness;
    std::uint32_t bit_offset;
    std::uint32_t bit_width;
    double scale;
    std::string unit;
};

struct PacketPayloadAxis {
    std::string name;
    PacketAxisValue extent;
    PacketAxisValue origin;
};

struct PacketFormatConfig {
    std::uint32_t schema_version;
    std::string format_id;
    std::uint64_t application_header_bytes;
    std::uint64_t payload_bytes;
    std::string bit_numbering;
    std::vector<ApplicationHeaderField> header_fields;
    std::string sample_format;
    std::string sample_encoding;
    std::string component_order;
    PacketEndianness payload_endianness;
    std::vector<std::string> packed_order;
    std::vector<std::string> output_order;
    std::vector<PacketPayloadAxis> axes;
};

// Load a strict, standalone packet-format profile and validate its complete
// application-header and payload-layout contract.
bool LoadPacketFormatConfig(const std::string& path,
                            PacketFormatConfig* config,
                            std::string* error);

// Validate a profile constructed by code. LoadPacketFormatConfig calls this
// before publishing its result.
bool ValidatePacketFormatConfig(const PacketFormatConfig& config,
                                std::string* error);

}  // namespace rdma_dada
