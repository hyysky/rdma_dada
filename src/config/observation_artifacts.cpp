#include "rdma_dada/config/observation_artifacts.h"

#include "rdma_dada/config/observation_config.h"
#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/pipeline_config.h"
#include "rdma_dada/config/resolved_plan_json.h"
#include "rdma_dada/config/sha256.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_config.h"
#include "rdma_dada/modules/vdif_unpack/vdif_unpack_header.h"
#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
#include "rdma_dada/pipeline/ascii_metadata.h"
#include "rdma_dada/pipeline/dada_header_builder.h"
#include "rdma_dada/pipeline/module_chain.h"
#include "rdma_dada/pipeline/worker_config.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/stdio.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace rdma_dada {
namespace {

const std::uint64_t kDadaHeaderBytes = 4096U;

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

std::string SystemError(const std::string& operation,
                        const std::string& path) {
    return operation + " " + path + ": " + std::strerror(errno);
}

std::string EscapeJson(const std::string& value) {
    std::ostringstream output;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const unsigned char current =
            static_cast<unsigned char>(value[index]);
        switch (current) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
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

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     const char* name, std::uint64_t* output,
                     std::string* error) {
    if (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Fail(std::string(name) + " exceeds uint64 range", error);
    }
    *output = left * right;
    return true;
}

std::string StationList(const std::vector<std::uint16_t>& stations) {
    std::ostringstream output;
    for (std::size_t index = 0; index < stations.size(); ++index) {
        if (index != 0U) output << ',';
        output << stations[index];
    }
    return output.str();
}

pipeline::Metadata BuildRawHeader(const ResolvedObservationPlan& plan,
                                  const PipelineConfig& config,
                                  const PipelineLayout& layout,
                                  std::string* error) {
    dada_header_t portable = dada_header_t();
    if (!BuildPipelineDadaHeader(config, layout, DataStage::kRaw, &portable,
                                 error)) {
        return pipeline::Metadata();
    }
    pipeline::Metadata header;
    header.SetString("HDR_VERSION", "1.0");
    header.SetUint64("HDR_SIZE", kDadaHeaderBytes);
    header.SetUint64("OBS_OFFSET", 0U);
    header.SetUint64("PIPELINE_VERSION", 1U);
    header.SetString("CONFIG_ID", plan.config_id);
    header.SetString("GEOMETRY_ID", plan.geometry_id);
    header.SetString("OBSERVATION_ID", plan.source.observation_id);
    header.SetString("TELESCOPE", plan.source.telescope);
    header.SetUint64("BANDWIDTH_HZ", plan.source.bandwidth_hz);
    header.SetUint64("CENTER_FREQUENCY_HZ",
                     plan.source.center_frequency_hz);
    header.SetString("DATA_STAGE", "RAW");
    header.SetString("ORDER", "TFP");
    header.SetString("UTC_START", plan.source.utc_start);
    header.SetDouble("MJD_START", portable.mjd);
    header.SetString("STATION_IDS", StationList(plan.source.station_ids));
    header.SetString("PACKET_FORMAT_ID", plan.wire.format_id);
    header.SetUint64("NANT", plan.nant);
    header.SetUint64("NCHAN", plan.source.nchan);
    header.SetUint64("NPOL", plan.source.npol);
    header.SetUint64("NBIT", 16U);
    header.SetString("SAMPLE_FORMAT", "CI8");
    header.SetString("SAMPLE_ENCODING", "TWOS_COMPLEMENT");
    header.SetString("COMPONENT_ORDER", "IQ");
    header.SetString("ENDIAN", "LITTLE");
    header.SetUint64("COMPONENT_NBIT", 8U);
    header.SetUint64("SAMPLE_NBIT", 16U);
    header.SetUint64("PKT_HEADER", plan.wire.application_header_bytes);
    header.SetUint64("PKT_DATA", plan.payload_bytes);
    header.SetUint64("PKT_NSAMP", plan.source.samples_per_packet);
    header.SetUint64("PKT_NPOL", plan.source.npol);
    header.SetUint64("PKT_NBIT", 16U);
    const double sample_interval_us =
        static_cast<double>(plan.source.sample_interval_ps) / 1000000.0;
    header.SetDouble("PKT_TSAMP", sample_interval_us);
    header.SetDouble("TSAMP", sample_interval_us);
    header.SetUint64("RECORD_HEADER_BYTES", plan.wire.application_header_bytes);
    header.SetUint64("RECORD_BYTES", plan.raw_record_bytes);
    header.SetUint64("RESOLUTION", plan.raw_record_bytes);
    header.SetUint64("BLOCK_BYTES", plan.raw_block_bytes);
    header.SetUint64("RING_BYTES", plan.raw_ring_bytes);
    header.SetUint64("BYTES_PER_SECOND", plan.payload_bytes_per_second);
    header.SetUint64("RAW_BYTES_PER_SECOND", plan.raw_bytes_per_second);
    header.SetUint64("FILE_SIZE", plan.raw_file_bytes);
    header.SetUint64("GROUP_PERIOD_PS", plan.group_period_ps);
    header.SetUint64("GROUP_START_REFERENCE_EPOCH",
                     plan.group_start_reference_epoch);
    header.SetUint64("GROUP_START_SECONDS", plan.group_start_seconds);
    header.SetUint64("GROUP_START_FRAME", plan.group_start_frame);
    header.SetUint64("EXPECTED_GROUPS", plan.expected_groups);
    header.SetUint64("FIRST_CHANNEL_ID", plan.source.first_channel_id);
    std::uint64_t transfer_records = 0;
    std::uint64_t transfer_bytes = 0;
    if (!CheckedMultiply(plan.expected_groups, plan.nant,
                         "raw transfer record count", &transfer_records,
                         error) ||
        !CheckedMultiply(transfer_records, plan.raw_record_bytes,
                         "raw transfer bytes", &transfer_bytes, error)) {
        return pipeline::Metadata();
    }
    header.SetUint64("TRANSFER_SIZE", transfer_bytes);
    (void)layout;
    return header;
}

void ApplyRequestedExecutionMetadata(
    const pipeline::WorkerConfig& requested,
    const std::string& memory,
    pipeline::Metadata* header) {
    header->SetString("EXECUTION_BACKEND", requested.execution_backend);
    header->SetString("MEMORY", memory);
    if (header->Has("COMPUTE_MODE")) {
        header->SetString("COMPUTE_MODE", requested.compute_mode);
    }
    if (requested.execution_backend == "CUDA") {
        header->SetUint64("CUDA_DEVICE",
                          static_cast<std::uint64_t>(requested.cuda_device));
    } else {
        header->Erase("CUDA_DEVICE");
    }
}

bool BuildProcessingHeaders(
    const ResolvedObservationPlan& plan,
    const pipeline::Metadata& unpacked_header,
    pipeline::Metadata* converted_header,
    pipeline::Metadata* beamformed_header,
    pipeline::Metadata* output_header,
    std::string* error) {
    if (plan.source.modules.empty()) return true;

    pipeline::WorkerConfig requested;
    pipeline::WorkerBlockGeometry geometry;
    if (!pipeline::BuildWorkerConfigFromResolvedPlan(
            plan, &requested, &geometry, error)) {
        return false;
    }
    pipeline::WorkerConfig planning = requested;
    planning.execution_backend = "CPU_REFERENCE";
    planning.cuda_device = -1;
    planning.compute_mode = "FP32";

    pipeline::StageParameters conversion_parameters;
    conversion_parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    conversion_parameters.SetDouble(
        "CONVERSION_SCALE", planning.conversion_scale);
    modules::complex_convert::ComplexConvertModule conversion;
    pipeline::StageStatus status = conversion.ConfigureHeader(
        unpacked_header, conversion_parameters, converted_header);
    if (!status.ok()) {
        return Fail("cannot derive converted header: " + status.message(),
                    error);
    }

    pipeline::WorkerConfig beam_only = planning;
    beam_only.product = pipeline::WorkerProduct::kBeamformed;
    beam_only.integration_enabled = false;
    beam_only.integration_length = 1U;
    beam_only.integration_operation = "SUM";
    pipeline::ModuleChain beam_chain;
    status = beam_chain.Configure(
        *converted_header, beam_only, beamformed_header);
    if (!status.ok()) {
        return Fail("cannot derive beamformed header: " + status.message(),
                    error);
    }

    pipeline::ModuleChain final_chain;
    status = final_chain.Configure(*converted_header, planning, output_header);
    if (!status.ok()) {
        return Fail("cannot derive output header: " + status.message(), error);
    }

    const std::string device_memory =
        requested.execution_backend == "CUDA" ? "CUDA_DEVICE" : "HOST";
    ApplyRequestedExecutionMetadata(
        requested, device_memory, converted_header);
    ApplyRequestedExecutionMetadata(
        requested, device_memory, beamformed_header);
    ApplyRequestedExecutionMetadata(requested, "HOST", output_header);
    std::string pipeline_modules;
    if (output_header->GetString("PIPELINE_MODULES", &pipeline_modules)) {
        output_header->SetString(
            "PIPELINE_MODULES", "complex_convert," + pipeline_modules);
    }
    output_header->SetUint64("INPUT_BLOCK_BYTES", geometry.input_block_bytes);
    output_header->SetUint64(
        "CONVERTED_BLOCK_BYTES", geometry.converted_block_bytes);
    output_header->SetDouble(
        "CONVERSION_SCALE", requested.conversion_scale);
    output_header->SetUint64("OUTPUT_BLOCK_BYTES", geometry.output_block_bytes);
    output_header->SetUint64("BLOCK_BYTES", geometry.output_block_bytes);
    output_header->SetUint64("RING_BYTES", plan.output_ring_bytes);
    return true;
}

std::string RingPlanJson(const ResolvedObservationPlan& plan) {
    std::ostringstream compute_key;
    compute_key << "0x" << std::hex << std::setw(4) << std::setfill('0')
                << plan.source.compute_key;
    std::ostringstream raw_key;
    raw_key << "0x" << std::hex << std::setw(4) << std::setfill('0')
                << plan.source.raw_key;
    std::ostringstream output_key;
    output_key << "0x" << std::hex << std::setw(4) << std::setfill('0')
               << plan.source.output_key;
    std::ostringstream output;
    output << '{'
           << "\"config_id\":" << Quote(plan.config_id) << ','
           << "\"geometry_id\":" << Quote(plan.geometry_id) << ','
           << "\"rings\":{"
           << "\"compute\":{"
           << "\"block_bytes\":" << plan.compute_block_bytes << ','
           << "\"blocks\":" << plan.source.compute_ring_blocks << ','
           << "\"header_bytes\":" << kDadaHeaderBytes << ','
           << "\"key\":" << Quote(compute_key.str()) << ','
           << "\"ring_bytes\":" << plan.compute_ring_bytes << "},";
    if (plan.output_block_bytes != 0U) {
        output << "\"output\":{"
               << "\"block_bytes\":" << plan.output_block_bytes << ','
               << "\"blocks\":" << plan.source.compute_ring_blocks << ','
               << "\"data_stage\":" << Quote(plan.output_data_stage) << ','
               << "\"header_bytes\":" << kDadaHeaderBytes << ','
               << "\"key\":" << Quote(output_key.str()) << ','
               << "\"order\":" << Quote(plan.output_order) << ','
               << "\"ring_bytes\":" << plan.output_ring_bytes << ','
               << "\"sample_format\":"
               << Quote(plan.output_sample_format) << "},";
    }
    output << "\"raw\":{"
           << "\"block_bytes\":" << plan.raw_block_bytes << ','
           << "\"blocks\":" << plan.source.raw_ring_blocks << ','
           << "\"header_bytes\":" << kDadaHeaderBytes << ','
           << "\"key\":" << Quote(raw_key.str()) << ','
           << "\"ring_bytes\":" << plan.raw_ring_bytes << "}},"
           << "\"schema_version\":1,"
           << "\"window\":{"
           << "\"groups\":" << plan.window_groups << ','
           << "\"payload_bytes\":" << plan.window_payload_bytes << ','
           << "\"validity_bytes\":" << plan.window_validity_bytes << "}"
           << "}\n";
    return output.str();
}

std::string ValidationReportJson(const ResolvedObservationPlan& plan) {
    std::ostringstream stage_headers;
    stage_headers << "[\"RAW\",\"UNPACKED\"";
    if (!plan.source.modules.empty()) {
        stage_headers << ",\"CONVERTED\",\"BEAMFORMED\","
                      << Quote(plan.output_data_stage);
    }
    stage_headers << ']';
    std::ostringstream output;
    output << '{'
           << "\"checks\":["
           << "{\"name\":\"observation_schema\",\"status\":\"PASS\"},"
           << "{\"name\":\"wire_contract\",\"status\":\"PASS\"},"
           << "{\"name\":\"checked_geometry\",\"status\":\"PASS\"},"
           << "{\"name\":\"identity_verification\",\"status\":\"PASS\"},"
           << "{\"name\":\"header_transform\",\"status\":\"PASS\"}],"
           << "\"config_id\":" << Quote(plan.config_id) << ','
           << "\"derived\":{"
           << "\"compute_block_bytes\":" << plan.compute_block_bytes << ','
           << "\"expected_groups\":" << plan.expected_groups << ','
           << "\"group_period_ps\":" << plan.group_period_ps << ','
           << "\"payload_bytes\":" << plan.payload_bytes << ','
           << "\"raw_block_bytes\":" << plan.raw_block_bytes << ','
           << "\"raw_record_bytes\":" << plan.raw_record_bytes << ','
           << "\"window_payload_bytes\":" << plan.window_payload_bytes
           << "},"
           << "\"geometry_id\":" << Quote(plan.geometry_id) << ','
           << "\"formulas\":{"
           << "\"compute_block_bytes\":"
           << Quote("groups_per_block*NANT*payload_bytes") << ','
           << "\"group_period_ps\":"
           << Quote("samples_per_packet*sample_interval_ps") << ','
           << "\"payload_bytes\":"
           << Quote("samples_per_packet*NCHAN*NPOL*2") << ','
           << "\"raw_block_bytes\":"
           << Quote("groups_per_block*NANT*(32+payload_bytes)") << ','
           << "\"window_payload_bytes\":"
           << Quote("window_blocks*compute_block_bytes") << "},"
           << "\"inputs\":{"
           << "\"observation\":" << Quote(plan.source.source_path) << ','
           << "\"wire_profile\":"
           << Quote(plan.source.wire_profile_path) << "},"
           << "\"observation_id\":" << Quote(plan.source.observation_id)
           << ',' << "\"schema_version\":1,"
           << "\"stage_headers\":" << stage_headers.str() << ','
           << "\"valid\":true"
           << "}\n";
    return output.str();
}

bool HeaderBytes(const pipeline::Metadata& metadata, std::string* bytes,
                 std::string* error) {
    std::vector<char> buffer(static_cast<std::size_t>(kDadaHeaderBytes), 0);
    if (!pipeline::SerializeAsciiMetadata(metadata, buffer.data(),
                                          kDadaHeaderBytes, error)) {
        return false;
    }
    bytes->assign(buffer.begin(), buffer.end());
    return true;
}

bool WriteAll(int descriptor, const std::string& contents,
              const std::string& path, std::string* error) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const ssize_t count = write(descriptor, contents.data() + offset,
                                    contents.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            return Fail(SystemError("cannot write", path), error);
        }
        offset += static_cast<std::size_t>(count);
    }
    if (fsync(descriptor) != 0) {
        return Fail(SystemError("cannot fsync", path), error);
    }
    return true;
}

