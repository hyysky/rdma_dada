#include "rdma_dada/modules/beamform/beamform_module.h"

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

bool WriteInt8Weights(const std::string& path) {
    std::string header =
        "{'descr': '|i1', 'fortran_order': False, "
        "'shape': (1, 1, 2, 2, 2), }";
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
        2, 0,   // f0,p0,a0,b0: 1 + 0i after scale=0.5
        4, 0,   // f0,p0,a0,b1: 2 + 0i
        0, 2,   // f0,p0,a1,b0: 0 + 1i
        2, -2   // f0,p0,a1,b1: 1 - 1i
    };
    output.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(weights), sizeof(weights));
    return output.good();
}

bool WriteInt16Weights(const std::string& path) {
    std::string header =
        "{'descr': '<i2', 'fortran_order': False, "
        "'shape': (1, 1, 1, 1, 2), }";
    while ((10 + header.size() + 1) % 16 != 0) header.push_back(' ');
    header.push_back('\n');

    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const unsigned char prefix[] = {
        0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<unsigned char>(header.size() & 0xff),
        static_cast<unsigned char>((header.size() >> 8) & 0xff)
    };
    // Little-endian int16 values [64, -32].
    const unsigned char weights[] = {0x40, 0x00, 0xe0, 0xff};
    output.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(weights), sizeof(weights));
    return output.good();
}

rdma_dada::pipeline::Metadata MakeInputHeader(std::uint64_t nant) {
    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "UNPACKED");
    header.SetString("ORDER", "TFPA");
    header.SetString("SAMPLE_FORMAT", "CF32");
    header.SetString("UTC_START", "2026-08-02-00:00:00");
    header.SetUint64("NCHAN", 1);
    header.SetUint64("NPOL", 1);
    header.SetUint64("NANT", nant);
    return header;
}

rdma_dada::pipeline::StageParameters MakeParameters(
    const std::string& weights_path, double scale, std::uint64_t nbeam) {
    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("WEIGHTS_FILE", weights_path);
    parameters.SetString("WEIGHTS_ORDER", "FPAB2");
    parameters.SetString("WEIGHTS_ID", "unit-test-v1");
    parameters.SetDouble("WEIGHTS_SCALE", scale);
    parameters.SetUint64("NBEAM", nbeam);
    parameters.SetString("COMPUTE_MODE", "FP32");
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    return parameters;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: beamform_module_test WEIGHTS_PATH\n";
        return 2;
    }
    const std::string weights_path = argv[1];
    if (!WriteInt8Weights(weights_path)) {
        std::cerr << "failed to create test NPY file\n";
        return 2;
    }

    const rdma_dada::pipeline::Metadata input_header = MakeInputHeader(2);
    const rdma_dada::pipeline::StageParameters parameters =
        MakeParameters(weights_path, 0.5, 2);

    rdma_dada::modules::beamform::BeamformModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "valid FPAB2 NPY weights configure the module");

    std::string value;
    std::uint64_t number = 0;
    Expect(output_header.GetString("DATA_STAGE", &value) &&
               value == "BEAMFORMED",
           "output DATA_STAGE is BEAMFORMED");
    Expect(output_header.GetString("ORDER", &value) && value == "TFPB",
           "output order is TFPB");
    Expect(output_header.GetString("SAMPLE_FORMAT", &value) &&
               value == "CF32",
           "output sample format is CF32");
    Expect(output_header.GetUint64("NBEAM", &number) && number == 2,
           "output NBEAM comes from validated weights");
    Expect(output_header.GetUint64("NANT", &number) && number == 2,
           "source NANT is preserved");
    Expect(output_header.GetUint64("RESOLUTION", &number) && number == 16,
           "output resolution is one TFPB time frame");
    Expect(output_header.GetString("UTC_START", &value) &&
               value == "2026-08-02-00:00:00",
           "observation metadata is preserved");

    typedef rdma_dada::modules::beamform::Complex32 Complex32;
    const Complex32 input_data[] = {{1.0f, 2.0f}, {3.0f, 4.0f}};
    Complex32 output_data[2] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data),
        17,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(output_data),
        sizeof(output_data),
        0,
        0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext host_context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = module.ProcessBlock(input, &output, host_context);
    Expect(status.ok(), "configured module processes one TFPA frame");
    Expect(output.size == sizeof(output_data), "output block byte size");
    Expect(output.sequence == 17, "input block sequence is propagated");
    ExpectNear(output_data[0].real, -3.0f, "beam 0 real");
    ExpectNear(output_data[0].imag, 5.0f, "beam 0 imaginary");
    ExpectNear(output_data[1].real, 9.0f, "beam 1 real");
    ExpectNear(output_data[1].imag, 5.0f, "beam 1 imaginary");

    rdma_dada::pipeline::OutputBlock short_output = {
        reinterpret_cast<std::uint8_t*>(output_data),
        sizeof(Complex32),
        0,
        0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = module.ProcessBlock(input, &short_output, host_context);
    Expect(!status.ok(), "an undersized output block is rejected");

    const std::string int16_path = weights_path + ".i16";
    Expect(WriteInt16Weights(int16_path), "create int16 NPY fixture");
    rdma_dada::modules::beamform::BeamformModule int16_module;
    const rdma_dada::pipeline::Metadata int16_input_header = MakeInputHeader(1);
    const rdma_dada::pipeline::StageParameters int16_parameters =
        MakeParameters(int16_path, 1.0 / 128.0, 1);
    status = int16_module.ConfigureHeader(
        int16_input_header, int16_parameters, &output_header);
    Expect(status.ok(), "little-endian int16 FPAB2 weights are accepted");
    const Complex32 int16_input_data[] = {{2.0f, 0.0f}};
    Complex32 int16_output_data[1] = {};
    const rdma_dada::pipeline::InputBlock int16_input = {
        reinterpret_cast<const std::uint8_t*>(int16_input_data),
        sizeof(int16_input_data),
        18,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock int16_output = {
        reinterpret_cast<std::uint8_t*>(int16_output_data),
        sizeof(int16_output_data),
        0,
        0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = int16_module.ProcessBlock(
        int16_input, &int16_output, host_context);
    Expect(status.ok(), "int16 weights process a block");
    ExpectNear(int16_output_data[0].real, 1.0f, "int16 scaled real");
    ExpectNear(int16_output_data[0].imag, -0.5f, "int16 scaled imaginary");

    rdma_dada::modules::beamform::BeamformModule mismatch_module;
    const rdma_dada::pipeline::StageParameters mismatch_parameters =
        MakeParameters(int16_path, 1.0 / 128.0, 2);
    status = mismatch_module.ConfigureHeader(
        int16_input_header, mismatch_parameters, &output_header);
    Expect(!status.ok(), "NPY B dimension must match configured NBEAM");

    rdma_dada::modules::beamform::BeamformModule unavailable_cuda_module;
    rdma_dada::pipeline::StageParameters unavailable_cuda_parameters =
        int16_parameters;
    unavailable_cuda_parameters.SetString("EXECUTION_BACKEND", "CUDA");
    unavailable_cuda_parameters.SetUint64("CUDA_DEVICE", 0);
    status = unavailable_cuda_module.ConfigureHeader(
        int16_input_header, unavailable_cuda_parameters, &output_header);
    Expect(!status.ok(),
           "a non-CUDA build rejects the CUDA execution backend");

    std::remove(weights_path.c_str());
    std::remove(int16_path.c_str());
    if (failures != 0) return 1;
    std::cout << "beamform_module_test passed\n";
    return 0;
}
