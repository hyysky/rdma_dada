#include "rdma_dada/config/observation_config.h"
#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/resolved_observation_plan.h"
#include "rdma_dada/config/resolved_plan_json.h"
#include "rdma_dada/config/sha256.h"

#include <cstdio>
#include <cstdlib>
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

std::string WriteTemp(const std::string& suffix, const std::string& contents) {
    std::ostringstream path;
    path << "/tmp/rdma_dada_identity_" << getpid() << '_' << suffix;
    std::ofstream output(path.str().c_str(), std::ios::binary);
    output << contents;
    return path.str();
}

std::string AlternateObservation(const std::string& wire_path) {
    std::ostringstream out;
    out << "{\n"
        << "  \"processing\": {\"modules\": [], \"run_once\": true, "
           "\"cuda_device\": 0, \"backend\": \"CUDA\"},\n"
        << "  \"receiver\": {\"destination_port\":1000,"
           "\"destination_ip\":\"174.0.1.111\","
           "\"destination_mac\":\"98:03:9b:aa:99:d8\","
           "\"device\":\"mlx5_0\"},\n"
        << "  \"storage\": {\"direct_io\":false,\"blocks_per_file\":0,"
           "\"enabled\":false},\n"
        << "  \"rings\": {\"compute_key\":\"0x00d4\","
           "\"raw_key\":\"0x00d2\"},\n"
        << "  \"blocks\": {\"window_blocks\":2,\"compute_ring_blocks\":8,"
           "\"raw_ring_blocks\":8,\"groups_per_block\":1024},\n"
        << "  \"wire\": {\"samples_per_packet\":512,\"profile\":\""
        << wire_path << "\"},\n"
        << "  \"metadata\": {\"center_frequency_hz\":1250000000,"
           "\"bandwidth_hz\":300000000,\"telescope\":\"CA\"},\n"
        << "  \"observation\": {\"sample_interval_ps\":1000000,"
           "\"npol\":2,\"nchan\":2,\"first_channel_id\":100,"
           "\"station_ids\":[101,102],\"duration_seconds\":\"7.752192\","
           "\"utc_start\":\"2026-08-08-00:00:00\","
           "\"observation_id\":\"ca-functional-v1\"},\n"
        << "  \"schema_version\": 1\n}"
        << '\n';
    return out.str();
}

