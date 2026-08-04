#include "rdma_dada/modules/power/power_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cmath>
#include <cstdint>
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

rdma_dada::pipeline::Metadata MakeInputHeader() {
    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "BEAMFORMED");
    header.SetString("ORDER", "TFPB");
    header.SetString("SAMPLE_FORMAT", "CF32");
    header.SetString("MEMORY", "HOST");
    header.SetString("UTC_START", "2026-08-04-00:00:00");
    header.SetUint64("NCHAN", 2);
    header.SetUint64("NPOL", 2);
    header.SetUint64("NANT", 8);
    header.SetUint64("NBEAM", 2);
    header.SetUint64("BYTES_PER_SECOND", 6400);
    return header;
}

rdma_dada::pipeline::StageParameters MakeHostParameters() {
    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    return parameters;
}

}  // namespace

int main() {
    typedef rdma_dada::pipeline::Complex32 Complex32;
    rdma_dada::modules::power::PowerModule module;
    const rdma_dada::pipeline::Metadata input_header = MakeInputHeader();
    const rdma_dada::pipeline::StageParameters parameters =
        MakeHostParameters();
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "valid TFPB CF32 header configures Power");

    std::string text;
    std::uint64_t number = 0;
    Expect(std::string(module.Name()) == "power", "module name is power");
    Expect(output_header.GetString("DATA_STAGE", &text) && text == "POWER",
           "output DATA_STAGE is POWER");
    Expect(output_header.GetString("ORDER", &text) && text == "TFPB",
           "output order remains TFPB");
    Expect(output_header.GetString("SAMPLE_FORMAT", &text) && text == "F32",
           "output sample format is F32");
    Expect(output_header.GetString("PRODUCTS", &text) && text == "POWER",
           "output product is POWER");
    Expect(output_header.GetUint64("SAMPLE_NBIT", &number) && number == 32,
           "output sample width is 32 bits");
    Expect(output_header.GetUint64("RECORD_BYTES", &number) && number == 32,
           "output record is one complete TFPB frame");
    Expect(output_header.GetUint64("RESOLUTION", &number) && number == 32,
           "output resolution is one complete TFPB frame");
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 3200,
           "output byte rate reflects CF32 to F32 conversion");
    Expect(output_header.GetUint64("NANT", &number) && number == 8,
           "source antenna count is preserved");
    Expect(output_header.GetString("UTC_START", &text) &&
               text == "2026-08-04-00:00:00",
           "unknown observation metadata is preserved");

    const Complex32 input_data[] = {
        {3.0f, 4.0f}, {-1.0f, 2.0f}, {0.0f, 0.0f}, {0.5f, -0.5f},
        {-2.0f, -3.0f}, {1.5f, 2.0f}, {-4.0f, 0.0f}, {0.0f, -5.0f},
        {1.0f, 1.0f}, {-1.0f, -1.0f}, {2.0f, -2.0f}, {-3.0f, 1.0f},
        {0.25f, 0.75f}, {-0.5f, 0.25f}, {6.0f, 8.0f}, {-7.0f, 24.0f}
    };
    const float expected[] = {
        25.0f, 5.0f, 0.0f, 0.5f,
        13.0f, 6.25f, 16.0f, 25.0f,
        2.0f, 2.0f, 8.0f, 10.0f,
        0.625f, 0.3125f, 100.0f, 625.0f
    };
    float output_data[16] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data),
        41,
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
    Expect(status.ok(), "CPU reference backend processes two TFPB frames");
    Expect(output.size == sizeof(output_data), "output byte size is halved");
    Expect(output.sequence == 41, "input sequence is propagated");
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(output_data[i], expected[i], "known power value");
    }

    rdma_dada::pipeline::OutputBlock short_output = output;
    short_output.capacity = sizeof(output_data) - sizeof(float);
    status = module.ProcessBlock(input, &short_output, host_context);
    Expect(!status.ok(), "undersized output block is rejected");

    Complex32 overlapping_data[16] = {};
    const rdma_dada::pipeline::InputBlock overlapping_input = {
        reinterpret_cast<const std::uint8_t*>(overlapping_data),
        sizeof(overlapping_data),
        42,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock overlapping_output = {
        reinterpret_cast<std::uint8_t*>(overlapping_data),
        sizeof(overlapping_data),
        0,
        0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = module.ProcessBlock(
        overlapping_input, &overlapping_output, host_context);
    Expect(!status.ok(), "overlapping input and output buffers are rejected");

    rdma_dada::pipeline::InputBlock partial_input = input;
    partial_input.size -= sizeof(Complex32);
    status = module.ProcessBlock(partial_input, &output, host_context);
    Expect(!status.ok(), "partial TFPB time frame is rejected");

    rdma_dada::pipeline::OutputBlock device_output = output;
    device_output.location =
        rdma_dada::pipeline::MemoryLocation::kCudaDevice;
    status = module.ProcessBlock(input, &device_output, host_context);
    Expect(!status.ok(), "CPU backend rejects CUDA device output");

    rdma_dada::pipeline::Metadata invalid_header = input_header;
    invalid_header.SetString("ORDER", "TFPA");
    rdma_dada::modules::power::PowerModule invalid_module;
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "non-TFPB input is rejected");

    invalid_header = input_header;
    invalid_header.SetString("SAMPLE_FORMAT", "F32");
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "non-CF32 input is rejected");

    invalid_header = input_header;
    invalid_header.SetString("DATA_STAGE", "UNPACKED");
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "input not produced by Beamform is rejected");

    invalid_header = input_header;
    invalid_header.Erase("MEMORY");
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "missing input memory location is rejected");

    invalid_header = input_header;
    invalid_header.SetUint64("BYTES_PER_SECOND", 6399);
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "non-integral output byte rate is rejected");

    rdma_dada::pipeline::StageParameters invalid_parameters = parameters;
    invalid_parameters.SetString("EXECUTION_BACKEND", "OPENCL");
    status = invalid_module.ConfigureHeader(
        input_header, invalid_parameters, &output_header);
    Expect(!status.ok(), "unknown execution backend is rejected");

    status = module.Finish();
    Expect(status.ok(), "Power finishes cleanly");
    status = module.ProcessBlock(input, &output, host_context);
    Expect(!status.ok(), "Power cannot process after Finish");

    if (failures != 0) return 1;
    std::cout << "power_module_test passed\n";
    return 0;
}
