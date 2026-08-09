#pragma once

#include <cstdint>

namespace rdma_dada {
namespace io {
namespace rdma {

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
