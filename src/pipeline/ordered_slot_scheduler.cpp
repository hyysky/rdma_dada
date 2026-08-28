#include "rdma_dada/pipeline/ordered_slot_scheduler.h"

#include <limits>

namespace rdma_dada {
namespace pipeline {

OrderedSlotScheduler::OrderedSlotScheduler(std::uint32_t slots)
    : slots_(slots),
      next_acquire_sequence_(0U),
      next_publish_sequence_(0U),
      first_failed_sequence_(std::numeric_limits<std::uint64_t>::max()) {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        slots_[index].state = SlotState::kFree;
        slots_[index].sequence = 0U;
    }
}

StageStatus OrderedSlotScheduler::Acquire(std::uint64_t sequence,
                                          SlotLease* lease) {
    if (!lease) return StageStatus::Error("slot lease output is null");
    if (slots_.empty()) return StageStatus::Error("slot scheduler has no slots");
    if (failed()) return StageStatus::Error(failure_message_);
    if (sequence != next_acquire_sequence_) {
        return StageStatus::Error("slot sequence is not the next input sequence");
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        SlotRecord& slot = slots_[index];
        if (slot.state != SlotState::kFree) continue;
        slot.state = SlotState::kSubmitted;
        slot.sequence = sequence;
        lease->slot_index = static_cast<std::uint32_t>(index);
        lease->sequence = sequence;
        ++next_acquire_sequence_;
        return StageStatus::Ok();
    }
    return StageStatus::Error("all GPU pipeline slots are occupied");
}

StageStatus OrderedSlotScheduler::ValidateLease(const SlotLease& lease,
                                                SlotState expected) const {
    if (lease.slot_index >= slots_.size()) {
        return StageStatus::Error("slot lease index is outside scheduler");
    }
    const SlotRecord& slot = slots_[lease.slot_index];
    if (slot.sequence != lease.sequence) {
        return StageStatus::Error("slot lease is stale");
    }
    if (slot.state != expected) {
        return StageStatus::Error("slot lease has an invalid state transition");
    }
    return StageStatus::Ok();
}

StageStatus OrderedSlotScheduler::MarkCompleted(const SlotLease& lease) {
    StageStatus status = ValidateLease(lease, SlotState::kSubmitted);
    if (!status.ok()) return status;
    slots_[lease.slot_index].state = SlotState::kCompleted;
    return StageStatus::Ok();
}

StageStatus OrderedSlotScheduler::NextPublishable(SlotLease* lease) const {
    if (!lease) return StageStatus::Error("publishable lease output is null");
    if (next_publish_sequence_ >= first_failed_sequence_) {
        return StageStatus::Error(failure_message_);
    }
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        const SlotRecord& slot = slots_[index];
        if (slot.sequence == next_publish_sequence_ &&
            slot.state == SlotState::kCompleted) {
            lease->slot_index = static_cast<std::uint32_t>(index);
            lease->sequence = slot.sequence;
            return StageStatus::Ok();
        }
    }
    return StageStatus::Error("next sequence is not complete");
}

StageStatus OrderedSlotScheduler::MarkPublished(const SlotLease& lease) {
    if (lease.sequence != next_publish_sequence_) {
        return StageStatus::Error("slot is not the next publish sequence");
    }
    StageStatus status = ValidateLease(lease, SlotState::kCompleted);
    if (!status.ok()) return status;
    SlotRecord& slot = slots_[lease.slot_index];
    slot.state = SlotState::kFree;
    ++next_publish_sequence_;
    return StageStatus::Ok();
}

StageStatus OrderedSlotScheduler::MarkFailed(const SlotLease& lease,
                                             const std::string& message) {
    if (message.empty()) return StageStatus::Error("slot failure is empty");
    StageStatus status = ValidateLease(lease, SlotState::kSubmitted);
    if (!status.ok()) {
        status = ValidateLease(lease, SlotState::kCompleted);
        if (!status.ok()) return status;
    }
    slots_[lease.slot_index].state = SlotState::kFailed;
    if (lease.sequence < first_failed_sequence_) {
        first_failed_sequence_ = lease.sequence;
        failure_message_ = message;
    }
    return StageStatus::Ok();
}

bool OrderedSlotScheduler::empty() const {
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        if (slots_[index].state != SlotState::kFree) return false;
    }
    return true;
}

bool OrderedSlotScheduler::failed() const {
    return first_failed_sequence_ != std::numeric_limits<std::uint64_t>::max();
}

const std::string& OrderedSlotScheduler::failure_message() const {
    return failure_message_;
}

}  // namespace pipeline
}  // namespace rdma_dada