bool BuildPlan(const std::string& observation_path,
               const std::string& wire_path,
               rdma_dada::ResolvedObservationPlan* plan,
               std::string* error) {
    rdma_dada::ObservationConfig observation;
    rdma_dada::PacketFormatConfig wire;
    return rdma_dada::LoadObservationConfig(observation_path, &observation,
                                            error) &&
        rdma_dada::LoadPacketFormatConfig(wire_path, &wire, error) &&
        rdma_dada::ResolveObservationPlan(observation, wire, plan, error) &&
        rdma_dada::ComputeObservationIdentities(plan, error);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: config_identity_test OBSERVATION WIRE\n";
        return 2;
    }
    const std::string wire_path = argv[2];
    char* resolved_wire = realpath(wire_path.c_str(), NULL);
    if (!resolved_wire) {
        std::cerr << "cannot resolve wire fixture\n";
        return 2;
    }
    const std::string absolute_wire_path = resolved_wire;
    std::free(resolved_wire);
    Expect(rdma_dada::Sha256Hex("abc", 3U) ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "SHA256 abc standard vector");

    std::string error;
    rdma_dada::ResolvedObservationPlan base;
    Expect(BuildPlan(argv[1], wire_path, &base, &error),
           "base plan identity: " + error);
    Expect(base.config_id.size() == 64U && base.geometry_id.size() == 64U,
           "both identities are lowercase SHA256 hex");

    const std::string alternate_path = WriteTemp(
        "alternate.json", AlternateObservation(absolute_wire_path));
    rdma_dada::ResolvedObservationPlan alternate;
    error.clear();
    Expect(BuildPlan(alternate_path, wire_path, &alternate, &error),
           "reordered observation identity: " + error);
    Expect(alternate.config_id == base.config_id &&
               alternate.geometry_id == base.geometry_id,
           "whitespace, key order and source path do not affect IDs");

    rdma_dada::ResolvedObservationPlan receiver_changed = base;
    receiver_changed.source.destination_port = 1001U;
    error.clear();
    Expect(rdma_dada::ComputeObservationIdentities(&receiver_changed, &error),
           "receiver variant identity: " + error);
    Expect(receiver_changed.config_id != base.config_id,
           "receiver change updates CONFIG_ID");
    Expect(receiver_changed.geometry_id == base.geometry_id,
           "receiver change does not update GEOMETRY_ID");

    rdma_dada::ObservationConfig geometry_source = base.source;
    geometry_source.nchan = 1U;
    rdma_dada::ResolvedObservationPlan geometry_changed;
    error.clear();
    Expect(rdma_dada::ResolveObservationPlan(geometry_source, base.wire,
                                             &geometry_changed, &error) &&
               rdma_dada::ComputeObservationIdentities(&geometry_changed,
                                                        &error),
           "geometry variant identity: " + error);
    Expect(geometry_changed.config_id != base.config_id &&
               geometry_changed.geometry_id != base.geometry_id,
           "geometry change updates both IDs");

    const std::string weights_path = WriteTemp("weights.bin", "AAAA");
    rdma_dada::ObservationModuleConfig beam =
        rdma_dada::ObservationModuleConfig();
    beam.kind = rdma_dada::ObservationModuleKind::kBeamform;
    beam.weights_file = weights_path;
    beam.weights_order = "FPAB2";
    beam.weights_id = "identity-fixture";
    beam.weights_scale = "0.0078125";
    beam.compute_mode = "FP32";
    rdma_dada::ResolvedObservationPlan weighted = base;
    weighted.source.modules.push_back(beam);
    error.clear();
    Expect(rdma_dada::ComputeObservationIdentities(&weighted, &error),
           "weighted identity: " + error);
    const std::string weighted_config_id = weighted.config_id;
    const std::string weighted_geometry_id = weighted.geometry_id;
    {
        std::ofstream changed(weights_path.c_str(), std::ios::binary | std::ios::trunc);
        changed << "AAAB";
    }
    error.clear();
    Expect(rdma_dada::ComputeObservationIdentities(&weighted, &error),
           "changed weight identity: " + error);
    Expect(weighted.config_id != weighted_config_id,
           "weight contents update CONFIG_ID");
    Expect(weighted.geometry_id == weighted_geometry_id,
           "weight contents do not update GEOMETRY_ID");

    std::string serialized;
    error.clear();
    Expect(rdma_dada::SerializeResolvedObservationPlan(base, &serialized,
                                                       &error),
           "serialize resolved plan: " + error);
    const std::string plan_path = WriteTemp("resolved.json", serialized);
    rdma_dada::ResolvedObservationPlan loaded;
    error.clear();
    Expect(rdma_dada::LoadResolvedObservationPlan(plan_path, &loaded, &error),
           "load serialized plan: " + error);
    Expect(loaded.config_id == base.config_id &&
               loaded.geometry_id == base.geometry_id &&
               loaded.raw_block_bytes == base.raw_block_bytes,
           "resolved plan round trip");

    std::string tampered = serialized;
    const std::string before = "\"raw_block_bytes\":8454144";
    const std::string::size_type position = tampered.find(before);
    if (position != std::string::npos) {
        tampered.replace(position, before.size(),
                         "\"raw_block_bytes\":8454145");
    }
    const std::string tampered_path = WriteTemp("tampered.json", tampered);
    error.clear();
    Expect(!rdma_dada::LoadResolvedObservationPlan(tampered_path, &loaded,
                                                   &error),
           "tampered derived geometry is rejected");

    std::string identity_tampered = serialized;
    const std::string config_marker = "\"config_id\":\"";
    const std::string::size_type config_position =
        identity_tampered.find(config_marker);
    if (config_position != std::string::npos) {
        const std::string::size_type digest_position =
            config_position + config_marker.size();
        identity_tampered[digest_position] =
            identity_tampered[digest_position] == '0' ? '1' : '0';
    }
    const std::string identity_tampered_path =
        WriteTemp("identity_tampered.json", identity_tampered);
    error.clear();
    Expect(!rdma_dada::LoadResolvedObservationPlan(identity_tampered_path,
                                                   &loaded, &error),
           "tampered CONFIG_ID is rejected");

    std::remove(alternate_path.c_str());
    std::remove(weights_path.c_str());
    std::remove(plan_path.c_str());
    std::remove(tampered_path.c_str());
    std::remove(identity_tampered_path.c_str());

    if (failures != 0) return 1;
    std::cout << "config_identity_test passed\n";
    return 0;
}
