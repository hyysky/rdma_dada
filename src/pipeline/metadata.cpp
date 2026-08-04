#include "rdma_dada/pipeline/metadata.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>

namespace rdma_dada {
namespace pipeline {

bool Metadata::Has(const std::string& key) const {
    return fields_.count(key) != 0;
}

bool Metadata::GetString(const std::string& key, std::string* value) const {
    if (!value) return false;
    const std::map<std::string, std::string>::const_iterator it = fields_.find(key);
    if (it == fields_.end()) return false;
    *value = it->second;
    return true;
}

bool Metadata::GetUint64(const std::string& key, std::uint64_t* value) const {
    if (!value) return false;
    std::string text;
    if (!GetString(key, &text) || text.empty() || text[0] == '-') return false;
    errno = 0;
    char* end = NULL;
    const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
    if (errno == ERANGE || end == text.c_str() || *end != '\0') return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

bool Metadata::GetDouble(const std::string& key, double* value) const {
    if (!value) return false;
    std::string text;
    if (!GetString(key, &text)) return false;
    errno = 0;
    char* end = NULL;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno == ERANGE || end == text.c_str() || *end != '\0' ||
        !std::isfinite(parsed)) {
        return false;
    }
    *value = parsed;
    return true;
}

void Metadata::SetString(const std::string& key, const std::string& value) {
    fields_[key] = value;
}

void Metadata::SetUint64(const std::string& key, std::uint64_t value) {
    std::ostringstream stream;
    stream << value;
    fields_[key] = stream.str();
}

void Metadata::SetDouble(const std::string& key, double value) {
    std::ostringstream stream;
    stream << std::setprecision(std::numeric_limits<double>::max_digits10)
           << value;
    fields_[key] = stream.str();
}

void Metadata::Erase(const std::string& key) {
    fields_.erase(key);
}

const std::map<std::string, std::string>& Metadata::Fields() const {
    return fields_;
}

}  // namespace pipeline
}  // namespace rdma_dada
