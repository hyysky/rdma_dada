#include "rdma_dada/config/packet_format_config.h"

#include "rdma_dada/config/json_value.h"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace rdma_dada {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool RequireObject(const json::Value& value, const std::string& path,
                   const json::Value::Object** object, std::string* error) {
    if (value.type() != json::Value::kObject) {
        return Fail(path + " must be a JSON object", error);
    }
    *object = &value.object();
    return true;
}

bool RequireExactKeys(const json::Value::Object& object,
                      const char* const* keys, std::size_t key_count,
                      const std::string& path, std::string* error) {
    for (std::size_t index = 0; index < key_count; ++index) {
        if (object.count(keys[index]) == 0U) {
            return Fail(path + " is missing required field: " + keys[index],
                        error);
        }
    }
    for (json::Value::Object::const_iterator item = object.begin();
         item != object.end(); ++item) {
        bool known = false;
        for (std::size_t index = 0; index < key_count; ++index) {
            if (item->first == keys[index]) {
                known = true;
                break;
            }
        }
        if (!known) {
            return Fail(path + " has unknown field: " + item->first, error);
        }
    }
    return true;
}

const json::Value& Field(const json::Value::Object& object,
                         const std::string& key) {
    return object.find(key)->second;
}

bool ParseUint64(const std::string& path, const json::Value& value,
                 std::uint64_t* output, std::string* error) {
    if (value.type() != json::Value::kNumber ||
        value.text().find_first_of(".eE-") != std::string::npos) {
        return Fail(path + " must use non-negative integer JSON syntax", error);
    }
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed =
        std::strtoull(value.text().c_str(), &end, 10);
    if (errno == ERANGE || end == value.text().c_str() || *end != '\0') {
        return Fail(path + " is not a valid uint64 integer", error);
    }
    *output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ReadUint32(const json::Value::Object& object, const std::string& key,
                const std::string& path, std::uint32_t* output,
                std::string* error) {
    std::uint64_t parsed = 0;
    if (!ParseUint64(path + "." + key, Field(object, key), &parsed, error)) {
        return false;
    }
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        return Fail(path + "." + key + " exceeds uint32 range", error);
    }
    *output = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ReadUint64(const json::Value::Object& object, const std::string& key,
                const std::string& path, std::uint64_t* output,
                std::string* error) {
    return ParseUint64(path + "." + key, Field(object, key), output, error);
}

bool ReadString(const json::Value::Object& object, const std::string& key,
                const std::string& path, std::string* output,
                std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kString || value.text().empty()) {
        return Fail(path + "." + key + " must be a non-empty string", error);
    }
    *output = value.text();
    return true;
}

bool ReadDouble(const json::Value::Object& object, const std::string& key,
                const std::string& path, double* output,
                std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kNumber) {
        return Fail(path + "." + key + " must be a number", error);
    }
    errno = 0;
    char* end = NULL;
    const double parsed = std::strtod(value.text().c_str(), &end);
    if (errno == ERANGE || end == value.text().c_str() || *end != '\0' ||
        !std::isfinite(parsed)) {
        return Fail(path + "." + key + " must be finite", error);
    }
    *output = parsed;
    return true;
}

bool ParseEndianness(const std::string& value, const std::string& path,
                     PacketEndianness* output, std::string* error) {
    if (value == "LITTLE") {
        *output = PacketEndianness::kLittle;
        return true;
    }
    if (value == "BIG") {
        *output = PacketEndianness::kBig;
        return true;
    }
    return Fail(path + " must be LITTLE or BIG", error);
}

bool ParseFieldType(const std::string& value, const std::string& path,
                    PacketFieldType* output, std::string* error) {
    if (value == "UINT8") *output = PacketFieldType::kUint8;
    else if (value == "UINT16") *output = PacketFieldType::kUint16;
    else if (value == "UINT32") *output = PacketFieldType::kUint32;
    else if (value == "UINT64") *output = PacketFieldType::kUint64;
    else if (value == "INT8") *output = PacketFieldType::kInt8;
    else if (value == "INT16") *output = PacketFieldType::kInt16;
    else if (value == "INT32") *output = PacketFieldType::kInt32;
    else if (value == "INT64") *output = PacketFieldType::kInt64;
    else return Fail(path + " has unsupported integer type: " + value, error);
    return true;
}

