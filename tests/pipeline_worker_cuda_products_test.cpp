#include "rdma_dada/modules/complex_convert/complex_convert_module.h"
#include "rdma_dada/modules/device_to_host/device_to_host_module.h"
#include "rdma_dada/modules/host_to_device/host_to_device_module.h"
#include "rdma_dada/pipeline/complex32.h"
#include "rdma_dada/pipeline/module_chain.h"
#include "rdma_dada/pipeline/worker_config.h"

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void PrintUsage(std::ostream& output, const char* program) {
    output << "Usage: " << program << " WEIGHTS.npy\n"
           << "Validate CUDA worker numerical output for Beamform-only, "
              "Power and Stokes.\n";
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

bool WriteIdentityWeights(const std::string& path) {
    std::string header =
        "{'descr': '|i1', 'fortran_order': False, "
        "'shape': (2, 2, 2, 2, 2), }";
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
        1, 0, 0, 0, 0, 0, 1, 0,
        1, 0, 0, 0, 0, 0, 1, 0,
        1, 0, 0, 0, 0, 0, 1, 0,
        1, 0, 0, 0, 0, 0, 1, 0
    };
    output.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(weights), sizeof(weights));
    return output.good();
}

rdma_dada::pipeline::Metadata MakeInputHeader() {
    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "UNPACKED");
    header.SetString("ORDER", "ATFP");
    header.SetString("LAYOUT_SCOPE", "BLOCK");
    header.SetString("SAMPLE_FORMAT", "CI8");
    header.SetString("SAMPLE_ENCODING", "TWOS_COMPLEMENT");
    header.SetString("COMPONENT_ORDER", "IQ");
    header.SetString("ENDIAN", "LITTLE");
    header.SetString("MEMORY", "HOST");
    header.SetString("POL_LABELS", "X,Y");
    header.SetString("SOURCE", "cuda-worker-products-test");
    header.SetUint64("NCHAN", 2);
    header.SetUint64("NPOL", 2);
    header.SetUint64("NANT", 2);
    header.SetUint64("COMPONENT_NBIT", 8);
    header.SetUint64("SAMPLE_NBIT", 16);
    header.SetUint64("BLOCK_NTIME", 2);
    header.SetUint64("RESOLUTION", 16);
    header.SetUint64("RECORD_BYTES", 32);
    header.SetUint64("OUTPUT_BLOCK_BYTES", 32);
    header.SetUint64("BYTES_PER_SECOND", 1600);
    header.SetUint64("TRANSFER_SIZE", 32);
    header.SetUint64("FILE_SIZE", 32);
    header.SetUint64("OBS_OFFSET", 0);
    header.SetDouble("TSAMP", 1.0);
    return header;
}

rdma_dada::pipeline::WorkerConfig MakeConfig(
    const std::string& weights_path,
    rdma_dada::pipeline::WorkerProduct product) {
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
    config.npol = 2;
    config.udp_payload_bytes = 8;
    config.samples_per_udp = 1;
    config.udp_packets_per_antenna_per_block = 2;
    config.conversion_scale = 1.0;
    config.weights_file = weights_path;
    config.weights_order = "FPAB2";
    config.weights_id = "cuda-worker-products-identity-v1";
    config.weights_scale = 1.0;
    config.nbeam = 2;
    config.compute_mode = "FP32";
    config.product = product;
    config.integration_enabled = false;
    config.integration_length = 1;
    config.integration_operation = "MEAN";
    return config;
}

