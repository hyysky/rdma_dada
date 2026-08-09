#pragma once

#include <cstddef>
#include <string>

namespace rdma_dada {

std::string Sha256Hex(const void* data, std::size_t size);

}  // namespace rdma_dada
