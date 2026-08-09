#include "rdma_dada/config/observation_config.h"

#include "rdma_dada/config/json_value.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
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
                   const json::Value::Object** output, std::string* error) {
    if (value.type() != json::Value::kObject) {
        return Fail(path + " must be an object", error);
    }
    *output = &value.object();
    return true;
}

bool RequireExactKeys(const json::Value::Object& object,
                      const char* const* keys, std::size_t key_count,
                      const std::string& path, std::string* error) {
    for (std::size_t index = 0; index < key_count; ++index) {
        if (object.count(keys[index]) == 0U) {
            return Fail(path + " is missing required field: " + keys[index], error);
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
        if (!known) return Fail(path + " has unknown field: " + item->first, error);
    }
    return true;
}

const json::Value& Field(const json::Value::Object& object,
                         const std::string& key) {
    return object.find(key)->second;
}

bool ParseUint64Text(const std::string& path, const std::string& text,
                     std::uint64_t* output, std::string* error) {
    if (text.empty() || text[0] == '-' ||
        text.find_first_of(".eE") != std::string::npos) {
        return Fail(path + " must be a non-negative integer", error);
    }
    errno = 0;
    char* end = NULL;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
        return Fail(path + " is outside uint64 range", error);
    }
    *output = static_cast<std::uint64_t>(value);
    return true;
}

bool ReadUint64(const json::Value::Object& object, const char* key,
                const std::string& path, std::uint64_t* output,
                std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kNumber) {
        return Fail(path + "." + key + " must be an integer", error);
    }
    return ParseUint64Text(path + "." + key, value.text(), output, error);
}

bool ReadUint32(const json::Value::Object& object, const char* key,
                const std::string& path, std::uint32_t* output,
                std::string* error) {
    std::uint64_t value = 0;
    if (!ReadUint64(object, key, path, &value, error)) return false;
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        return Fail(path + "." + key + " exceeds uint32 range", error);
    }
    *output = static_cast<std::uint32_t>(value);
    return true;
}

bool ReadInt(const json::Value::Object& object, const char* key,
             const std::string& path, int* output, std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kNumber ||
        value.text().find_first_of(".eE") != std::string::npos) {
        return Fail(path + "." + key + " must be an integer", error);
    }
    errno = 0;
    char* end = NULL;
    const long parsed = std::strtol(value.text().c_str(), &end, 10);
    if (errno == ERANGE || end == value.text().c_str() || *end != '\0' ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) {
        return Fail(path + "." + key + " is outside int range", error);
    }
    *output = static_cast<int>(parsed);
    return true;
}

bool ReadString(const json::Value::Object& object, const char* key,
                const std::string& path, std::string* output,
                std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kString || value.text().empty()) {
        return Fail(path + "." + key + " must be a non-empty string", error);
    }
    *output = value.text();
    return true;
}

bool ReadBool(const json::Value::Object& object, const char* key,
              const std::string& path, bool* output, std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kBoolean) {
        return Fail(path + "." + key + " must be true or false", error);
    }
    *output = value.boolean();
    return true;
}

std::string ResolveRelative(const std::string& source_path,
                            const std::string& referenced_path) {
    if (referenced_path.empty() || referenced_path[0] == '/') {
        return referenced_path;
    }
    const std::string::size_type separator = source_path.find_last_of('/');
    if (separator == std::string::npos) return referenced_path;
    return source_path.substr(0, separator + 1U) + referenced_path;
}

bool ParseRingKey(const std::string& path, const std::string& text,
                  std::uint32_t* output, std::string* error) {
    if (text.size() < 3U || text[0] != '0' ||
        (text[1] != 'x' && text[1] != 'X')) {
        return Fail(path + " must use 0x-prefixed hexadecimal syntax", error);
    }
    errno = 0;
    char* end = NULL;
    const unsigned long value = std::strtoul(text.c_str() + 2, &end, 16);
    if (errno == ERANGE || end == text.c_str() + 2 || *end != '\0' ||
        value > std::numeric_limits<std::uint32_t>::max()) {
        return Fail(path + " is not a valid uint32 ring key", error);
    }
    *output = static_cast<std::uint32_t>(value);
    return true;
}

