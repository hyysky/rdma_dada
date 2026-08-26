#include "rdma_dada/io/rdma/receive_policy.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void TestDestinationFilterWildcardsEverySourceField() {
    const std::uint8_t destination_mac[6] = {
        0x10, 0x70, 0xfd, 0x11, 0xe2, 0xe3
    };
    const std::uint32_t destination_ip = UINT32_C(0x0b10110a);
    const std::uint16_t destination_port = UINT16_C(17201);

    const rdma_dada::io::rdma::DestinationUdpFilter filter =
        rdma_dada::io::rdma::BuildDestinationUdpFilter(
            destination_mac, destination_ip, destination_port);

    Expect(std::memcmp(filter.destination_mac, destination_mac, 6) == 0,
           "destination MAC value is preserved");
    for (std::size_t index = 0; index < 6; ++index) {
        Expect(filter.destination_mac_mask[index] == 0xff,
               "destination MAC mask matches every bit");
        Expect(filter.source_mac[index] == 0,
               "source MAC value is cleared");
        Expect(filter.source_mac_mask[index] == 0,
               "source MAC mask is wildcarded");
    }
    Expect(filter.destination_ip == destination_ip &&
               filter.destination_ip_mask == UINT32_MAX,
           "destination IPv4 is matched exactly");
    Expect(filter.source_ip == 0 && filter.source_ip_mask == 0,
           "source IPv4 is wildcarded");
    Expect(filter.destination_port == destination_port &&
               filter.destination_port_mask == UINT16_MAX,
           "destination UDP port is matched exactly");
    Expect(filter.source_port == 0 && filter.source_port_mask == 0,
           "source UDP port is wildcarded");
}

void TestPeriodicReceiveStatusRequiresDebugMode() {
    Expect(!rdma_dada::io::rdma::ShouldEmitPeriodicReceiveStatus(false),
           "normal receive mode suppresses periodic status work");
    Expect(rdma_dada::io::rdma::ShouldEmitPeriodicReceiveStatus(true),
           "debug receive mode retains periodic status work");
}

void TestRawBlockTailAccounting() {
    namespace rdma = rdma_dada::io::rdma;
    const std::uint64_t record_bytes = 1056;
    const std::uint64_t block_bytes = 16U * record_bytes;

    rdma::RawBlockTail tail =
        rdma::ClassifyRawBlockTail(block_bytes, record_bytes, 0);
    Expect(tail.disposition == rdma::RawBlockTailDisposition::kNoData &&
               tail.valid_records == 0 && tail.valid_bytes == 0,
           "zero-byte tail does not publish a block");

    tail = rdma::ClassifyRawBlockTail(
        block_bytes, record_bytes, record_bytes);
    Expect(tail.disposition == rdma::RawBlockTailDisposition::kPublish &&
               tail.valid_records == 1 && tail.valid_bytes == record_bytes,
           "one complete record is publishable");

    tail = rdma::ClassifyRawBlockTail(
        block_bytes, record_bytes, 4U * record_bytes);
    Expect(tail.disposition == rdma::RawBlockTailDisposition::kPublish &&
               tail.valid_records == 4,
           "one work batch of complete records is publishable");

    tail = rdma::ClassifyRawBlockTail(
        block_bytes, record_bytes, 15U * record_bytes);
    Expect(tail.disposition == rdma::RawBlockTailDisposition::kPublish &&
               tail.valid_records == 15,
           "one record below a full block is publishable");

    tail = rdma::ClassifyRawBlockTail(
        block_bytes, record_bytes, record_bytes + 1U);
    Expect(tail.disposition == rdma::RawBlockTailDisposition::kInvalid,
           "partial record byte count is rejected");
    tail = rdma::ClassifyRawBlockTail(
        block_bytes, record_bytes, block_bytes + record_bytes);
    Expect(tail.disposition == rdma::RawBlockTailDisposition::kInvalid,
           "tail larger than a block is rejected");
    tail = rdma::ClassifyRawBlockTail(block_bytes, 0, 0);
    Expect(tail.disposition == rdma::RawBlockTailDisposition::kInvalid,
           "zero record geometry is rejected");
}

void TestDirectRawConfigurationContract() {
    namespace rdma = rdma_dada::io::rdma;
    Expect(rdma::kDirectRawNsge == 2 &&
               rdma::kDirectRawHeaderBytes == 42,
           "direct raw fixes two SGEs and a 42-byte network header");
    Expect(rdma::ValidateDirectRawConfiguration(1024, 12800, 16),
           "direct raw accepts a bounded WR queue and ring geometry");
    Expect(!rdma::ValidateDirectRawConfiguration(12801, 12800, 16),
           "two-block direct raw rejects WR depth larger than one block");
    Expect(!rdma::ValidateDirectRawConfiguration(1024, 12800, 1),
           "direct raw requires at least two ring blocks");
}

