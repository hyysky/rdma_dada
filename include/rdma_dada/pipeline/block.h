#pragma once

#include <cstddef>
#include <cstdint>

namespace rdma_dada {
namespace pipeline {

enum class MemoryLocation {
    kHost,
    kPinnedHost,
    kCudaDevice
};

// A data block is the unit passed to an algorithm. Header metadata is handled
// once per PSRDADA transfer, separately from these block views.
struct InputBlock {
    const std::uint8_t* data;
    std::uint64_t size;
    std::uint64_t sequence;
    MemoryLocation location;
};

struct OutputBlock {
    std::uint8_t* data;
    std::uint64_t capacity;
    std::uint64_t size;
    std::uint64_t sequence;
    MemoryLocation location;
};

}  // namespace pipeline
}  // namespace rdma_dada
