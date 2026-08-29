#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <deque>
#include <vector>

namespace rdma_dada {
namespace io {
namespace rdma {

constexpr unsigned int kDefaultReceivePollBatch = 32;
constexpr unsigned int kDefaultReceiveWrDepth = 1024;
constexpr unsigned int kDirectRawNsge = 2;
constexpr std::uint32_t kDirectRawHeaderBytes = 42;
constexpr std::uint32_t kDirectRawMaxConsecutiveWrongLength = 16;
constexpr std::uint64_t kDirectRawDrainDurationNs = UINT64_C(1000000000);
constexpr std::uint64_t kDirectRawDrainClockCheckEmptyPolls = 4096;

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

bool ValidateDirectRawConfiguration(std::size_t recv_wr_num,
                                    std::size_t records_per_block,
                                    std::size_t raw_ring_blocks);

enum class DirectRawCompletionAction {
    kKeepSlot,
    kZeroSlot,
    kFatal
};

DirectRawCompletionAction ClassifyDirectRawCompletion(
    bool success, bool receive_opcode, bool valid_wr_id,
    std::uint32_t byte_len, std::uint32_t expected_byte_len,
    std::uint32_t* consecutive_wrong_length);

bool ShouldCheckDirectRawDrainClock(std::uint64_t empty_polls_since_check);
bool ShouldRepostDirectRawWr(bool stop_requested,
                             bool drain_deadline_reached);
bool DirectRawDrainDeadlineReached(std::uint64_t now_ns,
                                   std::uint64_t deadline_ns);

enum class StagedRawCopyAction {
    kCopied,
    kZeroed,
    kFatal
};

StagedRawCopyAction CopyStagedRawRecord(
    const unsigned char* frame, std::size_t frame_bytes,
    std::size_t record_bytes, unsigned char* destination,
    std::uint32_t* consecutive_wrong_length);

class ReceiveLatencyHistogram {
  public:
    ReceiveLatencyHistogram();
    void Observe(std::uint64_t nanoseconds);
    std::uint64_t count() const;
    std::uint64_t Percentile(unsigned int percentile) const;

  private:
    std::array<std::uint64_t, 64> buckets_;
    std::uint64_t count_;
};

class DirectRawBlockProgress {
  public:
    explicit DirectRawBlockProgress(std::size_t slot_count);

    bool AssignSlot(std::size_t slot);
    bool CompleteSlot(std::size_t slot);
    std::size_t ContiguousCompletedSlots() const;
    bool ready_to_publish() const;

  private:
    std::vector<std::uint8_t> assigned_;
    std::vector<std::uint8_t> completed_;
    std::size_t assigned_count_;
    std::size_t completed_count_;
};

class DirectRawOutstandingBlockOrder {
  public:
    bool Push(std::uint64_t token);
    bool PopFront(std::uint64_t token);
    std::uint64_t front() const;
    std::size_t size() const;
    bool empty() const;

  private:
    std::deque<std::uint64_t> tokens_;
};

}  // namespace rdma
}  // namespace io
}  // namespace rdma_dada
