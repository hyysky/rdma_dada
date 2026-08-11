#include "rdma_dada/modules/time_integrate/time_integrate_module.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result ||
        (left != 0 && right > std::numeric_limits<std::uint64_t>::max() /
                                  left)) {
        return false;
    }
    *result = left * right;
    return true;
}

bool FitsSizeT(std::uint64_t value) {
    return value <= static_cast<std::uint64_t>(
                        std::numeric_limits<std::size_t>::max());
}

bool CheckCuda(cudaError_t status, const char* operation) {
    if (status == cudaSuccess) return true;
    std::cerr << operation << " failed: " << cudaGetErrorString(status)
              << '\n';
    return false;
}

void PrintUsage(std::ostream& output, const char* program) {
    output << "Usage: " << program
           << " [NTIME FRAME_ELEMENTS K ITERATIONS [SUM|MEAN [DEVICE]]]\n"
           << "Measures only CUDA time-integration kernel submissions using "
              "CUDA events; H2D/D2H are outside the timed region.\n";
}

struct CudaResources {
    CudaResources()
        : input(NULL), output(NULL), stream(NULL), start(NULL), stop(NULL) {}

    ~CudaResources() {
        if (stream) cudaStreamSynchronize(stream);
        if (start) cudaEventDestroy(start);
        if (stop) cudaEventDestroy(stop);
        if (input) cudaFree(input);
        if (output) cudaFree(output);
        if (stream) cudaStreamDestroy(stream);
    }

