#include "rdma_dada/pipeline/gpu_block_pipeline.h"
#include "rdma_dada/pipeline/worker_config.h"

#include <cuda_runtime_api.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool WriteIdentityWeights(const std::string& path) {
    std::string header =
        "{'descr': '|i1', 'fortran_order': False, "
        "'shape': (2, 2, 2, 2, 2), }";
    while ((10U + header.size() + 1U) % 16U != 0U) header.push_back(' ');
    header.push_back('\n');
    std::ofstream output(path.c_str(), std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const unsigned char prefix[] = {
        0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<unsigned char>(header.size() & 0xffU),
        static_cast<unsigned char>((header.size() >> 8U) & 0xffU)
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
    header.SetString("SOURCE", "gpu-block-pipeline-test");
    header.SetUint64("NCHAN", 2U);
    header.SetUint64("NPOL", 2U);
    header.SetUint64("NANT", 2U);
    header.SetUint64("COMPONENT_NBIT", 8U);
    header.SetUint64("SAMPLE_NBIT", 16U);
    header.SetUint64("BLOCK_NTIME", 2U);
    header.SetUint64("RESOLUTION", 16U);
    header.SetUint64("RECORD_BYTES", 32U);
    header.SetUint64("OUTPUT_BLOCK_BYTES", 32U);
    header.SetUint64("BYTES_PER_SECOND", 1600U);
    header.SetUint64("TRANSFER_SIZE", 96U);
    header.SetUint64("FILE_SIZE", 96U);
    header.SetUint64("OBS_OFFSET", 0U);
    header.SetDouble("TSAMP", 1.0);
    return header;
}

rdma_dada::pipeline::WorkerConfig MakeConfig(
    const std::string& weights_path, std::uint32_t inflight_blocks) {
    rdma_dada::pipeline::WorkerConfig config = {};
    config.input_key = 0xd4U;
    config.output_key = 0xd6U;
    config.input_key_text = "00d4";
    config.output_key_text = "00d6";
    config.execution_backend = "CUDA";
    config.cuda_device = 0;
    config.run_once = true;
    config.cuda_pipeline_mode = rdma_dada::CudaPipelineMode::kStagedPipeline;
    config.cuda_inflight_blocks = inflight_blocks;
    config.nchan = 2U;
    config.nant = 2U;
    config.npol = 2U;
    config.udp_payload_bytes = 8U;
    config.samples_per_udp = 1U;
    config.udp_packets_per_antenna_per_block = 2U;
    config.conversion_scale = 1.0;
    config.weights_file = weights_path;
    config.weights_order = "FPAB2";
    config.weights_id = "gpu-block-pipeline-identity-v1";
    config.weights_scale = 1.0;
    config.nbeam = 2U;
    config.compute_mode = "FP32";
    config.product = rdma_dada::pipeline::WorkerProduct::kPower;
    config.integration_enabled = true;
    config.integration_length = 2U;
    config.integration_operation = "MEAN";
    return config;
}

struct OutputHarness {
    explicit OutputHarness(std::uint64_t bytes, bool fail_commit = false)
        : blocks(3U, std::vector<std::uint8_t>(
                         static_cast<std::size_t>(bytes), 0U)),
          acquired(false), acquired_sequence(0U), aborted(false),
          fail_commit(fail_commit) {}

    rdma_dada::pipeline::StageStatus Acquire(
        std::uint64_t sequence, std::uint8_t** data,
        std::uint64_t* capacity) {
        std::lock_guard<std::mutex> lock(mutex);
        if (acquired || sequence >= blocks.size()) {
            return rdma_dada::pipeline::StageStatus::Error(
                "invalid output acquisition order");
        }
        acquired = true;
        acquired_sequence = sequence;
        *data = &blocks[static_cast<std::size_t>(sequence)][0];
        *capacity = blocks[static_cast<std::size_t>(sequence)].size();
        return rdma_dada::pipeline::StageStatus::Ok();
    }

    rdma_dada::pipeline::StageStatus Commit(
        std::uint64_t sequence, std::uint64_t bytes) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!acquired || acquired_sequence != sequence ||
            bytes != blocks[static_cast<std::size_t>(sequence)].size()) {
            return rdma_dada::pipeline::StageStatus::Error(
                "invalid output commit");
        }
        if (fail_commit) {
            return rdma_dada::pipeline::StageStatus::Error(
                "injected output commit failure");
        }
        acquired = false;
        published.push_back(sequence);
        return rdma_dada::pipeline::StageStatus::Ok();
    }

    rdma_dada::pipeline::StageStatus Abort(std::uint64_t sequence) {
        std::lock_guard<std::mutex> lock(mutex);
        if (acquired && acquired_sequence == sequence) acquired = false;
        aborted = true;
        return rdma_dada::pipeline::StageStatus::Ok();
    }

    std::vector<std::vector<std::uint8_t> > blocks;
    std::vector<std::uint64_t> published;
    bool acquired;
    std::uint64_t acquired_sequence;
    bool aborted;
    bool fail_commit;
    std::mutex mutex;
};

