#include "rdma_dada/config/observation_artifacts.h"

#include <iostream>
#include <string>

namespace {

void Usage() {
    std::cerr << "usage: observation_config_compile --config FILE "
                 "(--preflight-only | --output-dir DIRECTORY)\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string config_path;
    std::string output_directory;
    bool preflight_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--config" && index + 1 < argc) {
            config_path = argv[++index];
        } else if (argument == "--output-dir" && index + 1 < argc) {
            output_directory = argv[++index];
        } else if (argument == "--preflight-only") {
            preflight_only = true;
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
    std::string error;
    if (!rdma_dada::BuildObservationArtifacts(config_path, &artifacts,
                                               &error)) {
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
