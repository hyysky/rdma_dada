#include "rdma_dada/pipeline/ascii_metadata.h"
#include "rdma_dada/pipeline/complex32.h"
#include "rdma_dada/pipeline/module_chain.h"
#include "rdma_dada/pipeline/worker_config.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ExpectNear(float actual, float expected, const std::string& message) {
    if (std::fabs(actual - expected) > 1.0e-6f) {
        std::cerr << "FAIL: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

bool EndsWith(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

bool WriteWeights(const std::string& path) {
    std::string header =
        "{'descr': '|i1', 'fortran_order': False, "
        "'shape': (1, 2, 2, 1, 2), }";
    while ((10 + header.size() + 1) % 16 != 0) header.push_back(' ');
    header.push_back('\n');

    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const unsigned char prefix[] = {
        0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<unsigned char>(header.size() & 0xff),
        static_cast<unsigned char>((header.size() >> 8) & 0xff)
    };
    const std::int8_t weights[] = {
        1, 0, 1, 0,
        1, 0, 1, 0
    };
    output.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(weights), sizeof(weights));
    return output.good();
}

bool WriteLegacyWorkerConfig(const std::string& path,
                             const std::string& weights_path) {
    std::ofstream output(path.c_str(), std::ios::trunc);
    if (!output) return false;
    output
        << "{\n"
        << "  \"schema_version\": 1,\n"
        << "  \"rings\": {\"input_key\": \"dada\", "
           "\"output_key\": \"dadb\"},\n"
        << "  \"execution\": {\"backend\": \"CPU_REFERENCE\", "
           "\"cuda_device\": 0, \"run_once\": true},\n"
        << "  \"input_geometry\": {\"nchan\": 1, \"nant\": 2, "
           "\"npol\": 2, \"udp_payload_bytes\": 4, "
           "\"samples_per_udp\": 1, "
           "\"udp_packets_per_antenna_per_block\": 1},\n"
        << "  \"beamform\": {\"weights_file\": \"" << weights_path
        << "\", \"weights_order\": \"FPAB2\", "
           "\"weights_id\": \"legacy-v1\", \"weights_scale\": 1.0, "
           "\"nbeam\": 1, \"compute_mode\": \"FP32\"},\n"
        << "  \"output\": {\"product\": \"POWER\"}\n"
        << "}\n";
    return output.good();
}

rdma_dada::pipeline::Metadata MakeInputHeader() {
    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "CONVERTED");
    header.SetString("ORDER", "TFPA");
    header.SetString("SAMPLE_FORMAT", "CF32");
    header.SetString("MEMORY", "HOST");
    header.SetString("POL_LABELS", "X,Y");
    header.SetString("UTC_START", "2026-08-04-00:00:00");
    header.SetString("SOURCE", "PSR J1234+5678");
    header.SetUint64("NCHAN", 1);
    header.SetUint64("NPOL", 2);
    header.SetUint64("NANT", 2);
    header.SetUint64("RESOLUTION", 32);
    header.SetUint64("BYTES_PER_SECOND", 3200);
    header.SetUint64("TRANSFER_SIZE", 64);
    header.SetUint64("FILE_SIZE", 64);
    header.SetUint64("OBS_OFFSET", 32);
    header.SetDouble("TSAMP", 1.0);
    return header;
}

rdma_dada::pipeline::WorkerConfig MakeConfig(const std::string& weights_path) {
    rdma_dada::pipeline::WorkerConfig config = {};
    config.input_key = 0xdada;
    config.output_key = 0xdadb;
    config.input_key_text = "dada";
    config.output_key_text = "dadb";
    config.execution_backend = "CPU_REFERENCE";
    config.cuda_device = 0;
    config.run_once = true;
    config.nchan = 1;
    config.nant = 2;
    config.npol = 2;
    config.udp_payload_bytes = 4;
    config.samples_per_udp = 1;
    config.udp_packets_per_antenna_per_block = 1;
    config.conversion_scale = 1.0;
    config.weights_file = weights_path;
    config.weights_order = "FPAB2";
    config.weights_id = "worker-test-v1";
    config.weights_scale = 1.0;
    config.nbeam = 1;
    config.compute_mode = "FP32";
    config.product = rdma_dada::pipeline::WorkerProduct::kBeamformed;
    config.integration_enabled = false;
    config.integration_length = 1;
    config.integration_operation = "MEAN";
    return config;
}

