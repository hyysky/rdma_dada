#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

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

rdma_dada::pipeline::Metadata MakeAtfpHeader(
    const std::string& sample_format, std::uint64_t nchan,
    std::uint64_t npol, std::uint64_t nant, std::uint64_t block_ntime,
    const std::string& memory = "HOST") {
    const std::uint64_t component_bits = sample_format == "CI8" ? 8 : 16;
    const std::uint64_t sample_bytes = 2 * component_bits / 8;
    const std::uint64_t frame_bytes = nant * nchan * npol * sample_bytes;
    const std::uint64_t block_bytes = block_ntime * frame_bytes;

    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "UNPACKED");
    header.SetString("ORDER", "ATFP");
    header.SetString("LAYOUT_SCOPE", "BLOCK");
    header.SetString("SAMPLE_FORMAT", sample_format);
    header.SetString("SAMPLE_ENCODING", "TWOS_COMPLEMENT");
    header.SetString("COMPONENT_ORDER", "IQ");
    header.SetString("ENDIAN", "LITTLE");
    header.SetString("MEMORY", memory);
    header.SetString("SOURCE", "complex-convert-test");
    header.SetUint64("COMPONENT_NBIT", component_bits);
    header.SetUint64("SAMPLE_NBIT", 2 * component_bits);
    header.SetUint64("NCHAN", nchan);
    header.SetUint64("NPOL", npol);
    header.SetUint64("NANT", nant);
    header.SetUint64("BLOCK_NTIME", block_ntime);
    header.SetUint64("RESOLUTION", frame_bytes);
    header.SetUint64("RECORD_BYTES", block_bytes);
    header.SetUint64("OUTPUT_BLOCK_BYTES", block_bytes);
    header.SetUint64("BYTES_PER_SECOND", 10 * frame_bytes);
    header.SetUint64("TRANSFER_SIZE", block_bytes);
    header.SetUint64("FILE_SIZE", 2 * block_bytes);
    header.SetUint64("OBS_OFFSET", block_bytes);
    return header;
}

rdma_dada::pipeline::StageParameters CpuParameters(double scale) {
    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
    parameters.SetDouble("CONVERSION_SCALE", scale);
    return parameters;
}

rdma_dada::pipeline::BlockExecutionContext HostContext() {
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    return context;
}

