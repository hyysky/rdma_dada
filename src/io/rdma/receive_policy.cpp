#include "rdma_dada/io/rdma/receive_policy.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>

namespace rdma_dada {
namespace io {
namespace rdma {

ReceiveSpscQueue::ReceiveSpscQueue(std::size_t capacity)
    : entries_(capacity + 1), storage_size_(capacity + 1), head_(0), tail_(0),
      high_watermark_(0) {}

bool ReceiveSpscQueue::TryPush(const ReceiveWorkItem& item) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t next = (head + 1) % storage_size_;
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (next == tail) return false;

    entries_[head] = item;
    head_.store(next, std::memory_order_release);

    const std::size_t occupancy = next >= tail
        ? next - tail
        : storage_size_ - tail + next;
    std::size_t observed = high_watermark_.load(std::memory_order_relaxed);
    while (observed < occupancy &&
           !high_watermark_.compare_exchange_weak(
               observed, occupancy, std::memory_order_relaxed,
               std::memory_order_relaxed)) {}
    return true;
}

bool ReceiveSpscQueue::TryPop(ReceiveWorkItem* item) {
    if (!item) return false;
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return false;

    *item = entries_[tail];
    tail_.store((tail + 1) % storage_size_, std::memory_order_release);
    return true;
}

std::size_t ReceiveSpscQueue::size() const {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return head >= tail ? head - tail : storage_size_ - tail + head;
}

bool ReceiveSpscQueue::empty() const {
    return head_.load(std::memory_order_acquire) ==
           tail_.load(std::memory_order_acquire);
}

std::size_t ReceiveSpscQueue::capacity() const {
    return storage_size_ - 1;
}

std::size_t ReceiveSpscQueue::high_watermark() const {
    return high_watermark_.load(std::memory_order_relaxed);
}

std::size_t SelectAvailableBatch(std::size_t available,
                                 std::size_t maximum_batch) {
    return available < maximum_batch ? available : maximum_batch;
}

ReceiveCpuPlacement ResolveReceiveCpuPlacement(int legacy_cpu,
                                               int poll_cpu,
                                               int copy_cpu) {
    ReceiveCpuPlacement result = {false, poll_cpu, copy_cpu};
    if (legacy_cpu < -1 || poll_cpu < -1 || copy_cpu < -1) return result;
    if (legacy_cpu >= 0) {
        if (poll_cpu >= 0 && poll_cpu != legacy_cpu) return result;
        result.poll_cpu = legacy_cpu;
    }
    if (result.poll_cpu >= 0 && result.copy_cpu == result.poll_cpu)
        return result;
    result.valid = true;
    return result;
}

ReceiveShardCpuPlacement ResolveReceiveShardCpuPlacement(
    int legacy_cpu, const std::vector<int>& poll_cpus, int copy_cpu,
    std::size_t shard_count) {
    ReceiveShardCpuPlacement result = {false, poll_cpus, copy_cpu};
    if (shard_count == 0 || legacy_cpu < -1 || copy_cpu < -1) return result;
    if (legacy_cpu >= 0) {
        if (shard_count != 1 || !poll_cpus.empty()) return result;
        result.poll_cpus.push_back(legacy_cpu);
    }
    if (!result.poll_cpus.empty() && result.poll_cpus.size() != shard_count)
        return result;
    std::set<int> assigned;
    for (std::size_t index = 0; index < result.poll_cpus.size(); ++index) {
        const int cpu = result.poll_cpus[index];
        if (cpu < 0 || cpu == copy_cpu || !assigned.insert(cpu).second)
            return result;
    }
    result.valid = true;
    return result;
}

bool ParseReceiveFlowSpec(const std::string& text, ReceiveFlowSpec* flow,
                          std::string* error) {
    if (!flow) return false;
    const std::string::size_type separator = text.rfind(':');
    if (separator == std::string::npos || separator == 0 ||
        separator + 1 >= text.size()) {
        if (error) *error = "receive flow must use IPv4:port";
        return false;
    }
    const std::string ip = text.substr(0, separator);
    const std::string port_text = text.substr(separator + 1);
    struct in_addr address = {};
    if (inet_pton(AF_INET, ip.c_str(), &address) != 1) {
        if (error) *error = "receive flow has invalid IPv4 address";
        return false;
    }
    char* end = NULL;
    const unsigned long port = std::strtoul(port_text.c_str(), &end, 10);
    if (!end || *end != '\0' || port == 0 || port > 65535) {
        if (error) *error = "receive flow has invalid UDP port";
        return false;
    }
    flow->source_ip = ip;
    flow->source_port = static_cast<std::uint16_t>(port);
    return true;
}

DestinationUdpFilter BuildDestinationUdpFilter(
    const std::uint8_t destination_mac[6],
    std::uint32_t destination_ip,
    std::uint16_t destination_port) {
    DestinationUdpFilter filter;
    std::memset(&filter, 0, sizeof(filter));
    std::memcpy(filter.destination_mac, destination_mac,
                sizeof(filter.destination_mac));
    std::memset(filter.destination_mac_mask, 0xff,
                sizeof(filter.destination_mac_mask));
    filter.destination_ip = destination_ip;
    filter.destination_ip_mask = std::numeric_limits<std::uint32_t>::max();
    filter.destination_port = destination_port;
    filter.destination_port_mask =
        std::numeric_limits<std::uint16_t>::max();
    return filter;
}

DestinationUdpFilter BuildSourceUdpFilter(
    const std::uint8_t destination_mac[6],
    std::uint32_t source_ip, std::uint16_t source_port,
    std::uint32_t destination_ip, std::uint16_t destination_port) {
    DestinationUdpFilter filter = BuildDestinationUdpFilter(
        destination_mac, destination_ip, destination_port);
    filter.source_ip = source_ip;
    filter.source_ip_mask = std::numeric_limits<std::uint32_t>::max();
    filter.source_port = source_port;
    filter.source_port_mask = std::numeric_limits<std::uint16_t>::max();
    return filter;
}

ReceiveDisposition ClassifyReceiveCompletion(
    const ReceiveCompletion& completion,
    std::uint64_t wr_limit,
    std::uint32_t expected_byte_len) {
    if (!completion.success || !completion.receive_opcode ||
        completion.wr_id >= wr_limit) {
        return ReceiveDisposition::kFatal;
    }
    if (completion.byte_len != expected_byte_len) {
        return ReceiveDisposition::kDropWrongLength;
    }
    return ReceiveDisposition::kAccept;
}

bool ShouldLogWrongLengthDrop(std::uint64_t drop_count) {
    return drop_count != 0 && (drop_count & (drop_count - 1)) == 0;
}

bool ShouldEmitPeriodicReceiveStatus(bool debug_mode) {
    return debug_mode;
}

RawBlockTail ClassifyRawBlockTail(std::uint64_t block_bytes,
                                  std::uint64_t record_bytes,
                                  std::uint64_t valid_bytes) {
    RawBlockTail result;
    result.disposition = RawBlockTailDisposition::kInvalid;
    result.valid_bytes = valid_bytes;
    result.valid_records = 0;
    if (block_bytes == 0U || record_bytes == 0U ||
        block_bytes % record_bytes != 0U || valid_bytes > block_bytes ||
        valid_bytes % record_bytes != 0U) {
        return result;
    }
    result.valid_records = valid_bytes / record_bytes;
    result.disposition = valid_bytes == 0U
        ? RawBlockTailDisposition::kNoData
        : RawBlockTailDisposition::kPublish;
    return result;
}

}  // namespace rdma
}  // namespace io
}  // namespace rdma_dada
