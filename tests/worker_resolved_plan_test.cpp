#include "rdma_dada/config/observation_config.h"
#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/resolved_observation_plan.h"
#include "rdma_dada/config/resolved_plan_json.h"
#include "rdma_dada/pipeline/worker_config.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string WriteWeights() {
    std::ostringstream path;
    path << "/tmp/rdma_dada_worker_plan_" << getpid() << ".npy";
    std::string header =
        "{'descr': '|i1', 'fortran_order': False, "
        "'shape': (2, 2, 2, 3, 2), }";
    const std::size_t padding = 16U - ((10U + header.size() + 1U) % 16U);
    header.append(padding, ' ');
    header.push_back('\n');
    const unsigned char prefix[] = {
        0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<unsigned char>(header.size() & 0xffU),
        static_cast<unsigned char>((header.size() >> 8U) & 0xffU)
    };
    std::ofstream output(path.str().c_str(), std::ios::binary);
    output.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    const std::string payload(48U, '\0');
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    return path.str();
}

rdma_dada::ObservationModuleConfig Beamform(const std::string& path) {
    rdma_dada::ObservationModuleConfig result =
        rdma_dada::ObservationModuleConfig();
    result.kind = rdma_dada::ObservationModuleKind::kBeamform;
    result.weights_file = path;
    result.weights_order = "FPAB2";
    result.weights_id = "worker-resolved-test";
    result.weights_scale = "0.0078125";
    result.compute_mode = "FP32";
    return result;
}

rdma_dada::ObservationModuleConfig Module(
    rdma_dada::ObservationModuleKind kind) {
    rdma_dada::ObservationModuleConfig result =
        rdma_dada::ObservationModuleConfig();
    result.kind = kind;
    return result;
}

