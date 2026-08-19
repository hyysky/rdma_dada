#pragma once

#include <cstdint>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {

// A synchronous, non-owning view into one consecutive logical group range in
// the private antenna-major circular payload window. The view is valid only
// for the duration of the emitter call.
struct AtfpBlockView {
    const std::uint8_t* window_data;
    std::uint64_t window_capacity_groups;
    std::uint64_t first_group_ordinal;
    std::uint64_t first_slot;
    std::uint64_t group_count;
    std::uint32_t nant;
    std::uint64_t packet_payload_bytes;
    // Non-zero only for asynchronous publication. The consumer must release
    // this lease after it has finished reading the circular window range.
    std::uint64_t lease_id;
};

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
