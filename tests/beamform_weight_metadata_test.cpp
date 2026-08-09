#include "rdma_dada/config/beamform_weight_metadata.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <unistd.h>

namespace {

int failures = 0;

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::string Path(const char* suffix) {
    std::ostringstream path;
    path << "/tmp/rdma_dada_weight_metadata_" << getpid() << '_' << suffix
         << ".npy";
    return path.str();
}

bool WriteNpy(const std::string& path, const std::string& descr,
              const std::string& shape, std::size_t payload_bytes,
              bool fortran = false, bool trailing = false) {
    std::ostringstream dictionary;
    dictionary << "{'descr': '" << descr << "', 'fortran_order': "
               << (fortran ? "True" : "False") << ", 'shape': " << shape
               << ", }";
    std::string header = dictionary.str();
    const std::size_t prefix_bytes = 10U;
    const std::size_t padding = 16U - ((prefix_bytes + header.size() + 1U) % 16U);
    header.append(padding, ' ');
    header.push_back('\n');
    if (header.size() > 65535U) return false;

    std::ofstream output(path.c_str(), std::ios::binary);
    const unsigned char prefix[] = {
        0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0,
        static_cast<unsigned char>(header.size() & 0xffU),
        static_cast<unsigned char>((header.size() >> 8U) & 0xffU)
    };
    output.write(reinterpret_cast<const char*>(prefix), sizeof(prefix));
    output.write(header.data(), static_cast<std::streamsize>(header.size()));
    std::string payload(payload_bytes, '\0');
    output.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    if (trailing) output.put('x');
    return static_cast<bool>(output);
}

}  // namespace

int main() {
    const std::string valid = Path("valid");
    Expect(WriteNpy(valid, "|i1", "(2, 2, 3, 4, 2)", 96U),
           "write valid fixture");
    rdma_dada::BeamformWeightMetadata metadata =
        rdma_dada::BeamformWeightMetadata();
    std::string error;
    Expect(rdma_dada::ReadBeamformWeightMetadata(valid, &metadata, &error),
           "valid metadata: " + error);
    Expect(metadata.dtype == "|i1" && metadata.nchan == 2U &&
               metadata.npol == 2U && metadata.nant == 3U &&
               metadata.nbeam == 4U && metadata.component_bytes == 1U &&
               metadata.payload_bytes == 96U,
           "valid [F,P,A,B,2] geometry");

    const std::string int16 = Path("int16");
    Expect(WriteNpy(int16, "<i2", "(1, 1, 2, 5, 2)", 40U),
           "write int16 fixture");
    error.clear();
    Expect(rdma_dada::ReadBeamformWeightMetadata(int16, &metadata, &error) &&
               metadata.nbeam == 5U && metadata.component_bytes == 2U &&
               metadata.payload_bytes == 40U,
           "int16 metadata");

    const std::string wrong_shape = Path("shape");
    WriteNpy(wrong_shape, "|i1", "(2, 2, 3, 4)", 48U);
    error.clear();
    Expect(!rdma_dada::ReadBeamformWeightMetadata(wrong_shape, &metadata,
                                                  &error),
           "reject non-FPAB2 shape");

    const std::string fortran = Path("fortran");
    WriteNpy(fortran, "|i1", "(2, 2, 3, 4, 2)", 96U, true);
    error.clear();
    Expect(!rdma_dada::ReadBeamformWeightMetadata(fortran, &metadata, &error),
           "reject Fortran order");

    const std::string truncated = Path("truncated");
    WriteNpy(truncated, "|i1", "(2, 2, 3, 4, 2)", 95U);
    error.clear();
    Expect(!rdma_dada::ReadBeamformWeightMetadata(truncated, &metadata,
                                                  &error),
           "reject truncated payload");

    const std::string trailing = Path("trailing");
    WriteNpy(trailing, "|i1", "(2, 2, 3, 4, 2)", 96U, false, true);
    error.clear();
    Expect(!rdma_dada::ReadBeamformWeightMetadata(trailing, &metadata, &error),
           "reject trailing payload bytes");

    std::remove(valid.c_str());
    std::remove(int16.c_str());
    std::remove(wrong_shape.c_str());
    std::remove(fortran.c_str());
    std::remove(truncated.c_str());
    std::remove(trailing.c_str());
    if (failures != 0) return 1;
    std::cout << "beamform_weight_metadata_test passed\n";
    return 0;
}
