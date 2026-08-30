#pragma once

#include "rdma_dada/pipeline/stage.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rdma_dada {
namespace pipeline {

enum class SlotState {
    kFree,
    kSubmitted,
    kCompleted,
    kPublishing,
    kFailed
};

struct SlotLease {
    std::uint32_t slot_index;
    std::uint64_t sequence;
};

// Bounded sequence/slot state machine. Thread synchronization belongs to the
// owning GPU pipeline so this class stays portable and deterministic.
class OrderedSlotScheduler {
public:
    explicit OrderedSlotScheduler(std::uint32_t slots);

    StageStatus Acquire(std::uint64_t sequence, SlotLease* lease);
    StageStatus MarkCompleted(const SlotLease& lease);
    StageStatus NextPublishable(SlotLease* lease) const;
    StageStatus MarkPublished(const SlotLease& lease);
    StageStatus MarkFailed(const SlotLease& lease,
                           const std::string& message);

    bool empty() const;
    bool failed() const;
    const std::string& failure_message() const;

private:
    struct SlotRecord {
        SlotState state;
        std::uint64_t sequence;
    };

    StageStatus ValidateLease(const SlotLease& lease,
                              SlotState expected) const;

    std::vector<SlotRecord> slots_;
    std::size_t next_slot_index_;
    std::uint64_t next_acquire_sequence_;
    std::uint64_t next_publish_sequence_;
    std::uint64_t first_failed_sequence_;
    std::string failure_message_;
};

}  // namespace pipeline
}  // namespace rdma_dada
