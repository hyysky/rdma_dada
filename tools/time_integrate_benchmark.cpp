#include "rdma_dada/modules/time_integrate/time_integrate_module.h"

#include <chrono>
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

}  // namespace

int main(int argc, char** argv) {
    const std::uint64_t ntime = argc > 1 ? std::strtoull(argv[1], NULL, 10)
                                         : UINT64_C(65536);
    const std::uint64_t frame_elements =
        argc > 2 ? std::strtoull(argv[2], NULL, 10) : UINT64_C(180);
    const std::uint64_t integration_length =
        argc > 3 ? std::strtoull(argv[3], NULL, 10) : UINT64_C(128);
    const int iterations = argc > 4 ? std::atoi(argv[4]) : 40;
    const std::string operation = argc > 5 ? argv[5] : "MEAN";
    if (ntime == 0 || frame_elements == 0 || integration_length == 0 ||
        ntime % integration_length != 0 || iterations <= 0 ||
        (operation != "SUM" && operation != "MEAN")) {
        std::cerr << "usage: time_integrate_benchmark "
                     "[NTIME FRAME_ELEMENTS K ITERATIONS [SUM|MEAN]]\n";
        return 2;
    }

    std::uint64_t frame_bytes = 0;
    std::uint64_t input_elements = 0;
    std::uint64_t input_bytes = 0;
    std::uint64_t bytes_per_second = 0;
    if (!CheckedMultiply(frame_elements, sizeof(float), &frame_bytes) ||
        !CheckedMultiply(ntime, frame_elements, &input_elements) ||
        !CheckedMultiply(input_elements, sizeof(float), &input_bytes) ||
        !CheckedMultiply(frame_bytes, integration_length,
                         &bytes_per_second) ||
        !FitsSizeT(input_elements) ||
        !FitsSizeT(input_elements / integration_length)) {
        std::cerr << "benchmark geometry overflows\n";
        return 2;
    }
    const std::uint64_t output_elements =
        input_elements / integration_length;
    std::vector<float> input(static_cast<std::size_t>(input_elements));
    std::vector<float> output(static_cast<std::size_t>(output_elements));
    for (std::size_t index = 0; index < input.size(); ++index) {
        input[index] = static_cast<float>(
            static_cast<int>(index % 251U) - 125) * 0.0078125f;
    }

    rdma_dada::pipeline::Metadata header;
    header.SetString("DATA_STAGE", "POWER");
    header.SetString("ORDER", "TFPB");
    header.SetString("SAMPLE_FORMAT", "F32");
    header.SetString("MEMORY", "HOST");
    header.SetUint64("NCHAN", 1);
    header.SetUint64("NPOL", 1);
    header.SetUint64("NBEAM", frame_elements);
    header.SetUint64("RECORD_BYTES", frame_bytes);
    header.SetUint64("RESOLUTION", frame_bytes);
    header.SetUint64("BYTES_PER_SECOND", bytes_per_second);
    header.SetDouble("TSAMP", 1.0);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetString("EXECUTION_BACKEND", "CPU_REFERENCE");
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
        reinterpret_cast<const std::uint8_t*>(&input[0]), input_bytes, 1,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output_block = {
        reinterpret_cast<std::uint8_t*>(&output[0]),
        output_elements * sizeof(float), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };

    for (int warmup = 0; warmup < 3; ++warmup) {
        status = module.ProcessBlock(input_block, &output_block, context);
        if (!status.ok()) {
            std::cerr << status.message() << '\n';
            return 1;
        }
    }
    const std::vector<float> reference = output;

    const std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < iterations; ++iteration) {
        status = module.ProcessBlock(input_block, &output_block, context);
        if (!status.ok()) {
            std::cerr << status.message() << '\n';
            return 1;
        }
    }
    const std::chrono::steady_clock::time_point stop =
        std::chrono::steady_clock::now();
    if (output != reference) {
        std::cerr << "time integration produced non-deterministic output\n";
        return 1;
    }

    double checksum = 0.0;
    for (std::size_t index = 0; index < output.size(); ++index) {
        checksum += output[index];
    }
    if (!std::isfinite(checksum)) {
        std::cerr << "time integration checksum is not finite\n";
        return 1;
    }
    const double seconds =
        std::chrono::duration<double>(stop - start).count();
    const double input_gb = static_cast<double>(input_bytes) * iterations /
                            1.0e9;
    std::cout << std::fixed << std::setprecision(3)
              << "ntime=" << ntime
              << " frame_elements=" << frame_elements
              << " K=" << integration_length
              << " operation=" << operation
              << " iterations=" << iterations
              << " seconds=" << seconds
              << " input_GBps=" << input_gb / seconds
              << " checksum=" << checksum << '\n';
    return 0;
}
