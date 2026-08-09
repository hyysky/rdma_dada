#include "rdma_dada/config/resolved_plan_json.h"

#include "rdma_dada/config/json_value.h"
#include "rdma_dada/config/sha256.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

namespace rdma_dada {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

std::string EscapeJson(const std::string& value) {
    std::ostringstream output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char current =
            static_cast<unsigned char>(value[index]);
        switch (current) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (current < 0x20U) {
                    static const char hex[] = "0123456789abcdef";
                    output << "\\u00" << hex[current >> 4U]
                           << hex[current & 0x0fU];
                } else {
                    output << static_cast<char>(current);
                }
        }
    }
    return output.str();
}

std::string Quote(const std::string& value) {
    return std::string("\"") + EscapeJson(value) + "\"";
}

const char* BoolText(bool value) { return value ? "true" : "false"; }

const char* ModuleName(ObservationModuleKind kind) {
    switch (kind) {
        case ObservationModuleKind::kBeamform: return "beamform";
        case ObservationModuleKind::kPower: return "power";
        case ObservationModuleKind::kStokes: return "stokes";
        case ObservationModuleKind::kIntegrate: return "integrate";
    }
    return "unknown";
}

bool ReadFile(const std::string& path, std::string* contents,
              std::string* error) {
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return Fail("cannot open referenced file: " + path, error);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) return Fail("cannot read referenced file: " + path, error);
    *contents = buffer.str();
    return true;
}

bool AbsolutePath(const std::string& path, std::string* absolute,
                  std::string* error) {
    char* resolved = realpath(path.c_str(), NULL);
    if (!resolved) return Fail("cannot resolve referenced file: " + path, error);
    *absolute = resolved;
    std::free(resolved);
    return true;
}

bool NormalizeReferencedPaths(ObservationConfig* config, std::string* error) {
    if (!AbsolutePath(config->wire_profile_path, &config->wire_profile_path,
                      error)) {
        return false;
    }
    for (std::size_t index = 0; index < config->modules.size(); ++index) {
        ObservationModuleConfig& module = config->modules[index];
        if (module.kind == ObservationModuleKind::kBeamform &&
            !AbsolutePath(module.weights_file, &module.weights_file, error)) {
            return false;
        }
    }
    return true;
}

std::string StationArray(const std::vector<std::uint16_t>& stations) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < stations.size(); ++index) {
        if (index != 0U) output << ',';
        output << stations[index];
    }
    output << ']';
    return output.str();
}

std::string ModulesJson(const std::vector<ObservationModuleConfig>& modules,
                        bool include_paths) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < modules.size(); ++index) {
        if (index != 0U) output << ',';
        const ObservationModuleConfig& module = modules[index];
        output << '{';
        if (module.kind == ObservationModuleKind::kBeamform) {
            output << "\"compute_mode\":" << Quote(module.compute_mode) << ',';
            output << "\"type\":\"beamform\",";
            if (include_paths) {
                output << "\"weights_file\":" << Quote(module.weights_file)
                       << ',';
            }
            output << "\"weights_id\":" << Quote(module.weights_id) << ',';
            output << "\"weights_order\":" << Quote(module.weights_order)
                   << ',';
            output << "\"weights_scale\":" << Quote(module.weights_scale);
        } else if (module.kind == ObservationModuleKind::kIntegrate) {
            output << "\"length\":" << module.integration_length << ',';
            output << "\"operation\":"
                   << Quote(module.integration_operation) << ',';
            output << "\"type\":\"integrate\"";
        } else {
            output << "\"type\":" << Quote(ModuleName(module.kind));
        }
        output << '}';
    }
    output << ']';
    return output.str();
}