bool WriteExclusiveFile(const std::string& path, const std::string& contents,
                        std::string* error) {
    const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL,
                                S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
    if (descriptor < 0) {
        return Fail(SystemError("cannot create", path), error);
    }
    const bool written = WriteAll(descriptor, contents, path, error);
    const int close_result = close(descriptor);
    if (!written) return false;
    if (close_result != 0) {
        return Fail(SystemError("cannot close", path), error);
    }
    return true;
}

std::string ParentPath(const std::string& path) {
    const std::string::size_type slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0U) return "/";
    return path.substr(0, slash);
}

std::string BaseName(const std::string& path) {
    const std::string::size_type slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1U);
}

void RemoveStaging(const std::string& directory,
                   const std::map<std::string, std::string>& files) {
    for (std::map<std::string, std::string>::const_iterator item =
             files.begin(); item != files.end(); ++item) {
        unlink((directory + "/" + item->first).c_str());
    }
    unlink((directory + "/MANIFEST.sha256").c_str());
    rmdir(directory.c_str());
}

bool PublishDirectoryNoReplace(const std::string& staging,
                               const std::string& destination,
                               std::string* error) {
#if defined(__APPLE__)
    if (renamex_np(staging.c_str(), destination.c_str(), RENAME_EXCL) == 0) {
        return true;
    }
    return Fail(SystemError("cannot publish artifact directory", destination),
                error);
#elif defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, staging.c_str(), AT_FDCWD,
                destination.c_str(), RENAME_NOREPLACE) == 0) {
        return true;
    }
    if (errno != ENOSYS && errno != EINVAL) {
        return Fail(SystemError("cannot publish artifact directory",
                                destination), error);
    }
