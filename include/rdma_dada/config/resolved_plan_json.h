#pragma once

#include "rdma_dada/config/resolved_observation_plan.h"

#include <string>

namespace rdma_dada {

bool SerializeResolvedObservationPlan(const ResolvedObservationPlan& plan,
                                      std::string* json,
                                      std::string* error);

bool LoadResolvedObservationPlan(const std::string& path,
                                 ResolvedObservationPlan* plan,
                                 std::string* error);

bool ComputeObservationIdentities(ResolvedObservationPlan* plan,
                                  std::string* error);

}  // namespace rdma_dada
