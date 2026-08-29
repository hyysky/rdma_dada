#include "rdma_dada/io/rdma/receive_policy.h"

#include <cstring>
#include <limits>

namespace rdma_dada {
namespace io {
namespace rdma {

DestinationUdpFilter BuildDestinationUdpFilter(
    const std::uint8_t destination_mac[6],
    std::uint32_t destination_ip,
    std::uint16_t destination_port) {
    DestinationUdpFilter filter;
    std::memset(&filter, 0, sizeof(filter));
    std::memcpy(filter.destination_mac, destination_mac,
                sizeof(filter.destination_mac));
    std::memset(filter.destination_mac_mask, 0xff,
                sizeof(filter.destination_mac_mask));
    filter.destination_ip = destination_ip;
    filter.destination_ip_mask = std::numeric_limits<std::uint32_t>::max();
    filter.destination_port = destination_port;
    filter.destination_port_mask =
        std::numeric_limits<std::uint16_t>::max();
    return filter;
}

bool ShouldEmitPeriodicReceiveStatus(bool debug_mode) {
    return debug_mode;
}

RawBlockTail ClassifyRawBlockTail(std::uint64_t block_bytes,
                                  std::uint64_t record_bytes,
                                  std::uint64_t valid_bytes) {
    RawBlockTail result;
    result.disposition = RawBlockTailDisposition::kInvalid;
    result.valid_bytes = valid_bytes;
    result.valid_records = 0;
    if (block_bytes == 0U || record_bytes == 0U ||
        block_bytes % record_bytes != 0U || valid_bytes > block_bytes ||
        valid_bytes % record_bytes != 0U) {
        return result;
    }
    result.valid_records = valid_bytes / record_bytes;
    result.disposition = valid_bytes == 0U
        ? RawBlockTailDisposition::kNoData
        : RawBlockTailDisposition::kPublish;
    return result;
}

bool ValidateDirectRawConfiguration(std::size_t recv_wr_num,
                                    std::size_t records_per_block,
                                    std::size_t raw_ring_blocks) {
    return recv_wr_num != 0U && records_per_block != 0U &&
           recv_wr_num <= records_per_block && raw_ring_blocks >= 2U;
}

DirectRawCompletionAction ClassifyDirectRawCompletion(
    bool success, bool receive_opcode, bool valid_wr_id,
    std::uint32_t byte_len, std::uint32_t expected_byte_len,
    std::uint32_t* consecutive_wrong_length) {
    if (!success || !receive_opcode || !valid_wr_id ||
        !consecutive_wrong_length || expected_byte_len == 0U) {
        return DirectRawCompletionAction::kFatal;
    }
    if (byte_len == expected_byte_len) {
        *consecutive_wrong_length = 0;
        return DirectRawCompletionAction::kKeepSlot;
    }
    ++(*consecutive_wrong_length);
    return *consecutive_wrong_length >=
                   kDirectRawMaxConsecutiveWrongLength
        ? DirectRawCompletionAction::kFatal
        : DirectRawCompletionAction::kZeroSlot;
}

bool ShouldCheckDirectRawDrainClock(
    std::uint64_t empty_polls_since_check) {
    return empty_polls_since_check >= kDirectRawDrainClockCheckEmptyPolls;
}

bool ShouldRepostDirectRawWr(bool stop_requested,
                             bool drain_deadline_reached) {
    return !stop_requested || !drain_deadline_reached;
}

bool DirectRawDrainDeadlineReached(std::uint64_t now_ns,
                                   std::uint64_t deadline_ns) {
    return now_ns >= deadline_ns;
}

StagedRawCopyAction CopyStagedRawRecord(
    const unsigned char* frame, std::size_t frame_bytes,
    std::size_t record_bytes, unsigned char* destination,
    std::uint32_t* consecutive_wrong_length) {
    if (!frame || !destination || !consecutive_wrong_length ||
        record_bytes == 0U) {
        return StagedRawCopyAction::kFatal;
    }
    const std::size_t expected = kDirectRawHeaderBytes + record_bytes;
    if (frame_bytes != expected) {
        ++(*consecutive_wrong_length);
        if (*consecutive_wrong_length >= kDirectRawMaxConsecutiveWrongLength)
            return StagedRawCopyAction::kFatal;
        std::memset(destination, 0, record_bytes);
        return StagedRawCopyAction::kZeroed;
    }
    *consecutive_wrong_length = 0;
    std::memcpy(destination, frame + kDirectRawHeaderBytes, record_bytes);
    return StagedRawCopyAction::kCopied;
}

ReceiveLatencyHistogram::ReceiveLatencyHistogram()
    : buckets_(), count_(0) {}

void ReceiveLatencyHistogram::Observe(std::uint64_t nanoseconds) {
    std::size_t bucket = 0;
    std::uint64_t value = nanoseconds;
    while (value > 1U && bucket + 1U < buckets_.size()) {
        value >>= 1U;
        ++bucket;
    }
    ++buckets_[bucket];
    ++count_;
}

std::uint64_t ReceiveLatencyHistogram::count() const { return count_; }

std::uint64_t ReceiveLatencyHistogram::Percentile(
    unsigned int percentile) const {
    if (count_ == 0U || percentile == 0U || percentile > 100U) return 0U;
    const std::uint64_t wanted =
        (count_ * percentile + UINT64_C(99)) / UINT64_C(100);
    std::uint64_t seen = 0;
    for (std::size_t bucket = 0; bucket < buckets_.size(); ++bucket) {
        seen += buckets_[bucket];
        if (seen >= wanted) {
            if (bucket == 63U) return UINT64_MAX;
            return (UINT64_C(1) << (bucket + 1U)) - 1U;
        }
    }
    return UINT64_MAX;
}

DirectRawBlockProgress::DirectRawBlockProgress(std::size_t slot_count)
    : assigned_(slot_count, 0), completed_(slot_count, 0),
      assigned_count_(0), completed_count_(0) {}

bool DirectRawBlockProgress::AssignSlot(std::size_t slot) {
    if (slot >= assigned_.size() || assigned_[slot] != 0) return false;
    assigned_[slot] = 1;
    ++assigned_count_;
    return true;
}

bool DirectRawBlockProgress::CompleteSlot(std::size_t slot) {
    if (slot >= assigned_.size() || assigned_[slot] == 0 ||
        completed_[slot] != 0) {
        return false;
    }
    completed_[slot] = 1;
    ++completed_count_;
    return true;
}

std::size_t DirectRawBlockProgress::ContiguousCompletedSlots() const {
    std::size_t count = 0;
    while (count < completed_.size() && completed_[count] != 0) ++count;
    return count;
}

bool DirectRawBlockProgress::ready_to_publish() const {
    return !assigned_.empty() && assigned_count_ == assigned_.size() &&
           completed_count_ == assigned_.size();
}

bool DirectRawOutstandingBlockOrder::Push(std::uint64_t token) {
    if (tokens_.size() >= 2U) return false;
    tokens_.push_back(token);
    return true;
}

bool DirectRawOutstandingBlockOrder::PopFront(std::uint64_t token) {
    if (tokens_.empty() || tokens_.front() != token) return false;
    tokens_.pop_front();
    return true;
}

std::uint64_t DirectRawOutstandingBlockOrder::front() const {
    return tokens_.empty() ? 0U : tokens_.front();
}

std::size_t DirectRawOutstandingBlockOrder::size() const {
    return tokens_.size();
}

bool DirectRawOutstandingBlockOrder::empty() const {
    return tokens_.empty();
}

}  // namespace rdma
}  // namespace io
}  // namespace rdma_dada
