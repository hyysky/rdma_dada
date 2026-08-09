#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime.h>

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

bool IsHelpRequest(int argc, char** argv) {
    return argc == 2 &&
           (std::strcmp(argv[1], "-h") == 0 ||
            std::strcmp(argv[1], "--help") == 0);
}

rdma_dada::pipeline::Metadata MakeHeader(
    const std::string& format, std::uint64_t nchan, std::uint64_t npol,
    std::uint64_t nant, std::uint64_t nominal_t,
    const std::string& memory, int device) {
    const std::uint64_t component_bits = format == "CI8" ? 8 : 16;
    const std::uint64_t sample_bytes = 2 * component_bits / 8;
    const std::uint64_t frame_bytes = nchan * npol * nant * sample_bytes;
    const std::uint64_t block_bytes = nominal_t * frame_bytes;
    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "UNPACKED");
    header.SetString("ORDER", "ATFP");
    header.SetString("LAYOUT_SCOPE", "BLOCK");
    header.SetString("SAMPLE_FORMAT", format);
    header.SetString("SAMPLE_ENCODING", "TWOS_COMPLEMENT");
    header.SetString("COMPONENT_ORDER", "IQ");
    header.SetString("ENDIAN", "LITTLE");
    header.SetString("MEMORY", memory);
    header.SetUint64("COMPONENT_NBIT", component_bits);
    header.SetUint64("SAMPLE_NBIT", 2 * component_bits);
    header.SetUint64("NCHAN", nchan);
    header.SetUint64("NPOL", npol);
    header.SetUint64("NANT", nant);
    header.SetUint64("BLOCK_NTIME", nominal_t);
    header.SetUint64("RESOLUTION", frame_bytes);
    header.SetUint64("RECORD_BYTES", block_bytes);
    header.SetUint64("OUTPUT_BLOCK_BYTES", block_bytes);
    header.SetUint64("BYTES_PER_SECOND", 10 * frame_bytes);
    header.SetUint64("TRANSFER_SIZE", block_bytes);
    if (memory == "CUDA_DEVICE") {
        header.SetUint64("CUDA_DEVICE", static_cast<std::uint64_t>(device));
    }
    return header;
}

rdma_dada::pipeline::StageParameters Parameters(
    const std::string& backend, double scale, int device) {
    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", backend);
    parameters.SetDouble("CONVERSION_SCALE", scale);
    if (backend == "CUDA") {
        parameters.SetUint64("CUDA_DEVICE", static_cast<std::uint64_t>(device));
    }
    return parameters;
}

std::vector<std::uint8_t> MakeInput(const std::string& format,
                                    std::uint64_t sample_count) {
    const std::uint64_t sample_bytes = format == "CI8" ? 2 : 4;
    std::vector<std::uint8_t> bytes(sample_count * sample_bytes, 0);
    if (format == "CI8") {
        std::int8_t* values = reinterpret_cast<std::int8_t*>(&bytes[0]);
        for (std::uint64_t i = 0; i < sample_count; ++i) {
            values[2 * i] = static_cast<std::int8_t>(
                static_cast<std::int64_t>((37 * i + 11) % 251) - 125);
            values[2 * i + 1] = static_cast<std::int8_t>(
                static_cast<std::int64_t>((53 * i + 7) % 253) - 126);
        }
        values[0] = -128;
        values[1] = 127;
    } else {
        for (std::uint64_t i = 0; i < sample_count; ++i) {
            const std::int16_t real = static_cast<std::int16_t>(
                static_cast<std::int64_t>((7919 * i + 101) % 65521) -
                32760);
            const std::int16_t imag = static_cast<std::int16_t>(
                static_cast<std::int64_t>((3571 * i + 307) % 65519) -
                32759);
            std::memcpy(&bytes[4 * i], &real, sizeof(real));
            std::memcpy(&bytes[4 * i + 2], &imag, sizeof(imag));
        }
        const std::int16_t minimum = -32768;
        const std::int16_t maximum = 32767;
        std::memcpy(&bytes[0], &minimum, sizeof(minimum));
        std::memcpy(&bytes[2], &maximum, sizeof(maximum));
    }
    return bytes;
}

