#include "rdma_dada/pipeline/group_block_writer.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class MemoryBlockSink : public rdma_dada::pipeline::WritableBlockSink {
public:
    explicit MemoryBlockSink(std::uint64_t capacity)
        : storage_(static_cast<std::size_t>(capacity), 0), acquired_(false) {}

    bool Acquire(std::uint8_t** data, std::uint64_t* capacity,
                 std::string* error) {
        if (acquired_) {
            if (error) *error = "test sink block already acquired";
            return false;
        }
        acquired_ = true;
        std::memset(storage_.data(), 0xee, storage_.size());
        *data = storage_.data();
        *capacity = storage_.size();
        return true;
    }

    bool Commit(std::uint64_t bytes, std::string* error) {
        if (!acquired_ || bytes > storage_.size()) {
            if (error) *error = "test sink invalid commit";
            return false;
        }
        committed_blocks.push_back(std::vector<std::uint8_t>(
            storage_.begin(), storage_.begin() + static_cast<std::size_t>(bytes)));
        acquired_ = false;
        return true;
    }

    bool acquired() const { return acquired_; }

    std::vector<std::vector<std::uint8_t> > committed_blocks;

private:
    std::vector<std::uint8_t> storage_;
    bool acquired_;
};

void TestGroupsFillFullAndPartialBlocks() {
    MemoryBlockSink sink(12);
    rdma_dada::pipeline::GroupBlockWriter writer;
    std::string error;
    Expect(writer.Configure(4, 12, &sink, &error),
           "writer configures: " + error);
    const std::uint8_t groups[5][4] = {
        {0, 1, 2, 3}, {4, 5, 6, 7}, {8, 9, 10, 11},
        {12, 13, 14, 15}, {16, 17, 18, 19}
    };
    for (std::size_t i = 0; i < 5; ++i) {
        Expect(writer.Append(groups[i], 4, &error),
               "group append succeeds: " + error);
    }
    Expect(sink.committed_blocks.size() == 1U,
           "three groups commit one full block before EOD");
    Expect(writer.Finish(&error), "partial block commits at EOD: " + error);
    Expect(sink.committed_blocks.size() == 2U,
           "EOD commits exactly one non-empty partial block");
    if (sink.committed_blocks.size() == 2U) {
        const std::vector<std::uint8_t> full = {
            0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
        };
        const std::vector<std::uint8_t> partial = {
            12, 13, 14, 15, 16, 17, 18, 19
        };
        Expect(sink.committed_blocks[0] == full,
               "full block preserves exact group order and bytes");
        Expect(sink.committed_blocks[1] == partial,
               "partial block commits its exact valid byte count");
    }
    Expect(!sink.acquired(), "Finish releases the partial sink block");
    Expect(writer.Finish(&error), "second Finish is idempotent");
    Expect(sink.committed_blocks.size() == 2U,
           "second Finish does not commit an empty block");
}

void TestEmptyFinishIsLazy() {
    MemoryBlockSink sink(12);
    rdma_dada::pipeline::GroupBlockWriter writer;
    std::string error;
    Expect(writer.Configure(4, 12, &sink, &error), "empty writer configures");
    Expect(writer.Finish(&error), "empty writer finishes");
    Expect(sink.committed_blocks.empty() && !sink.acquired(),
           "empty transfer never acquires or commits a block");
}

void TestConfigurationAndAppendErrorsDoNotPublishData() {
    std::string error;
    MemoryBlockSink sink(12);
    rdma_dada::pipeline::GroupBlockWriter writer;
    Expect(!writer.Configure(0, 12, &sink, &error),
           "zero group size rejected");
    Expect(!writer.Configure(4, 10, &sink, &error),
           "block capacity must be a group multiple");
    Expect(!writer.Configure(4, 12, NULL, &error), "null sink rejected");
    Expect(writer.Configure(4, 12, &sink, &error), "valid writer configures");
    const std::uint8_t group[4] = {1, 2, 3, 4};
    Expect(!writer.Append(group, 3, &error), "wrong group byte count rejected");
    Expect(!writer.Append(NULL, 4, &error), "null group pointer rejected");
    Expect(sink.committed_blocks.empty() && !sink.acquired(),
           "invalid append is rejected before acquiring sink storage");
    Expect(writer.Append(group, 4, &error),
           "writer remains usable after an input contract error");
    Expect(writer.Finish(&error), "valid data commits after rejected input");
    Expect(sink.committed_blocks.size() == 1U &&
           sink.committed_blocks[0] == std::vector<std::uint8_t>({1, 2, 3, 4}),
           "only valid group bytes are published");
}

void TestAcquiredCapacityMustMatchConfiguration() {
    MemoryBlockSink wrong_capacity(8);
    rdma_dada::pipeline::GroupBlockWriter writer;
    std::string error;
    Expect(writer.Configure(4, 12, &wrong_capacity, &error),
           "writer configures before lazy sink acquire");
    const std::uint8_t group[4] = {1, 2, 3, 4};
    Expect(!writer.Append(group, 4, &error),
           "sink capacity mismatch is rejected");
    Expect(wrong_capacity.committed_blocks.empty(),
           "capacity mismatch never commits bytes");
}

}  // namespace

int main() {
    TestGroupsFillFullAndPartialBlocks();
    TestEmptyFinishIsLazy();
    TestConfigurationAndAppendErrorsDoNotPublishData();
    TestAcquiredCapacityMustMatchConfiguration();
    if (failures) return 1;
    std::cout << "group_block_writer_test passed\n";
    return 0;
}