void RunStaged(const std::string& weights_path,
               std::uint32_t inflight_blocks) {
    const std::int8_t input[] = {
        1, 2, 5, 6, 2, 0, -1, 2,
        -2, 3, 0, -1, 3, -2, 2, 1,
        3, 4, 7, 8, 1, -1, 4, 1,
        1, 0, 2, 2, -1, -3, 0, 4
    };
    const float expected[] = {9.0F, 13.0F, 31.0F, 60.5F,
                              8.5F, 6.0F, 5.0F, 16.5F};
    rdma_dada::pipeline::WorkerConfig config =
        MakeConfig(weights_path, inflight_blocks);
    rdma_dada::pipeline::WorkerBlockGeometry geometry = {};
    std::string error;
    Expect(rdma_dada::pipeline::ComputeWorkerBlockGeometry(
               config, &geometry, &error),
           "compute staged geometry: " + error);
    if (failures != 0) return;
    Expect(geometry.input_block_bytes == sizeof(input),
           "staged input block geometry is exact");
    Expect(geometry.output_block_bytes == sizeof(expected),
           "staged integrated output geometry is exact");

    OutputHarness harness(geometry.output_block_bytes);
    rdma_dada::pipeline::OutputBlockFunctions output;
    output.acquire = [&harness](std::uint64_t sequence, std::uint8_t** data,
                                std::uint64_t* capacity) {
        return harness.Acquire(sequence, data, capacity);
    };
    output.commit = [&harness](std::uint64_t sequence, std::uint64_t bytes) {
        return harness.Commit(sequence, bytes);
    };
    output.abort = [&harness](std::uint64_t sequence) {
        return harness.Abort(sequence);
    };

    rdma_dada::pipeline::GpuBlockPipeline pipeline;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status = pipeline.Configure(
        config, geometry, MakeInputHeader(), output, &output_header);
    Expect(status.ok(), "configure staged pipeline: " + status.message());
    std::uint64_t output_cuda_device = 0U;
    Expect(output_header.GetUint64("CUDA_DEVICE", &output_cuda_device) &&
               output_cuda_device == 0U,
           "staged output preserves CUDA_DEVICE provenance");
    for (std::uint64_t sequence = 0U; sequence < 3U && status.ok();
         ++sequence) {
        status = pipeline.SubmitBlock(
            sequence, reinterpret_cast<const std::uint8_t*>(input),
            sizeof(input));
        Expect(status.ok(), "submit staged block " +
                            std::to_string(sequence) + ": " +
                            status.message());
    }
    if (status.ok()) {
        status = pipeline.Drain();
        Expect(status.ok(), "drain staged pipeline: " + status.message());
    }
    Expect(harness.published == std::vector<std::uint64_t>({0U, 1U, 2U}),
           "staged writer publishes strict block sequence");
    Expect(!harness.aborted, "staged writer does not abort output blocks");
    for (std::size_t block = 0U; block < harness.blocks.size(); ++block) {
        const float* values = reinterpret_cast<const float*>(
            &harness.blocks[block][0]);
        for (std::size_t index = 0U;
             index < sizeof(expected) / sizeof(expected[0]); ++index) {
            Expect(std::fabs(values[index] - expected[index]) < 1.0e-4F,
                   "staged numerical output block " +
                       std::to_string(block) + " value " +
                       std::to_string(index));
        }
    }
    const rdma_dada::pipeline::WorkerMetrics& metrics = pipeline.metrics();
    Expect(metrics.submitted_blocks() == 3U &&
               metrics.completed_blocks() == 3U &&
               metrics.published_blocks() == 3U &&
               metrics.blocks() == 3U,
           "staged lifecycle counters close exactly");
    Expect(metrics.max_inflight() >= 1U &&
               metrics.max_inflight() <= inflight_blocks,
           "staged max inflight stays bounded");
    Expect(metrics.input_staging_bytes() == 0U,
           "registered-ring staged input performs no host staging copy");
    Expect(metrics.output_staging_bytes() == 3U * sizeof(expected),
           "staged output copy byte accounting is exact");
    status = pipeline.Finish();
    Expect(status.ok(), "finish staged pipeline: " + status.message());
}

