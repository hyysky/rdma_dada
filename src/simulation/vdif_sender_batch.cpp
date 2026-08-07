#include "rdma_dada/simulation/vdif_sender_batch.h"

#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"

#include <algorithm>
#include <limits>

namespace rdma_dada {
namespace simulation {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool Contains(const std::vector<std::uint64_t>& values, std::uint64_t value) {
    return std::binary_search(values.begin(), values.end(), value);
}

}  // namespace

VdifSenderBatch::VdifSenderBatch()
    : record_bytes_(0), size_(0) {}

bool VdifSenderBatch::Initialize(const VdifSenderSimConfig& config,
                                 std::string* error) {
    if (config.batch_packets == 0 || config.batch_packets > 64U)
        return Fail("batch_packets must be in 1..64", error);
    std::vector<std::uint8_t> first;
    if (!BuildVdifSenderRecord(config, 0, &first, error)) return false;
    if (first.size() < 32U)
        return Fail("VDIF record is smaller than its header", error);
    if (first.size() > std::numeric_limits<std::size_t>::max() /
                           config.batch_packets)
        return Fail("packet batch storage exceeds size_t range", error);

    config_ = config;
    record_bytes_ = first.size();
    size_ = 0;
    repeat_template_ = first;
    storage_.assign(record_bytes_ * config.batch_packets, 0);
    packets_.resize(config.batch_packets);
    for (std::uint32_t i = 0; i < config.batch_packets; ++i) {
        std::uint8_t* slot = &storage_[static_cast<std::size_t>(i) * record_bytes_];
        std::copy(first.begin() + 32, first.end(), slot + 32);
        packets_[i].data = slot;
        packets_[i].bytes = record_bytes_;
        packets_[i].group_index = 0;
    }
    return true;
}

bool VdifSenderBatch::Prepare(std::uint64_t first_group,
                              std::uint32_t packet_count,
                              std::string* error) {
    if (record_bytes_ == 0) return Fail("packet batch is not initialized", error);
    if (packet_count == 0 || packet_count > packets_.size())
        return Fail("packet_count must be within batch capacity", error);
    if (first_group >= config_.group_count ||
        packet_count > config_.group_count - first_group)
        return Fail("packet batch group range exceeds group_count", error);

    for (std::uint32_t i = 0; i < packet_count; ++i) {
        const std::uint64_t group = first_group + i;
        std::uint8_t* slot = const_cast<std::uint8_t*>(packets_[i].data);
        if (config_.payload_mode == "DETERMINISTIC") {
            std::vector<std::uint8_t> record;
            if (!BuildVdifSenderRecord(config_, group, &record, error)) return false;
            std::copy(record.begin(), record.end(), slot);
        } else {
            modules::vdif_unpack::ProjectVdifHeader header = {};
            if (!BuildVdifSenderHeader(config_, group, &header, error) ||
                !modules::vdif_unpack::EncodeProjectVdifV1(
                    header, slot, record_bytes_, error)) return false;
            if (Contains(config_.invalid_header_groups, group)) slot[28] = 1;
        }
        packets_[i].group_index = group;
    }
    size_ = packet_count;
    return true;
}

std::uint32_t VdifSenderBatch::capacity() const {
    return static_cast<std::uint32_t>(packets_.size());
}

std::uint32_t VdifSenderBatch::size() const { return size_; }

const VdifPacketView& VdifSenderBatch::packet(std::uint32_t index) const {
    return packets_[index];
}

}  // namespace simulation
}  // namespace rdma_dada
