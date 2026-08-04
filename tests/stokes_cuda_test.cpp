#include "rdma_dada/modules/stokes/stokes_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void PrintUsage(std::ostream& output, const char* program) {
    output << "Usage: " << program << " [OPTIONS]\n"
           << "Run the CUDA Stokes-product correctness test on CUDA device 0.\n\n"
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
    input_header.SetString("DATA_STAGE", "BEAMFORMED");
    input_header.SetString("ORDER", "TFPB");
    input_header.SetString("SAMPLE_FORMAT", "CF32");
    input_header.SetString("MEMORY", "CUDA_DEVICE");
    input_header.SetString("POL_LABELS", "X,Y");
    input_header.SetUint64("NCHAN", 2);
    input_header.SetUint64("NPOL", 2);
    input_header.SetUint64("NBEAM", 2);
    input_header.SetUint64("BYTES_PER_SECOND", 6400);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CUDA");
    parameters.SetUint64("CUDA_DEVICE", 0);

    rdma_dada::modules::stokes::StokesModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "CUDA Stokes configures on device 0");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    typedef rdma_dada::pipeline::Complex32 Complex32;
    const Complex32 host_input[] = {
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
    float host_output[32] = {};
    Complex32* device_input = NULL;
    float* device_output = NULL;
    cudaStream_t stream = NULL;

    Expect(cudaSetDevice(0) == cudaSuccess, "select CUDA device 0");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create non-blocking CUDA stream");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      sizeof(host_input)) == cudaSuccess,
           "allocate CUDA Stokes input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output),
                      sizeof(host_output)) == cudaSuccess,
           "allocate CUDA Stokes output");
    if (failures != 0) {
        if (device_input) cudaFree(device_input);
        if (device_output) cudaFree(device_output);
        if (stream) cudaStreamDestroy(stream);
        return 1;
    }

    Expect(cudaMemcpyAsync(device_input, host_input, sizeof(host_input),
                           cudaMemcpyHostToDevice, stream) == cudaSuccess,
           "enqueue Stokes input copy");
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(device_input),
        sizeof(host_input),
        72,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock output = {
        reinterpret_cast<std::uint8_t*>(device_output),
        sizeof(host_output),
        0,
        0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda,
        0,
        reinterpret_cast<void*>(stream)
    };
    status = module.ProcessBlock(input, &output, context);
    Expect(status.ok(), "enqueue CUDA Stokes kernel");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(output.size == sizeof(host_output), "CUDA output byte size");
    Expect(output.sequence == 72, "CUDA output sequence");

    Expect(cudaMemcpyAsync(host_output, device_output, sizeof(host_output),
                           cudaMemcpyDeviceToHost, stream) == cudaSuccess,
           "enqueue Stokes output copy");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           "wait for CUDA Stokes result");
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(host_output[i], expected[i],
                   "CUDA known coherency product");
    }

    const rdma_dada::pipeline::BlockExecutionContext default_stream_context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0, NULL
    };
    status = module.ProcessBlock(input, &output, default_stream_context);
    Expect(!status.ok(), "CUDA Stokes rejects a null/default stream");

    status = module.Finish();
    Expect(status.ok(), "finish CUDA Stokes module");
    cudaFree(device_input);
    cudaFree(device_output);
    cudaStreamDestroy(stream);

    if (failures != 0) return 1;
    std::cout << "stokes_cuda_test passed\n";
    return 0;
}