#endif
    struct stat destination_status = {};
    if (lstat(destination.c_str(), &destination_status) == 0) {
        return Fail("output directory appeared during write: " + destination,
                    error);
    }
    if (errno != ENOENT || rename(staging.c_str(), destination.c_str()) != 0) {
        return Fail(SystemError("cannot publish artifact directory",
                                destination), error);
    }
    return true;
}

}  // namespace

bool BuildObservationArtifacts(const std::string& observation_path,
                               ObservationArtifacts* artifacts,
                               std::string* error) {
    if (!artifacts) return Fail("artifact output pointer is null", error);
    ObservationConfig observation;
    if (!LoadObservationConfig(observation_path, &observation, error)) {
        return false;
    }
    PacketFormatConfig wire;
    if (!LoadPacketFormatConfig(observation.wire_profile_path, &wire, error)) {
        return false;
    }
    ResolvedObservationPlan plan;
    if (!ResolveObservationPlan(observation, wire, &plan, error) ||
        !ComputeObservationIdentities(&plan, error)) {
        return false;
    }
    return BuildObservationArtifactsFromResolvedPlan(plan, artifacts, error);
}

bool BuildObservationArtifactsFromResolvedPlan(
    const ResolvedObservationPlan& plan,
    ObservationArtifacts* artifacts,
    std::string* error) {
    if (!artifacts) return Fail("artifact output pointer is null", error);
    ResolvedObservationPlan verified = plan;
    if (!ComputeObservationIdentities(&verified, error)) return false;
    if (verified.config_id != plan.config_id ||
        verified.geometry_id != plan.geometry_id) {
        return Fail("resolved plan identities are missing or stale", error);
    }
    PipelineConfig pipeline_config;
    PipelineLayout pipeline_layout;
    modules::vdif_unpack::VdifUnpackConfig unpack_config;
    modules::vdif_unpack::VdifUnpackLayout unpack_layout;
    if (!modules::vdif_unpack::BuildVdifUnpackRuntimeFromResolvedPlan(
            plan, &unpack_config, &pipeline_config, &pipeline_layout,
            &unpack_layout, error)) {
        return false;
    }

    pipeline::Metadata raw_header =
        BuildRawHeader(plan, pipeline_config, pipeline_layout, error);
    if (raw_header.Fields().empty()) return false;

    pipeline::Metadata unpacked_header;
    if (!modules::vdif_unpack::BuildVdifUnpackOutputHeader(
            raw_header, unpack_config, pipeline_config, pipeline_layout,
            unpack_layout, &unpacked_header, error)) {
        return false;
    }

    ObservationArtifacts result;
    result.plan = plan;
    result.raw_header = raw_header;
    result.unpacked_header = unpacked_header;
    if (!BuildProcessingHeaders(
            plan, unpacked_header, &result.converted_header,
            &result.beamformed_header, &result.output_header, error)) {
        return false;
    }
    if (!SerializeResolvedObservationPlan(plan, &result.resolved_plan_json,
                                          error)) {
        return false;
    }
    result.ring_plan_json = RingPlanJson(plan);
    result.validation_report_json = ValidationReportJson(plan);
    *artifacts = result;
    return true;
}