void TestConfigAndAsciiCodec(const std::string& config_path) {
    rdma_dada::pipeline::WorkerConfig config;
    std::string error;
    Expect(rdma_dada::pipeline::LoadWorkerConfig(
               config_path, &config, &error),
           "example worker JSON loads: " + error);
    Expect(config.input_key == 0xdada && config.output_key == 0xdadb,
           "hexadecimal ring keys are parsed");
    Expect(config.execution_backend == "CPU_REFERENCE" && config.run_once,
           "execution settings are parsed");
    Expect(std::fabs(config.conversion_scale - 0.0078125) < 1.0e-12,
           "mandatory integer-to-CF32 conversion scale is parsed");
    Expect(config.product == rdma_dada::pipeline::WorkerProduct::kPower,
           "output product is parsed");
    Expect(config.integration_enabled && config.integration_length == 128 &&
               config.integration_operation == "MEAN",
           "time integration settings are parsed from schema v2");
    Expect(EndsWith(config.weights_file,
                    "config/weights/beamform_weights.npy"),
           "relative weight path is resolved from the JSON directory");
    rdma_dada::pipeline::WorkerBlockGeometry geometry;
    Expect(rdma_dada::pipeline::ComputeWorkerBlockGeometry(
               config, &geometry, &error),
           "example worker block geometry computes: " + error);
    Expect(geometry.ntime == UINT64_C(2097152),
           "T is UDP samples times UDP packets per antenna per block");
    Expect(geometry.udp_antenna_group_bytes == 8192,
           "one UDP antenna group is payload bytes times A");
    Expect(geometry.input_frame_bytes == 16 &&
               geometry.input_block_bytes == UINT64_C(33554432),
           "CI8 ATFP ring input block is computed from F*A*P*T");
    Expect(geometry.converted_frame_bytes == 64 &&
               geometry.converted_block_bytes == UINT64_C(134217728),
           "converted TFPA CF32 block has an independent capacity");
    Expect(geometry.beamformed_block_bytes == UINT64_C(67108864) &&
               geometry.product_block_bytes == UINT64_C(33554432),
           "beamformed and unintegrated power blocks are derived from T");
    Expect(geometry.output_ntime == UINT64_C(16384) &&
               geometry.output_block_bytes == UINT64_C(262144),
           "integration shortens T and final output block by K");
    Expect(geometry.scratch_block_bytes == UINT64_C(100663296),
           "integrated power chain scratch holds beamformed and product blocks");

    const char input[] =
        "HDR_SIZE 4096\n"
        "SOURCE PSR J1234+5678\n"
        "NCHAN 16 # inline comment\n"
        "DATA\n";
    rdma_dada::pipeline::Metadata metadata;
    Expect(rdma_dada::pipeline::ParseAsciiMetadata(
               input, sizeof(input), &metadata, &error),
           "ASCII DADA header parses: " + error);
    std::string text;
    Expect(metadata.GetString("SOURCE", &text) && text == "PSR J1234+5678",
           "ASCII header values may contain spaces");
    char output[512] = {};
    Expect(rdma_dada::pipeline::SerializeAsciiMetadata(
               metadata, output, sizeof(output), &error),
           "metadata serializes into a NUL-padded header: " + error);
    rdma_dada::pipeline::Metadata roundtrip;
    Expect(rdma_dada::pipeline::ParseAsciiMetadata(
               output, sizeof(output), &roundtrip, &error),
           "serialized header parses again: " + error);
    std::uint64_t number = 0;
    Expect(roundtrip.GetUint64("HDR_SIZE", &number) && number == sizeof(output),
           "serializer publishes the actual output header capacity");
    const char duplicate[] = "NCHAN 1\nNCHAN 2\n";
    Expect(!rdma_dada::pipeline::ParseAsciiMetadata(
               duplicate, sizeof(duplicate), &roundtrip, &error),
           "duplicate header fields are rejected");
}

void TestLegacyConfigDefaults(const std::string& config_path) {
    rdma_dada::pipeline::WorkerConfig config;
    std::string error;
    Expect(rdma_dada::pipeline::LoadWorkerConfig(
               config_path, &config, &error),
           "schema v1 worker JSON remains readable: " + error);
    Expect(!config.integration_enabled && config.integration_length == 1 &&
               config.integration_operation == "MEAN" &&
               std::fabs(config.conversion_scale - 1.0) < 1.0e-12,
           "schema v1 keeps legacy integration and conversion defaults");
}

