#include "rdma_dada/config/json_value.h"

#include <cctype>
#include <cstddef>
#include <iomanip>
#include <sstream>

namespace rdma_dada {
namespace json {

Value::Value() : type_(kNull), boolean_(false) {}

Value Value::Boolean(bool value) {
    Value result;
    result.type_ = kBoolean;
    result.boolean_ = value;
    return result;
}

Value Value::Number(const std::string& value) {
    Value result;
    result.type_ = kNumber;
    result.text_ = value;
    return result;
}

Value Value::String(const std::string& value) {
    Value result;
    result.type_ = kString;
    result.text_ = value;
    return result;
}

Value Value::ArrayValue(const Array& value) {
    Value result;
    result.type_ = kArray;
    result.array_ = value;
    return result;
}

Value Value::ObjectValue(const Object& value) {
    Value result;
    result.type_ = kObject;
    result.object_ = value;
    return result;
}

Value::Type Value::type() const { return type_; }
bool Value::boolean() const { return boolean_; }
const std::string& Value::text() const { return text_; }
const Value::Array& Value::array() const { return array_; }
const Value::Object& Value::object() const { return object_; }

namespace {

bool Fail(const std::string& message, std::string* error) {
    if (error) *error = message;
    return false;
}

void AppendUtf8(unsigned int code_point, std::string* output) {
    if (code_point <= 0x7fU) {
        output->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7ffU) {
        output->push_back(static_cast<char>(0xc0U | (code_point >> 6U)));
        output->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else if (code_point <= 0xffffU) {
        output->push_back(static_cast<char>(0xe0U | (code_point >> 12U)));
        output->push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    } else {
        output->push_back(static_cast<char>(0xf0U | (code_point >> 18U)));
        output->push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3fU)));
        output->push_back(static_cast<char>(0x80U | (code_point & 0x3fU)));
    }
}

class Parser {
public:
    Parser(const std::string& input, std::string* error)
        : input_(input), position_(0), error_(error) {}

    bool Run(Value* output) {
        if (!output) return Error("output pointer is null");
        SkipWhitespace();
        if (!ParseValue(output)) return false;
        SkipWhitespace();
        if (position_ != input_.size()) return Error("unexpected trailing data");
        return true;
    }

private:
    bool ParseValue(Value* output) {
        if (position_ == input_.size()) return Error("expected a JSON value");
        const char current = input_[position_];
        if (current == '{') return ParseObject(output);
        if (current == '[') return ParseArray(output);
        if (current == '"') {
            std::string value;
            if (!ParseString(&value)) return false;
            *output = Value::String(value);
            return true;
        }
        if (current == 't') return ParseLiteral("true", Value::Boolean(true), output);
        if (current == 'f') return ParseLiteral("false", Value::Boolean(false), output);
        if (current == 'n') return ParseLiteral("null", Value(), output);
        if (current == '-' || (current >= '0' && current <= '9')) {
            return ParseNumber(output);
        }
        return Error("unexpected character while reading a value");
    }

    bool ParseObject(Value* output) {
        ++position_;
        SkipWhitespace();
        Value::Object object;
        if (Consume('}')) {
            *output = Value::ObjectValue(object);
            return true;
        }
        while (true) {
            if (position_ == input_.size() || input_[position_] != '"') {
                return Error("object key must be a string");
            }
            std::string key;
            if (!ParseString(&key)) return false;
            if (object.count(key) != 0U) return Error("duplicate object key: " + key);
            SkipWhitespace();
            if (!Consume(':')) return Error("expected ':' after object key");
            SkipWhitespace();
            Value value;
            if (!ParseValue(&value)) return false;
            object.insert(std::make_pair(key, value));
            SkipWhitespace();
            if (Consume('}')) break;
            if (!Consume(',')) return Error("expected ',' or '}' in object");
            SkipWhitespace();
        }
        *output = Value::ObjectValue(object);
        return true;
    }

    bool ParseArray(Value* output) {
        ++position_;
        SkipWhitespace();
        Value::Array array;
        if (Consume(']')) {
            *output = Value::ArrayValue(array);
            return true;
        }
        while (true) {
            Value value;
            if (!ParseValue(&value)) return false;
            array.push_back(value);
            SkipWhitespace();
            if (Consume(']')) break;
            if (!Consume(',')) return Error("expected ',' or ']' in array");
            SkipWhitespace();
        }
        *output = Value::ArrayValue(array);
        return true;
    }

