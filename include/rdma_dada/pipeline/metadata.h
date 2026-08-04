#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace rdma_dada {
namespace pipeline {

// Dependency-free representation of a DADA ASCII header or stage parameters.
// The PSRDADA adapter is responsible for converting to/from the header block.
class Metadata {
public:
    bool Has(const std::string& key) const;
    bool GetString(const std::string& key, std::string* value) const;
    bool GetUint64(const std::string& key, std::uint64_t* value) const;
    bool GetDouble(const std::string& key, double* value) const;

    void SetString(const std::string& key, const std::string& value);
    void SetUint64(const std::string& key, std::uint64_t value);
    void SetDouble(const std::string& key, double value);
    void Erase(const std::string& key);

    const std::map<std::string, std::string>& Fields() const;

private:
    std::map<std::string, std::string> fields_;
};

using StageParameters = Metadata;

}  // namespace pipeline
}  // namespace rdma_dada
