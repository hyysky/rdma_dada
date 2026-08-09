#include "rdma_dada/modules/vdif_unpack/atfp_block_writer.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (!result ||
        (left != 0U &&
         right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *result = left * right;
    return true;
}

}  // namespace

AtfpBlockWriter::AtfpBlockWriter()
    : block_capacity_(0), sink_(NULL), configured_(false), finished_(false),
      failed_(false), statistics_() {}

bool AtfpBlockWriter::Configure(std::uint64_t block_capacity,
                                pipeline::WritableBlockSink* sink,
                                std::string* error) {
    if (configured_ && !finished_)
        return Fail("cannot reconfigure an active ATFP block writer", error);
    if (!sink) return Fail("ATFP block sink pointer is null", error);
    if (block_capacity == 0U ||
        block_capacity > std::numeric_limits<std::size_t>::max()) {
        return Fail("ATFP block capacity must fit positive host size_t", error);
    }
    block_capacity_ = block_capacity;
    sink_ = sink;
    configured_ = true;
    finished_ = false;
    failed_ = false;
    statistics_ = AtfpBlockWriterStatistics();
    return true;
}

bool AtfpBlockWriter::Write(const AtfpBlockView& view,
                            std::string* error) {
    if (!configured_) return Fail("ATFP block writer is not configured", error);
    if (finished_) return Fail("cannot write after ATFP writer Finish", error);
    if (failed_) return Fail("ATFP block writer is in a failed transfer", error);
    if (!view.window_data)
        return Fail("ATFP window data pointer is null", error);
    if (view.window_capacity_groups == 0U || view.group_count == 0U ||
        view.nant == 0U || view.packet_payload_bytes == 0U) {
        return Fail("ATFP block view extents must be positive", error);
    }
    if (view.first_slot >= view.window_capacity_groups ||
        view.group_count > view.window_capacity_groups) {
        return Fail("ATFP block view exceeds circular window", error);
    }

    std::uint64_t bytes_per_antenna = 0;
    std::uint64_t output_bytes = 0;
    std::uint64_t window_plane_bytes = 0;
    if (!CheckedMultiply(view.group_count, view.packet_payload_bytes,
                         &bytes_per_antenna) ||
        !CheckedMultiply(bytes_per_antenna, view.nant, &output_bytes) ||
        !CheckedMultiply(view.window_capacity_groups,
                         view.packet_payload_bytes, &window_plane_bytes) ||
        output_bytes > block_capacity_) {
        return Fail("ATFP block view byte geometry exceeds capacity", error);
    }

    const std::uint64_t first_groups = std::min(
        view.group_count, view.window_capacity_groups - view.first_slot);
    const std::uint64_t second_groups = view.group_count - first_groups;
    std::uint64_t first_bytes = 0;
    std::uint64_t second_bytes = 0;
    if (!CheckedMultiply(first_groups, view.packet_payload_bytes,
                         &first_bytes) ||
        !CheckedMultiply(second_groups, view.packet_payload_bytes,
                         &second_bytes)) {
        return Fail("ATFP circular segment geometry overflows", error);
    }

    std::uint8_t* block = NULL;
    std::uint64_t acquired_capacity = 0;
    const std::chrono::steady_clock::time_point acquire_begin =
        std::chrono::steady_clock::now();
    ++statistics_.acquire_calls;
    const bool acquired = sink_->Acquire(&block, &acquired_capacity, error);
    const std::chrono::steady_clock::time_point acquire_end =
        std::chrono::steady_clock::now();
    statistics_.acquire_wait_ns += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            acquire_end - acquire_begin).count());
    if (!acquired) {
        failed_ = true;
        return false;
    }
    if (!block || acquired_capacity != block_capacity_) {
        failed_ = true;
        return Fail("acquired ATFP block capacity does not match configuration",
                    error);
    }

    for (std::uint32_t antenna = 0; antenna < view.nant; ++antenna) {
        const std::uint8_t* source_plane =
            view.window_data +
            static_cast<std::size_t>(antenna * window_plane_bytes);
        std::uint8_t* destination_plane =
            block + static_cast<std::size_t>(antenna * bytes_per_antenna);
        std::memcpy(destination_plane,
                    source_plane + static_cast<std::size_t>(
                        view.first_slot * view.packet_payload_bytes),
                    static_cast<std::size_t>(first_bytes));
        if (second_bytes != 0U) {
            std::memcpy(destination_plane + static_cast<std::size_t>(first_bytes),
                        source_plane,
                        static_cast<std::size_t>(second_bytes));
        }
    }

    ++statistics_.commit_calls;
    if (!sink_->Commit(output_bytes, error)) {
        failed_ = true;
        return false;
    }
    ++statistics_.committed_blocks;
    statistics_.committed_bytes += output_bytes;
    return true;
}

bool AtfpBlockWriter::Finish(std::string* error) {
    if (!configured_) return Fail("ATFP block writer is not configured", error);
    if (finished_) return true;
    if (failed_) return Fail("ATFP block writer is in a failed transfer", error);
    finished_ = true;
    return true;
}

const AtfpBlockWriterStatistics& AtfpBlockWriter::statistics() const {
    return statistics_;
}

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