std::string ObservationJson(const ObservationConfig& config,
                            const std::string& wire_reference,
                            bool include_paths) {
    std::ostringstream output;
    output << '{'
           << "\"blocks\":{"
           << "\"compute_ring_blocks\":" << config.compute_ring_blocks << ','
           << "\"groups_per_block\":" << config.groups_per_block << ','
           << "\"raw_ring_blocks\":" << config.raw_ring_blocks << ','
           << "\"window_blocks\":" << config.window_blocks << "},"
           << "\"metadata\":{"
           << "\"bandwidth_hz\":" << config.bandwidth_hz << ','
           << "\"center_frequency_hz\":" << config.center_frequency_hz << ','
           << "\"telescope\":" << Quote(config.telescope) << "},"
           << "\"observation\":{"
           << "\"duration_seconds\":" << Quote(config.duration_seconds) << ','
           << "\"first_channel_id\":" << config.first_channel_id << ','
           << "\"nchan\":" << config.nchan << ','
           << "\"npol\":" << config.npol << ','
           << "\"observation_id\":" << Quote(config.observation_id) << ','
           << "\"sample_interval_ps\":" << config.sample_interval_ps << ','
           << "\"station_ids\":" << StationArray(config.station_ids) << ','
           << "\"utc_start\":" << Quote(config.utc_start) << "},"
           << "\"processing\":{"
           << "\"backend\":" << Quote(config.backend) << ','
           << "\"cuda_device\":" << config.cuda_device << ','
           << "\"modules\":" << ModulesJson(config.modules, include_paths)
           << ',' << "\"run_once\":" << BoolText(config.run_once) << "},"
           << "\"receiver\":{"
           << "\"destination_ip\":" << Quote(config.destination_ip) << ','
           << "\"destination_mac\":" << Quote(config.destination_mac) << ','
           << "\"destination_port\":" << config.destination_port << ','
           << "\"device\":" << Quote(config.receiver_device) << "},"
           << "\"rings\":{"
           << "\"compute_key\":";
    std::ostringstream compute_key;
    compute_key << "0x" << std::hex << config.compute_key;
    std::ostringstream raw_key;
    raw_key << "0x" << std::hex << config.raw_key;
    output << Quote(compute_key.str()) << ','
           << "\"raw_key\":" << Quote(raw_key.str()) << "},"
           << "\"schema_version\":" << config.schema_version << ','
           << "\"storage\":{"
           << "\"blocks_per_file\":" << config.blocks_per_file << ','
           << "\"direct_io\":" << BoolText(config.direct_io) << ','
           << "\"enabled\":" << BoolText(config.disk_enabled) << "},"
           << "\"wire\":{"
           << "\"profile\":" << Quote(wire_reference) << ','
           << "\"samples_per_packet\":" << config.samples_per_packet << "}"
           << '}';
    return output.str();
}

std::map<std::string, std::uint64_t> ResolvedValues(
    const ResolvedObservationPlan& plan) {
    std::map<std::string, std::uint64_t> values;
    values["complex_sample_bytes"] = plan.complex_sample_bytes;
    values["compute_block_bytes"] = plan.compute_block_bytes;
    values["compute_file_bytes"] = plan.compute_file_bytes;
    values["compute_ring_bytes"] = plan.compute_ring_bytes;
    values["expected_groups"] = plan.expected_groups;
    values["group_period_ps"] = plan.group_period_ps;
    values["group_start_frame"] = plan.group_start_frame;
    values["group_start_reference_epoch"] = plan.group_start_reference_epoch;
    values["group_start_seconds"] = plan.group_start_seconds;
    values["nant"] = plan.nant;
    values["payload_bytes"] = plan.payload_bytes;
    values["payload_bytes_per_second"] = plan.payload_bytes_per_second;
    values["raw_block_bytes"] = plan.raw_block_bytes;
    values["raw_bytes_per_second"] = plan.raw_bytes_per_second;
    values["raw_file_bytes"] = plan.raw_file_bytes;
    values["raw_record_bytes"] = plan.raw_record_bytes;
    values["raw_ring_bytes"] = plan.raw_ring_bytes;
    values["records_per_block"] = plan.records_per_block;
    values["samples_per_block"] = plan.samples_per_block;
    values["window_groups"] = plan.window_groups;
    values["window_payload_bytes"] = plan.window_payload_bytes;
    values["window_validity_bytes"] = plan.window_validity_bytes;
    return values;
}

std::string ResolvedValuesJson(const ResolvedObservationPlan& plan) {
    const std::map<std::string, std::uint64_t> values = ResolvedValues(plan);
    std::ostringstream output;
    output << '{';
    bool first = true;
    for (std::map<std::string, std::uint64_t>::const_iterator item =
             values.begin(); item != values.end(); ++item) {
        if (!first) output << ',';
        first = false;
        output << Quote(item->first) << ':' << item->second;
    }
    output << '}';
    return output.str();
}

