#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/config/json_value.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace rdma_dada {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool ParseUint64(const std::string& key, const std::string& text,
                 std::uint64_t* value, std::string* error) {
    if (text.empty() || text[0] == '-') {
        return Fail(key + " must be a non-negative integer", error);
    }
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') {
        return Fail(key + " is not a valid integer: " + text, error);
    }
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ParseUint32(const std::string& key, const std::string& text,
                 std::uint32_t* value, std::string* error) {
    std::uint64_t parsed = 0;
    if (!ParseUint64(key, text, &parsed, error)) return false;
    if (parsed > std::numeric_limits<std::uint32_t>::max()) {
        return Fail(key + " exceeds uint32 range", error);
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseDouble(const std::string& key, const std::string& text,
                 double* value, std::string* error) {
    errno = 0;
    char* end = NULL;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
        !std::isfinite(parsed)) {
        return Fail(key + " is not a valid finite number: " + text, error);
    }
    *value = parsed;
    return true;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     const std::string& name, std::uint64_t* result,
                     std::string* error) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Fail(name + " exceeds uint64 range", error);
    }
    *result = left * right;
    return true;
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

bool ReadUint64(const json::Value::Object& object, const std::string& key,
                const std::string& path, std::uint64_t* output,
                std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kNumber) {
        return Fail(path + "." + key + " must be an integer", error);
    }
    const std::string& text = value.text();
    if (text.find_first_of(".eE") != std::string::npos) {
        return Fail(path + "." + key + " must use integer JSON syntax", error);
    }
    return ParseUint64(path + "." + key, text, output, error);
}

bool ReadUint32(const json::Value::Object& object, const std::string& key,
                const std::string& path, std::uint32_t* output,
                std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kNumber) {
        return Fail(path + "." + key + " must be an integer", error);
    }
    const std::string& text = value.text();
    if (text.find_first_of(".eE") != std::string::npos) {
        return Fail(path + "." + key + " must use integer JSON syntax", error);
    }
    return ParseUint32(path + "." + key, text, output, error);
}

bool ReadDouble(const json::Value::Object& object, const std::string& key,
                const std::string& path, double* output, std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kNumber) {
        return Fail(path + "." + key + " must be a number", error);
    }
    return ParseDouble(path + "." + key, value.text(), output, error);
}

bool ReadString(const json::Value::Object& object, const std::string& key,
                const std::string& path, std::string* output,
                std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kString) {
        return Fail(path + "." + key + " must be a string", error);
    }
    *output = value.text();
    return true;
}

bool ReadBool(const json::Value::Object& object, const std::string& key,
              const std::string& path, bool* output, std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kBoolean) {
        return Fail(path + "." + key + " must be true or false", error);
    }
    *output = value.boolean();
    return true;
}

bool RoundedRate(std::uint64_t bytes, long double packet_seconds,
                 const std::string& name, std::uint64_t* result,
                 std::string* error) {
    const long double rate = static_cast<long double>(bytes) / packet_seconds;
    if (!std::isfinite(rate) || rate < 0.0L ||
        rate > static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
        return Fail(name + " exceeds uint64 range", error);
    }
    *result = static_cast<std::uint64_t>(std::floor(rate + 0.5L));
    return true;
}

}  // namespace

