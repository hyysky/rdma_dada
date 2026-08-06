#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::vector<std::uint8_t> GoldenCi8Header() {
    const std::uint8_t bytes[] = {
        0x39, 0x30, 0x00, 0x00,
        0x34, 0x12, 0x00, 0x11,
        0x07, 0x00, 0x00, 0x1f,
        0x34, 0x12, 0x00, 0x9c,
        0x00, 0x01, 0x01, 0xff,
        0x02, 0x03, 0x2c, 0x01,
        0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };
    return std::vector<std::uint8_t>(bytes, bytes + sizeof(bytes));
}

void TestGoldenDecodeAndEncode() {
    const std::vector<std::uint8_t> golden = GoldenCi8Header();
    rdma_dada::modules::vdif_unpack::ProjectVdifHeader header = {};
    std::string error;
    Expect(rdma_dada::modules::vdif_unpack::DecodeProjectVdifV1(
               &golden[0], golden.size(), &header, &error),
           "golden header decodes: " + error);
    Expect(!header.invalid_data, "golden packet is valid");
    Expect(header.seconds_from_reference_epoch == 12345U,
           "seconds decode from Word 0");
    Expect(header.reference_epoch == 17U, "reference epoch decode");
    Expect(header.frame_number_within_second == 0x1234U,
           "frame number decode");
    Expect(header.station_id == 0x1234U, "numeric Station ID decode");
    Expect(header.first_channel_id == 300U, "first channel decode");
    Expect(header.nchan == 3U, "arbitrary non-power-of-two NCHAN decode");
    Expect(header.npol == 2U, "NPOL decode");
    Expect(header.nsamp_per_packet == 2U, "NSAMP decode");
    Expect(header.component_bits == 8U, "CI8 component width decode");
    Expect(header.frame_length_units_8_bytes == 7U,
           "56-byte record length decode");

    std::vector<std::uint8_t> encoded(32U, 0U);
    error.clear();
    Expect(rdma_dada::modules::vdif_unpack::EncodeProjectVdifV1(
               header, &encoded[0], encoded.size(), &error),
           "decoded header encodes: " + error);
    Expect(encoded == golden, "encode exactly reproduces golden bytes");
}

void TestGeometryValidation() {
    const std::vector<std::uint8_t> golden = GoldenCi8Header();
    rdma_dada::modules::vdif_unpack::ProjectVdifHeader header = {};
    std::string error;
    Expect(rdma_dada::modules::vdif_unpack::DecodeProjectVdifV1(
               &golden[0], golden.size(), &header, &error),
           "geometry fixture decodes");

    rdma_dada::modules::vdif_unpack::ProjectVdifGeometry geometry = {};
    geometry.first_channel_id = 300U;
    geometry.nchan = 3U;
    geometry.npol = 2U;
    geometry.nsamp_per_packet = 2U;
    geometry.component_bits = 8U;
    geometry.payload_bytes = 24U;
    error.clear();
    Expect(rdma_dada::modules::vdif_unpack::ValidateProjectVdifV1(
               header, geometry, 56U, &error),
           "CI8 geometry validates: " + error);

    rdma_dada::modules::vdif_unpack::ProjectVdifHeader ci16 = header;
    ci16.component_bits = 16U;
    ci16.frame_length_units_8_bytes = 10U;
    geometry.component_bits = 16U;
    geometry.payload_bytes = 48U;
    error.clear();
    Expect(rdma_dada::modules::vdif_unpack::ValidateProjectVdifV1(
               ci16, geometry, 80U, &error),
           "CI16 geometry validates: " + error);

    geometry.payload_bytes = 47U;
    error.clear();
    Expect(!rdma_dada::modules::vdif_unpack::ValidateProjectVdifV1(
               ci16, geometry, 79U, &error),
           "payload formula mismatch is rejected");

    geometry.payload_bytes = 48U;
    geometry.first_channel_id = 301U;
    error.clear();
    Expect(!rdma_dada::modules::vdif_unpack::ValidateProjectVdifV1(
               ci16, geometry, 80U, &error),
           "observation frequency mismatch is rejected");

    geometry.first_channel_id = 300U;
    ci16.nsamp_per_packet = UINT32_MAX;
    ci16.nchan = 255U;
    ci16.npol = 2U;
    ci16.frame_length_units_8_bytes = 0xffffffU;
    geometry.nsamp_per_packet = UINT32_MAX;
    geometry.nchan = 255U;
    geometry.npol = 2U;
    geometry.payload_bytes = UINT64_C(8761733281800);
    error.clear();
    Expect(!rdma_dada::modules::vdif_unpack::ValidateProjectVdifV1(
               ci16, geometry, UINT64_C(8761733281832), &error),
           "record larger than the 24-bit VDIF frame field is rejected");
}

void TestMalformedHeaders() {
    const std::vector<std::uint8_t> golden = GoldenCi8Header();
    rdma_dada::modules::vdif_unpack::ProjectVdifHeader header = {};
    std::string error;

    Expect(!rdma_dada::modules::vdif_unpack::DecodeProjectVdifV1(
               &golden[0], 31U, &header, &error),
           "short header is rejected");

    std::vector<std::uint8_t> malformed = golden;
    malformed[28] = 1U;
    error.clear();
    Expect(!rdma_dada::modules::vdif_unpack::DecodeProjectVdifV1(
               &malformed[0], malformed.size(), &header, &error),
           "nonzero Word 7 is rejected");

    malformed = golden;
    malformed[11] = 0x1eU;
    error.clear();
    Expect(!rdma_dada::modules::vdif_unpack::DecodeProjectVdifV1(
               &malformed[0], malformed.size(), &header, &error),
           "wrong project channel sentinel is rejected");

    malformed = golden;
    malformed[19] = 0xfeU;
    error.clear();
    Expect(!rdma_dada::modules::vdif_unpack::DecodeProjectVdifV1(
               &malformed[0], malformed.size(), &header, &error),
           "wrong project EDV is rejected");

    error.clear();
    Expect(!rdma_dada::modules::vdif_unpack::EncodeProjectVdifV1(
               header, &malformed[0], 31U, &error),
           "short encode destination is rejected");
}

}  // namespace

int main() {
    TestGoldenDecodeAndEncode();
    TestGeometryValidation();
    TestMalformedHeaders();
    if (failures != 0) return 1;
    std::cout << "project_vdif_v1_test passed\n";
    return 0;
}
