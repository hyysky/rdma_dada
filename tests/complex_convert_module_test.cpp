#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
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

void TestCi8Conversion() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "UNPACKED");
    input_header.SetString("ORDER", "TFPA");
    input_header.SetString("SAMPLE_FORMAT", "CI8");
    input_header.SetString("COMPONENT_ORDER", "RI");
    input_header.SetString("ENDIAN", "LITTLE");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetString("SOURCE", "complex-convert-test");
    input_header.SetUint64("COMPONENT_NBIT", 8);
    input_header.SetUint64("COMPONENT_SIGNED", 1);
    input_header.SetUint64("SAMPLE_NBIT", 16);
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NANT", 2);
    input_header.SetUint64("RECORD_BYTES", 4);
    input_header.SetUint64("RESOLUTION", 4);
    input_header.SetUint64("BYTES_PER_SECOND", 40);
    input_header.SetUint64("TRANSFER_SIZE", 8);
    input_header.SetUint64("FILE_SIZE", 20);
    input_header.SetUint64("OBS_OFFSET", 4);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetDouble("CONVERSION_SCALE", 0.5);

    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "CI8 TFPA header configures complex conversion");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return;
    }

    std::string text;
    std::uint64_t number = 0;
    double decimal = 0.0;
    Expect(output_header.GetString("DATA_STAGE", &text) &&
               text == "CONVERTED",
           "conversion updates DATA_STAGE");
    Expect(output_header.GetString("ORDER", &text) && text == "TFPA",
           "conversion preserves TFPA order");
    Expect(output_header.GetString("SAMPLE_FORMAT", &text) && text == "CF32",
           "conversion publishes CF32 format");
    Expect(output_header.GetString("SOURCE_SAMPLE_FORMAT", &text) &&
               text == "CI8",
           "conversion records source sample format");
    Expect(output_header.GetString("SOURCE", &text) &&
               text == "complex-convert-test",
           "conversion preserves unknown observation metadata");
    Expect(output_header.GetUint64("SOURCE_COMPONENT_NBIT", &number) &&
               number == 8,
           "conversion records source component width");
    Expect(output_header.GetUint64("COMPONENT_NBIT", &number) && number == 32,
           "conversion publishes float component width");
    Expect(output_header.GetUint64("SAMPLE_NBIT", &number) && number == 64,
           "conversion publishes complex sample width");
    Expect(output_header.GetUint64("RECORD_BYTES", &number) && number == 16,
           "conversion expands one TFPA frame");
    Expect(output_header.GetUint64("RESOLUTION", &number) && number == 16,
           "conversion publishes output frame resolution");
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 160,
           "conversion scales byte rate by frame ratio");
    Expect(output_header.GetUint64("TRANSFER_SIZE", &number) && number == 32,
           "conversion scales transfer size by frame ratio");
    Expect(output_header.GetUint64("FILE_SIZE", &number) && number == 80,
           "conversion scales file size by frame ratio");
    Expect(output_header.GetUint64("OBS_OFFSET", &number) && number == 16,
           "conversion scales observation offset by frame ratio");
    Expect(output_header.GetDouble("CONVERSION_SCALE", &decimal) &&
               std::fabs(decimal - 0.5) < 1.0e-12,
           "conversion publishes the applied scale");

    const std::int8_t input_data[] = {
        -128, 127, 2, -3,
        4, -5, 6, -7
    };
    rdma_dada::pipeline::Complex32 output_data[4] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 91,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(output_data), sizeof(output_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = module.ProcessBlock(input, &output, context);
    Expect(status.ok(), "CI8 block converts to CF32");
    if (!status.ok()) std::cerr << status.message() << '\n';

    const float expected[][2] = {
        {-64.0f, 63.5f}, {1.0f, -1.5f},
        {2.0f, -2.5f}, {3.0f, -3.5f}
    };
    for (std::size_t i = 0; i < 4; ++i) {
        ExpectNear(output_data[i].real, expected[i][0], "CI8 real component");
        ExpectNear(output_data[i].imag, expected[i][1], "CI8 imag component");
    }
    Expect(output.size == sizeof(output_data), "CI8 output expands to CF32");
    Expect(output.sequence == 91, "conversion preserves block sequence");
}