bool RunProduct(
    const std::string& weights_path,
    rdma_dada::pipeline::WorkerProduct product,
    const char* expected_modules,
    const std::vector<float>& expected,
    const std::uint8_t* device_converted,
    std::uint64_t converted_bytes,
    std::uint8_t* device_scratch,
    std::uint64_t scratch_capacity,
    std::uint8_t* device_output,
    std::uint64_t output_capacity,
    cudaStream_t stream,
    const rdma_dada::pipeline::Metadata& converted_header) {
    const rdma_dada::pipeline::WorkerConfig config =
        MakeConfig(weights_path, product);
    rdma_dada::pipeline::ModuleChain chain;
    rdma_dada::pipeline::Metadata chain_output_header;
    rdma_dada::pipeline::StageStatus status =
        chain.Configure(converted_header, config, &chain_output_header);
    Expect(status.ok(), std::string("configure ") + expected_modules);
    if (!status.ok()) {
        std::cerr << status.message() << '\n';
        return false;
    }

    std::uint64_t scratch_bytes = 0;
    std::uint64_t output_bytes = 0;
    status = chain.PlanBlock(converted_bytes, &scratch_bytes, &output_bytes);
    Expect(status.ok(), std::string("plan ") + expected_modules);
    Expect(scratch_bytes <= scratch_capacity,
           std::string(expected_modules) + " scratch fits worker capacity");
    Expect(output_bytes == expected.size() * sizeof(float),
           std::string(expected_modules) + " output size is exact");
    if (!status.ok() || scratch_bytes > scratch_capacity ||
        output_bytes > output_capacity) {
        chain.Finish();
        return false;
    }

    std::string modules;
    Expect(chain_output_header.GetString("PIPELINE_MODULES", &modules) &&
               modules == expected_modules,
           std::string(expected_modules) + " header records selected chain");

    rdma_dada::pipeline::Metadata device_output_header = chain_output_header;
    device_output_header.SetString("MEMORY", "CUDA_DEVICE");
    device_output_header.SetUint64("CUDA_DEVICE", 0);
    rdma_dada::pipeline::StageParameters transfer_parameters;
    transfer_parameters.SetString("EXECUTION_BACKEND", "CUDA");
    transfer_parameters.SetUint64("CUDA_DEVICE", 0);
    transfer_parameters.SetString("OUTPUT_MEMORY", "HOST");
    rdma_dada::modules::device_to_host::DeviceToHostModule d2h;
    rdma_dada::pipeline::Metadata host_output_header;
    status = d2h.ConfigureHeader(
        device_output_header, transfer_parameters, &host_output_header);
    Expect(status.ok(), std::string("configure D2H for ") + expected_modules);
    Expect(host_output_header.Fields() == chain_output_header.Fields(),
           std::string(expected_modules) + " D2H publishes host-ring header");

    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0,
        reinterpret_cast<void*>(stream)
    };
    const rdma_dada::pipeline::InputBlock input = {
        device_converted, converted_bytes, 902,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock output = {
        device_output, output_capacity, 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    status = chain.ProcessBlock(
        input, &output, device_scratch, scratch_capacity, context);
    Expect(status.ok(), std::string("enqueue ") + expected_modules);
    Expect(output.size == output_bytes,
           std::string(expected_modules) + " reports exact output bytes");
    Expect(output.sequence == 902,
           std::string(expected_modules) + " preserves block sequence");

    std::vector<float> actual(expected.size(), 0.0f);
    if (status.ok()) {
        const rdma_dada::pipeline::InputBlock device_result = {
            output.data, output.size, output.sequence,
            rdma_dada::pipeline::MemoryLocation::kCudaDevice
        };
        rdma_dada::pipeline::OutputBlock host_result = {
            reinterpret_cast<std::uint8_t*>(&actual[0]), output_bytes,
            0, 0, rdma_dada::pipeline::MemoryLocation::kHost
        };
        status = d2h.ProcessBlock(device_result, &host_result, context);
        Expect(status.ok(),
               std::string("enqueue D2H result for ") + expected_modules);
        Expect(host_result.size == output_bytes && host_result.sequence == 902,
               std::string(expected_modules) +
                   " D2H preserves output size and sequence");
        Expect(cudaStreamSynchronize(stream) == cudaSuccess,
               std::string("synchronize ") + expected_modules);
        for (std::size_t i = 0; i < expected.size(); ++i) {
            ExpectNear(actual[i], expected[i],
                       std::string(expected_modules) + " value " +
                           std::to_string(i));
        }
    }
    status = d2h.Finish();
    Expect(status.ok(), std::string("finish D2H for ") + expected_modules);
    status = chain.Finish();
    Expect(status.ok(), std::string("finish ") + expected_modules);
    return status.ok();
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
    if (!WriteIdentityWeights(argv[1])) {
        std::cerr << "failed to create P=2 worker test weights\n";
        return 2;
    }

    const std::int8_t host_input[] = {
        1, 2, 5, 6, 2, 0, -1, 2,
        -2, 3, 0, -1, 3, -2, 2, 1,
        3, 4, 7, 8, 1, -1, 4, 1,
        1, 0, 2, 2, -1, -3, 0, 4
    };
    const std::vector<float> expected_beamformed = {
        1, 2, 3, 4, 5, 6, 7, 8,
        2, 0, 1, -1, -1, 2, 4, 1,
        -2, 3, 1, 0, 0, -1, 2, 2,
        3, -2, -1, -3, 2, 1, 0, 4
    };
    const std::vector<float> expected_power = {
        5, 25, 61, 113, 4, 2, 5, 17,
        13, 1, 1, 8, 13, 10, 5, 16
    };
    const std::vector<float> expected_stokes = {
        5, 61, 17, 4, 25, 113, 53, 4,
        4, 5, -2, -4, 2, 17, 3, -5,
        13, 1, -3, -2, 1, 8, 2, -2,
        13, 5, 4, -7, 10, 16, -12, 4
    };

    cudaStream_t stream = NULL;
    std::uint8_t* device_input = NULL;
    std::uint8_t* device_converted = NULL;
    std::uint8_t* device_scratch = NULL;
    std::uint8_t* device_output = NULL;
    Expect(cudaSetDevice(0) == cudaSuccess, "select CUDA device 0");
    Expect(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) ==
               cudaSuccess,
           "create worker-owned non-blocking stream");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_input),
                      sizeof(host_input)) == cudaSuccess,
           "allocate ATFP device input");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_converted), 128) ==
               cudaSuccess,
           "allocate converted TFPA device block");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_scratch), 128) ==
               cudaSuccess,
           "allocate product-chain scratch");
    Expect(cudaMalloc(reinterpret_cast<void**>(&device_output), 128) ==
               cudaSuccess,
           "allocate product output");
    if (failures != 0) {
        if (device_input) cudaFree(device_input);
        if (device_converted) cudaFree(device_converted);
        if (device_scratch) cudaFree(device_scratch);
        if (device_output) cudaFree(device_output);
        if (stream) cudaStreamDestroy(stream);
        std::remove(argv[1]);
        return 1;
    }

    rdma_dada::pipeline::StageParameters transfer_parameters;
    transfer_parameters.SetString("EXECUTION_BACKEND", "CUDA");
    transfer_parameters.SetUint64("CUDA_DEVICE", 0);
    rdma_dada::modules::host_to_device::HostToDeviceModule h2d;
    const rdma_dada::pipeline::Metadata input_header = MakeInputHeader();
    rdma_dada::pipeline::Metadata device_input_header;
    rdma_dada::pipeline::StageStatus status = h2d.ConfigureHeader(
        input_header, transfer_parameters, &device_input_header);
    Expect(status.ok(), "configure products-test H2D");

    rdma_dada::pipeline::StageParameters conversion_parameters;
    conversion_parameters.SetString("EXECUTION_BACKEND", "CUDA");
    conversion_parameters.SetUint64("CUDA_DEVICE", 0);
    conversion_parameters.SetDouble("CONVERSION_SCALE", 1.0);
    rdma_dada::modules::complex_convert::ComplexConvertModule conversion;
    rdma_dada::pipeline::Metadata converted_header;
    status = conversion.ConfigureHeader(
        device_input_header, conversion_parameters, &converted_header);
    Expect(status.ok(),
           "configure products-test ATFP conversion: " + status.message());

    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kCuda, 0,
        reinterpret_cast<void*>(stream)
    };
    const rdma_dada::pipeline::InputBlock ring_input = {
        reinterpret_cast<const std::uint8_t*>(host_input), sizeof(host_input),
        902, rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock transferred_input = {
        device_input, sizeof(host_input), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    status = h2d.ProcessBlock(ring_input, &transferred_input, context);
    Expect(status.ok(), "enqueue products-test H2D");
    const rdma_dada::pipeline::InputBlock conversion_input = {
        transferred_input.data, transferred_input.size,
        transferred_input.sequence,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    rdma_dada::pipeline::OutputBlock conversion_output = {
        device_converted, 128, 0, 0,
        rdma_dada::pipeline::MemoryLocation::kCudaDevice
    };
    status = conversion.ProcessBlock(conversion_input, &conversion_output,
                                     context);
    Expect(status.ok(), "enqueue products-test fused conversion");
    Expect(conversion_output.size == 128,
           "conversion reports exact TFPA CF32 bytes");

    if (failures == 0) {
        RunProduct(argv[1], rdma_dada::pipeline::WorkerProduct::kBeamformed,
                   "beamform", expected_beamformed, device_converted, 128,
                   device_scratch, 128, device_output, 128, stream,
                   converted_header);
        RunProduct(argv[1], rdma_dada::pipeline::WorkerProduct::kPower,
                   "beamform,power", expected_power, device_converted, 128,
                   device_scratch, 128, device_output, 128, stream,
                   converted_header);
        RunProduct(argv[1], rdma_dada::pipeline::WorkerProduct::kStokes,
                   "beamform,stokes", expected_stokes, device_converted, 128,
                   device_scratch, 128, device_output, 128, stream,
                   converted_header);
    }

    status = conversion.Finish();
    Expect(status.ok(), "finish products-test conversion");
    status = h2d.Finish();
    Expect(status.ok(), "finish products-test H2D");
    cudaFree(device_input);
    cudaFree(device_converted);
    cudaFree(device_scratch);
    cudaFree(device_output);
    cudaStreamDestroy(stream);
    std::remove(argv[1]);

    if (failures != 0) return 1;
    std::cout << "pipeline_worker_cuda_products_test passed\n";
    return 0;
}
