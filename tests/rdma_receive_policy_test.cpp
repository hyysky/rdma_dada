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

void TestSourceUdpFilterMatchesOneSenderFlow() {
    const std::uint8_t destination_mac[6] = {
        0x98, 0x03, 0x9b, 0xaa, 0x99, 0xd8
    };
    const std::uint32_t source_ip = UINT32_C(0x640100ae);
    const std::uint32_t destination_ip = UINT32_C(0x6f0100ae);
    const std::uint16_t source_port = UINT16_C(41001);
    const std::uint16_t destination_port = UINT16_C(1000);

    const rdma_dada::io::rdma::DestinationUdpFilter filter =
        rdma_dada::io::rdma::BuildSourceUdpFilter(
            destination_mac, source_ip, source_port,
            destination_ip, destination_port);

    Expect(filter.source_ip == source_ip &&
               filter.source_ip_mask == UINT32_MAX,
           "source IPv4 is matched exactly for one receiver shard");
    Expect(filter.source_port == source_port &&
               filter.source_port_mask == UINT16_MAX,
           "source UDP port is matched exactly for one receiver shard");
    Expect(filter.destination_ip == destination_ip &&
               filter.destination_ip_mask == UINT32_MAX,
           "sharded filter preserves exact destination IPv4");
    Expect(filter.destination_port == destination_port &&
               filter.destination_port_mask == UINT16_MAX,
           "sharded filter preserves exact destination UDP port");
}

void TestReceiveFlowSpecParser() {
    namespace rdma = rdma_dada::io::rdma;
    rdma::ReceiveFlowSpec flow;
    std::string error;
    Expect(rdma::ParseReceiveFlowSpec("174.0.1.100:41001", &flow, &error),
           "IPv4 and UDP source port form a valid receive flow");
    Expect(flow.source_ip == "174.0.1.100" && flow.source_port == 41001,
           "receive flow parser preserves source endpoint");
    Expect(!rdma::ParseReceiveFlowSpec("174.0.1.100", &flow, &error),
           "receive flow without a source port is rejected");
    Expect(!rdma::ParseReceiveFlowSpec("174.0.1.100:0", &flow, &error),
           "receive flow rejects port zero");
    Expect(!rdma::ParseReceiveFlowSpec("not-an-ip:41001", &flow, &error),
           "receive flow rejects invalid IPv4 text");
}

void TestReceiveShardCpuPlacement() {
    namespace rdma = rdma_dada::io::rdma;
    rdma::ReceiveShardCpuPlacement placement =
        rdma::ResolveReceiveShardCpuPlacement(-1, {13, 14}, 15, 2);
    Expect(placement.valid && placement.poll_cpus.size() == 2 &&
               placement.poll_cpus[0] == 13 && placement.poll_cpus[1] == 14 &&
               placement.copy_cpu == 15,
           "two receiver shards use distinct poll CPUs and one writer CPU");
    Expect(!rdma::ResolveReceiveShardCpuPlacement(-1, {13}, 15, 2).valid,
           "poll CPU count must equal receiver shard count");
    Expect(!rdma::ResolveReceiveShardCpuPlacement(-1, {13, 13}, 15, 2).valid,
           "receiver shards cannot share an explicitly assigned poll CPU");
    Expect(!rdma::ResolveReceiveShardCpuPlacement(-1, {13, 15}, 15, 2).valid,
           "writer CPU cannot overlap a receiver poll CPU");
    placement = rdma::ResolveReceiveShardCpuPlacement(13, {}, 15, 1);
    Expect(placement.valid && placement.poll_cpus == std::vector<int>({13}),
           "legacy poll CPU remains compatible with one receiver shard");
}

void TestWrongLengthIsRecoverable() {
    rdma_dada::io::rdma::ReceiveCompletion completion;
    completion.success = true;
    completion.receive_opcode = true;
    completion.wr_id = 7;
    completion.byte_len = 1057;

    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kDropWrongLength,
           "successful wrong-length receive is recoverable");

    completion.byte_len = 1066;
    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kAccept,
           "matching receive completion is accepted");
}

void TestTransportErrorsRemainFatal() {
    rdma_dada::io::rdma::ReceiveCompletion completion;
    completion.success = false;
    completion.receive_opcode = true;
    completion.wr_id = 7;
    completion.byte_len = 1066;
    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kFatal,
           "failed CQ status is fatal");

    completion.success = true;
    completion.receive_opcode = false;
    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kFatal,
           "unexpected CQ opcode is fatal");

    completion.receive_opcode = true;
    completion.wr_id = 32;
    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kFatal,
           "out-of-range WR ID is fatal");
}

void TestWrongLengthLogIsRateLimited() {
    Expect(!rdma_dada::io::rdma::ShouldLogWrongLengthDrop(0),
           "zero drops do not log");
    Expect(rdma_dada::io::rdma::ShouldLogWrongLengthDrop(1),
           "first drop logs");
    Expect(rdma_dada::io::rdma::ShouldLogWrongLengthDrop(2),
           "second drop logs");
    Expect(!rdma_dada::io::rdma::ShouldLogWrongLengthDrop(3),
           "third drop is rate limited");
    Expect(rdma_dada::io::rdma::ShouldLogWrongLengthDrop(4),
           "power-of-two drop count logs");
    Expect(!rdma_dada::io::rdma::ShouldLogWrongLengthDrop(5),
           "non-power-of-two drop count is rate limited");
}

void TestPeriodicReceiveStatusRequiresDebugMode() {
    Expect(!rdma_dada::io::rdma::ShouldEmitPeriodicReceiveStatus(false),
           "normal receive mode suppresses periodic status work");
    Expect(rdma_dada::io::rdma::ShouldEmitPeriodicReceiveStatus(true),
           "debug receive mode retains periodic status work");
}