void ExpectWorkerCase(
    const rdma_dada::ObservationConfig& base,
    const rdma_dada::PacketFormatConfig& wire,
    const std::string& weights,
    rdma_dada::ObservationModuleKind product,
    bool integrate,
    std::uint64_t expected_output_bytes,
    const std::string& expected_stage,
    const std::string& expected_order,
    rdma_dada::pipeline::WorkerProduct expected_product) {
    rdma_dada::ObservationConfig source = base;
    source.modules.clear();
    source.modules.push_back(Beamform(weights));
    if (product == rdma_dada::ObservationModuleKind::kPower ||
        product == rdma_dada::ObservationModuleKind::kStokes) {
        source.modules.push_back(Module(product));
    }
    if (integrate) {
        rdma_dada::ObservationModuleConfig integration =
            Module(rdma_dada::ObservationModuleKind::kIntegrate);
        integration.integration_length = 128U;
        integration.integration_operation = "MEAN";
        source.modules.push_back(integration);
    }
    rdma_dada::ResolvedObservationPlan plan;
    rdma_dada::pipeline::WorkerConfig config;
    rdma_dada::pipeline::WorkerBlockGeometry geometry;
    std::string error;
    Expect(rdma_dada::ResolveObservationPlan(source, wire, &plan, &error) &&
               rdma_dada::pipeline::BuildWorkerConfigFromResolvedPlan(
                   plan, &config, &geometry, &error),
           "resolve worker product case: " + error);
    Expect(plan.output_block_bytes == expected_output_bytes &&
               geometry.output_block_bytes == expected_output_bytes &&
               plan.output_data_stage == expected_stage &&
               plan.output_order == expected_order &&
               config.product == expected_product &&
               config.integration_enabled == integrate,
           "worker product case has exact output contract");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    rdma_dada::ObservationConfig observation;
    rdma_dada::PacketFormatConfig wire;
    std::string error;
    Expect(rdma_dada::LoadObservationConfig(argv[1], &observation, &error),
           "load observation: " + error);
    error.clear();
    Expect(rdma_dada::LoadPacketFormatConfig(argv[2], &wire, &error),
           "load wire profile: " + error);
    const std::string weights = WriteWeights();
    ExpectWorkerCase(
        observation, wire, weights,
        rdma_dada::ObservationModuleKind::kBeamform, false,
        50331648U, "BEAMFORMED", "TFPB",
        rdma_dada::pipeline::WorkerProduct::kBeamformed);
    ExpectWorkerCase(
        observation, wire, weights,
        rdma_dada::ObservationModuleKind::kPower, false,
        25165824U, "POWER", "TFPB",
        rdma_dada::pipeline::WorkerProduct::kPower);
    ExpectWorkerCase(
        observation, wire, weights,
        rdma_dada::ObservationModuleKind::kStokes, false,
        50331648U, "POLARIZATION_PRODUCTS", "TFBS",
        rdma_dada::pipeline::WorkerProduct::kStokes);
    ExpectWorkerCase(
        observation, wire, weights,
        rdma_dada::ObservationModuleKind::kStokes, true,
        393216U, "POLARIZATION_PRODUCTS_INTEGRATED", "TFBS",
        rdma_dada::pipeline::WorkerProduct::kStokes);
    observation.modules.push_back(Beamform(weights));
    observation.modules.push_back(
        Module(rdma_dada::ObservationModuleKind::kPower));
    rdma_dada::ObservationModuleConfig integration =
        Module(rdma_dada::ObservationModuleKind::kIntegrate);
    integration.integration_length = 128U;
    integration.integration_operation = "MEAN";
    observation.modules.push_back(integration);

    rdma_dada::ResolvedObservationPlan plan;
    error.clear();
    Expect(rdma_dada::ResolveObservationPlan(observation, wire, &plan, &error),
           "resolve processing plan: " + error);
    rdma_dada::pipeline::WorkerConfig config;
    rdma_dada::pipeline::WorkerBlockGeometry geometry;
    error.clear();
    Expect(rdma_dada::pipeline::BuildWorkerConfigFromResolvedPlan(
               plan, &config, &geometry, &error),
           "build worker config from plan: " + error);
    Expect(config.input_key == observation.compute_key &&
               config.output_key == observation.output_key,
           "resolved ring keys");
    Expect(config.nchan == 2U && config.nant == 2U && config.npol == 2U &&
               config.nbeam == 3U,
           "resolved F/A/P/B geometry");
    Expect(config.udp_payload_bytes == 4096U &&
               config.samples_per_udp == 512U &&
               config.udp_packets_per_antenna_per_block == 1024U,
           "resolved packet and block grouping");
    Expect(std::fabs(config.conversion_scale - 0.0078125) < 1.0e-12,
           "configured conversion scale is preserved");
    Expect(config.product == rdma_dada::pipeline::WorkerProduct::kPower &&
               config.integration_enabled &&
               config.integration_length == 128U,
           "resolved module selection");
    Expect(geometry.input_block_bytes == plan.compute_block_bytes &&
               geometry.converted_block_bytes == plan.converted_block_bytes &&
               geometry.beamformed_block_bytes ==
                   plan.beamformed_block_bytes &&
               geometry.product_block_bytes == plan.product_block_bytes &&
               geometry.output_block_bytes == plan.output_block_bytes,
           "worker geometry exactly matches resolved plan");

    error.clear();
    Expect(rdma_dada::ComputeObservationIdentities(&plan, &error),
           "compute resolved identities: " + error);
    std::string plan_json;
    error.clear();
    Expect(rdma_dada::SerializeResolvedObservationPlan(
               plan, &plan_json, &error),
           "serialize resolved worker plan: " + error);
    const std::string plan_path = weights + ".resolved.json";
    {
        std::ofstream output(plan_path.c_str(), std::ios::binary);
        output << plan_json;
    }
    rdma_dada::pipeline::WorkerConfig loaded_config;
    rdma_dada::pipeline::WorkerBlockGeometry loaded_geometry;
    error.clear();
    Expect(rdma_dada::pipeline::LoadWorkerConfigFromResolvedPlan(
               plan_path, &loaded_config, &loaded_geometry, &error),
           "load worker from serialized resolved plan: " + error);
    Expect(loaded_geometry.output_block_bytes == plan.output_block_bytes,
           "serialized plan preserves worker output geometry");

    {
        std::fstream changed(weights.c_str(),
                             std::ios::binary | std::ios::in | std::ios::out);
        changed.seekp(-1, std::ios::end);
        changed.put(1);
    }
    error.clear();
    Expect(!rdma_dada::pipeline::LoadWorkerConfigFromResolvedPlan(
               plan_path, &loaded_config, &loaded_geometry, &error),
           "changed weight contents invalidate resolved CONFIG_ID");

    rdma_dada::ResolvedObservationPlan stale = plan;
    ++stale.output_block_bytes;
    error.clear();
    Expect(!rdma_dada::pipeline::BuildWorkerConfigFromResolvedPlan(
               stale, &config, &geometry, &error),
           "stale derived worker geometry is rejected");

    rdma_dada::ObservationConfig empty_source = observation;
    empty_source.modules.clear();
    rdma_dada::ResolvedObservationPlan empty;
    error.clear();
    Expect(rdma_dada::ResolveObservationPlan(
               empty_source, wire, &empty, &error),
           "re-resolve fixture");
    error.clear();
    Expect(!rdma_dada::pipeline::BuildWorkerConfigFromResolvedPlan(
               empty, &config, &geometry, &error),
           "worker requires a beamform processing chain");

    std::remove(weights.c_str());
    std::remove(plan_path.c_str());
    if (failures != 0) return 1;
    std::cout << "worker_resolved_plan_test passed\n";
    return 0;
}