bool ParseAxisValue(const std::string& expression, bool is_extent,
                    const std::string& path, PacketAxisValue* output,
                    std::string* error) {
    const std::size_t separator = expression.find(':');
    if (separator == std::string::npos || separator == 0U ||
        separator + 1U == expression.size()) {
        return Fail(path + " must use SOURCE:value syntax", error);
    }
    const std::string source = expression.substr(0, separator);
    const std::string value = expression.substr(separator + 1U);
    PacketAxisValue parsed = PacketAxisValue();
    if (source == "CONST") {
        json::Value number = json::Value::Number(value);
        if (!ParseUint64(path, number, &parsed.constant, error)) return false;
        if (is_extent && parsed.constant == 0U) {
            return Fail(path + " constant extent must be greater than zero",
                        error);
        }
        parsed.source = PacketAxisValueSource::kConstant;
    } else if (source == "CONFIG") {
        parsed.source = PacketAxisValueSource::kConfig;
        parsed.reference = value;
    } else if (source == "HEADER") {
        parsed.source = PacketAxisValueSource::kHeader;
        parsed.reference = value;
    } else if (source == "DERIVED" && !is_extent) {
        parsed.source = PacketAxisValueSource::kDerived;
        parsed.reference = value;
    } else if (source == "LOOKUP" && !is_extent) {
        const std::size_t input_separator = value.find(':');
        if (input_separator == std::string::npos || input_separator == 0U ||
            input_separator + 1U == value.size() ||
            value.find(':', input_separator + 1U) != std::string::npos) {
            return Fail(path +
                            " LOOKUP must use LOOKUP:map_name:field_name syntax",
                        error);
        }
        parsed.source = PacketAxisValueSource::kLookup;
        parsed.reference = value.substr(0, input_separator);
        parsed.input_field = value.substr(input_separator + 1U);
    } else {
        return Fail(path + " has unsupported source: " + source, error);
    }
    *output = parsed;
    return true;
}

bool ReadStringArray(const json::Value& value, const std::string& path,
                     std::vector<std::string>* output, std::string* error) {
    if (value.type() != json::Value::kArray || value.array().empty()) {
        return Fail(path + " must be a non-empty JSON array", error);
    }
    std::vector<std::string> parsed;
    for (std::size_t index = 0; index < value.array().size(); ++index) {
        const json::Value& item = value.array()[index];
        if (item.type() != json::Value::kString || item.text().empty()) {
            std::ostringstream message;
            message << path << '[' << index << "] must be a non-empty string";
            return Fail(message.str(), error);
        }
        parsed.push_back(item.text());
    }
    *output = parsed;
    return true;
}

std::uint32_t FieldStorageBits(PacketFieldType type) {
    switch (type) {
        case PacketFieldType::kUint8:
        case PacketFieldType::kInt8: return 8;
        case PacketFieldType::kUint16:
        case PacketFieldType::kInt16: return 16;
        case PacketFieldType::kUint32:
        case PacketFieldType::kInt32: return 32;
        case PacketFieldType::kUint64:
        case PacketFieldType::kInt64: return 64;
    }
    return 0;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     const std::string& name, std::uint64_t* output,
                     std::string* error) {
    if (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Fail(name + " exceeds uint64 range", error);
    }
    *output = left * right;
    return true;
}

bool IsReferenceName(const std::string& value, bool allow_dots) {
    if (value.empty()) return false;
    bool segment_start = true;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char current =
            static_cast<unsigned char>(value[index]);
        if (allow_dots && current == '.') {
            if (segment_start) return false;
            segment_start = true;
            continue;
        }
        if (segment_start) {
            if (current != '_' && !std::isalpha(current)) return false;
            segment_start = false;
        } else if (current != '_' && !std::isalnum(current)) {
            return false;
        }
    }
    return !segment_start;
}

struct ExpectedHeaderField {
    const char* name;
    const char* semantic;
    std::uint32_t offset_bytes;
    std::uint32_t bit_offset;
    std::uint32_t bit_width;
    double scale;
    const char* unit;
};

