#include "rdma_dada/pipeline/worker_config.h"

#include "rdma_dada/config/json_value.h"
#include "rdma_dada/config/resolved_plan_json.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>

namespace rdma_dada {
namespace pipeline {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     const std::string& name, std::uint64_t* result,
                     std::string* error) {
    if (!result) return Fail(name + " output pointer is null", error);
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Fail(name + " exceeds uint64 range", error);
    }
    *result = left * right;
    return true;
}

bool CheckedAdd(std::uint64_t left, std::uint64_t right,
                const std::string& name, std::uint64_t* result,
                std::string* error) {
    if (!result) return Fail(name + " output pointer is null", error);
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return Fail(name + " exceeds uint64 range", error);
    }
    *result = left + right;
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
    for (std::size_t i = 0; i < key_count; ++i) {
        if (object.count(keys[i]) == 0U) {
            return Fail(path + " is missing required field: " + keys[i], error);
        }
    }
    for (json::Value::Object::const_iterator field = object.begin();
         field != object.end(); ++field) {
        bool known = false;
        for (std::size_t i = 0; i < key_count; ++i) {
            if (field->first == keys[i]) {
                known = true;
                break;
            }
        }
        if (!known) return Fail(path + " has unknown field: " + field->first, error);
    }
    return true;
}