    bool ParseString(std::string* output) {
        ++position_;
        output->clear();
        while (position_ < input_.size()) {
            const unsigned char current =
                static_cast<unsigned char>(input_[position_++]);
            if (current == '"') return true;
            if (current < 0x20U) return Error("unescaped control character in string");
            if (current != '\\') {
                output->push_back(static_cast<char>(current));
                continue;
            }
            if (position_ == input_.size()) return Error("incomplete string escape");
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"': output->push_back('"'); break;
                case '\\': output->push_back('\\'); break;
                case '/': output->push_back('/'); break;
                case 'b': output->push_back('\b'); break;
                case 'f': output->push_back('\f'); break;
                case 'n': output->push_back('\n'); break;
                case 'r': output->push_back('\r'); break;
                case 't': output->push_back('\t'); break;
                case 'u': {
                    unsigned int code_point = 0;
                    if (!ParseHex4(&code_point)) return false;
                    if (code_point >= 0xd800U && code_point <= 0xdbffU) {
                        if (position_ + 2U > input_.size() ||
                            input_[position_] != '\\' ||
                            input_[position_ + 1U] != 'u') {
                            return Error("high surrogate must be followed by a low surrogate");
                        }
                        position_ += 2U;
                        unsigned int low = 0;
                        if (!ParseHex4(&low)) return false;
                        if (low < 0xdc00U || low > 0xdfffU) {
                            return Error("invalid low surrogate");
                        }
                        code_point = 0x10000U +
                            ((code_point - 0xd800U) << 10U) + (low - 0xdc00U);
                    } else if (code_point >= 0xdc00U && code_point <= 0xdfffU) {
                        return Error("unexpected low surrogate");
                    }
                    AppendUtf8(code_point, output);
                    break;
                }
                default: return Error("invalid string escape");
            }
        }
        return Error("unterminated string");
    }

    bool ParseHex4(unsigned int* output) {
        if (position_ + 4U > input_.size()) return Error("incomplete unicode escape");
        unsigned int value = 0;
        for (unsigned int index = 0; index < 4U; ++index) {
            const char digit = input_[position_++];
            value <<= 4U;
            if (digit >= '0' && digit <= '9') value += static_cast<unsigned int>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') value += static_cast<unsigned int>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') value += static_cast<unsigned int>(digit - 'A' + 10);
            else return Error("invalid hexadecimal digit in unicode escape");
        }
        *output = value;
        return true;
    }

    bool ParseNumber(Value* output) {
        const std::size_t start = position_;
        Consume('-');
        if (Consume('0')) {
            if (position_ < input_.size() && std::isdigit(
                    static_cast<unsigned char>(input_[position_]))) {
                return Error("leading zero in number");
            }
        } else if (!ConsumeDigits()) {
            return Error("expected integer digits");
        }
        if (Consume('.')) {
            if (!ConsumeDigits()) return Error("expected digits after decimal point");
        }
        if (position_ < input_.size() &&
            (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_;
            if (position_ < input_.size() &&
                (input_[position_] == '+' || input_[position_] == '-')) {
                ++position_;
            }
            if (!ConsumeDigits()) return Error("expected exponent digits");
        }
        *output = Value::Number(input_.substr(start, position_ - start));
        return true;
    }

    bool ConsumeDigits() {
        const std::size_t start = position_;
        while (position_ < input_.size() && std::isdigit(
                   static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        return position_ != start;
    }

    bool ParseLiteral(const char* literal, const Value& value, Value* output) {
        const std::string expected(literal);
        if (input_.compare(position_, expected.size(), expected) != 0) {
            return Error("invalid literal");
        }
        position_ += expected.size();
        *output = value;
        return true;
    }

    bool Consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void SkipWhitespace() {
        while (position_ < input_.size()) {
            const char current = input_[position_];
            if (current != ' ' && current != '\t' && current != '\r' &&
                current != '\n') {
                break;
            }
            ++position_;
        }
    }

    bool Error(const std::string& message) {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t index = 0; index < position_ && index < input_.size(); ++index) {
            if (input_[index] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        std::ostringstream formatted;
        formatted << "JSON line " << line << ", column " << column << ": " << message;
        return Fail(formatted.str(), error_);
    }

    const std::string& input_;
    std::size_t position_;
    std::string* error_;
};

}  // namespace

bool Parse(const std::string& input, Value* output, std::string* error) {
    Parser parser(input, error);
    return parser.Run(output);
}

}  // namespace json
}  // namespace rdma_dada