bool WriteObservationArtifacts(const ObservationArtifacts& artifacts,
                               const std::string& output_directory,
                               std::string* error) {
    if (output_directory.empty() || BaseName(output_directory).empty()) {
        return Fail("output directory must name a new directory", error);
    }
    struct stat destination_status = {};
    if (lstat(output_directory.c_str(), &destination_status) == 0) {
        return Fail("output directory already exists: " + output_directory,
                    error);
    }
    if (errno != ENOENT) {
        return Fail(SystemError("cannot inspect", output_directory), error);
    }

    std::string raw_header;
    std::string unpacked_header;
    if (!HeaderBytes(artifacts.raw_header, &raw_header, error) ||
        !HeaderBytes(artifacts.unpacked_header, &unpacked_header, error)) {
        return false;
    }
    std::map<std::string, std::string> files;
    files["raw.header"] = raw_header;
    files["resolved_observation.json"] = artifacts.resolved_plan_json;
    files["ring_plan.json"] = artifacts.ring_plan_json;
    files["unpacked.header"] = unpacked_header;
    if (!artifacts.converted_header.Fields().empty()) {
        std::string converted_header;
        std::string beamformed_header;
        std::string output_header;
        if (!HeaderBytes(artifacts.converted_header, &converted_header,
                         error) ||
            !HeaderBytes(artifacts.beamformed_header, &beamformed_header,
                         error) ||
            !HeaderBytes(artifacts.output_header, &output_header, error)) {
            return false;
        }
        files["converted.header"] = converted_header;
        files["beamformed.header"] = beamformed_header;
        files["output.header"] = output_header;
    }
    files["validation_report.json"] = artifacts.validation_report_json;

    std::ostringstream staging_name;
    staging_name << ParentPath(output_directory) << "/."
                 << BaseName(output_directory) << ".staging-" << getpid();
    const std::string staging = staging_name.str();
    if (mkdir(staging.c_str(), S_IRWXU | S_IRGRP | S_IXGRP |
                               S_IROTH | S_IXOTH) != 0) {
        return Fail(SystemError("cannot create staging directory", staging),
                    error);
    }

    for (std::map<std::string, std::string>::const_iterator item =
             files.begin(); item != files.end(); ++item) {
        if (!WriteExclusiveFile(staging + "/" + item->first, item->second,
                                error)) {
            RemoveStaging(staging, files);
            return false;
        }
    }
    std::ostringstream manifest;
    for (std::map<std::string, std::string>::const_iterator item =
             files.begin(); item != files.end(); ++item) {
        manifest << Sha256Hex(item->second.data(), item->second.size())
                 << "  " << item->first << '\n';
    }
    if (!WriteExclusiveFile(staging + "/MANIFEST.sha256", manifest.str(),
                            error)) {
        RemoveStaging(staging, files);
        return false;
    }
    const int directory_descriptor = open(staging.c_str(), O_RDONLY);
    if (directory_descriptor < 0 || fsync(directory_descriptor) != 0) {
        const std::string message =
            SystemError("cannot fsync staging directory", staging);
        if (directory_descriptor >= 0) close(directory_descriptor);
        RemoveStaging(staging, files);
        return Fail(message, error);
    }
    close(directory_descriptor);
    if (!PublishDirectoryNoReplace(staging, output_directory, error)) {
        const std::string message = error ? *error :
            "cannot publish artifact directory";
        RemoveStaging(staging, files);
        return Fail(message, error);
    }
    const int parent_descriptor = open(ParentPath(output_directory).c_str(),
                                       O_RDONLY);
    if (parent_descriptor >= 0) {
        fsync(parent_descriptor);
        close(parent_descriptor);
    }
    return true;
}

}  // namespace rdma_dada