bool LoadPipelineConfig(const std::string& path,
                        PipelineConfig* config,
                        std::string* error) {
    if (!config) return Fail("config output pointer is null", error);

    std::ifstream input(path.c_str());
    if (!input) return Fail("cannot open config file: " + path, error);

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) return Fail("failed while reading config file: " + path, error);

    json::Value root;
    if (!json::Parse(contents.str(), &root, error)) return false;
    const json::Value::Object* root_object = NULL;
    if (!RequireObject(root, "root", &root_object, error)) return false;
    static const char* const root_keys[] = {
        "schema_version", "observation", "packet", "ring_buffers", "disk"
    };
    if (!RequireExactKeys(*root_object, root_keys,
                          sizeof(root_keys) / sizeof(root_keys[0]),
                          "root", error)) return false;

    std::uint32_t schema_version = 0;
    if (!ReadUint32(*root_object, "schema_version", "root",
                    &schema_version, error)) return false;
    if (schema_version != 1U) {
        return Fail("unsupported pipeline config schema_version: " +
                    Field(*root_object, "schema_version").text(), error);
    }

    const json::Value::Object* observation = NULL;
    const json::Value::Object* packet = NULL;
    const json::Value::Object* ring_buffers = NULL;
    const json::Value::Object* disk = NULL;
    if (!RequireObject(Field(*root_object, "observation"), "observation",
                       &observation, error) ||
        !RequireObject(Field(*root_object, "packet"), "packet",
                       &packet, error) ||
        !RequireObject(Field(*root_object, "ring_buffers"), "ring_buffers",
                       &ring_buffers, error) ||
        !RequireObject(Field(*root_object, "disk"), "disk", &disk, error)) {
        return false;
    }
    static const char* const observation_keys[] = {
        "nant", "nchan", "npol", "payload_order", "utc_start"
    };
    static const char* const packet_keys[] = {
        "header_bytes", "payload_bytes", "samples", "nbit",
        "sample_interval_us"
    };
    static const char* const ring_keys[] = {
        "records_per_block", "raw_blocks", "compute_blocks"
    };
    static const char* const disk_keys[] = {
        "enabled", "blocks_per_file", "direct_io"
    };
    if (!RequireExactKeys(*observation, observation_keys,
                          sizeof(observation_keys) / sizeof(observation_keys[0]),
                          "observation", error) ||
        !RequireExactKeys(*packet, packet_keys,
                          sizeof(packet_keys) / sizeof(packet_keys[0]),
                          "packet", error) ||
        !RequireExactKeys(*ring_buffers, ring_keys,
                          sizeof(ring_keys) / sizeof(ring_keys[0]),
                          "ring_buffers", error) ||
        !RequireExactKeys(*disk, disk_keys,
                          sizeof(disk_keys) / sizeof(disk_keys[0]),
                          "disk", error)) {
        return false;
    }

    PipelineConfig parsed = PipelineConfig();
    if (!ReadUint32(*observation, "nant", "observation", &parsed.nant, error) ||
        !ReadUint32(*observation, "nchan", "observation", &parsed.nchan, error) ||
        !ReadUint32(*observation, "npol", "observation", &parsed.npol, error) ||
        !ReadString(*observation, "payload_order", "observation",
                    &parsed.payload_order, error) ||
        !ReadString(*observation, "utc_start", "observation",
                    &parsed.utc_start, error) ||
        !ReadUint64(*packet, "header_bytes", "packet",
                    &parsed.packet_header_bytes, error) ||
        !ReadUint64(*packet, "payload_bytes", "packet",
                    &parsed.packet_payload_bytes, error) ||
        !ReadUint64(*packet, "samples", "packet",
                    &parsed.packet_samples, error) ||
        !ReadUint32(*packet, "nbit", "packet", &parsed.packet_nbit, error) ||
        !ReadDouble(*packet, "sample_interval_us", "packet",
                    &parsed.sample_interval_us, error) ||
        !ReadUint64(*ring_buffers, "records_per_block", "ring_buffers",
                    &parsed.records_per_block, error) ||
        !ReadUint64(*ring_buffers, "raw_blocks", "ring_buffers",
                    &parsed.raw_ring_blocks, error) ||
        !ReadUint64(*ring_buffers, "compute_blocks", "ring_buffers",
                    &parsed.compute_ring_blocks, error) ||
        !ReadBool(*disk, "enabled", "disk", &parsed.disk_enabled, error) ||
        !ReadUint64(*disk, "blocks_per_file", "disk",
                    &parsed.file_blocks, error) ||
        !ReadBool(*disk, "direct_io", "disk", &parsed.direct_io, error)) {
        return false;
    }

    *config = parsed;
    return true;
}