void RunCase(const std::string& format, std::uint64_t nchan,
             std::uint64_t npol, std::uint64_t nant,
             std::uint64_t actual_t, std::uint64_t nominal_t,
             double scale, std::uint64_t sequence, bool check_bad_context) {
    const std::uint64_t q = actual_t * nchan * npol;
    const std::uint64_t sample_count = nant * q;
    std::vector<std::uint8_t> host_input = MakeInput(format, sample_count);
    std::vector<rdma_dada::pipeline::Complex32> reference(sample_count);
    std::vector<rdma_dada::pipeline::Complex32> result(sample_count);

    rdma_dada::modules::complex_convert::ComplexConvertModule cpu_module;
    rdma_dada::pipeline::Metadata cpu_output_header;
    rdma_dada::pipeline::StageStatus status = cpu_module.ConfigureHeader(
        MakeHeader(format, nchan, npol, nant, nominal_t, "HOST", 0),
        Parameters("CPU_REFERENCE", scale, 0), &cpu_output_header);
    Expect(status.ok(), format + " CPU oracle configures");
    if (!status.ok()) return;
    const rdma_dada::pipeline::InputBlock cpu_input = {
        &host_input[0], static_cast<std::uint64_t>(host_input.size()), sequence,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock cpu_output = {
        reinterpret_cast<std::uint8_t*>(&reference[0]),
        static_cast<std::uint64_t>(reference.size() * sizeof(reference[0])),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext cpu_context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = cpu_module.ProcessBlock(cpu_input, &cpu_output, cpu_context);
    Expect(status.ok(), format + " CPU oracle executes");
    if (!status.ok()) return;

    rdma_dada::modules::complex_convert::ComplexConvertModule cuda_module;
    rdma_dada::pipeline::Metadata cuda_output_header;
    status = cuda_module.ConfigureHeader(
        MakeHeader(format, nchan, npol, nant, nominal_t, "CUDA_DEVICE", 0),
        Parameters("CUDA", scale, 0), &cuda_output_header);
    Expect(status.ok(), format + " CUDA transpose configures");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return;
    }

    std::string text;
    Expect(cuda_output_header.GetString("ORDER", &text) && text == "TFPA",
           format + " CUDA header publishes TFPA");
    Expect(cuda_output_header.GetString("SOURCE_ORDER", &text) &&
               text == "ATFP",
           format + " CUDA header records ATFP source");

    std::uint8_t* device_input = NULL;
    std::uint8_t* device_output = NULL;
    cudaStream_t stream = NULL;
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      host_input.size()) == cudaSuccess,
           format + " allocates device input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output),
                      result.size() * sizeof(result[0])) == cudaSuccess,
           format + " allocates device output");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           format + " creates non-default stream");
    if (!device_input || !device_output || !stream) return;
    Expect(cudaMemcpyAsync(device_input, &host_input[0], host_input.size(),
                           cudaMemcpyHostToDevice, stream) == cudaSuccess,
           format + " enqueues H2D");

    const rdma_dada::pipeline::InputBlock cuda_input = {
        device_input, static_cast<std::uint64_t>(host_input.size()), sequence,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock cuda_output = {
        device_output,
        static_cast<std::uint64_t>(result.size() * sizeof(result[0])),
        0, 0, rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    const rdma_dada::pipeline::BlockExecutionContext cuda_context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0,
        reinterpret_cast<void*>(stream)
    };
    status = cuda_module.ProcessBlock(cuda_input, &cuda_output, cuda_context);
    Expect(status.ok(), format + " enqueues fused ATFP transpose");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(cuda_output.size == result.size() * sizeof(result[0]),
           format + " reports exact output bytes");
    Expect(cuda_output.sequence == sequence,
           format + " preserves block sequence");

    if (check_bad_context) {
        const rdma_dada::pipeline::BlockExecutionContext null_stream = {
            rdma_dada::pipeline::ExecutionBackend::kCuda, 0, NULL
        };
        status = cuda_module.ProcessBlock(cuda_input, &cuda_output, null_stream);
        Expect(!status.ok(), "CUDA transpose rejects null/default stream");
        const rdma_dada::pipeline::BlockExecutionContext wrong_device = {
            rdma_dada::pipeline::ExecutionBackend::kCuda, 1,
            reinterpret_cast<void*>(stream)
        };
        status = cuda_module.ProcessBlock(
            cuda_input, &cuda_output, wrong_device);
        Expect(!status.ok(), "CUDA transpose rejects wrong device context");
    }

    Expect(cudaMemcpyAsync(&result[0], device_output,
                           result.size() * sizeof(result[0]),
                           cudaMemcpyDeviceToHost, stream) == cudaSuccess,
           format + " enqueues D2H");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           format + " completes caller-owned stream");
    Expect(std::memcmp(&reference[0], &result[0],
                       result.size() * sizeof(result[0])) == 0,
           format + " CUDA output exactly matches CPU ATFP oracle");

    Expect(cuda_module.Finish().ok(), format + " module finishes");
    cudaStreamDestroy(stream);
    cudaFree(device_output);
    cudaFree(device_input);
}

}  // namespace

int main(int argc, char** argv) {
    if (IsHelpRequest(argc, argv)) {
        std::cout << "Usage: complex_convert_cuda_test\n";
        return 0;
    }
    if (argc != 1) {
        std::cerr << "Usage: complex_convert_cuda_test\n";
        return 2;
    }
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        std::cout << "SKIP: no CUDA device is available\n";
        return 77;
    }
    Expect(cudaSetDevice(0) == cudaSuccess, "select CUDA device 0");

    RunCase("CI8", 5, 1, 3, 1, 3, 0.25, 201, true);   // Q=5 < tile.
    RunCase("CI16", 17, 2, 3, 2, 2, 0.125, 202, false); // Q=68.
    RunCase("CI8", 65, 1, 37, 3, 3, 0.5, 203, false);  // 37x195.

    if (failures != 0) return 1;
    std::cout << "complex_convert_cuda_test passed\n";
    return 0;
}