void RunCommitFailure(const std::string& weights_path) {
    const std::int8_t input[32] = {};
    rdma_dada::pipeline::WorkerConfig config = MakeConfig(weights_path, 2U);
    rdma_dada::pipeline::WorkerBlockGeometry geometry = {};
    std::string error;
    Expect(rdma_dada::pipeline::ComputeWorkerBlockGeometry(
               config, &geometry, &error),
           "compute failure geometry: " + error);
    if (failures != 0) return;
    OutputHarness harness(geometry.output_block_bytes, true);
    rdma_dada::pipeline::OutputBlockFunctions output;
    output.acquire = [&harness](std::uint64_t sequence, std::uint8_t** data,
                                std::uint64_t* capacity) {
        return harness.Acquire(sequence, data, capacity);
    };
    output.commit = [&harness](std::uint64_t sequence, std::uint64_t bytes) {
        return harness.Commit(sequence, bytes);
    };
    output.abort = [&harness](std::uint64_t sequence) {
        return harness.Abort(sequence);
    };
    rdma_dada::pipeline::GpuBlockPipeline pipeline;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status = pipeline.Configure(
        config, geometry, MakeInputHeader(), output, &output_header);
    Expect(status.ok(), "configure commit-failure pipeline");
    if (status.ok()) {
        status = pipeline.SubmitBlock(
            0U, reinterpret_cast<const std::uint8_t*>(input), sizeof(input));
        Expect(status.ok(), "enqueue block before commit failure");
    }
    if (status.ok()) status = pipeline.Drain();
    Expect(!status.ok(), "commit failure propagates through drain");
    Expect(harness.aborted, "commit failure aborts the acquired output block");
    status = pipeline.Finish();
    Expect(!status.ok(), "finish preserves the first staged failure");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " WEIGHTS.npy\n";
        return 2;
    }
    int device_count = 0;
    if (cudaGetDeviceCount(&device_count) != cudaSuccess || device_count == 0) {
        std::cout << "SKIP: no CUDA device is available\n";
        return 77;
    }
    if (!WriteIdentityWeights(argv[1])) {
        std::cerr << "cannot write staged test weights\n";
        return 2;
    }
    for (std::uint32_t inflight = 1U; inflight <= 4U; ++inflight) {
        RunStaged(argv[1], inflight);
    }
    RunCommitFailure(argv[1]);
    std::remove(argv[1]);
    if (failures != 0) return 1;
    std::cout << "gpu_block_pipeline_cuda_test passed\n";
    return 0;
}