const ExpectedHeaderField kProjectVdifHeaderFields[] = {
    {"invalid_data", "INVALID_DATA", 0, 31, 1, 1.0, "1"},
    {"legacy_mode", "LEGACY_MODE", 0, 30, 1, 1.0, "1"},
    {"seconds_from_reference_epoch", "SECONDS_FROM_REFERENCE_EPOCH",
     0, 0, 30, 1.0, "s"},
    {"word1_reserved", "RESERVED", 4, 30, 2, 1.0, "1"},
    {"reference_epoch", "REFERENCE_EPOCH", 4, 24, 6, 1.0, "half_year"},
    {"frame_number_within_second", "FRAME_NUMBER_WITHIN_SECOND",
     4, 0, 24, 1.0, "frame"},
    {"vdif_version", "VDIF_VERSION", 8, 29, 3, 1.0, "1"},
    {"channel_count_code", "PROJECT_CHANNEL_COUNT_SENTINEL",
     8, 24, 5, 1.0, "1"},
    {"frame_length_units_8_bytes", "FRAME_LENGTH_UNITS_8_BYTES",
     8, 0, 24, 8.0, "byte"},
    {"data_type", "DATA_TYPE", 12, 31, 1, 1.0, "1"},
    {"component_bits_minus_one", "COMPONENT_BITS_MINUS_ONE",
     12, 26, 5, 1.0, "bit"},
    {"thread_id", "THREAD_ID", 12, 16, 10, 1.0, "thread"},
    {"station_id", "STATION_ID", 12, 0, 16, 1.0, "station"},
    {"edv", "EXTENDED_DATA_VERSION", 16, 24, 8, 1.0, "1"},
    {"profile_version", "PROJECT_PROFILE_VERSION", 16, 16, 8, 1.0, "1"},
    {"sample_encoding", "SAMPLE_ENCODING_CODE", 16, 8, 8, 1.0, "1"},
    {"flags", "PROJECT_FLAGS", 16, 0, 8, 1.0, "1"},
    {"first_channel_id", "FIRST_CHANNEL_ID", 20, 16, 16, 1.0, "channel"},
    {"nchan", "NCHAN", 20, 8, 8, 1.0, "channel"},
    {"npol", "NPOL", 20, 0, 8, 1.0, "polarization"},
    {"nsamp_per_packet", "NSAMP_PER_PACKET", 24, 0, 32, 1.0, "sample"},
    {"word7_reserved", "RESERVED", 28, 0, 32, 1.0, "1"}
};

bool AxisValueEquals(const PacketAxisValue& value,
                     PacketAxisValueSource source,
                     std::uint64_t constant,
                     const char* reference,
                     const char* input_field) {
    return value.source == source && value.constant == constant &&
           value.reference == reference && value.input_field == input_field;
}

}  // namespace

bool IsSignedPacketFieldType(PacketFieldType type) {
    switch (type) {
        case PacketFieldType::kInt8:
        case PacketFieldType::kInt16:
        case PacketFieldType::kInt32:
        case PacketFieldType::kInt64: return true;
        case PacketFieldType::kUint8:
        case PacketFieldType::kUint16:
        case PacketFieldType::kUint32:
        case PacketFieldType::kUint64: return false;
    }
    return false;
}