void TestDirectRawWrongLengthPolicy() {
    namespace rdma = rdma_dada::io::rdma;
    std::uint32_t consecutive = 0;
    for (std::uint32_t count = 1; count < 16; ++count) {
        const rdma::DirectRawCompletionAction action =
            rdma::ClassifyDirectRawCompletion(true, true, true, 4169, 4170,
                                               &consecutive);
        Expect(action == rdma::DirectRawCompletionAction::kZeroSlot,
               "isolated direct raw wrong-length packet zeroes one slot");
        Expect(consecutive == count,
               "direct raw counts consecutive wrong-length packets");
    }
    Expect(rdma::ClassifyDirectRawCompletion(
               true, true, true, 4169, 4170, &consecutive) ==
               rdma::DirectRawCompletionAction::kFatal,
           "sixteenth consecutive wrong-length packet stops the transfer");

    consecutive = 7;
    Expect(rdma::ClassifyDirectRawCompletion(
               true, true, true, 4170, 4170, &consecutive) ==
               rdma::DirectRawCompletionAction::kKeepSlot &&
               consecutive == 0,
           "valid direct raw packet resets the consecutive error count");
    Expect(rdma::ClassifyDirectRawCompletion(
               false, true, true, 4170, 4170, &consecutive) ==
               rdma::DirectRawCompletionAction::kFatal,
           "CQ failure remains fatal in direct raw mode");
    Expect(rdma::ClassifyDirectRawCompletion(
               true, false, true, 4170, 4170, &consecutive) ==
               rdma::DirectRawCompletionAction::kFatal,
           "unexpected CQ opcode remains fatal in direct raw mode");
    Expect(rdma::ClassifyDirectRawCompletion(
               true, true, false, 4170, 4170, &consecutive) ==
               rdma::DirectRawCompletionAction::kFatal,
           "invalid WR identity remains fatal in direct raw mode");
}

void TestDirectRawDrainPolicy() {
    namespace rdma = rdma_dada::io::rdma;
    const std::uint64_t started_ns = UINT64_C(5000000000);
    const std::uint64_t deadline_ns =
        started_ns + rdma::kDirectRawDrainDurationNs;

    Expect(rdma::kDirectRawDrainDurationNs == UINT64_C(1000000000),
           "direct raw drain retains one second after stop");
    Expect(!rdma::ShouldCheckDirectRawDrainClock(4095) &&
               rdma::ShouldCheckDirectRawDrainClock(4096),
           "direct raw drain checks the clock every 4096 empty polls");
    Expect(rdma::ShouldRepostDirectRawWr(true, false),
           "direct raw continues WR reposting while drain is active");
    Expect(!rdma::ShouldRepostDirectRawWr(true, true),
           "direct raw stops WR reposting after the drain deadline");
    Expect(!rdma::DirectRawDrainDeadlineReached(
               deadline_ns - 1U, deadline_ns) &&
               rdma::DirectRawDrainDeadlineReached(deadline_ns, deadline_ns),
           "direct raw drain exits on the monotonic one-second deadline");
}

void TestDirectRawBlockProgress() {
    namespace rdma = rdma_dada::io::rdma;
    rdma::DirectRawBlockProgress progress(4);
    Expect(progress.AssignSlot(0) && progress.AssignSlot(1) &&
               progress.AssignSlot(2) && progress.AssignSlot(3),
           "direct raw assigns every block slot once");
    Expect(!progress.AssignSlot(3) && !progress.AssignSlot(4),
           "direct raw rejects duplicate and out-of-range slot assignment");
    Expect(progress.CompleteSlot(1) && progress.CompleteSlot(0),
           "direct raw accepts out-of-order CQ completion within one block");
    Expect(progress.ContiguousCompletedSlots() == 2,
           "direct raw reports only the continuous completed prefix");
    Expect(!progress.ready_to_publish(),
           "partially completed full block is not publishable");
    Expect(progress.CompleteSlot(3) && progress.CompleteSlot(2),
           "all assigned direct raw slots can complete");
    Expect(progress.ready_to_publish() &&
               progress.ContiguousCompletedSlots() == 4,
           "full block publishes only after every slot completes");
}

void TestDirectRawOutstandingBlockOrder() {
    namespace rdma = rdma_dada::io::rdma;
    rdma::DirectRawOutstandingBlockOrder order;
    Expect(order.Push(7) && order.Push(8),
           "direct raw keeps exactly two acquired ring blocks");
    Expect(!order.Push(9),
           "direct raw refuses a third outstanding ring block");
    Expect(!order.PopFront(8),
           "direct raw cannot commit the second ring block first");
    Expect(order.PopFront(7) && order.Push(9),
           "committing the oldest block opens one replacement slot");
    Expect(order.front() == 8 && order.size() == 2,
           "direct raw preserves FIFO ring ownership");
    Expect(order.PopFront(8) && order.PopFront(9) && order.empty(),
           "direct raw drains outstanding ring blocks in order");
}

}  // namespace

int main() {
    TestDestinationFilterWildcardsEverySourceField();
    TestPeriodicReceiveStatusRequiresDebugMode();
    TestRawBlockTailAccounting();
    TestDirectRawConfigurationContract();
    TestDirectRawWrongLengthPolicy();
    TestDirectRawDrainPolicy();
    TestDirectRawBlockProgress();
    TestDirectRawOutstandingBlockOrder();
    if (failures != 0) {
        std::fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return 1;
    }
    return 0;
}