bool IsHex(char value) {
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

bool IsMacAddress(const std::string& text) {
    if (text.size() != 17U) return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index % 3U == 2U) {
            if (text[index] != ':') return false;
        } else if (!IsHex(text[index])) {
            return false;
        }
    }
    return true;
}

bool IsIpv4Address(const std::string& text) {
    std::size_t begin = 0;
    for (int part = 0; part < 4; ++part) {
        const std::size_t end = text.find('.', begin);
        if ((part < 3 && end == std::string::npos) ||
            (part == 3 && end != std::string::npos)) {
            return false;
        }
        const std::size_t limit = part == 3 ? text.size() : end;
        if (limit == begin || limit - begin > 3U) return false;
        unsigned int value = 0;
        for (std::size_t index = begin; index < limit; ++index) {
            if (text[index] < '0' || text[index] > '9') return false;
            value = value * 10U + static_cast<unsigned int>(text[index] - '0');
        }
        if (value > 255U) return false;
        begin = limit + 1U;
    }
    return true;
}

bool ParseUtcDateTimeValue(const std::string& text, UtcDateTime* value) {
    if (!value) return false;
    if (text.size() != 19U || text[4] != '-' || text[7] != '-' ||
        text[10] != '-' || text[13] != ':' || text[16] != ':') {
        return false;
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (index == 4U || index == 7U || index == 10U || index == 13U ||
            index == 16U) {
            continue;
        }
        if (text[index] < '0' || text[index] > '9') return false;
    }
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    int consumed = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d-%d:%d:%d%n", &year, &month,
                    &day, &hour, &minute, &second, &consumed) != 6 ||
        consumed != static_cast<int>(text.size())) {
        return false;
    }
    if (year < 2000 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 60) {
        return false;
    }
    static const int days_per_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    int days = days_per_month[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 2 && leap) ++days;
    if (day < 1 || day > days) return false;
    value->year = year;
    value->month = month;
    value->day = day;
    value->hour = hour;
    value->minute = minute;
    value->second = second;
    return true;
}

bool PositiveDecimal(const std::string& text) {
    if (text.empty()) return false;
    const std::string::size_type decimal = text.find('.');
    if (decimal != std::string::npos &&
        (decimal == 0U || decimal + 1U == text.size() ||
         text.find('.', decimal + 1U) != std::string::npos)) {
        return false;
    }
    const std::size_t whole_size = decimal == std::string::npos
        ? text.size() : decimal;
    if (whole_size > 1U && text[0] == '0') return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '.' && (text[index] < '0' || text[index] > '9')) {
            return false;
        }
    }
    errno = 0;
    char* end = NULL;
    const double value = std::strtod(text.c_str(), &end);
    return errno != ERANGE && end != text.c_str() && *end == '\0' &&
           std::isfinite(value) && value > 0.0;
}

