#include "rdma_dada/modules/time_integrate/time_integrate_module.h"

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

void ExpectNear(double actual, double expected, const std::string& message) {
    if (std::fabs(actual - expected) > 1.0e-6) {
        std::cerr << "FAIL: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

void TestTfpbMean() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POWER");
    input_header.SetString("ORDER", "TFPB");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetString("SOURCE", "integration-test");
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NBEAM", 2);
    input_header.SetUint64("RECORD_BYTES", 8);
    input_header.SetUint64("RESOLUTION", 8);
    input_header.SetUint64("BYTES_PER_SECOND", 80);
    input_header.SetUint64("TRANSFER_SIZE", 32);
    input_header.SetUint64("FILE_SIZE", 32);
    input_header.SetUint64("OBS_OFFSET", 16);
    input_header.SetDouble("TSAMP", 1.5);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetUint64("INTEGRATION_LENGTH", 2);
    parameters.SetString("INTEGRATION_OPERATION", "MEAN");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "TFPB mean integration configures");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return;
    }

    std::string text;
    std::uint64_t number = 0;
    double decimal = 0.0;
    Expect(output_header.GetString("DATA_STAGE", &text) &&
               text == "POWER_INTEGRATED",
           "integration updates POWER data stage");
    Expect(output_header.GetString("ORDER", &text) && text == "TFPB",
           "integration preserves TFPB order");
    Expect(output_header.GetString("SAMPLE_FORMAT", &text) && text == "F32",
           "integration preserves F32 sample format");
    Expect(output_header.GetString("SOURCE", &text) &&
               text == "integration-test",
           "integration preserves unknown observation metadata");
    Expect(output_header.GetString("INTEGRATION_OPERATION", &text) &&
               text == "MEAN",
           "integration publishes MEAN operation");
    Expect(output_header.GetUint64("INTEGRATION_LENGTH", &number) &&
               number == 2,
           "integration publishes local length");
    Expect(output_header.GetUint64("TOTAL_INTEGRATION_LENGTH", &number) &&
               number == 2,
           "integration publishes total length");
    Expect(output_header.GetUint64("RESOLUTION", &number) && number == 8,
           "integration preserves one-frame resolution");
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 40,
           "integration scales byte rate");
    Expect(output_header.GetUint64("TRANSFER_SIZE", &number) && number == 16,
           "integration scales transfer size");
    Expect(output_header.GetUint64("FILE_SIZE", &number) && number == 16,
           "integration scales file size");
    Expect(output_header.GetUint64("OBS_OFFSET", &number) && number == 8,
           "integration scales observation offset");
    Expect(output_header.GetDouble("TSAMP", &decimal),
           "integration publishes TSAMP");
    ExpectNear(decimal, 3.0, "integration scales TSAMP");

    const float input_data[] = {
        1.0f, 10.0f,
        3.0f, 14.0f,
        5.0f, 18.0f,
        7.0f, 22.0f
    };
    const float expected[] = {2.0f, 12.0f, 6.0f, 20.0f};
    float output_data[4] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 73,
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
    Expect(status.ok(), "TFPB mean integrates one block");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(output.size == sizeof(output_data),
           "integration output block shrinks by K");
    Expect(output.sequence == 73, "integration preserves block sequence");
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(output_data[i], expected[i], "known TFPB mean value");
    }
}

