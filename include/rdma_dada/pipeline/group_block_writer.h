#pragma once

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace pipeline {

class WritableBlockSink {
public:
    virtual ~WritableBlockSink() {}
    virtual bool Acquire(std::uint8_t** data, std::uint64_t* capacity,
                         std::string* error) = 0;
    virtual bool Commit(std::uint64_t bytes, std::string* error) = 0;
};

class GroupBlockWriter {
public:
    GroupBlockWriter();

    bool Configure(std::uint64_t group_bytes,
                   std::uint64_t block_capacity,
                   WritableBlockSink* sink,
                   std::string* error);
    bool Append(const std::uint8_t* group, std::uint64_t bytes,
                std::string* error);
    bool Finish(std::string* error);

private:
    bool CommitCurrent(std::uint64_t bytes, std::string* error);

    std::uint64_t group_bytes_;
    std::uint64_t block_capacity_;
    WritableBlockSink* sink_;
    std::uint8_t* block_;
    std::uint64_t offset_;
    bool configured_;
    bool finished_;
    bool failed_;
};

}  // namespace pipeline
}  // namespace rdma_dada
