#include "rdma_dada/modules/time_integrate/time_integrate_module.h"

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void PrintUsage(std::ostream& output, const char* program) {
    output << "Usage: " << program << " [OPTIONS]\n"
           << "Run the CUDA time-integration correctness test on CUDA device 0.\n\n"
           << "Options:\n"
           << "  -h, --help  Show this help message\n\n"
           << "Exit codes: 0=passed/help, 1=failed, 2=invalid usage, "
              "77=no CUDA device\n";
}

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ExpectNear(float actual, float expected, const std::string& message) {
    if (std::fabs(actual - expected) > 1.0e-5f) {
        std::cerr << "FAIL: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        PrintUsage(std::cout, argv[0]);
        return 0;
    }
    if (argc != 1) {
        std::cerr << "Error: unexpected command-line argument\n";
        PrintUsage(std::cerr, argv[0]);
        return 2;
    }

    int device_count = 0;
    const cudaError_t count_status = cudaGetDeviceCount(&device_count);
    if (count_status != cudaSuccess || device_count == 0) {
        std::cout << "SKIP: no CUDA device is available\n";
        return 77;
    }

    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "POLARIZATION_PRODUCTS");
    input_header.SetString("ORDER", "TFBS");
    input_header.SetString("SAMPLE_FORMAT", "F32");
    input_header.SetString("MEMORY", "CUDA_DEVICE");
    input_header.SetUint64("CUDA_DEVICE", 0);
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NBEAM", 1);
    input_header.SetUint64("NPRODUCT", 4);
    input_header.SetUint64("RECORD_BYTES", 16);
    input_header.SetUint64("RESOLUTION", 16);
    input_header.SetUint64("BYTES_PER_SECOND", 160);
    input_header.SetDouble("TSAMP", 2.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CUDA");
    parameters.SetUint64("CUDA_DEVICE", 0);
    parameters.SetUint64("INTEGRATION_LENGTH", 2);
    parameters.SetString("INTEGRATION_OPERATION", "MEAN");

    rdma_dada::modules::time_integrate::TimeIntegrateModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "CUDA TFBS mean integration configures");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    const float host_input[] = {
        1.0f, 2.0f, 3.0f, 4.0f,
        10.0f, 20.0f, 30.0f, 40.0f,
        5.0f, 6.0f, 7.0f, 8.0f,
        50.0f, 60.0f, 70.0f, 80.0f
    };
    const float expected[] = {
        5.5f, 11.0f, 16.5f, 22.0f,
        27.5f, 33.0f, 38.5f, 44.0f
    };
    float host_output[8] = {};
    float* device_input = NULL;
    float* device_output = NULL;
    cudaStream_t stream = NULL;

    Expect(cudaSetDevice(0) == cudaSuccess, "select CUDA device 0");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create non-blocking CUDA stream");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      sizeof(host_input)) == cudaSuccess,
           "allocate CUDA integration input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output),
                      sizeof(host_output)) == cudaSuccess,
           "allocate CUDA integration output");
    if (failures != 0) {
        if (device_input) cudaFree(device_input);
        if (device_output) cudaFree(device_output);
        if (stream) cudaStreamDestroy(stream);
        return 1;
    }

    Expect(cudaMemcpyAsync(device_input, host_input, sizeof(host_input),
                           cudaMemcpyHostToDevice, stream) == cudaSuccess,
           "enqueue CUDA integration input copy");
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(device_input),
        sizeof(host_input), 93,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(device_output), sizeof(host_output),
        0, 0, rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda,
        0,
        reinterpret_cast<void*>(stream)
    };
    status = module.ProcessBlock(input, &output, context);
    Expect(status.ok(), "enqueue CUDA time-integration kernel");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(output.size == sizeof(host_output), "CUDA integration output size");
    Expect(output.sequence == 93, "CUDA integration preserves sequence");
    Expect(cudaMemcpyAsync(host_output, device_output, sizeof(host_output),
                           cudaMemcpyDeviceToHost, stream) == cudaSuccess,
           "enqueue CUDA integration output copy");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           "wait for CUDA integration result");
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(host_output[i], expected[i],
                   "known CUDA TFBS mean value");
    }

    const rdma_dada::pipeline::BlockExecutionContext default_stream_context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0, NULL
    };
    status = module.ProcessBlock(input, &output, default_stream_context);
    Expect(!status.ok(), "CUDA integration rejects null/default stream");

    status = module.Finish();
    Expect(status.ok(), "finish CUDA integration module");
    cudaFree(device_input);
    cudaFree(device_output);
    cudaStreamDestroy(stream);

    if (failures != 0) return 1;
    std::cout << "time_integrate_cuda_test passed\n";
    return 0;
}