void TestTfbsSumWithUpstreamIntegration() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POLARIZATION_PRODUCTS");
    input_header.SetString("ORDER", "TFBS");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetString("PRODUCTS", "AA,BB,AB_REAL,AB_IMAG");
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NBEAM", 1);
    input_header.SetUint64("NPRODUCT", 4);
    input_header.SetUint64("RECORD_BYTES", 16);
    input_header.SetUint64("RESOLUTION", 16);
    input_header.SetUint64("BYTES_PER_SECOND", 160);
    input_header.SetUint64("TRANSFER_SIZE", 64);
    input_header.SetUint64("FILE_SIZE", 64);
    input_header.SetUint64("OBS_OFFSET", 32);
    input_header.SetUint64("TOTAL_INTEGRATION_LENGTH", 4);
    input_header.SetDouble("TSAMP", 2.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetUint64("INTEGRATION_LENGTH", 2);
    parameters.SetString("INTEGRATION_OPERATION", "SUM");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "TFBS sum integration configures");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return;
    }

    std::string text;
    std::uint64_t number = 0;
    double decimal = 0.0;
    Expect(output_header.GetString("DATA_STAGE", &text) &&
               text == "POLARIZATION_PRODUCTS_INTEGRATED",
           "integration updates polarization-products data stage");
    Expect(output_header.GetString("ORDER", &text) && text == "TFBS",
           "integration preserves TFBS order");
    Expect(output_header.GetString("INTEGRATION_OPERATION", &text) &&
               text == "SUM",
           "integration publishes SUM operation");
    Expect(output_header.GetUint64("TOTAL_INTEGRATION_LENGTH", &number) &&
               number == 8,
           "integration multiplies upstream total length");
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 80,
           "TFBS integration scales byte rate");
    Expect(output_header.GetDouble("TSAMP", &decimal),
           "TFBS integration publishes TSAMP");
    ExpectNear(decimal, 4.0, "TFBS integration scales TSAMP");

    const float input_data[] = {
        1.0f, 2.0f, 3.0f, 4.0f,
        10.0f, 20.0f, 30.0f, 40.0f,
        5.0f, 6.0f, 7.0f, 8.0f,
        50.0f, 60.0f, 70.0f, 80.0f
    };
    const float expected[] = {
        11.0f, 22.0f, 33.0f, 44.0f,
        55.0f, 66.0f, 77.0f, 88.0f
    };
    float output_data[8] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 81,
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
    Expect(status.ok(), "TFBS sum integrates one block");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(output.size == sizeof(output_data),
           "TFBS integration output shrinks by K");
    Expect(output.sequence == 81, "TFBS integration preserves sequence");
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(output_data[i], expected[i], "known TFBS sum value");
    }
}

void TestMultiAxisMeanPreservesFrameLayout() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POWER");
    input_header.SetString("ORDER", "TFPB");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("NCHAN", 2);
    input_header.SetUint64("NPOL", 2);
    input_header.SetUint64("NBEAM", 2);
    input_header.SetUint64("RECORD_BYTES", 32);
    input_header.SetUint64("RESOLUTION", 32);
    input_header.SetUint64("BYTES_PER_SECOND", 96);
    input_header.SetDouble("TSAMP", 1.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetUint64("INTEGRATION_LENGTH", 3);
    parameters.SetString("INTEGRATION_OPERATION", "MEAN");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "multi-axis mean integration configures");
    if (!status.ok()) return;

    float input_data[48] = {};
    for (std::size_t time = 0; time < 6; ++time) {
        for (std::size_t element = 0; element < 8; ++element) {
            input_data[time * 8 + element] =
                static_cast<float>(time * 100 + element);
        }
    }
    float output_data[16] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 82,
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
    Expect(status.ok(), "multi-axis mean integrates one block");
    for (std::size_t element = 0; element < 8; ++element) {
        ExpectNear(output_data[element], 100.0 + element,
                   "first integrated frame preserves FPB element order");
        ExpectNear(output_data[8 + element], 400.0 + element,
                   "second integrated frame preserves FPB element order");
    }
}

void TestReintegrationStage() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POWER_INTEGRATED");
    input_header.SetString("ORDER", "TFPB");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NBEAM", 1);
    input_header.SetUint64("RECORD_BYTES", 4);
    input_header.SetUint64("RESOLUTION", 4);
    input_header.SetUint64("BYTES_PER_SECOND", 48);
    input_header.SetUint64("TOTAL_INTEGRATION_LENGTH", 3);
    input_header.SetDouble("TSAMP", 3.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetUint64("INTEGRATION_LENGTH", 4);
    parameters.SetString("INTEGRATION_OPERATION", "MEAN");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    const rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "already integrated POWER can be integrated again");
    if (!status.ok()) return;
    std::string text;
    std::uint64_t number = 0;
    Expect(output_header.GetString("DATA_STAGE", &text) &&
               text == "POWER_INTEGRATED",
           "reintegration keeps canonical integrated stage");
    Expect(output_header.GetUint64("TOTAL_INTEGRATION_LENGTH", &number) &&
               number == 12,
           "reintegration multiplies total length");
}