void TestAtfpCi8AsymmetricPartialBlock() {
    rdma_dada::pipeline::Metadata input_header =
        MakeAtfpHeader("CI8", 2, 1, 3, 3);
    // Legacy fields must never leak into converted metadata.
    input_header.SetUint64("COMPONENT_SIGNED", 1);
    input_header.SetUint64("SOURCE_COMPONENT_SIGNED", 1);

    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status = module.ConfigureHeader(
        input_header, CpuParameters(0.25), &output_header);
    Expect(status.ok(), "asymmetric ATFP CI8 header configures conversion");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return;
    }

    std::string text;
    std::uint64_t number = 0;
    double decimal = 0.0;
    Expect(output_header.GetString("DATA_STAGE", &text) &&
               text == "CONVERTED",
           "conversion publishes CONVERTED stage");
    Expect(output_header.GetString("ORDER", &text) && text == "TFPA",
           "conversion publishes TFPA order");
    Expect(output_header.GetString("SOURCE_ORDER", &text) && text == "ATFP",
           "conversion records ATFP source order");
    Expect(output_header.GetString("SAMPLE_FORMAT", &text) && text == "CF32",
           "conversion publishes CF32 format");
    Expect(output_header.GetString("SOURCE_SAMPLE_FORMAT", &text) &&
               text == "CI8",
           "conversion records CI8 source format");
    Expect(output_header.GetString("SOURCE_SAMPLE_ENCODING", &text) &&
               text == "TWOS_COMPLEMENT",
           "conversion records two's-complement source encoding");
    Expect(output_header.GetString("COMPONENT_ORDER", &text) && text == "RI",
           "CF32 output uses RI component order");
    Expect(output_header.GetString("SOURCE_COMPONENT_ORDER", &text) &&
               text == "IQ",
           "conversion records IQ source component order");
    Expect(!output_header.Has("COMPONENT_SIGNED") &&
               !output_header.Has("SOURCE_COMPONENT_SIGNED") &&
               !output_header.Has("SAMPLE_ENCODING"),
           "converted metadata contains no integer signedness encoding");
    Expect(output_header.GetString("SOURCE", &text) &&
               text == "complex-convert-test",
           "conversion preserves unrelated observation metadata");
    Expect(output_header.GetUint64("COMPONENT_NBIT", &number) && number == 32,
           "CF32 component width is 32 bits");
    Expect(output_header.GetUint64("SAMPLE_NBIT", &number) && number == 64,
           "CF32 sample width is 64 bits");
    Expect(output_header.GetUint64("RESOLUTION", &number) && number == 48,
           "output resolution is one TFPA time frame");
    Expect(output_header.GetUint64("RECORD_BYTES", &number) && number == 144,
           "output record bytes describe the nominal block");
    Expect(output_header.GetUint64("OUTPUT_BLOCK_BYTES", &number) &&
               number == 144,
           "output nominal block bytes are scaled");
    Expect(output_header.GetUint64("BYTES_PER_SECOND", &number) &&
               number == 480,
           "output byte rate is scaled by sample width");
    Expect(output_header.GetUint64("TRANSFER_SIZE", &number) && number == 144,
           "output transfer size is scaled");
    Expect(output_header.GetUint64("FILE_SIZE", &number) && number == 288,
           "output file size is scaled");
    Expect(output_header.GetUint64("OBS_OFFSET", &number) && number == 144,
           "output observation offset is scaled");
    Expect(output_header.GetDouble("CONVERSION_SCALE", &decimal) &&
               std::fabs(decimal - 0.25) < 1.0e-12,
           "output records the scale applied exactly once");

    // [A,Q], Q=actual_T*F*P=2*2*1=4. Nominal T is 3, so this is partial.
    const std::int8_t input_data[] = {
        -128, 127, -12, 13, -10, 11, -8, 9,
        -6, 7, -4, 5, -2, 3, 0, 1,
        2, -3, 4, -5, 6, -7, 8, -9
    };
    rdma_dada::pipeline::Complex32 output_data[12] = {};
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(input_data),
        sizeof(input_data), 101,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(output_data), sizeof(output_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = module.ProcessBlock(input, &output, HostContext());
    Expect(status.ok(), "asymmetric ATFP CI8 block converts to TFPA CF32");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return;
    }

    const float expected[][2] = {
        {-32.0f, 31.75f}, {-1.5f, 1.75f}, {0.5f, -0.75f},
        {-3.0f, 3.25f}, {-1.0f, 1.25f}, {1.0f, -1.25f},
        {-2.5f, 2.75f}, {-0.5f, 0.75f}, {1.5f, -1.75f},
        {-2.0f, 2.25f}, {0.0f, 0.25f}, {2.0f, -2.25f}
    };
    for (std::size_t i = 0; i < 12; ++i) {
        ExpectNear(output_data[i].real, expected[i][0], "CI8 real value");
        ExpectNear(output_data[i].imag, expected[i][1], "CI8 imaginary value");
    }
    Expect(output.size == sizeof(output_data),
           "partial block reports exact converted bytes");
    Expect(output.sequence == 101, "conversion preserves block sequence");
}

void TestAtfpCi16AsymmetricConversion() {
    rdma_dada::pipeline::Metadata input_header =
        MakeAtfpHeader("CI16", 1, 2, 3, 2, "PINNED_HOST");
    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status = module.ConfigureHeader(
        input_header, CpuParameters(0.5), &output_header);
    Expect(status.ok(), "asymmetric ATFP CI16 header configures conversion");
    if (!status.ok()) return;

    const std::int16_t values[] = {
        -32768, 32767, -30, 31, -28, 29, -26, 27,
        -24, 25, -22, 23, -20, 21, -18, 19,
        -16, 17, -14, 15, -12, 13, -10, 11
    };
    std::vector<std::uint8_t> input_bytes(sizeof(values));
    std::memcpy(&input_bytes[0], values, sizeof(values));
    rdma_dada::pipeline::Complex32 output_data[12] = {};
    const rdma_dada::pipeline::InputBlock input = {
        &input_bytes[0], static_cast<std::uint64_t>(input_bytes.size()), 102,
        rdma_dada::pipeline::MemoryLocation::kPinnedHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(output_data), sizeof(output_data),
        0, 0, rdma_dada::pipeline::MemoryLocation::kPinnedHost
    };
    status = module.ProcessBlock(input, &output, HostContext());
    Expect(status.ok(), "asymmetric ATFP CI16 block converts to TFPA CF32");
    if (!status.ok()) return;

    const std::size_t source_order[] = {0, 4, 8, 1, 5, 9,
                                        2, 6, 10, 3, 7, 11};
    for (std::size_t dst = 0; dst < 12; ++dst) {
        const std::size_t src = source_order[dst];
        ExpectNear(output_data[dst].real,
                   static_cast<float>(values[2 * src]) * 0.5f,
                   "CI16 transposed real value");
        ExpectNear(output_data[dst].imag,
                   static_cast<float>(values[2 * src + 1]) * 0.5f,
                   "CI16 transposed imaginary value");
    }
    Expect(output.size == sizeof(output_data), "CI16 reports exact bytes");
    Expect(output.sequence == 102, "CI16 preserves sequence");
}

