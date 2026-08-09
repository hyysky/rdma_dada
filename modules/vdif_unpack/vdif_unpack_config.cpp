#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"

#include "rdma_dada/config/json_value.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedMultiply(std::uint64_t a, std::uint64_t b,
                     const std::string& name, std::uint64_t* result,
                     std::string* error) {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        return Fail(name + " exceeds uint64 range", error);
    }
    *result = a * b;
    return true;
}

bool ExactKeys(const json::Value::Object& object,
               const char* const* keys, std::size_t count,
               const std::string& path, std::string* error) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!object.count(keys[i])) return Fail(path + " is missing: " + keys[i], error);
    }
    for (json::Value::Object::const_iterator it = object.begin();
         it != object.end(); ++it) {
        bool found = false;
        for (std::size_t i = 0; i < count; ++i) found = found || it->first == keys[i];
        if (!found) return Fail(path + " has unknown field: " + it->first, error);
    }
    return true;
}

bool ObjectField(const json::Value::Object& parent, const std::string& key,
                 const json::Value::Object** result, std::string* error) {
    const json::Value& value = parent.find(key)->second;
    if (value.type() != json::Value::kObject)
        return Fail(key + " must be an object", error);
    *result = &value.object();
    return true;
}

bool StringField(const json::Value::Object& object, const std::string& key,
                 std::string* result, std::string* error) {
    const json::Value& value = object.find(key)->second;
    if (value.type() != json::Value::kString)
        return Fail(key + " must be a string", error);
    *result = value.text();
    return true;
}

bool BoolField(const json::Value::Object& object, const std::string& key,
               bool* result, std::string* error) {
    const json::Value& value = object.find(key)->second;
    if (value.type() != json::Value::kBoolean)
        return Fail(key + " must be true or false", error);
    *result = value.boolean();
    return true;
}

bool Uint64Field(const json::Value::Object& object, const std::string& key,
                 std::uint64_t* result, std::string* error) {
    const json::Value& value = object.find(key)->second;
    if (value.type() != json::Value::kNumber ||
        value.text().find_first_of(".eE-") != std::string::npos)
        return Fail(key + " must be a non-negative integer", error);
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = std::strtoull(value.text().c_str(), &end, 10);
    if (errno == ERANGE || end == value.text().c_str() || *end != '\0')
        return Fail(key + " is outside uint64 range", error);
    *result = static_cast<std::uint64_t>(parsed);
    return true;
}

bool RingKeyField(const json::Value::Object& object, const std::string& key,
                  std::uint32_t* result, std::string* error) {
    std::string text;
    if (!StringField(object, key, &text, error)) return false;
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
        return Fail(key + " must use a 0x-prefixed hexadecimal string", error);
    errno = 0;
    char* end = NULL;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 16);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
        parsed > std::numeric_limits<std::uint32_t>::max())
        return Fail(key + " is outside uint32 range", error);
    *result = static_cast<std::uint32_t>(parsed);
    return true;
}

std::string ResolveRelative(const std::string& config_path,
                            const std::string& referenced) {
    if (!referenced.empty() && referenced[0] == '/') return referenced;
    const std::string::size_type slash = config_path.find_last_of('/');
    return slash == std::string::npos ? referenced
                                     : config_path.substr(0, slash + 1) + referenced;
}

}  // namespace

