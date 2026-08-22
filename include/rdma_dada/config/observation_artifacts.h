#pragma once

#include "rdma_dada/config/resolved_observation_plan.h"
#include "rdma_dada/pipeline/metadata.h"

#include <string>

namespace rdma_dada {

struct ObservationArtifactOptions {
    ObservationArtifactOptions()
        : budget_target_payload_bits_per_second(0U) {}

    std::uint64_t budget_target_payload_bits_per_second;
};

struct ObservationArtifacts {
    ResolvedObservationPlan plan;
    pipeline::Metadata raw_header;
    pipeline::Metadata unpacked_header;
    pipeline::Metadata converted_header;
    pipeline::Metadata beamformed_header;
    pipeline::Metadata output_header;
    std::string resolved_plan_json;
    std::string ring_plan_json;
    std::string validation_report_json;
};

bool BuildObservationArtifacts(const std::string& observation_path,
                               ObservationArtifacts* artifacts,
                               std::string* error);

bool BuildObservationArtifactsWithOptions(
    const std::string& observation_path,
    const ObservationArtifactOptions& options,
    ObservationArtifacts* artifacts,
    std::string* error);

bool BuildObservationArtifactsFromResolvedPlan(
    const ResolvedObservationPlan& plan,
    ObservationArtifacts* artifacts,
    std::string* error);

bool BuildObservationArtifactsFromResolvedPlanWithOptions(
    const ResolvedObservationPlan& plan,
    const ObservationArtifactOptions& options,
    ObservationArtifacts* artifacts,
    std::string* error);

// Writes a new artifact directory through a sibling staging directory. The
// destination must not already exist.
bool WriteObservationArtifacts(const ObservationArtifacts& artifacts,
                               const std::string& output_directory,
                               std::string* error);

}  // namespace rdma_dada
