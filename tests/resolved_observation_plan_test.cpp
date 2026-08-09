#include "rdma_dada/config/observation_config.h"
#include "rdma_dada/config/packet_format_config.h"
#include "rdma_dada/config/resolved_observation_plan.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool Resolve(const rdma_dada::ObservationConfig& observation,
             const rdma_dada::PacketFormatConfig& wire,
             rdma_dada::ResolvedObservationPlan* plan,
             std::string* error) {
    error->clear();
    return rdma_dada::ResolveObservationPlan(observation, wire, plan, error);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: resolved_observation_plan_test OBSERVATION WIRE\n";
        return 2;
    }

    rdma_dada::ObservationConfig observation;
    rdma_dada::PacketFormatConfig wire;
    std::string error;
    Expect(rdma_dada::LoadObservationConfig(argv[1], &observation, &error),
           "observation fixture loads: " + error);
    error.clear();
    Expect(rdma_dada::LoadPacketFormatConfig(argv[2], &wire, &error),
           "wire fixture loads: " + error);
    if (failures != 0) return 1;

    rdma_dada::ResolvedObservationPlan plan;
    Expect(Resolve(observation, wire, &plan, &error),
           "example plan resolves: " + error);
    if (failures == 0) {
        Expect(plan.nant == 2U, "NANT comes from ordered Station IDs");
        Expect(plan.complex_sample_bytes == 2U, "CI8 complex sample is 2 bytes");
        Expect(plan.payload_bytes == 4096U, "packet payload bytes");
        Expect(plan.raw_record_bytes == 4128U, "raw record includes 32-byte header");
        Expect(plan.group_period_ps == UINT64_C(512000000), "group period ps");
        Expect(plan.expected_groups == 15141U, "exact observation group count");
        Expect(plan.records_per_block == 2048U, "all-Station records per block");
        Expect(plan.samples_per_block == 524288U, "per-Station samples per block");
        Expect(plan.raw_block_bytes == 8454144U, "raw block bytes");
        Expect(plan.compute_block_bytes == 8388608U, "ATFP compute block bytes");
        Expect(plan.window_groups == 2048U, "two-block window groups");
        Expect(plan.window_payload_bytes == 16777216U,
               "payload-only window bytes");
        Expect(plan.window_validity_bytes == 512U,
               "one validity bit per Station/group slot");
        Expect(plan.raw_ring_bytes == 67633152U, "raw ring bytes");
        Expect(plan.compute_ring_bytes == 67108864U, "compute ring bytes");
        Expect(plan.raw_file_bytes == 0U && plan.compute_file_bytes == 0U,
               "disabled storage has no file allocation");
        Expect(plan.payload_bytes_per_second == 16000000U,
               "aggregate payload byte rate");
        Expect(plan.raw_bytes_per_second == 16125000U,
               "aggregate raw byte rate");
        Expect(plan.group_start_reference_epoch == 53U,
               "2026-08 uses VDIF reference epoch 53");
        Expect(plan.group_start_seconds == 3283200U,
               "seconds since 2026-07-01");
        Expect(plan.group_start_frame == 0U, "integer-second start frame is zero");
        Expect(plan.config_id.empty() && plan.geometry_id.empty(),
               "identity fields are assigned by Task 4");
    }

    rdma_dada::ObservationConfig invalid = observation;
    invalid.duration_ps -= 1U;
    Expect(!Resolve(invalid, wire, &plan, &error),
           "duration remainder is rejected");

    invalid = observation;
    invalid.duration_seconds = "1";
    Expect(!Resolve(invalid, wire, &plan, &error),
           "duration string and parsed picoseconds must agree");

    invalid = observation;
    invalid.samples_per_packet = std::numeric_limits<std::uint64_t>::max();
    Expect(!Resolve(invalid, wire, &plan, &error),
           "payload multiplication overflow is rejected");

    invalid = observation;
    invalid.samples_per_packet = UINT64_C(67108848);
    invalid.nchan = 1U;
    invalid.npol = 1U;
    invalid.sample_interval_ps = 1U;
    Expect(!Resolve(invalid, wire, &plan, &error),
           "VDIF 24-bit frame-length overflow is rejected");

    invalid = observation;
    invalid.nchan = 256U;
    Expect(!Resolve(invalid, wire, &plan, &error),
           "NCHAN above the VDIF UINT8 range is rejected");

    invalid = observation;
    invalid.samples_per_packet = 1U;
    invalid.sample_interval_ps = 1U;
    invalid.duration_seconds = "0.000000000001";
    invalid.duration_ps = 1U;
    Expect(!Resolve(invalid, wire, &plan, &error),
           "groups-per-second above the VDIF frame field are rejected");

    invalid = observation;
    invalid.station_ids.push_back(invalid.station_ids[0]);
    Expect(!Resolve(invalid, wire, &plan, &error),
           "duplicate Station IDs are rejected by the resolver");

    invalid = observation;
    invalid.utc_start = "2026-02-31-00:00:00";
    Expect(!Resolve(invalid, wire, &plan, &error),
           "invalid UTC calendar date is rejected by the resolver");

    rdma_dada::PacketFormatConfig invalid_wire = wire;
    invalid_wire.application_header_bytes = 64U;
    Expect(!Resolve(observation, invalid_wire, &plan, &error),
           "non-32-byte Project VDIF profile is rejected");

    invalid = observation;
    invalid.station_ids.assign(1U, 101U);
    invalid.nchan = 1U;
    invalid.npol = 2U;
    invalid.samples_per_packet = 2U;
    invalid.sample_interval_ps = UINT64_C(1000000);
    invalid.duration_seconds = "0.000002";
    invalid.duration_ps = UINT64_C(2000000);
    invalid.groups_per_block = 1U;
    invalid.raw_ring_blocks = 1U;
    invalid.compute_ring_blocks = 1U;
    invalid.window_blocks = 2U;
    invalid.disk_enabled = true;
    invalid.blocks_per_file = 1U;
    invalid.direct_io = true;
    Expect(!Resolve(invalid, wire, &plan, &error),
           "direct-I/O block misalignment is rejected");

    if (failures != 0) return 1;
    std::cout << "resolved_observation_plan_test passed\n";
    return 0;
}