bool ParseModule(const json::Value& value, const std::string& source_path,
                 std::size_t index, ObservationModuleConfig* module,
                 std::string* error) {
    std::ostringstream path_builder;
    path_builder << "processing.modules[" << index << "]";
    const std::string path = path_builder.str();
    const json::Value::Object* object = NULL;
    if (!RequireObject(value, path, &object, error)) return false;
    if (object->count("type") == 0U ||
        Field(*object, "type").type() != json::Value::kString) {
        return Fail(path + ".type must be a string", error);
    }
    const std::string type = Field(*object, "type").text();
    ObservationModuleConfig parsed = ObservationModuleConfig();
    if (type == "beamform") {
        static const char* const keys[] = {
            "type", "weights_file", "weights_order", "weights_id",
            "weights_scale", "compute_mode"
        };
        if (!RequireExactKeys(*object, keys, sizeof(keys) / sizeof(keys[0]),
                              path, error) ||
            !ReadString(*object, "weights_file", path, &parsed.weights_file, error) ||
            !ReadString(*object, "weights_order", path, &parsed.weights_order, error) ||
            !ReadString(*object, "weights_id", path, &parsed.weights_id, error) ||
            !ReadString(*object, "weights_scale", path, &parsed.weights_scale, error) ||
            !ReadString(*object, "compute_mode", path, &parsed.compute_mode, error)) {
            return false;
        }
        if (parsed.weights_order != "FPAB2") {
            return Fail(path + ".weights_order must be FPAB2", error);
        }
        if (!PositiveDecimal(parsed.weights_scale)) {
            return Fail(path + ".weights_scale must be positive and finite", error);
        }
        if (parsed.compute_mode != "FP32" && parsed.compute_mode != "TF32") {
            return Fail(path + ".compute_mode must be FP32 or TF32", error);
        }
        parsed.weights_file = ResolveRelative(source_path, parsed.weights_file);
        parsed.kind = ObservationModuleKind::kBeamform;
    } else if (type == "power" || type == "stokes") {
        static const char* const keys[] = {"type"};
        if (!RequireExactKeys(*object, keys, 1U, path, error)) return false;
        parsed.kind = type == "power" ? ObservationModuleKind::kPower
                                      : ObservationModuleKind::kStokes;
    } else if (type == "integrate") {
        static const char* const keys[] = {"type", "length", "operation"};
        if (!RequireExactKeys(*object, keys, 3U, path, error) ||
            !ReadUint64(*object, "length", path,
                        &parsed.integration_length, error) ||
            !ReadString(*object, "operation", path,
                        &parsed.integration_operation, error)) {
            return false;
        }
        if (parsed.integration_length == 0U) {
            return Fail(path + ".length must be positive", error);
        }
        if (parsed.integration_operation != "SUM" &&
            parsed.integration_operation != "MEAN") {
            return Fail(path + ".operation must be SUM or MEAN", error);
        }
        parsed.kind = ObservationModuleKind::kIntegrate;
    } else {
        return Fail(path + ".type is unsupported: " + type, error);
    }
    *module = parsed;
    return true;
}

bool ValidateModuleOrder(const std::vector<ObservationModuleConfig>& modules,
                         std::uint32_t npol, std::string* error) {
    if (modules.empty()) return true;
    if (modules[0].kind != ObservationModuleKind::kBeamform) {
        return Fail("processing.modules must start with beamform", error);
    }
    if (modules.size() == 1U) return true;
    if (modules[1].kind != ObservationModuleKind::kPower &&
        modules[1].kind != ObservationModuleKind::kStokes) {
        return Fail("beamform may be followed only by power or stokes", error);
    }
    if (modules[1].kind == ObservationModuleKind::kStokes && npol != 2U) {
        return Fail("stokes requires observation.npol=2", error);
    }
    if (modules.size() == 2U) return true;
    if (modules.size() == 3U &&
        modules[2].kind == ObservationModuleKind::kIntegrate) {
        return true;
    }
    return Fail("integration may appear only once after power or stokes", error);
}

}  // namespace

bool ParseUtcDateTime(const std::string& text,
                      UtcDateTime* value,
                      std::string* error) {
    if (!ParseUtcDateTimeValue(text, value)) {
        return Fail("UTC time must be a valid YYYY-MM-DD-HH:MM:SS value", error);
    }
    return true;
}

bool ParseExactSecondsToPicoseconds(const std::string& text,
                                    std::uint64_t* picoseconds,
                                    std::string* error) {
    if (!picoseconds) return Fail("picoseconds output pointer is null", error);
    if (text.empty() || text[0] == '-' || text.find_first_of("eE") != std::string::npos) {
        return Fail("duration_seconds must be a positive decimal string", error);
    }
    const std::string::size_type decimal = text.find('.');
    if (decimal != std::string::npos && text.find('.', decimal + 1U) != std::string::npos) {
        return Fail("duration_seconds contains multiple decimal points", error);
    }
    const std::string whole_text = decimal == std::string::npos
        ? text : text.substr(0, decimal);
    const std::string fraction_text = decimal == std::string::npos
        ? std::string() : text.substr(decimal + 1U);
    if (whole_text.empty() || (decimal != std::string::npos && fraction_text.empty()) ||
        fraction_text.size() > 12U) {
        return Fail("duration_seconds must have 1 to 12 fractional digits", error);
    }
    if (whole_text.size() > 1U && whole_text[0] == '0') {
        return Fail("duration_seconds must not contain leading zeroes", error);
    }
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '.' && (text[index] < '0' || text[index] > '9')) {
            return Fail("duration_seconds contains a non-decimal character", error);
        }
    }
    std::uint64_t whole = 0;
    if (!ParseUint64Text("duration_seconds", whole_text, &whole, error)) return false;
    const std::uint64_t scale = UINT64_C(1000000000000);
    if (whole > std::numeric_limits<std::uint64_t>::max() / scale) {
        return Fail("duration_seconds exceeds uint64 picoseconds", error);
    }
    std::uint64_t fraction = 0;
    if (!fraction_text.empty() &&
        !ParseUint64Text("duration_seconds fraction", fraction_text,
                         &fraction, error)) {
        return false;
    }
    for (std::size_t index = fraction_text.size(); index < 12U; ++index) {
        fraction *= 10U;
    }
    const std::uint64_t base = whole * scale;
    if (fraction > std::numeric_limits<std::uint64_t>::max() - base) {
        return Fail("duration_seconds exceeds uint64 picoseconds", error);
    }
    const std::uint64_t result = base + fraction;
    if (result == 0U) return Fail("duration_seconds must be positive", error);
    *picoseconds = result;
    return true;
}