void TestCi16Conversion() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "UNPACKED");
    input_header.SetString("ORDER", "TFPA");
    input_header.SetString("SAMPLE_FORMAT", "CI16");
    input_header.SetString("COMPONENT_ORDER", "RI");
    input_header.SetString("ENDIAN", "LITTLE");
    input_header.SetString("MEMORY", "PINNED_HOST");
    input_header.SetUint64("COMPONENT_NBIT", 16);
    input_header.SetUint64("COMPONENT_SIGNED", 1);
    input_header.SetUint64("SAMPLE_NBIT", 32);
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NANT", 2);
    input_header.SetUint64("RECORD_BYTES", 8);
    input_header.SetUint64("RESOLUTION", 8);
    input_header.SetUint64("BYTES_PER_SECOND", 80);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetDouble("CONVERSION_SCALE", 0.25);

    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "CI16 TFPA header configures complex conversion");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return;
    }

    const std::int16_t input_data[] = {
        -32768, 32767, -2, 3,
        4, -5, 1200, -1600
    };
    rdma_dada::pipeline::Complex32 output_data[4] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 92,
        rdma_dada::pipeline::MemoryLocation::kPinnedHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(output_data), sizeof(output_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kPinnedHost
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = module.ProcessBlock(input, &output, context);
    Expect(status.ok(), "CI16 block converts to CF32");
    if (!status.ok()) std::cerr << status.message() << '\n';

    const float expected[][2] = {
        {-8192.0f, 8191.75f}, {-0.5f, 0.75f},
        {1.0f, -1.25f}, {300.0f, -400.0f}
    };
    for (std::size_t i = 0; i < 4; ++i) {
        ExpectNear(output_data[i].real, expected[i][0], "CI16 real component");
        ExpectNear(output_data[i].imag, expected[i][1], "CI16 imag component");
    }
    Expect(output.size == sizeof(output_data), "CI16 output expands to CF32");
    Expect(output.sequence == 92, "CI16 conversion preserves sequence");
}

void TestRejectsOverlappingBlocks() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "UNPACKED");
    input_header.SetString("ORDER", "TFPA");
    input_header.SetString("SAMPLE_FORMAT", "CI8");
    input_header.SetString("COMPONENT_ORDER", "RI");
    input_header.SetString("ENDIAN", "LITTLE");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("COMPONENT_NBIT", 8);
    input_header.SetUint64("COMPONENT_SIGNED", 1);
    input_header.SetUint64("SAMPLE_NBIT", 16);
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NANT", 1);
    input_header.SetUint64("RECORD_BYTES", 2);
    input_header.SetUint64("RESOLUTION", 2);
    input_header.SetUint64("BYTES_PER_SECOND", 20);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetDouble("CONVERSION_SCALE", 1.0);

    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "overlap test configures conversion");
    if (!status.ok()) return;

    std::uint8_t storage[16] = {};
    const rdma_dada::pipeline::InputBlock input = {
        storage, 2, 93, rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        storage, sizeof(storage), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = module.ProcessBlock(input, &output, context);
    Expect(!status.ok(), "conversion rejects overlapping input/output blocks");
}

void TestRejectsUnsignedComponents() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "UNPACKED");
    input_header.SetString("ORDER", "TFPA");
    input_header.SetString("SAMPLE_FORMAT", "CI8");
    input_header.SetString("COMPONENT_ORDER", "RI");
    input_header.SetString("ENDIAN", "LITTLE");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("COMPONENT_NBIT", 8);
    input_header.SetUint64("COMPONENT_SIGNED", 0);
    input_header.SetUint64("SAMPLE_NBIT", 16);
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NANT", 1);
    input_header.SetUint64("RECORD_BYTES", 2);
    input_header.SetUint64("RESOLUTION", 2);
    input_header.SetUint64("BYTES_PER_SECOND", 20);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetDouble("CONVERSION_SCALE", 1.0);

    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    const rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(!status.ok(), "conversion rejects unsigned integer components");
}

void TestRejectsScaleThatUnderflowsFloat() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "UNPACKED");
    input_header.SetString("ORDER", "TFPA");
    input_header.SetString("SAMPLE_FORMAT", "CI8");
    input_header.SetString("COMPONENT_ORDER", "RI");
    input_header.SetString("ENDIAN", "LITTLE");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("COMPONENT_NBIT", 8);
    input_header.SetUint64("COMPONENT_SIGNED", 1);
    input_header.SetUint64("SAMPLE_NBIT", 16);
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NANT", 1);
    input_header.SetUint64("RECORD_BYTES", 2);
    input_header.SetUint64("RESOLUTION", 2);
    input_header.SetUint64("BYTES_PER_SECOND", 20);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetDouble("CONVERSION_SCALE", 1.0e-100);

    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    const rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(!status.ok(),
           "conversion rejects a scale that underflows its FP32 kernel value");
}

}  // namespace

int main() {
    TestCi8Conversion();
    TestCi16Conversion();
    TestRejectsOverlappingBlocks();
    TestRejectsUnsignedComponents();
    TestRejectsScaleThatUnderflowsFloat();
    if (failures != 0) return 1;
    std::cout << "complex_convert_module_test passed\n";
    return 0;
}
