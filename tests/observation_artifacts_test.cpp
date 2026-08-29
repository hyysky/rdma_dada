#include "rdma_dada/config/observation_artifacts.h"
#include "rdma_dada/config/observation_config.h"
#include "rdma_dada/config/gpu_pipeline_budget.h"
#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/resolved_plan_json.h"

#include "rdma_dada/pipeline/ascii_metadata.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ExpectText(const rdma_dada::pipeline::Metadata& metadata,
                const std::string& key, const std::string& expected) {
    std::string actual;
    Expect(metadata.GetString(key, &actual) && actual == expected,
           key + " mismatch");
}

void ExpectUint(const rdma_dada::pipeline::Metadata& metadata,
                const std::string& key, std::uint64_t expected) {
    std::uint64_t actual = 0;
    const bool found = metadata.GetUint64(key, &actual);
    std::ostringstream message;
    message << key << " mismatch: expected " << expected << ", got ";
    if (found) message << actual;
    else message << "<missing>";
    Expect(found && actual == expected, message.str());
}

void ExpectDouble(const rdma_dada::pipeline::Metadata& metadata,
                  const std::string& key, double expected) {
    double actual = 0.0;
    Expect(metadata.GetDouble(key, &actual) && actual == expected,
           key + " mismatch");
}

std::string ReadFile(const std::string& path) {
    std::ifstream input(path.c_str(), std::ios::binary);
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

std::string WriteWeights() {
    std::ostringstream path;
    path << "/tmp/rdma_dada_artifact_weights_" << getpid() << ".npy";
    std::string header =
        "{'descr': '|i1', 'fortran_order': False, "
        "'shape': (2, 2, 2, 3, 2), }";
    const std::size_t padding = 16U - ((10U + header.size() + 1U) % 16U);
    header.append(padding, ' ');
    header.push_back('\n');
    const unsigned char prefix[] = {
        0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<unsigned char>(header.size() & 0xffU),
        static_cast<unsigned char>((header.size() >> 8U) & 0xffU)
    };
    std::ofstream output(path.str().c_str(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    const std::string payload(48U, '\0');
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    return path.str();
}

void RemoveArtifacts(const std::string& directory) {
    static const char* const files[] = {
        "beamformed.header", "converted.header", "output.header",
        "resolved_observation.json", "ring_plan.json", "raw.header",
        "unpacked.header", "validation_report.json", "MANIFEST.sha256"
    };
    for (std::size_t index = 0; index < sizeof(files) / sizeof(files[0]);
         ++index) {
        std::remove((directory + "/" + files[index]).c_str());
    }
    rmdir(directory.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: observation_artifacts_test OBSERVATION\n";
        return 2;
    }

    rdma_dada::ObservationArtifacts artifacts;
    std::string error;
    Expect(rdma_dada::BuildObservationArtifacts(argv[1], &artifacts, &error),
           "artifact build: " + error);
    if (failures != 0) return 1;

    const rdma_dada::pipeline::Metadata& raw = artifacts.raw_header;
    ExpectText(raw, "CONFIG_ID", artifacts.plan.config_id);
    ExpectUint(raw, "HDR_SIZE", 4096U);
    ExpectText(raw, "GEOMETRY_ID", artifacts.plan.geometry_id);
    ExpectText(raw, "OBSERVATION_ID", "ca-functional-v1");
    ExpectText(raw, "TELESCOPE", "CA");
    ExpectText(raw, "DATA_STAGE", "RAW");
    ExpectText(raw, "ORDER", "TFP");
    ExpectText(raw, "UTC_START", "2026-08-08-00:00:00");
    ExpectDouble(raw, "MJD_START", 61260.0);
    Expect(!raw.Has("STATION_IDS"),
           "ring headers do not duplicate the resolved Station map");
    ExpectUint(raw, "BANDWIDTH_HZ", 300000000U);
    ExpectUint(raw, "CENTER_FREQUENCY_HZ", 1250000000U);
    ExpectUint(raw, "NANT", 2U);
    ExpectUint(raw, "NCHAN", 2U);
    ExpectUint(raw, "NPOL", 2U);
    ExpectUint(raw, "PKT_HEADER", 32U);
    ExpectUint(raw, "PKT_DATA", 4096U);
    ExpectUint(raw, "PKT_NSAMP", 512U);
    ExpectUint(raw, "RECORD_HEADER_BYTES", 32U);
    ExpectUint(raw, "RECORD_BYTES", 4128U);
    ExpectUint(raw, "RESOLUTION", 4128U);
    ExpectUint(raw, "BLOCK_BYTES", 8454144U);
    ExpectUint(raw, "RING_BYTES", 67633152U);
    ExpectUint(raw, "BYTES_PER_SECOND", 16000000U);
    ExpectUint(raw, "RAW_BYTES_PER_SECOND", 16125000U);
    ExpectUint(raw, "FILE_SIZE", 0U);
    ExpectUint(raw, "GROUP_PERIOD_PS", 512000000U);
    ExpectUint(raw, "GROUP_START_REFERENCE_EPOCH", 53U);
    ExpectUint(raw, "GROUP_START_SECONDS", 3283200U);
    ExpectUint(raw, "GROUP_START_FRAME", 0U);
    ExpectUint(raw, "EXPECTED_GROUPS", 15141U);

    const rdma_dada::pipeline::Metadata& unpacked = artifacts.unpacked_header;
    ExpectUint(unpacked, "HDR_SIZE", 4096U);
    ExpectText(unpacked, "CONFIG_ID", artifacts.plan.config_id);
    ExpectText(unpacked, "GEOMETRY_ID", artifacts.plan.geometry_id);
    ExpectText(unpacked, "TELESCOPE", "CA");
    ExpectText(unpacked, "DATA_STAGE", "UNPACKED");
    ExpectText(unpacked, "ORDER", "ATFP");
    ExpectUint(unpacked, "RECORD_HEADER_BYTES", 0U);
    ExpectUint(unpacked, "RECORD_BYTES", 8388608U);
    ExpectUint(unpacked, "RESOLUTION", 16U);
    ExpectUint(unpacked, "BLOCK_NTIME", 524288U);
    ExpectUint(unpacked, "OUTPUT_BLOCK_BYTES", 8388608U);
    ExpectUint(unpacked, "BLOCK_BYTES", 8388608U);
    ExpectUint(unpacked, "RING_BYTES", 67108864U);
    ExpectUint(unpacked, "TRANSFER_SIZE", 124035072U);

    Expect(artifacts.resolved_plan_json.find(artifacts.plan.config_id) !=
               std::string::npos,
           "resolved JSON contains CONFIG_ID");
    Expect(artifacts.ring_plan_json.find(
               "\"raw\":{\"block_bytes\":8454144") !=
               std::string::npos,
           "ring plan contains raw block geometry");
    Expect(artifacts.validation_report_json.find("\"valid\":true") !=
               std::string::npos,
           "validation report records success");
    Expect(artifacts.validation_report_json.find(
               "\"gpu_pipeline_budget\":{") == std::string::npos,
           "observation without GPU modules omits the GPU budget");
    Expect(artifacts.validation_report_json.find("\"formulas\":") !=
               std::string::npos,
           "validation report records formulas");
    Expect(artifacts.validation_report_json.find("\"stage_headers\":[\"RAW\",\"UNPACKED\"]") !=
               std::string::npos,
           "validation report records generated stages");

    rdma_dada::ObservationConfig processing_source = artifacts.plan.source;
    const std::string weights = WriteWeights();
    rdma_dada::ObservationModuleConfig beamform =
        rdma_dada::ObservationModuleConfig();
    beamform.kind = rdma_dada::ObservationModuleKind::kBeamform;
    beamform.weights_file = weights;
    beamform.weights_order = "FPAB2";
    beamform.weights_id = "artifact-test";
    beamform.weights_scale = "0.0078125";
    beamform.compute_mode = "FP32";
    processing_source.modules.push_back(beamform);
    rdma_dada::ObservationModuleConfig power =
        rdma_dada::ObservationModuleConfig();
    power.kind = rdma_dada::ObservationModuleKind::kPower;
    processing_source.modules.push_back(power);
    rdma_dada::ObservationModuleConfig integration =
        rdma_dada::ObservationModuleConfig();
    integration.kind = rdma_dada::ObservationModuleKind::kIntegrate;
    integration.integration_length = 128U;
    integration.integration_operation = "MEAN";
    processing_source.modules.push_back(integration);
    rdma_dada::ResolvedObservationPlan processing_plan;
    error.clear();
    Expect(rdma_dada::ResolveObservationPlan(
               processing_source, artifacts.plan.wire, &processing_plan,
               &error) &&
               rdma_dada::ComputeObservationIdentities(
                   &processing_plan, &error),
           "resolve processing artifact geometry: " + error);
    rdma_dada::ObservationArtifacts processing_artifacts;
    error.clear();
    Expect(rdma_dada::BuildObservationArtifactsFromResolvedPlan(
               processing_plan, &processing_artifacts, &error),
           "build processing ring artifact: " + error);
    Expect(processing_artifacts.ring_plan_json.find(
               "\"output\":{\"block_bytes\":196608,\"blocks\":8") !=
               std::string::npos &&
               processing_artifacts.ring_plan_json.find(
                   "\"key\":\"0x00d6\"") != std::string::npos &&
               processing_artifacts.ring_plan_json.find(
                   "\"ring_bytes\":1572864") != std::string::npos,
           "ring plan derives one output ring from compute ring and modules");
    ExpectText(processing_artifacts.converted_header,
               "DATA_STAGE", "CONVERTED");
    ExpectText(processing_artifacts.converted_header, "ORDER", "TFPA");
    ExpectText(processing_artifacts.converted_header,
               "SAMPLE_FORMAT", "CF32");
    ExpectUint(processing_artifacts.converted_header,
               "OUTPUT_BLOCK_BYTES", 33554432U);
    ExpectText(processing_artifacts.beamformed_header,
               "DATA_STAGE", "BEAMFORMED");
    ExpectText(processing_artifacts.beamformed_header, "ORDER", "TFPB");
    ExpectUint(processing_artifacts.beamformed_header, "NBEAM", 3U);
    ExpectUint(processing_artifacts.beamformed_header,
               "OUTPUT_BLOCK_BYTES", 50331648U);
    ExpectText(processing_artifacts.output_header,
               "DATA_STAGE", "POWER_INTEGRATED");
    ExpectText(processing_artifacts.output_header, "ORDER", "TFPB");
    ExpectText(processing_artifacts.output_header, "SAMPLE_FORMAT", "F32");
    ExpectUint(processing_artifacts.output_header, "BLOCK_NTIME", 4096U);
    ExpectUint(processing_artifacts.output_header,
               "OUTPUT_BLOCK_BYTES", 196608U);
    Expect(processing_artifacts.validation_report_json.find(
               "\"stage_headers\":[\"RAW\",\"UNPACKED\",\"CONVERTED\","
               "\"BEAMFORMED\",\"POWER_INTEGRATED\"]") !=
               std::string::npos,
           "processing validation report lists every generated stage");
    Expect(processing_artifacts.validation_report_json.find(
               "\"gpu_pipeline_budget\":{") != std::string::npos,
           "processing validation report contains GPU pipeline budget");
    Expect(processing_artifacts.validation_report_json.find(
               "\"block_interval_ns\":524288000") != std::string::npos,
           "GPU budget derives the compute block arrival interval");
    Expect(processing_artifacts.validation_report_json.find(
               "\"service_deadline_ns\":419430400") != std::string::npos,
           "GPU budget reserves twenty percent of the block deadline");
    Expect(processing_artifacts.validation_report_json.find(
               "\"device_input_bytes\":8388608") != std::string::npos &&
               processing_artifacts.validation_report_json.find(
                   "\"device_converted_bytes\":33554432") !=
                   std::string::npos &&
               processing_artifacts.validation_report_json.find(
                   "\"device_scratch_bytes\":75497472") !=
                   std::string::npos &&
               processing_artifacts.validation_report_json.find(
                   "\"device_output_bytes\":196608") !=
                   std::string::npos,
           "GPU budget matches pipeline_worker device allocations");
    Expect(processing_artifacts.validation_report_json.find(
               "\"device_weight_bytes\":192") != std::string::npos &&
               processing_artifacts.validation_report_json.find(
                   "\"planned_device_bytes\":117637312") !=
                   std::string::npos &&
               processing_artifacts.validation_report_json.find(
                   "\"recommended_free_device_bytes\":141164775") !=
                   std::string::npos,
           "GPU budget includes converted resident weights and memory reserve");
    Expect(processing_artifacts.validation_report_json.find(
               "\"required_h2d_bytes_per_second\":20000000") !=
               std::string::npos &&
               processing_artifacts.validation_report_json.find(
                   "\"required_d2h_bytes_per_second\":468750") !=
                   std::string::npos,
           "GPU budget converts block transfers into reserved service rates");
    Expect(processing_artifacts.validation_report_json.find(
               "\"host_device_transfer_bytes_per_block\":8585216") !=
               std::string::npos &&
               processing_artifacts.validation_report_json.find(
                   "\"required_combined_host_device_bytes_per_second\":"
                   "20468750") != std::string::npos,
           "GPU budget reports total sequential H2D and D2H volume");

    rdma_dada::ObservationConfig staged_source = processing_source;
    staged_source.cuda_pipeline_mode =
        rdma_dada::CudaPipelineMode::kStagedPipeline;
    staged_source.cuda_inflight_blocks = 3U;
    rdma_dada::ResolvedObservationPlan staged_plan;
    error.clear();
    Expect(rdma_dada::ResolveObservationPlan(
               staged_source, artifacts.plan.wire, &staged_plan, &error) &&
               rdma_dada::ComputeObservationIdentities(&staged_plan, &error),
           "resolve staged GPU budget geometry: " + error);
    rdma_dada::ObservationArtifacts staged_artifacts;
    error.clear();
    Expect(rdma_dada::BuildObservationArtifactsFromResolvedPlan(
               staged_plan, &staged_artifacts, &error),
           "build staged GPU budget: " + error);
    Expect(staged_artifacts.validation_report_json.find(
               "\"execution_mode\":\"STAGED_PIPELINE\"") !=
               std::string::npos &&
               staged_artifacts.validation_report_json.find(
                   "\"inflight_blocks\":3") != std::string::npos,
           "GPU budget records staged execution contract");
    Expect(staged_artifacts.validation_report_json.find(
               "\"device_bytes_per_slot\":117637120") !=
               std::string::npos &&
               staged_artifacts.validation_report_json.find(
                   "\"slot_device_bytes_total\":352911360") !=
               std::string::npos &&
               staged_artifacts.validation_report_json.find(
                   "\"planned_device_bytes\":352911552") !=
               std::string::npos,
           "staged GPU budget multiplies private device buffers by slots");
    Expect(staged_artifacts.validation_report_json.find(
               "\"pinned_input_bytes\":0") !=
               std::string::npos &&
               staged_artifacts.validation_report_json.find(
                   "\"pinned_output_bytes\":589824") !=
               std::string::npos &&
               staged_artifacts.validation_report_json.find(
                   "\"planned_pinned_host_bytes\":589824") !=
               std::string::npos,
           "staged GPU budget accounts only for output pinned slots");

    rdma_dada::ObservationArtifactOptions performance_options;
    performance_options.budget_target_payload_bits_per_second =
        UINT64_C(30000000000);
    rdma_dada::ObservationArtifacts performance_artifacts;
    error.clear();
    Expect(rdma_dada::BuildObservationArtifactsFromResolvedPlanWithOptions(
               processing_plan, performance_options, &performance_artifacts,
               &error),
           "build 30 Gbps performance budget: " + error);
    Expect(performance_artifacts.validation_report_json.find(
               "\"observation_payload_bits_per_second\":128000000") !=
               std::string::npos &&
               performance_artifacts.validation_report_json.find(
                   "\"budget_target_payload_bits_per_second\":30000000000") !=
                   std::string::npos &&
               performance_artifacts.validation_report_json.find(
                   "\"rate_source\":\"PERFORMANCE_OVERRIDE\"") !=
                   std::string::npos,
           "performance budget preserves observation and override rates");
    Expect(performance_artifacts.validation_report_json.find(
               "\"block_interval_ns\":2236962") != std::string::npos &&
               performance_artifacts.validation_report_json.find(
                   "\"service_deadline_ns\":1789569") !=
                   std::string::npos &&
               performance_artifacts.validation_report_json.find(
                   "\"required_h2d_bytes_per_second\":4687501852") !=
                   std::string::npos,
           "30 Gbps budget applies the twenty-percent service reserve");

    rdma_dada::ObservationConfig beam_source = processing_source;
    beam_source.modules.resize(1U);
    rdma_dada::ResolvedObservationPlan beam_plan;
    error.clear();
    Expect(rdma_dada::ResolveObservationPlan(
               beam_source, artifacts.plan.wire, &beam_plan, &error) &&
               rdma_dada::ComputeObservationIdentities(&beam_plan, &error),
           "resolve beam-only GPU budget geometry: " + error);
    rdma_dada::ObservationArtifacts beam_artifacts;
    error.clear();
    Expect(rdma_dada::BuildObservationArtifactsFromResolvedPlan(
               beam_plan, &beam_artifacts, &error),
           "build beam-only GPU budget: " + error);
    Expect(beam_artifacts.validation_report_json.find(
               "\"device_scratch_bytes\":0") != std::string::npos &&
               beam_artifacts.validation_report_json.find(
                   "\"planned_device_bytes\":92274880") !=
                   std::string::npos &&
               beam_artifacts.validation_report_json.find(
                   "\"recommended_free_device_bytes\":110729856") !=
                   std::string::npos &&
               beam_artifacts.validation_report_json.find(
                   "\"required_d2h_bytes_per_second\":120000000") !=
                   std::string::npos,
           "beam-only budget matches its output allocation and rate");

    rdma_dada::ObservationConfig power_source = processing_source;
    power_source.modules.resize(2U);
    rdma_dada::ResolvedObservationPlan power_plan;
    error.clear();
    Expect(rdma_dada::ResolveObservationPlan(
               power_source, artifacts.plan.wire, &power_plan, &error) &&
               rdma_dada::ComputeObservationIdentities(&power_plan, &error),
           "resolve power GPU budget geometry: " + error);
    rdma_dada::ObservationArtifacts power_artifacts;
    error.clear();
    Expect(rdma_dada::BuildObservationArtifactsFromResolvedPlan(
               power_plan, &power_artifacts, &error),
           "build power GPU budget: " + error);
    Expect(power_artifacts.validation_report_json.find(
               "\"device_scratch_bytes\":50331648") != std::string::npos &&
               power_artifacts.validation_report_json.find(
                   "\"planned_device_bytes\":117440704") !=
                   std::string::npos &&
               power_artifacts.validation_report_json.find(
                   "\"recommended_free_device_bytes\":140928845") !=
                   std::string::npos &&
               power_artifacts.validation_report_json.find(
                   "\"required_d2h_bytes_per_second\":60000000") !=
                   std::string::npos,
           "power budget includes beam scratch and F32 output");

    rdma_dada::ObservationConfig stokes_source = power_source;
    stokes_source.modules[1].kind = rdma_dada::ObservationModuleKind::kStokes;
    rdma_dada::ResolvedObservationPlan stokes_plan;
    error.clear();
    Expect(rdma_dada::ResolveObservationPlan(
               stokes_source, artifacts.plan.wire, &stokes_plan, &error) &&
               rdma_dada::ComputeObservationIdentities(&stokes_plan, &error),
           "resolve Stokes GPU budget geometry: " + error);
    rdma_dada::GpuPipelineBudget stokes_budget =
        rdma_dada::GpuPipelineBudget();
    error.clear();
    Expect(rdma_dada::ComputeGpuPipelineBudget(
               stokes_plan, &stokes_budget, &error),
           "compute Stokes GPU budget: " + error);
    Expect(stokes_budget.device_scratch_bytes == 50331648U &&
               stokes_budget.device_output_bytes == 50331648U &&
               stokes_budget.device_weight_bytes == 192U &&
               stokes_budget.planned_device_bytes == 142606528U &&
               stokes_budget.recommended_free_device_bytes == 171127834U &&
               stokes_budget.required_d2h_bytes_per_second == 120000000U,
           "Stokes budget includes TFBS output and twenty-percent reserve");

    rdma_dada::GpuPipelineBudget invalid_budget =
        rdma_dada::GpuPipelineBudget();
    error.clear();
    Expect(!rdma_dada::ComputeGpuPipelineBudget(
               artifacts.plan, &invalid_budget, &error) &&
               error.find("requires a processing module chain") !=
                   std::string::npos,
           "GPU budget rejects a plan without processing modules");
    rdma_dada::ResolvedObservationPlan overflow_plan = beam_plan;
    overflow_plan.compute_block_bytes = UINT64_MAX;
    overflow_plan.payload_bytes_per_second = 1U;
    error.clear();
    Expect(!rdma_dada::ComputeGpuPipelineBudget(
               overflow_plan, &invalid_budget, &error) &&
               error.find("exceeds uint64 range") != std::string::npos,
           "GPU budget rejects arithmetic overflow");


    std::ostringstream processing_directory;
    processing_directory << "/tmp/rdma_dada_processing_artifacts_" << getpid();
    RemoveArtifacts(processing_directory.str());
    error.clear();
    Expect(rdma_dada::WriteObservationArtifacts(
               processing_artifacts, processing_directory.str(), &error),
           "write processing stage headers: " + error);
    struct stat processing_status = {};
    Expect(stat((processing_directory.str() + "/converted.header").c_str(),
                &processing_status) == 0 &&
               processing_status.st_size == 4096,
           "converted.header is one DADA header block");
    Expect(stat((processing_directory.str() + "/beamformed.header").c_str(),
                &processing_status) == 0 &&
               processing_status.st_size == 4096,
           "beamformed.header is one DADA header block");
    Expect(stat((processing_directory.str() + "/output.header").c_str(),
                &processing_status) == 0 &&
               processing_status.st_size == 4096,
           "output.header is one DADA header block");
    Expect(ReadFile(processing_directory.str() + "/MANIFEST.sha256").find(
               "  output.header\n") != std::string::npos,
           "manifest covers final output header");
    RemoveArtifacts(processing_directory.str());
    std::remove(weights.c_str());

    std::ostringstream directory;
    directory << "/tmp/rdma_dada_artifacts_" << getpid();
    RemoveArtifacts(directory.str());
    error.clear();
    Expect(rdma_dada::WriteObservationArtifacts(
               artifacts, directory.str(), &error),
           "atomic artifact write: " + error);
    struct stat status = {};
    Expect(stat((directory.str() + "/raw.header").c_str(), &status) == 0 &&
               status.st_size == 4096,
           "raw.header is one DADA header block");
    Expect(stat((directory.str() + "/unpacked.header").c_str(), &status) == 0 &&
               status.st_size == 4096,
           "unpacked.header is one DADA header block");
    Expect(ReadFile(directory.str() + "/MANIFEST.sha256").find(
               "  resolved_observation.json\n") != std::string::npos,
           "manifest uses sha256sum-compatible spacing");

    std::vector<char> raw_bytes(4096U, 0);
    const std::string raw_file = ReadFile(directory.str() + "/raw.header");
    Expect(raw_file.size() == raw_bytes.size(), "raw header read size");
    if (raw_file.size() == raw_bytes.size()) {
        std::copy(raw_file.begin(), raw_file.end(), raw_bytes.begin());
        rdma_dada::pipeline::Metadata parsed;
        error.clear();
        Expect(rdma_dada::pipeline::ParseAsciiMetadata(
                   raw_bytes.data(), raw_bytes.size(), &parsed, &error),
               "written raw header parses: " + error);
        ExpectText(parsed, "CONFIG_ID", artifacts.plan.config_id);
    }

    error.clear();
    Expect(!rdma_dada::WriteObservationArtifacts(
               artifacts, directory.str(), &error),
           "existing artifact directory is never overwritten");
    RemoveArtifacts(directory.str());

    if (failures != 0) return 1;
    std::cout << "observation_artifacts_test passed\n";
    return 0;
}
