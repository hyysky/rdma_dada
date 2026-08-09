#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>

namespace {

void Usage() {
    std::cout
        << "Usage: complex_convert_transpose_cuda_benchmark "
        << "CI8|CI16 NANT Q ITERATIONS SCALE CUDA_DEVICE\n";
}

bool ParseUint64(const char* text, std::uint64_t* value) {
    if (!text || !value || *text == '\0' || *text == '-') return false;
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (errno != 0 || !end || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ParseDouble(const char* text, double* value) {
    if (!text || !value || *text == '\0') return false;
    errno = 0;
    char* end = NULL;
    const double parsed = std::strtod(text, &end);
    if (errno != 0 || !end || *end != '\0' || !(parsed > 0.0)) return false;
    *value = parsed;
    return true;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result ||
        (left != 0 &&
         right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

rdma_dada::pipeline::Metadata MakeHeader(
    const std::string& format, std::uint64_t nant, std::uint64_t q,
    std::uint64_t input_bytes, int device) {
    const std::uint64_t component_bits = format == "CI8" ? 8 : 16;
    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "UNPACKED");
    header.SetString("ORDER", "ATFP");
    header.SetString("LAYOUT_SCOPE", "BLOCK");
    header.SetString("SAMPLE_FORMAT", format);
    header.SetString("SAMPLE_ENCODING", "TWOS_COMPLEMENT");
    header.SetString("COMPONENT_ORDER", "IQ");
    header.SetString("ENDIAN", "LITTLE");
    header.SetString("MEMORY", "CUDA_DEVICE");
    header.SetUint64("CUDA_DEVICE", static_cast<std::uint64_t>(device));
    header.SetUint64("COMPONENT_NBIT", component_bits);
    header.SetUint64("SAMPLE_NBIT", 2 * component_bits);
    header.SetUint64("NCHAN", q);
    header.SetUint64("NPOL", 1);
    header.SetUint64("NANT", nant);
    header.SetUint64("BLOCK_NTIME", 1);
    header.SetUint64("RESOLUTION", input_bytes);
    header.SetUint64("RECORD_BYTES", input_bytes);
    header.SetUint64("OUTPUT_BLOCK_BYTES", input_bytes);
    header.SetUint64("BYTES_PER_SECOND", input_bytes);
    header.SetUint64("TRANSFER_SIZE", input_bytes);
    return header;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        (std::strcmp(argv[1], "-h") == 0 ||
         std::strcmp(argv[1], "--help") == 0)) {
        Usage();
        return 0;
    }
    if (argc != 7) {
        Usage();
        return 2;
    }
    const std::string format(argv[1]);
    std::uint64_t nant = 0;
    std::uint64_t q = 0;
    std::uint64_t iterations = 0;
    std::uint64_t device_value = 0;
    double scale = 0.0;
    if ((format != "CI8" && format != "CI16") ||
        !ParseUint64(argv[2], &nant) || nant == 0 ||
        !ParseUint64(argv[3], &q) || q == 0 ||
        !ParseUint64(argv[4], &iterations) || iterations == 0 ||
        !ParseDouble(argv[5], &scale) ||
        !ParseUint64(argv[6], &device_value) ||
        device_value > static_cast<std::uint64_t>(
                           std::numeric_limits<int>::max())) {
        Usage();
        return 2;
    }
    const int device = static_cast<int>(device_value);
    const std::uint64_t input_sample_bytes = format == "CI8" ? 2 : 4;
    std::uint64_t sample_count = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    if (!CheckedMultiply(nant, q, &sample_count) ||
        !CheckedMultiply(sample_count, input_sample_bytes, &input_bytes) ||
        !CheckedMultiply(sample_count,
                         sizeof(rdma_dada::pipeline::Complex32),
                         &output_bytes) ||
        input_bytes > std::numeric_limits<std::size_t>::max() ||
        output_bytes > std::numeric_limits<std::size_t>::max()) {
        std::cerr << "benchmark geometry overflows\n";
        return 2;
    }

    if (cudaSetDevice(device) != cudaSuccess) {
        std::cerr << "cannot select CUDA device " << device << '\n';
        return 1;
    }
    std::uint8_t* host_input = NULL;
    std::uint8_t* device_input = NULL;
    std::uint8_t* device_output = NULL;
    cudaStream_t stream = NULL;
    cudaEvent_t start = NULL;
    cudaEvent_t stop = NULL;
    if (cudaHostAlloc(reinterpret_cast<void**>(&host_input),
                      static_cast<std::size_t>(input_bytes),
                      cudaHostAllocDefault) != cudaSuccess ||
        cudaMalloc(reinterpret_cast<void**>(&device_input),
                   static_cast<std::size_t>(input_bytes)) != cudaSuccess ||
        cudaMalloc(reinterpret_cast<void**>(&device_output),
                   static_cast<std::size_t>(output_bytes)) != cudaSuccess ||
        cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) !=
            cudaSuccess ||
        cudaEventCreate(&start) != cudaSuccess ||
        cudaEventCreate(&stop) != cudaSuccess) {
        std::cerr << "CUDA allocation or event creation failed\n";
        return 1;
    }
    std::memset(host_input, 1, static_cast<std::size_t>(input_bytes));

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CUDA");
    parameters.SetUint64("CUDA_DEVICE", device_value);
    parameters.SetDouble("CONVERSION_SCALE", scale);
    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status = module.ConfigureHeader(
        MakeHeader(format, nant, q, input_bytes, device), parameters,
        &output_header);
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    cudaEventRecord(start, stream);
    cudaMemcpyAsync(device_input, host_input,
                    static_cast<std::size_t>(input_bytes),
                    cudaMemcpyHostToDevice, stream);
    cudaEventRecord(stop, stream);
    cudaEventSynchronize(stop);
    float h2d_ms = 0.0f;
    cudaEventElapsedTime(&h2d_ms, start, stop);

    const rdma_dada::pipeline::InputBlock input = {
        device_input, input_bytes, 1,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock output = {
        device_output, output_bytes, 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, device,
        reinterpret_cast<void*>(stream)
    };

    status = module.ProcessBlock(input, &output, context);
    if (!status.ok() || cudaStreamSynchronize(stream) != cudaSuccess) {
        std::cerr << (status.ok() ? "CUDA warm-up failed" : status.message())
                  << '\n';
        return 1;
    }

    cudaEventRecord(start, stream);
    for (std::uint64_t i = 0; i < iterations; ++i) {
        status = module.ProcessBlock(input, &output, context);
        if (!status.ok()) {
            std::cerr << status.message() << '\n';
            return 1;
        }
    }
    cudaEventRecord(stop, stream);
    cudaEventSynchronize(stop);
    float total_kernel_ms = 0.0f;
    cudaEventElapsedTime(&total_kernel_ms, start, stop);

    const double kernel_seconds = total_kernel_ms * 1.0e-3;
    const double moved_bytes = static_cast<double>(iterations) *
                               static_cast<double>(input_bytes + output_bytes);
    const double effective_gbps = moved_bytes / kernel_seconds / 1.0e9;
    const double h2d_gbps =
        static_cast<double>(input_bytes) / (h2d_ms * 1.0e-3) / 1.0e9;
    std::cout << std::fixed << std::setprecision(6)
              << "format=" << format << " nant=" << nant << " q=" << q
              << " samples=" << sample_count
              << " input_bytes=" << input_bytes
              << " output_bytes=" << output_bytes
              << " warmup_runs=1 iterations=" << iterations
              << " h2d_ms=" << h2d_ms
              << " h2d_gbps=" << h2d_gbps
              << " kernel_ms_per_iteration="
              << total_kernel_ms / static_cast<double>(iterations)
              << " effective_input_output_gbps=" << effective_gbps << '\n';

    module.Finish();
    cudaEventDestroy(stop);
    cudaEventDestroy(start);
    cudaStreamDestroy(stream);
    cudaFree(device_output);
    cudaFree(device_input);
    cudaFreeHost(host_input);
    return 0;
}