bool LoadVdifUnpackConfig(const std::string& path,
                          VdifUnpackConfig* config,
                          std::string* error) {
    if (!config) return Fail("config output pointer is null", error);
    std::ifstream input(path.c_str());
    if (!input) return Fail("cannot open config file: " + path, error);
    std::ostringstream contents; contents << input.rdbuf();
    json::Value root;
    if (!json::Parse(contents.str(), &root, error)) return false;
    if (root.type() != json::Value::kObject) return Fail("root must be an object", error);
    const json::Value::Object& object = root.object();
    static const char* const root_keys[] = {
        "schema_version", "rings", "sources", "selection", "window", "output",
        "runtime"
    };
    if (!ExactKeys(object, root_keys, 7, "root", error)) return false;
    std::uint64_t schema = 0;
    if (!Uint64Field(object, "schema_version", &schema, error) || schema != 1)
        return Fail("schema_version must be 1", error);

    const json::Value::Object *rings, *sources, *selection, *window, *output,
                              *runtime;
    if (!ObjectField(object, "rings", &rings, error) ||
        !ObjectField(object, "sources", &sources, error) ||
        !ObjectField(object, "selection", &selection, error) ||
        !ObjectField(object, "window", &window, error) ||
        !ObjectField(object, "output", &output, error) ||
        !ObjectField(object, "runtime", &runtime, error)) return false;
    static const char* const ring_keys[] = {"input_key", "output_key"};
    static const char* const source_keys[] = {"pipeline_config", "packet_format"};
    static const char* const selection_keys[] = {"first_channel_id", "antenna_map"};
    static const char* const window_keys[] = {"blocks", "max_bytes"};
    static const char* const output_keys[] = {"memory"};
    static const char* const runtime_keys[] = {"run_once"};
    if (!ExactKeys(*rings, ring_keys, 2, "rings", error) ||
        !ExactKeys(*sources, source_keys, 2, "sources", error) ||
        !ExactKeys(*selection, selection_keys, 2, "selection", error) ||
        !ExactKeys(*window, window_keys, 2, "window", error) ||
        !ExactKeys(*output, output_keys, 1, "output", error) ||
        !ExactKeys(*runtime, runtime_keys, 1, "runtime", error)) return false;

    VdifUnpackConfig parsed = {};
    std::string pipeline_path, packet_path;
    std::uint64_t first_channel = 0, blocks = 0;
    if (!RingKeyField(*rings, "input_key", &parsed.input_key, error) ||
        !RingKeyField(*rings, "output_key", &parsed.output_key, error) ||
        !StringField(*sources, "pipeline_config", &pipeline_path, error) ||
        !StringField(*sources, "packet_format", &packet_path, error) ||
        !Uint64Field(*selection, "first_channel_id", &first_channel, error) ||
        !Uint64Field(*window, "blocks", &blocks, error) ||
        !Uint64Field(*window, "max_bytes", &parsed.max_window_bytes, error) ||
        !StringField(*output, "memory", &parsed.output_memory, error) ||
        !BoolField(*runtime, "run_once", &parsed.run_once, error)) return false;
    if (parsed.input_key == parsed.output_key) return Fail("ring keys must differ", error);
    if (first_channel > std::numeric_limits<std::uint16_t>::max())
        return Fail("first_channel_id exceeds uint16 range", error);
    if (blocks > std::numeric_limits<std::uint32_t>::max())
        return Fail("window.blocks exceeds uint32 range", error);
    const json::Value& antennas = selection->find("antenna_map")->second;
    if (antennas.type() != json::Value::kArray)
        return Fail("antenna_map must be an array", error);
    for (std::size_t i = 0; i < antennas.array().size(); ++i) {
        const json::Value& item = antennas.array()[i];
        if (item.type() != json::Value::kNumber ||
            item.text().find_first_of(".eE-") != std::string::npos)
            return Fail("antenna_map entries must be uint16 integers", error);
        errno = 0; char* end = NULL;
        const unsigned long station = std::strtoul(item.text().c_str(), &end, 10);
        if (errno == ERANGE || end == item.text().c_str() || *end != '\0' || station > 65535U)
            return Fail("antenna_map entry exceeds uint16 range", error);
        parsed.antenna_map.push_back(static_cast<std::uint16_t>(station));
    }
    parsed.first_channel_id = static_cast<std::uint16_t>(first_channel);
    parsed.window_blocks = static_cast<std::uint32_t>(blocks);
    parsed.pipeline_config_path = ResolveRelative(path, pipeline_path);
    parsed.packet_format_path = ResolveRelative(path, packet_path);
    if (parsed.output_memory != "HOST") return Fail("output.memory must be HOST", error);
    *config = parsed;
    return true;
}

