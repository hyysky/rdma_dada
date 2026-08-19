#pragma once

#include "rdma_dada/modules/vdif_unpack/atfp_block_view.h"
#include "rdma_dada/pipeline/group_block_writer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {

struct AtfpBlockWriterStatistics {
    std::uint64_t acquire_calls;
    std::uint64_t commit_calls;
    std::uint64_t committed_blocks;
    std::uint64_t committed_bytes;
    std::uint64_t acquire_wait_ns;
};

class AtfpBlockWriter {
public:
    AtfpBlockWriter();

    bool Configure(std::uint64_t block_capacity,
                   pipeline::WritableBlockSink* sink,
                   std::string* error);
    bool Write(const AtfpBlockView& view, std::string* error);
    bool Finish(std::string* error);
    const AtfpBlockWriterStatistics& statistics() const;

private:
    std::uint64_t block_capacity_;
    pipeline::WritableBlockSink* sink_;
    bool configured_;
    bool finished_;
    bool failed_;
    AtfpBlockWriterStatistics statistics_;
};

struct AsyncAtfpBlockWriterStatistics {
    std::uint64_t enqueued_blocks;
    std::uint64_t queue_high_watermark;
    std::uint64_t enqueue_wait_ns;
};

class AsyncAtfpBlockWriter {
public:
    AsyncAtfpBlockWriter();
    ~AsyncAtfpBlockWriter();

    bool Configure(std::uint64_t block_capacity,
                   std::uint64_t queue_capacity,
                   int writer_cpu,
                   pipeline::WritableBlockSink* sink,
                   const std::function<bool(std::uint64_t, std::string*)>&
                       release,
                   std::string* error);
    bool Enqueue(const AtfpBlockView& view, std::string* error);
    bool Finish(std::string* error);
    void Abort();
    const AtfpBlockWriterStatistics& writer_statistics() const;
    const AsyncAtfpBlockWriterStatistics& statistics() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
