#include "rdma_dada/modules/stokes/stokes_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;
const float kAbsoluteTolerance = 1.0e-6f;
const float kRelativeTolerance = 1.0e-6f;

struct ErrorEvidence {
    float max_absolute;
    float max_relative;
    std::uint64_t nan_count;
    std::uint64_t inf_count;
};

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ExpectNear(float actual, float expected, const std::string& message) {
    if (std::fabs(actual - expected) > kAbsoluteTolerance) {
        std::cerr << "FAIL: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

ErrorEvidence MeasureErrors(
    const float* actual, const float* expected, std::size_t count) {
    ErrorEvidence evidence = {0.0f, 0.0f, 0, 0};
    for (std::size_t i = 0; i < count; ++i) {
        if (std::isnan(actual[i])) {
            ++evidence.nan_count;
            continue;
        }
        if (std::isinf(actual[i])) {
            ++evidence.inf_count;
            continue;
        }
        const float absolute = std::fabs(actual[i] - expected[i]);
        float relative = 0.0f;
        if (expected[i] != 0.0f) {
            relative = absolute / std::fabs(expected[i]);
        } else if (absolute != 0.0f) {
            relative = std::numeric_limits<float>::infinity();
        }
        if (absolute > evidence.max_absolute) {
            evidence.max_absolute = absolute;
        }
        if (relative > evidence.max_relative) {
            evidence.max_relative = relative;
        }
    }
    return evidence;
}

bool WriteEvidence(
    const std::string& path,
    const ErrorEvidence& product_errors,
    const ErrorEvidence& derived_errors) {
    std::ofstream output(path.c_str(), std::ios::out | std::ios::trunc);
    if (!output) return false;
    output << "{\n"
           << "  \"schema_version\": 1,\n"
           << "  \"test_result\": \""
           << (failures == 0 ? "PASS" : "FAIL") << "\",\n"
           << "  \"module\": \"stokes\",\n"
           << "  \"backend\": \"CPU_REFERENCE\",\n"
           << "  \"input\": {\"order\": \"TFPB\", "
              "\"sample_format\": \"CF32\", \"shape\": [2, 2, 2, 2], "
              "\"bytes\": 128},\n"
           << "  \"output\": {\"order\": \"TFBS\", "
              "\"sample_format\": \"F32\", "
              "\"products\": [\"AA\", \"BB\", \"AB_REAL\", "
              "\"AB_IMAG\"], \"shape\": [2, 2, 2, 4], "
              "\"bytes\": 128},\n"
           << "  \"integration\": {\"enabled\": false},\n"
           << "  \"errors\": {\"absolute_tolerance\": 0.000001, "
              "\"relative_tolerance\": 0.000001, "
              "\"max_absolute\": " << product_errors.max_absolute
           << ", \"max_relative\": " << product_errors.max_relative
           << ", \"nan_count\": " << product_errors.nan_count
           << ", \"inf_count\": " << product_errors.inf_count << "},\n"
           << "  \"derived_reference\": {\"products\": "
              "[\"I\", \"Q\", \"U\", \"V\"], "
              "\"shape\": [2, 2, 2, 4], \"errors\": {"
              "\"max_absolute\": " << derived_errors.max_absolute
           << ", \"max_relative\": " << derived_errors.max_relative
           << ", \"nan_count\": " << derived_errors.nan_count
           << ", \"inf_count\": " << derived_errors.inf_count << "}}\n"
           << "}\n";
    return output.good();
}

rdma_dada::pipeline::Metadata MakeInputHeader() {
    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "BEAMFORMED");
    header.SetString("ORDER", "TFPB");
    header.SetString("SAMPLE_FORMAT", "CF32");
    header.SetString("MEMORY", "HOST");
    header.SetString("POL_LABELS", "X,Y");
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

int main(int argc, char** argv) {
    std::string result_json;
    if (argc == 3 && std::string(argv[1]) == "--result-json") {
        result_json = argv[2];
    } else if (argc != 1) {
        std::cerr << "Usage: " << argv[0] << " [--result-json PATH]\n";
        return 2;
    }
    typedef rdma_dada::pipeline::Complex32 Complex32;
    rdma_dada::modules::stokes::StokesModule module;
    const rdma_dada::pipeline::Metadata input_header = MakeInputHeader();
    const rdma_dada::pipeline::StageParameters parameters =
        MakeHostParameters();
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "valid dual-polarization beam header configures Stokes");

    std::string text;
    std::uint64_t number = 0;
    Expect(std::string(module.Name()) == "stokes", "module name is stokes");
    Expect(output_header.GetString("DATA_STAGE", &text) &&
               text == "POLARIZATION_PRODUCTS",
           "output DATA_STAGE identifies polarization products");
    Expect(output_header.GetString("ORDER", &text) && text == "TFBS",
           "output order is TFBS");
    Expect(output_header.GetString("SAMPLE_FORMAT", &text) && text == "F32",
           "output sample format is F32");
    Expect(output_header.GetString("PRODUCTS", &text) &&
               text == "AA,BB,AB_REAL,AB_IMAG",
           "output product order is explicit");
    Expect(output_header.GetString("POL_LABELS", &text) && text == "X,Y",
           "input polarization labels are preserved");
    Expect(output_header.GetUint64("NPRODUCT", &number) && number == 4,
           "output has four products");
    Expect(output_header.GetUint64("NPOL", &number) && number == 2,
           "dual-polarization source geometry is preserved");
    Expect(output_header.GetUint64("SAMPLE_NBIT", &number) && number == 32,
           "each output product is one F32 sample");
    Expect(output_header.GetUint64("RECORD_BYTES", &number) && number == 64,
           "output record is one complete TFBS frame");
    Expect(output_header.GetUint64("RESOLUTION", &number) && number == 64,
           "output resolution is one complete TFBS frame");
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 6400,
           "byte rate is unchanged for two CF32 to four F32 products");
    Expect(output_header.GetUint64("NANT", &number) && number == 8,
           "source antenna count is preserved");
    Expect(output_header.GetString("UTC_START", &text) &&
               text == "2026-08-04-00:00:00",
           "unknown observation metadata is preserved");

    // T=2, F=2, P=2, B=2 in TFPB order. For each (t,f), the two
    // polarization planes contain [beam0, beam1].
    const Complex32 input_data[] = {
        {1.0f, 2.0f}, {3.0f, 4.0f}, {5.0f, 6.0f}, {7.0f, 8.0f},
        {1.0f, -1.0f}, {-2.0f, 0.5f}, {0.5f, 2.0f}, {3.0f, -4.0f},
        {0.0f, 0.0f}, {6.0f, 8.0f}, {-3.0f, 4.0f}, {1.0f, -1.0f},
        {-1.0f, -2.0f}, {0.25f, 0.75f}, {-5.0f, 12.0f}, {-0.5f, 0.25f}
    };
    const float expected[] = {
        5.0f, 61.0f, 17.0f, 4.0f,
        25.0f, 113.0f, 53.0f, 4.0f,
        2.0f, 4.25f, -1.5f, -2.5f,
        4.25f, 25.0f, -8.0f, -6.5f,
        0.0f, 25.0f, 0.0f, 0.0f,
        100.0f, 2.0f, -2.0f, 14.0f,
        5.0f, 169.0f, -19.0f, 22.0f,
        0.625f, 0.3125f, 0.0625f, -0.4375f
    };
    float output_data[32] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data),
        61,
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
    Expect(status.ok(), "CPU reference backend processes two TFBS frames");
    Expect(output.size == sizeof(output_data), "output block byte size");
    Expect(output.sequence == 61, "input sequence is propagated");
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(output_data[i], expected[i], "known coherency product");
    }
    const ErrorEvidence product_errors = MeasureErrors(
        output_data, expected, sizeof(expected) / sizeof(expected[0]));
    Expect(product_errors.max_absolute <= kAbsoluteTolerance,
           "coherency-product absolute error is within tolerance");
    Expect(product_errors.max_relative <= kRelativeTolerance,
           "coherency-product relative error is within tolerance");
    Expect(product_errors.nan_count == 0 && product_errors.inf_count == 0,
           "coherency products contain no NaN or Inf");

    const float expected_derived[] = {
        66.0f, -56.0f, 34.0f, -8.0f,
        138.0f, -88.0f, 106.0f, -8.0f,
        6.25f, -2.25f, -3.0f, 5.0f,
        29.25f, -20.75f, -16.0f, 13.0f,
        25.0f, -25.0f, 0.0f, 0.0f,
        102.0f, 98.0f, -4.0f, -28.0f,
        174.0f, -164.0f, -38.0f, -44.0f,
        0.9375f, 0.3125f, 0.125f, 0.875f
    };
    float actual_derived[32] = {};
    for (std::size_t frame = 0; frame < 8; ++frame) {
        const float aa = output_data[frame * 4];
        const float bb = output_data[frame * 4 + 1];
        const float ab_real = output_data[frame * 4 + 2];
        const float ab_imag = output_data[frame * 4 + 3];
        actual_derived[frame * 4] = aa + bb;
        actual_derived[frame * 4 + 1] = aa - bb;
        actual_derived[frame * 4 + 2] = 2.0f * ab_real;
        actual_derived[frame * 4 + 3] = -2.0f * ab_imag;
    }
    const ErrorEvidence derived_errors = MeasureErrors(
        actual_derived, expected_derived,
        sizeof(expected_derived) / sizeof(expected_derived[0]));
    Expect(derived_errors.max_absolute <= kAbsoluteTolerance,
           "derived I/Q/U/V absolute error is within tolerance");
    Expect(derived_errors.max_relative <= kRelativeTolerance,
           "derived I/Q/U/V relative error is within tolerance");
    Expect(derived_errors.nan_count == 0 && derived_errors.inf_count == 0,
           "derived I/Q/U/V contains no NaN or Inf");

    rdma_dada::pipeline::OutputBlock short_output = output;
    short_output.capacity -= sizeof(float);
    status = module.ProcessBlock(input, &short_output, host_context);
    Expect(!status.ok(), "undersized output block is rejected");

    Complex32 overlapping_data[16] = {};
    const rdma_dada::pipeline::InputBlock overlapping_input = {
        reinterpret_cast<const std::uint8_t*>(overlapping_data),
        sizeof(overlapping_data),
        62,
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
    rdma_dada::modules::stokes::StokesModule invalid_module;
    invalid_header.SetUint64("NPOL", 1);
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "Stokes rejects NPOL other than two");

    invalid_header = input_header;
    invalid_header.Erase("POL_LABELS");
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "missing polarization labels are rejected");

    invalid_header = input_header;
    invalid_header.SetString("POL_LABELS", "X");
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "POL_LABELS must contain exactly two labels");

    invalid_header = input_header;
    invalid_header.SetString("DATA_STAGE", "POWER");
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "Power output cannot feed Stokes");

    invalid_header = input_header;
    invalid_header.SetString("ORDER", "TFBS");
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "non-TFPB input is rejected");

    invalid_header = input_header;
    invalid_header.SetString("SAMPLE_FORMAT", "F32");
    status = invalid_module.ConfigureHeader(
        invalid_header, parameters, &output_header);
    Expect(!status.ok(), "non-CF32 input is rejected");

    rdma_dada::pipeline::StageParameters invalid_parameters = parameters;
    invalid_parameters.SetString("EXECUTION_BACKEND", "OPENCL");
    status = invalid_module.ConfigureHeader(
        input_header, invalid_parameters, &output_header);
    Expect(!status.ok(), "unknown execution backend is rejected");

    status = module.Finish();
    Expect(status.ok(), "Stokes finishes cleanly");
    status = module.ProcessBlock(input, &output, host_context);
    Expect(!status.ok(), "Stokes cannot process after Finish");

    if (!result_json.empty() &&
        !WriteEvidence(result_json, product_errors, derived_errors)) {
        std::cerr << "FAIL: cannot write numerical evidence "
                  << result_json << '\n';
        ++failures;
    }
    if (failures != 0) return 1;
    std::cout << "stokes_module_test passed\n";
    return 0;
}
