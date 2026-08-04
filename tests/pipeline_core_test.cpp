#include "rdma_dada/pipeline/stage.h"

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

class HeaderAwareCopyStage : public rdma_dada::pipeline::Stage {
public:
    const char* Name() const { return "copy"; }

    rdma_dada::pipeline::StageStatus ConfigureHeader(
        const rdma_dada::pipeline::Metadata& input_header,
        const rdma_dada::pipeline::StageParameters& parameters,
        rdma_dada::pipeline::Metadata* output_header) {
        if (!output_header) {
            return rdma_dada::pipeline::StageStatus::Error("null output header");
        }
        std::uint64_t output_bytes = 0;
        if (!parameters.GetUint64("OUTPUT_RECORD_BYTES", &output_bytes)) {
            return rdma_dada::pipeline::StageStatus::Error(
                "missing OUTPUT_RECORD_BYTES");
        }
        *output_header = input_header;
        output_header->SetString("DATA_STAGE", "COPY");
        output_header->SetString("PARENT_STAGE", "RAW");
        output_header->SetUint64("RECORD_BYTES", output_bytes);
        output_header->SetUint64("RESOLUTION", output_bytes);
        return rdma_dada::pipeline::StageStatus::Ok();
    }

    rdma_dada::pipeline::StageStatus ProcessBlock(
        const rdma_dada::pipeline::InputBlock& input,
        rdma_dada::pipeline::OutputBlock* output,
        const rdma_dada::pipeline::BlockExecutionContext& context) {
        if (context.backend !=
            rdma_dada::pipeline::ExecutionBackend::kHost) {
            return rdma_dada::pipeline::StageStatus::Error(
                "copy requires host execution");
        }
        if (!output || output->capacity < input.size) {
            return rdma_dada::pipeline::StageStatus::Error(
                "output block is too small");
        }
        std::memcpy(output->data, input.data, static_cast<std::size_t>(input.size));
        output->size = input.size;
        output->sequence = input.sequence;
        return rdma_dada::pipeline::StageStatus::Ok();
    }
};

}  // namespace

int main() {
    rdma_dada::pipeline::Metadata input_header;
    input_header.SetString("DATA_STAGE", "RAW");
    input_header.SetString("UTC_START", "2026-08-01-00:00:00");
    input_header.SetUint64("RECORD_BYTES", 8256);

    rdma_dada::pipeline::StageParameters parameters;
    parameters.SetUint64("OUTPUT_RECORD_BYTES", 8192);

    HeaderAwareCopyStage stage;
    rdma_dada::pipeline::Metadata output_header;
    rdma_dada::pipeline::StageStatus status =
        stage.ConfigureHeader(input_header, parameters, &output_header);
    Expect(status.ok(), "header transform should succeed");

    std::string utc;
    std::uint64_t record_bytes = 0;
    Expect(output_header.GetString("UTC_START", &utc) &&
               utc == "2026-08-01-00:00:00",
           "unchanged upstream header fields are preserved");
    Expect(output_header.GetUint64("RECORD_BYTES", &record_bytes) &&
               record_bytes == 8192,
           "stage config updates output header geometry");

    const std::uint8_t input_data[] = {1, 2, 3, 4};
    std::uint8_t output_data[sizeof(input_data)] = {};
    const rdma_dada::pipeline::InputBlock input = {
        input_data, sizeof(input_data), 7,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    rdma_dada::pipeline::OutputBlock output = {
        output_data, sizeof(output_data), 0, 0,
        rdma_dada::pipeline::MemoryLocation::kHost
    };
    const rdma_dada::pipeline::BlockExecutionContext context = {
        rdma_dada::pipeline::ExecutionBackend::kHost, -1, NULL
    };
    status = stage.ProcessBlock(input, &output, context);
    Expect(status.ok(), "data block transform should succeed");
    Expect(output.size == sizeof(input_data), "output data size");
    Expect(output.sequence == 7, "block sequence is propagated");
    Expect(std::memcmp(input_data, output_data, sizeof(input_data)) == 0,
           "algorithm receives and writes data bytes");

    if (failures != 0) return 1;
    std::cout << "pipeline_core_test passed\n";
    return 0;
}