void TestTunedReceiveQueueDefaults() {
    namespace rdma = rdma_dada::io::rdma;
    Expect(rdma::kDefaultReceiveCopyBatch == 64,
           "default receive copy batch is 64 packets");
    Expect(rdma::kDefaultReceiveNsge == 1,
           "default receive WR uses one SGE");
    Expect(rdma::kDefaultReceivePollBatch == 32,
           "default CQ poll batch is 32 completions");
    Expect(rdma::kDefaultReceiveWrDepth == 1024,
           "default receive WR depth is 1024");
}

void TestReceiveSpscQueuePreservesOwnershipOrder() {
    namespace rdma = rdma_dada::io::rdma;
    rdma::ReceiveSpscQueue queue(4);
    rdma::ReceiveWorkItem item = {};

    Expect(queue.empty(), "new receive SPSC queue is empty");
    Expect(queue.TryPush({10, 100}), "first ownership push succeeds");
    Expect(queue.TryPush({11, 110}), "second ownership push succeeds");
    Expect(queue.size() == 2, "queue size counts pending ownership items");
    Expect(queue.TryPop(&item) && item.wr_id == 10 &&
               item.completion_ns == 100,
           "first ownership item is popped first");
    Expect(queue.TryPop(&item) && item.wr_id == 11 &&
               item.completion_ns == 110,
           "second ownership item follows first");
    Expect(!queue.TryPop(&item), "empty ownership pop fails cleanly");
}

void TestReceiveSpscQueueWrapsAtUsableCapacity() {
    namespace rdma = rdma_dada::io::rdma;
    rdma::ReceiveSpscQueue queue(3);
    rdma::ReceiveWorkItem item = {};

    Expect(queue.capacity() == 3, "reported capacity is fully usable");
    Expect(queue.TryPush({1, 10}), "capacity slot one is usable");
    Expect(queue.TryPush({2, 20}), "capacity slot two is usable");
    Expect(queue.TryPush({3, 30}), "capacity slot three is usable");
    Expect(!queue.TryPush({4, 40}), "full ownership queue rejects push");
    Expect(queue.TryPop(&item) && item.wr_id == 1,
           "oldest item is released before wrap");
    Expect(queue.TryPush({4, 40}), "push succeeds after index wrap");
    Expect(queue.high_watermark() == 3,
           "high-water mark preserves peak occupancy");
    Expect(queue.TryPop(&item) && item.wr_id == 2,
           "wrapped queue preserves second item");
    Expect(queue.TryPop(&item) && item.wr_id == 3,
           "wrapped queue preserves third item");
    Expect(queue.TryPop(&item) && item.wr_id == 4,
           "wrapped queue appends fourth item");
    Expect(queue.empty(), "wrapped queue drains completely");
}

void TestReceiveBatchUsesAvailableWorkImmediately() {
    namespace rdma = rdma_dada::io::rdma;
    Expect(rdma::SelectAvailableBatch(0, 64) == 0,
           "empty ownership queue produces no batch");
    Expect(rdma::SelectAvailableBatch(1, 64) == 1,
           "one available record does not wait for 64");
    Expect(rdma::SelectAvailableBatch(63, 64) == 63,
           "partial available batch is processed immediately");
    Expect(rdma::SelectAvailableBatch(64, 64) == 64,
           "full available batch uses send_n");
    Expect(rdma::SelectAvailableBatch(100, 64) == 64,
           "available work is capped at send_n");
    Expect(rdma::SelectAvailableBatch(10, 0) == 0,
           "zero maximum batch rejects work");
}

void TestReceiveCpuPlacementResolution() {
    namespace rdma = rdma_dada::io::rdma;
    rdma::ReceiveCpuPlacement placement =
        rdma::ResolveReceiveCpuPlacement(-1, -1, -1);
    Expect(placement.valid && placement.poll_cpu == -1 &&
               placement.copy_cpu == -1,
           "unbound receive placement remains valid");
    placement = rdma::ResolveReceiveCpuPlacement(13, -1, 14);
    Expect(placement.valid && placement.poll_cpu == 13 &&
               placement.copy_cpu == 14,
           "legacy cpu aliases poll cpu");
    placement = rdma::ResolveReceiveCpuPlacement(-1, 13, 14);
    Expect(placement.valid && placement.poll_cpu == 13 &&
               placement.copy_cpu == 14,
           "distinct explicit receive CPUs are valid");
    Expect(!rdma::ResolveReceiveCpuPlacement(12, 13, 14).valid,
           "conflicting legacy and poll CPUs are rejected");
    Expect(!rdma::ResolveReceiveCpuPlacement(-2, -1, -1).valid,
           "CPU values below minus one are rejected");
    Expect(!rdma::ResolveReceiveCpuPlacement(-1, 13, 13).valid,
           "poll and copy threads cannot share an explicit CPU");
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

}  // namespace

int main() {
    TestDestinationFilterWildcardsEverySourceField();
    TestSourceUdpFilterMatchesOneSenderFlow();
    TestReceiveFlowSpecParser();
    TestReceiveShardCpuPlacement();
    TestWrongLengthIsRecoverable();
    TestTransportErrorsRemainFatal();
    TestWrongLengthLogIsRateLimited();
    TestPeriodicReceiveStatusRequiresDebugMode();
    TestTunedReceiveQueueDefaults();
    TestReceiveSpscQueuePreservesOwnershipOrder();
    TestReceiveSpscQueueWrapsAtUsableCapacity();
    TestReceiveBatchUsesAvailableWorkImmediately();
    TestReceiveCpuPlacementResolution();
    TestRawBlockTailAccounting();
    if (failures != 0) {
        std::fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return 1;
    }
    return 0;
}