void TestModuleChain(const std::string& weights_path) {
    typedef rdma_dada::pipeline::Complex32 Complex32;
    const Complex32 input_data[] = {
        {1.0f, 2.0f}, {3.0f, 4.0f},
        {5.0f, 6.0f}, {7.0f, 8.0f}
    };
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 41,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    const rdma_dada::pipeline::Metadata input_header = MakeInputHeader();
    rdma_dada::pipeline::WorkerConfig config = MakeConfig(weights_path);
    rdma_dada::pipeline::ModuleChain chain;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        chain.Configure(input_header, config, &output_header);
    Expect(status.ok(), "beamformed chain configures: " + status.message());
    Expect(chain.plan().module_count == 1 &&
               chain.plan().input_frame_bytes == 32 &&
               chain.plan().beamformed_frame_bytes == 16 &&
               chain.plan().output_frame_bytes == 16,
           "beamformed chain frame plan is correct");
    Complex32 beamformed_data[2] = {};
    rdma_dada::pipeline::OutputBlock beamformed_output = {
        reinterpret_cast<std::uint8_t*>(beamformed_data),
        sizeof(beamformed_data), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = chain.ProcessBlock(
        input, &beamformed_output, NULL, 0, context);
    Expect(status.ok(), "beamformed chain processes one block");
    ExpectNear(beamformed_data[0].real, 4.0f, "beamformed P0 real");
    ExpectNear(beamformed_data[0].imag, 6.0f, "beamformed P0 imag");
    ExpectNear(beamformed_data[1].real, 12.0f, "beamformed P1 real");
    ExpectNear(beamformed_data[1].imag, 14.0f, "beamformed P1 imag");

    config.product = rdma_dada::pipeline::WorkerProduct::kPower;
    status = chain.Configure(input_header, config, &output_header);
    Expect(status.ok(), "beamform+power chain configures: " + status.message());
    std::uint64_t number = 0;
    std::string text;
    Expect(output_header.GetUint64("RESOLUTION", &number) && number == 8,
           "power output frame is 8 bytes");
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 800,
           "power output byte rate is scaled by frame ratio");
    Expect(output_header.GetUint64("TRANSFER_SIZE", &number) && number == 16,
           "power transfer size is scaled by frame ratio");
    Expect(output_header.GetUint64("OBS_OFFSET", &number) && number == 8,
           "power observation offset is scaled by frame ratio");
    Expect(output_header.GetString("SOURCE", &text) &&
               text == "PSR J1234+5678",
           "unknown observation metadata survives the module chain");
    Expect(output_header.GetString("MEMORY", &text) && text == "HOST",
           "published output ring memory is host");
    Expect(output_header.GetString("PIPELINE_MODULES", &text) &&
               text == "beamform,power",
           "output header records the selected module chain");
    Expect(output_header.GetUint64("BLOCK_NTIME", &number) && number == 1,
           "output header publishes configured block T");
    Expect(output_header.GetUint64("INPUT_BLOCK_BYTES", &number) &&
               number == 32,
           "output header publishes computed input block bytes");
    Expect(output_header.GetUint64("OUTPUT_BLOCK_BYTES", &number) &&
               number == 8,
           "output header publishes computed output block bytes");
    Complex32 scratch[2] = {};
    float power_data[2] = {};
    rdma_dada::pipeline::OutputBlock power_output = {
        reinterpret_cast<std::uint8_t*>(power_data), sizeof(power_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = chain.ProcessBlock(
        input, &power_output, reinterpret_cast<std::uint8_t*>(scratch),
        sizeof(scratch), context);
    Expect(status.ok(), "beamform+power processes one block");
    ExpectNear(power_data[0], 52.0f, "P0 power");
    ExpectNear(power_data[1], 340.0f, "P1 power");
    status = chain.ProcessBlock(
        input, &power_output, reinterpret_cast<std::uint8_t*>(scratch),
        sizeof(Complex32), context);
    Expect(!status.ok(), "undersized intermediate scratch is rejected");

    config.product = rdma_dada::pipeline::WorkerProduct::kStokes;
    status = chain.Configure(input_header, config, &output_header);
    Expect(status.ok(), "beamform+stokes chain configures: " + status.message());
    Expect(output_header.GetUint64("RESOLUTION", &number) && number == 16,
           "Stokes output frame is 16 bytes");
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 1600,
           "Stokes output byte rate is scaled by frame ratio");
    float stokes_data[4] = {};
    rdma_dada::pipeline::OutputBlock stokes_output = {
        reinterpret_cast<std::uint8_t*>(stokes_data), sizeof(stokes_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = chain.ProcessBlock(
        input, &stokes_output, reinterpret_cast<std::uint8_t*>(scratch),
        sizeof(scratch), context);
    Expect(status.ok(), "beamform+stokes processes one block");
    ExpectNear(stokes_data[0], 52.0f, "Stokes AA");
    ExpectNear(stokes_data[1], 340.0f, "Stokes BB");
    ExpectNear(stokes_data[2], 132.0f, "Stokes AB real");
    ExpectNear(stokes_data[3], 16.0f, "Stokes AB imaginary");

    std::uint64_t intermediate_bytes = 0;
    std::uint64_t output_bytes = 0;
    status = chain.PlanBlock(31, &intermediate_bytes, &output_bytes);
    Expect(!status.ok(), "partial input time frames are rejected");

    rdma_dada::pipeline::Metadata invalid_header = input_header;
    invalid_header.SetUint64("RESOLUTION", 16);
    status = chain.Configure(invalid_header, config, &output_header);
    Expect(!status.ok(), "input RESOLUTION must match TFPA geometry");

    invalid_header = input_header;
    invalid_header.SetUint64("NANT", 3);
    status = chain.Configure(invalid_header, config, &output_header);
    Expect(!status.ok(), "header F/A/P must match configured input geometry");

    rdma_dada::pipeline::WorkerConfig invalid_geometry = config;
    invalid_geometry.product = rdma_dada::pipeline::WorkerProduct::kPower;
    invalid_geometry.udp_payload_bytes = 15;
    std::string geometry_error;
    rdma_dada::pipeline::WorkerBlockGeometry invalid_plan;
    Expect(!rdma_dada::pipeline::ComputeWorkerBlockGeometry(
               invalid_geometry, &invalid_plan, &geometry_error),
           "input block must be divisible by UDP payload bytes times A");

    config.product = static_cast<rdma_dada::pipeline::WorkerProduct>(99);
    status = chain.Configure(input_header, config, &output_header);
    Expect(!status.ok(), "unknown output product enum is rejected");
}

void TestIntegratedModuleChain(const std::string& weights_path) {
    typedef rdma_dada::pipeline::Complex32 Complex32;
    const Complex32 input_data[] = {
        {1.0f, 0.0f}, {1.0f, 0.0f},
        {2.0f, 0.0f}, {2.0f, 0.0f},
        {2.0f, 0.0f}, {2.0f, 0.0f},
        {3.0f, 0.0f}, {3.0f, 0.0f},
        {3.0f, 0.0f}, {3.0f, 0.0f},
        {4.0f, 0.0f}, {4.0f, 0.0f},
        {4.0f, 0.0f}, {4.0f, 0.0f},
        {5.0f, 0.0f}, {5.0f, 0.0f}
    };
    rdma_dada::pipeline::Metadata input_header = MakeInputHeader();
    input_header.SetUint64("TRANSFER_SIZE", sizeof(input_data));
    input_header.SetUint64("FILE_SIZE", sizeof(input_data));
    input_header.SetUint64("OBS_OFFSET", 0);

    rdma_dada::pipeline::WorkerConfig config = MakeConfig(weights_path);
    config.product = rdma_dada::pipeline::WorkerProduct::kPower;
    config.udp_packets_per_antenna_per_block = 4;
    config.integration_enabled = true;
    config.integration_length = 2;
    config.integration_operation = "MEAN";

    rdma_dada::pipeline::ModuleChain chain;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        chain.Configure(input_header, config, &output_header);
    Expect(status.ok(),
           "beamform+power+integration chain configures: " +
               status.message());
    if (!status.ok()) return;

    std::string text;
    std::uint64_t number = 0;
    Expect(output_header.GetString("PIPELINE_MODULES", &text) &&
               text == "beamform,power,time_integrate",
           "output header records integration module");
    Expect(output_header.GetUint64("INTEGRATION_LENGTH", &number) &&
               number == 2,
           "chain publishes integration length");
    Expect(output_header.GetUint64("BLOCK_NTIME", &number) && number == 2,
           "chain publishes integrated output T");
    Expect(output_header.GetUint64("INPUT_BLOCK_NTIME", &number) &&
               number == 4,
           "chain preserves input block T");
    Expect(output_header.GetUint64("OUTPUT_BLOCK_BYTES", &number) &&
               number == 16,
           "chain publishes integrated output block bytes");

    std::uint64_t scratch_bytes = 0;
    std::uint64_t output_bytes = 0;
    status = chain.PlanBlock(
        sizeof(input_data), &scratch_bytes, &output_bytes);
    Expect(status.ok(), "integrated chain plans one input block");
    Expect(scratch_bytes == 96,
           "integrated chain scratch holds beamformed and power blocks");
    Expect(output_bytes == 16, "integrated chain output is reduced by K");

    std::uint8_t scratch[96] = {};
    float output_data[4] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 121,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(output_data), sizeof(output_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = chain.ProcessBlock(
        input, &output, scratch, sizeof(scratch), context);
    Expect(status.ok(), "integrated chain processes one block");
    if (!status.ok()) std::cerr << status.message() << '\n';
    const float expected[] = {10.0f, 26.0f, 50.0f, 82.0f};
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(output_data[i], expected[i],
                   "known beamform+power+mean value");
    }
    Expect(output.size == sizeof(output_data),
           "integrated chain reports final output size");
    Expect(output.sequence == 121,
           "integrated chain preserves block sequence");

    rdma_dada::pipeline::WorkerConfig invalid_config = config;
    invalid_config.udp_packets_per_antenna_per_block = 3;
    rdma_dada::pipeline::WorkerBlockGeometry invalid_geometry;
    std::string geometry_error;
    Expect(!rdma_dada::pipeline::ComputeWorkerBlockGeometry(
               invalid_config, &invalid_geometry, &geometry_error),
           "worker geometry rejects T not divisible by integration length");

    invalid_config = config;
    invalid_config.product = rdma_dada::pipeline::WorkerProduct::kBeamformed;
    Expect(!rdma_dada::pipeline::ComputeWorkerBlockGeometry(
               invalid_config, &invalid_geometry, &geometry_error),
           "worker geometry rejects integration before power or Stokes");
}

void TestIntegratedStokesChain(const std::string& weights_path) {
    typedef rdma_dada::pipeline::Complex32 Complex32;
    const Complex32 input_data[] = {
        {1.0f, 0.0f}, {1.0f, 0.0f},
        {2.0f, 0.0f}, {2.0f, 0.0f},
        {3.0f, 0.0f}, {3.0f, 0.0f},
        {4.0f, 0.0f}, {4.0f, 0.0f}
    };
    rdma_dada::pipeline::Metadata input_header = MakeInputHeader();
    input_header.SetUint64("TRANSFER_SIZE", sizeof(input_data));
    input_header.SetUint64("FILE_SIZE", sizeof(input_data));
    input_header.SetUint64("OBS_OFFSET", 0);

    rdma_dada::pipeline::WorkerConfig config = MakeConfig(weights_path);
    config.product = rdma_dada::pipeline::WorkerProduct::kStokes;
    config.udp_packets_per_antenna_per_block = 2;
    config.integration_enabled = true;
    config.integration_length = 2;
    config.integration_operation = "SUM";

    rdma_dada::pipeline::ModuleChain chain;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        chain.Configure(input_header, config, &output_header);
    Expect(status.ok(),
           "beamform+Stokes+integration chain configures: " +
               status.message());
    if (!status.ok()) return;

    std::uint64_t number = 0;
    double decimal = 0.0;
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 800,
           "Stokes integration scales output byte rate by K");
    Expect(output_header.GetDouble("TSAMP", &decimal) && decimal == 2.0,
           "Stokes integration scales output sample interval by K");

    std::uint64_t scratch_bytes = 0;
    std::uint64_t output_bytes = 0;
    status = chain.PlanBlock(
        sizeof(input_data), &scratch_bytes, &output_bytes);
    Expect(status.ok() && scratch_bytes == 64 && output_bytes == 16,
           "integrated Stokes chain plans beamformed/product scratch");

    std::uint8_t scratch[64] = {};
    float output_data[4] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 122,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(output_data), sizeof(output_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = chain.ProcessBlock(
        input, &output, scratch, sizeof(scratch), context);
    Expect(status.ok(), "integrated Stokes chain processes one block");
    const float expected[] = {40.0f, 80.0f, 56.0f, 0.0f};
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(output_data[i], expected[i],
                   "known beamform+Stokes+sum value");
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: pipeline_worker_core_test CONFIG WEIGHTS_PATH\n";
        return 2;
    }
    TestConfigAndAsciiCodec(argv[1]);
    if (!WriteWeights(argv[2])) {
        std::cerr << "failed to create worker test NPY file\n";
        return 2;
    }
    const std::string legacy_config_path = std::string(argv[2]) + ".json";
    if (!WriteLegacyWorkerConfig(legacy_config_path, argv[2])) {
        std::cerr << "failed to create schema v1 worker config\n";
        std::remove(argv[2]);
        return 2;
    }
    TestLegacyConfigDefaults(legacy_config_path);
    TestModuleChain(argv[2]);
    TestIntegratedModuleChain(argv[2]);
    TestIntegratedStokesChain(argv[2]);
    std::remove(legacy_config_path.c_str());
    std::remove(argv[2]);
    if (failures != 0) return 1;
    std::cout << "pipeline_worker_core_test passed\n";
    return 0;
}
