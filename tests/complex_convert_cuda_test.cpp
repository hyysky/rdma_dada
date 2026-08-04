#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
#include "rdma_dada/pipeline/complex32.h"

#include <cuda_runtime.h>

#include <cmath>
#include <cstdint>
#include <cstring>
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

bool IsHelpRequest(int argc, char** argv) {
    return argc == 2 &&
           (std::strcmp(argv[1], "-h") == 0 ||
            std::strcmp(argv[1], "--help") == 0);
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

    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "UNPACKED");
    input_header.SetString("ORDER", "TFPA");
    input_header.SetString("SAMPLE_FORMAT", "CI8");
    input_header.SetString("COMPONENT_ORDER", "RI");
    input_header.SetString("ENDIAN", "LITTLE");
    input_header.SetString("MEMORY", "CUDA_DEVICE");
    input_header.SetUint64("CUDA_DEVICE", 0);
    input_header.SetUint64("COMPONENT_NBIT", 8);
    input_header.SetUint64("COMPONENT_SIGNED", 1);
    input_header.SetUint64("SAMPLE_NBIT", 16);
    input_header.SetUint64("NCHAN", 1);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NANT", 2);
    input_header.SetUint64("RECORD_BYTES", 4);
    input_header.SetUint64("RESOLUTION", 4);
    input_header.SetUint64("BYTES_PER_SECOND", 40);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CUDA");
    parameters.SetUint64("CUDA_DEVICE", 0);
    parameters.SetDouble("CONVERSION_SCALE", 0.25);

    rdma_dada::modules::complex_convert::ComplexConvertModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "CUDA CI8 conversion configures");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    const std::int8_t host_input[] = {
        -128, 127, 8, -12,
        20, -24, 40, -48
    };
    const float expected[][2] = {
        {-32.0f, 31.75f}, {2.0f, -3.0f},
        {5.0f, -6.0f}, {10.0f, -12.0f}
    };
    rdma_dada::pipeline::Complex32 host_output[4] = {};
    std::uint8_t* device_input = NULL;
    std::uint8_t* device_output = NULL;
    cudaStream_t stream = NULL;
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      sizeof(host_input)) == cudaSuccess,
           "allocate CUDA CI8 input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output),
                      sizeof(host_output)) == cudaSuccess,
           "allocate CUDA CF32 output");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create nonblocking CUDA stream");
    if (!device_input || !device_output || !stream) return 1;
    Expect(cudaMemcpyAsync(device_input, host_input, sizeof(host_input),
                           cudaMemcpyHostToDevice, stream) == cudaSuccess,
           "enqueue CI8 input copy");

    const rdma_dada::pipeline::InputBlock input = {
        device_input, sizeof(host_input), 101,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock output = {
        device_output, sizeof(host_output), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0,
        reinterpret_cast<void*>(stream)
    };
    status = module.ProcessBlock(input, &output, context);
    Expect(status.ok(), "enqueue CUDA CI8 to CF32 conversion");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(output.size == sizeof(host_output), "CUDA output has CF32 size");
    Expect(output.sequence == 101, "CUDA conversion preserves sequence");
    Expect(cudaMemcpyAsync(host_output, device_output, sizeof(host_output),
                           cudaMemcpyDeviceToHost, stream) == cudaSuccess,
           "enqueue CUDA conversion output copy");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           "wait for CUDA conversion result");
    for (std::size_t i = 0; i < 4; ++i) {
        ExpectNear(host_output[i].real, expected[i][0],
                   "CUDA CI8 real component");
        ExpectNear(host_output[i].imag, expected[i][1],
                   "CUDA CI8 imag component");
    }

    const rdma_dada::pipeline::BlockExecutionContext null_stream_context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0, NULL
    };
    status = module.ProcessBlock(input, &output, null_stream_context);
    Expect(!status.ok(), "CUDA conversion rejects null/default stream");

    status = module.Finish();
    Expect(status.ok(), "finish CUDA conversion module");
    cudaFree(device_input);
    cudaFree(device_output);
    cudaStreamDestroy(stream);

    rdma_dada::pipeline::Metadata ci16_header = input_header;
    ci16_header.SetString("SAMPLE_FORMAT", "CI16");
    ci16_header.SetUint64("COMPONENT_NBIT", 16);
    ci16_header.SetUint64("SAMPLE_NBIT", 32);
    ci16_header.SetUint64("RECORD_BYTES", 8);
    ci16_header.SetUint64("RESOLUTION", 8);
    ci16_header.SetUint64("BYTES_PER_SECOND", 80);
    parameters.SetDouble("CONVERSION_SCALE", 0.125);

    rdma_dada::modules::complex_convert::ComplexConvertModule ci16_module;
    status = ci16_module.ConfigureHeader(
        ci16_header, parameters, &output_header);
    Expect(status.ok(), "CUDA CI16 conversion configures");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    const std::int16_t ci16_host_input[] = {
        -32768, 32767, -8, 12,
        40, -48, 800, -1600
    };
    const float ci16_expected[][2] = {
        {-4096.0f, 4095.875f}, {-1.0f, 1.5f},
        {5.0f, -6.0f}, {100.0f, -200.0f}
    };
    rdma_dada::pipeline::Complex32 ci16_host_output[4] = {};
    device_input = NULL;
    device_output = NULL;
    stream = NULL;
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      sizeof(ci16_host_input)) == cudaSuccess,
           "allocate CUDA CI16 input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output),
                      sizeof(ci16_host_output)) == cudaSuccess,
           "allocate CUDA CI16 output");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create CI16 nonblocking CUDA stream");
    if (!device_input || !device_output || !stream) return 1;
    Expect(cudaMemcpyAsync(device_input, ci16_host_input,
                           sizeof(ci16_host_input), cudaMemcpyHostToDevice,
                           stream) == cudaSuccess,
           "enqueue CI16 input copy");

    const rdma_dada::pipeline::InputBlock ci16_input = {
        device_input, sizeof(ci16_host_input), 102,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock ci16_output = {
        device_output, sizeof(ci16_host_output), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    const rdma_dada::pipeline::BlockExecutionContext ci16_context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0,
        reinterpret_cast<void*>(stream)
    };
    status = ci16_module.ProcessBlock(
        ci16_input, &ci16_output, ci16_context);
    Expect(status.ok(), "enqueue CUDA CI16 to CF32 conversion");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(ci16_output.size == sizeof(ci16_host_output),
           "CUDA CI16 output has CF32 size");
    Expect(ci16_output.sequence == 102,
           "CUDA CI16 conversion preserves sequence");
    Expect(cudaMemcpyAsync(ci16_host_output, device_output,
                           sizeof(ci16_host_output), cudaMemcpyDeviceToHost,
                           stream) == cudaSuccess,
           "enqueue CUDA CI16 output copy");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           "wait for CUDA CI16 conversion result");
    for (std::size_t i = 0; i < 4; ++i) {
        ExpectNear(ci16_host_output[i].real, ci16_expected[i][0],
                   "CUDA CI16 real component");
        ExpectNear(ci16_host_output[i].imag, ci16_expected[i][1],
                   "CUDA CI16 imag component");
    }
    status = ci16_module.Finish();
    Expect(status.ok(), "finish CUDA CI16 conversion module");
    cudaFree(device_input);
    cudaFree(device_output);
    cudaStreamDestroy(stream);

    if (failures != 0) return 1;
    std::cout << "complex_convert_cuda_test passed\n";
    return 0;
}
