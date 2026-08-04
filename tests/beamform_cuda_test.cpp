#include "rdma_dada/modules/beamform/beamform_module.h"

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
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

bool WriteInt8Weights(const std::string& path) {
    std::string header =
        "{'descr': '|i1', 'fortran_order': False, "
        "'shape': (2, 1, 2, 2, 2), }";
    while ((10 + header.size() + 1) % 16 != 0) header.push_back(' ');
    header.push_back('\n');

    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const unsigned char prefix[] = {
        0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<unsigned char>(header.size() & 0xff),
        static_cast<unsigned char>((header.size() >> 8) & 0xff)
    };
    const std::int8_t weights[] = {
        // f0: [[1, 2], [i, 1-i]] after scale=0.5.
        2, 0, 4, 0, 0, 2, 2, -2,
        // f1: [[1, i], [1, -i]].
        2, 0, 0, 2, 2, 0, 0, -2
    };
    output.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(weights), sizeof(weights));
    return output.good();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: beamform_cuda_test WEIGHTS_PATH FP32|TF32\n";
        return 2;
    }

    int device_count = 0;
    const cudaError_t count_status = cudaGetDeviceCount(&device_count);
    if (count_status != cudaSuccess || device_count == 0) {
        std::cout << "SKIP: no CUDA device is available\n";
        return 77;
    }

    const std::string weights_path = argv[1];
    const std::string compute_mode = argv[2];
    if (compute_mode != "FP32" && compute_mode != "TF32") {
        std::cerr << "compute mode must be FP32 or TF32\n";
        return 2;
    }
    if (!WriteInt8Weights(weights_path)) {
        std::cerr << "failed to create test NPY file\n";
        return 2;
    }

    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "CONVERTED");
    input_header.SetString("ORDER", "TFPA");
    input_header.SetString("SAMPLE_FORMAT", "CF32");
    input_header.SetString("MEMORY", "CUDA_DEVICE");
    input_header.SetUint64("NCHAN", 2);
    input_header.SetUint64("NPOL", 1);
    input_header.SetUint64("NANT", 2);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("WEIGHTS_FILE", weights_path);
    parameters.SetString("WEIGHTS_ORDER", "FPAB2");
    parameters.SetString("WEIGHTS_ID", "cuda-unit-test-v1");
    parameters.SetDouble("WEIGHTS_SCALE", 0.5);
    parameters.SetUint64("NBEAM", 2);
    parameters.SetString("COMPUTE_MODE", compute_mode);
    parameters.SetString("EXECUTION_BACKEND", "CUDA");
    parameters.SetUint64("CUDA_DEVICE", 0);

    rdma_dada::modules::beamform::BeamformModule module;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        module.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "CUDA backend configures with requested compute mode");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        std::remove(weights_path.c_str());
        return 1;
    }

    typedef rdma_dada::modules::beamform::Complex32 Complex32;
    const Complex32 host_input[] = {
        // t0, f0 then f1.
        {1.0f, 2.0f}, {3.0f, 4.0f},
        {5.0f, 6.0f}, {7.0f, 8.0f},
        // t1, f0 then f1.
        {2.0f, 0.0f}, {0.0f, 1.0f},
        {1.0f, -1.0f}, {2.0f, 2.0f}
    };
    Complex32 host_output[8] = {};
    Complex32* device_input = NULL;
    Complex32* device_output = NULL;
    cudaStream_t stream = NULL;

    Expect(cudaSetDevice(0) == cudaSuccess, "select CUDA device 0");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create non-blocking CUDA stream");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      sizeof(host_input)) == cudaSuccess,
           "allocate device input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output),
                      sizeof(host_output)) == cudaSuccess,
           "allocate device output");
    if (failures != 0) {
        if (device_input) cudaFree(device_input);
        if (device_output) cudaFree(device_output);
        if (stream) cudaStreamDestroy(stream);
        std::remove(weights_path.c_str());
        return 1;
    }

    Expect(cudaMemcpyAsync(device_input, host_input, sizeof(host_input),
                           cudaMemcpyHostToDevice, stream) == cudaSuccess,
           "enqueue input copy");
    const rdma_dada::pipeline::InputBlock input = {
        reinterpret_cast<const std::uint8_t*>(device_input),
        sizeof(host_input),
        23,
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
    Expect(status.ok(), "enqueue CUDA beamforming");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(output.size == sizeof(host_output), "CUDA output byte size");
    Expect(output.sequence == 23, "CUDA output sequence");

    Expect(cudaMemcpyAsync(host_output, device_output, sizeof(host_output),
                           cudaMemcpyDeviceToHost, stream) == cudaSuccess,
           "enqueue output copy");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           "wait for CUDA result");
    const Complex32 expected[] = {
        {-3.0f, 5.0f}, {9.0f, 5.0f},
        {12.0f, 14.0f}, {2.0f, -2.0f},
        {1.0f, 0.0f}, {5.0f, 1.0f},
        {3.0f, 1.0f}, {3.0f, -1.0f}
    };
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(host_output[i].real, expected[i].real,
                   "CUDA multi-frame/batch real");
        ExpectNear(host_output[i].imag, expected[i].imag,
                   "CUDA multi-frame/batch imaginary");
    }

    status = module.Finish();
    Expect(status.ok(), "finish CUDA beamform module");
    cudaFree(device_input);
    cudaFree(device_output);
    cudaStreamDestroy(stream);
    std::remove(weights_path.c_str());

    if (failures != 0) return 1;
    std::cout << "beamform_cuda_test passed\n";
    return 0;
}
