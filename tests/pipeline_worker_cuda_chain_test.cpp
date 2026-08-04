#include "rdma_dada/modules/device_to_host/device_to_host_module.h"
#include "rdma_dada/modules/host_to_device/host_to_device_module.h"
#include "rdma_dada/pipeline/complex32.h"
#include "rdma_dada/pipeline/module_chain.h"
#include "rdma_dada/pipeline/worker_config.h"

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void PrintUsage(std::ostream& output, const char* program) {
    output << "Usage: " << program << " WEIGHTS.npy\n"
           << "Validate H2D -> Beamform -> Power -> TimeIntegrate -> D2H "
              "on CUDA device 0.\n";
}

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void ExpectNear(float actual, float expected, const std::string& message) {
    if (std::fabs(actual - expected) > 1.0e-4f) {
        std::cerr << "FAIL: " << message << ": expected " << expected
                  << ", got " << actual << '\n';
        ++failures;
    }
}

rdma_dada::pipeline::Metadata MakeInputHeader() {
    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "CONVERTED");
    header.SetString("ORDER", "TFPA");
    header.SetString("SAMPLE_FORMAT", "CF32");
    header.SetString("MEMORY", "HOST");
    header.SetString("SOURCE", "cuda-worker-chain-test");
    header.SetUint64("NCHAN", 2);
    header.SetUint64("NPOL", 1);
    header.SetUint64("NANT", 2);
    header.SetUint64("RESOLUTION", 32);
    header.SetUint64("BYTES_PER_SECOND", 3200);
    header.SetUint64("TRANSFER_SIZE", 64);
    header.SetUint64("FILE_SIZE", 64);
    header.SetUint64("OBS_OFFSET", 0);
    header.SetDouble("TSAMP", 1.0);
    return header;
}

