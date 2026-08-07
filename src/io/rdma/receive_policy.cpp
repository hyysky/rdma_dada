#include "rdma_dada/io/rdma/receive_policy.h"

#include <cstring>
#include <limits>

namespace rdma_dada {
namespace io {
namespace rdma {

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

}  // namespace rdma
}  // namespace io
}  // namespace rdma_dada
