// Json.cpp — minimal recursive-descent JSON parser (see Json.hpp).
#include "jade/Json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jade {
namespace json {

namespace {

struct Parser {
    const char* begin;
    const char* p;
    const char* end;

    explicit Parser(const char* d, size_t n) : begin(d), p(d), end(d + n) {}

    [[noreturn]] void fail(const char* reason) {
        throw ParseError(reason, size_t(p - begin));
    }
    [[noreturn]] void fail_at(const char* reason, const char* at) {
        throw ParseError(reason, size_t(at - begin));
    }

    void skip_ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }
    char peek() { return p < end ? *p : '\0'; }

    Value parse_value() {
        skip_ws();
        if (p >= end) fail("Expecting value");
        char c = *p;
        switch (c) {
            case '{': return parse_object();
            case '[': return parse_array();
            case '"': { Value v; v.type = Value::Type::String; v.str = parse_string(); return v; }
            case 't': case 'f': return parse_bool();
            case 'n': return parse_null();
            default:
                if (c == '-' || (c >= '0' && c <= '9'))
                    return parse_number();
                fail("Expecting value");
        }
    }

    Value parse_object() {
        Value v;
        v.type = Value::Type::Object;
        ++p;  // {
        skip_ws();
        if (peek() == '}') { ++p; return v; }
        while (true) {
            skip_ws();
            if (peek() != '"')
                fail("Expecting property name enclosed in double quotes");
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') fail("Expecting ':' delimiter");
            ++p;
            Value value = parse_value();
            // CPython's json decoder keeps the last value for duplicate keys.
            v.obj[std::move(key)] = std::move(value);
            skip_ws();
            char c = peek();
            if (c == ',') { ++p; continue; }
            if (c == '}') { ++p; break; }
            fail("Expecting ',' delimiter");
        }
        return v;
    }

    Value parse_array() {
        Value v;
        v.type = Value::Type::Array;
        ++p;  // [
        skip_ws();
        if (peek() == ']') { ++p; return v; }
        while (true) {
            v.arr.push_back(parse_value());
            skip_ws();
            char c = peek();
            if (c == ',') { ++p; continue; }
            if (c == ']') { ++p; break; }
            fail("Expecting ',' delimiter");
        }
        return v;
    }

    std::string parse_string() {
        const char* opening_quote = p;
        ++p;  // opening quote
        std::string out;
        while (p < end) {
            char c = *p++;
            if (c == '"') return out;
            if (c == '\\') {
                if (p >= end) fail("Invalid \\escape");
                char e = *p++;
                switch (e) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        if (end - p < 4) fail("Invalid \\uXXXX escape");
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p++;
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= unsigned(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= unsigned(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= unsigned(h - 'A' + 10);
                            else fail("Invalid \\uXXXX escape");
                        }
                        // Encode the BMP code point as UTF-8 (glTF keys are ASCII;
                        // surrogate pairs are not expected in glTF strings).
                        if (cp < 0x80) {
                            out.push_back(char(cp));
                        } else if (cp < 0x800) {
                            out.push_back(char(0xC0 | (cp >> 6)));
                            out.push_back(char(0x80 | (cp & 0x3F)));
                        } else {
                            out.push_back(char(0xE0 | (cp >> 12)));
                            out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
                            out.push_back(char(0x80 | (cp & 0x3F)));
                        }
                        break;
                    }
                    default: fail_at("Invalid \\escape", p - 2);
                }
            } else {
                if (static_cast<unsigned char>(c) < 0x20)
                    fail_at("Invalid control character at", p - 1);
                out.push_back(c);
            }
        }
        fail_at("Unterminated string starting at", opening_quote);
    }