bool ParseObservationConfigText(const std::string& contents,
                                const std::string& path,
                                ObservationConfig* config,
                                std::string* error) {
    if (!config) return Fail("config output pointer is null", error);
    json::Value root;
    if (!json::Parse(contents, &root, error)) return false;
    const json::Value::Object* root_object = NULL;
    if (!RequireObject(root, "root", &root_object, error)) return false;
    static const char* const root_keys[] = {
        "schema_version", "observation", "metadata", "wire", "blocks",
        "rings", "storage", "receiver", "processing"
    };
    if (!RequireExactKeys(*root_object, root_keys,
                          sizeof(root_keys) / sizeof(root_keys[0]),
                          "root", error)) return false;

    ObservationConfig parsed = ObservationConfig();
    parsed.source_path = path;
    if (!ReadUint32(*root_object, "schema_version", "root",
                    &parsed.schema_version, error)) return false;
    if (parsed.schema_version != 1U) {
        return Fail("unsupported observation schema_version", error);
    }

    const json::Value::Object *observation = NULL, *metadata = NULL;
    const json::Value::Object *wire = NULL, *blocks = NULL, *rings = NULL;
    const json::Value::Object *storage = NULL, *receiver = NULL, *processing = NULL;
    if (!RequireObject(Field(*root_object, "observation"), "observation",
                       &observation, error) ||
        !RequireObject(Field(*root_object, "metadata"), "metadata",
                       &metadata, error) ||
        !RequireObject(Field(*root_object, "wire"), "wire", &wire, error) ||
        !RequireObject(Field(*root_object, "blocks"), "blocks", &blocks, error) ||
        !RequireObject(Field(*root_object, "rings"), "rings", &rings, error) ||
        !RequireObject(Field(*root_object, "storage"), "storage", &storage, error) ||
        !RequireObject(Field(*root_object, "receiver"), "receiver", &receiver, error) ||
        !RequireObject(Field(*root_object, "processing"), "processing",
                       &processing, error)) return false;

    static const char* const observation_keys[] = {
        "observation_id", "utc_start", "duration_seconds", "station_ids",
        "first_channel_id", "nchan", "npol", "sample_interval_ps"
    };
    static const char* const metadata_keys[] = {
        "telescope", "bandwidth_hz", "center_frequency_hz"
    };
    static const char* const wire_keys[] = {"profile", "samples_per_packet"};
    static const char* const block_keys[] = {
        "groups_per_block", "raw_ring_blocks", "compute_ring_blocks",
        "window_blocks"
    };
    static const char* const ring_keys[] = {"raw_key", "compute_key"};
    static const char* const storage_keys[] = {
        "enabled", "blocks_per_file", "direct_io"
    };
    static const char* const receiver_keys[] = {
        "device", "destination_mac", "destination_ip", "destination_port"
    };
    static const char* const processing_keys[] = {
        "backend", "cuda_device", "run_once", "modules"
    };
    if (!RequireExactKeys(*observation, observation_keys,
                          sizeof(observation_keys) / sizeof(observation_keys[0]),
                          "observation", error) ||
        !RequireExactKeys(*metadata, metadata_keys, 3U, "metadata", error) ||
        !RequireExactKeys(*wire, wire_keys, 2U, "wire", error) ||
        !RequireExactKeys(*blocks, block_keys, 4U, "blocks", error) ||
        !RequireExactKeys(*rings, ring_keys, 2U, "rings", error) ||
        !RequireExactKeys(*storage, storage_keys, 3U, "storage", error) ||
        !RequireExactKeys(*receiver, receiver_keys, 4U, "receiver", error) ||
        !RequireExactKeys(*processing, processing_keys, 4U,
                          "processing", error)) return false;

    std::uint64_t first_channel = 0;
    std::uint64_t destination_port = 0;
    std::string profile_path;
    std::string raw_key_text;
    std::string compute_key_text;
    if (!ReadString(*observation, "observation_id", "observation",
                    &parsed.observation_id, error) ||
        !ReadString(*observation, "utc_start", "observation",
                    &parsed.utc_start, error) ||
        !ReadString(*observation, "duration_seconds", "observation",
                    &parsed.duration_seconds, error) ||
        !ParseExactSecondsToPicoseconds(parsed.duration_seconds,
                                        &parsed.duration_ps, error) ||
        !ReadUint64(*observation, "first_channel_id", "observation",
                    &first_channel, error) ||
        !ReadUint32(*observation, "nchan", "observation", &parsed.nchan, error) ||
        !ReadUint32(*observation, "npol", "observation", &parsed.npol, error) ||
        !ReadUint64(*observation, "sample_interval_ps", "observation",
                    &parsed.sample_interval_ps, error) ||
        !ReadString(*metadata, "telescope", "metadata", &parsed.telescope, error) ||
        !ReadUint64(*metadata, "bandwidth_hz", "metadata",
                    &parsed.bandwidth_hz, error) ||
        !ReadUint64(*metadata, "center_frequency_hz", "metadata",
                    &parsed.center_frequency_hz, error) ||
        !ReadString(*wire, "profile", "wire", &profile_path, error) ||
        !ReadUint64(*wire, "samples_per_packet", "wire",
                    &parsed.samples_per_packet, error) ||
        !ReadUint64(*blocks, "groups_per_block", "blocks",
                    &parsed.groups_per_block, error) ||
        !ReadUint64(*blocks, "raw_ring_blocks", "blocks",
                    &parsed.raw_ring_blocks, error) ||
        !ReadUint64(*blocks, "compute_ring_blocks", "blocks",
                    &parsed.compute_ring_blocks, error) ||
        !ReadUint64(*blocks, "window_blocks", "blocks",
                    &parsed.window_blocks, error) ||
        !ReadString(*rings, "raw_key", "rings", &raw_key_text, error) ||
        !ReadString(*rings, "compute_key", "rings", &compute_key_text, error) ||
        !ParseRingKey("rings.raw_key", raw_key_text, &parsed.raw_key, error) ||
        !ParseRingKey("rings.compute_key", compute_key_text,
                      &parsed.compute_key, error) ||
        !ReadBool(*storage, "enabled", "storage", &parsed.disk_enabled, error) ||
        !ReadUint64(*storage, "blocks_per_file", "storage",
                    &parsed.blocks_per_file, error) ||
        !ReadBool(*storage, "direct_io", "storage", &parsed.direct_io, error) ||
        !ReadString(*receiver, "device", "receiver",
                    &parsed.receiver_device, error) ||
        !ReadString(*receiver, "destination_mac", "receiver",
                    &parsed.destination_mac, error) ||
        !ReadString(*receiver, "destination_ip", "receiver",
                    &parsed.destination_ip, error) ||
        !ReadUint64(*receiver, "destination_port", "receiver",
                    &destination_port, error) ||
        !ReadString(*processing, "backend", "processing",
                    &parsed.backend, error) ||
        !ReadInt(*processing, "cuda_device", "processing",
                 &parsed.cuda_device, error) ||
        !ReadBool(*processing, "run_once", "processing",
                  &parsed.run_once, error)) return false;

    UtcDateTime utc_start = UtcDateTime();
    if (!ParseUtcDateTime(parsed.utc_start, &utc_start, error)) return false;
    if (first_channel > std::numeric_limits<std::uint16_t>::max()) {
        return Fail("observation.first_channel_id exceeds uint16 range", error);
    }
    parsed.first_channel_id = static_cast<std::uint16_t>(first_channel);
    if (parsed.nchan == 0U || parsed.nchan > 255U ||
        first_channel + parsed.nchan > UINT64_C(65536)) {
        return Fail("observation channel selection is outside Project VDIF range", error);
    }
    if (parsed.npol != 1U && parsed.npol != 2U) {
        return Fail("observation.npol must be 1 or 2", error);
    }
    if (parsed.sample_interval_ps == 0U || parsed.samples_per_packet == 0U ||
        parsed.groups_per_block == 0U || parsed.raw_ring_blocks == 0U ||
        parsed.compute_ring_blocks == 0U || parsed.window_blocks < 2U) {
        return Fail("sample, packet, block, ring and window values must be positive", error);
    }
    if (parsed.bandwidth_hz == 0U || parsed.center_frequency_hz == 0U) {
        return Fail("frequency metadata must be positive", error);
    }
    if (parsed.raw_key == parsed.compute_key) {
        return Fail("raw and compute ring keys must differ", error);
    }
    if (parsed.disk_enabled && parsed.blocks_per_file == 0U) {
        return Fail("storage.blocks_per_file must be positive when enabled", error);
    }
    if (!IsMacAddress(parsed.destination_mac)) {
        return Fail("receiver.destination_mac is invalid", error);
    }
    if (!IsIpv4Address(parsed.destination_ip)) {
        return Fail("receiver.destination_ip is invalid", error);
    }
    if (destination_port == 0U || destination_port > 65535U) {
        return Fail("receiver.destination_port must be in 1..65535", error);
    }
    parsed.destination_port = static_cast<std::uint16_t>(destination_port);
    if (parsed.backend != "CPU_REFERENCE" && parsed.backend != "CUDA") {
        return Fail("processing.backend must be CPU_REFERENCE or CUDA", error);
    }
    if (parsed.cuda_device < 0) {
        return Fail("processing.cuda_device must be non-negative", error);
    }
    parsed.wire_profile_path = ResolveRelative(path, profile_path);

    const json::Value& stations = Field(*observation, "station_ids");
    if (stations.type() != json::Value::kArray || stations.array().empty()) {
        return Fail("observation.station_ids must be a non-empty array", error);
    }
    std::set<std::uint16_t> station_set;
    for (std::size_t index = 0; index < stations.array().size(); ++index) {
        const json::Value& station = stations.array()[index];
        std::uint64_t value = 0;
        if (station.type() != json::Value::kNumber ||
            !ParseUint64Text("observation.station_ids", station.text(),
                             &value, error)) return false;
        if (value > std::numeric_limits<std::uint16_t>::max()) {
            return Fail("Station ID exceeds uint16 range", error);
        }
        const std::uint16_t id = static_cast<std::uint16_t>(value);
        if (!station_set.insert(id).second) {
            return Fail("observation.station_ids contains a duplicate", error);
        }
        parsed.station_ids.push_back(id);
    }

    const json::Value& modules = Field(*processing, "modules");
    if (modules.type() != json::Value::kArray) {
        return Fail("processing.modules must be an array", error);
    }
    for (std::size_t index = 0; index < modules.array().size(); ++index) {
        ObservationModuleConfig module = ObservationModuleConfig();
        if (!ParseModule(modules.array()[index], path, index, &module, error)) {
            return false;
        }
        parsed.modules.push_back(module);
    }
    if (!ValidateModuleOrder(parsed.modules, parsed.npol, error)) return false;

    *config = parsed;
    return true;
}

bool LoadObservationConfig(const std::string& path,
                           ObservationConfig* config,
                           std::string* error) {
    std::ifstream input(path.c_str());
    if (!input) return Fail("cannot open observation config: " + path, error);
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        return Fail("failed reading observation config: " + path, error);
    }
    return ParseObservationConfigText(contents.str(), path, config, error);
}

}  // namespace rdma_dada
