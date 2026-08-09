#pragma once

#include "rdma_dada/modules/vdif_unpack/atfp_block_view.h"
#include "rdma_dada/pipeline/group_block_writer.h"

#include <cstdint>
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

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
