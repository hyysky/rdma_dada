#pragma once

#include <map>
#include <string>
#include <vector>

namespace rdma_dada {
namespace json {

class Value {
public:
    enum Type {
        kNull,
        kBoolean,
        kNumber,
        kString,
        kArray,
        kObject
    };

    typedef std::vector<Value> Array;
    typedef std::map<std::string, Value> Object;

    Value();

    static Value Boolean(bool value);
    static Value Number(const std::string& value);
    static Value String(const std::string& value);
    static Value ArrayValue(const Array& value);
    static Value ObjectValue(const Object& value);

    Type type() const;
    bool boolean() const;
    const std::string& text() const;
    const Array& array() const;
    const Object& object() const;

private:
    Type type_;
    bool boolean_;
    std::string text_;
    Array array_;
    Object object_;
};

bool Parse(const std::string& input, Value* output, std::string* error);

}  // namespace json
}  // namespace rdma_dada