    Value parse_bool() {
        Value v;
        v.type = Value::Type::Bool;
        if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) { v.b = true; p += 4; return v; }
        if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) { v.b = false; p += 5; return v; }
        fail("Expecting value");
    }

    Value parse_null() {
        if (end - p >= 4 && std::memcmp(p, "null", 4) == 0) { p += 4; return Value{}; }
        fail("Expecting value");
    }

    Value parse_number() {
        const char* start = p;
        if (*p == '-') ++p;
        if (p >= end) fail_at("Expecting value", start);
        if (*p == '0') {
            ++p;
        } else if (*p >= '1' && *p <= '9') {
            do { ++p; } while (p < end && *p >= '0' && *p <= '9');
        } else {
            fail_at("Expecting value", start);
        }
        bool integer_syntax = true;
        if (p < end && *p == '.' && p + 1 < end &&
            p[1] >= '0' && p[1] <= '9') {
            integer_syntax = false;
            p += 2;
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }
        if (p < end && (*p == 'e' || *p == 'E')) {
            const char* exponent = p;
            const char* q = p + 1;
            if (q < end && (*q == '+' || *q == '-')) ++q;
            if (q < end && *q >= '0' && *q <= '9') {
                integer_syntax = false;
                p = q + 1;
                while (p < end && *p >= '0' && *p <= '9') ++p;
            } else {
                // The Python scanner stops before a malformed exponent; the
                // surrounding object/array or strict top-level check reports
                // the delimiter/extra-data error at the exponent marker.
                p = exponent;
            }
        }
        std::string tok(start, p);
        Value v;
        v.type = Value::Type::Number;
        v.num = std::strtod(tok.c_str(), nullptr);
        v.number_is_integer = integer_syntax;
        if (integer_syntax)
            v.integer_text = tok == "-0" ? "0" : tok;
        return v;
    }
};

}  // namespace

Value parse(const char* data, size_t n) {
    Parser ps(data, n);
    Value v = ps.parse_value();
    ps.skip_ws();
    // trailing bytes after the top-level value are tolerated (GLB JSON chunks are
    // 4-byte padded with spaces).
    return v;
}

Value parse_strict(const char* data, size_t n) {
    Parser ps(data, n);
    Value v = ps.parse_value();
    ps.skip_ws();
    if (ps.p != ps.end) ps.fail("Extra data");
    return v;
}

// ── serializer ──

namespace {

void dump_string(const std::string& s, std::string& out) {
    out += '"';
    for (char ch : s) {
        switch (ch) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x",
                                  static_cast<unsigned>(
                                      static_cast<unsigned char>(ch)));
                    out += buf;
                } else {
                    out += ch;  // UTF-8 passes through (ensure_ascii=False)
                }
        }
    }
    out += '"';
}

void dump_number(const Value& value, std::string& out) {
    if (value.number_is_integer && !value.integer_text.empty()) {
        out += value.integer_text;
        return;
    }
    const double d = value.num;
    // Integral doubles inside the exactly-representable range print as
    // integers (Python ints round-trip as ints).
    if (d >= -9007199254740992.0 && d <= 9007199254740992.0 &&
        d == static_cast<double>(static_cast<long long>(d))) {
        out += std::to_string(static_cast<long long>(d));
        return;
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", d);
    // Trim to shortest round-trip representation.
    for (int prec = 1; prec < 17; ++prec) {
        char t[32];
        std::snprintf(t, sizeof t, "%.*g", prec, d);
        if (std::strtod(t, nullptr) == d) {
            out += t;
            return;
        }
    }
    out += buf;
}

void dump_value(const Value& v, int indent, int depth, std::string& out) {
    const std::string pad(indent > 0 ? size_t(indent) * (depth + 1) : 0, ' ');
    const std::string end_pad(indent > 0 ? size_t(indent) * depth : 0, ' ');
    const char* nl = indent > 0 ? "\n" : "";
    const char* kv_sep = indent > 0 ? ": " : ":";
    switch (v.type) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.b ? "true" : "false"; break;
        case Value::Type::Number: dump_number(v, out); break;
        case Value::Type::String: dump_string(v.str, out); break;
        case Value::Type::Array: {
            if (v.arr.empty()) { out += "[]"; break; }
            out += '[';
            out += nl;
            for (size_t i = 0; i < v.arr.size(); ++i) {
                out += pad;
                dump_value(v.arr[i], indent, depth + 1, out);
                if (i + 1 < v.arr.size()) out += ',';
                out += nl;
            }
            out += end_pad;
            out += ']';
            break;
        }
        case Value::Type::Object: {
            if (v.obj.empty()) { out += "{}"; break; }
            out += '{';
            out += nl;
            size_t i = 0;
            for (const auto& kv : v.obj) {
                out += pad;
                dump_string(kv.first, out);
                out += kv_sep;
                dump_value(kv.second, indent, depth + 1, out);
                if (++i < v.obj.size()) out += ',';
                out += nl;
            }
            out += end_pad;
            out += '}';
            break;
        }
    }
}

}  // namespace

std::string dump(const Value& v, int indent) {
    std::string out;
    dump_value(v, indent, 0, out);
    return out;
}

Value make_integer(const std::string& canonical_decimal) {
    Value value;
    value.type = Value::Type::Number;
    value.num = std::strtod(canonical_decimal.c_str(), nullptr);
    value.number_is_integer = true;
    value.integer_text = canonical_decimal;
    return value;
}

}  // namespace json
}  // namespace jade