rdma_dada::pipeline::WorkerConfig MakeConfig(
    const std::string& weights_path) {
    rdma_dada::pipeline::WorkerConfig config = {};
    config.input_key = 0xdada;
    config.output_key = 0xdadb;
    config.input_key_text = "dada";
    config.output_key_text = "dadb";
    config.execution_backend = "CUDA";
    config.cuda_device = 0;
    config.run_once = true;
    config.nchan = 2;
    config.nant = 2;
    config.npol = 1;
    config.udp_payload_bytes = 16;
    config.samples_per_udp = 1;
    config.udp_packets_per_antenna_per_block = 2;
    config.weights_file = weights_path;
    config.weights_order = "FPAB2";
    config.weights_id = "cuda-worker-chain-test-v1";
    config.weights_scale = 0.5;
    config.nbeam = 2;
    config.compute_mode = "FP32";
    config.product = rdma_dada::pipeline::WorkerProduct::kPower;
    config.integration_enabled = true;
    config.integration_length = 2;
    config.integration_operation = "MEAN";
    return config;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        PrintUsage(std::cout, argv[0]);
        return 0;
    }
    if (argc != 2) {
        PrintUsage(std::cerr, argv[0]);
        return 2;
    }

    int device_count = 0;
    const cudaError_t count_status = cudaGetDeviceCount(&device_count);
    if (count_status != cudaSuccess || device_count == 0) {
        std::cout << "SKIP: no CUDA device is available\n";
        return 77;
    }

    const rdma_dada::pipeline::Metadata input_header = MakeInputHeader();
    const rdma_dada::pipeline::WorkerConfig config = MakeConfig(argv[1]);
    rdma_dada::pipeline::ModuleChain chain;
    rdma_dada::pipeline::Metadata chain_output_header;
    rdma_dada::pipeline::StageStatus status =
        chain.Configure(input_header, config, &chain_output_header);
    Expect(status.ok(), "configure CUDA Beamform/Power/Integration chain");
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return 1;
    }

    std::uint64_t scratch_bytes = 0;
    std::uint64_t output_bytes = 0;
    status = chain.PlanBlock(64, &scratch_bytes, &output_bytes);
    Expect(status.ok(), "plan one CUDA worker input block");
    Expect(scratch_bytes == 96, "chain scratch is 64-byte beamformed plus "
                                "32-byte power block");
    Expect(output_bytes == 16, "K=2 integration produces a 16-byte block");

    rdma_dada::pipeline::StageParameters transfer_parameters;
    transfer_parameters.SetString("EXECUTION_BACKEND", "CUDA");
    transfer_parameters.SetUint64("CUDA_DEVICE", 0);
    rdma_dada::modules::host_to_device::HostToDeviceModule h2d;
    rdma_dada::pipeline::Metadata device_input_header;
    status = h2d.ConfigureHeader(
        input_header, transfer_parameters, &device_input_header);
    Expect(status.ok(), "configure worker H2D stage");
    Expect(device_input_header.Fields() == chain.plan().input_header.Fields(),
           "H2D header matches module-chain CUDA input header");

    rdma_dada::pipeline::Metadata device_output_header = chain_output_header;
    device_output_header.SetString("MEMORY", "CUDA_DEVICE");
    device_output_header.SetUint64("CUDA_DEVICE", 0);
    transfer_parameters.SetString("OUTPUT_MEMORY", "HOST");
    rdma_dada::modules::device_to_host::DeviceToHostModule d2h;
    rdma_dada::pipeline::Metadata host_output_header;
    status = d2h.ConfigureHeader(
        device_output_header, transfer_parameters, &host_output_header);
    Expect(status.ok(), "configure worker D2H stage");
    Expect(host_output_header.Fields() == chain_output_header.Fields(),
           "D2H header matches the final host-ring header");
    std::string pipeline_modules;
    std::uint64_t header_value = 0;
    Expect(host_output_header.GetString(
               "PIPELINE_MODULES", &pipeline_modules) &&
               pipeline_modules == "beamform,power,time_integrate",
           "output header records the complete CUDA module chain");
    Expect(host_output_header.GetUint64(
               "BYTES_PER_SECOND", &header_value) && header_value == 800,
           "output header scales byte rate through Power and K=2 mean");
    Expect(host_output_header.GetUint64("BLOCK_NTIME", &header_value) &&
               header_value == 1,
           "output header publishes integrated block T");

    typedef rdma_dada::pipeline::Complex32 Complex32;
    const Complex32 host_input[] = {
        {1.0f, 2.0f}, {3.0f, 4.0f},
        {5.0f, 6.0f}, {7.0f, 8.0f},
        {2.0f, 0.0f}, {0.0f, 1.0f},
        {1.0f, -1.0f}, {2.0f, 2.0f}
    };
    float host_output[4] = {};
    std::uint8_t* device_input = NULL;
    std::uint8_t* device_scratch = NULL;
    std::uint8_t* device_output = NULL;
    cudaStream_t stream = NULL;
    Expect(cudaSetDevice(0) == cudaSuccess, "select CUDA device 0");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create worker-owned non-blocking stream");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      sizeof(host_input)) == cudaSuccess,
           "allocate worker device input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_scratch),
                      static_cast<std::size_t>(scratch_bytes)) == cudaSuccess,
           "allocate worker chain scratch");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output),
                      static_cast<std::size_t>(output_bytes)) == cudaSuccess,
           "allocate worker device output");
    if (failures != 0) {
        if (device_input) cudaFree(device_input);
        if (device_scratch) cudaFree(device_scratch);
        if (device_output) cudaFree(device_output);
        if (stream) cudaStreamDestroy(stream);
        return 1;
    }

    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0,
        reinterpret_cast<void*>(stream)
    };
    const rdma_dada::pipeline::InputBlock ring_input = {
        reinterpret_cast<const std::uint8_t*>(host_input),
        sizeof(host_input), 501,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock transferred_input = {
        device_input, sizeof(host_input), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    status = h2d.ProcessBlock(ring_input, &transferred_input, context);
    Expect(status.ok(), "enqueue worker H2D");

    const rdma_dada::pipeline::InputBlock chain_input = {
        transferred_input.data, transferred_input.size,
        transferred_input.sequence,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock chain_output = {
        device_output, output_bytes, 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    status = chain.ProcessBlock(
        chain_input, &chain_output, device_scratch, scratch_bytes, context);
    Expect(status.ok(), "enqueue Beamform/Power/Integration chain");
    Expect(chain_output.size == output_bytes,
           "chain reports integrated output bytes");
    Expect(chain_output.sequence == 501,
           "chain preserves input block sequence");

    const rdma_dada::pipeline::InputBlock transferred_output = {
        chain_output.data, chain_output.size, chain_output.sequence,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock ring_output = {
        reinterpret_cast<std::uint8_t*>(host_output), sizeof(host_output),
        0, 0, rdma_dada::pipeline::MemoryLocation::kHost
    };
    status = d2h.ProcessBlock(transferred_output, &ring_output, context);
    Expect(status.ok(), "enqueue worker D2H");
    Expect(cudaStreamSynchronize(stream) == cudaSuccess,
           "synchronize once after the complete CUDA worker chain");

    const float expected[] = {17.5f, 66.0f, 175.0f, 9.0f};
    for (std::size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        ExpectNear(host_output[i], expected[i],
                   "known Beamform/Power/Mean result");
    }
    Expect(ring_output.size == sizeof(host_output),
           "D2H reports the final block size");
    Expect(ring_output.sequence == 501,
           "D2H preserves the worker block sequence");

    status = chain.Finish();
    Expect(status.ok(), "finish module chain");
    status = h2d.Finish();
    Expect(status.ok(), "finish H2D");
    status = d2h.Finish();
    Expect(status.ok(), "finish D2H");
    cudaFree(device_input);
    cudaFree(device_scratch);
    cudaFree(device_output);
    cudaStreamDestroy(stream);

    if (failures != 0) return 1;
    std::cout << "pipeline_worker_cuda_chain_test passed\n";
    return 0;
}
