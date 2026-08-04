#include "rdma_dada/modules/device_to_host/device_to_host_module.h"
#include "rdma_dada/modules/host_to_device/host_to_device_module.h"

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void PrintUsage(std::ostream& output, const char* program) {
    output << "Usage: " << program << " [OPTIONS]\n"
           << "Run the CUDA host-to-device-to-host round-trip test on CUDA "
              "device 0.\n\n"
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

void ExpectString(const rdma_dada::pipeline::Metadata& metadata,
                  const std::string& key, const std::string& expected) {
    std::string actual;
    Expect(metadata.GetString(key, &actual) && actual == expected,
           key + " is preserved as " + expected);
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

    rdma_dada::pipeline::Metadata ring_header;
    ring_header.SetString("DATA_STAGE", "CONVERTED");
    ring_header.SetString("ORDER", "TFPA");
    ring_header.SetString("SAMPLE_FORMAT", "CF32");
    ring_header.SetString("MEMORY", "HOST");
    ring_header.SetString("SOURCE", "transfer-roundtrip");
    ring_header.SetUint64("NCHAN", 2);
    ring_header.SetUint64("NANT", 2);
    ring_header.SetUint64("NPOL", 2);
    ring_header.SetUint64("RESOLUTION", 64);
    ring_header.SetUint64("BYTES_PER_SECOND", 6400);

    rdma_dada::pipeline::StageParameters transfer_parameters;
    transfer_parameters.SetString("EXECUTION_BACKEND", "CUDA");
    transfer_parameters.SetUint64("CUDA_DEVICE", 0);

    rdma_dada::modules::host_to_device::HostToDeviceModule h2d;
    rdma_dada::pipeline::Metadata device_header;
    rdma_dada::pipeline::StageStatus status =
        h2d.ConfigureHeader(
            ring_header, transfer_parameters, &device_header);
    Expect(status.ok(), "configure H2D on device 0");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }
    ExpectString(device_header, "MEMORY", "CUDA_DEVICE");
    ExpectString(device_header, "DATA_STAGE", "CONVERTED");
    ExpectString(device_header, "ORDER", "TFPA");
    ExpectString(device_header, "SAMPLE_FORMAT", "CF32");
    ExpectString(device_header, "SOURCE", "transfer-roundtrip");

    rdma_dada::pipeline::StageParameters d2h_parameters =
        transfer_parameters;
    d2h_parameters.SetString("OUTPUT_MEMORY", "HOST");
    rdma_dada::modules::device_to_host::DeviceToHostModule d2h;
    rdma_dada::pipeline::Metadata output_header;
    status = d2h.ConfigureHeader(
        device_header, d2h_parameters, &output_header);
    Expect(status.ok(), "configure D2H on device 0");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }
    ExpectString(output_header, "MEMORY", "HOST");
    ExpectString(output_header, "DATA_STAGE", "CONVERTED");
    ExpectString(output_header, "ORDER", "TFPA");
    ExpectString(output_header, "SAMPLE_FORMAT", "CF32");
    ExpectString(output_header, "SOURCE", "transfer-roundtrip");

    const std::size_t block_bytes = 512;
    std::vector<std::uint8_t> input(block_bytes);
    std::vector<std::uint8_t> output(block_bytes, 0);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = static_cast<std::uint8_t>((i * 37U + 11U) & 0xffU);
    }

    std::uint8_t* device_block = NULL;
    cudaStream_t stream = NULL;
    Expect(cudaSetDevice(0) == cudaSuccess, "select CUDA device 0");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create non-blocking CUDA stream");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_block), block_bytes) ==
               cudaSuccess,
           "allocate device transfer block");
    if (failures != 0) {
        if (device_block) cudaFree(device_block);
        if (stream) cudaStreamDestroy(stream);
        return 1;
    }

    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda,
        0,
        reinterpret_cast<void*>(stream)
    };
    const rdma_dada::pipeline::InputBlock host_input = {
        &input[0], input.size(), 91,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock device_output = {
        device_block, block_bytes, 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    status = h2d.ProcessBlock(host_input, &device_output, context);
    Expect(status.ok(), "enqueue H2D copy");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(device_output.size == block_bytes, "H2D output byte size");
    Expect(device_output.sequence == 91, "H2D preserves sequence");

    const rdma_dada::pipeline::InputBlock device_input = {
        device_output.data, device_output.size, device_output.sequence,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock host_output = {
        &output[0], output.size(), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = d2h.ProcessBlock(device_input, &host_output, context);
    Expect(status.ok(), "enqueue D2H copy after H2D");
    if (!status.ok()) std::cerr << status.message() << '\n';
    Expect(host_output.size == block_bytes, "D2H output byte size");
    Expect(host_output.sequence == 91, "D2H preserves sequence");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           "wait for H2D-D2H transfer chain");
    Expect(std::memcmp(&input[0], &output[0], block_bytes) == 0,
           "H2D-D2H output is byte-identical to input");

    status = h2d.Finish();
    Expect(status.ok(), "finish H2D module");
    status = d2h.Finish();
    Expect(status.ok(), "finish D2H module");
    cudaFree(device_block);
    cudaStreamDestroy(stream);

    if (failures != 0) return 1;
    std::cout << "transfer_cuda_roundtrip_test passed\n";
    return 0;
}