const json::Value& Field(const json::Value::Object& object,
                         const std::string& key) {
    return object.find(key)->second;
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

bool ReadBool(const json::Value::Object& object, const std::string& key,
              const std::string& path, bool* output, std::string* error) {
    const json::Value& value = Field(object, key);
    if (value.type() != json::Value::kBoolean) {
        return Fail(path + "." + key + " must be true or false", error);
    }
    *output = value.boolean();
    return true;
}

bool ParseUint64(const std::string& name, const json::Value& value,
                 std::uint64_t* output, std::string* error) {
    if (value.type() != json::Value::kNumber ||
        value.text().find_first_of(".eE-") != std::string::npos) {
        return Fail(name + " must use non-negative integer JSON syntax", error);
    }
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed =
        std::strtoull(value.text().c_str(), &end, 10);
    if (errno == ERANGE || end == value.text().c_str() || *end != '\0') {
        return Fail(name + " is not a valid integer", error);
    }
    *output = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ReadDouble(const json::Value::Object& object, const std::string& key,
                const std::string& path, double* output, std::string* error) {
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

bool ParseRingKey(const std::string& text, const std::string& name,
                  std::uint32_t* output, std::string* error) {
    const char* begin = text.c_str();
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        begin += 2;
    }
    if (*begin == '\0' || *begin == '-') {
        return Fail(name + " must be a non-zero hexadecimal ring key", error);
    }
    errno = 0;
    char* end = NULL;
    const unsigned long parsed = std::strtoul(begin, &end, 16);
    if (errno == ERANGE || end == begin || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return Fail(name + " must be a non-zero uint32 hexadecimal ring key",
                    error);
    }
    *output = static_cast<std::uint32_t>(parsed);
    return true;
}

std::string ResolveRelativePath(const std::string& config_path,
                                const std::string& value) {
    if (value.empty() || value[0] == '/') return value;
    const std::size_t slash = config_path.find_last_of('/');
    if (slash == std::string::npos) return value;
    return config_path.substr(0, slash + 1) + value;
}

}  // namespace

bool LoadWorkerConfig(const std::string& path, WorkerConfig* config,
                      std::string* error) {
    if (!config) return Fail("worker config output pointer is null", error);
    std::ifstream input(path.c_str());
    if (!input) return Fail("cannot open worker config file: " + path, error);
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) return Fail("failed while reading worker config: " + path, error);

    json::Value root;
    if (!json::Parse(contents.str(), &root, error)) return false;
    const json::Value::Object* root_object = NULL;
    if (!RequireObject(root, "root", &root_object, error)) return false;
    static const char* const root_keys_v1[] = {
        "schema_version", "rings", "execution", "input_geometry",
        "beamform", "output"
    };
    static const char* const root_keys_v2[] = {
        "schema_version", "rings", "execution", "input_geometry",
        "beamform", "output", "integration"
    };
    static const char* const root_keys_v3[] = {
        "schema_version", "rings", "execution", "input_geometry",
        "conversion", "beamform", "output", "integration"
    };

    std::uint64_t schema_version = 0;
    if (root_object->count("schema_version") == 0U) {
        return Fail("root is missing required field: schema_version", error);
    }
    if (!ParseUint64("root.schema_version", Field(*root_object, "schema_version"),
                     &schema_version, error)) return false;
    if (schema_version == 1) {
        if (!RequireExactKeys(
                *root_object, root_keys_v1,
                sizeof(root_keys_v1) / sizeof(root_keys_v1[0]),
                "root", error)) {
            return false;
        }
    } else if (schema_version == 2) {
        if (!RequireExactKeys(
                *root_object, root_keys_v2,
                sizeof(root_keys_v2) / sizeof(root_keys_v2[0]),
                "root", error)) {
            return false;
        }
    } else if (schema_version == 3) {
        if (!RequireExactKeys(
                *root_object, root_keys_v3,
                sizeof(root_keys_v3) / sizeof(root_keys_v3[0]),
                "root", error)) {
            return false;
        }
    } else {
        return Fail("unsupported worker schema_version", error);
    }

    const json::Value::Object* rings = NULL;
    const json::Value::Object* execution = NULL;
    const json::Value::Object* input_geometry = NULL;
    const json::Value::Object* conversion = NULL;
    const json::Value::Object* beamform = NULL;
    const json::Value::Object* output = NULL;
    const json::Value::Object* integration = NULL;
    if (!RequireObject(Field(*root_object, "rings"), "rings", &rings, error) ||
        !RequireObject(Field(*root_object, "execution"), "execution",
                       &execution, error) ||
        !RequireObject(Field(*root_object, "input_geometry"),
                       "input_geometry", &input_geometry, error) ||
        !RequireObject(Field(*root_object, "beamform"), "beamform",
                       &beamform, error) ||
        !RequireObject(Field(*root_object, "output"), "output", &output,
                       error)) {
        return false;
    }
    if (schema_version == 3 &&
        !RequireObject(Field(*root_object, "conversion"), "conversion",
                       &conversion, error)) {
        return false;
    }
    if (schema_version >= 2 &&
        !RequireObject(Field(*root_object, "integration"), "integration",
                       &integration, error)) {
        return false;
    }

    static const char* const ring_keys[] = {"input_key", "output_key"};
    static const char* const execution_keys[] = {
        "backend", "cuda_device", "run_once"
    };
    static const char* const input_geometry_keys[] = {
        "nchan", "nant", "npol", "udp_payload_bytes",
        "samples_per_udp", "udp_packets_per_antenna_per_block"
    };
    static const char* const beamform_keys[] = {
        "weights_file", "weights_order", "weights_id", "weights_scale",
        "nbeam", "compute_mode"
    };
    static const char* const conversion_keys[] = {"scale"};
    static const char* const output_keys[] = {"product"};
    static const char* const integration_keys[] = {
        "enabled", "length", "operation"
    };
    if (!RequireExactKeys(*rings, ring_keys,
                          sizeof(ring_keys) / sizeof(ring_keys[0]),
                          "rings", error) ||
        !RequireExactKeys(*execution, execution_keys,
                          sizeof(execution_keys) / sizeof(execution_keys[0]),
                          "execution", error) ||
        !RequireExactKeys(
            *input_geometry, input_geometry_keys,
            sizeof(input_geometry_keys) / sizeof(input_geometry_keys[0]),
            "input_geometry", error) ||
        !RequireExactKeys(*beamform, beamform_keys,
                          sizeof(beamform_keys) / sizeof(beamform_keys[0]),
                          "beamform", error) ||
        !RequireExactKeys(*output, output_keys,
                          sizeof(output_keys) / sizeof(output_keys[0]),
                          "output", error)) {
        return false;
    }
    if (schema_version == 3 &&
        !RequireExactKeys(*conversion, conversion_keys,
                          sizeof(conversion_keys) /
                              sizeof(conversion_keys[0]),
                          "conversion", error)) {
        return false;
    }
    if (schema_version >= 2 &&
        !RequireExactKeys(
            *integration, integration_keys,
            sizeof(integration_keys) / sizeof(integration_keys[0]),
            "integration", error)) {
        return false;
    }

    WorkerConfig parsed = WorkerConfig();
    parsed.cuda_device = -1;
    parsed.integration_enabled = false;
    parsed.integration_length = 1;
    parsed.integration_operation = "MEAN";
    parsed.conversion_scale = 1.0;
    std::uint64_t cuda_device = 0;
    std::uint64_t nbeam = 0;
    std::string product;
    if (!ReadString(*rings, "input_key", "rings", &parsed.input_key_text,
                    error) ||
        !ReadString(*rings, "output_key", "rings", &parsed.output_key_text,
                    error) ||
        !ParseRingKey(parsed.input_key_text, "rings.input_key",
                      &parsed.input_key, error) ||
        !ParseRingKey(parsed.output_key_text, "rings.output_key",
                      &parsed.output_key, error) ||
        !ReadString(*execution, "backend", "execution",
                    &parsed.execution_backend, error) ||
        !ParseUint64("execution.cuda_device",
                     Field(*execution, "cuda_device"), &cuda_device, error) ||
        !ReadBool(*execution, "run_once", "execution", &parsed.run_once,
                  error) ||
        !ParseUint64("input_geometry.nchan",
                     Field(*input_geometry, "nchan"), &parsed.nchan, error) ||
        !ParseUint64("input_geometry.nant",
                     Field(*input_geometry, "nant"), &parsed.nant, error) ||
        !ParseUint64("input_geometry.npol",
                     Field(*input_geometry, "npol"), &parsed.npol, error) ||
        !ParseUint64("input_geometry.udp_payload_bytes",
                     Field(*input_geometry, "udp_payload_bytes"),
                     &parsed.udp_payload_bytes, error) ||
        !ParseUint64("input_geometry.samples_per_udp",
                     Field(*input_geometry, "samples_per_udp"),
                     &parsed.samples_per_udp, error) ||
        !ParseUint64(
            "input_geometry.udp_packets_per_antenna_per_block",
            Field(*input_geometry, "udp_packets_per_antenna_per_block"),
            &parsed.udp_packets_per_antenna_per_block, error) ||
        !ReadString(*beamform, "weights_file", "beamform",
                    &parsed.weights_file, error) ||
        !ReadString(*beamform, "weights_order", "beamform",
                    &parsed.weights_order, error) ||
        !ReadString(*beamform, "weights_id", "beamform",
                    &parsed.weights_id, error) ||
        !ReadDouble(*beamform, "weights_scale", "beamform",
                    &parsed.weights_scale, error) ||
        !ParseUint64("beamform.nbeam", Field(*beamform, "nbeam"),
                     &nbeam, error) ||
        !ReadString(*beamform, "compute_mode", "beamform",
                    &parsed.compute_mode, error) ||
        !ReadString(*output, "product", "output", &product, error)) {
        return false;
    }

    if (parsed.input_key == parsed.output_key) {
        return Fail("input and output ring keys must be different", error);
    }
    if (schema_version == 3 &&
        !ReadDouble(*conversion, "scale", "conversion",
                    &parsed.conversion_scale, error)) {
        return false;
    }
    if (!(parsed.conversion_scale > 0.0) ||
        !std::isfinite(parsed.conversion_scale)) {
        return Fail("conversion.scale must be finite and positive", error);
    }
    if (cuda_device > static_cast<std::uint64_t>(
                          std::numeric_limits<int>::max())) {
        return Fail("execution.cuda_device exceeds int range", error);
    }
    parsed.cuda_device = static_cast<int>(cuda_device);
    if (parsed.execution_backend != "CPU_REFERENCE" &&
        parsed.execution_backend != "CUDA") {
        return Fail("execution.backend must be CPU_REFERENCE or CUDA", error);
    }
    if (parsed.weights_order != "FPAB2") {
        return Fail("beamform.weights_order must be FPAB2", error);
    }
    if (parsed.weights_scale <= 0.0 || !std::isfinite(parsed.weights_scale)) {
        return Fail("beamform.weights_scale must be finite and positive", error);
    }
    if (nbeam == 0) return Fail("beamform.nbeam must be greater than zero", error);
    parsed.nbeam = nbeam;
    if (parsed.compute_mode != "FP32" && parsed.compute_mode != "TF32") {
        return Fail("beamform.compute_mode must be FP32 or TF32", error);
    }
    if (parsed.execution_backend == "CPU_REFERENCE" &&
        parsed.compute_mode != "FP32") {
        return Fail("CPU_REFERENCE supports only beamform.compute_mode=FP32",
                    error);
    }
    if (product == "BEAMFORMED") {
        parsed.product = WorkerProduct::kBeamformed;
    } else if (product == "POWER") {
        parsed.product = WorkerProduct::kPower;
    } else if (product == "STOKES") {
        parsed.product = WorkerProduct::kStokes;
    } else {
        return Fail("output.product must be BEAMFORMED, POWER, or STOKES",
                    error);
    }

    if (schema_version >= 2) {
        std::string integration_operation;
        if (!ReadBool(*integration, "enabled", "integration",
                      &parsed.integration_enabled, error) ||
            !ParseUint64("integration.length",
                         Field(*integration, "length"),
                         &parsed.integration_length, error) ||
            !ReadString(*integration, "operation", "integration",
                        &integration_operation, error)) {
            return false;
        }
        if (parsed.integration_length == 0) {
            return Fail("integration.length must be greater than zero", error);
        }
        if (integration_operation == "sum") {
            parsed.integration_operation = "SUM";
        } else if (integration_operation == "mean") {
            parsed.integration_operation = "MEAN";
        } else {
            return Fail("integration.operation must be sum or mean", error);
        }
        if (parsed.integration_enabled &&
            parsed.product == WorkerProduct::kBeamformed) {
            return Fail(
                "time integration requires output.product POWER or STOKES",
                error);
        }
    }

    parsed.weights_file = ResolveRelativePath(path, parsed.weights_file);
    WorkerBlockGeometry geometry;
    if (!ComputeWorkerBlockGeometry(parsed, &geometry, error)) return false;
    *config = parsed;
    return true;
}