void TestRejectsMismatchedRecordBytes() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POWER");
    input_header.SetString("ORDER", "TFPB");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NBEAM", 1);
    input_header.SetUint64("RECORD_BYTES", 8);
    input_header.SetUint64("RESOLUTION", 4);
    input_header.SetUint64("BYTES_PER_SECOND", 40);
    input_header.SetDouble("TSAMP", 1.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetUint64("INTEGRATION_LENGTH", 2);
    parameters.SetString("INTEGRATION_OPERATION", "SUM");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    const rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(!status.ok(),
           "integration rejects RECORD_BYTES inconsistent with F/P/B");
}

void TestRejectsZeroByteRate() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POWER");
    input_header.SetString("ORDER", "TFPB");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NBEAM", 1);
    input_header.SetUint64("RECORD_BYTES", 4);
    input_header.SetUint64("RESOLUTION", 4);
    input_header.SetUint64("BYTES_PER_SECOND", 0);
    input_header.SetDouble("TSAMP", 1.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetUint64("INTEGRATION_LENGTH", 2);
    parameters.SetString("INTEGRATION_OPERATION", "SUM");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    const rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(!status.ok(), "integration rejects zero BYTES_PER_SECOND");
}

void TestRequiresByteRate() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POWER");
    input_header.SetString("ORDER", "TFPB");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NBEAM", 1);
    input_header.SetUint64("RECORD_BYTES", 4);
    input_header.SetUint64("RESOLUTION", 4);
    input_header.SetDouble("TSAMP", 1.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetUint64("INTEGRATION_LENGTH", 2);
    parameters.SetString("INTEGRATION_OPERATION", "SUM");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    const rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(!status.ok(), "integration requires BYTES_PER_SECOND");
}

void TestRejectsInvalidBlocks() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POWER");
    input_header.SetString("ORDER", "TFPB");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "HOST");
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NBEAM", 1);
    input_header.SetUint64("RECORD_BYTES", 4);
    input_header.SetUint64("RESOLUTION", 4);
    input_header.SetUint64("BYTES_PER_SECOND", 40);
    input_header.SetDouble("TSAMP", 1.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetUint64("INTEGRATION_LENGTH", 2);
    parameters.SetString("INTEGRATION_OPERATION", "SUM");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "block-validation integration configures");
    if (!status.ok()) return;

    float input_data[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    float output_data[2] = {};
    rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        3 * sizeof(float), 9,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(output_data), sizeof(output_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext host_context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = module.ProcessBlock(input, &output, host_context);
    Expect(!status.ok(), "integration rejects T not divisible by K");

    input.size = sizeof(input_data);
    output.capacity = sizeof(float);
    status = module.ProcessBlock(input, &output, host_context);
    Expect(!status.ok(), "integration rejects undersized output block");

    output.data = reinterpret_cast<std::uint8_t*>(input_data);
    output.capacity = sizeof(input_data);
    status = module.ProcessBlock(input, &output, host_context);
    Expect(!status.ok(), "integration rejects overlapping input and output");

    output.data = reinterpret_cast<std::uint8_t*>(output_data);
    output.capacity = sizeof(output_data);
    const rdma_dada::pipeline::BlockExecutionContext wrong_context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0,
        reinterpret_cast<void*>(1)
    };
    status = module.ProcessBlock(input, &output, wrong_context);
    Expect(!status.ok(), "integration rejects a mismatched backend context");
}

}  // namespace

int main() {
    TestTfpbMean();
    TestTfbsSumWithUpstreamIntegration();
    TestMultiAxisMeanPreservesFrameLayout();
    TestReintegrationStage();
    TestRejectsMismatchedRecordBytes();
    TestRejectsZeroByteRate();
    TestRequiresByteRate();
    TestRejectsInvalidBlocks();
    if (failures != 0) return 1;
    std::cout << "time_integrate_module_test passed\n";
    return 0;
}
