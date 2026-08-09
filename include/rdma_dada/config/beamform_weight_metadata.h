#pragma once

#include <cstdint>
#include <string>

namespace rdma_dada {

struct BeamformWeightMetadata {
    std::string dtype;
    std::uint64_t nchan;
    std::uint64_t npol;
    std::uint64_t nant;
    std::uint64_t nbeam;
    std::uint64_t component_bytes;
    std::uint64_t payload_bytes;
};

// Reads and validates a C-contiguous NumPy v1/v2 weight file with shape
// [F,P,A,B,2]. The payload is size-checked but not decoded.
bool ReadBeamformWeightMetadata(const std::string& path,
                                BeamformWeightMetadata* metadata,
                                std::string* error);

}  // namespace rdma_dada
