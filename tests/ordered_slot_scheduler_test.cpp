#include "rdma_dada/pipeline/ordered_slot_scheduler.h"

#include <iostream>
#include <string>

namespace {
int failures = 0;
void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}
}  // namespace

int main() {
    using rdma_dada::pipeline::OrderedSlotScheduler;
    using rdma_dada::pipeline::SlotLease;

    OrderedSlotScheduler scheduler(3U);
    SlotLease zero = {};
    SlotLease one = {};
    SlotLease two = {};
    SlotLease lease = {};
    Expect(scheduler.Acquire(0U, &zero).ok(), "acquire sequence zero");
    Expect(scheduler.Acquire(1U, &one).ok(), "acquire sequence one");
    Expect(scheduler.Acquire(2U, &two).ok(), "acquire sequence two");
    Expect(zero.slot_index != one.slot_index &&
               zero.slot_index != two.slot_index &&
               one.slot_index != two.slot_index,
           "in-flight sequences own distinct slots");
    Expect(!scheduler.Acquire(3U, &lease).ok(),
           "bounded scheduler reports pressure when every slot is occupied");

    Expect(scheduler.MarkCompleted(one).ok(),
           "sequence one may complete before zero");
    Expect(!scheduler.NextPublishable(&lease).ok(),
           "sequence one cannot publish before zero");
    Expect(scheduler.MarkCompleted(zero).ok(), "complete sequence zero");
    Expect(scheduler.NextPublishable(&lease).ok() &&
               lease.sequence == 0U && lease.slot_index == zero.slot_index,
           "zero becomes first publishable sequence");
    Expect(scheduler.MarkPublished(lease).ok(), "publish sequence zero");
    Expect(scheduler.NextPublishable(&lease).ok() && lease.sequence == 1U,
           "already-completed sequence one publishes next");
    Expect(scheduler.MarkPublished(lease).ok(), "publish sequence one");
    Expect(scheduler.Acquire(3U, &lease).ok(),
           "publishing releases one bounded slot");
    Expect(!scheduler.MarkPublished(zero).ok(),
           "stale lease cannot publish a reused slot");

    OrderedSlotScheduler failed(2U);
    SlotLease before = {};
    SlotLease broken = {};
    Expect(failed.Acquire(0U, &before).ok() &&
               failed.Acquire(1U, &broken).ok(),
           "failure fixture acquires ordered slots");
    Expect(failed.MarkCompleted(before).ok(),
           "sequence before failure completes");
    Expect(failed.MarkFailed(broken, "injected failure").ok(),
           "first slot failure is recorded");
    Expect(failed.failed(), "scheduler exposes failed state");
    Expect(failed.NextPublishable(&lease).ok() && lease.sequence == 0U,
           "completed sequence before failure remains publishable");
    Expect(failed.MarkPublished(lease).ok(),
           "publish completed sequence before failure");
    Expect(!failed.NextPublishable(&lease).ok(),
           "failed sequence and later work cannot publish");
    Expect(!failed.Acquire(2U, &lease).ok(),
           "failure rejects later acquisition");
    Expect(!failed.MarkCompleted(broken).ok(),
           "failed lease cannot transition to completed");

    if (failures != 0) return 1;
    std::cout << "ordered_slot_scheduler_test passed\n";
    return 0;
}