bool ValidatePacketFormatConfig(const PacketFormatConfig& config,
                                std::string* error) {
    if (config.schema_version != 1U) {
        return Fail("unsupported packet format schema_version", error);
    }
    if (config.format_id != "project-vdif-v1") {
        return Fail("format_id must be project-vdif-v1", error);
    }
    if (config.application_header_bytes != 32U) {
        return Fail("record.application_header_bytes must be 32", error);
    }
    if (config.payload_bytes == 0U) {
        return Fail("record.payload_bytes must be greater than zero", error);
    }
    if (config.bit_numbering != "LSB0") {
        return Fail("application_header.bit_numbering must be LSB0", error);
    }
    if (config.header_fields.empty()) {
        return Fail("application_header.fields must not be empty", error);
    }
    if (config.sample_format != "CI8" && config.sample_format != "CI16") {
        return Fail("payload.sample_format must be CI8 or CI16", error);
    }
    if (config.sample_encoding != "TWOS_COMPLEMENT") {
        return Fail("payload.sample_encoding must be TWOS_COMPLEMENT", error);
    }
    if (config.component_order != "IQ") {
        return Fail("payload.component_order must be IQ", error);
    }
    if (config.payload_endianness != PacketEndianness::kLittle) {
        return Fail("payload.endian must be LITTLE", error);
    }
    std::set<std::string> field_names;
    std::vector<bool> occupied_header_bits(
        static_cast<std::size_t>(config.application_header_bytes * 8U), false);
    for (std::size_t index = 0; index < config.header_fields.size(); ++index) {
        const ApplicationHeaderField& field = config.header_fields[index];
        if (!IsReferenceName(field.name, false) ||
            !field_names.insert(field.name).second) {
            return Fail("application-header field names must be non-empty and unique",
                        error);
        }
        const std::uint32_t storage_bits = FieldStorageBits(field.type);
        if (field.bit_width == 0U || field.bit_offset >= storage_bits ||
            field.bit_width > storage_bits - field.bit_offset) {
            return Fail("application-header field bit range exceeds its type",
                        error);
        }
        const std::uint64_t storage_bytes = storage_bits / 8U;
        if (field.offset_bytes > config.application_header_bytes ||
            storage_bytes > config.application_header_bytes - field.offset_bytes) {
            return Fail("application-header field exceeds the 32-byte header",
                        error);
        }
        for (std::uint32_t bit = 0; bit < field.bit_width; ++bit) {
            const std::uint32_t integer_bit = field.bit_offset + bit;
            const std::uint32_t integer_byte = integer_bit / 8U;
            const std::uint32_t wire_byte_within_storage =
                field.endianness == PacketEndianness::kLittle ?
                    integer_byte :
                    static_cast<std::uint32_t>(storage_bytes - 1U -
                                               integer_byte);
            const std::size_t position =
                static_cast<std::size_t>(
                    (static_cast<std::uint64_t>(field.offset_bytes) +
                     wire_byte_within_storage) * 8U + integer_bit % 8U);
            if (occupied_header_bits[position]) {
                return Fail("application-header fields overlap at bit " +
                                std::to_string(position),
                            error);
            }
            occupied_header_bits[position] = true;
        }
        if (!std::isfinite(field.scale) || field.scale == 0.0) {
            return Fail("application-header field scale must be finite and non-zero",
                        error);
        }
        if (field.semantic.empty() || field.unit.empty()) {
            return Fail("application-header field semantic and unit must not be empty",
                        error);
        }
    }

    const std::size_t expected_field_count =
        sizeof(kProjectVdifHeaderFields) /
        sizeof(kProjectVdifHeaderFields[0]);
    if (config.header_fields.size() != expected_field_count) {
        return Fail("Project VDIF v1 must declare exactly 22 header fields",
                    error);
    }
    for (std::size_t index = 0; index < expected_field_count; ++index) {
        const ApplicationHeaderField& actual = config.header_fields[index];
        const ExpectedHeaderField& expected =
            kProjectVdifHeaderFields[index];
        if (actual.name != expected.name ||
            actual.semantic != expected.semantic ||
            actual.offset_bytes != expected.offset_bytes ||
            actual.type != PacketFieldType::kUint32 ||
            actual.endianness != PacketEndianness::kLittle ||
            actual.bit_offset != expected.bit_offset ||
            actual.bit_width != expected.bit_width ||
            actual.scale != expected.scale || actual.unit != expected.unit) {
            return Fail("Project VDIF v1 header field mismatch at index " +
                            std::to_string(index),
                        error);
        }
    }

    static const char* const expected_axes[] = {"T", "F", "P", "A"};
    if (config.packed_order.size() != 3U ||
        config.output_order.size() != 4U || config.axes.size() != 4U) {
        return Fail("payload must define packed TFP and output TFPA axes", error);
    }
    static const char* const expected_packed_axes[] = {"T", "F", "P"};
    for (std::size_t index = 0; index < 3U; ++index) {
        if (config.packed_order[index] != expected_packed_axes[index]) {
            return Fail("payload.packed_order must be TFP", error);
        }
    }
    std::set<std::string> described_axes;
    bool all_extents_are_constant = true;
    std::uint64_t constant_sample_count = 1U;
    for (std::size_t index = 0; index < 4U; ++index) {
        if (config.output_order[index] != expected_axes[index]) {
            return Fail("payload.output_order must be TFPA", error);
        }
        const PacketPayloadAxis& axis = config.axes[index];
        if (!described_axes.insert(axis.name).second) {
            return Fail("payload axis names must be unique", error);
        }
        if (axis.extent.source == PacketAxisValueSource::kConstant) {
            if (axis.extent.constant == 0U) {
                return Fail("constant payload extent must be greater than zero",
                            error);
            }
            if (!CheckedMultiply(constant_sample_count, axis.extent.constant,
                                 "constant payload sample count",
                                 &constant_sample_count, error)) {
                return false;
            }
        } else if (axis.extent.source == PacketAxisValueSource::kConfig) {
            all_extents_are_constant = false;
            if (!IsReferenceName(axis.extent.reference, true)) {
                return Fail("CONFIG payload extent reference is invalid",
                            error);
            }
        } else if (axis.extent.source == PacketAxisValueSource::kHeader) {
            all_extents_are_constant = false;
            if (!IsReferenceName(axis.extent.reference, false) ||
                field_names.count(axis.extent.reference) == 0U) {
                return Fail(
                    "payload axis extent references unknown header field: " +
                        axis.extent.reference,
                    error);
            }
        } else {
            return Fail("payload extent source must be CONST, CONFIG or HEADER",
                        error);
        }
        if (axis.origin.source == PacketAxisValueSource::kHeader) {
            if (!IsReferenceName(axis.origin.reference, false) ||
                field_names.count(axis.origin.reference) == 0U) {
                return Fail(
                    "payload axis origin references unknown header field: " +
                        axis.origin.reference,
                    error);
            }
        } else if (axis.origin.source == PacketAxisValueSource::kConfig) {
            if (!IsReferenceName(axis.origin.reference, true)) {
                return Fail("CONFIG payload origin reference is invalid", error);
            }
        } else if (axis.origin.source == PacketAxisValueSource::kDerived) {
            if (!IsReferenceName(axis.origin.reference, false)) {
                return Fail("DERIVED payload origin reference is invalid", error);
            }
        } else if (axis.origin.source == PacketAxisValueSource::kLookup) {
            if (!IsReferenceName(axis.origin.reference, true) ||
                !IsReferenceName(axis.origin.input_field, false) ||
                field_names.count(axis.origin.input_field) == 0U) {
                return Fail(
                    "LOOKUP payload origin must name a valid map and header field",
                    error);
            }
        } else if (axis.origin.source != PacketAxisValueSource::kConstant) {
            return Fail(
                "payload origin source must be CONST, CONFIG, HEADER, DERIVED or LOOKUP",
                error);
        }
    }
    for (std::size_t index = 0; index < 4U; ++index) {
        if (described_axes.count(expected_axes[index]) == 0U) {
            return Fail("payload.axes must describe T, F, P and A", error);
        }
    }

    if (config.axes[0].name != "T" ||
        !AxisValueEquals(config.axes[0].extent,
                         PacketAxisValueSource::kHeader, 0,
                         "nsamp_per_packet", "") ||
        !AxisValueEquals(config.axes[0].origin,
                         PacketAxisValueSource::kDerived, 0,
                         "vdif_frame_time", "") ||
        config.axes[1].name != "F" ||
        !AxisValueEquals(config.axes[1].extent,
                         PacketAxisValueSource::kHeader, 0, "nchan", "") ||
        !AxisValueEquals(config.axes[1].origin,
                         PacketAxisValueSource::kHeader, 0,
                         "first_channel_id", "") ||
        config.axes[2].name != "P" ||
        !AxisValueEquals(config.axes[2].extent,
                         PacketAxisValueSource::kHeader, 0, "npol", "") ||
        !AxisValueEquals(config.axes[2].origin,
                         PacketAxisValueSource::kConstant, 0, "", "") ||
        config.axes[3].name != "A" ||
        !AxisValueEquals(config.axes[3].extent,
                         PacketAxisValueSource::kConstant, 1, "", "") ||
        !AxisValueEquals(config.axes[3].origin,
                         PacketAxisValueSource::kLookup, 0, "antenna_map",
                         "station_id")) {
        return Fail("payload.axes must use the Project VDIF v1 TFP-to-TFPA mapping",
                    error);
    }

    const std::uint64_t sample_bytes = config.sample_format == "CI8" ? 2U : 4U;
    if (config.payload_bytes % sample_bytes != 0U) {
        return Fail("record.payload_bytes must contain complete complex samples",
                    error);
    }
    if (config.payload_bytes >
        std::numeric_limits<std::uint64_t>::max() -
            config.application_header_bytes) {
        return Fail("Project VDIF frame byte count exceeds uint64 range", error);
    }
    const std::uint64_t frame_bytes =
        config.application_header_bytes + config.payload_bytes;
    if (frame_bytes % 8U != 0U) {
        return Fail("Project VDIF frame byte count must be divisible by 8",
                    error);
    }
    if (frame_bytes / 8U > UINT64_C(0xffffff)) {
        return Fail(
            "Project VDIF frame length exceeds the Word 2 24-bit unit field",
            error);
    }
    if (all_extents_are_constant) {
        std::uint64_t described_payload_bytes = 0;
        if (!CheckedMultiply(constant_sample_count, sample_bytes,
                             "constant payload byte count",
                             &described_payload_bytes, error)) {
            return false;
        }
        if (described_payload_bytes != config.payload_bytes) {
            return Fail(
                "constant payload axis geometry does not match record.payload_bytes",
                error);
        }
    }
    return true;
}