bool ParseUint64(const std::string& path, const json::Value& value,
                 std::uint64_t* output, std::string* error) {
    if (value.type() != json::Value::kNumber ||
        value.text().find_first_of("-.eE") != std::string::npos) {
        return Fail(path + " must be a uint64 integer", error);
    }
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed =
        std::strtoull(value.text().c_str(), &end, 10);
    if (errno == ERANGE || end == value.text().c_str() || *end != '\0') {
        return Fail(path + " is outside uint64 range", error);
    }
    *output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool StringField(const json::Value::Object& object, const char* name,
                 std::string* output, std::string* error) {
    const json::Value::Object::const_iterator item = object.find(name);
    if (item == object.end() || item->second.type() != json::Value::kString) {
        return Fail(std::string("resolved plan missing string field: ") + name,
                    error);
    }
    *output = item->second.text();
    return true;
}

bool ExactTopLevel(const json::Value::Object& object, std::string* error) {
    static const char* const keys[] = {
        "config_id", "geometry_id", "resolved", "schema_version",
        "source_json", "source_path", "wire_profile_path"
    };
    if (object.size() != sizeof(keys) / sizeof(keys[0])) {
        return Fail("resolved plan has missing or unknown top-level fields", error);
    }
    for (std::size_t index = 0; index < sizeof(keys) / sizeof(keys[0]); ++index) {
        if (object.count(keys[index]) == 0U) {
            return Fail(std::string("resolved plan missing field: ") + keys[index],
                        error);
        }
    }
    return true;
}

bool ReadReferencedDigests(const ResolvedObservationPlan& plan,
                           std::string* wire_digest,
                           std::vector<std::pair<std::string, std::string> >*
                               weight_digests,
                           std::string* error) {
    std::string contents;
    if (!ReadFile(plan.source.wire_profile_path, &contents, error)) return false;
    *wire_digest = Sha256Hex(contents.data(), contents.size());
    weight_digests->clear();
    for (std::size_t index = 0; index < plan.source.modules.size(); ++index) {
        const ObservationModuleConfig& module = plan.source.modules[index];
        if (module.kind != ObservationModuleKind::kBeamform) continue;
        if (!ReadFile(module.weights_file, &contents, error)) return false;
        weight_digests->push_back(std::make_pair(
            module.weights_id, Sha256Hex(contents.data(), contents.size())));
    }
    return true;
}

std::string GeometryMaterial(const ResolvedObservationPlan& plan,
                             const std::string& wire_digest) {
    std::ostringstream output;
    output << '{'
           << "\"first_channel_id\":" << plan.source.first_channel_id << ','
           << "\"module_chain\":"
           << ModulesJson(plan.source.modules, false) << ','
           << "\"nchan\":" << plan.source.nchan << ','
           << "\"npol\":" << plan.source.npol << ','
           << "\"resolved\":" << ResolvedValuesJson(plan) << ','
           << "\"sample_interval_ps\":" << plan.source.sample_interval_ps
           << ',' << "\"samples_per_packet\":"
           << plan.source.samples_per_packet << ','
           << "\"station_ids\":" << StationArray(plan.source.station_ids)
           << ',' << "\"utc_start\":" << Quote(plan.source.utc_start) << ','
           << "\"wire_sha256\":" << Quote(wire_digest)
           << '}';
    return output.str();
}

std::string ConfigMaterial(
    const ResolvedObservationPlan& plan, const std::string& wire_digest,
    const std::vector<std::pair<std::string, std::string> >& weight_digests) {
    std::ostringstream weights;
    weights << '[';
    for (std::size_t index = 0; index < weight_digests.size(); ++index) {
        if (index != 0U) weights << ',';
        weights << "{\"id\":" << Quote(weight_digests[index].first)
                << ",\"sha256\":" << Quote(weight_digests[index].second)
                << '}';
    }
    weights << ']';
    std::ostringstream output;
    output << '{'
           << "\"observation\":"
           << ObservationJson(plan.source, plan.wire.format_id, false) << ','
           << "\"resolved\":" << ResolvedValuesJson(plan) << ','
           << "\"weight_files\":" << weights.str() << ','
           << "\"wire_sha256\":" << Quote(wire_digest)
           << '}';
    return output.str();
}

}  // namespace

bool ComputeObservationIdentities(ResolvedObservationPlan* plan,
                                  std::string* error) {
    if (!plan) return Fail("resolved plan pointer is null", error);
    ResolvedObservationPlan expected;
    if (!ResolveObservationPlan(plan->source, plan->wire, &expected, error)) {
        return false;
    }
    if (ResolvedValues(expected) != ResolvedValues(*plan)) {
        return Fail("resolved values conflict with source configuration", error);
    }
    std::string wire_digest;
    std::vector<std::pair<std::string, std::string> > weight_digests;
    if (!ReadReferencedDigests(*plan, &wire_digest, &weight_digests, error)) {
        return false;
    }
    const std::string geometry = GeometryMaterial(*plan, wire_digest);
    const std::string config = ConfigMaterial(*plan, wire_digest, weight_digests);
    plan->geometry_id = Sha256Hex(geometry.data(), geometry.size());
    plan->config_id = Sha256Hex(config.data(), config.size());
    return true;
}

