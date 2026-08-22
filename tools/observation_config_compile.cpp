#include "rdma_dada/config/observation_artifacts.h"
#include "rdma_dada/config/gpu_pipeline_budget.h"

#include <iostream>
#include <string>

namespace {

void Usage() {
    std::cerr << "usage: observation_config_compile --config FILE "
                 "[--budget-payload-gbps RATE] "
                 "(--preflight-only | --output-dir DIRECTORY)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::string output_directory;
    std::string budget_payload_gbps;
    bool preflight_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
        } else if (argument == "--output-dir" && index + 1 < argc) {
            output_directory = argv[++index];
        } else if (argument == "--preflight-only") {
            preflight_only = true;
        } else if (argument == "--budget-payload-gbps" &&
                   index + 1 < argc) {
            budget_payload_gbps = argv[++index];
        } else if (argument == "--help" || argument == "-h") {
            Usage();
            return 0;
        } else {
            Usage();
            return 2;
        }
    }
    if (config_path.empty() || preflight_only == !output_directory.empty()) {
        Usage();
        return 2;
    }

    rdma_dada::ObservationArtifacts artifacts;
    rdma_dada::ObservationArtifactOptions options;
    std::string error;
    if (!budget_payload_gbps.empty() &&
        !rdma_dada::ParsePayloadGigabitsPerSecond(
            budget_payload_gbps,
            &options.budget_target_payload_bits_per_second, &error)) {
        std::cerr << "invalid GPU budget rate: " << error << '\n';
        return 2;
    }
    if (!rdma_dada::BuildObservationArtifactsWithOptions(
            config_path, options, &artifacts, &error)) {
        std::cerr << "observation preflight failed: " << error << '\n';
        return 1;
    }
    if (preflight_only) {
        std::cout << artifacts.validation_report_json;
        return 0;
    }
    if (!rdma_dada::WriteObservationArtifacts(
            artifacts, output_directory, &error)) {
        std::cerr << "cannot write observation artifacts: " << error << '\n';
        return 1;
    }
    std::cout << artifacts.validation_report_json;
    return 0;
}
