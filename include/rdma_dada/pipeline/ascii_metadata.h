#pragma once

#include "rdma_dada/pipeline/metadata.h"

#include <cstdint>
#include <string>

namespace rdma_dada {
namespace pipeline {

// Parse a NUL-padded PSRDADA-style ASCII header without depending on PSRDADA.
// Values may contain spaces; comments beginning with '#' are not retained.
bool ParseAsciiMetadata(const char* header, std::uint64_t capacity,
                        Metadata* metadata, std::string* error);

// Serialize all metadata fields into a NUL-padded header block. HDR_SIZE is
// set to capacity. Unknown fields parsed from the input metadata are retained.
bool SerializeAsciiMetadata(const Metadata& metadata, char* header,
                            std::uint64_t capacity, std::string* error);

}  // namespace pipeline
}  // namespace rdma_dada