bool SerializeResolvedObservationPlan(const ResolvedObservationPlan& plan,
                                      std::string* json_text,
                                      std::string* error) {
    if (!json_text) return Fail("serialized JSON output pointer is null", error);
    ResolvedObservationPlan verified = plan;
    if (!ComputeObservationIdentities(&verified, error)) return false;
    if (verified.config_id != plan.config_id ||
        verified.geometry_id != plan.geometry_id) {
        return Fail("resolved plan identities are missing or stale", error);
    }
    ObservationConfig serialized_source = plan.source;
    if (!NormalizeReferencedPaths(&serialized_source, error)) return false;
    const std::string source_json = ObservationJson(
        serialized_source, serialized_source.wire_profile_path, true);
    std::ostringstream output;
    output << '{'
           << "\"config_id\":" << Quote(plan.config_id) << ','
           << "\"geometry_id\":" << Quote(plan.geometry_id) << ','
           << "\"resolved\":" << ResolvedValuesJson(plan) << ','
           << "\"schema_version\":1,"
           << "\"source_json\":" << Quote(source_json) << ','
           << "\"source_path\":" << Quote(plan.source.source_path) << ','
           << "\"wire_profile_path\":"
           << Quote(serialized_source.wire_profile_path)
           << "}\n";
    *json_text = output.str();
    return true;
}

bool LoadResolvedObservationPlan(const std::string& path,
                                 ResolvedObservationPlan* plan,
                                 std::string* error) {
    if (!plan) return Fail("resolved plan output pointer is null", error);
    std::string contents;
    if (!ReadFile(path, &contents, error)) return false;
    json::Value root;
    if (!json::Parse(contents, &root, error) ||
        root.type() != json::Value::kObject) {
        return Fail("resolved plan root must be a JSON object", error);
    }
    const json::Value::Object& object = root.object();
    if (!ExactTopLevel(object, error)) return false;
    std::uint64_t schema_version = 0;
    if (!ParseUint64("schema_version", object.find("schema_version")->second,
                     &schema_version, error) || schema_version != 1U) {
        return Fail("resolved plan schema_version must be 1", error);
    }
    std::string stored_config_id;
    std::string stored_geometry_id;
    std::string source_json;
    std::string source_path;
    std::string wire_profile_path;
    if (!StringField(object, "config_id", &stored_config_id, error) ||
        !StringField(object, "geometry_id", &stored_geometry_id, error) ||
        !StringField(object, "source_json", &source_json, error) ||
        !StringField(object, "source_path", &source_path, error) ||
        !StringField(object, "wire_profile_path", &wire_profile_path, error)) {
        return false;
    }
    ObservationConfig source;
    if (!ParseObservationConfigText(source_json, source_path, &source, error)) {
        return false;
    }
    if (source.wire_profile_path != wire_profile_path) {
        return Fail("wire_profile_path conflicts with embedded source", error);
    }
    PacketFormatConfig wire;
    if (!LoadPacketFormatConfig(wire_profile_path, &wire, error)) return false;
    ResolvedObservationPlan resolved;
    if (!ResolveObservationPlan(source, wire, &resolved, error)) return false;

    const json::Value& resolved_json = object.find("resolved")->second;
    if (resolved_json.type() != json::Value::kObject) {
        return Fail("resolved field must be an object", error);
    }
    const std::map<std::string, std::uint64_t> expected =
        ResolvedValues(resolved);
    if (resolved_json.object().size() != expected.size()) {
        return Fail("resolved geometry has missing or unknown fields", error);
    }
    for (std::map<std::string, std::uint64_t>::const_iterator item =
             expected.begin(); item != expected.end(); ++item) {
        const json::Value::Object::const_iterator stored =
            resolved_json.object().find(item->first);
        std::uint64_t value = 0;
        if (stored == resolved_json.object().end() ||
            !ParseUint64("resolved." + item->first, stored->second, &value,
                         error) || value != item->second) {
            return Fail("resolved geometry mismatch: " + item->first, error);
        }
    }
    if (!ComputeObservationIdentities(&resolved, error)) return false;
    if (resolved.config_id != stored_config_id ||
        resolved.geometry_id != stored_geometry_id) {
        return Fail("resolved plan identity verification failed", error);
    }
    *plan = resolved;
    return true;
}

}  // namespace rdma_dada
