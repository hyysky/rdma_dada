#include "rdma_dada/pipeline/ascii_metadata.h"

#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <sstream>

namespace rdma_dada {
namespace pipeline {
namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

std::string Trim(const std::string& value) {
    std::size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

bool ValidKey(const std::string& key) {
    if (key.empty()) return false;
    for (std::size_t i = 0; i < key.size(); ++i) {
        const unsigned char value = static_cast<unsigned char>(key[i]);
        if (std::isspace(value) || value == '#' || value == '\0') return false;
    }
    return true;
}

bool ValidValue(const std::string& value) {
    return value.find('\n') == std::string::npos &&
           value.find('\r') == std::string::npos &&
           value.find('\0') == std::string::npos;
}

}  // namespace

bool ParseAsciiMetadata(const char* header, std::uint64_t capacity,
                        Metadata* metadata, std::string* error) {
    if (!header) return Fail("null ASCII header", error);
    if (!metadata) return Fail("null metadata output", error);
    if (capacity == 0 ||
        capacity > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max())) {
        return Fail("invalid ASCII header capacity", error);
    }

    const std::size_t size = static_cast<std::size_t>(capacity);
    const void* terminator = std::memchr(header, '\0', size);
    const std::size_t text_size = terminator ?
        static_cast<std::size_t>(
            static_cast<const char*>(terminator) - header) : size;
    std::istringstream lines(std::string(header, text_size));
    Metadata parsed;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        if (!line.empty() && line[line.size() - 1] == '\r') line.resize(line.size() - 1);
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        line = Trim(line);
        if (line.empty() || line == "DATA") continue;

        std::size_t split = 0;
        while (split < line.size() &&
               !std::isspace(static_cast<unsigned char>(line[split]))) {
            ++split;
        }
        const std::string key = line.substr(0, split);
        const std::string value = Trim(line.substr(split));
        if (!ValidKey(key) || value.empty()) {
            std::ostringstream message;
            message << "invalid ASCII header field at line " << line_number;
            return Fail(message.str(), error);
        }
        if (parsed.Has(key)) {
            return Fail("duplicate ASCII header field: " + key, error);
        }
        parsed.SetString(key, value);
    }
    *metadata = parsed;
    return true;
}

bool SerializeAsciiMetadata(const Metadata& metadata, char* header,
                            std::uint64_t capacity, std::string* error) {
    if (!header) return Fail("null ASCII header output", error);
    if (capacity == 0 ||
        capacity > static_cast<std::uint64_t>(
                       std::numeric_limits<std::size_t>::max())) {
        return Fail("invalid ASCII header output capacity", error);
    }

    Metadata serialized = metadata;
    serialized.SetUint64("HDR_SIZE", capacity);
    std::ostringstream text;
    const std::map<std::string, std::string>& fields = serialized.Fields();
    for (std::map<std::string, std::string>::const_iterator field =
             fields.begin(); field != fields.end(); ++field) {
        if (!ValidKey(field->first) || !ValidValue(field->second) ||
            field->second.empty()) {
            return Fail("metadata contains an invalid field: " + field->first,
                        error);
        }
        text << field->first << ' ' << field->second << '\n';
    }
    const std::string output = text.str();
    if (output.size() >= capacity) {
        return Fail("serialized metadata exceeds output header block", error);
    }
    std::memset(header, 0, static_cast<std::size_t>(capacity));
    std::memcpy(header, output.data(), output.size());
    return true;
}

}  // namespace pipeline
}  // namespace rdma_dada