bool ComputePipelineLayout(const PipelineConfig& config,
                           PipelineLayout* layout,
                           std::string* error) {
    if (!layout) return Fail("layout output pointer is null", error);
    if (config.nant == 0 || config.nchan == 0 || config.npol == 0) {
        return Fail("NANT, NCHAN, and NPOL must all be greater than zero", error);
    }
    if (config.payload_order.empty()) return Fail("PAYLOAD_ORDER must not be empty", error);
    if (config.packet_header_bytes != 64) {
        return Fail("PKT_HEADER must be 64 for pipeline contract version 1", error);
    }
    if (config.packet_nbit != 16) {
        return Fail("PKT_NBIT must be 16 bits for pipeline contract version 1", error);
    }
    if (config.packet_payload_bytes == 0 || config.packet_samples == 0 ||
        config.sample_interval_us <= 0.0 || config.records_per_block == 0 ||
        config.raw_ring_blocks == 0 || config.compute_ring_blocks == 0) {
        return Fail("packet, timing, block, and ring sizes must be greater than zero", error);
    }
    if (config.disk_enabled && config.file_blocks == 0) {
        return Fail("disk.blocks_per_file must be greater than zero when disk is enabled", error);
    }
    if (config.utc_start.empty()) return Fail("UTC_START must be supplied externally", error);

    PipelineLayout result = PipelineLayout();
    if (config.packet_header_bytes >
        std::numeric_limits<std::uint64_t>::max() - config.packet_payload_bytes) {
        return Fail("raw record size exceeds uint64 range", error);
    }
    result.raw_record_bytes = config.packet_header_bytes + config.packet_payload_bytes;
    result.compute_record_bytes = config.packet_payload_bytes;
    result.raw_resolution = result.raw_record_bytes;
    result.compute_resolution = result.compute_record_bytes;

    if (!CheckedMultiply(result.raw_record_bytes, config.records_per_block,
                         "raw block size", &result.raw_block_bytes, error) ||
        !CheckedMultiply(result.compute_record_bytes, config.records_per_block,
                         "compute block size", &result.compute_block_bytes, error) ||
        !CheckedMultiply(result.raw_block_bytes, config.raw_ring_blocks,
                         "raw ring size", &result.raw_ring_bytes, error) ||
        !CheckedMultiply(result.compute_block_bytes, config.compute_ring_blocks,
                         "compute ring size", &result.compute_ring_bytes, error) ||
        !CheckedMultiply(result.raw_block_bytes, config.file_blocks,
                         "raw file size", &result.raw_file_bytes, error) ||
        !CheckedMultiply(result.compute_block_bytes, config.file_blocks,
                         "compute file size", &result.compute_file_bytes, error)) {
        return false;
    }

    const long double packet_seconds =
        static_cast<long double>(config.packet_samples) *
        static_cast<long double>(config.sample_interval_us) * 1.0e-6L;
    if (!std::isfinite(packet_seconds) || packet_seconds <= 0.0L) {
        return Fail("packet duration is invalid", error);
    }
    if (!RoundedRate(result.compute_record_bytes, packet_seconds,
                     "payload byte rate", &result.payload_bytes_per_second, error) ||
        !RoundedRate(result.raw_record_bytes, packet_seconds,
                     "raw byte rate", &result.raw_bytes_per_second, error)) {
        return false;
    }

    if (config.disk_enabled && config.direct_io) {
        const std::uint64_t sector_bytes = 512;
        if (result.raw_block_bytes % sector_bytes != 0 ||
            result.compute_block_bytes % sector_bytes != 0) {
            return Fail("DIRECT_IO requires every ring block size to be a multiple of 512 bytes", error);
        }
        std::uint64_t raw_file_multiple = 0;
        std::uint64_t compute_file_multiple = 0;
        if (!CheckedMultiply(result.raw_resolution, sector_bytes,
                             "raw O_DIRECT file multiple", &raw_file_multiple, error) ||
            !CheckedMultiply(result.compute_resolution, sector_bytes,
                             "compute O_DIRECT file multiple", &compute_file_multiple, error)) {
            return false;
        }
        if (result.raw_file_bytes % raw_file_multiple != 0 ||
            result.compute_file_bytes % compute_file_multiple != 0) {
            return Fail("DIRECT_IO requires file size to be an exact multiple of 512 * RESOLUTION", error);
        }
    }

    *layout = result;
    return true;
}

}  // namespace rdma_dada