void TestBlockValidation() {
    const rdma_dada::pipeline::Metadata header =
        MakeAtfpHeader("CI8", 2, 1, 3, 2);
    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status = module.ConfigureHeader(
        header, CpuParameters(1.0), &output_header);
    Expect(status.ok(), "block-validation module configures");
    if (!status.ok()) return;

    std::uint8_t input_storage[25] = {};
    std::uint8_t output_storage[128] = {};
    rdma_dada::pipeline::InputBlock input = {
        input_storage, sizeof(input_storage), 103,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        output_storage, sizeof(output_storage), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = module.ProcessBlock(input, &output, HostContext());
    Expect(!status.ok(), "conversion rejects an incomplete ATFP time frame");

    input.size = 24;
    output.capacity = 95;
    status = module.ProcessBlock(input, &output, HostContext());
    Expect(!status.ok(), "conversion rejects insufficient output capacity");

    std::uint8_t overlap_storage[128] = {};
    input.data = overlap_storage;
    output.data = overlap_storage + 8;
    output.capacity = 120;
    status = module.ProcessBlock(input, &output, HostContext());
    Expect(!status.ok(), "conversion rejects overlapping input and output");
}

void ExpectHeaderRejected(rdma_dada::pipeline::Metadata header,
                          const std::string& message) {
    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output;
    const rdma_dada::pipeline::StageStatus status = module.ConfigureHeader(
        header, CpuParameters(1.0), &output);
    Expect(!status.ok(), message);
}

void TestHeaderValidation() {
    rdma_dada::pipeline::Metadata header =
        MakeAtfpHeader("CI8", 2, 1, 3, 2);
    header.SetString("ORDER", "TFPA");
    ExpectHeaderRejected(header, "conversion rejects non-ATFP input order");

    header = MakeAtfpHeader("CI8", 2, 1, 3, 2);
    header.SetString("LAYOUT_SCOPE", "TRANSFER");
    ExpectHeaderRejected(header, "conversion rejects non-block layout scope");

    header = MakeAtfpHeader("CI8", 2, 1, 3, 2);
    header.SetString("SAMPLE_ENCODING", "OFFSET_BINARY");
    ExpectHeaderRejected(header,
                         "conversion rejects non-two's-complement encoding");

    header = MakeAtfpHeader("CI8", 2, 1, 3, 2);
    header.SetString("COMPONENT_ORDER", "RI");
    ExpectHeaderRejected(header, "conversion rejects non-IQ source order");

    header = MakeAtfpHeader("CI8", 2, 1, 3, 2);
    header.SetUint64("OUTPUT_BLOCK_BYTES", 23);
    ExpectHeaderRejected(header,
                         "conversion rejects inconsistent nominal block bytes");

    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output;
    const rdma_dada::pipeline::StageStatus status = module.ConfigureHeader(
        MakeAtfpHeader("CI8", 2, 1, 3, 2), CpuParameters(1.0e-100),
        &output);
    Expect(!status.ok(), "conversion rejects an FP32-underflowing scale");
}

}  // namespace

int main() {
    TestAtfpCi8AsymmetricPartialBlock();
    TestAtfpCi16AsymmetricConversion();
    TestBlockValidation();
    TestHeaderValidation();
    if (failures != 0) return 1;
    std::cout << "complex_convert_module_test passed\n";
    return 0;
}
