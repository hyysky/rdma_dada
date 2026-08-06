#pragma once

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace modules {
namespace vdif_unpack {

struct ProjectVdifHeader {
    bool invalid_data;
    std::uint32_t seconds_from_reference_epoch;
    std::uint8_t reference_epoch;
    std::uint32_t frame_number_within_second;
    std::uint16_t station_id;
    std::uint16_t first_channel_id;
    std::uint8_t nchan;
    std::uint8_t npol;
    std::uint32_t nsamp_per_packet;
    std::uint8_t component_bits;
    std::uint32_t frame_length_units_8_bytes;
};

struct ProjectVdifGeometry {
    std::uint16_t first_channel_id;
    std::uint8_t nchan;
    std::uint8_t npol;
    std::uint32_t nsamp_per_packet;
    std::uint8_t component_bits;
    std::uint64_t payload_bytes;
};

bool DecodeProjectVdifV1(const std::uint8_t* bytes, std::uint64_t size,
                         ProjectVdifHeader* header, std::string* error);

bool EncodeProjectVdifV1(const ProjectVdifHeader& header,
                         std::uint8_t* bytes, std::uint64_t size,
                         std::string* error);

bool ValidateProjectVdifV1(const ProjectVdifHeader& header,
                           const ProjectVdifGeometry& geometry,
                           std::uint64_t actual_record_bytes,
                           std::string* error);

}  // namespace vdif_unpack
}  // namespace modules
}  // namespace rdma_dada
