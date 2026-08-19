#include "rdma_dada/modules/vdif_unpack/atfp_block_writer.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class MemorySink : public rdma_dada::pipeline::WritableBlockSink {
public:
    explicit MemorySink(std::uint64_t capacity)
        : storage(static_cast<std::size_t>(capacity), 0), open(false),
          fail_acquire(false), fail_commit(false), acquire_count(0),
          commit_count(0) {}

    bool Acquire(std::uint8_t** data, std::uint64_t* capacity,
                 std::string* error) {
        ++acquire_count;
        if (fail_acquire) {
            if (error) *error = "injected acquire failure";
            return false;
        }
        if (open) {
            if (error) *error = "second block acquired while first is open";
            return false;
        }
        open = true;
        std::fill(storage.begin(), storage.end(), 0xeeU);
        *data = storage.data();
        *capacity = storage.size();
        return true;
    }

    bool Commit(std::uint64_t bytes, std::string* error) {
        ++commit_count;
        if (!open) {
            if (error) *error = "commit without acquire";
            return false;
        }
        if (fail_commit) {
            if (error) *error = "injected commit failure";
            return false;
        }
        committed.push_back(std::vector<std::uint8_t>(
            storage.begin(), storage.begin() + static_cast<std::ptrdiff_t>(bytes)));
        open = false;
        return true;
    }

    std::vector<std::uint8_t> storage;
    std::vector<std::vector<std::uint8_t> > committed;
    bool open;
    bool fail_acquire;
    bool fail_commit;
    std::uint64_t acquire_count;
    std::uint64_t commit_count;
};

std::vector<std::uint8_t> MakeWindow() {
    // [A=3, circular_group=4, payload_bytes=4]
    std::vector<std::uint8_t> window(48);
    for (std::uint64_t antenna = 0; antenna < 3; ++antenna) {
        for (std::uint64_t slot = 0; slot < 4; ++slot) {
            for (std::uint64_t byte = 0; byte < 4; ++byte) {
                window[static_cast<std::size_t>(
                    (antenna * 4U + slot) * 4U + byte)] =
                    static_cast<std::uint8_t>(antenna * 64U + slot * 8U + byte);
            }
        }
    }
    return window;
}

rdma_dada::modules::vdif_unpack::AtfpBlockView MakeView(
    const std::vector<std::uint8_t>& window, std::uint64_t first_slot,
    std::uint64_t group_count) {
    rdma_dada::modules::vdif_unpack::AtfpBlockView view = {};
    view.window_data = window.data();
    view.window_capacity_groups = 4;
    view.first_group_ordinal = 20;
    view.first_slot = first_slot;
    view.group_count = group_count;
    view.nant = 3;
    view.packet_payload_bytes = 4;
    return view;
}

std::vector<std::uint8_t> Expected(const std::vector<std::uint8_t>& window,
                                   std::uint64_t first_slot,
                                   std::uint64_t groups) {
    std::vector<std::uint8_t> expected;
    for (std::uint64_t antenna = 0; antenna < 3; ++antenna) {
        for (std::uint64_t group = 0; group < groups; ++group) {
            const std::uint64_t slot = (first_slot + group) % 4U;
            const std::uint64_t source = (antenna * 4U + slot) * 4U;
            expected.insert(expected.end(), window.begin() + source,
                            window.begin() + source + 4U);
        }
    }
    return expected;
}

void TestNoWrapAndWrapUseOneBlockEach() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    const std::vector<std::uint8_t> window = MakeWindow();
    MemorySink sink(24);
    unpack::AtfpBlockWriter writer;
    std::string error;
    Expect(writer.Configure(24, &sink, &error),
           "writer configures: " + error);
    Expect(writer.Write(MakeView(window, 1, 2), &error),
           "non-wrapped range writes: " + error);
    Expect(writer.Write(MakeView(window, 3, 2), &error),
           "wrapped range writes: " + error);
    Expect(writer.Finish(&error), "writer finishes: " + error);
    Expect(sink.acquire_count == 2U && sink.commit_count == 2U,
           "each output block has exactly one acquire and one commit");
    const unpack::AtfpBlockWriterStatistics& statistics = writer.statistics();
    Expect(statistics.acquire_calls == 2U && statistics.commit_calls == 2U &&
               statistics.committed_blocks == 2U &&
               statistics.committed_bytes == 48U,
           "writer reports exact sink lifecycle and committed bytes");
    Expect(!sink.open, "no writable block remains held after Write");
    Expect(sink.committed.size() == 2U, "two blocks are committed");
    if (sink.committed.size() == 2U) {
        Expect(sink.committed[0] == Expected(window, 1, 2),
               "non-wrapped output is compact ATFP");
        Expect(sink.committed[1] == Expected(window, 3, 2),
               "two disjoint wrap segments copy each group exactly once");
    }
}