    float* input;
    float* output;
    cudaStream_t stream;
    cudaEvent_t start;
    cudaEvent_t stop;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        PrintUsage(std::cout, argv[0]);
        return 0;
    }
    const std::uint64_t ntime = argc > 1 ? std::strtoull(argv[1], NULL, 10)
                                         : UINT64_C(65536);
    const std::uint64_t frame_elements =
        argc > 2 ? std::strtoull(argv[2], NULL, 10) : UINT64_C(180);
    const std::uint64_t integration_length =
        argc > 3 ? std::strtoull(argv[3], NULL, 10) : UINT64_C(128);
    const int iterations = argc > 4 ? std::atoi(argv[4]) : 200;
    const std::string operation = argc > 5 ? argv[5] : "MEAN";
    const int device = argc > 6 ? std::atoi(argv[6]) : 0;
    if (argc > 7 || ntime == 0 || frame_elements == 0 ||
        integration_length == 0 || ntime % integration_length != 0 ||
        iterations <= 0 || device < 0 ||
        (operation != "SUM" && operation != "MEAN")) {
        PrintUsage(std::cerr, argv[0]);
        return 2;
    }

    int device_count = 0;
    if (!CheckCuda(cudaGetDeviceCount(&device_count), "cudaGetDeviceCount")) {
        return 1;
    }
    if (device_count == 0) {
        std::cout << "SKIP: no CUDA device is available\n";
        return 77;
    }
    if (device >= device_count) {
        std::cerr << "requested CUDA device is unavailable\n";
        return 2;
    }

    std::uint64_t frame_bytes = 0;
    std::uint64_t input_elements = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t bytes_per_second = 0;
    if (!CheckedMultiply(frame_elements, sizeof(float), &frame_bytes) ||
        !CheckedMultiply(ntime, frame_elements, &input_elements) ||
        !CheckedMultiply(input_elements, sizeof(float), &input_bytes) ||
        !CheckedMultiply(frame_bytes, integration_length,
                         &bytes_per_second)) {
        std::cerr << "benchmark geometry overflows\n";
        return 2;
    }
    const std::uint64_t output_elements =
        input_elements / integration_length;
    if (!CheckedMultiply(output_elements, sizeof(float), &output_bytes) ||
        !FitsSizeT(input_elements) || !FitsSizeT(input_bytes) ||
        !FitsSizeT(output_elements) || !FitsSizeT(output_bytes)) {
        std::cerr << "benchmark geometry exceeds addressable memory\n";
        return 2;
    }

    std::vector<float> host_input(
        static_cast<std::size_t>(input_elements));
    std::vector<float> host_output(
        static_cast<std::size_t>(output_elements));
    std::vector<float> expected(
        static_cast<std::size_t>(output_elements));
    for (std::size_t index = 0; index < host_input.size(); ++index) {
        host_input[index] = static_cast<float>(
            static_cast<int>(index % 251U) - 125) * 0.0078125f;
    }
    const std::uint64_t output_ntime = ntime / integration_length;
    for (std::uint64_t output_time = 0;
         output_time < output_ntime; ++output_time) {
        for (std::uint64_t element = 0;
             element < frame_elements; ++element) {
            float sum = 0.0f;
            for (std::uint64_t integration_index = 0;
                 integration_index < integration_length;
                 ++integration_index) {
                const std::uint64_t input_time =
                    output_time * integration_length + integration_index;
                sum += host_input[
                    static_cast<std::size_t>(
                        input_time * frame_elements + element)];
            }
            expected[static_cast<std::size_t>(
                output_time * frame_elements + element)] =
                operation == "MEAN" ?
                    sum / static_cast<float>(integration_length) : sum;
        }
    }

    if (!CheckCuda(cudaSetDevice(device), "cudaSetDevice")) return 1;
    CudaResources resources;
    if (!CheckCuda(cudaStreamCreateWithFlags(
                       &resources.stream, cudaStreamNonBlocking),
                   "cudaStreamCreateWithFlags") ||
        !CheckCuda(cudaEventCreate(&resources.start),
                   "cudaEventCreate(start)") ||
        !CheckCuda(cudaEventCreate(&resources.stop),
                   "cudaEventCreate(stop)") ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&resources.input),
                              static_cast<std::size_t>(input_bytes)),
                   "cudaMalloc(input)") ||
        !CheckCuda(cudaMalloc(reinterpret_cast<void**>(&resources.output),
                              static_cast<std::size_t>(output_bytes)),
                   "cudaMalloc(output)") ||
        !CheckCuda(cudaMemcpyAsync(
                       resources.input, &host_input[0],
                       static_cast<std::size_t>(input_bytes),
                       cudaMemcpyHostToDevice, resources.stream),
                   "cudaMemcpyAsync(H2D)")) {
        return 1;
    }

    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "POWER");
    header.SetString("ORDER", "TFPB");
    header.SetString("SAMPLE_FORMAT", "F32");
    header.SetString("MEMORY", "CUDA_DEVICE");
    header.SetUint64("CUDA_DEVICE", static_cast<std::uint64_t>(device));
    header.SetUint64("NCHAN", 1);
    header.SetUint64("NPOL", 1);
    header.SetUint64("NBEAM", frame_elements);
    header.SetUint64("RECORD_BYTES", frame_bytes);
    header.SetUint64("RESOLUTION", frame_bytes);
    header.SetUint64("BYTES_PER_SECOND", bytes_per_second);
    header.SetDouble("TSAMP", 1.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CUDA");
    parameters.SetUint64("CUDA_DEVICE", static_cast<std::uint64_t>(device));
    parameters.SetUint64("INTEGRATION_LENGTH", integration_length);
    parameters.SetString("INTEGRATION_OPERATION", operation);

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(header, parameters, &output_header);
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }
    const rdma_dada::pipeline::InputBlock input_block = {
        reinterpret_cast<const std::uint8_t*>(resources.input), input_bytes, 1,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock output_block = {
        reinterpret_cast<std::uint8_t*>(resources.output), output_bytes, 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, device,
        reinterpret_cast<void*>(resources.stream)
    };

    for (int warmup = 0; warmup < 5; ++warmup) {
        status = module.ProcessBlock(input_block, &output_block, context);
        if (!status.ok()) {
            std::cerr << status.message() << '\n';
            return 1;
        }
    }
    if (!CheckCuda(cudaEventRecord(resources.start, resources.stream),
                   "cudaEventRecord(start)")) {
        return 1;
    }
    for (int iteration = 0; iteration < iterations; ++iteration) {
        status = module.ProcessBlock(input_block, &output_block, context);
        if (!status.ok()) {
            std::cerr << status.message() << '\n';
            return 1;
        }
    }
    if (!CheckCuda(cudaEventRecord(resources.stop, resources.stream),
                   "cudaEventRecord(stop)") ||
        !CheckCuda(cudaEventSynchronize(resources.stop),
                   "cudaEventSynchronize(stop)")) {
        return 1;
    }
    float elapsed_ms = 0.0f;
    if (!CheckCuda(cudaEventElapsedTime(
                       &elapsed_ms, resources.start, resources.stop),
                   "cudaEventElapsedTime") ||
        elapsed_ms <= 0.0f ||
        !CheckCuda(cudaMemcpyAsync(
                       &host_output[0], resources.output,
                       static_cast<std::size_t>(output_bytes),
                       cudaMemcpyDeviceToHost, resources.stream),
                   "cudaMemcpyAsync(D2H)") ||
        !CheckCuda(cudaStreamSynchronize(resources.stream),
                   "cudaStreamSynchronize")) {
        return 1;
    }

    for (std::size_t index = 0; index < expected.size(); ++index) {
        const float tolerance =
            1.0e-5f * std::max(1.0f, std::fabs(expected[index]));
        if (std::fabs(host_output[index] - expected[index]) > tolerance) {
            std::cerr << "CUDA result mismatch at element " << index
                      << ": expected " << expected[index]
                      << ", got " << host_output[index] << '\n';
            return 1;
        }
    }
    status = module.Finish();
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    const double elapsed_seconds = elapsed_ms / 1000.0;
    const double input_gb = static_cast<double>(input_bytes) * iterations /
                            1.0e9;
    std::cout << std::fixed << std::setprecision(3)
              << "device=" << device
              << " ntime=" << ntime
              << " frame_elements=" << frame_elements
              << " K=" << integration_length
              << " operation=" << operation
              << " iterations=" << iterations
              << " kernel_ms=" << elapsed_ms
              << " us_per_block=" << elapsed_ms * 1000.0 / iterations
              << " input_GBps=" << input_gb / elapsed_seconds << '\n';
    return 0;
}
