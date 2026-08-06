#include "rdma_dada/modules/vdif_unpack/project_vdif_v1.h"

#include <limits>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {
namespace {

const std::uint64_t kHeaderBytes = 32U;
const std::uint32_t kMaxFrameLengthUnits = 0x00ffffffU;

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

std::uint32_t LoadLittleEndianWord(const std::uint8_t* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void StoreLittleEndianWord(std::uint32_t word, std::uint8_t* bytes) {
    bytes[0] = static_cast<std::uint8_t>(word & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((word >> 8U) & 0xffU);
    bytes[2] = static_cast<std::uint8_t>((word >> 16U) & 0xffU);
    bytes[3] = static_cast<std::uint8_t>((word >> 24U) & 0xffU);
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* result) {
    if (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        return false;
    }
    *result = left * right;
    return true;
}

bool ValidComponentBits(std::uint8_t component_bits) {
    return component_bits == 8U || component_bits == 16U;
}

bool ValidateEncodableHeader(const ProjectVdifHeader& header,
                             std::string* error) {
    if (header.seconds_from_reference_epoch > 0x3fffffffU) {
        return Fail("seconds_from_reference_epoch exceeds 30 bits", error);
    }
    if (header.reference_epoch > 0x3fU) {
        return Fail("reference_epoch exceeds 6 bits", error);
    }
    if (header.frame_number_within_second > 0x00ffffffU) {
        return Fail("frame_number_within_second exceeds 24 bits", error);
    }
    if (!ValidComponentBits(header.component_bits)) {
        return Fail("component_bits must be 8 or 16", error);
    }
    if (header.frame_length_units_8_bytes == 0U ||
        header.frame_length_units_8_bytes > kMaxFrameLengthUnits) {
        return Fail("frame length must fit the 24-bit eight-byte field", error);
    }
    if (header.nchan == 0U) return Fail("nchan must be positive", error);
    if (header.npol != 1U && header.npol != 2U) {
        return Fail("npol must be one or two", error);
    }
    if (header.nsamp_per_packet == 0U) {
        return Fail("nsamp_per_packet must be positive", error);
    }
    return true;
}

}  // namespace

bool DecodeProjectVdifV1(const std::uint8_t* bytes, std::uint64_t size,
                         ProjectVdifHeader* header, std::string* error) {
    if (!bytes) return Fail("header input pointer is null", error);
    if (!header) return Fail("header output pointer is null", error);
    if (size < kHeaderBytes) {
        return Fail("Project VDIF v1 header requires 32 bytes", error);
    }

    std::uint32_t words[8];
    for (std::uint32_t index = 0; index < 8U; ++index) {
        words[index] = LoadLittleEndianWord(bytes + index * 4U);
    }

    if ((words[0] & 0x40000000U) != 0U) {
        return Fail("legacy_mode must be zero", error);
    }
    if ((words[1] & 0xc0000000U) != 0U) {
        return Fail("Word 1 reserved bits must be zero", error);
    }
    if (((words[2] >> 29U) & 0x7U) != 0U) {
        return Fail("vdif_version must be zero", error);
    }
    if (((words[2] >> 24U) & 0x1fU) != 31U) {
        return Fail("channel_count_code must be the project sentinel 31", error);
    }
    if (((words[3] >> 31U) & 0x1U) != 1U) {
        return Fail("data_type must identify complex samples", error);
    }
    if (((words[3] >> 16U) & 0x3ffU) != 0U) {
        return Fail("thread_id must be zero", error);
    }
    if (((words[4] >> 24U) & 0xffU) != 0xffU) {
        return Fail("EDV must be the project value 0xff", error);
    }
    if (((words[4] >> 16U) & 0xffU) != 1U) {
        return Fail("profile_version must be one", error);
    }
    if (((words[4] >> 8U) & 0xffU) != 1U) {
        return Fail("sample_encoding must be TWOS_COMPLEMENT", error);
    }
    if ((words[4] & 0xffU) != 0U) {
        return Fail("Project VDIF flags must be zero", error);
    }
    if (words[7] != 0U) return Fail("Word 7 must be zero", error);

    ProjectVdifHeader decoded = {};
    decoded.invalid_data = (words[0] & 0x80000000U) != 0U;
    decoded.seconds_from_reference_epoch = words[0] & 0x3fffffffU;
    decoded.reference_epoch =
        static_cast<std::uint8_t>((words[1] >> 24U) & 0x3fU);
    decoded.frame_number_within_second = words[1] & 0x00ffffffU;
    decoded.frame_length_units_8_bytes = words[2] & 0x00ffffffU;
    decoded.component_bits = static_cast<std::uint8_t>(
        ((words[3] >> 26U) & 0x1fU) + 1U);
    decoded.station_id = static_cast<std::uint16_t>(words[3] & 0xffffU);
    decoded.first_channel_id =
        static_cast<std::uint16_t>((words[5] >> 16U) & 0xffffU);
    decoded.nchan = static_cast<std::uint8_t>((words[5] >> 8U) & 0xffU);
    decoded.npol = static_cast<std::uint8_t>(words[5] & 0xffU);
    decoded.nsamp_per_packet = words[6];

    if (!ValidateEncodableHeader(decoded, error)) return false;
    *header = decoded;
    return true;
}

bool EncodeProjectVdifV1(const ProjectVdifHeader& header,
                         std::uint8_t* bytes, std::uint64_t size,
                         std::string* error) {
    if (!bytes) return Fail("header output pointer is null", error);
    if (size < kHeaderBytes) {
        return Fail("Project VDIF v1 header destination requires 32 bytes",
                    error);
    }
    if (!ValidateEncodableHeader(header, error)) return false;

    std::uint32_t words[8] = {};
    words[0] = (header.invalid_data ? 0x80000000U : 0U) |
               header.seconds_from_reference_epoch;
    words[1] = (static_cast<std::uint32_t>(header.reference_epoch) << 24U) |
               header.frame_number_within_second;
    words[2] = (31U << 24U) | header.frame_length_units_8_bytes;
    words[3] = 0x80000000U |
               ((static_cast<std::uint32_t>(header.component_bits) - 1U)
                << 26U) |
               static_cast<std::uint32_t>(header.station_id);
    words[4] = 0xff010100U;
    words[5] =
        (static_cast<std::uint32_t>(header.first_channel_id) << 16U) |
        (static_cast<std::uint32_t>(header.nchan) << 8U) |
        static_cast<std::uint32_t>(header.npol);
    words[6] = header.nsamp_per_packet;

    for (std::uint32_t index = 0; index < 8U; ++index) {
        StoreLittleEndianWord(words[index], bytes + index * 4U);
    }
    return true;
}

bool ValidateProjectVdifV1(const ProjectVdifHeader& header,
                           const ProjectVdifGeometry& geometry,
                           std::uint64_t actual_record_bytes,
                           std::string* error) {
    if (!ValidateEncodableHeader(header, error)) return false;
    if (header.first_channel_id != geometry.first_channel_id) {
        return Fail("first_channel_id does not match observation geometry",
                    error);
    }
    if (header.nchan != geometry.nchan) {
        return Fail("nchan does not match observation geometry", error);
    }
    if (header.npol != geometry.npol) {
        return Fail("npol does not match observation geometry", error);
    }
    if (header.nsamp_per_packet != geometry.nsamp_per_packet) {
        return Fail("nsamp_per_packet does not match observation geometry",
                    error);
    }
    if (header.component_bits != geometry.component_bits) {
        return Fail("component_bits does not match observation geometry",
                    error);
    }
    if (!ValidComponentBits(geometry.component_bits)) {
        return Fail("geometry component_bits must be 8 or 16", error);
    }
    if (geometry.nchan == 0U ||
        (geometry.npol != 1U && geometry.npol != 2U) ||
        geometry.nsamp_per_packet == 0U) {
        return Fail("observation geometry has an invalid extent", error);
    }

    std::uint64_t payload_bytes = geometry.nsamp_per_packet;
    if (!CheckedMultiply(payload_bytes, geometry.nchan, &payload_bytes) ||
        !CheckedMultiply(payload_bytes, geometry.npol, &payload_bytes) ||
        !CheckedMultiply(payload_bytes,
                         2U * (geometry.component_bits / 8U),
                         &payload_bytes)) {
        return Fail("payload byte geometry exceeds uint64 range", error);
    }
    if (payload_bytes != geometry.payload_bytes) {
        return Fail("configured payload bytes do not match TFP geometry",
                    error);
    }
    if (payload_bytes > std::numeric_limits<std::uint64_t>::max() -
                            kHeaderBytes) {
        return Fail("record byte geometry exceeds uint64 range", error);
    }
    const std::uint64_t record_bytes = kHeaderBytes + payload_bytes;
    if (actual_record_bytes != record_bytes) {
        return Fail("actual record bytes do not match header plus payload",
                    error);
    }
    if (record_bytes % 8U != 0U) {
        return Fail("Project VDIF record bytes must be divisible by eight",
                    error);
    }
    const std::uint64_t frame_units = record_bytes / 8U;
    if (frame_units > kMaxFrameLengthUnits) {
        return Fail("record length exceeds the 24-bit VDIF frame field",
                    error);
    }
    if (header.frame_length_units_8_bytes != frame_units) {
        return Fail("header frame length does not match record geometry",
                    error);
    }
    return true;
}

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
