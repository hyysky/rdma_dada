#include "rdma_dada/config/beamform_weight_metadata.h"

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace rdma_dada {
namespace {

const std::uint64_t kMaxNpyHeaderBytes = 1024U * 1024U;

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

bool CheckedMultiply(std::uint64_t left, std::uint64_t right,
                     std::uint64_t* output) {
    if (!output || (left != 0U &&
        right > std::numeric_limits<std::uint64_t>::max() / left)) {
        return false;
    }
    *output = left * right;
    return true;
}

bool FindDictionaryValue(const std::string& header, const std::string& key,
                         std::size_t* value_position) {
    const std::string single = "'" + key + "'";
    const std::string dual = "\"" + key + "\"";
    std::size_t position = header.find(single);
    if (position == std::string::npos) position = header.find(dual);
    if (position == std::string::npos) return false;
    position = header.find(':', position + key.size() + 2U);
    if (position == std::string::npos) return false;
    do {
        ++position;
    } while (position < header.size() &&
             (header[position] == ' ' || header[position] == '\t'));
    if (position == header.size()) return false;
    *value_position = position;
    return true;
}

bool ParseQuotedValue(const std::string& header, const std::string& key,
                      std::string* value) {
    std::size_t position = 0;
    if (!FindDictionaryValue(header, key, &position)) return false;
    const char quote = header[position];
    if (quote != '\'' && quote != '"') return false;
    const std::size_t end = header.find(quote, position + 1U);
    if (end == std::string::npos) return false;
    *value = header.substr(position + 1U, end - position - 1U);
    return true;
}

bool ParseFortranOrder(const std::string& header, bool* fortran_order) {
    std::size_t position = 0;
    if (!FindDictionaryValue(header, "fortran_order", &position)) return false;
    if (header.compare(position, 4U, "True") == 0) {
        *fortran_order = true;
        return true;
    }
    if (header.compare(position, 5U, "False") == 0) {
        *fortran_order = false;
        return true;
    }
    return false;
}

bool ParseShape(const std::string& header,
                std::vector<std::uint64_t>* shape) {
    std::size_t position = 0;
    if (!FindDictionaryValue(header, "shape", &position) ||
        header[position] != '(') {
        return false;
    }
    const std::size_t end = header.find(')', position + 1U);
    if (end == std::string::npos) return false;
    shape->clear();
    ++position;
    while (position < end) {
        while (position < end &&
               (header[position] == ' ' || header[position] == '\t' ||
                header[position] == ',')) {
            ++position;
        }
        if (position == end) break;
        if (header[position] < '0' || header[position] > '9') return false;
        errno = 0;
        char* number_end = NULL;
        const unsigned long long value =
            std::strtoull(header.c_str() + position, &number_end, 10);
        if (errno == ERANGE || number_end == header.c_str() + position ||
            value == 0U) {
            return false;
        }
        position = static_cast<std::size_t>(number_end - header.c_str());
        if (position > end) return false;
        shape->push_back(static_cast<std::uint64_t>(value));
        while (position < end &&
               (header[position] == ' ' || header[position] == '\t')) {
            ++position;
        }
        if (position < end && header[position] != ',') return false;
    }
    return !shape->empty();
}

}  // namespace

bool ReadBeamformWeightMetadata(const std::string& path,
                                BeamformWeightMetadata* metadata,
                                std::string* error) {
    if (!metadata) return Fail("weight metadata output pointer is null", error);
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input) return Fail("cannot open beamform weight file: " + path, error);

    unsigned char prefix[8] = {};
    input.read(reinterpret_cast<char*>(prefix), sizeof(prefix));
    static const unsigned char magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y'};
    for (std::size_t index = 0; index < sizeof(magic); ++index) {
        if (!input || prefix[index] != magic[index]) {
            return Fail("beamform weight file is not NumPy NPY", error);
        }
    }
    const unsigned int major = prefix[6];
    const std::size_t length_bytes = major == 1U ? 2U : major == 2U ? 4U : 0U;
    if (length_bytes == 0U) {
        return Fail("unsupported NPY version; expected version 1 or 2", error);
    }
    unsigned char encoded_length[4] = {};
    input.read(reinterpret_cast<char*>(encoded_length),
               static_cast<std::streamsize>(length_bytes));
    if (!input) return Fail("truncated NPY header length", error);
    std::uint64_t header_bytes = 0U;
    for (std::size_t index = 0; index < length_bytes; ++index) {
        header_bytes |= static_cast<std::uint64_t>(encoded_length[index])
            << (8U * index);
    }
    if (header_bytes == 0U || header_bytes > kMaxNpyHeaderBytes ||
        header_bytes > static_cast<std::uint64_t>(
                           std::numeric_limits<std::streamsize>::max())) {
        return Fail("invalid or excessive NPY header length", error);
    }
    std::string header(static_cast<std::size_t>(header_bytes), '\0');
    input.read(&header[0], static_cast<std::streamsize>(header.size()));
    if (!input) return Fail("truncated NPY dictionary header", error);

    BeamformWeightMetadata parsed = BeamformWeightMetadata();
    bool fortran_order = false;
    std::vector<std::uint64_t> shape;
    if (!ParseQuotedValue(header, "descr", &parsed.dtype) ||
        !ParseFortranOrder(header, &fortran_order) ||
        !ParseShape(header, &shape)) {
        return Fail("malformed NPY dictionary header", error);
    }
    if (fortran_order) {
        return Fail("beamform weights must be C-contiguous", error);
    }
    if (shape.size() != 5U || shape[4] != 2U) {
        return Fail("beamform weight shape must be [F,P,A,B,2]", error);
    }
    if (parsed.dtype == "|i1") {
        parsed.component_bytes = 1U;
    } else if (parsed.dtype == "<i2") {
        parsed.component_bytes = 2U;
    } else {
        return Fail("beamform weight dtype must be |i1 or <i2", error);
    }
    parsed.nchan = shape[0];
    parsed.npol = shape[1];
    parsed.nant = shape[2];
    parsed.nbeam = shape[3];
    std::uint64_t components = 1U;
    for (std::size_t index = 0; index < shape.size(); ++index) {
        if (!CheckedMultiply(components, shape[index], &components)) {
            return Fail("beamform weight shape exceeds uint64 range", error);
        }
    }
    if (!CheckedMultiply(components, parsed.component_bytes,
                         &parsed.payload_bytes)) {
        return Fail("beamform weight payload exceeds uint64 range", error);
    }
    if (parsed.payload_bytes > static_cast<std::uint64_t>(
                                   std::numeric_limits<std::streamoff>::max())) {
        return Fail("beamform weight payload is too large", error);
    }
    input.seekg(0, std::ios::end);
    if (!input) return Fail("cannot size beamform weight file", error);
    const std::streamoff actual_end = input.tellg();
    const std::uint64_t expected_end = 8U + length_bytes + header_bytes +
                                       parsed.payload_bytes;
    if (actual_end < 0 || static_cast<std::uint64_t>(actual_end) != expected_end) {
        return Fail("beamform weight payload size does not match NPY shape",
                    error);
    }
    *metadata = parsed;
    return true;
}

}  // namespace rdma_dada