bool BuildWorkerConfigFromResolvedPlan(
    const ResolvedObservationPlan& plan,
    WorkerConfig* config,
    WorkerBlockGeometry* geometry,
    std::string* error) {
    if (!config || !geometry) {
        return Fail("resolved worker output pointer is null", error);
    }
    ResolvedObservationPlan verified;
    if (!ResolveObservationPlan(plan.source, plan.wire, &verified, error)) {
        return false;
    }
    if (plan.source.modules.empty()) {
        return Fail("pipeline_worker requires a beamform module chain", error);
    }
    if (verified.nbeam != plan.nbeam ||
        verified.compute_block_bytes != plan.compute_block_bytes ||
        verified.converted_block_bytes != plan.converted_block_bytes ||
        verified.beamformed_block_bytes != plan.beamformed_block_bytes ||
        verified.product_block_bytes != plan.product_block_bytes ||
        verified.output_samples_per_block != plan.output_samples_per_block ||
        verified.output_block_bytes != plan.output_block_bytes ||
        verified.output_ring_bytes != plan.output_ring_bytes ||
        verified.output_data_stage != plan.output_data_stage ||
        verified.output_order != plan.output_order ||
        verified.output_sample_format != plan.output_sample_format) {
        return Fail("resolved plan contains stale worker geometry", error);
    }

    const ObservationModuleConfig& beamform = plan.source.modules[0];
    WorkerConfig result = WorkerConfig();
    result.input_key = plan.source.compute_key;
    result.output_key = plan.source.output_key;
    std::ostringstream input_key;
    std::ostringstream output_key;
    input_key << "0x" << std::hex << result.input_key;
    output_key << "0x" << std::hex << result.output_key;
    result.input_key_text = input_key.str();
    result.output_key_text = output_key.str();
    result.execution_backend = plan.source.backend;
    result.cuda_device = plan.source.cuda_device;
    result.run_once = plan.source.run_once;
    result.nchan = plan.source.nchan;
    result.nant = plan.nant;
    result.npol = plan.source.npol;
    result.udp_payload_bytes = plan.payload_bytes;
    result.samples_per_udp = plan.source.samples_per_packet;
    result.udp_packets_per_antenna_per_block =
        plan.source.groups_per_block;

    errno = 0;
    char* scale_end = NULL;
    result.conversion_scale =
        std::strtod(plan.source.conversion_scale.c_str(), &scale_end);
    if (errno == ERANGE || scale_end == plan.source.conversion_scale.c_str() ||
        *scale_end != '\0' || !std::isfinite(result.conversion_scale) ||
        result.conversion_scale <= 0.0) {
        return Fail("processing.conversion.scale must be finite and positive",
                    error);
    }
    result.weights_file = beamform.weights_file;
    result.weights_order = beamform.weights_order;
    result.weights_id = beamform.weights_id;
    errno = 0;
    char* weights_scale_end = NULL;
    result.weights_scale =
        std::strtod(beamform.weights_scale.c_str(), &weights_scale_end);
    if (errno == ERANGE || weights_scale_end == beamform.weights_scale.c_str() ||
        *weights_scale_end != '\0' || !std::isfinite(result.weights_scale) ||
        result.weights_scale <= 0.0) {
        return Fail("beamform weights_scale must be finite and positive",
                    error);
    }
    result.nbeam = plan.nbeam;
    result.compute_mode = beamform.compute_mode;
    result.product = WorkerProduct::kBeamformed;
    result.integration_enabled = false;
    result.integration_length = 1U;
    result.integration_operation = "MEAN";

    if (plan.source.modules.size() >= 2U) {
        const ObservationModuleKind kind = plan.source.modules[1].kind;
        if (kind == ObservationModuleKind::kPower) {
            result.product = WorkerProduct::kPower;
        } else if (kind == ObservationModuleKind::kStokes) {
            result.product = WorkerProduct::kStokes;
        } else {
            return Fail("resolved worker product must be power or stokes",
                        error);
        }
    }
    if (plan.source.modules.size() == 3U) {
        const ObservationModuleConfig& integration = plan.source.modules[2];
        if (integration.kind != ObservationModuleKind::kIntegrate) {
            return Fail("resolved worker third module must be integration",
                        error);
        }
        result.integration_enabled = true;
        result.integration_length = integration.integration_length;
        result.integration_operation = integration.integration_operation;
    }

    WorkerBlockGeometry computed;
    if (!ComputeWorkerBlockGeometry(result, &computed, error)) return false;
    if (computed.input_block_bytes != plan.compute_block_bytes ||
        computed.converted_block_bytes != plan.converted_block_bytes ||
        computed.beamformed_block_bytes != plan.beamformed_block_bytes ||
        computed.product_block_bytes != plan.product_block_bytes ||
        computed.output_ntime != plan.output_samples_per_block ||
        computed.output_block_bytes != plan.output_block_bytes) {
        return Fail("worker geometry conflicts with resolved observation plan",
                    error);
    }
    *config = result;
    *geometry = computed;
    return true;
}

