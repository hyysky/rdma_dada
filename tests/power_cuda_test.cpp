#include "rdma_dada/modules/power/power_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime_api.h>

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
    if (std::fabs(actual - expected) > 1.0e-5f) {
        std::cerr << "FAIL: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

}  // namespace

int main() {
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
    input_header.SetUint64("NCHAN", 2);
    input_header.SetUint64("NPOL", 2);
    input_header.SetUint64("NBEAM", 2);
    input_header.SetUint64("BYTES_PER_SECOND", 6400);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CUDA");
    parameters.SetUint64("CUDA_DEVICE", 0);

    rdma_dada::modules::power::PowerModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "CUDA Power configures on device 0");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    typedef rdma_dada::pipeline::Complex32 Complex32;
    const Complex32 host_input[] = {
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
    float host_output[16] = {};
    Complex32* device_input = NULL;
    float* device_output = NULL;
    cudaStream_t stream = NULL;

    Expect(cudaSetDevice(0) == cudaSuccess, "select CUDA device 0");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create non-blocking CUDA stream");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      sizeof(host_input)) == cudaSuccess,
           "allocate CUDA Power input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output),
                      sizeof(host_output)) == cudaSuccess,
           "allocate CUDA Power output");
    if (failures != 0) {
        if (device_input) cudaFree(device_input);
        if (device_output) cudaFree(device_output);
        if (stream) cudaStreamDestroy(stream);
        return 1;
    }

    Expect(cudaMemcpyAsync(device_input, host_input, sizeof(host_input),
                           cudaMemcpyHostToDevice, stream) == cudaSuccess,
           "enqueue Power input copy");
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(device_input),
        sizeof(host_input),
        52,
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
    Expect(status.ok(), "enqueue CUDA Power kernel");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(output.size == sizeof(host_output), "CUDA output byte size");
    Expect(output.sequence == 52, "CUDA output sequence");

    Expect(cudaMemcpyAsync(host_output, device_output, sizeof(host_output),
                           cudaMemcpyDeviceToHost, stream) == cudaSuccess,
           "enqueue Power output copy");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           "wait for CUDA Power result");
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(host_output[i], expected[i], "CUDA known power value");
    }

    const rdma_dada::pipeline::BlockExecutionContext default_stream_context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0, NULL
    };
    status = module.ProcessBlock(input, &output, default_stream_context);
    Expect(!status.ok(), "CUDA Power rejects a null/default stream");

    status = module.Finish();
    Expect(status.ok(), "finish CUDA Power module");
    cudaFree(device_input);
    cudaFree(device_output);
    cudaStreamDestroy(stream);

    if (failures != 0) return 1;
    std::cout << "power_cuda_test passed\n";
    return 0;
}
