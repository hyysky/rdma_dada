#pragma once

namespace rdma_dada {
namespace pipeline {

// Dependency-free logical representation of a CF32 sample. CUDA backends
// validate this layout before viewing the same storage as float2/cuComplex.
struct Complex32 {
    float real;
    float imag;
};

}  // namespace pipeline
}  // namespace rdma_dada