bool ComputeVdifUnpackLayout(const VdifUnpackConfig& config,
                             const PipelineConfig& pipeline,
                             const PacketFormatConfig& packet,
                             VdifUnpackLayout* layout,
                             std::string* error) {
    if (!layout) return Fail("layout output pointer is null", error);
    PipelineLayout base = {};
    if (!ComputePipelineLayout(pipeline, &base, error)) return false;
    if (config.antenna_map.size() != pipeline.nant)
        return Fail("antenna_map length must equal NANT", error);
    std::set<std::uint16_t> stations(config.antenna_map.begin(), config.antenna_map.end());
    if (stations.size() != config.antenna_map.size())
        return Fail("antenna_map contains duplicate Station IDs", error);
    if (config.window_blocks < 2) return Fail("window.blocks must be at least 2", error);
    if (static_cast<std::uint32_t>(config.first_channel_id) + pipeline.nchan > 65536U)
        return Fail("selected channel range exceeds uint16 range", error);
    if (packet.format_id != "project-vdif-v1" || packet.application_header_bytes != 32 ||
        packet.sample_format != "CI8" || packet.sample_encoding != "TWOS_COMPLEMENT" ||
        packet.component_order != "IQ" || packet.packed_order.size() != 3 ||
        packet.packed_order[0] != "T" || packet.packed_order[1] != "F" ||
        packet.packed_order[2] != "P")
        return Fail("packet profile does not match the Project VDIF wire contract", error);

    VdifUnpackLayout result = {};
    result.raw_record_bytes = base.raw_record_bytes;
    result.records_per_raw_block = pipeline.records_per_block;
    if (!CheckedMultiply(pipeline.packet_payload_bytes, pipeline.nant,
                         "group bytes", &result.group_bytes, error) ||
        !CheckedMultiply(config.window_blocks, base.packets_per_antenna_per_block,
                         "window group capacity", &result.window_capacity_groups, error) ||
        !CheckedMultiply(config.window_blocks, base.compute_block_bytes,
                         "window bytes", &result.window_bytes, error)) return false;
    result.compute_block_bytes = base.compute_block_bytes;
    if (result.window_bytes > config.max_window_bytes)
        return Fail("window allocation exceeds window.max_bytes", error);
    *layout = result;
    return true;
}

bool BuildVdifUnpackRuntimeFromResolvedPlan(
    const ResolvedObservationPlan& plan,
    VdifUnpackConfig* config,
    PipelineConfig* pipeline,
    PipelineLayout* pipeline_layout,
    VdifUnpackLayout* unpack_layout,
    std::string* error) {
    if (!config || !pipeline || !pipeline_layout || !unpack_layout) {
        return Fail("unpack runtime output pointer is null", error);
    }
    if (plan.config_id.size() != 64U || plan.geometry_id.size() != 64U) {
        return Fail("resolved plan identities are missing", error);
    }
    PipelineConfig pipeline_result;
    PipelineLayout pipeline_layout_result;
    if (!BuildPipelineRuntimeFromResolvedPlan(
            plan, &pipeline_result, &pipeline_layout_result, error)) {
        return false;
    }
    VdifUnpackConfig config_result = {};
    config_result.config_id = plan.config_id;
    config_result.geometry_id = plan.geometry_id;
    config_result.input_key = plan.source.raw_key;
    config_result.output_key = plan.source.compute_key;
    config_result.first_channel_id = plan.source.first_channel_id;
    config_result.antenna_map = plan.source.station_ids;
    config_result.window_blocks =
        static_cast<std::uint32_t>(plan.source.window_blocks);
    config_result.max_window_bytes = plan.window_payload_bytes;
    config_result.output_memory = "HOST";
    config_result.run_once = plan.source.run_once;
    VdifUnpackLayout unpack_result;
    if (!ComputeVdifUnpackLayout(config_result, pipeline_result, plan.wire,
                                 &unpack_result, error)) {
        return false;
    }
    if (unpack_result.raw_record_bytes != plan.raw_record_bytes ||
        unpack_result.records_per_raw_block != plan.records_per_block ||
        unpack_result.window_capacity_groups != plan.window_groups ||
        unpack_result.window_bytes != plan.window_payload_bytes ||
        unpack_result.compute_block_bytes != plan.compute_block_bytes) {
        return Fail("unpack runtime conflicts with resolved plan", error);
    }
    *config = config_result;
    *pipeline = pipeline_result;
    *pipeline_layout = pipeline_layout_result;
    *unpack_layout = unpack_result;
    return true;
}

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
