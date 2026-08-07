#include "rdma_dada/io/rdma/receive_policy.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

int failures = 0;

void Expect(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

void TestDestinationFilterWildcardsEverySourceField() {
    const std::uint8_t destination_mac[6] = {
        0x10, 0x70, 0xfd, 0x11, 0xe2, 0xe3
    };
    const std::uint32_t destination_ip = UINT32_C(0x0b10110a);
    const std::uint16_t destination_port = UINT16_C(17201);

    const rdma_dada::io::rdma::DestinationUdpFilter filter =
        rdma_dada::io::rdma::BuildDestinationUdpFilter(
            destination_mac, destination_ip, destination_port);

    Expect(std::memcmp(filter.destination_mac, destination_mac, 6) == 0,
           "destination MAC value is preserved");
    for (std::size_t index = 0; index < 6; ++index) {
        Expect(filter.destination_mac_mask[index] == 0xff,
               "destination MAC mask matches every bit");
        Expect(filter.source_mac[index] == 0,
               "source MAC value is cleared");
        Expect(filter.source_mac_mask[index] == 0,
               "source MAC mask is wildcarded");
    }
    Expect(filter.destination_ip == destination_ip &&
               filter.destination_ip_mask == UINT32_MAX,
           "destination IPv4 is matched exactly");
    Expect(filter.source_ip == 0 && filter.source_ip_mask == 0,
           "source IPv4 is wildcarded");
    Expect(filter.destination_port == destination_port &&
               filter.destination_port_mask == UINT16_MAX,
           "destination UDP port is matched exactly");
    Expect(filter.source_port == 0 && filter.source_port_mask == 0,
           "source UDP port is wildcarded");
}

void TestWrongLengthIsRecoverable() {
    rdma_dada::io::rdma::ReceiveCompletion completion;
    completion.success = true;
    completion.receive_opcode = true;
    completion.wr_id = 7;
    completion.byte_len = 1057;

    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kDropWrongLength,
           "successful wrong-length receive is recoverable");

    completion.byte_len = 1066;
    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kAccept,
           "matching receive completion is accepted");
}

void TestTransportErrorsRemainFatal() {
    rdma_dada::io::rdma::ReceiveCompletion completion;
    completion.success = false;
    completion.receive_opcode = true;
    completion.wr_id = 7;
    completion.byte_len = 1066;
    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kFatal,
           "failed CQ status is fatal");

    completion.success = true;
    completion.receive_opcode = false;
    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kFatal,
           "unexpected CQ opcode is fatal");

    completion.receive_opcode = true;
    completion.wr_id = 32;
    Expect(rdma_dada::io::rdma::ClassifyReceiveCompletion(
               completion, 32, 1066) ==
               rdma_dada::io::rdma::ReceiveDisposition::kFatal,
           "out-of-range WR ID is fatal");
}

void TestWrongLengthLogIsRateLimited() {
    Expect(!rdma_dada::io::rdma::ShouldLogWrongLengthDrop(0),
           "zero drops do not log");
    Expect(rdma_dada::io::rdma::ShouldLogWrongLengthDrop(1),
           "first drop logs");
    Expect(rdma_dada::io::rdma::ShouldLogWrongLengthDrop(2),
           "second drop logs");
    Expect(!rdma_dada::io::rdma::ShouldLogWrongLengthDrop(3),
           "third drop is rate limited");
    Expect(rdma_dada::io::rdma::ShouldLogWrongLengthDrop(4),
           "power-of-two drop count logs");
    Expect(!rdma_dada::io::rdma::ShouldLogWrongLengthDrop(5),
           "non-power-of-two drop count is rate limited");
}

}  // namespace

int main() {
    TestDestinationFilterWildcardsEverySourceField();
    TestWrongLengthIsRecoverable();
    TestTransportErrorsRemainFatal();
    TestWrongLengthLogIsRateLimited();
    if (failures != 0) {
        std::fprintf(stderr, "%d test assertion(s) failed\n", failures);
        return 1;
    }
    return 0;
}