bool LoadPacketFormatConfig(const std::string& path,
                            PacketFormatConfig* config,
                            std::string* error) {
    if (!config) return Fail("packet format output pointer is null", error);
    std::ifstream input(path.c_str());
    if (!input) return Fail("cannot open packet format file: " + path, error);
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        return Fail("failed while reading packet format file: " + path, error);
    }

    json::Value root;
    if (!json::Parse(contents.str(), &root, error)) return false;
    const json::Value::Object* root_object = NULL;
    if (!RequireObject(root, "root", &root_object, error)) return false;
    static const char* const root_keys[] = {
        "schema_version", "format_id", "record", "application_header", "payload"
    };
    if (!RequireExactKeys(*root_object, root_keys,
                          sizeof(root_keys) / sizeof(root_keys[0]),
                          "root", error)) return false;

    PacketFormatConfig parsed = PacketFormatConfig();
    if (!ReadUint32(*root_object, "schema_version", "root",
                    &parsed.schema_version, error) ||
        !ReadString(*root_object, "format_id", "root", &parsed.format_id,
                    error)) {
        return false;
    }

    const json::Value::Object* record = NULL;
    const json::Value::Object* application_header = NULL;
    const json::Value::Object* payload = NULL;
    if (!RequireObject(Field(*root_object, "record"), "record", &record, error) ||
        !RequireObject(Field(*root_object, "application_header"),
                       "application_header", &application_header, error) ||
        !RequireObject(Field(*root_object, "payload"), "payload", &payload,
                       error)) {
        return false;
    }
    static const char* const record_keys[] = {
        "application_header_bytes", "payload_bytes"
    };
    static const char* const header_keys[] = {"bit_numbering", "fields"};
    static const char* const payload_keys[] = {
        "sample_format", "sample_encoding", "component_order", "endian",
        "packed_order", "output_order", "axes"
    };
    if (!RequireExactKeys(*record, record_keys,
                          sizeof(record_keys) / sizeof(record_keys[0]),
                          "record", error) ||
        !RequireExactKeys(*application_header, header_keys,
                          sizeof(header_keys) / sizeof(header_keys[0]),
                          "application_header", error) ||
        !RequireExactKeys(*payload, payload_keys,
                          sizeof(payload_keys) / sizeof(payload_keys[0]),
                          "payload", error) ||
        !ReadUint64(*record, "application_header_bytes", "record",
                    &parsed.application_header_bytes, error) ||
        !ReadUint64(*record, "payload_bytes", "record", &parsed.payload_bytes,
                    error) ||
        !ReadString(*application_header, "bit_numbering",
                    "application_header", &parsed.bit_numbering, error) ||
        !ReadString(*payload, "sample_format", "payload",
                    &parsed.sample_format, error) ||
        !ReadString(*payload, "sample_encoding", "payload",
                    &parsed.sample_encoding, error) ||
        !ReadString(*payload, "component_order", "payload",
                    &parsed.component_order, error)) {
        return false;
    }
    std::string payload_endian;
    if (!ReadString(*payload, "endian", "payload", &payload_endian, error) ||
        !ParseEndianness(payload_endian, "payload.endian",
                         &parsed.payload_endianness, error) ||
        !ReadStringArray(Field(*payload, "packed_order"),
                         "payload.packed_order", &parsed.packed_order, error) ||
        !ReadStringArray(Field(*payload, "output_order"),
                         "payload.output_order", &parsed.output_order, error)) {
        return false;
    }

    const json::Value& fields_value = Field(*application_header, "fields");
    if (fields_value.type() != json::Value::kArray ||
        fields_value.array().empty()) {
        return Fail("application_header.fields must be a non-empty JSON array",
                    error);
    }
    static const char* const field_keys[] = {
        "name", "semantic", "offset_bytes", "type", "endian", "bit_offset",
        "bit_width", "scale", "unit"
    };
    for (std::size_t index = 0; index < fields_value.array().size(); ++index) {
        std::ostringstream path_stream;
        path_stream << "application_header.fields[" << index << ']';
        const std::string path_text = path_stream.str();
        const json::Value::Object* field_object = NULL;
        if (!RequireObject(fields_value.array()[index], path_text, &field_object,
                           error) ||
            !RequireExactKeys(*field_object, field_keys,
                              sizeof(field_keys) / sizeof(field_keys[0]),
                              path_text, error)) {
            return false;
        }
        ApplicationHeaderField field = ApplicationHeaderField();
        std::string field_type;
        std::string field_endian;
        if (!ReadString(*field_object, "name", path_text, &field.name, error) ||
            !ReadString(*field_object, "semantic", path_text, &field.semantic,
                        error) ||
            !ReadUint32(*field_object, "offset_bytes", path_text,
                        &field.offset_bytes, error) ||
            !ReadString(*field_object, "type", path_text, &field_type, error) ||
            !ParseFieldType(field_type, path_text + ".type", &field.type,
                            error) ||
            !ReadString(*field_object, "endian", path_text, &field_endian,
                        error) ||
            !ParseEndianness(field_endian, path_text + ".endian",
                             &field.endianness, error) ||
            !ReadUint32(*field_object, "bit_offset", path_text,
                        &field.bit_offset, error) ||
            !ReadUint32(*field_object, "bit_width", path_text,
                        &field.bit_width, error) ||
            !ReadDouble(*field_object, "scale", path_text, &field.scale,
                        error) ||
            !ReadString(*field_object, "unit", path_text, &field.unit, error)) {
            return false;
        }
        parsed.header_fields.push_back(field);
    }

    const json::Value& axes_value = Field(*payload, "axes");
    if (axes_value.type() != json::Value::kArray || axes_value.array().empty()) {
        return Fail("payload.axes must be a non-empty JSON array", error);
    }
    static const char* const axis_keys[] = {"name", "extent", "origin"};
    for (std::size_t index = 0; index < axes_value.array().size(); ++index) {
        std::ostringstream path_stream;
        path_stream << "payload.axes[" << index << ']';
        const std::string path_text = path_stream.str();
        const json::Value::Object* axis_object = NULL;
        if (!RequireObject(axes_value.array()[index], path_text, &axis_object,
                           error) ||
            !RequireExactKeys(*axis_object, axis_keys,
                              sizeof(axis_keys) / sizeof(axis_keys[0]),
                              path_text, error)) {
            return false;
        }
        PacketPayloadAxis axis = PacketPayloadAxis();
        std::string extent;
        std::string origin;
        if (!ReadString(*axis_object, "name", path_text, &axis.name, error) ||
            !ReadString(*axis_object, "extent", path_text, &extent, error) ||
            !ReadString(*axis_object, "origin", path_text, &origin, error) ||
            !ParseAxisValue(extent, true, path_text + ".extent", &axis.extent,
                            error) ||
            !ParseAxisValue(origin, false, path_text + ".origin", &axis.origin,
                            error)) {
            return false;
        }
        parsed.axes.push_back(axis);
    }

    if (!ValidatePacketFormatConfig(parsed, error)) return false;
    *config = parsed;
    return true;
}

}  // namespace rdma_dada