bool LoadWorkerConfigFromResolvedPlan(
    const std::string& path,
    WorkerConfig* config,
    WorkerBlockGeometry* geometry,
    std::string* error) {
    ResolvedObservationPlan plan;
    if (!LoadResolvedObservationPlan(path, &plan, error)) return false;
    return BuildWorkerConfigFromResolvedPlan(plan, config, geometry, error);
}

bool ComputeWorkerBlockGeometry(const WorkerConfig& config,
                                WorkerBlockGeometry* geometry,
                                std::string* error) {
    if (!geometry) return Fail("worker block geometry output is null", error);
    if (config.nchan == 0 || config.nant == 0 || config.npol == 0) {
        return Fail("input_geometry F/A/P dimensions must be greater than zero",
                    error);
    }
    if (config.udp_payload_bytes == 0 || config.samples_per_udp == 0 ||
        config.udp_packets_per_antenna_per_block == 0) {
        return Fail("input_geometry UDP and block counts must be greater than "
                    "zero", error);
    }
    if (config.nbeam == 0) {
        return Fail("beamform.nbeam must be greater than zero", error);
    }
    if (config.product == WorkerProduct::kStokes && config.npol != 2) {
        return Fail("STOKES block geometry requires input_geometry.npol=2",
                    error);
    }
    if (config.integration_enabled) {
        if (config.product == WorkerProduct::kBeamformed) {
            return Fail(
                "time integration requires POWER or STOKES product", error);
        }
        if (config.integration_length == 0) {
            return Fail("integration length must be greater than zero", error);
        }
        if (config.integration_operation != "SUM" &&
            config.integration_operation != "MEAN") {
            return Fail("integration operation must be SUM or MEAN", error);
        }
    }

    WorkerBlockGeometry result = WorkerBlockGeometry();
    if (!CheckedMultiply(config.samples_per_udp,
                         config.udp_packets_per_antenna_per_block,
                         "block time samples", &result.ntime, error) ||
        !CheckedMultiply(config.udp_payload_bytes, config.nant,
                         "UDP all-antenna group bytes",
                         &result.udp_antenna_group_bytes, error)) {
        return false;
    }

    std::uint64_t elements = 0;
    if (!CheckedMultiply(config.nchan, config.npol,
                         "input frame F*P", &elements, error) ||
        !CheckedMultiply(elements, config.nant,
                         "input frame F*P*A", &elements, error) ||
        !CheckedMultiply(elements, UINT64_C(2),
                         "input CI8 frame bytes",
                         &result.input_frame_bytes, error) ||
        !CheckedMultiply(result.input_frame_bytes, result.ntime,
                         "input block bytes", &result.input_block_bytes,
                         error)) {
        return false;
    }
    if (result.input_block_bytes % result.udp_antenna_group_bytes != 0) {
        return Fail(
            "computed input block bytes must be an integer multiple of "
            "udp_payload_bytes * NANT", error);
    }
    result.udp_group_multiple =
        result.input_block_bytes / result.udp_antenna_group_bytes;

    if (!CheckedMultiply(config.nchan, config.npol,
                         "converted frame F*P", &elements, error) ||
        !CheckedMultiply(elements, config.nant,
                         "converted frame F*P*A", &elements, error) ||
        !CheckedMultiply(elements, sizeof(Complex32),
                         "converted CF32 frame bytes",
                         &result.converted_frame_bytes, error) ||
        !CheckedMultiply(result.converted_frame_bytes, result.ntime,
                         "converted block bytes",
                         &result.converted_block_bytes, error)) {
        return false;
    }

    if (!CheckedMultiply(config.nchan, config.npol,
                         "beamformed frame F*P", &elements, error) ||
        !CheckedMultiply(elements, config.nbeam,
                         "beamformed frame F*P*B", &elements, error) ||
        !CheckedMultiply(elements, sizeof(Complex32),
                         "beamformed CF32 frame bytes",
                         &result.beamformed_frame_bytes, error) ||
        !CheckedMultiply(result.beamformed_frame_bytes, result.ntime,
                         "beamformed block bytes",
                         &result.beamformed_block_bytes, error)) {
        return false;
    }

    switch (config.product) {
        case WorkerProduct::kBeamformed:
            result.output_frame_bytes = result.beamformed_frame_bytes;
            break;
        case WorkerProduct::kPower:
            if (!CheckedMultiply(config.nchan, config.npol,
                                 "power frame F*P", &elements, error) ||
                !CheckedMultiply(elements, config.nbeam,
                                 "power frame F*P*B", &elements, error) ||
                !CheckedMultiply(elements, sizeof(float),
                                 "power F32 frame bytes",
                                 &result.output_frame_bytes, error)) {
                return false;
            }
            break;
        case WorkerProduct::kStokes:
            if (!CheckedMultiply(config.nchan, config.nbeam,
                                 "Stokes frame F*B", &elements, error) ||
                !CheckedMultiply(elements, UINT64_C(4),
                                 "Stokes frame F*B*S", &elements, error) ||
                !CheckedMultiply(elements, sizeof(float),
                                 "Stokes F32 frame bytes",
                                 &result.output_frame_bytes, error)) {
                return false;
            }
            break;
        default:
            return Fail("unsupported worker output product", error);
    }
    if (!CheckedMultiply(result.output_frame_bytes, result.ntime,
                         "unintegrated product block bytes",
                         &result.product_block_bytes,
                         error)) {
        return false;
    }
    result.output_ntime = result.ntime;
    result.output_block_bytes = result.product_block_bytes;
    result.scratch_block_bytes =
        config.product == WorkerProduct::kBeamformed ?
            0 : result.beamformed_block_bytes;
    if (config.integration_enabled) {
        if (result.ntime % config.integration_length != 0) {
            return Fail(
                "block T must be divisible by integration length", error);
        }
        result.output_ntime = result.ntime / config.integration_length;
        result.output_block_bytes =
            result.product_block_bytes / config.integration_length;
        if (!CheckedAdd(result.beamformed_block_bytes,
                        result.product_block_bytes,
                        "integrated chain scratch block bytes",
                        &result.scratch_block_bytes, error)) {
            return false;
        }
    }
    *geometry = result;
    return true;
}

const char* WorkerProductName(WorkerProduct product) {
    switch (product) {
        case WorkerProduct::kBeamformed: return "BEAMFORMED";
        case WorkerProduct::kPower: return "POWER";
        case WorkerProduct::kStokes: return "STOKES";
    }
    return "UNKNOWN";
}

}  // namespace pipeline
}  // namespace rdma_dada
