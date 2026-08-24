// Json.hpp — a minimal JSON parser for the glTF reader.
//
// glTF JSON is a small, well-formed subset (objects / arrays / strings /
// numbers / bool / null). Numeric consumers keep the convenient double value,
// while integer-syntax tokens also retain their exact decimal spelling.  glTF
// integers are small; project JSON can contain Python arbitrary-precision ints
// and needs the exact token for faithful coercion and round-tripping.
#pragma once

#include <cstddef>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace jade {
namespace json {

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object };
    Type type = Type::Null;
    bool        b = false;
    double      num = 0.0;
    // True only when a parsed JSON number used integer syntax. This matters
    // for Python-compatible schema checks: json.loads("1") returns int while
    // json.loads("1.0") returns float, even though both have the same value.
    bool        number_is_integer = false;
    // Canonical decimal value for an exact integer. Empty for floating-point
    // numbers and for legacy callers that only supplied a double.
    std::string integer_text;
    std::string str;
    std::vector<Value>          arr;
    std::map<std::string, Value> obj;

    bool is_obj() const { return type == Type::Object; }
    bool is_arr() const { return type == Type::Array; }
    bool is_num() const { return type == Type::Number; }
    bool is_str() const { return type == Type::String; }

    // Object member by key, or nullptr. Non-objects return nullptr.
    const Value* find(const std::string& key) const {
        if (type != Type::Object) return nullptr;
        auto it = obj.find(key);
        return it == obj.end() ? nullptr : &it->second;
    }
    bool has(const std::string& key) const { return find(key) != nullptr; }

    // Numeric accessors with a default (for optional glTF fields).
    double num_or(double d) const { return type == Type::Number ? num : d; }
    long long int_or(long long d) const {
        return type == Type::Number ? static_cast<long long>(num) : d;
    }
    // Convenience: object[key] as int / with default.
    long long get_int(const std::string& key, long long d = 0) const {
        const Value* v = find(key);
        return v ? v->int_or(d) : d;
    }
    const std::string& str_ref() const { return str; }
    const std::vector<Value>& items() const { return arr; }
};

class ParseError : public std::runtime_error {
public:
    ParseError(std::string reason, size_t offset)
        : std::runtime_error("json: " + reason),
          reason_(std::move(reason)), offset_(offset) {}

    const std::string& reason() const { return reason_; }
    size_t offset() const { return offset_; }

private:
    std::string reason_;
    size_t offset_ = 0;
};

// Parse a complete JSON document. Throws std::runtime_error on malformed input.
Value parse(const char* data, size_t n);
inline Value parse(const std::string& s) { return parse(s.data(), s.size()); }

// Parse a standalone JSON document and reject every non-whitespace trailing
// byte. The regular parser intentionally tolerates GLB chunk padding; project
// sidecar formats such as .jprefab use this strict form instead.
Value parse_strict(const char* data, size_t n);
inline Value parse_strict(const std::string& s) {
    return parse_strict(s.data(), s.size());
}

// Serialize a Value back to JSON text. indent > 0 pretty-prints (Python
// json.dump(indent=N) layout); indent == 0 emits compact one-line JSON.
// Integral numbers are emitted without a decimal point so Python-written
// ints round-trip as ints. Non-ASCII passes through as UTF-8
// (ensure_ascii=False). Object keys are emitted in map order (sorted) —
// callers that need Python's insertion order must not rely on key order.
std::string dump(const Value& v, int indent = 0);

// Convenience builders for document-authoring code (the project model).
inline Value make_str(const std::string& s) {
    Value v; v.type = Value::Type::String; v.str = s; return v;
}
inline Value make_num(double d) {
    Value v;
    v.type = Value::Type::Number;
    v.num = d;
    v.number_is_integer =
        d >= -9007199254740992.0 && d <= 9007199254740992.0 &&
        d == static_cast<double>(static_cast<long long>(d));
    if (v.number_is_integer)
        v.integer_text = std::to_string(static_cast<long long>(d));
    return v;
}
// Construct an arbitrary-precision integer from canonical signed decimal text.
// The exact text drives serialization; num remains available as an approximate
// value for existing small-number consumers.
Value make_integer(const std::string& canonical_decimal);
inline Value make_bool(bool b) {
    Value v; v.type = Value::Type::Bool; v.b = b; return v;
}
inline Value make_obj() { Value v; v.type = Value::Type::Object; return v; }
inline Value make_arr() { Value v; v.type = Value::Type::Array; return v; }

}  // namespace json
}  // namespace jade
