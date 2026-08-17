#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rdma_dada {
namespace io {
namespace rdma {

constexpr unsigned int kDefaultReceiveCopyBatch = 64;
constexpr unsigned int kDefaultReceiveNsge = 1;
constexpr unsigned int kDefaultReceivePollBatch = 32;
constexpr unsigned int kDefaultReceiveWrDepth = 1024;

struct ReceiveWorkItem {
    std::uint64_t wr_id;
    std::uint64_t completion_ns;
};

class ReceiveSpscQueue {
  public:
    explicit ReceiveSpscQueue(std::size_t capacity);

    bool TryPush(const ReceiveWorkItem& item);
    bool TryPop(ReceiveWorkItem* item);

    std::size_t size() const;
    bool empty() const;
    std::size_t capacity() const;
    std::size_t high_watermark() const;

  private:
    ReceiveSpscQueue(const ReceiveSpscQueue&) = delete;
    ReceiveSpscQueue& operator=(const ReceiveSpscQueue&) = delete;

    std::vector<ReceiveWorkItem> entries_;
    const std::size_t storage_size_;
    std::atomic<std::size_t> head_;
    std::atomic<std::size_t> tail_;
    std::atomic<std::size_t> high_watermark_;
};

std::size_t SelectAvailableBatch(std::size_t available,
                                 std::size_t maximum_batch);

struct ReceiveCpuPlacement {
    bool valid;
    int poll_cpu;
    int copy_cpu;
};

ReceiveCpuPlacement ResolveReceiveCpuPlacement(int legacy_cpu,
                                               int poll_cpu,
                                               int copy_cpu);

struct ReceiveShardCpuPlacement {
    bool valid;
    std::vector<int> poll_cpus;
    int copy_cpu;
};

ReceiveShardCpuPlacement ResolveReceiveShardCpuPlacement(
    int legacy_cpu, const std::vector<int>& poll_cpus, int copy_cpu,
    std::size_t shard_count);

struct ReceiveFlowSpec {
    std::string source_ip;
    std::uint16_t source_port;
};

bool ParseReceiveFlowSpec(const std::string& text, ReceiveFlowSpec* flow,
                          std::string* error);

struct DestinationUdpFilter {
    std::uint8_t source_mac[6];
    std::uint8_t source_mac_mask[6];
    std::uint8_t destination_mac[6];
    std::uint8_t destination_mac_mask[6];
    std::uint32_t source_ip;
    std::uint32_t source_ip_mask;
    std::uint32_t destination_ip;
    std::uint32_t destination_ip_mask;
    std::uint16_t source_port;
    std::uint16_t source_port_mask;
    std::uint16_t destination_port;
    std::uint16_t destination_port_mask;
};

DestinationUdpFilter BuildDestinationUdpFilter(
    const std::uint8_t destination_mac[6],
    std::uint32_t destination_ip,
    std::uint16_t destination_port);

DestinationUdpFilter BuildSourceUdpFilter(
    const std::uint8_t destination_mac[6],
    std::uint32_t source_ip, std::uint16_t source_port,
    std::uint32_t destination_ip, std::uint16_t destination_port);

struct ReceiveCompletion {
    bool success;
    bool receive_opcode;
    std::uint64_t wr_id;
    std::uint32_t byte_len;
};

enum class ReceiveDisposition {
    kAccept,
    kDropWrongLength,
    kFatal
};

ReceiveDisposition ClassifyReceiveCompletion(
    const ReceiveCompletion& completion,
    std::uint64_t wr_limit,
    std::uint32_t expected_byte_len);

bool ShouldLogWrongLengthDrop(std::uint64_t drop_count);

bool ShouldEmitPeriodicReceiveStatus(bool debug_mode);

enum class RawBlockTailDisposition {
    kNoData,
    kPublish,
    kInvalid
};

struct RawBlockTail {
    RawBlockTailDisposition disposition;
    std::uint64_t valid_bytes;
    std::uint64_t valid_records;
};

RawBlockTail ClassifyRawBlockTail(std::uint64_t block_bytes,
                                  std::uint64_t record_bytes,
                                  std::uint64_t valid_bytes);

}  // namespace rdma
}  // namespace io
}  // namespace rdma_dada
