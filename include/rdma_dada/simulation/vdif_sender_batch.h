#pragma once

#include "rdma_dada/simulation/vdif_sender_sim.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rdma_dada {
namespace simulation {

struct VdifPacketView {
    const std::uint8_t* data;
    std::size_t bytes;
    std::uint64_t group_index;
    std::uint16_t station_id;
    std::uint32_t station_index;
};

class VdifSenderBatch {
public:
    VdifSenderBatch();

    bool Initialize(const VdifSenderSimConfig& config, std::string* error);
    bool Prepare(std::uint64_t first_packet,
                 std::uint32_t packet_count,
                 std::string* error);

    std::uint32_t capacity() const;
    std::uint32_t size() const;
    const VdifPacketView& packet(std::uint32_t index) const;

private:
    VdifSenderSimConfig config_;
    std::size_t record_bytes_;
    std::uint32_t size_;
    std::vector<std::uint8_t> storage_;
    std::vector<VdifPacketView> packets_;
    std::vector<std::vector<std::uint8_t> > station_templates_;
};

}  // namespace simulation
}  // namespace rdma_dada