void TestPartialBlockUsesActualAntennaStride() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    const std::vector<std::uint8_t> window = MakeWindow();
    MemorySink sink(24);
    unpack::AtfpBlockWriter writer;
    std::string error;
    Expect(writer.Configure(24, &sink, &error),
           "partial writer configures: " + error);
    Expect(writer.Write(MakeView(window, 2, 1), &error),
           "partial range writes: " + error);
    Expect(sink.committed.size() == 1U && sink.committed[0].size() == 12U,
           "partial block commits A*actual_G*payload bytes");
    if (sink.committed.size() == 1U)
        Expect(sink.committed[0] == Expected(window, 2, 1),
               "partial block has no nominal-stride antenna holes");
}

void TestInvalidViewDoesNotAcquire() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    const std::vector<std::uint8_t> window = MakeWindow();
    MemorySink sink(24);
    unpack::AtfpBlockWriter writer;
    std::string error;
    Expect(writer.Configure(24, &sink, &error),
           "validation writer configures: " + error);
    unpack::AtfpBlockView invalid = MakeView(window, 0, 2);
    invalid.first_slot = invalid.window_capacity_groups;
    Expect(!writer.Write(invalid, &error), "out-of-range first slot is rejected");
    Expect(sink.acquire_count == 0U && !sink.open,
           "invalid geometry is rejected before acquiring a block");
    Expect(writer.statistics().acquire_calls == 0U,
           "invalid geometry is absent from acquire statistics");
}

void TestSinkFailuresPoisonOnlyThatWriter() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    const std::vector<std::uint8_t> window = MakeWindow();
    std::string error;
    MemorySink acquire_sink(24);
    acquire_sink.fail_acquire = true;
    unpack::AtfpBlockWriter acquire_writer;
    Expect(acquire_writer.Configure(24, &acquire_sink, &error),
           "acquire-failure writer configures");
    Expect(!acquire_writer.Write(MakeView(window, 0, 2), &error),
           "sink acquire failure propagates");
    Expect(!acquire_writer.Write(MakeView(window, 0, 2), &error),
           "failed writer cannot publish later data");

    MemorySink commit_sink(24);
    commit_sink.fail_commit = true;
    unpack::AtfpBlockWriter commit_writer;
    Expect(commit_writer.Configure(24, &commit_sink, &error),
           "commit-failure writer configures");
    Expect(!commit_writer.Write(MakeView(window, 0, 2), &error),
           "sink commit failure propagates");
    Expect(commit_sink.acquire_count == 1U && commit_sink.commit_count == 1U,
           "commit failure still uses only one acquired block");
    Expect(commit_writer.statistics().acquire_calls == 1U &&
               commit_writer.statistics().commit_calls == 1U &&
               commit_writer.statistics().committed_blocks == 0U,
           "failed commit is counted but not reported as published");
}

void TestAsyncWriterPreservesFifoAndReleasesLeases() {
    namespace unpack = rdma_dada::modules::vdif_unpack;
    const std::vector<std::uint8_t> window = MakeWindow();
    MemorySink sink(24);
    std::vector<std::uint64_t> released;
    std::mutex released_mutex;
    unpack::AsyncAtfpBlockWriter writer;
    std::string error;
    Expect(writer.Configure(
               24, 2, -1, &sink,
               [&released, &released_mutex](std::uint64_t lease,
                                            std::string*) {
                   std::lock_guard<std::mutex> lock(released_mutex);
                   released.push_back(lease);
                   return true;
               },
               &error),
           "async writer configures: " + error);
    unpack::AtfpBlockView first = MakeView(window, 0, 2);
    unpack::AtfpBlockView second = MakeView(window, 2, 2);
    first.lease_id = 11;
    second.lease_id = 12;
    Expect(writer.Enqueue(first, &error), "first lease enqueues: " + error);
    Expect(writer.Enqueue(second, &error), "second lease enqueues: " + error);
    Expect(writer.Finish(&error), "async writer drains: " + error);
    Expect(sink.committed.size() == 2U, "async writer commits two blocks");
    Expect(released == std::vector<std::uint64_t>({11, 12}),
           "async writer releases leases in FIFO order");
    Expect(writer.statistics().enqueued_blocks == 2U,
           "async writer reports enqueue count");
}

}  // namespace

int main() {
    TestNoWrapAndWrapUseOneBlockEach();
    TestPartialBlockUsesActualAntennaStride();
    TestInvalidViewDoesNotAcquire();
    TestSinkFailuresPoisonOnlyThatWriter();
    TestAsyncWriterPreservesFifoAndReleasesLeases();
    if (failures != 0) return 1;
    std::cout << "atfp_block_writer_test passed\n";
    return 0;
}
