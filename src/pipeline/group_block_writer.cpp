#include "rdma_dada/pipeline/group_block_writer.h"

#include <cstring>
#include <limits>

namespace rdma_dada {
namespace pipeline {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

}  // namespace

GroupBlockWriter::GroupBlockWriter()
    : group_bytes_(0), block_capacity_(0), sink_(NULL), block_(NULL),
      offset_(0), configured_(false), finished_(false), failed_(false) {}

bool GroupBlockWriter::Configure(std::uint64_t group_bytes,
                                 std::uint64_t block_capacity,
                                 WritableBlockSink* sink,
                                 std::string* error) {
    if (configured_ && !finished_)
        return Fail("cannot reconfigure an active block writer", error);
    if (!sink) return Fail("block sink pointer is null", error);
    if (group_bytes == 0) return Fail("group_bytes must be positive", error);
    if (block_capacity == 0 || block_capacity % group_bytes != 0)
        return Fail("block_capacity must be a positive multiple of group_bytes", error);
    if (block_capacity > std::numeric_limits<std::size_t>::max())
        return Fail("block_capacity exceeds host size_t range", error);

    group_bytes_ = group_bytes;
    block_capacity_ = block_capacity;
    sink_ = sink;
    block_ = NULL;
    offset_ = 0;
    configured_ = true;
    finished_ = false;
    failed_ = false;
    return true;
}

bool GroupBlockWriter::CommitCurrent(std::uint64_t bytes,
                                     std::string* error) {
    if (!sink_->Commit(bytes, error)) {
        failed_ = true;
        return false;
    }
    block_ = NULL;
    offset_ = 0;
    return true;
}

bool GroupBlockWriter::Append(const std::uint8_t* group,
                              std::uint64_t bytes,
                              std::string* error) {
    if (!configured_) return Fail("block writer is not configured", error);
    if (finished_) return Fail("cannot append after Finish", error);
    if (failed_) return Fail("block writer is in a failed transfer", error);
    if (!group) return Fail("group data pointer is null", error);
    if (bytes != group_bytes_) return Fail("group byte count does not match configuration", error);

    if (!block_) {
        std::uint8_t* acquired = NULL;
        std::uint64_t capacity = 0;
        if (!sink_->Acquire(&acquired, &capacity, error)) {
            failed_ = true;
            return false;
        }
        if (!acquired || capacity != block_capacity_) {
            failed_ = true;
            return Fail("acquired block capacity does not match configuration", error);
        }
        block_ = acquired;
    }

    std::memcpy(block_ + static_cast<std::size_t>(offset_), group,
                static_cast<std::size_t>(group_bytes_));
    offset_ += group_bytes_;
    if (offset_ == block_capacity_)
        return CommitCurrent(block_capacity_, error);
    return true;
}

bool GroupBlockWriter::Finish(std::string* error) {
    if (!configured_) return Fail("block writer is not configured", error);
    if (finished_) return true;
    if (failed_) return Fail("block writer is in a failed transfer", error);
    if (block_ && offset_ != 0 && !CommitCurrent(offset_, error)) return false;
    finished_ = true;
    return true;
}

}  // namespace pipeline
}  // namespace rdma_dada
