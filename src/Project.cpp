// Project.cpp — implementation. Ports project/serialize.py's load, the
// ModProject data layer, and build/builder.py's apply/write pipeline with
// raw-JSON dispatch for the complete set of known project operation names.
#include "jade/Project.hpp"
#include "jade/ProjectAssets.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "jade/Compression.hpp"
#include "jade/Collision.hpp"
#include "jade/CollisionFollow.hpp"
#include "jade/Crc32.hpp"
#include "jade/Gao.hpp"
#include "jade/Image.hpp"
#include "jade/Jtmod.hpp"
#include "jade/Keys.hpp"
#include "jade/Light.hpp"
#include "jade/LoadSim.hpp"
#include "jade/LoaValidate.hpp"
#include "jade/Material.hpp"
#include "jade/ObjectPlacer.hpp"
#include "jade/Patcher.hpp"
#include "jade/PatcherModel.hpp"
#include "jade/Rli.hpp"
#include "jade/RliRescale.hpp"
#include "jade/MeshSwap.hpp"   // splice_sub_entry
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"
#include "jade/WolInfo.hpp"
#include "jade/WowStream.hpp"
#include "jade/Zone.hpp"
#include "jade/ZoneCreate.hpp"

namespace jade {
namespace project {

// ── SHA-256 (compact, public-domain layout) ─────────────────────────────────
namespace {
struct VerifyLeafIssue {
    std::string level;
    std::string message;
};

std::vector<VerifyLeafIssue> check_texture_insertion(
    const std::vector<uint8_t>& old_dec,
    const std::vector<uint8_t>& new_dec, uint32_t new_key,
    size_t expected_new_records = std::numeric_limits<size_t>::max());

struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64];
    uint64_t len = 0;
    size_t fill = 0;

    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
            0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
            0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
            0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
            0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
            0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
            0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 |
                   uint32_t(p[i * 4 + 2]) << 8 | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5],
                 g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const uint8_t* p, size_t n) {
        len += n;
        while (n) {
            size_t take = std::min(n, size_t(64) - fill);
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        uint8_t z = 0;
        while (fill != 56) update(&z, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = uint8_t(bits >> (56 - i * 8));
        update(lb, 8);
        char out[65];
        for (int i = 0; i < 8; ++i) std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return std::string(out, 64);
    }
};
}  // namespace

std::string sha256_of_file(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return "";
    Sha256 s;
    std::vector<uint8_t> chunk(1 << 20);
    while (f) {
        f.read(reinterpret_cast<char*>(chunk.data()), std::streamsize(chunk.size()));
        std::streamsize got = f.gcount();
        if (got <= 0) break;
        s.update(chunk.data(), size_t(got));
    }
    return s.hex();
}

bool BaseRef::matches(const std::string& archive_path) const {
    std::ifstream f(std::filesystem::u8path(archive_path),
                    std::ios::binary | std::ios::ate);
    if (!f) return false;
    if (uint64_t(f.tellg()) != archive_size) return false;
    return sha256_of_file(archive_path) == archive_sha256;
}

// ── load ────────────────────────────────────────────────────────────────────

namespace {
std::vector<uint32_t> utf8_codepoints(const std::string& text) {
    std::vector<uint32_t> result;
    for (size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index++]);
        if (first < 0x80) {
            result.push_back(first);
            continue;
        }
        int continuation = 0;
        uint32_t codepoint = 0;
        if ((first & 0xe0) == 0xc0) {
            continuation = 1;
            codepoint = first & 0x1f;
        } else if ((first & 0xf0) == 0xe0) {
            continuation = 2;
            codepoint = first & 0x0f;
        } else if ((first & 0xf8) == 0xf0) {
            continuation = 3;
            codepoint = first & 0x07;
        } else {
            result.push_back(0xfffd);
            continue;
        }
        if (index + size_t(continuation) > text.size()) {
            result.push_back(0xfffd);
            break;
        }
        bool valid = true;
        for (int part = 0; part < continuation; ++part) {
            const unsigned char next =
                static_cast<unsigned char>(text[index + size_t(part)]);
            if ((next & 0xc0) != 0x80) {
                valid = false;
                break;
            }
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if (!valid) {
            result.push_back(0xfffd);
            continue;
        }
        index += size_t(continuation);
        result.push_back(codepoint);
    }
    return result;
}

bool python_numeric_space(uint32_t cp) {
    return cp == 0x09 || cp == 0x0a || cp == 0x0b || cp == 0x0c ||
           cp == 0x0d || cp == 0x20 || cp == 0x85 || cp == 0xa0 ||
           cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200a) ||
           cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f ||
           cp == 0x3000;
}

int python_decimal_digit(uint32_t cp) {
    static const uint32_t zeroes[] = {
        0x30, 0x660, 0x6f0, 0x7c0, 0x966, 0x9e6, 0xa66, 0xae6,
        0xb66, 0xbe6, 0xc66, 0xce6, 0xd66, 0xde6, 0xe50, 0xed0,
        0xf20, 0x1040, 0x1090, 0x17e0, 0x1810, 0x1946, 0x19d0,
        0x1a80, 0x1a90, 0x1b50, 0x1bb0, 0x1c40, 0x1c50, 0xa620,
        0xa8d0, 0xa900, 0xa9d0, 0xa9f0, 0xaa50, 0xabf0, 0xff10,
        0x104a0, 0x10d30, 0x11066, 0x110f0, 0x11136, 0x111d0,
        0x112f0, 0x11450, 0x114d0, 0x11650, 0x116c0, 0x11730,
        0x118e0, 0x11950, 0x11c50, 0x11d50, 0x11da0, 0x11f50,
        0x16a60, 0x16ac0, 0x16b50, 0x1d7ce, 0x1d7d8, 0x1d7e2,
        0x1d7ec, 0x1d7f6, 0x1e140, 0x1e2f0, 0x1e4f0, 0x1e950,
        0x1fbf0,
    };
    for (uint32_t zero : zeroes)
        if (cp >= zero && cp <= zero + 9) return int(cp - zero);
    return -1;
}

bool python_repr_nonprintable(uint32_t cp) {
    return cp < 0x20 || cp == 0x7f ||
           (cp != 0x20 && python_numeric_space(cp)) ||
           cp == 0x200b || (cp >= 0xd800 && cp <= 0xdfff);
}

std::string python_repr_string(const std::string& value) {
    const bool use_double =
        value.find('\'') != std::string::npos &&
        value.find('"') == std::string::npos;
    const char quote = use_double ? '"' : '\'';
    std::string out(1, quote);
    for (uint32_t cp : utf8_codepoints(value)) {
        if (cp == static_cast<unsigned char>(quote)) {
            out.push_back('\\');
            out.push_back(char(cp));
        } else if (cp == '\\') {
            out += "\\\\";
        } else if (cp == '\n') {
            out += "\\n";
        } else if (cp == '\r') {
            out += "\\r";
        } else if (cp == '\t') {
            out += "\\t";
        } else if (python_repr_nonprintable(cp)) {
            char escaped[16];
            if (cp <= 0xff)
                std::snprintf(escaped, sizeof escaped, "\\x%02x", unsigned(cp));
            else if (cp <= 0xffff)
                std::snprintf(escaped, sizeof escaped, "\\u%04x", unsigned(cp));
            else
                std::snprintf(escaped, sizeof escaped, "\\U%08x", unsigned(cp));
            out += escaped;
        } else if (cp < 0x80) {
            out.push_back(char(cp));
        } else if (cp < 0x800) {
            out.push_back(char(0xc0 | (cp >> 6)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        } else if (cp < 0x10000) {
            out.push_back(char(0xe0 | (cp >> 12)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        } else {
            out.push_back(char(0xf0 | (cp >> 18)));
            out.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
            out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
            out.push_back(char(0x80 | (cp & 0x3f)));
        }
    }
    out.push_back(quote);
    return out;
}

std::string python_number_repr(const json::Value& value) {
    if (value.number_is_integer) {
        if (!value.integer_text.empty()) return value.integer_text;
        if (value.num == 0.0) return "0";
        char integer[64];
        std::snprintf(integer, sizeof integer, "%.0f", value.num);
        return integer;
    }
    if (std::isnan(value.num)) return "nan";
    if (std::isinf(value.num)) return value.num < 0 ? "-inf" : "inf";
    char number[64];
    std::snprintf(number, sizeof number, "%.17g", value.num);
    std::string out(number);
    if (value.num == std::trunc(value.num) &&
        out.find_first_of(".eE") == std::string::npos)
        out += ".0";
    return out;
}

std::string python_repr(const json::Value* value) {
    if (!value || value->type == json::Value::Type::Null) return "None";
    if (value->type == json::Value::Type::Bool)
        return value->b ? "True" : "False";
    if (value->type == json::Value::Type::Number)
        return python_number_repr(*value);
    if (value->type == json::Value::Type::String)
        return python_repr_string(value->str);
    if (value->type == json::Value::Type::Array) {
        std::string out = "[";
        for (size_t index = 0; index < value->arr.size(); ++index) {
            if (index) out += ", ";
            out += python_repr(&value->arr[index]);
        }
        return out + "]";
    }
    std::string out = "{";
    size_t index = 0;
    for (const auto& field : value->obj) {
        if (index++) out += ", ";
        out += python_repr_string(field.first) + ": " +
               python_repr(&field.second);
    }
    return out + "}";
}

std::string python_json_error(const std::string& text,
                              const json::ParseError& error) {
    size_t line = 1;
    size_t column = 1;
    const size_t stop = std::min(error.offset(), text.size());
    for (size_t index = 0; index < stop; ++index) {
        if (text[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return error.reason() + ": line " + std::to_string(line) +
           " column " + std::to_string(column) + " (char " +
           std::to_string(error.offset()) + ")";
}

std::string python_text_newlines(const std::string& raw) {
    // open(..., encoding="utf-8") uses universal-newline translation.
    std::string text;
    text.reserve(raw.size());
    for (size_t index = 0; index < raw.size(); ++index) {
        if (raw[index] == '\r') {
            if (index + 1 < raw.size() && raw[index + 1] == '\n') ++index;
            text.push_back('\n');
        } else {
            text.push_back(raw[index]);
        }
    }
    return text;
}

std::string project_iso_now();
bool project_truthy(const json::Value* value);

std::string python_type_name(const json::Value* value) {
    if (!value || value->type == json::Value::Type::Null) return "NoneType";
    if (value->type == json::Value::Type::Bool) return "bool";
    if (value->is_num())
        return value->number_is_integer ? "int" : "float";
    if (value->is_str()) return "str";
    if (value->is_arr()) return "list";
    return "dict";
}

void set_python_load_error(ModProject& project, const std::string& type,
                           const std::string& message) {
    project.ok = false;
    project.error_type = type;
    project.error = message;
}

std::string canonical_decimal(bool negative, std::string digits) {
    const size_t first = digits.find_first_not_of('0');
    if (first == std::string::npos) return "0";
    digits.erase(0, first);
    return negative ? "-" + digits : digits;
}

std::string decimal_mul_add(std::string digits, unsigned multiplier,
                            unsigned addend) {
    unsigned carry = addend;
    for (size_t index = digits.size(); index-- > 0;) {
        const unsigned value = unsigned(digits[index] - '0') * multiplier + carry;
        digits[index] = char('0' + value % 10);
        carry = value / 10;
    }
    while (carry) {
        digits.insert(digits.begin(), char('0' + carry % 10));
        carry /= 10;
    }
    return digits;
}

bool parse_python_integer_string(const std::string& raw, int base,
                                 std::string& canonical) {
    const std::vector<uint32_t> codepoints = utf8_codepoints(raw);
    size_t begin = 0;
    size_t end = codepoints.size();
    while (begin < end && python_numeric_space(codepoints[begin])) ++begin;
    while (end > begin && python_numeric_space(codepoints[end - 1])) --end;

    bool negative = false;
    size_t index = begin;
    if (index < end && (codepoints[index] == '+' || codepoints[index] == '-')) {
        negative = codepoints[index] == '-';
        ++index;
    }
    bool prefixed = false;
    if (base == 16 && index + 1 < end && codepoints[index] == '0' &&
        (codepoints[index + 1] == 'x' || codepoints[index + 1] == 'X')) {
        prefixed = true;
        index += 2;
    }

    std::vector<unsigned> values;
    bool previous_digit = false;
    bool prefix_underscore = prefixed;
    for (; index < end; ++index) {
        const uint32_t cp = codepoints[index];
        int digit = python_decimal_digit(cp);
        if (digit < 0 && base == 16 && cp >= 'a' && cp <= 'f')
            digit = int(cp - 'a') + 10;
        if (digit < 0 && base == 16 && cp >= 'A' && cp <= 'F')
            digit = int(cp - 'A') + 10;
        if (digit >= 0 && digit < base) {
            values.push_back(unsigned(digit));
            previous_digit = true;
            prefix_underscore = false;
            continue;
        }
        if (cp == '_' && (previous_digit || prefix_underscore) &&
            index + 1 < end) {
            const uint32_t next = codepoints[index + 1];
            int next_digit = python_decimal_digit(next);
            if (next_digit < 0 && base == 16 && next >= 'a' && next <= 'f')
                next_digit = int(next - 'a') + 10;
            if (next_digit < 0 && base == 16 && next >= 'A' && next <= 'F')
                next_digit = int(next - 'A') + 10;
            if (next_digit >= 0 && next_digit < base) {
                previous_digit = false;
                prefix_underscore = false;
                continue;
            }
        }
        return false;
    }
    if (values.empty() || !previous_digit) return false;

    std::string magnitude = "0";
    for (unsigned digit : values)
        magnitude = decimal_mul_add(std::move(magnitude), unsigned(base), digit);
    canonical = canonical_decimal(negative, std::move(magnitude));
    return true;
}

std::string truncated_double_integer(double number) {
    const double truncated = std::trunc(number);
    const int required = std::snprintf(nullptr, 0, "%.0f", truncated);
    std::string result(size_t(std::max(0, required)), '\0');
    if (required > 0)
        std::snprintf(&result[0], result.size() + 1, "%.0f", truncated);
    return canonical_decimal(!result.empty() && result[0] == '-',
                             !result.empty() && result[0] == '-'
                                 ? result.substr(1) : result);
}

bool python_int_value(const json::Value* value, json::Value& result,
                      std::string& error_type, std::string& error) {
    if (!value || value->type == json::Value::Type::Null) {
        error_type = "TypeError";
        error = "int() argument must be a string, a bytes-like object or a "
                "real number, not 'NoneType'";
        return false;
    }
    if (value->type == json::Value::Type::Bool) {
        result = json::make_integer(value->b ? "1" : "0");
        return true;
    }
    if (value->is_num()) {
        if (value->number_is_integer && !value->integer_text.empty())
            result = json::make_integer(value->integer_text);
        else
            result = json::make_integer(truncated_double_integer(value->num));
        return true;
    }
    if (value->is_str()) {
        const std::string& raw = value->str;
        std::string canonical;
        if (!parse_python_integer_string(raw, 10, canonical)) {
            error_type = "ValueError";
            error = "invalid literal for int() with base 10: " +
                    python_repr_string(raw);
            return false;
        }
        result = json::make_integer(canonical);
        return true;
    }

    error_type = "TypeError";
    error = "int() argument must be a string, a bytes-like object or a real "
            "number, not '" + python_type_name(value) + "'";
    return false;
}

bool known_project_operation(const std::string& type) {
    static const std::unordered_set<std::string> known = {
        "replace_texture", "replace_mesh", "stub_mesh", "replace_animation",
        "edit_light", "modify_transform", "add_object_collision",
        "modify_gao_flags", "modify_gao_material", "add_object",
        "retarget_material_texture", "set_multi_material_slot",
        "set_element_material", "set_material_flags", "add_texture",
        "add_material", "replace_entry_raw", "create_zone",
        "add_zone_dependency",
    };
    return known.count(type) != 0;
}

std::string python_str_value(const json::Value* value) {
    if (!value || value->type == json::Value::Type::Null) return "None";
    if (value->is_str()) return value->str;
    return python_repr(value);
}

bool python_hex_value(const json::Value* value, json::Value& result,
                      std::string& error_type, std::string& error) {
    if (!value || value->type == json::Value::Type::Null ||
        (value->is_str() && value->str.empty())) {
        result = json::make_integer("0");
        return true;
    }
    if (!value->is_str())
        return python_int_value(value, result, error_type, error);

    const std::string& raw = value->str;
    std::string canonical;
    if (!parse_python_integer_string(raw, 16, canonical)) {
        error_type = "ValueError";
        error = "invalid literal for int() with base 16: " +
                python_repr_string(raw);
        return false;
    }
    result = json::make_integer(canonical);
    return true;
}

bool parse_python_float_string(const std::string& raw, double& result) {
    const std::vector<uint32_t> codepoints = utf8_codepoints(raw);
    size_t begin = 0;
    size_t end = codepoints.size();
    while (begin < end && python_numeric_space(codepoints[begin])) ++begin;
    while (end > begin && python_numeric_space(codepoints[end - 1])) --end;
    if (begin == end) return false;

    std::string ascii;
    for (size_t index = begin; index < end; ++index) {
        const uint32_t cp = codepoints[index];
        const int digit = python_decimal_digit(cp);
        if (digit >= 0) {
            ascii.push_back(char('0' + digit));
        } else if (cp == '_') {
            if (index == begin || index + 1 >= end ||
                python_decimal_digit(codepoints[index - 1]) < 0 ||
                python_decimal_digit(codepoints[index + 1]) < 0)
                return false;
            // CPython permits separators only between decimal digits.
        } else if (cp < 0x80) {
            ascii.push_back(char(cp));
        } else {
            return false;
        }
    }
    char* parsed_end = nullptr;
    errno = 0;
    result = std::strtod(ascii.c_str(), &parsed_end);
    return parsed_end && parsed_end != ascii.c_str() && *parsed_end == '\0';
}

bool python_float_value(const json::Value* value, double& result,
                        std::string& error_type, std::string& error) {
    if (!value || value->type == json::Value::Type::Null) {
        error_type = "TypeError";
        error = "float() argument must be a string or a real number, not "
                "'NoneType'";
        return false;
    }
    if (value->type == json::Value::Type::Bool) {
        result = value->b ? 1.0 : 0.0;
        return true;
    }
    if (value->is_num()) {
        result = value->num;
        return true;
    }
    if (value->is_str()) {
        if (parse_python_float_string(value->str, result)) return true;
        error_type = "ValueError";
        error = "could not convert string to float: " +
                python_repr_string(value->str);
        return false;
    }
    error_type = "TypeError";
    error = "float() argument must be a string or a real number, not '" +
            python_type_name(value) + "'";
    return false;
}

bool python_iterable_array(const json::Value* value, json::Value& result,
                           std::string& error_type, std::string& error) {
    result = json::make_arr();
    if (value && value->is_arr()) {
        result = *value;
        return true;
    }
    if (value && value->is_str()) {
        for (char ch : value->str)
            result.arr.push_back(json::make_str(std::string(1, ch)));
        return true;
    }
    if (value && value->is_obj()) {
        for (const auto& item : value->obj)
            result.arr.push_back(json::make_str(item.first));
        return true;
    }
    error_type = "TypeError";
    error = "'" + python_type_name(value) + "' object is not iterable";
    return false;
}

const std::string& exact_integer_text(const json::Value& value) {
    return value.integer_text;
}

bool exact_integer_zero(const json::Value& value) {
    return value.integer_text.empty() ? value.num == 0.0
                                      : value.integer_text == "0";
}

bool exact_integer_negative(const json::Value& value) {
    return !value.integer_text.empty() && value.integer_text[0] == '-';
}

uint64_t exact_integer_modulo(const json::Value& value, uint64_t modulus) {
    if (modulus == 0) return 0;
    if (value.integer_text.empty())
        return uint64_t(static_cast<long long>(value.num)) % modulus;
    const bool negative = exact_integer_negative(value);
    uint64_t remainder = 0;
    for (char ch : value.integer_text) {
        if (ch == '-') continue;
        remainder = (remainder * 10 + unsigned(ch - '0')) % modulus;
    }
    if (negative && remainder) remainder = modulus - remainder;
    return remainder;
}

int compare_exact_integers(const json::Value& left, const json::Value& right) {
    const std::string& a = left.integer_text;
    const std::string& b = right.integer_text;
    const bool an = !a.empty() && a[0] == '-';
    const bool bn = !b.empty() && b[0] == '-';
    if (an != bn) return an ? -1 : 1;
    const size_t ad = a.size() - (an ? 1 : 0);
    const size_t bd = b.size() - (bn ? 1 : 0);
    if (ad != bd) {
        const int magnitude = ad < bd ? -1 : 1;
        return an ? -magnitude : magnitude;
    }
    const int lexical = a.compare(an ? 1 : 0, ad, b, bn ? 1 : 0, bd);
    if (lexical == 0) return 0;
    const int magnitude = lexical < 0 ? -1 : 1;
    return an ? -magnitude : magnitude;
}

long long exact_integer_long_long(const json::Value& value) {
    if (value.integer_text.empty()) return static_cast<long long>(value.num);
    errno = 0;
    const long long parsed = std::strtoll(value.integer_text.c_str(), nullptr, 10);
    if (errno != ERANGE) return parsed;
    return exact_integer_negative(value) ? std::numeric_limits<long long>::min()
                                         : std::numeric_limits<long long>::max();
}

bool python_float_sequence(const json::Value* value, json::Value& result,
                           std::string& error_type, std::string& error) {
    json::Value items;
    if (!python_iterable_array(value, items, error_type, error)) return false;
    result = json::make_arr();
    for (const json::Value& item : items.arr) {
        double converted = 0.0;
        if (!python_float_value(&item, converted, error_type, error))
            return false;
        result.arr.push_back(json::make_num(converted));
    }
    return true;
}

bool python_int_sequence(const json::Value* value, json::Value& result,
                         bool hexadecimal, bool byte_mask,
                         std::string& error_type, std::string& error) {
    json::Value items;
    if (!python_iterable_array(value, items, error_type, error)) return false;
    result = json::make_arr();
    for (const json::Value& item : items.arr) {
        json::Value converted;
        const bool ok = hexadecimal
            ? python_hex_value(&item, converted, error_type, error)
            : python_int_value(&item, converted, error_type, error);
        if (!ok) return false;
        if (byte_mask)
            converted = json::make_integer(
                std::to_string(exact_integer_modulo(converted, 256)));
        result.arr.push_back(std::move(converted));
    }
    return true;
}

bool python_int_mapping(const json::Value* value, json::Value& result,
                        bool string_values, std::string& error_type,
                        std::string& error) {
    result = json::make_obj();
    if (!value || !project_truthy(value)) return true;
    if (!value->is_obj()) {
        error_type = "AttributeError";
        error = "'" + python_type_name(value) +
                "' object has no attribute 'items'";
        return false;
    }
    for (const auto& [key, raw_value] : value->obj) {
        json::Value key_value = json::make_str(key);
        json::Value numeric_key;
        if (!python_int_value(&key_value, numeric_key, error_type, error))
            return false;
        if (string_values) {
            result.obj[exact_integer_text(numeric_key)] =
                json::make_str(python_str_value(&raw_value));
        } else {
            json::Value numeric_value;
            if (!python_int_value(&raw_value, numeric_value, error_type, error))
                return false;
            result.obj[exact_integer_text(numeric_key)] =
                std::move(numeric_value);
        }
    }
    return true;
}

bool normalize_loaded_operation(const json::Value& operation,
                                json::Value& normalized,
                                std::string& error_type,
                                std::string& error) {
    normalized = operation;
    const json::Value* raw_type = operation.find("op");
    const std::string type = raw_type && raw_type->is_str() ? raw_type->str : "";
    if (!known_project_operation(type)) return true;

    json::Value& target = normalized.obj["target"];
    if (!target.is_obj()) target = json::make_obj();
    json::Value& params = normalized.obj["params"];
    if (!params.is_obj()) params = json::make_obj();

    auto hex_field = [&](json::Value& object, const char* field) {
        const json::Value* raw = object.find(field);
        json::Value converted;
        if (!python_hex_value(raw, converted, error_type, error)) return false;
        object.obj[field] = std::move(converted);
        return true;
    };
    auto decimal_field = [&](json::Value& object, const char* field,
                             bool falsey_zero, bool clamp_zero = false) {
        const json::Value* raw = object.find(field);
        const json::Value zero = json::make_num(0);
        if (!raw || (falsey_zero && !project_truthy(raw))) raw = &zero;
        json::Value converted;
        if (!python_int_value(raw, converted, error_type, error)) return false;
        if (clamp_zero && exact_integer_negative(converted))
            converted = json::make_integer("0");
        object.obj[field] = std::move(converted);
        return true;
    };
    auto optional_hex = [&](json::Value& object, const char* field,
                            bool all_falsey) {
        const json::Value* raw = object.find(field);
        const bool absent = !raw || raw->type == json::Value::Type::Null ||
            (all_falsey && !project_truthy(raw)) ||
            (!all_falsey &&
             ((raw->is_str() && raw->str.empty()) ||
              (raw->is_num() && raw->num == 0.0) ||
              (raw->type == json::Value::Type::Bool && !raw->b)));
        if (absent) {
            object.obj[field] = json::Value{};
            return true;
        }
        return hex_field(object, field);
    };
    auto float_sequence = [&](const char* field,
                              std::initializer_list<double> fallback,
                              int required_length = -1,
                              const char* length_prefix = nullptr) {
        const json::Value* raw = params.find(field);
        json::Value fallback_value = json::make_arr();
        if (!raw) {
            for (double item : fallback)
                fallback_value.arr.push_back(json::make_num(item));
            raw = &fallback_value;
        }
        json::Value converted;
        if (!python_float_sequence(raw, converted, error_type, error))
            return false;
        if (required_length >= 0 &&
            int(converted.arr.size()) != required_length) {
            error_type = "ValueError";
            error = std::string(length_prefix) + std::to_string(required_length) +
                    " floats, got " + std::to_string(converted.arr.size());
            return false;
        }
        params.obj[field] = std::move(converted);
        return true;
    };

    const std::map<std::string, std::vector<const char*>> target_keys = {
        {"replace_texture", {"entry_key", "sub_key"}},
        {"replace_mesh", {"entry_key", "sub_key"}},
        {"stub_mesh", {"entry_key", "sub_key"}},
        {"replace_animation", {"entry_key", "sub_key"}},
        {"edit_light", {"entry_key", "light_key", "gao_key"}},
        {"modify_transform", {"entry_key", "gao_key"}},
        {"add_object_collision", {"entry_key", "gao_key"}},
        {"modify_gao_flags", {"entry_key", "gao_key"}},
        {"modify_gao_material", {"entry_key", "gao_key"}},
        {"add_object", {"entry_key"}},
        {"retarget_material_texture", {"entry_key", "sub_key"}},
        {"set_multi_material_slot", {"entry_key", "multi_key"}},
        {"set_element_material", {"entry_key", "geo_key", "multi_key"}},
        {"set_material_flags", {"entry_key", "sub_key"}},
        {"add_texture", {"entry_key"}},
        {"add_material", {"entry_key"}},
        {"replace_entry_raw", {"entry_key"}},
        {"add_zone_dependency", {"wol_entry_key"}},
    };
    auto key_set = target_keys.find(type);
    if (key_set != target_keys.end())
        for (const char* field : key_set->second)
            if (!hex_field(target, field)) return false;

    const std::map<std::string, std::vector<const char*>> param_keys = {
        {"modify_gao_flags", {"set_mask", "clear_mask"}},
        {"modify_gao_material", {"material_key"}},
        {"retarget_material_texture", {"new_texture_key"}},
        {"set_multi_material_slot", {"new_material_key"}},
        {"set_element_material", {"new_material_key"}},
        {"set_material_flags", {"ul_flags"}},
        {"add_texture", {"new_key"}},
        {"add_material", {"new_key", "texture_key", "donor_key"}},
    };
    key_set = param_keys.find(type);
    if (key_set != param_keys.end())
        for (const char* field : key_set->second)
            if (!hex_field(params, field)) return false;

    if (type == "replace_mesh") {
        for (const auto& spec : {std::pair{"bone_map", false},
                                 std::pair{"bone_map_source", true},
                                 std::pair{"drop_targets", false}}) {
            json::Value converted;
            if (!python_int_mapping(params.find(spec.first), converted,
                                    spec.second, error_type, error))
                return false;
            params.obj[spec.first] = std::move(converted);
        }
        const json::Value* drops = params.find("bone_drops");
        json::Value empty = json::make_arr();
        if (!drops || !project_truthy(drops)) drops = &empty;
        json::Value converted_drops;
        if (!python_int_sequence(drops, converted_drops, false, false,
                                 error_type, error))
            return false;
        params.obj["bone_drops"] = std::move(converted_drops);
        const json::Value* rigid = params.find("rigid_bind_bone");
        if (rigid && rigid->type != json::Value::Type::Null &&
            !decimal_field(params, "rigid_bind_bone", false))
            return false;
    } else if (type == "edit_light") {
        const json::Value* light_type = params.find("light_type");
        if (light_type && light_type->type != json::Value::Type::Null &&
            !decimal_field(params, "light_type", false))
            return false;
        for (const char* field : {"diffuse", "specular"}) {
            const json::Value* raw = params.find(field);
            if (!raw || raw->type == json::Value::Type::Null) continue;
            json::Value converted;
            if (!python_int_sequence(raw, converted, false, true,
                                     error_type, error))
                return false;
            params.obj[field] = std::move(converted);
        }
        for (const char* field : {"near", "far", "inner_angle",
                                  "outer_angle", "intensity"}) {
            const json::Value* raw = params.find(field);
            if (!raw || raw->type == json::Value::Type::Null) continue;
            double converted = 0.0;
            if (!python_float_value(raw, converted, error_type, error))
                return false;
            params.obj[field] = json::make_num(converted);
        }
    } else if (type == "modify_transform") {
        if (!float_sequence("position", {0.0, 0.0, 0.0}) ||
            !float_sequence("rotation_euler_deg", {0.0, 0.0, 0.0}) ||
            !float_sequence("scale", {1.0, 1.0, 1.0}))
            return false;
        const json::Value* world = params.find("world_matrix");
        if (world && world->type != json::Value::Type::Null &&
            !float_sequence("world_matrix", {}, 16,
                            "modify_transform.world_matrix must be "))
            return false;
        const json::Value* follow = params.find("collision_follow");
        if (follow && project_truthy(follow) && !follow->is_obj()) {
            if (follow->is_arr()) {
                error_type = "TypeError";
                error = "cannot convert dictionary update sequence element #0 "
                        "to a sequence";
            } else if (follow->is_str()) {
                error_type = "ValueError";
                error = "dictionary update sequence element #0 has length 1; "
                        "2 is required";
            } else {
                error_type = "TypeError";
                error = "'" + python_type_name(follow) +
                        "' object is not iterable";
            }
            return false;
        }
        if (!follow || !project_truthy(follow))
            params.obj["collision_follow"] = json::Value{};
    } else if (type == "add_object_collision") {
        const json::Value* profile = params.find("collision_profile");
        if (!profile || !project_truthy(profile))
            params.obj["collision_profile"] = json::make_str("simple_box");
        const json::Value* shape = params.find("collision_shape");
        if (!shape || !project_truthy(shape)) {
            params.obj["collision_shape"] = json::make_str("mesh");
        } else if (!shape->is_str()) {
            error_type = "AttributeError";
            error = "'" + python_type_name(shape) +
                    "' object has no attribute 'lower'";
            return false;
        } else {
            std::string lowered = shape->str;
            std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                           [](unsigned char ch) {
                               return char(std::tolower(ch));
                           });
            params.obj["collision_shape"] = json::make_str(lowered);
        }
        const json::Value* room = params.find("room_cob_key");
        if (room && room->type != json::Value::Type::Null &&
            !hex_field(params, "room_cob_key"))
            return false;
    } else if (type == "add_object") {
        const json::Value* raw_kind = params.find("kind");
        const json::Value default_kind = json::make_str("cube");
        const std::string kind = python_str_value(
            raw_kind ? raw_kind : &default_kind);
        std::string lowered = kind;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });
        static const std::unordered_set<std::string> kinds = {
            "clone", "model", "cube", "sphere", "cylinder"};
        if (!kinds.count(lowered)) {
            error_type = "ValueError";
            error = "AddObject: unknown kind " + python_repr_string(lowered) +
                    "; expected one of ('clone', 'model', 'cube', 'sphere', "
                    "'cylinder')";
            return false;
        }
        params.obj["kind"] = json::make_str(lowered);
        const json::Value* name = params.find("name");
        if (!name || !project_truthy(name))
            params.obj["name"] = json::make_str("JadePlaced_" + lowered);
        if (!float_sequence("position", {0.0, 0.0, 0.0}) ||
            !float_sequence("rotation_euler_deg", {0.0, 0.0, 0.0}) ||
            !float_sequence("scale_xform", {1.0, 1.0, 1.0}) ||
            !float_sequence("size", {1.0, 1.0, 1.0}))
            return false;
        const json::Value* world = params.find("world_matrix");
        if (world && world->type != json::Value::Type::Null &&
            !float_sequence("world_matrix", {}, 16,
                            "AddObject.world_matrix must be "))
            return false;
        for (const char* field : {"material_key", "source_gao_key",
                                  "source_entry_key", "room_cob_key"})
            if (!optional_hex(params, field, false)) return false;
        const json::Value* profile = params.find("collision_profile");
        std::string profile_text = python_str_value(
            profile && project_truthy(profile)
                ? profile
                : &params.obj["collision_profile"]);
        if (!profile || !project_truthy(profile)) profile_text = "simple_box";
        std::transform(profile_text.begin(), profile_text.end(),
                       profile_text.begin(), [](unsigned char ch) {
                           return char(std::tolower(ch));
                       });
        params.obj["collision_profile"] = json::make_str(profile_text);
        for (const char* field : {"source", "model_path"}) {
            const json::Value* raw = params.find(field);
            if (!raw || !project_truthy(raw))
                params.obj[field] = json::make_str("");
        }
        const json::Value* color = params.find("vertex_color");
        if (color && color->type != json::Value::Type::Null) {
            json::Value converted;
            if (!python_int_sequence(color, converted, false, true,
                                     error_type, error))
                return false;
            if (converted.arr.size() != 3 && converted.arr.size() != 4) {
                error_type = "ValueError";
                error = "AddObject.vertex_color must be 3 or 4 bytes, got " +
                        std::to_string(converted.arr.size());
                return false;
            }
            if (converted.arr.size() == 3)
                converted.arr.push_back(json::make_num(255));
            params.obj["vertex_color"] = std::move(converted);
        }
    } else if (type == "retarget_material_texture" ||
               type == "set_material_flags") {
        if (!decimal_field(params, "layer", true, true)) return false;
    } else if (type == "set_multi_material_slot") {
        if (!decimal_field(params, "slot", false)) return false;
    } else if (type == "set_element_material") {
        if (!optional_hex(target, "container_entry_key", true) ||
            !decimal_field(params, "element", false))
            return false;
    } else if (type == "add_texture") {
        const json::Value* encode = params.find("encode");
        const json::Value default_encode = json::make_str("7");
        params.obj["encode"] = json::make_str(
            python_str_value(encode ? encode : &default_encode));
    } else if (type == "create_zone") {
        const json::Value empty_string = json::make_str("");
        for (const char* field : {"template_zone", "new_name"}) {
            const json::Value* raw = params.find(field);
            params.obj[field] = json::make_str(
                python_str_value(raw ? raw : &empty_string));
        }
        if (!hex_field(params, "new_map_prefix")) return false;
        if (!project_truthy(params.find("new_map_prefix")))
            params.obj["new_map_prefix"] = json::make_num(0x27);
        const json::Value* target_dir = params.find("target_dir_idx");
        if (target_dir && target_dir->type != json::Value::Type::Null &&
            !decimal_field(params, "target_dir_idx", false))
            return false;
        const json::Value* deps = params.find("extra_deps");
        json::Value fallback = json::make_arr();
        if (!deps) deps = &fallback;
        json::Value converted;
        if (!python_int_sequence(deps, converted, true, false,
                                 error_type, error))
            return false;
        params.obj["extra_deps"] = std::move(converted);
    } else if (type == "add_zone_dependency") {
        const json::Value* deps = params.find("dep_keys");
        json::Value fallback = json::make_arr();
        if (!deps) deps = &fallback;
        json::Value converted;
        if (!python_int_sequence(deps, converted, true, false,
                                 error_type, error))
            return false;
        params.obj["dep_keys"] = std::move(converted);
    }
    return true;
}
}  // namespace

ModProject load_project(const std::string& jmod_dir) {
    ModProject p;
    p.error_type = "ProjectFormatError";
    const std::filesystem::path native_path =
        std::filesystem::u8path(jmod_dir) / "project.json";
    const std::string path = native_path.u8string();
    std::ifstream f(native_path, std::ios::binary);
    if (!f) {
        p.error = "no project.json in " + python_repr_string(jmod_dir);
        return p;
    }
    const std::string raw_text((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    const std::string text = python_text_newlines(raw_text);
    json::Value doc;
    try {
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xef &&
            static_cast<unsigned char>(text[1]) == 0xbb &&
            static_cast<unsigned char>(text[2]) == 0xbf) {
            throw json::ParseError(
                "Unexpected UTF-8 BOM (decode using utf-8-sig)", 0);
        }
        doc = json::parse_strict(text);
    } catch (const json::ParseError& e) {
        p.error = "failed to read " + python_repr_string(path) + ": " +
                  python_json_error(text, e);
        return p;
    } catch (const std::exception& e) {
        p.error = "failed to read " + python_repr_string(path) + ": " +
                  e.what();
        return p;
    }
    if (!doc.is_obj()) {
        p.error = "project.json: top-level must be an object";
        return p;
    }
    const json::Value* tag = doc.find("format");
    if (!tag || !tag->is_str() || tag->str != "jade-mod-project") {
        p.error = "project.json: format tag is " + python_repr(tag) +
                  ", expected 'jade-mod-project'";
        return p;
    }
    const json::Value* version = doc.find("format_version");
    const bool version_is_int =
        version &&
        (version->type == json::Value::Type::Bool ||
         (version->is_num() && version->number_is_integer));
    if (version_is_int && version->is_num() &&
        version->num > double(std::numeric_limits<long long>::max())) {
        p.error = "project.json: format_version " + python_repr(version) +
                  " is newer than this toolkit understands (1). Update the toolkit.";
        return p;
    }
    if (version_is_int && version->is_num() &&
        version->num < double(std::numeric_limits<long long>::min())) {
        p.error = "project.json: format_version is " + python_repr(version) +
                  ", must be a positive int";
        return p;
    }
    const long long fv =
        !version ? 0
                 : version->type == json::Value::Type::Bool
                       ? (version->b ? 1 : 0)
                       : static_cast<long long>(version->num);
    if (!version_is_int || fv < 1) {
        p.error = "project.json: format_version is " + python_repr(version) +
                  ", must be a positive int";
        return p;
    }
    if (fv > 1) {
        p.error = "project.json: format_version " + std::to_string(fv) +
                  " is newer than this toolkit understands (1). Update the toolkit.";
        return p;
    }
    json::Value normalized = json::make_obj();
    normalized.obj["format"] = json::make_str("jade-mod-project");
    normalized.obj["format_version"] = json::make_num(1);

    auto raw_or = [&](const char* key, json::Value fallback) {
        const json::Value* value = doc.find(key);
        return value ? *value : std::move(fallback);
    };
    normalized.obj["name"] = raw_or("name", json::make_str(""));
    normalized.obj["author"] = raw_or("author", json::make_str(""));
    normalized.obj["description"] =
        raw_or("description", json::make_str(""));
    normalized.obj["created"] =
        raw_or("created", json::make_str(project_iso_now()));
    normalized.obj["modified"] =
        raw_or("modified", normalized.obj["created"]);

    auto typed_string = [](const json::Value& value) {
        return value.is_str() ? value.str : std::string();
    };
    p.name = typed_string(normalized.obj["name"]);
    p.author = typed_string(normalized.obj["author"]);
    p.description = typed_string(normalized.obj["description"]);
    p.created = typed_string(normalized.obj["created"]);
    p.modified = typed_string(normalized.obj["modified"]);

    const json::Value empty_object = json::make_obj();
    const json::Value* base = doc.find("base");
    if (!base) base = &empty_object;
    if (!base->is_obj()) {
        set_python_load_error(
            p, "AttributeError", "'" + python_type_name(base) +
                                     "' object has no attribute 'get'");
        return p;
    }
    json::Value normalized_base = json::make_obj();
    normalized_base.obj["game"] =
        base->find("game") ? *base->find("game") : json::make_str("");
    normalized_base.obj["archive_name"] =
        base->find("archive_name") ? *base->find("archive_name")
                                   : json::make_str("");
    normalized_base.obj["archive_sha256"] =
        base->find("archive_sha256") ? *base->find("archive_sha256")
                                     : json::make_str("");
    const json::Value default_zero = json::make_num(0);
    const json::Value* raw_archive_size = base->find("archive_size");
    if (!raw_archive_size) raw_archive_size = &default_zero;
    json::Value archive_size;
    std::string conversion_type, conversion_error;
    if (!python_int_value(raw_archive_size, archive_size, conversion_type,
                          conversion_error)) {
        set_python_load_error(p, conversion_type, conversion_error);
        return p;
    }
    normalized_base.obj["archive_size"] = archive_size;
    normalized.obj["base"] = std::move(normalized_base);
    p.base.game = typed_string(normalized.obj["base"].obj["game"]);
    p.base.archive_name =
        typed_string(normalized.obj["base"].obj["archive_name"]);
    p.base.archive_sha256 =
        typed_string(normalized.obj["base"].obj["archive_sha256"]);
    const long long typed_archive_size = exact_integer_long_long(archive_size);
    p.base.archive_size = typed_archive_size < 0
                              ? 0
                              : uint64_t(typed_archive_size);

    const json::Value* build = doc.find("build");
    if (!build || !project_truthy(build)) build = &empty_object;
    if (!build->is_obj()) {
        set_python_load_error(
            p, "AttributeError", "'" + python_type_name(build) +
                                     "' object has no attribute 'get'");
        return p;
    }
    json::Value normalized_build = json::make_obj();
    const json::Value* output_name = build->find("output_name");
    normalized_build.obj["output_name"] =
        output_name ? *output_name : json::make_str("");
    const json::Value* strict = build->find("strict_inplace");
    normalized_build.obj["strict_inplace"] =
        json::make_bool(strict == nullptr ? true : project_truthy(strict));
    normalized.obj["build"] = std::move(normalized_build);
    p.build.output_name =
        typed_string(normalized.obj["build"].obj["output_name"]);
    p.build.strict_inplace =
        normalized.obj["build"].obj["strict_inplace"].b;

    p.jmod_dir = jmod_dir;
    const json::Value default_one = json::make_num(1);
    const json::Value* raw_serial = doc.find("next_op_serial");
    if (!raw_serial) raw_serial = &default_one;
    json::Value serial;
    if (!python_int_value(raw_serial, serial, conversion_type,
                          conversion_error)) {
        set_python_load_error(p, conversion_type, conversion_error);
        return p;
    }
    normalized.obj["next_op_serial"] = serial;
    const long long typed_serial = exact_integer_long_long(serial);
    if (typed_serial > std::numeric_limits<int>::max())
        p.next_op_serial = std::numeric_limits<int>::max();
    else if (typed_serial < std::numeric_limits<int>::min())
        p.next_op_serial = std::numeric_limits<int>::min();
    else
        p.next_op_serial = int(typed_serial);

    json::Value normalized_operations = json::make_arr();
    const json::Value* ops = doc.find("operations");
    if (!ops || !project_truthy(ops)) {
        ops = nullptr;
    } else if (!ops->is_arr()) {
        if (ops->is_str() || ops->is_obj()) {
            set_python_load_error(
                p, "AttributeError",
                "'str' object has no attribute 'get'");
        } else {
            set_python_load_error(
                p, "TypeError", "'" + python_type_name(ops) +
                                    "' object is not iterable");
        }
        return p;
    }
    if (ops) {
        p.operations.reserve(ops->arr.size());
        normalized_operations.arr.reserve(ops->arr.size());
        for (const json::Value& operation : ops->arr) {
            if (!operation.is_obj()) {
                set_python_load_error(
                    p, "AttributeError", "'" + python_type_name(&operation) +
                                             "' object has no attribute 'get'");
                return p;
            }
            const json::Value* raw_type = operation.find("op");
            if (raw_type && (raw_type->is_arr() || raw_type->is_obj())) {
                set_python_load_error(
                    p, "TypeError", "unhashable type: '" +
                                        python_type_name(raw_type) + "'");
                return p;
            }
            const std::string operation_type =
                raw_type && raw_type->is_str() ? raw_type->str : "";
            if (known_project_operation(operation_type)) {
                auto require_mapping_if_truthy = [&](const char* field) {
                    const json::Value* value = operation.find(field);
                    if (!value || !project_truthy(value) || value->is_obj())
                        return true;
                    set_python_load_error(
                        p, "AttributeError", "'" + python_type_name(value) +
                                                 "' object has no attribute 'get'");
                    return false;
                };
                if (operation_type != "create_zone" &&
                    !require_mapping_if_truthy("target"))
                    return p;
                if (operation_type != "stub_mesh" &&
                    !require_mapping_if_truthy("params"))
                    return p;
            }
            json::Value normalized_operation;
            if (!normalize_loaded_operation(operation, normalized_operation,
                                            conversion_type,
                                            conversion_error)) {
                set_python_load_error(p, conversion_type, conversion_error);
                return p;
            }
            json::Value converted = operation_to_dict(normalized_operation);
            p.operations.push_back(converted);
            normalized_operations.arr.push_back(std::move(converted));
        }
    }
    normalized.obj["operations"] = std::move(normalized_operations);
    p.loaded_dict = std::move(normalized);
    p.error_type.clear();
    p.ok = true;
    return p;
}

std::vector<const json::Value*> ModProject::enabled_operations() const {
    std::vector<const json::Value*> out;
    for (const json::Value& op : operations) {
        const json::Value* en = op.find("enabled");
        if (en != nullptr && en->type == json::Value::Type::Bool && !en->b)
            continue;
        out.push_back(&op);
    }
    return out;
}

// ── EntrySet (build/entryset.py) ───────────────────────────────────────────

namespace {

struct NewEntry {
    std::string name;
    uint32_t parent_dir_idx = 0;
    std::vector<uint8_t> data;
};

struct EntrySet {
    BigFile& bf;
    std::map<uint32_t, std::vector<uint8_t>> cache;   // key -> dec (ordered)
    std::unordered_set<uint32_t> modified;
    std::unordered_map<uint32_t, BFFile*> by_key;
    // New FAT entries staged by ops (CreateZone). Key-ordered; the Python
    // dict is insertion-ordered, but CreateZone allocates ascending low16
    // pairs so the orders coincide.
    std::map<uint32_t, NewEntry> new_entries;

    // add_new_entry: throws like the Python on dup / already-in-FAT keys.
    void add_new_entry(uint32_t key, const std::string& name,
                       uint32_t parent_dir_idx, std::vector<uint8_t> data) {
        char b[64];
        if (new_entries.count(key)) {
            std::snprintf(b, sizeof b, "new entry 0x%08x already staged", key);
            throw std::runtime_error(b);
        }
        if (file_for(key) != nullptr) {
            std::snprintf(b, sizeof b,
                          "key 0x%08x already exists in the BigFile FAT", key);
            throw std::runtime_error(b);
        }
        new_entries[key] = {name, parent_dir_idx, std::move(data)};
    }

    explicit EntrySet(BigFile& b) : bf(b) {
        for (auto& kv : bf.files)
            if (kv.second.key != INVALID_KEY) by_key[kv.second.key] = &kv.second;
    }
    BFFile* file_for(uint32_t key) {
        auto it = by_key.find(key);
        return it == by_key.end() ? nullptr : it->second;
    }
    // Throws std::runtime_error like Python's EntryNotFoundError/ValueError.
    std::vector<uint8_t>& get(uint32_t key) {
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
        BFFile* fi = file_for(key);
        if (fi == nullptr) {
            char b[48];
            std::snprintf(b, sizeof b, "entry 0x%08x not in archive", key);
            throw std::runtime_error(b);
        }
        LzoResult r = decompress_lzo(bf.read_data(fi->index));
        if (!r.ok) {
            char b[56];
            std::snprintf(b, sizeof b, "entry 0x%08x: decompression failed", key);
            throw std::runtime_error(b);
        }
        return cache.emplace(key, std::move(r.data)).first->second;
    }
};

// hex-string or number -> u32 (operations.py _hex_to_int).
uint32_t key_of(const json::Value* v) {
    if (v == nullptr) return 0;
    if (v->is_num()) return uint32_t(v->num);
    if (v->is_str() && !v->str.empty())
        return uint32_t(std::strtoul(v->str.c_str(), nullptr, 16));
    return 0;
}

std::string op_id_of(const json::Value& op) {
    const json::Value* v = op.find("id");
    return v && v->is_str() ? v->str : "";
}

struct BuildState {
    std::vector<BuildIssue>& issues;
    std::vector<std::string>& log;
    void error(const json::Value* op, const std::string& m) {
        issues.push_back({"error", m, op ? op_id_of(*op) : ""});
    }
    void warning(const json::Value* op, const std::string& m) {
        issues.push_back({"warning", m, op ? op_id_of(*op) : ""});
    }
};

// ── typed operation appliers ──

// ops_mesh._STUB_TEMPLATE_HEX: a null GEO (version 7, nb_uvs... all zero) —
// NOT mesh_swap's 48-byte null-resource template (version 8 GAO-shaped).
const uint8_t kStub48[48] = {
    0x07, 0x00, 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

void apply_modify_transform(const json::Value& op, EntrySet& es, BuildState& st) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t gao_key = key_of(tgt ? tgt->find("gao_key") : nullptr);
    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = nullptr;
    for (const SubEntry& s : subs)
        if (s.key == gao_key && s.ext == ".gao") { target = &s; break; }
    if (target == nullptr) {
        char b[96];
        std::snprintf(b, sizeof b,
                      "modify_transform: GAO 0x%08x not found in entry 0x%08x",
                      gao_key, entry_key);
        throw std::runtime_error(b);
    }
    long long mat_off = global_matrix_offset(target->data.data(), target->data.size());
    if (mat_off < 0)
        throw std::runtime_error(
            "modify_transform: GAO has no parseable global_matrix");
    size_t dec_off = target->offset + 12 + size_t(mat_off);

    auto put_f32 = [&](size_t o, double v) {
        float fv = float(v);
        std::memcpy(dec.data() + o, &fv, 4);
    };
    collision_follow::JadeMatrix m_old{};
    for (size_t i = 0; i < 16; ++i) {
        float value;
        std::memcpy(&value, dec.data() + dec_off + i * 4, 4);
        m_old[i] = double(value);
    }
    collision_follow::JadeMatrix m_new{};
    const json::Value* wm = prm ? prm->find("world_matrix") : nullptr;
    if (wm != nullptr && wm->is_arr() && wm->arr.size() == 16) {
        placer::Mat16 m{};
        for (size_t i = 0; i < 16; ++i) {
            m[i] = wm->arr[i].num;
            m_new[i] = m[i];
            put_f32(dec_off + i * 4, m[i]);
        }
        uint32_t ltype = placer::ltype_from_jade_matrix(m);
        dec[dec_off + 64] = uint8_t(ltype);
        dec[dec_off + 65] = uint8_t(ltype >> 8);
        dec[dec_off + 66] = uint8_t(ltype >> 16);
        dec[dec_off + 67] = uint8_t(ltype >> 24);
    } else {
        auto vec3_of = [&](const char* k, double dflt) {
            placer::Vec3 v{dflt, dflt, dflt};
            const json::Value* a = prm ? prm->find(k) : nullptr;
            if (a && a->is_arr())
                for (size_t i = 0; i < 3 && i < a->arr.size(); ++i)
                    v[i] = a->arr[i].num;
            return v;
        };
        placer::Mat16 delta = placer::trs_to_matrix(
            vec3_of("position", 0.0), vec3_of("rotation_euler_deg", 0.0),
            vec3_of("scale", 1.0));
        placer::Mat16 nv = placer::col_major_mul(delta, m_old);
        for (size_t i = 0; i < 16; ++i) {
            m_new[i] = nv[i];
            put_f32(dec_off + i * 4, nv[i]);
        }
    }

    const json::Value* collision = prm ? prm->find("collision_follow") : nullptr;
    if (collision && collision->is_obj()) {
        auto cf_key = [](const json::Value* value) -> uint32_t {
            if (!value) return 0;
            if (value->is_num()) return uint32_t(value->num);
            if (!value->is_str()) return 0;
            const std::string& text = value->str;
            const int base = text.size() > 2 && text[0] == '0'
                && (text[1] == 'x' || text[1] == 'X') ? 16 : 10;
            return uint32_t(std::strtoul(text.c_str(), nullptr, base));
        };
        collision_follow::CollisionLinks links;
        links.moved_gao_key = gao_key;
        const json::Value* same = collision->find("same_gao");
        links.same_gao = same && same->type == json::Value::Type::Bool && same->b;
        const json::Value* dedicated = collision->find("dedicated");
        if (dedicated && dedicated->is_arr())
            for (const json::Value& value : dedicated->arr) {
                if (!value.is_obj()) continue;
                collision_follow::DedicatedLink link;
                link.gao_key = cf_key(value.find("gao_key"));
                const json::Value* name = value.find("name");
                if (name && name->is_str()) link.name = name->str;
                const json::Value* keys = value.find("cob_keys");
                if (keys && keys->is_arr())
                    for (const json::Value& key : keys->arr)
                        link.cob_keys.push_back(cf_key(&key));
                links.dedicated.push_back(std::move(link));
            }
        const json::Value* carves = collision->find("carves");
        if (carves && carves->is_arr())
            for (const json::Value& value : carves->arr) {
                if (!value.is_obj()) continue;
                collision_follow::CarveLink link;
                link.cob_key = cf_key(value.find("cob_key"));
                link.owner_gao_key = cf_key(value.find("owner_gao_key"));
                const json::Value* name = value.find("owner_name");
                if (name && name->is_str()) link.owner_name = name->str;
                const json::Value* faces = value.find("face_indices");
                if (faces && faces->is_arr())
                    for (const json::Value& face : faces->arr)
                        if (face.is_num() && face.num >= 0
                            && face.num <= std::numeric_limits<uint32_t>::max())
                            link.face_indices.push_back(uint32_t(face.num));
                const json::Value* total = value.find("n_total_faces");
                if (total && total->is_num() && total->num >= 0)
                    link.n_total_faces = uint32_t(total->num);
                links.carves.push_back(std::move(link));
            }
        dec = collision_follow::apply_collision_follow(
            dec, links, m_old, m_new,
            [&](const std::string& message) { st.log.push_back(message); });
    }
    es.modified.insert(entry_key);
}

void apply_stub_mesh(const json::Value& op, EntrySet& es, BuildState& st,
                     std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t sub_key = key_of(tgt ? tgt->find("sub_key") : nullptr);
    std::vector<uint8_t>& dec = es.get(entry_key);

    // find_sub_entry_bounds: payload spans cookie+12 to the NEXT cookie-4
    // (padding included), first key match wins.
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    long long start = -1, end = -1;
    for (size_t i = 0; i < subs.size(); ++i)
        if (subs[i].key == sub_key) {
            start = static_cast<long long>(subs[i].offset) + 12;
            end = (i + 1 < subs.size())
                      ? static_cast<long long>(subs[i + 1].offset) - 4
                      : static_cast<long long>(dec.size());
            break;
        }
    char b[96];
    if (start < 0) {
        std::snprintf(b, sizeof b, "[%s] stub_mesh: sub 0x%08x not found",
                      op_id_of(op).c_str(), sub_key);
        log.push_back(b);
        return;
    }
    size_t old_size = size_t(end - start);
    if (sizeof kStub48 > old_size) {
        std::snprintf(b, sizeof b,
                      "[%s] stub_mesh: template too large (48B) for slot (%zuB); "
                      "skipped", op_id_of(op).c_str(), old_size);
        log.push_back(b);
        return;
    }
    std::memcpy(dec.data() + start, kStub48, sizeof kStub48);
    std::memset(dec.data() + start + sizeof kStub48, 0,
                old_size - sizeof kStub48);
    es.modified.insert(entry_key);
    (void)st;
}

void apply_edit_light(const json::Value& op, EntrySet& es) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t light_key = key_of(tgt ? tgt->find("light_key") : nullptr);
    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = nullptr;
    for (const SubEntry& s : subs)
        if (s.key == light_key && !s.gro_null && s.gro_type == GRO_LIGHT) {
            target = &s;
            break;
        }
    if (target == nullptr) {
        char b[96];
        std::snprintf(b, sizeof b,
                      "edit_light: light 0x%08x (gro_type 2) not found in "
                      "entry 0x%08x", light_key, entry_key);
        throw std::runtime_error(b);
    }
    LightEdit ed;
    if (prm) {
        auto fnum = [&](const char* k) -> std::optional<float> {
            const json::Value* v = prm->find(k);
            if (v && v->is_num()) return float(v->num);
            return std::nullopt;
        };
        auto rgb = [&](const char* k) -> std::optional<std::array<int, 3>> {
            const json::Value* v = prm->find(k);
            if (v && v->is_arr() && v->arr.size() >= 3)
                return std::array<int, 3>{int(v->arr[0].num), int(v->arr[1].num),
                                          int(v->arr[2].num)};
            return std::nullopt;
        };
        const json::Value* lt = prm->find("light_type");
        if (lt && lt->is_num()) ed.light_type = uint32_t(lt->num) & 0x7;
        ed.diffuse = rgb("diffuse");
        ed.specular = rgb("specular");
        ed.near_ = fnum("near");
        ed.far_ = fnum("far");
        ed.inner = fnum("inner_angle");
        ed.outer = fnum("outer_angle");
        ed.intensity = fnum("intensity");
    }
    std::vector<uint8_t> nw = write_light_fields(target->data.data(),
                                                 target->data.size(), ed);
    if (nw.empty() || nw.size() != target->data.size())
        throw std::runtime_error("edit_light: payload patch failed");
    std::memcpy(dec.data() + target->offset + 12, nw.data(), nw.size());
    es.modified.insert(entry_key);
}

// ── remove_object (NATIVE-FIRST, no Python counterpart) ─────────────────────
// Hide a shipped object without structural changes — all edits are same-size
// in-place (jtmod-friendly, trivially reversible by disabling the op).
//   mode "hide":              visual gro_key -> INVALID (mesh not drawn).
//   mode "hide_no_collision": also sever every ColMap ref in the GAO body
//                             (unaligned scan) so the ghost doesn't block.
void apply_remove_object(const json::Value& op, EntrySet& es,
                         std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t gao_key = key_of(tgt ? tgt->find("gao_key") : nullptr);
    std::string mode = "hide";
    if (prm) {
        const json::Value* m = prm->find("mode");
        if (m && m->is_str()) mode = m->str;
    }
    if (mode != "hide" && mode != "hide_no_collision")
        throw std::runtime_error("remove_object: unknown mode '" + mode + "'");

    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;
    const SubEntry* target = nullptr;
    for (const SubEntry& s : subs)
        if (s.key == gao_key && s.ext == ".gao") { target = &s; break; }
    if (target == nullptr) {
        char b[96];
        std::snprintf(b, sizeof b,
                      "remove_object: GAO 0x%08x not found in entry 0x%08x",
                      gao_key, entry_key);
        throw std::runtime_error(b);
    }

    long long vis_off = visual_block_offset(target->data.data(),
                                            target->data.size());
    bool hid_visual = false;
    if (vis_off >= 0 && size_t(vis_off) + 4 <= target->data.size()) {
        size_t abs = target->offset + 12 + size_t(vis_off);
        dec[abs] = 0xFF; dec[abs + 1] = 0xFF; dec[abs + 2] = 0xFF; dec[abs + 3] = 0xFF;
        hid_visual = true;
    }

    int severed = 0;
    if (mode == "hide_no_collision") {
        for (const auto& hit : jade::gao_colmap_key_offsets(target->data, by_key)) {
            size_t abs = target->offset + 12 + hit.first;
            if (abs + 4 > dec.size()) continue;
            dec[abs] = 0xFF; dec[abs + 1] = 0xFF; dec[abs + 2] = 0xFF; dec[abs + 3] = 0xFF;
            ++severed;
        }
    }
    if (!hid_visual && severed == 0)
        throw std::runtime_error(
            "remove_object: GAO has no visual block and no ColMap refs — "
            "nothing to remove");
    es.modified.insert(entry_key);
    char b[128];
    std::snprintf(b, sizeof b,
                  "[%s] remove_object 0x%08x: visual %s, %d ColMap ref(s) severed",
                  op_id_of(op).c_str(), gao_key, hid_visual ? "hidden" : "absent",
                  severed);
    log.push_back(b);
}

// modify_gao_flags: new = (old & ~clear) | set on the GAO identity @+8.
void apply_modify_gao_flags(const json::Value& op, EntrySet& es,
                            std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t gao_key = key_of(tgt ? tgt->find("gao_key") : nullptr);
    uint32_t set_mask = key_of(prm ? prm->find("set_mask") : nullptr);
    uint32_t clear_mask = key_of(prm ? prm->find("clear_mask") : nullptr);

    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = nullptr;
    for (const SubEntry& s : subs)
        if (s.key == gao_key && s.ext == ".gao") { target = &s; break; }
    if (target == nullptr) {
        char b[96];
        std::snprintf(b, sizeof b,
                      "modify_gao_flags: GAO 0x%08x not found in entry 0x%08x",
                      gao_key, entry_key);
        throw std::runtime_error(b);
    }
    size_t identity_off = target->offset + 12 + 8;
    uint32_t old = uint32_t(dec[identity_off]) |
                   (uint32_t(dec[identity_off + 1]) << 8) |
                   (uint32_t(dec[identity_off + 2]) << 16) |
                   (uint32_t(dec[identity_off + 3]) << 24);
    uint32_t nw = (old & ~clear_mask) | set_mask;
    char b[128];
    if (nw == old) {
        std::snprintf(b, sizeof b,
                      "[%s] modify_gao_flags noop: GAO 0x%08x identity already "
                      "0x%08x", op_id_of(op).c_str(), gao_key, old);
        log.push_back(b);
        return;
    }
    dec[identity_off] = uint8_t(nw);
    dec[identity_off + 1] = uint8_t(nw >> 8);
    dec[identity_off + 2] = uint8_t(nw >> 16);
    dec[identity_off + 3] = uint8_t(nw >> 24);
    es.modified.insert(entry_key);
    std::snprintf(b, sizeof b,
                  "[%s] modify_gao_flags GAO 0x%08x: 0x%08x -> 0x%08x",
                  op_id_of(op).c_str(), gao_key, old, nw);
    log.push_back(b);
}

// add_object: the placement op (primitives + intra/cross-bin clones) through
// the fully-ported placer core. Both builders seed the key allocator with
// crc32(op.id), avoiding Python's process-randomized hash(str).
// The op-JSON -> PlaceOp conversion, shared by apply_add_object and the
// pre-build validator's LOA simulation (ops_object._to_placement_dict +
// the cross-bin chain pre-collection). Throws on structural problems.
struct PlacePrep {
    placer::PlaceOp po;
    uint32_t entry_key = 0;
    uint32_t seed = 0;
    bool cross_bin = false;
    size_t source_chain_size = 0;
    std::string source_entry_name;
};

// (defined in the asset-store section below)
std::string resolve_asset(const std::string& jmod_dir, const std::string& ref);

PlacePrep build_place_op(const json::Value& op, EntrySet& es,
                         const std::string& jmod_dir,
                         bool validation_simulation = false) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);

    placer::PlaceOp po;
    bool cross_bin = false;
    std::string source_entry_name;
    auto str = [&](const char* k) {
        const json::Value* v = prm ? prm->find(k) : nullptr;
        return v && v->is_str() ? v->str : std::string();
    };
    auto vec3 = [&](const char* k, double dflt) {
        placer::Vec3 out{dflt, dflt, dflt};
        const json::Value* v = prm ? prm->find(k) : nullptr;
        if (v && v->is_arr())
            for (size_t i = 0; i < 3 && i < v->arr.size(); ++i)
                out[i] = v->arr[i].num;
        return out;
    };
    po.kind = str("kind").empty() ? "cube" : str("kind");
    std::transform(po.kind.begin(), po.kind.end(), po.kind.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (po.kind != "clone" && po.kind != "model" && po.kind != "cube"
        && po.kind != "sphere" && po.kind != "cylinder")
        throw std::runtime_error("AddObject: unknown kind '" + po.kind + "'");
    po.name = str("name");
    po.position = vec3("position", 0.0);
    po.rotation_euler_deg = vec3("rotation_euler_deg", 0.0);
    po.scale_xform = vec3("scale_xform", 1.0);
    const json::Value* wm = prm ? prm->find("world_matrix") : nullptr;
    if (wm && wm->is_arr() && wm->arr.size() == 16) {
        po.has_world_matrix = true;
        for (size_t i = 0; i < 16; ++i) po.world_matrix[i] = wm->arr[i].num;
    }
    const json::Value* mk = prm ? prm->find("material_key") : nullptr;
    if (mk) { po.has_material_key = true; po.material_key = key_of(mk); }
    if (po.kind == "model") {
        const std::string source = str("source");
        const std::string model_path = str("model_path");
        if (!source.empty()) {
            po.model_path = resolve_asset(jmod_dir, source);
            if (po.model_path.empty())
                throw std::runtime_error("asset missing: " + source);
        } else {
            po.model_path = model_path;
        }
        if (po.model_path.empty())
            throw std::runtime_error(
                "add_object: model needs source or model_path");
        const json::Value* ivc =
            prm ? prm->find("import_vertex_colors") : nullptr;
        po.import_vertex_colors =
            ivc != nullptr && ivc->type == json::Value::Type::Bool && ivc->b;
    }
    if (po.kind == "clone") {
        po.source_key = key_of(prm ? prm->find("source_gao_key") : nullptr);
        const json::Value* sek = prm ? prm->find("source_entry_key") : nullptr;
        const json::Value* cwc = prm ? prm->find("clone_with_collision") : nullptr;
        po.collision = cwc != nullptr &&
                       cwc->type == json::Value::Type::Bool && cwc->b;
        if (sek != nullptr) {
            const uint32_t src_entry_key = key_of(sek);
            BFFile* src_fi = es.file_for(src_entry_key);
            if (validation_simulation) {
                // AddObject._simulate_loa silently falls back to same-bin
                // clone resolution when the source entry/GAO cannot be
                // collected, and uses its legacy LIFO/source-order chain
                // when it can. The actual build path below stays BFS.
                if (src_fi != nullptr) {
                    placer::XbinSource xs =
                        placer::collect_xbin_validation_source_chain(
                            es.bf, src_fi->index, po.source_key);
                    if (xs.ok) {
                        po.has_source_gao_data = true;
                        po.source_gao_data = std::move(xs.gao_data);
                        po.source_resource_chain = std::move(xs.chain);
                    }
                }
            } else if (src_fi == nullptr) {
                char b[80];
                std::snprintf(b, sizeof b,
                              "cross-bin clone: source entry 0x%08x not in "
                              "archive", src_entry_key);
                throw std::runtime_error(b);
            } else {
                // Cross-bin build: deep-copy the source GAO's BFS closure.
                placer::XbinSource xs = placer::collect_xbin_source_chain(
                    es.bf, src_fi->index, po.source_key);
                if (!xs.ok) throw std::runtime_error(xs.error);
                po.has_source_gao_data = true;
                po.source_gao_data = std::move(xs.gao_data);
                po.source_resource_chain = std::move(xs.chain);
                // Retain metadata for AddObject.apply's first log callback.
                source_entry_name = src_fi->name;
                cross_bin = true;
            }
        }
    } else {
        po.size = vec3("size", 1.0);
        const json::Value* col = prm ? prm->find("collision") : nullptr;
        po.collision = col != nullptr &&
                       col->type == json::Value::Type::Bool && col->b;
        const json::Value* vc = prm ? prm->find("vertex_color") : nullptr;
        if (vc && vc->is_arr() && vc->arr.size() >= 3) {
            po.has_vertex_color = true;
            po.vertex_color = {int(vc->arr[0].num), int(vc->arr[1].num),
                               int(vc->arr[2].num),
                               vc->arr.size() > 3 ? int(vc->arr[3].num) : 255};
        }
    }
    std::string prof = str("collision_profile");
    if (!prof.empty()) po.collision_profile = prof;
    const json::Value* rck = prm ? prm->find("room_cob_key") : nullptr;
    if (rck) { po.has_room_cob_key = true; po.room_cob_key = key_of(rck); }

    // Deterministic seed shared with ops_object._placement_allocator_seed.
    std::string id = op_id_of(op);
    uint32_t id_hash = crc32(reinterpret_cast<const uint8_t*>(id.data()),
                             id.size());
    uint32_t seed = (entry_key ^ id_hash ^ 0x4A414445u) & 0xFFFFFFFFu;
    const size_t source_chain_size = po.source_resource_chain.size();
    return {std::move(po), entry_key, seed, cross_bin,
            source_chain_size, std::move(source_entry_name)};
}

void apply_add_object(const json::Value& op, EntrySet& es,
                      const std::string& jmod_dir,
                      std::vector<std::string>& log) {
    PlacePrep prep = build_place_op(op, es, jmod_dir);
    std::vector<uint8_t>& dec = es.get(prep.entry_key);
    std::vector<uint32_t> extra_keys;
    for (const auto& kv : es.bf.files)
        if (kv.second.key != INVALID_KEY) extra_keys.push_back(kv.second.key);

    const std::string op_id = op_id_of(op);
    if (prep.cross_bin) {
        log.push_back(
            "[" + op_id + "] cross-bin clone: deep-copying source GAO + "
            + std::to_string(prep.source_chain_size)
            + " transitive sub-entries from "
            + python_repr_string(prep.source_entry_name));
    }

    placer::PlacementResult r = placer::apply_placements_to_dec(
        dec, {prep.po}, /*geo_version=*/0, extra_keys, prep.seed);
    if (!r.ok) throw std::runtime_error(r.error);
    dec = std::move(r.patched);
    es.modified.insert(prep.entry_key);
    const std::string name = prep.po.name.empty()
                                 ? "JadePlaced_" + prep.po.kind
                                 : prep.po.name;
    log.push_back(
        "[" + op_id + "] add_object " + prep.po.kind + "/"
        + python_repr_string(name) + ": +"
        + std::to_string(r.additions.size()) + " sub-entries, +"
        + std::to_string(r.objects_registered)
        + " world-list entry/entries");
}

// replace_mesh: one GEO sub-entry from a GLB asset (ops_mesh.ReplaceMesh ->
// the native patcher model half). Carries the full bone-remap option set.
void apply_replace_mesh(const json::Value& op, EntrySet& es,
                        const std::string& jmod_dir,
                        std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t sub_key = key_of(tgt ? tgt->find("sub_key") : nullptr);
    std::string source;
    if (prm) {
        const json::Value* s = prm->find("source");
        if (s && s->is_str()) source = s->str;
    }
    if (source.empty()) {
        log.push_back("[" + op_id_of(op) +
                      "] replace_mesh: no source asset; skipped");
        return;
    }
    std::string src_path = resolve_asset(jmod_dir, source);
    if (src_path.empty())
        throw std::runtime_error("asset missing: " + source);
    std::ifstream f(std::filesystem::u8path(src_path), std::ios::binary);
    if (!f) throw std::runtime_error("asset missing: " + source);
    std::vector<uint8_t> glb((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    patcher::PatchModelOptions po;
    if (prm) {
        auto int_map = [&](const char* k, std::map<int, int>& out) {
            const json::Value* m = prm->find(k);
            if (m == nullptr || m->type != json::Value::Type::Object) return;
            for (const auto& kv : m->obj)
                if (kv.second.is_num())
                    out[std::atoi(kv.first.c_str())] = int(kv.second.num);
        };
        int_map("bone_map", po.bone_map);
        int_map("drop_targets", po.drop_targets);
        const json::Value* bms = prm->find("bone_map_source");
        if (bms != nullptr && bms->type == json::Value::Type::Object)
            for (const auto& kv : bms->obj)
                if (kv.second.is_str())
                    po.bone_map_source[std::atoi(kv.first.c_str())] =
                        kv.second.str;
        const json::Value* bd = prm->find("bone_drops");
        if (bd != nullptr && bd->is_arr())
            for (const json::Value& v : bd->arr)
                if (v.is_num()) po.bone_drops.insert(int(v.num));
        const json::Value* rb = prm->find("rigid_bind_bone");
        if (rb != nullptr && rb->is_num()) {
            po.has_rigid_bind_bone = true;
            po.rigid_bind_bone = int(rb->num);
        } else {
            // Python defaults rigid_bind_bone to 0 (not None) at load time.
            po.has_rigid_bind_bone = true;
            po.rigid_bind_bone = 0;
        }
        auto flag = [&](const char* k) {
            const json::Value* v = prm->find(k);
            return v != nullptr && v->type == json::Value::Type::Bool && v->b;
        };
        po.auto_rig = flag("auto_rig");
        po.diagnose_rest_pose = flag("diagnose_rest_pose");
        po.import_vertex_colors = flag("import_vertex_colors");
        po.keep_original_skin = flag("keep_original_skin");
    }

    std::vector<uint8_t>& dec = es.get(entry_key);
    patcher::PatchModelResult r =
        patcher::patch_model_glb(dec, sub_key, glb, po, log);
    if (!r.changed) {
        log.push_back("[" + op_id_of(op) +
                      "] replace_mesh: patcher returned no change");
        return;
    }
    dec = std::move(r.patched);
    es.modified.insert(entry_key);
}

// add_object_collision: extend the host room COB(s) with the object's mesh
// (shape-accurate) or its oriented OBBox, at its current pose (ops_transform.
// AddObjectCollision -> placer add_object_collision_box).
void apply_add_object_collision(const json::Value& op, EntrySet& es,
                                std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t gao_key = key_of(tgt ? tgt->find("gao_key") : nullptr);
    std::string profile = "simple_box", shape = "mesh";
    bool has_rck = false;
    uint32_t rck = 0;
    if (prm) {
        const json::Value* v;
        if ((v = prm->find("collision_profile")) && v->is_str() &&
            !v->str.empty())
            profile = v->str;
        if ((v = prm->find("collision_shape")) && v->is_str() &&
            !v->str.empty())
            shape = v->str;
        if ((v = prm->find("room_cob_key")) &&
            v->type != json::Value::Type::Null) {
            has_rck = true;
            rck = key_of(v);
        }
    }
    for (char& c : shape) c = char(std::tolower(static_cast<unsigned char>(c)));

    std::vector<uint8_t>& dec = es.get(entry_key);
    placer::AddCollisionResult r = placer::add_object_collision_box(
        dec, gao_key, profile, has_rck, rck, shape);
    if (!r.ok) throw std::runtime_error(r.error);
    if (r.dec != dec) dec = std::move(r.dec);
    es.modified.insert(entry_key);
    std::string detail = "shape " + shape;
    if (shape == "box") detail += "/" + profile;
    char host[32];
    if (has_rck)
        std::snprintf(host, sizeof host, " host 0x%08x", rck);
    else
        std::snprintf(host, sizeof host, " (auto host)");
    char b[176];
    std::snprintf(b, sizeof b,
                  "[%s] add_object_collision GAO 0x%08x in entry 0x%08x: %s%s",
                  op_id_of(op).c_str(), gao_key, entry_key, detail.c_str(),
                  host);
    log.push_back(b);
}

// ── material ops (slice 2) ──────────────────────────────────────────────────
namespace matops {
inline void put32(std::vector<uint8_t>& dec, size_t o, uint32_t v) {
    dec[o] = uint8_t(v); dec[o + 1] = uint8_t(v >> 8);
    dec[o + 2] = uint8_t(v >> 16); dec[o + 3] = uint8_t(v >> 24);
}
inline uint32_t rd32(const std::vector<uint8_t>& dec, size_t o) {
    return uint32_t(dec[o]) | (uint32_t(dec[o + 1]) << 8) |
           (uint32_t(dec[o + 2]) << 16) | (uint32_t(dec[o + 3]) << 24);
}
const SubEntry* find_sub(const std::vector<SubEntry>& subs, uint32_t key,
                         int want_gro = -1) {
    for (const SubEntry& s : subs) {
        if (s.key != key) continue;
        if (want_gro >= 0 && (s.gro_null || int(s.gro_type) != want_gro)) continue;
        return &s;
    }
    return nullptr;
}
}  // namespace matops

// retarget_material_texture: texture-layer key write (kind-aware offset).
void apply_retarget_material_texture(const json::Value& op, EntrySet& es,
                                     std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t sub_key = key_of(tgt ? tgt->find("sub_key") : nullptr);
    uint32_t new_key = key_of(prm ? prm->find("new_texture_key") : nullptr);
    uint32_t layer = 0;
    if (prm) {
        const json::Value* l = prm->find("layer");
        if (l && l->is_num()) layer = uint32_t(std::max(0.0, l->num));
    }
    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = matops::find_sub(subs, sub_key);
    char b[160];
    if (target == nullptr) {
        std::snprintf(b, sizeof b,
                      "[%s] retarget_material_texture: sub-entry 0x%08x not in "
                      "0x%08x", op_id_of(op).c_str(), sub_key, entry_key);
        log.push_back(b);
        return;
    }
    long long off = texture_layer_offset(target->data.data(),
                                         target->data.size(), layer);
    if (off < 0) {
        std::snprintf(b, sizeof b,
                      "[%s] retarget_material_texture: sub-material 0x%08x has "
                      "no texture layer %u", op_id_of(op).c_str(), sub_key, layer);
        log.push_back(b);
        return;
    }
    size_t write_off = target->offset + 12 + size_t(off);
    uint32_t old = matops::rd32(dec, write_off);
    matops::put32(dec, write_off, new_key);
    es.modified.insert(entry_key);
    std::snprintf(b, sizeof b,
                  "[%s] retarget_material_texture: 0x%08x/0x%08x texture "
                  "0x%08x -> 0x%08x", op_id_of(op).c_str(), entry_key, sub_key,
                  old, new_key);
    log.push_back(b);
}

// set_multi_material_slot: positional slot-key write in a gro-4 container.
void apply_set_multi_material_slot(const json::Value& op, EntrySet& es,
                                   std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t multi_key = key_of(tgt ? tgt->find("multi_key") : nullptr);
    uint32_t new_key = key_of(prm ? prm->find("new_material_key") : nullptr);
    long long slot = 0;
    if (prm) {
        const json::Value* s = prm->find("slot");
        if (s && s->is_num()) slot = (long long)s->num;
    }
    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = matops::find_sub(subs, multi_key, 4);
    char b[160];
    if (target == nullptr) {
        std::snprintf(b, sizeof b,
                      "[%s] set_multi_material_slot: multi-material 0x%08x not "
                      "in 0x%08x", op_id_of(op).c_str(), multi_key, entry_key);
        log.push_back(b);
        return;
    }
    long long off = slot < 0 ? -1
                             : multi_slot_offset(target->data.data(),
                                                 target->data.size(),
                                                 uint32_t(slot));
    if (off < 0) {
        std::snprintf(b, sizeof b,
                      "[%s] set_multi_material_slot: slot %lld out of range for "
                      "0x%08x", op_id_of(op).c_str(), slot, multi_key);
        log.push_back(b);
        return;
    }
    size_t write_off = target->offset + 12 + size_t(off);
    uint32_t old = matops::rd32(dec, write_off);
    matops::put32(dec, write_off, new_key);
    es.modified.insert(entry_key);
    std::snprintf(b, sizeof b,
                  "[%s] set_multi_material_slot: 0x%08x/0x%08x slot %lld: "
                  "0x%08x -> 0x%08x", op_id_of(op).c_str(), entry_key,
                  multi_key, slot, old, new_key);
    log.push_back(b);
}

// set_material_flags: the ENGINE-read ul_Flags word (per-level on kinds 8/9).
void apply_set_material_flags(const json::Value& op, EntrySet& es,
                              std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t sub_key = key_of(tgt ? tgt->find("sub_key") : nullptr);
    uint32_t ul_flags = key_of(prm ? prm->find("ul_flags") : nullptr);
    uint32_t layer = 0;
    if (prm) {
        const json::Value* l = prm->find("layer");
        if (l && l->is_num()) layer = uint32_t(std::max(0.0, l->num));
    }
    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = matops::find_sub(subs, sub_key);
    char b[160];
    if (target == nullptr) {
        std::snprintf(b, sizeof b,
                      "[%s] set_material_flags: sub-material 0x%08x not in "
                      "0x%08x", op_id_of(op).c_str(), sub_key, entry_key);
        log.push_back(b);
        return;
    }
    long long off = material_ulflags_offset(target->data.data(),
                                            target->data.size(), layer);
    if (off < 0) {
        std::snprintf(b, sizeof b,
                      "[%s] set_material_flags: sub-material 0x%08x is not a "
                      "recognised material (or has no layer %u)",
                      op_id_of(op).c_str(), sub_key, layer);
        log.push_back(b);
        return;
    }
    size_t write_off = target->offset + 12 + size_t(off);
    uint32_t old = matops::rd32(dec, write_off);
    matops::put32(dec, write_off, ul_flags);
    es.modified.insert(entry_key);
    std::snprintf(b, sizeof b,
                  "[%s] set_material_flags: 0x%08x/0x%08x ul_Flags 0x%08x -> "
                  "0x%08x", op_id_of(op).c_str(), entry_key, sub_key, old,
                  ul_flags);
    log.push_back(b);
}

// modify_gao_material: the visual block's grm_key (vis_off + 4).
void apply_modify_gao_material(const json::Value& op, EntrySet& es,
                               std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t gao_key = key_of(tgt ? tgt->find("gao_key") : nullptr);
    uint32_t material_key = key_of(prm ? prm->find("material_key") : nullptr);

    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = nullptr;
    for (const SubEntry& s : subs)
        if (s.key == gao_key && s.ext == ".gao") { target = &s; break; }
    char b[144];
    if (target == nullptr) {
        std::snprintf(b, sizeof b,
                      "modify_gao_material: GAO 0x%08x not found in entry 0x%08x",
                      gao_key, entry_key);
        throw std::runtime_error(b);
    }
    long long vis_off = visual_block_offset(target->data.data(),
                                            target->data.size());
    if (vis_off < 0)
        throw std::runtime_error(
            "modify_gao_material: GAO has no visual block (FLAG_VISUAL clear) "
            "— nothing to patch");
    size_t dec_off = target->offset + 12 + size_t(vis_off) + 4;
    uint32_t old = matops::rd32(dec, dec_off);
    if (old == material_key) {
        std::snprintf(b, sizeof b,
                      "[%s] modify_gao_material noop: GAO 0x%08x already "
                      "references material 0x%08x", op_id_of(op).c_str(),
                      gao_key, old);
        log.push_back(b);
        return;
    }
    matops::put32(dec, dec_off, material_key);
    es.modified.insert(entry_key);
    std::snprintf(b, sizeof b,
                  "[%s] modify_gao_material GAO 0x%08x: material 0x%08x -> "
                  "0x%08x", op_id_of(op).c_str(), gao_key, old, material_key);
    log.push_back(b);
}

// ── asset store (project/assets.py: <jmod>/assets/<sha256><ext>) ───────────
std::string resolve_asset(const std::string& jmod_dir, const std::string& ref) {
    try {
        return project_assets::AssetStore(
                   (std::filesystem::u8path(jmod_dir) / "assets").u8string())
            .resolve(ref);
    } catch (const std::exception&) {
        return "";
    }
}

// replace_texture delegates to the same public patcher core used by the
// manifest/change-set workflow, preventing the two native entry points from
// drifting on texture formats, dimensions, stub headers, or diagnostics.
void apply_replace_texture(const json::Value& op, EntrySet& es,
                           const std::string& jmod_dir,
                           std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t sub_key = key_of(tgt ? tgt->find("sub_key") : nullptr);
    std::string source, encode = "auto";
    if (prm) {
        const json::Value* s = prm->find("source");
        if (s && s->is_str()) source = s->str;
        const json::Value* e = prm->find("encode");
        if (e && e->is_str()) encode = e->str;
        else if (e && e->is_num()) encode = std::to_string(int(e->num));
    }
    char b[176];
    if (source.empty()) {
        std::snprintf(b, sizeof b, "[%s] replace_texture: no source asset; "
                      "skipped", op_id_of(op).c_str());
        log.push_back(b);
        return;
    }
    std::string src_path = resolve_asset(jmod_dir, source);
    if (src_path.empty())
        throw std::runtime_error("asset missing: " + source);
    std::vector<uint8_t>& dec = es.get(entry_key);
    patcher::PatchTextureResult r =
        patcher::patch_texture(dec, sub_key, src_path, encode, log);
    if (r.changed) {
        dec = std::move(r.patched);
        es.modified.insert(entry_key);
    }
}

// ── add_texture / add_material (core/asset_add.py — in-bin insertion) ──────
namespace assetadd {

bool is_tex_stub(const std::vector<uint8_t>& d) {
    if (!is_texture_entry(d.data(), d.size())) return false;
    TexInfo t = parse_texture(d.data(), d.size());
    return t.valid && is_placeholder(t, d.size() - std::min(d.size(), t.pix_start));
}
bool is_tex_full(const std::vector<uint8_t>& d) {
    if (!is_texture_entry(d.data(), d.size())) return false;
    TexInfo t = parse_texture(d.data(), d.size());
    return t.valid && !is_placeholder(t, d.size() - std::min(d.size(), t.pix_start));
}
// pack_record: [size][99C0FFEE][key][type][payload] (size counts the type).
std::vector<uint8_t> pack_record(uint32_t key, uint32_t type_field,
                                 const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> out;
    auto p32 = [&](uint32_t v) {
        out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8));
        out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 24));
    };
    p32(uint32_t(4 + payload.size()));
    out.insert(out.end(), {0x99, 0xC0, 0xFF, 0xEE});
    p32(key);
    p32(type_field);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// insert_texture's wave slot: before the first wave record with key >
// new_key, else after the last.
size_t wave_slot(const std::vector<const SubEntry*>& wave, uint32_t new_key,
                 size_t /*dec_size*/) {
    for (const SubEntry* s : wave)
        if (s->key > new_key) return s->offset - 4;
    const SubEntry* last = wave.back();
    return last->offset - 4 + 12 + last->size;
}

}  // namespace assetadd

void apply_add_texture(const json::Value& op, EntrySet& es,
                       const std::string& jmod_dir,
                       std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t new_key = key_of(prm ? prm->find("new_key") : nullptr);
    std::string source, encode = "7";
    if (prm) {
        const json::Value* s = prm->find("source");
        if (s && s->is_str()) source = s->str;
        const json::Value* e = prm->find("encode");
        if (e && e->is_str()) encode = e->str;
    }
    char b[176];
    if (source.empty()) {
        std::snprintf(b, sizeof b, "[%s] add_texture: no source asset; skipped",
                      op_id_of(op).c_str());
        log.push_back(b);
        return;
    }
    uint32_t fmt;
    if (encode == "0") fmt = 0;
    else if (encode == "5") fmt = 5;
    else if (encode == "7") fmt = 7;
    else throw std::runtime_error("unsupported encode '" + encode + "'");

    std::string src_path = resolve_asset(jmod_dir, source);
    if (src_path.empty()) throw std::runtime_error("asset missing: " + source);
    RgbaImage img = load_rgba_image(src_path);
    if (!img.ok)
        throw std::runtime_error("could not read source image " + src_path
                                 + ": " + img.error);
    const uint32_t source_w = img.width, source_h = img.height;
    const uint32_t new_w = nearest_power_of_two(source_w);
    const uint32_t new_h = nearest_power_of_two(source_h);
    if (new_w != source_w || new_h != source_h) {
        img.rgba = resize_rgba_lanczos(img.rgba.data(), source_w, source_h,
                                       new_w, new_h);
        if (img.rgba.empty())
            throw std::runtime_error("add_texture: image resize failed");
        img.width = new_w;
        img.height = new_h;
    }

    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    for (const SubEntry& s : subs)
        if (s.key == new_key) {
            std::snprintf(b, sizeof b,
                          "key 0x%08X already exists in this bin", new_key);
            throw std::runtime_error(b);
        }
    std::vector<const SubEntry*> stubs, fulls;
    for (const SubEntry& s : subs) {
        if (assetadd::is_tex_stub(s.data)) stubs.push_back(&s);
        else if (assetadd::is_tex_full(s.data)) fulls.push_back(&s);
    }
    if (fulls.empty()) {
        std::snprintf(b, sizeof b,
                      "entry 0x%08x ships no full texture — "
                      "cannot host a new one", entry_key);
        throw std::runtime_error(b);
    }
    // Donor = the LARGEST full texture; its 52-byte header seeds the records
    // (logical dims inherited — the engine samples UVs against them).
    const SubEntry* donor = fulls.front();
    for (const SubEntry* s : fulls)
        if (s->data.size() > donor->data.size()) donor = s;

    std::vector<uint8_t> header(donor->data.begin(), donor->data.begin() + 52);
    matops::put32(header, 36, fmt);
    matops::put32(header, 40, img.width);
    matops::put32(header, 44, img.height);
    matops::put32(header, 48, 0);                     // base level only
    std::vector<uint8_t> pix;
    if (fmt == 0) pix = encode_bgra(img.rgba.data(), img.width, img.height);
    else if (fmt == 5) pix = encode_dxt1(img.rgba.data(), img.width, img.height);
    else pix = encode_dxt5(img.rgba.data(), img.width, img.height);
    std::vector<uint8_t> full_payload = header;
    if (fmt == 5) full_payload.insert(full_payload.end(), {0, 0, 0, 0});
    std::vector<uint8_t> stub_payload = full_payload;   // header (+prefix) only
    full_payload.insert(full_payload.end(), pix.begin(), pix.end());
    // NOTE: the texture record's TYPE FIELD is its own key (asset_add
    // pack_record(new_key, new_key, ...)).
    std::vector<uint8_t> full_rec = assetadd::pack_record(new_key, new_key,
                                                          full_payload);
    std::vector<uint8_t> stub_rec = assetadd::pack_record(new_key, new_key,
                                                          stub_payload);

    struct Ins { size_t off; const std::vector<uint8_t>* rec; };
    std::vector<Ins> inserts{{assetadd::wave_slot(fulls, new_key, dec.size()),
                              &full_rec}};
    bool inserted_stub = !stubs.empty();
    if (inserted_stub)
        inserts.push_back({assetadd::wave_slot(stubs, new_key, dec.size()),
                           &stub_rec});
    std::sort(inserts.begin(), inserts.end(),
              [](const Ins& a, const Ins& z) { return a.off > z.off; });
    const std::vector<uint8_t> old_dec = dec;
    for (const Ins& in : inserts)
        dec.insert(dec.begin() + long(in.off), in.rec->begin(), in.rec->end());

    const std::vector<VerifyLeafIssue> problems = check_texture_insertion(
        old_dec, dec, new_key, inserted_stub ? 2 : 1);
    std::string errors;
    for (const VerifyLeafIssue& problem : problems) {
        if (problem.level == "error") {
            if (!errors.empty()) errors += "; ";
            errors += problem.message;
        }
    }
    if (!errors.empty())
        throw std::runtime_error("insertion self-check failed: " + errors);
    for (const VerifyLeafIssue& problem : problems) {
        std::snprintf(b, sizeof b, "[%s] add_texture: %s: %s",
                      op_id_of(op).c_str(), problem.level.c_str(),
                      problem.message.c_str());
        log.push_back(b);
    }
    es.modified.insert(entry_key);
    std::snprintf(b, sizeof b,
                  "[%s] add_texture: 0x%08x (%ux%u fmt%u) inserted into 0x%08x "
                  "(stub wave: %zu, full wave: %zu)", op_id_of(op).c_str(),
                  new_key, source_w, source_h, fmt, entry_key, stubs.size(),
                  fulls.size());
    log.push_back(b);
}

void apply_add_material(const json::Value& op, EntrySet& es,
                        std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t new_key = key_of(prm ? prm->find("new_key") : nullptr);
    uint32_t texture_key = key_of(prm ? prm->find("texture_key") : nullptr);
    uint32_t donor_key = key_of(prm ? prm->find("donor_key") : nullptr);

    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    for (const SubEntry& s : subs)
        if (s.key == new_key) {
            char duplicate[64];
            std::snprintf(duplicate, sizeof duplicate,
                          "key 0x%08X already exists in this bin", new_key);
            throw std::runtime_error(duplicate);
        }
    const SubEntry* donor = nullptr;
    for (const SubEntry& s : subs) {
        if (s.gro_null || s.gro_type != 5) continue;
        // is_submaterial: the KIND is recognised (a texture-key offset
        // exists) — the stored key value itself is irrelevant.
        if (texture_layer_offset(s.data.data(), s.data.size(), 0) < 0) continue;
        if (donor_key != 0 && s.key != donor_key) continue;
        donor = &s;
        break;
    }
    if (donor == nullptr) {
        char missing[96];
        if (donor_key != 0)
            std::snprintf(missing, sizeof missing,
                          "no donor sub-material 0x%08X in this bin",
                          donor_key);
        else
            std::snprintf(missing, sizeof missing,
                          "no donor sub-material in this bin");
        throw std::runtime_error(missing);
    }
    std::vector<uint8_t> new_payload =
        set_texture_key(donor->data.data(), donor->data.size(), texture_key);
    if (new_payload.empty())
        throw std::runtime_error("donor has an unrecognised material kind");
    std::vector<uint8_t> rec =
        assetadd::pack_record(new_key, donor->gro_type, new_payload);
    size_t insert_at = donor->offset - 4 + 12 + donor->size;
    dec.insert(dec.begin() + long(insert_at), rec.begin(), rec.end());
    es.modified.insert(entry_key);
    char b[160];
    uint32_t donor_kind = donor->data.size() >= 4
                              ? matops::rd32(donor->data, 0)
                              : 0;
    std::snprintf(b, sizeof b,
                  "[%s] add_material: 0x%08x -> texture 0x%08x (donor 0x%08x "
                  "kind %u) in 0x%08x", op_id_of(op).c_str(), new_key,
                  texture_key, donor->key, donor_kind, entry_key);
    log.push_back(b);
}

// replace_entry_raw: whole-entry replacement from a raw-bytes asset.
void apply_replace_entry_raw(const json::Value& op, EntrySet& es,
                             const std::string& jmod_dir,
                             std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    std::string source;
    if (prm) {
        const json::Value* s = prm->find("source");
        if (s && s->is_str()) source = s->str;
    }
    char b[144];
    if (source.empty()) {
        std::snprintf(b, sizeof b, "[%s] replace_entry_raw: no source asset; "
                      "skipped", op_id_of(op).c_str());
        log.push_back(b);
        return;
    }
    std::string src_path = resolve_asset(jmod_dir, source);
    if (src_path.empty()) throw std::runtime_error("asset missing: " + source);
    std::ifstream f(std::filesystem::u8path(src_path), std::ios::binary);
    std::vector<uint8_t> new_bytes((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
    std::vector<uint8_t>& dec = es.get(entry_key);   // pre-load (size record)
    dec = std::move(new_bytes);
    es.modified.insert(entry_key);
    std::snprintf(b, sizeof b, "[%s] entry 0x%08x: replaced with %zuB from %s",
                  op_id_of(op).c_str(), entry_key, dec.size(), source.c_str());
    log.push_back(b);
}

// replace_animation (ops_animation.ReplaceAnimation ->
// patcher._patch_animation): replace one TRL payload in place.  The Python
// operation deliberately rejects size changes because animation slots have a
// strict fixed-size contract.
void apply_replace_animation(const json::Value& op, EntrySet& es,
                             const std::string& jmod_dir,
                             std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t sub_key = key_of(tgt ? tgt->find("sub_key") : nullptr);
    std::string source;
    if (prm) {
        const json::Value* s = prm->find("source");
        if (s && s->is_str()) source = s->str;
    }
    if (source.empty()) {
        log.push_back("[" + op_id_of(op) +
                      "] replace_animation: no source asset; skipped");
        return;
    }
    std::string src_path = resolve_asset(jmod_dir, source);
    if (src_path.empty())
        throw std::runtime_error("asset missing: " + source);
    std::ifstream f(std::filesystem::u8path(src_path), std::ios::binary);
    if (!f) throw std::runtime_error("asset missing: " + source);
    std::vector<uint8_t> new_trl((std::istreambuf_iterator<char>(f)),
                                 std::istreambuf_iterator<char>());

    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    long long payload_start = -1, payload_end = -1;
    for (size_t i = 0; i < subs.size(); ++i) {
        if (subs[i].key != sub_key) continue;
        payload_start = static_cast<long long>(subs[i].offset) + 12;
        payload_end = (i + 1 < subs.size())
                          ? static_cast<long long>(subs[i + 1].offset) - 4
                          : static_cast<long long>(dec.size());
        break;
    }

    char b[144];
    if (payload_start < 0) {
        std::snprintf(b, sizeof b,
                      "  Animation sub-entry 0x%08X not found", sub_key);
        log.push_back(b);
        return;
    }
    size_t old_size = static_cast<size_t>(payload_end - payload_start);
    if (new_trl.size() != old_size) {
        std::snprintf(b, sizeof b,
                      "  TRL size mismatch: %zu vs %zu (must match for in-place)",
                      new_trl.size(), old_size);
        log.push_back(b);
        return;
    }
    std::copy(new_trl.begin(), new_trl.end(),
              dec.begin() + payload_start);
    es.modified.insert(entry_key);
    std::snprintf(b, sizeof b, "  Patched ANIM 0x%08X: %zuB", sub_key,
                  new_trl.size());
    log.push_back(b);
}

// add_zone_dependency (zone_create.add_wol_deps): insert .wow-tagged deps
// BEFORE the last existing dep (the wow self-ref — load order matters).
void apply_add_zone_dependency(const json::Value& op, EntrySet& es,
                               std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t wol_entry_key = key_of(tgt ? tgt->find("wol_entry_key") : nullptr);
    std::vector<uint32_t> dep_keys;
    if (prm) {
        const json::Value* dk = prm->find("dep_keys");
        if (dk && dk->is_arr())
            for (const json::Value& v : dk->arr) {
                uint32_t k = key_of(&v);
                if (k != INVALID_KEY) dep_keys.push_back(k);
            }
    }
    char b[144];
    if (dep_keys.empty()) {
        std::snprintf(b, sizeof b, "[%s] add_zone_dependency: nothing to add — "
                      "skipped", op_id_of(op).c_str());
        log.push_back(b);
        return;
    }
    std::vector<uint8_t>& dec = es.get(wol_entry_key);
    auto updated = zonecreate::add_wol_deps(dec, dep_keys);
    dec = std::move(updated.first);
    // Python marks the entry modified even if every requested dependency was
    // already present (or the payload was too short to edit).
    es.modified.insert(wol_entry_key);
    std::snprintf(b, sizeof b, "[%s] add_zone_dependency: wol=0x%08x "
                  "requested=%zu added=%zu", op_id_of(op).c_str(), wol_entry_key,
                  dep_keys.size(), size_t(updated.second));
    log.push_back(b);
}

// ── set_element_material (per-GEO element repaint, container growth) ───────
namespace elem {

const SubEntry* find_geo(const std::vector<SubEntry>& subs, uint32_t geo_key,
                         uint32_t element) {
    for (const SubEntry& s : subs) {
        if (s.key != geo_key || s.gro_null || s.gro_type != 1) continue;
        if (element_matid_offset(s.data.data(), s.data.size(), element) >= 0)
            return &s;
    }
    return nullptr;
}

// _geos_using_container: geo keys whose GAO visual points at the container.
std::unordered_set<uint32_t> geos_using_container(
    const std::vector<SubEntry>& subs, uint32_t multi_key, uint32_t seed_geo) {
    std::unordered_set<uint32_t> keys{seed_geo};
    for (const SubEntry& s : subs) {
        if (s.ext != ".gao") continue;
        GaoInfo gi = parse_gao_full(s.data.data(), s.data.size());
        if (gi.ok && gi.vis_read && gi.grm_key == multi_key &&
            gi.gro_key != 0 && gi.gro_key != INVALID_KEY)
            keys.insert(gi.gro_key);
    }
    return keys;
}

// body + cooked-trailing matId write for one element (same-size, in place).
bool write_element_matid(std::vector<uint8_t>& dec,
                         const std::vector<SubEntry>& subs, uint32_t geo_key,
                         uint32_t element, uint32_t new_matid) {
    for (const SubEntry& s : subs) {
        if (s.key != geo_key || s.gro_null || s.gro_type != 1) continue;
        long long off = element_matid_offset(s.data.data(), s.data.size(),
                                             element);
        if (off < 0) continue;
        size_t base = s.offset + 12;
        matops::put32(dec, base + size_t(off), new_matid);
        std::vector<size_t> cooked =
            cooked_element_matid_offsets(s.data.data(), s.data.size());
        if (element < cooked.size())
            matops::put32(dec, base + cooked[element], new_matid);
        return true;
    }
    return false;
}

}  // namespace elem

void apply_set_element_material(const json::Value& op, EntrySet& es,
                                std::vector<std::string>& log) {
    const json::Value* tgt = op.find("target");
    const json::Value* prm = op.find("params");
    uint32_t entry_key = key_of(tgt ? tgt->find("entry_key") : nullptr);
    uint32_t geo_key = key_of(tgt ? tgt->find("geo_key") : nullptr);
    uint32_t multi_key = key_of(tgt ? tgt->find("multi_key") : nullptr);
    const json::Value* cek = tgt ? tgt->find("container_entry_key") : nullptr;
    uint32_t cont_entry = cek != nullptr ? key_of(cek) : entry_key;
    bool same = cont_entry == entry_key;
    uint32_t element = 0, new_material_key = 0;
    if (prm) {
        const json::Value* e = prm->find("element");
        if (e && e->is_num()) element = uint32_t(e->num);
        new_material_key = key_of(prm->find("new_material_key"));
    }
    char b[224];

    std::vector<uint8_t>& geo_dec = es.get(entry_key);
    std::vector<SubEntry> geo_subs = walk_sub_entries(geo_dec);
    std::vector<uint8_t>* cont_dec = &geo_dec;
    std::vector<SubEntry> cont_subs;
    if (!same) {
        cont_dec = &es.get(cont_entry);
        cont_subs = walk_sub_entries(*cont_dec);
    } else {
        cont_subs = geo_subs;
    }

    const SubEntry* container = matops::find_sub(cont_subs, multi_key, 4);
    if (container == nullptr) {
        std::snprintf(b, sizeof b,
                      "[%s] set_element_material: multi-material 0x%08x not in "
                      "0x%08x", op_id_of(op).c_str(), multi_key, cont_entry);
        log.push_back(b);
        return;
    }
    MatInfo mm = parse_material(container->data.data(), container->data.size(), 4);
    const std::vector<uint32_t>& slots = mm.sub_material_keys;

    // (in_geo, geo_key, element, new_matid) repins.
    std::vector<std::tuple<bool, uint32_t, uint32_t, uint32_t>> repins;
    uint32_t slot;
    bool appended = false;
    auto sit = std::find(slots.begin(), slots.end(), new_material_key);
    if (sit != slots.end()) {
        slot = uint32_t(sit - slots.begin());
    } else {
        uint32_t n_sub = mm.ok ? mm.n_sub : uint32_t(slots.size());
        slot = n_sub;
        // Pin every previously-dangling element before growing the clamp
        // ceiling (engine clamps matId >= n_sub to the LAST slot).
        if (n_sub >= 1) {
            std::unordered_set<uint32_t> geos_using =
                elem::geos_using_container(geo_subs, multi_key, geo_key);
            if (!same)
                for (uint32_t k : elem::geos_using_container(cont_subs,
                                                             multi_key, geo_key))
                    geos_using.insert(k);
            std::vector<std::pair<const std::vector<SubEntry>*, bool>> sources;
            sources.push_back({&geo_subs, true});
            if (!same) sources.push_back({&cont_subs, false});
            for (const auto& src : sources)
                for (const SubEntry& s : *src.first) {
                    if (!geos_using.count(s.key) || s.gro_null || s.gro_type != 1)
                        continue;
                    GeoInfo g = parse_geometry(s.data.data(), s.data.size());
                    if (!g.ok || g.elements.empty()) continue;
                    for (size_t ei = 0; ei * 2 + 1 < g.elements.size(); ++ei) {
                        uint32_t mid = g.elements[ei * 2 + 1];
                        if (mid < n_sub) continue;
                        if (src.second && s.key == geo_key && ei == element)
                            continue;
                        repins.push_back({src.second, s.key, uint32_t(ei),
                                          n_sub - 1});
                    }
                }
        }
        // append_multi_sub_key: insert the key at 8 + n_sub*4, n_sub += 1.
        std::vector<uint8_t> np = container->data;
        if (np.size() < 8 || 8 + size_t(mm.n_sub) * 4 > np.size()) {
            std::snprintf(b, sizeof b,
                          "[%s] set_element_material: container 0x%08x is "
                          "malformed; cannot append a slot",
                          op_id_of(op).c_str(), multi_key);
            log.push_back(b);
            return;
        }
        size_t ins = 8 + size_t(mm.n_sub) * 4;
        uint8_t kb[4] = {uint8_t(new_material_key), uint8_t(new_material_key >> 8),
                         uint8_t(new_material_key >> 16),
                         uint8_t(new_material_key >> 24)};
        np.insert(np.begin() + long(ins), kb, kb + 4);
        matops::put32(np, 4, mm.n_sub + 1);
        appended = true;
        // Splice grown payload + bump the size field.
        size_t payload_start = container->offset + 12;
        size_t old_len = container->size - 4;
        cont_dec->erase(cont_dec->begin() + long(payload_start),
                        cont_dec->begin() + long(payload_start + old_len));
        cont_dec->insert(cont_dec->begin() + long(payload_start), np.begin(),
                         np.end());
        matops::put32(*cont_dec, container->offset - 4, uint32_t(4 + np.size()));
        cont_subs = walk_sub_entries(*cont_dec);

        // _relocate_after_slots: a 0x7A material record must sit at a higher
        // stream offset than every shipped slot record (forward-only walk).
        if ((new_material_key >> 24) == 0x7A) {
            const SubEntry* c2 = matops::find_sub(cont_subs, multi_key, 4);
            MatInfo mm2 = parse_material(c2->data.data(), c2->data.size(), 4);
            std::unordered_set<uint32_t> slot_keys;
            for (uint32_t k : mm2.sub_material_keys)
                if (k != 0 && k != INVALID_KEY && k != new_material_key)
                    slot_keys.insert(k);
            size_t insert_at = c2->offset - 4 + 12 + c2->size;
            for (const SubEntry& s : cont_subs)
                if (slot_keys.count(s.key))
                    insert_at = std::max(insert_at,
                                         s.offset - 4 + 12 + s.size);
            const SubEntry* ours = nullptr;
            for (const SubEntry& s : cont_subs)
                if (s.key == new_material_key) { ours = &s; break; }
            if (ours != nullptr) {
                size_t start = ours->offset - 4;
                size_t end = start + 12 + ours->size;
                if (start < insert_at) {
                    std::vector<uint8_t> rec(cont_dec->begin() + long(start),
                                             cont_dec->begin() + long(end));
                    cont_dec->erase(cont_dec->begin() + long(start),
                                    cont_dec->begin() + long(end));
                    insert_at -= rec.size();
                    cont_dec->insert(cont_dec->begin() + long(insert_at),
                                     rec.begin(), rec.end());
                    std::snprintf(b, sizeof b,
                                  "[%s] set_element_material: relocated "
                                  "material record 0x%08x after the "
                                  "container's last slot record",
                                  op_id_of(op).c_str(), new_material_key);
                    log.push_back(b);
                    cont_subs = walk_sub_entries(*cont_dec);
                }
            }
        }
        if (same) geo_subs = cont_subs;
    }

    const SubEntry* geo = elem::find_geo(geo_subs, geo_key, element);
    if (geo == nullptr) {
        std::snprintf(b, sizeof b,
                      "[%s] set_element_material: GEO 0x%08x (element %u) not "
                      "parseable in 0x%08x", op_id_of(op).c_str(), geo_key,
                      element, entry_key);
        log.push_back(b);
        return;
    }
    long long mat_off = element_matid_offset(geo->data.data(),
                                             geo->data.size(), element);
    size_t write_off = geo->offset + 12 + size_t(mat_off);
    uint32_t old_mid = matops::rd32(geo_dec, write_off);
    matops::put32(geo_dec, write_off, slot);
    std::vector<size_t> cooked =
        cooked_element_matid_offsets(geo->data.data(), geo->data.size());
    bool trailing_done = element < cooked.size();
    if (trailing_done)
        matops::put32(geo_dec, geo->offset + 12 + cooked[element], slot);

    int n_pinned = 0;
    for (const auto& rp : repins) {
        bool in_geo = std::get<0>(rp);
        std::vector<uint8_t>& dec = in_geo ? geo_dec : *cont_dec;
        const std::vector<SubEntry>& subs = in_geo ? geo_subs : cont_subs;
        if (elem::write_element_matid(dec, subs, std::get<1>(rp),
                                      std::get<2>(rp), std::get<3>(rp)))
            ++n_pinned;
    }

    es.modified.insert(entry_key);
    if (!same) es.modified.insert(cont_entry);
    std::snprintf(b, sizeof b,
                  "[%s] set_element_material: GEO 0x%08x element %u: matId %u "
                  "-> %u (%s slot -> material 0x%08x)%s%s",
                  op_id_of(op).c_str(), geo_key, element, old_mid, slot,
                  appended ? "appended" : "reused", new_material_key,
                  n_pinned ? "; pinned dangling element(s)" : "",
                  trailing_done ? "" : "; WARNING: cooked trailing matId "
                                       "table not found");
    log.push_back(b);
}

// ── create_zone (core/zone_create.py clone machinery) ──────────────────────
void apply_create_zone(const json::Value& op, EntrySet& es,
                       std::vector<std::string>& log) {
    const json::Value* prm = op.find("params");
    std::string template_zone, new_name;
    uint32_t new_map_prefix = 0x27;
    long long target_dir_idx = -1;
    std::vector<uint32_t> extra_deps;
    if (prm) {
        const json::Value* v;
        if ((v = prm->find("template_zone")) && v->is_str()) template_zone = v->str;
        if ((v = prm->find("new_name")) && v->is_str()) new_name = v->str;
        if ((v = prm->find("new_map_prefix"))) new_map_prefix = key_of(v) & 0xFF;
        if ((v = prm->find("target_dir_idx")) && v->is_num())
            target_dir_idx = (long long)v->num;
        if ((v = prm->find("extra_deps")) && v->is_arr())
            for (const json::Value& k : prm->find("extra_deps")->arr)
                extra_deps.push_back(key_of(&k));
    }
    std::vector<ZoneInfo> zones = discover_zones(es.bf);
    const ZoneInfo* tpl = nullptr;
    for (const ZoneInfo& z : zones)
        if (z.name == template_zone) { tpl = &z; break; }
    if (tpl == nullptr)
        throw zonecreate::Error(
            "RuntimeError", "template zone " + python_repr_string(template_zone) +
                                " not found in archive");
    LzoResult wr = decompress_lzo(es.bf.read_data(tpl->wow_index));
    LzoResult lr = decompress_lzo(es.bf.read_data(tpl->wol_index));
    if (!wr.ok || !lr.ok || wr.data.empty() || lr.data.empty())
        throw zonecreate::Error(
            "RuntimeError", "template zone " + python_repr_string(template_zone) +
                                " did not decompress");

    // alloc_new_zone_bf_keys: 0xFF00_0000 | ((prefix & 0x0F) << 16) | low16
    // with a deterministic incrementing low16 (staged keys included).
    std::vector<uint32_t> staged;
    staged.reserve(es.new_entries.size());
    for (const auto& item : es.new_entries) staged.push_back(item.first);
    const zonecreate::AllocatedZoneKeys allocated =
        zonecreate::alloc_new_zone_bf_keys(es.bf, new_map_prefix, staged);

    std::unordered_set<uint32_t> used;
    for (const auto& item : es.bf.files)
        if (item.second.key != INVALID_KEY) used.insert(item.second.key);
    used.insert(staged.begin(), staged.end());

    zonecreate::ClonedZone cloned = zonecreate::clone_zone(
        wr.data, lr.data, new_name, allocated.low16, used, new_map_prefix);

    // Extra deps into the cloned wol (add_wol_deps — before the self-ref).
    auto with_deps = zonecreate::add_wol_deps(cloned.wol_bytes, extra_deps);

    uint32_t parent_dir = target_dir_idx >= 0
                              ? uint32_t(target_dir_idx)
                              : es.bf.files.at(tpl->wow_index).parent;
    char nm[128];
    std::snprintf(nm, sizeof nm, "%s_wow_%08x.bin", new_name.c_str(),
                  allocated.wow_key);
    es.add_new_entry(allocated.wow_key, nm, parent_dir,
                     std::move(cloned.wow_bytes));
    std::snprintf(nm, sizeof nm, "%s_wol_%08x.bin", new_name.c_str(),
                  allocated.wol_key);
    es.add_new_entry(allocated.wol_key, nm, parent_dir,
                     std::move(with_deps.first));

    char wow_bf[16], wol_bf[16], internal_wow[16];
    std::snprintf(wow_bf, sizeof wow_bf, "0x%08x", allocated.wow_key);
    std::snprintf(wol_bf, sizeof wol_bf, "0x%08x", allocated.wol_key);
    std::snprintf(internal_wow, sizeof internal_wow, "0x%08x",
                  cloned.new_wow_key);
    log.push_back(
        "[" + op_id_of(op) + "] create_zone: template=" +
        python_repr_string(template_zone) + " -> new=" +
        python_repr_string(new_name) + "; wow_bf=" + wow_bf +
        " wol_bf=" + wol_bf + " internal_wow=" + internal_wow +
        " rekeyed=" + std::to_string(cloned.rekeyed_count) +
        " local_keys=" + std::to_string(cloned.local_keys_seen.size()) +
        " extra_deps_added=" + std::to_string(with_deps.second) +
        " parent_dir_idx=" + std::to_string(parent_dir));
}

// ── rescale_rli (NATIVE-FIRST) ──────────────────────────────────────────────
// The atmosphere grade as a recorded, buildable op: every district entry's
// baked RLI (+ optionally GRO_Lights and unlit-surface textures) through one
// make_transform(brightness, tint, contrast). Same-size in-place edits.
void apply_rescale_rli(const json::Value& op, EntrySet& es,
                       std::vector<std::string>& log) {
    const json::Value* prm = op.find("params");
    auto num = [&](const char* k, double d) {
        const json::Value* v = prm ? prm->find(k) : nullptr;
        return v && v->is_num() ? v->num : d;
    };
    auto flag = [&](const char* k, bool d) {
        const json::Value* v = prm ? prm->find(k) : nullptr;
        return v && v->type == json::Value::Type::Bool ? v->b : d;
    };
    std::string district;
    if (prm) {
        const json::Value* v = prm->find("district");
        if (v && v->is_str()) district = v->str;
    }
    double brightness = num("brightness", 1.0);
    double contrast = num("contrast", 1.0);
    std::array<int, 3> tint{255, 255, 255};
    if (prm) {
        const json::Value* v = prm->find("tint");
        if (v && v->is_arr() && v->arr.size() >= 3)
            tint = {int(v->arr[0].num), int(v->arr[1].num), int(v->arr[2].num)};
    }
    bool grade_lights = flag("grade_lights", true);
    bool grade_textures = flag("grade_textures", false);

    RliColorFn fn = make_transform(brightness, tint, contrast);
    std::vector<uint32_t> district_keys;
    for (const auto& kv : es.bf.files) {
        const BFFile& fi = kv.second;
        if (fi.name.empty() || fi.key == INVALID_KEY) continue;
        if (district.empty() || fi.name.find(district) != std::string::npos)
            district_keys.push_back(fi.key);
    }
    if (district_keys.empty())
        throw std::runtime_error("rescale_rli: district filter '" + district +
                                 "' matches no entries");

    // Pass 1 (textures): classify against the CURRENT entryset state.
    std::set<uint32_t> target_tex;
    if (grade_textures) {
        std::set<uint32_t> lit, unlit;
        for (uint32_t k : district_keys)
            classify_zone_textures_pub(parse_sub_entries(es.get(k)), lit, unlit);
        for (uint32_t k : unlit)
            if (!lit.count(k)) target_tex.insert(k);
    }

    uint32_t bins = 0, gaos = 0, lights = 0, textures = 0;
    for (uint32_t k : district_keys) {
        std::vector<uint8_t>& dec = es.get(k);
        RescaleDecResult rd = rescale_dec(dec, fn, grade_lights, target_tex);
        if (!rd.changed) continue;
        dec = std::move(rd.dec);
        es.modified.insert(k);
        ++bins;
        gaos += rd.gaos;
        lights += rd.lights;
        textures += rd.textures;
    }
    char b[160];
    std::snprintf(b, sizeof b,
                  "[%s] rescale_rli '%s': %u bins, %u GAOs, %u lights, %u "
                  "textures graded", op_id_of(op).c_str(), district.c_str(),
                  bins, gaos, lights, textures);
    log.push_back(b);
}

// ── pre-build validation (build/validate.py + per-op validate()) ───────────
// The em-dash the Python messages use (kept verbatim for oracle parity).
#define JD_DASH "\xe2\x80\x94"

namespace vals {

std::string hx(uint32_t k) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08x", k);
    return b;
}

// ops_gao_flags._TOGGLEABLE_MASK: ColMap (0x100) + ODE (0x10000000).
constexpr uint32_t kToggleableMask = 0x10000100u;

struct V {
    std::vector<BuildIssue>& issues;
    std::string op_id;
    size_t base;                 // issue count at validator entry
    V(std::vector<BuildIssue>& i, const json::Value& op)
        : issues(i), op_id(op_id_of(op)), base(i.size()) {}
    void err(const std::string& m) { issues.push_back({"error", m, op_id}); }
    void warn(const std::string& m) { issues.push_back({"warning", m, op_id}); }
    bool any() const { return issues.size() > base; }
    bool any_error() const {
        for (size_t i = base; i < issues.size(); ++i)
            if (issues[i].level == "error") return true;
        return false;
    }
};

// Shared field access.
const json::Value* T(const json::Value& op, const char* k) {
    const json::Value* t = op.find("target");
    return t ? t->find(k) : nullptr;
}
const json::Value* P(const json::Value& op, const char* k) {
    const json::Value* p = op.find("params");
    return p ? p->find(k) : nullptr;
}
bool is_set(const json::Value* v) {
    return v != nullptr && v->type != json::Value::Type::Null;
}
std::string str_or(const json::Value* v, const char* dflt) {
    return v && v->is_str() ? v->str : std::string(dflt);
}

// entry-not-in-archive error text shared by nearly every op.
bool require_entry(V& v, EntrySet& es, uint32_t key) {
    if (es.file_for(key) != nullptr) return true;
    v.err("entry " + hx(key) + " not in archive");
    return false;
}

// Decompress+walk with the Python's silent-on-failure semantics.
bool subs_of(EntrySet& es, uint32_t key, std::vector<SubEntry>& out) {
    try { out = walk_sub_entries(es.get(key)); return true; }
    catch (...) { return false; }
}

const SubEntry* gao_sub(const std::vector<SubEntry>& subs, uint32_t key) {
    for (const SubEntry& s : subs)
        if (s.key == key && s.ext == ".gao") return &s;
    return nullptr;
}

// ops_add_asset.added_asset_keys_before: keys minted by enabled add-asset
// ops on entry_key that precede `until` in list order (nullptr = all).
std::unordered_set<uint32_t> added_keys_before(
    const ModProject& proj, const json::Value* until, uint32_t entry_key,
    std::initializer_list<const char*> types) {
    std::unordered_set<uint32_t> keys;
    for (const json::Value* other : proj.enabled_operations()) {
        if (other == until) break;
        std::string t = str_or(other->find("op"), "");
        bool match = false;
        for (const char* want : types)
            if (t == want) { match = true; break; }
        if (!match || key_of(T(*other, "entry_key")) != entry_key) continue;
        keys.insert(key_of(P(*other, "new_key")));
    }
    return keys;
}

// ── per-op validators (live=False build-time shape) ──

void v_modify_transform(const json::Value& op, EntrySet& es, V& v,
                        bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t gao_key = key_of(T(op, "gao_key"));
    if (!require_entry(v, es, entry_key)) return;
    // _is_identity.
    const json::Value* wm = P(op, "world_matrix");
    bool identity;
    if (is_set(wm) && wm->is_arr() && wm->arr.size() == 16) {
        static const double kI[16] = {1, 0, 0, 0, 0, 1, 0, 0,
                                      0, 0, 1, 0, 0, 0, 0, 1};
        identity = true;
        for (size_t i = 0; i < 16; ++i)
            if (std::fabs(wm->arr[i].num - kI[i]) >= 1e-6) {
                identity = false;
                break;
            }
    } else {
        auto near3 = [&](const char* k, double want) {
            const json::Value* a = P(op, k);
            if (!is_set(a) || !a->is_arr()) return true;   // absent = default
            for (size_t i = 0; i < 3 && i < a->arr.size(); ++i)
                if (std::fabs(a->arr[i].num - want) >= 1e-6) return false;
            return true;
        };
        identity = near3("position", 0.0) && near3("rotation_euler_deg", 0.0)
                   && near3("scale", 1.0);
    }
    if (identity) v.warn("transform is identity (no-op)");
    if (live) return;
    try {
        std::vector<SubEntry> subs;
        if (!subs_of(es, entry_key, subs)) return;
        const SubEntry* g = gao_sub(subs, gao_key);
        if (g == nullptr)
            v.err("GAO " + hx(gao_key) + " not in entry " + hx(entry_key));
        else if (global_matrix_offset(g->data.data(), g->data.size()) < 0)
            v.err("GAO " + hx(gao_key) +
                  " has no parseable global_matrix slot");
    } catch (const std::exception& e) {
        v.warn(std::string("transform pre-check skipped: ") + e.what());
    }
}

void v_stub_mesh(const json::Value& op, EntrySet& es, V& v) {
    require_entry(v, es, key_of(T(op, "entry_key")));
}

void v_edit_light(const json::Value& op, EntrySet& es, V& v, bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t light_key = key_of(T(op, "light_key"));
    if (!require_entry(v, es, entry_key)) return;
    static const char* kFields[] = {"light_type", "diffuse", "specular",
                                    "near",       "far",     "inner_angle",
                                    "outer_angle", "intensity"};
    bool has_any = false;
    for (const char* f : kFields)
        if (is_set(P(op, f))) { has_any = true; break; }
    if (!has_any) v.warn("no light fields set (no-op)");
    if (live) return;
    try {
        std::vector<SubEntry> subs;
        if (!subs_of(es, entry_key, subs)) return;
        const SubEntry* t = nullptr;
        for (const SubEntry& s : subs)
            if (s.key == light_key && !s.gro_null && s.gro_type == 2) {
                t = &s;
                break;
            }
        if (t == nullptr)
            v.err("light " + hx(light_key) + " not in entry " + hx(entry_key));
        else if (!parse_light(t->data.data(), t->data.size()).ok)
            v.err("light " + hx(light_key) + " payload not parseable");
    } catch (const std::exception& e) {
        v.warn(std::string("light pre-check skipped: ") + e.what());
    }
}

void v_modify_gao_flags(const json::Value& op, EntrySet& es, V& v,
                        bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t gao_key = key_of(T(op, "gao_key"));
    if (!require_entry(v, es, entry_key)) return;
    uint32_t set_mask = key_of(P(op, "set_mask"));
    uint32_t clear_mask = key_of(P(op, "clear_mask"));
    if (set_mask == 0 && clear_mask == 0)
        v.warn("no bits to set or clear (no-op)");
    uint32_t overlap = set_mask & clear_mask;
    if (overlap)
        v.warn("bits " + hx(overlap) + " appear in both set and clear "
               "masks " JD_DASH " set wins");
    uint32_t dangerous_set = set_mask & ~kToggleableMask;
    uint32_t dangerous_clr = clear_mask & ~kToggleableMask;
    if (dangerous_set)
        v.warn("set bits " + hx(dangerous_set) + " are outside the tested "
               "toggle set " JD_DASH " proceed with care");
    if (dangerous_clr)
        v.warn("clear bits " + hx(dangerous_clr) + " are outside the tested "
               "toggle set " JD_DASH " proceed with care");
    if (live) return;
    try {
        std::vector<SubEntry> subs;
        if (!subs_of(es, entry_key, subs)) return;
        if (gao_sub(subs, gao_key) == nullptr)
            v.err("GAO " + hx(gao_key) + " not in entry " + hx(entry_key));
    } catch (const std::exception& e) {
        v.warn(std::string("flag pre-check skipped: ") + e.what());
    }
}

// remove_object is NATIVE-FIRST: entry + GAO existence + mode sanity.
void v_remove_object(const json::Value& op, EntrySet& es, V& v) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t gao_key = key_of(T(op, "gao_key"));
    std::string mode = str_or(P(op, "mode"), "hide");
    if (mode != "hide" && mode != "hide_no_collision")
        v.err("remove_object: unknown mode '" + mode + "'");
    if (!require_entry(v, es, entry_key)) return;
    std::vector<SubEntry> subs;
    if (!subs_of(es, entry_key, subs)) return;
    if (gao_sub(subs, gao_key) == nullptr)
        v.err("GAO " + hx(gao_key) + " not in entry " + hx(entry_key));
}

void v_add_object(const json::Value& op, EntrySet& es,
                  const std::string& jmod_dir, V& v, bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    if (!require_entry(v, es, entry_key)) return;
    std::string kind = str_or(P(op, "kind"), "cube");
    std::transform(kind.begin(), kind.end(), kind.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    if (kind == "clone" && !is_set(P(op, "source_gao_key")))
        v.err("clone needs source_gao_key (the GAO to duplicate)");
    if (kind == "model") {
        const std::string source = str_or(P(op, "source"), "");
        const std::string model_path = str_or(P(op, "model_path"), "");
        if (source.empty() && model_path.empty())
            v.err("model kind needs `source` asset or `model_path`");
        else if (!source.empty() && resolve_asset(jmod_dir, source).empty())
            v.err("missing asset " + source);
        else if (source.empty()) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(
                    std::filesystem::u8path(model_path), error))
                v.err("model file missing: " + model_path);
        }
    }
    if (v.any_error()) return;
    if (live) return;

    // The LOA stream-order simulation (ops_object._simulate_loa): run the
    // placement in memory against the BASE entry and stream-check the result.
    try {
        PlacePrep prep = build_place_op(
            op, es, jmod_dir, /*validation_simulation=*/true);
        const std::vector<uint8_t>& dec = es.get(entry_key);
        std::vector<uint32_t> extra_keys;
        for (const auto& kv : es.bf.files)
            if (kv.second.key != INVALID_KEY)
                extra_keys.push_back(kv.second.key);
        placer::PlacementResult r = placer::apply_placements_to_dec(
            dec, {prep.po}, /*geo_version=*/0, extra_keys, prep.seed);
        if (!r.ok) {
            v.err("add_object would fail: " + r.error);
            return;
        }
        for (const loa::Issue& issue : loa::validate_loa_stream(r.patched))
            v.issues.push_back({issue.level, issue.message, v.op_id});
    } catch (const std::exception& e) {
        v.warn(std::string("LOA pre-check skipped: ") + e.what());
    }
}

void v_retarget_material_texture(const json::Value& op, EntrySet& es,
                                 const ModProject& proj, V& v, bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t sub_key = key_of(T(op, "sub_key"));
    uint32_t new_key = key_of(P(op, "new_texture_key"));
    uint32_t layer = key_of(P(op, "layer"));
    const json::Value* l = P(op, "layer");
    if (l && l->is_num()) layer = uint32_t(std::max(0.0, l->num));
    if (!require_entry(v, es, entry_key)) return;
    if (live) return;
    std::vector<SubEntry> subs;
    if (!subs_of(es, entry_key, subs)) return;
    const SubEntry* target = matops::find_sub(subs, sub_key);
    if (target == nullptr) {
        v.err("sub-material " + hx(sub_key) + " not in " + hx(entry_key));
        return;
    }
    if (texture_layer_offset(target->data.data(), target->data.size(),
                             layer) < 0) {
        uint32_t kind = target->data.size() >= 4
                            ? matops::rd32(target->data, 0) : 0;
        char kb[16];
        std::snprintf(kb, sizeof kb, "0x%x", kind);
        char lb[16];
        std::snprintf(lb, sizeof lb, "%u", layer);
        v.err("sub-material " + hx(sub_key) + " (kind " + kb + ") has no "
              "texture layer " + lb + " " JD_DASH " out of range / "
              "unrecognised kind");
        return;
    }
    for (const SubEntry& s : subs)
        if (s.key == new_key) return;      // shipped in this entry — done
    if (added_keys_before(proj, &op, entry_key, {"add_texture"})
            .count(new_key))
        return;                             // minted by an EARLIER add op
    if (added_keys_before(proj, nullptr, entry_key, {"add_texture"})
            .count(new_key)) {
        v.err("texture " + hx(new_key) + " is added by an add_texture op "
              "that runs AFTER this retarget " JD_DASH " reorder the "
              "operations");
        return;
    }
    v.warn("texture " + hx(new_key) + " not in this entry's sub-resources "
           JD_DASH " assuming it's shared from another wow file (engine "
           "resolves at draw time)");
}

void v_set_multi_material_slot(const json::Value& op, EntrySet& es,
                               const ModProject& proj, V& v, bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t multi_key = key_of(T(op, "multi_key"));
    uint32_t new_key = key_of(P(op, "new_material_key"));
    long long slot = 0;
    const json::Value* s = P(op, "slot");
    if (s && s->is_num()) slot = (long long)s->num;
    if (!require_entry(v, es, entry_key)) return;
    if (live) return;
    std::vector<SubEntry> subs;
    if (!subs_of(es, entry_key, subs)) return;
    const SubEntry* target = matops::find_sub(subs, multi_key, 4);
    if (target == nullptr) {
        v.err("multi-material " + hx(multi_key) + " not in " + hx(entry_key));
        return;
    }
    if (slot < 0 || multi_slot_offset(target->data.data(),
                                      target->data.size(),
                                      uint32_t(slot)) < 0) {
        char sb[24];
        std::snprintf(sb, sizeof sb, "%lld", slot);
        v.err(std::string("slot ") + sb + " out of range for multi-material "
              + hx(multi_key));
        return;
    }
    for (const SubEntry& e : subs)
        if (e.key == new_key) return;
    if (added_keys_before(proj, &op, entry_key, {"add_material"})
            .count(new_key))
        return;
    if (added_keys_before(proj, nullptr, entry_key, {"add_material"})
            .count(new_key)) {
        v.err("material " + hx(new_key) + " is added by an add_material op "
              "that runs AFTER this slot edit " JD_DASH " reorder the "
              "operations");
        return;
    }
    v.warn("material " + hx(new_key) + " not in this entry's sub-resources "
           JD_DASH " assuming it's shared from another wow file");
}

void v_set_element_material(const json::Value& op, EntrySet& es,
                            const ModProject& proj, V& v, bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t geo_key = key_of(T(op, "geo_key"));
    uint32_t container_entry_key = key_of(T(op, "container_entry_key"));
    if (container_entry_key == 0) container_entry_key = entry_key;
    uint32_t multi_key = key_of(T(op, "multi_key"));
    uint32_t new_key = key_of(P(op, "new_material_key"));
    uint32_t element = key_of(P(op, "element"));
    const json::Value* el = P(op, "element");
    if (el && el->is_num()) element = uint32_t(std::max(0.0, el->num));
    if (!require_entry(v, es, entry_key)) return;
    if (es.file_for(container_entry_key) == nullptr) {
        v.err("container entry " + hx(container_entry_key)
              + " not in archive");
        return;
    }
    if (live) return;
    std::vector<SubEntry> geo_subs;
    if (!subs_of(es, entry_key, geo_subs)) return;
    std::vector<SubEntry> cont_local;
    const std::vector<SubEntry>* cont_subs = &geo_subs;
    if (container_entry_key != entry_key) {
        if (!subs_of(es, container_entry_key, cont_local)) return;
        cont_subs = &cont_local;
    }
    if (matops::find_sub(*cont_subs, multi_key, 4) == nullptr)
        v.err("multi-material " + hx(multi_key) + " not in "
              + hx(container_entry_key));
    if (elem::find_geo(geo_subs, geo_key, element) == nullptr) {
        char eb[16];
        std::snprintf(eb, sizeof eb, "%u", element);
        v.err("GEO " + hx(geo_key) + " with element " + eb + " not in "
              + hx(entry_key));
    }
    for (const SubEntry& s : *cont_subs)
        if (s.key == new_key) return;
    if (added_keys_before(proj, &op, container_entry_key, {"add_material"})
            .count(new_key))
        return;
    if (added_keys_before(proj, nullptr, container_entry_key,
                          {"add_material"}).count(new_key)) {
        v.err("material " + hx(new_key) + " is added by an add_material op "
              "that runs AFTER this element edit " JD_DASH " reorder the "
              "operations");
        return;
    }
    v.warn("material " + hx(new_key) + " not in the container's bin "
           JD_DASH " assuming it's shared from another wow file");
}

void v_set_material_flags(const json::Value& op, EntrySet& es, V& v,
                          bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t sub_key = key_of(T(op, "sub_key"));
    uint32_t layer = 0;
    const json::Value* l = P(op, "layer");
    if (l && l->is_num()) layer = uint32_t(std::max(0.0, l->num));
    if (!require_entry(v, es, entry_key)) return;
    if (live) return;
    std::vector<SubEntry> subs;
    if (!subs_of(es, entry_key, subs)) return;
    const SubEntry* target = matops::find_sub(subs, sub_key);
    if (target == nullptr) {
        v.err("sub-material " + hx(sub_key) + " not in " + hx(entry_key));
        return;
    }
    if (material_ulflags_offset(target->data.data(), target->data.size(),
                                layer) < 0) {
        char lb[16];
        std::snprintf(lb, sizeof lb, "%u", layer);
        v.err("sub-material " + hx(sub_key) + " is not a recognised "
              "material (or has no layer " + lb + " to edit)");
    }
}

void v_modify_gao_material(const json::Value& op, EntrySet& es, V& v,
                           bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t gao_key = key_of(T(op, "gao_key"));
    uint32_t material_key = key_of(P(op, "material_key"));
    if (!require_entry(v, es, entry_key)) return;
    if (live) return;
    try {
        std::vector<SubEntry> subs;
        if (!subs_of(es, entry_key, subs)) return;
        const SubEntry* g = gao_sub(subs, gao_key);
        if (g == nullptr)
            v.err("GAO " + hx(gao_key) + " not in entry " + hx(entry_key));
        else if (visual_block_offset(g->data.data(), g->data.size()) < 0)
            v.err("GAO " + hx(gao_key) + " has no visual block " JD_DASH
                  " cannot retarget material");
        if (es.file_for(material_key) == nullptr)
            v.warn("material key " + hx(material_key) + " is not in the "
                   "archive's FAT; the engine will see an invalid reference "
                   "and fall back to a default");
    } catch (const std::exception& e) {
        v.warn(std::string("material pre-check skipped: ") + e.what());
    }
}

// mesh_swap.validate_glb_skin's warning list (the deep half of
// ReplaceMesh.validate). Structural failures throw like the Python (the
// caller wraps them into the "skin validation skipped" warning).
std::vector<std::string> glb_skin_warnings(EntrySet& es, uint32_t entry_key,
                                           uint32_t geo_key,
                                           const std::string& glb_path) {
    std::vector<uint8_t>& dec = es.get(entry_key);
    std::vector<SubEntry> subs = walk_sub_entries(dec);
    const SubEntry* target = pick_geo_sub(subs, geo_key);
    if (target == nullptr) {
        char b[96];
        std::snprintf(b, sizeof b,
                      "No .geo sub-entry with key 0x%08x in entry", geo_key);
        throw std::runtime_error(b);
    }
    GeoInfo orig_geo = parse_geometry(target->data.data(),
                                      target->data.size());
    if (!orig_geo.ok)
        throw std::runtime_error("Could not parse original geometry");
    size_t orig_bones =
        orig_geo.skin_present ? orig_geo.skin_bones.size() : 0;

    // Original bone names via the host GAO's gizmo_ptrs.
    std::unordered_map<uint32_t, const SubEntry*> by_key;
    for (const SubEntry& s : subs) by_key[s.key] = &s;
    std::vector<std::string> orig_bone_names;
    const SubEntry* host = host_gao_sub(subs, geo_key);
    if (host != nullptr && orig_bones > 0) {
        GaoInfo gi = parse_gao_full(host->data.data(), host->data.size());
        size_t n_gizmos = gi.gizmo_flat.size() / 2;
        for (const GeoBone& bo : orig_geo.skin_bones) {
            std::string nm;
            if (gi.ok && size_t(bo.bone_idx) < n_gizmos) {
                uint32_t gk = gi.gizmo_flat[size_t(bo.bone_idx) * 2];
                auto it = by_key.find(gk);
                if (it != by_key.end() && it->second->ext == ".gao") {
                    GaoInfo hdr = parse_gao_full(it->second->data.data(),
                                                 it->second->data.size());
                    if (hdr.ok && !hdr.name.empty()) {
                        nm = hdr.name;
                        size_t a = nm.find_first_not_of(" \t\r\n\f\v");
                        size_t bb = nm.find_last_not_of(" \t\r\n\f\v");
                        nm = a == std::string::npos
                                 ? "" : nm.substr(a, bb - a + 1);
                        nm.erase(std::remove(nm.begin(), nm.end(), '\0'),
                                 nm.end());
                        if (nm.size() >= 4 &&
                            nm.compare(nm.size() - 4, 4, ".gao") == 0)
                            nm.resize(nm.size() - 4);
                    }
                }
            }
            if (nm.empty()) {
                char b[24];
                std::snprintf(b, sizeof b, "bone_%03d", bo.bone_idx);
                nm = b;
            }
            orig_bone_names.push_back(nm);
        }
    } else {
        for (const GeoBone& bo : orig_geo.skin_bones) {
            char b[24];
            std::snprintf(b, sizeof b, "bone_%03d", bo.bone_idx);
            orig_bone_names.push_back(b);
        }
    }

    // GLB skin metadata (joint names + extras bone_idx presence).
    std::vector<std::string> glb_names;
    std::vector<bool> glb_has_bidx;
    {
        std::ifstream f(std::filesystem::u8path(glb_path), std::ios::binary);
        if (!f) throw std::runtime_error("Not a valid GLB file");
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        gltf::GlbDoc doc = gltf::parse_glb(data.data(), data.size());
        const json::Value* skins = doc.gltf.find("skins");
        const json::Value* nodes = doc.gltf.find("nodes");
        if (skins != nullptr && skins->is_arr() && !skins->arr.empty()) {
            const json::Value* joints = skins->arr[0].find("joints");
            if (joints != nullptr && joints->is_arr())
                for (const json::Value& jv : joints->arr) {
                    long long ji = jv.is_num() ? (long long)jv.num : -1;
                    std::string nm;
                    bool has_bidx = false;
                    if (nodes != nullptr && nodes->is_arr() && ji >= 0 &&
                        size_t(ji) < nodes->arr.size()) {
                        const json::Value& n = nodes->arr[size_t(ji)];
                        const json::Value* nn = n.find("name");
                        if (nn != nullptr && nn->is_str() && !nn->str.empty())
                            nm = nn->str;
                        const json::Value* ex = n.find("extras");
                        const json::Value* bi =
                            ex != nullptr ? ex->find("bone_idx") : nullptr;
                        has_bidx = bi != nullptr &&
                                   bi->type != json::Value::Type::Null;
                    }
                    if (nm.empty()) {
                        char b[24];
                        std::snprintf(b, sizeof b, "node_%lld", ji);
                        nm = b;
                    }
                    glb_names.push_back(nm);
                    glb_has_bidx.push_back(has_bidx);
                }
        }
    }
    bool glb_has_skin = !glb_names.empty();

    std::vector<std::string> warnings;
    char b[256];
    if (!glb_has_skin && orig_bones > 0) {
        std::snprintf(b, sizeof b,
                      "GLB has no skin " JD_DASH " the new mesh will load "
                      "unskinned, but the original geometry is skinned to "
                      "%zu bones. The mesh will not deform with the "
                      "skeleton.", orig_bones);
        warnings.push_back(b);
    }
    if (glb_has_skin) {
        if (glb_names.size() < orig_bones) {
            std::snprintf(b, sizeof b,
                          "GLB has only %zu joints, but the original "
                          "geometry binds to %zu bones. Vertices weighted to "
                          "missing bones will fall back to bone 0.",
                          glb_names.size(), orig_bones);
            warnings.push_back(b);
        } else if (glb_names.size() > orig_bones) {
            std::snprintf(b, sizeof b,
                          "GLB has %zu joints, more than the original's %zu "
                          "bones. Extra joints will be ignored by the "
                          "skeleton.", glb_names.size(), orig_bones);
            warnings.push_back(b);
        }
        std::unordered_map<std::string, size_t> orig_lower;
        for (size_t i = 0; i < orig_bone_names.size(); ++i) {
            std::string lo = orig_bone_names[i];
            for (char& c : lo) c = char(std::tolower(uint8_t(c)));
            orig_lower.emplace(lo, i);
        }
        size_t name_matches = 0;
        for (const std::string& gname : glb_names) {
            std::string lo = gname;
            for (char& c : lo) c = char(std::tolower(uint8_t(c)));
            if (orig_lower.count(lo)) ++name_matches;
        }
        size_t none_extras = 0;
        for (bool hb : glb_has_bidx)
            if (!hb) ++none_extras;
        if (none_extras == glb_has_bidx.size() && !glb_has_bidx.empty())
            warnings.push_back(
                "GLB joints have no embedded bone_idx (extras), so bone "
                "indices will default to joint position in the GLB. This "
                "usually means the GLB was authored from scratch in Blender "
                "rather than round-tripped through this toolkit's exporter. "
                "Joint order must match the original skeleton, or skinning "
                "will be wrong.");
        if (name_matches == 0 && !orig_bone_names.empty())
            warnings.push_back(
                "No GLB joint name matches any original bone name. The mesh "
                "will likely deform incorrectly. Re-export from Blender "
                "preserving the source armature's bone names.");
    }
    return warnings;
}

void v_replace_mesh(const json::Value& op, EntrySet& es, bool has_assets,
                    const std::string& jmod_dir, V& v, bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t sub_key = key_of(T(op, "sub_key"));
    if (!require_entry(v, es, entry_key)) return;
    std::string source = str_or(P(op, "source"), "");
    if (source.empty()) {
        v.err("no source asset set");
        return;
    }
    std::string src_path;
    if (has_assets) {
        src_path = resolve_asset(jmod_dir, source);
        if (src_path.empty()) {
            v.err("missing asset " + source);
            return;
        }
    }
    if (live) return;
    if (src_path.empty()) return;
    std::string lo = src_path;
    for (char& c : lo) c = char(std::tolower(uint8_t(c)));
    if (lo.size() < 4 || lo.compare(lo.size() - 4, 4, ".glb") != 0) {
        size_t slash = src_path.find_last_of("/\\");
        std::string base = slash == std::string::npos
                               ? src_path : src_path.substr(slash + 1);
        v.warn("source '" + base + "' is not a .glb file");
        return;
    }
    const json::Value* ss = P(op, "strict_skin");
    bool strict = !(ss != nullptr && ss->type == json::Value::Type::Bool &&
                    !ss->b);
    std::vector<std::string> warnings;
    try {
        warnings = glb_skin_warnings(es, entry_key, sub_key, src_path);
    } catch (const std::exception& e) {
        v.warn(std::string("skin validation skipped: ") + e.what());
        return;
    }
    for (const std::string& w : warnings)
        v.issues.push_back({strict ? "error" : "warning",
                            "skin: " + w + " (strict_skin=" +
                                (strict ? "True" : "False") + ")",
                            v.op_id});
}

void v_source_asset(const json::Value& op, EntrySet& es, bool has_assets,
                    const std::string& jmod_dir, V& v) {
    // Shared shape of ReplaceTexture / ReplaceEntryRaw validate.
    require_entry(v, es, key_of(T(op, "entry_key")));
    std::string source = str_or(P(op, "source"), "");
    if (source.empty())
        v.err("no source asset set");
    else if (has_assets && resolve_asset(jmod_dir, source).empty())
        v.err("missing asset " + source);
}

void v_add_texture(const json::Value& op, EntrySet& es, bool has_assets,
                   const std::string& jmod_dir, V& v, bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t new_key = key_of(P(op, "new_key"));
    v_source_asset(op, es, has_assets, jmod_dir, v);
    std::string encode = str_or(P(op, "encode"), "7");
    if (encode != "0" && encode != "5" && encode != "7")
        v.err("unsupported encode '" + encode + "' (use 0, 5 or 7)");
    if (new_key < 0x7A000000u || new_key > 0x7AFFFFFFu)
        v.warn("new key " + hx(new_key) + " is outside the modded 0x7A "
               "range " JD_DASH " may collide with shipped content");
    if (live || v.any()) return;
    std::vector<SubEntry> subs;
    if (!subs_of(es, entry_key, subs)) return;
    bool dup = false, has_donor = false;
    for (const SubEntry& s : subs) {
        if (s.key == new_key) dup = true;
        if (assetadd::is_tex_full(s.data)) has_donor = true;
    }
    if (dup)
        v.err("key " + hx(new_key) + " already exists in entry "
              + hx(entry_key));
    if (!has_donor)
        v.err("entry " + hx(entry_key) + " ships no textures " JD_DASH
              " pick a bin that already has a texture wave");
}

void v_add_material(const json::Value& op, EntrySet& es,
                    const ModProject& proj, V& v, bool live) {
    uint32_t entry_key = key_of(T(op, "entry_key"));
    uint32_t new_key = key_of(P(op, "new_key"));
    uint32_t donor_key = key_of(P(op, "donor_key"));
    uint32_t texture_key = key_of(P(op, "texture_key"));
    if (!require_entry(v, es, entry_key)) return;
    if (live) return;
    std::vector<SubEntry> subs;
    if (!subs_of(es, entry_key, subs)) return;
    bool dup = false, donor_found = false, any_donor = false,
         tex_in_bin = false;
    for (const SubEntry& s : subs) {
        if (s.key == new_key) dup = true;
        if (s.key == texture_key) tex_in_bin = true;
        if (texture_layer_offset(s.data.data(), s.data.size(), 0) >= 0) {
            any_donor = true;
            if (s.key == donor_key) donor_found = true;
        }
    }
    if (dup)
        v.err("key " + hx(new_key) + " already exists in entry "
              + hx(entry_key));
    if (donor_key != 0 && !donor_found)
        v.err("donor sub-material " + hx(donor_key) + " not in "
              + hx(entry_key));
    else if (!any_donor)
        v.err("entry " + hx(entry_key) + " has no sub-material to clone");
    if (!tex_in_bin
        && !added_keys_before(proj, &op, entry_key, {"add_texture"})
                .count(texture_key)) {
        if (added_keys_before(proj, nullptr, entry_key, {"add_texture"})
                .count(texture_key))
            v.err("texture " + hx(texture_key) + " is added by an "
                  "add_texture op that runs AFTER this one " JD_DASH
                  " reorder the operations");
        else
            v.warn("texture " + hx(texture_key) + " not in this entry's "
                   "sub-resources " JD_DASH " assuming it's shared from "
                   "another wow file");
    }
}

void v_add_zone_dependency(const json::Value& op, EntrySet& es, V& v) {
    uint32_t wol_key = key_of(T(op, "wol_entry_key"));
    const json::Value* dk = P(op, "dep_keys");
    if (dk == nullptr || !dk->is_arr() || dk->arr.empty())
        v.warn("no deps to add");
    BFFile* fi = es.file_for(wol_key);
    if (fi == nullptr) {
        v.err("target wol " + hx(wol_key) + " not in archive");
        return;
    }
    if (fi->name.find("_wol_") == std::string::npos)
        v.warn("target " + hx(wol_key) + " (name='" + fi->name + "') does "
               "not look like a _wol_ entry " JD_DASH " adding deps to it "
               "will likely corrupt the entry");
}

void v_create_zone(const json::Value& op, EntrySet& es, V& v, bool live) {
    std::string new_name = str_or(P(op, "new_name"), "");
    std::string template_zone = str_or(P(op, "template_zone"), "");
    if (new_name.empty()) v.err("new_name cannot be empty");
    if (template_zone.empty()) v.err("template_zone cannot be empty");
    std::vector<ZoneInfo> zones = discover_zones(es.bf);
    const ZoneInfo* tpl = nullptr;
    for (const ZoneInfo& z : zones)
        if (z.name == template_zone) { tpl = &z; break; }
    if (tpl == nullptr) {
        v.err("template zone " + python_repr_string(template_zone) +
              " not in archive");
        return;
    }
    for (unsigned char c : new_name)
        if (c > 0x7F) {
            v.err("new_name " + python_repr_string(new_name) +
                  " contains non-ASCII chars");
            break;
        }
    if (utf8_codepoints(new_name).size() > 31)
        v.err("new_name " + python_repr_string(new_name) +
              " exceeds 31 chars (wow record's "
              "name field is 32 bytes including NUL)");
    if (live) return;
    // Build-time: mirror CreateZone.validate's direct allocation and clone
    // simulation (extra-dependency insertion is an apply-time concern).
    try {
        LzoResult wr = decompress_lzo(es.bf.read_data(tpl->wow_index));
        LzoResult lr = decompress_lzo(es.bf.read_data(tpl->wol_index));
        const json::Value* prefix_value = P(op, "new_map_prefix");
        const uint32_t prefix = prefix_value
                                    ? (key_of(prefix_value) & 0xffu)
                                    : zonecreate::MODDED_KEY_PREFIX;
        const zonecreate::AllocatedZoneKeys allocated =
            zonecreate::alloc_new_zone_bf_keys(es.bf, prefix);
        std::unordered_set<uint32_t> used;
        for (const auto& item : es.bf.files)
            if (item.second.key != INVALID_KEY) used.insert(item.second.key);
        zonecreate::clone_zone(wr.data, lr.data, new_name, allocated.low16,
                               used, prefix);
    } catch (const zonecreate::Error& e) {
        v.err(std::string("clone simulation failed: ") + e.python_type +
              ": " + e.what());
    } catch (const std::exception& e) {
        v.err(std::string("clone simulation failed: RuntimeError: ") +
              e.what());
    }
}

// rescale_rli is NATIVE-FIRST: filter + identity-grade sanity.
void v_rescale_rli(const json::Value& op, EntrySet& es, V& v) {
    std::string district = str_or(P(op, "district"), "");
    if (district.empty()) {
        v.warn("district is empty " JD_DASH " the grade will touch every "
               "entry in the archive");
    } else {
        bool any = false;
        for (const auto& kv : es.bf.files) {
            const BFFile& fi = kv.second;
            if (fi.name.empty() || fi.key == INVALID_KEY) continue;
            if (fi.name.find(district) != std::string::npos) {
                any = true;
                break;
            }
        }
        if (!any)
            v.err("rescale_rli: district filter '" + district
                  + "' matches no entries");
    }
    auto num = [&](const char* k, double d) {
        const json::Value* n = P(op, k);
        return n && n->is_num() ? n->num : d;
    };
    bool tint_id = true;
    const json::Value* t = P(op, "tint");
    if (t && t->is_arr() && t->arr.size() >= 3)
        for (size_t i = 0; i < 3; ++i)
            if (int(t->arr[i].num) != 255) { tint_id = false; break; }
    if (num("brightness", 1.0) == 1.0 && num("contrast", 1.0) == 1.0
        && tint_id)
        v.warn("grade parameters are identity (no-op)");
}

// The per-op dispatch. Unknown ops warn like Python UnknownOperation.
void validate_one(const json::Value& op, EntrySet& es, const ModProject& proj,
                  bool has_assets, const std::string& jmod_dir,
                  std::vector<BuildIssue>& issues, bool live) {
    std::string type = str_or(op.find("op"), "");
    V v(issues, op);
    if (type == "modify_transform") v_modify_transform(op, es, v, live);
    else if (type == "stub_mesh") v_stub_mesh(op, es, v);
    else if (type == "edit_light") v_edit_light(op, es, v, live);
    else if (type == "modify_gao_flags")
        v_modify_gao_flags(op, es, v, live);
    else if (type == "remove_object") v_remove_object(op, es, v);
    else if (type == "add_object")
        v_add_object(op, es, jmod_dir, v, live);
    else if (type == "retarget_material_texture")
        v_retarget_material_texture(op, es, proj, v, live);
    else if (type == "set_multi_material_slot")
        v_set_multi_material_slot(op, es, proj, v, live);
    else if (type == "set_element_material")
        v_set_element_material(op, es, proj, v, live);
    else if (type == "set_material_flags")
        v_set_material_flags(op, es, v, live);
    else if (type == "modify_gao_material")
        v_modify_gao_material(op, es, v, live);
    else if (type == "replace_texture" || type == "replace_entry_raw" ||
             type == "replace_animation")
        v_source_asset(op, es, has_assets, jmod_dir, v);
    else if (type == "add_texture")
        v_add_texture(op, es, has_assets, jmod_dir, v, live);
    else if (type == "add_material")
        v_add_material(op, es, proj, v, live);
    else if (type == "add_zone_dependency") v_add_zone_dependency(op, es, v);
    else if (type == "create_zone") v_create_zone(op, es, v, live);
    else if (type == "rescale_rli") v_rescale_rli(op, es, v);
    else if (type == "add_object_collision")
        require_entry(v, es, key_of(T(op, "entry_key")));
    else if (type == "replace_mesh")
        v_replace_mesh(op, es, has_assets, jmod_dir, v, live);
    else
        v.warn("unknown op type " + python_repr_string(type)
               + "; will be skipped at build");
}

// Operation.asset_refs for the store-existence pass.
std::vector<std::string> asset_refs_of(const json::Value& op) {
    std::string type = str_or(op.find("op"), "");
    if (type == "replace_texture" || type == "replace_entry_raw" ||
        type == "replace_animation"
        || type == "add_texture" || type == "add_object"
        || type == "replace_mesh") {
        std::string s = str_or(P(op, "source"), "");
        if (!s.empty()) return {s};
    }
    return {};
}

}  // namespace vals

struct FileCopyResult {
    bool ok = false;
    std::string error;
};

#ifdef _WIN32
std::string windows_error_text(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = length && buffer
                               ? std::wstring(buffer, buffer + length)
                               : std::wstring(L"Windows error");
    if (buffer) LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ' || message.back() == L'.'))
        message.pop_back();
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, message.c_str(),
                                          int(message.size()), nullptr, 0,
                                          nullptr, nullptr);
    std::string utf8(size_t(std::max(0, bytes)), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, message.c_str(), int(message.size()),
                            &utf8[0], bytes, nullptr, nullptr);
    return "[WinError " + std::to_string(code) + "] " + utf8;
}
#endif

FileCopyResult copy_file_bytes(const std::string& src,
                               const std::string& dst) {
    namespace fs = std::filesystem;
    const fs::path source = fs::u8path(src);
    const fs::path destination = fs::u8path(dst);
    std::error_code error;
    if (fs::exists(destination, error)) {
        error.clear();
        if (fs::equivalent(source, destination, error) && !error) {
#ifdef _WIN32
            // shutil.copy2's Windows fast-copy path reports the sharing
            // violation raised while trying to open one file for both roles.
            return {false, windows_error_text(ERROR_SHARING_VIOLATION)};
#else
            return {false, "'" + src + "' and '" + dst +
                               "' are the same file"};
#endif
        }
    }

    error.clear();
#ifdef _WIN32
    if (!CopyFileW(source.c_str(), destination.c_str(), FALSE)) {
        const DWORD code = GetLastError();
        error = std::error_code(static_cast<int>(code),
                                std::system_category());
        return {false, windows_error_text(code)};
#else
    if (!fs::copy_file(source, destination,
                       fs::copy_options::overwrite_existing, error)) {
#endif
        return {false, error ? error.message()
                             : "copy operation did not create an output"};
    }

    // shutil.copy2 applies the source stat record after copying. CopyFileW
    // already carries the precise Windows timestamps; MinGW's conversion via
    // filesystem::file_time_type rounds them to whole seconds, so do not
    // overwrite that more accurate result on Windows.
#ifndef _WIN32
    const fs::file_status source_status = fs::status(source, error);
    if (error) return {false, error.message()};
    fs::permissions(destination, source_status.permissions(),
                    fs::perm_options::replace, error);
    if (error) return {false, error.message()};
    const fs::file_time_type source_time = fs::last_write_time(source, error);
    if (error) return {false, error.message()};
    fs::last_write_time(destination, source_time, error);
    if (error) return {false, error.message()};
#endif
    return {true, ""};
}

}  // namespace

namespace {

int64_t conflict_integer(const json::Value* value) {
    if (value == nullptr) return 0;
    if (value->type == json::Value::Type::Bool) return value->b ? 1 : 0;
    if (value->is_num()) return exact_integer_long_long(*value);
    if (value->is_str() && !value->str.empty())
        return std::strtoll(value->str.c_str(), nullptr, 16);
    return 0;
}

int64_t decimal_integer(const json::Value* value) {
    if (value == nullptr) return 0;
    if (value->type == json::Value::Type::Bool) return value->b ? 1 : 0;
    if (value->is_num()) return exact_integer_long_long(*value);
    if (value->is_str() && !value->str.empty())
        return std::strtoll(value->str.c_str(), nullptr, 10);
    return 0;
}

uint32_t project_key(const json::Value* value) {
    if (value && value->is_num() && value->number_is_integer &&
        !value->integer_text.empty())
        return uint32_t(exact_integer_modulo(*value, uint64_t(1) << 32));
    return static_cast<uint32_t>(conflict_integer(value));
}

std::string project_hex(uint32_t value) {
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08x", value);
    return hex;
}

bool project_truthy(const json::Value* value) {
    if (value == nullptr || value->type == json::Value::Type::Null)
        return false;
    if (value->type == json::Value::Type::Bool) return value->b;
    if (value->is_num())
        return value->number_is_integer && !value->integer_text.empty()
                   ? !exact_integer_zero(*value)
                   : value->num != 0.0;
    if (value->is_str()) return !value->str.empty();
    if (value->is_arr()) return !value->arr.empty();
    if (value->is_obj()) return !value->obj.empty();
    return false;
}

std::string project_string(const json::Value* value,
                           const std::string& fallback = "") {
    if (value == nullptr) return fallback;
    if (value->is_str()) return value->str;
    if (value->type == json::Value::Type::Null) return "None";
    if (value->type == json::Value::Type::Bool)
        return value->b ? "True" : "False";
    if (value->is_num()) {
        if (value->number_is_integer && !value->integer_text.empty())
            return value->integer_text;
        std::ostringstream out;
        if (std::isfinite(value->num) && std::floor(value->num) == value->num)
            out << static_cast<int64_t>(value->num);
        else
            out << value->num;
        return out.str();
    }
    return fallback;
}

json::Value project_number_array(const json::Value* value,
                                 std::initializer_list<double> fallback) {
    json::Value result = json::make_arr();
    if (value && value->is_arr()) {
        for (const json::Value& item : value->arr) {
            if (item.is_num()) result.arr.push_back(json::make_num(item.num));
            else if (item.is_str())
                result.arr.push_back(json::make_num(
                    std::strtod(item.str.c_str(), nullptr)));
            else
                result.arr.push_back(json::make_num(0.0));
        }
    } else {
        for (double item : fallback)
            result.arr.push_back(json::make_num(item));
    }
    return result;
}

bool project_array_any_nonzero(const json::Value& value) {
    if (!value.is_arr()) return false;
    for (const json::Value& item : value.arr)
        if (item.is_num() && std::fabs(item.num) > 1e-6) return true;
    return false;
}

bool project_array_any_not_one(const json::Value& value) {
    if (!value.is_arr()) return false;
    for (const json::Value& item : value.arr)
        if (!item.is_num() || std::fabs(item.num - 1.0) > 1e-6) return true;
    return false;
}

std::string project_iso_now() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

json::Value project_common_operation(const json::Value& operation,
                                     const std::string& type) {
    json::Value result = json::make_obj();
    const json::Value* id = operation.find("id");
    const json::Value* enabled = operation.find("enabled");
    const json::Value* label = operation.find("label");
    const json::Value* created = operation.find("created");
    result.obj["id"] = id ? *id : json::make_str("");
    result.obj["op"] = json::make_str(type);
    result.obj["enabled"] = json::make_bool(
        enabled == nullptr ? true : project_truthy(enabled));
    result.obj["label"] = label ? *label : json::make_str("");
    result.obj["created"] = created && project_truthy(created)
                                ? *created
                                : json::make_str(project_iso_now());
    return result;
}

json::Value project_target(std::initializer_list<std::pair<const char*, uint32_t>>
                               fields) {
    json::Value target = json::make_obj();
    for (const auto& field : fields)
        target.obj[field.first] = json::make_str(project_hex(field.second));
    return target;
}

ConflictSignature conflict_signature(
    const std::string& kind,
    std::initializer_list<ConflictValue> values) {
    ConflictSignature signature;
    signature.kind = kind;
    signature.values.assign(values.begin(), values.end());
    return signature;
}

std::string conflict_python_repr(const std::string& value) {
    const bool use_double = value.find('\'') != std::string::npos &&
                            value.find('"') == std::string::npos;
    const char quote = use_double ? '"' : '\'';
    std::string out(1, quote);
    static const char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        if (ch == '\\' || ch == static_cast<unsigned char>(quote)) {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch < 0x20 || ch == 0x7f) {
            out += "\\x";
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0f]);
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    out.push_back(quote);
    return out;
}

}  // namespace

std::vector<ConflictSignature> operation_conflict_signatures(
    const json::Value& operation) {
    const std::string type = vals::str_or(operation.find("op"), "");
    auto target = [&](const char* field) {
        return ConflictValue::from_integer(
            project_key(vals::T(operation, field)));
    };
    auto decimal_param = [&](const char* field) {
        return ConflictValue::from_integer(
            decimal_integer(vals::P(operation, field)));
    };
    auto key_param = [&](const char* field) {
        return ConflictValue::from_integer(
            project_key(vals::P(operation, field)));
    };
    auto text = [&](const char* field) {
        return ConflictValue::from_string(
            vals::str_or(vals::P(operation, field), ""));
    };
    if (type == "modify_transform")
        return {conflict_signature("gao_transform",
                                   {target("entry_key"), target("gao_key")})};
    if (type == "add_object_collision")
        return {conflict_signature("add_collision",
                                   {target("entry_key"), target("gao_key")})};
    if (type == "stub_mesh")
        return {conflict_signature("sub",
                                   {target("entry_key"), target("sub_key")})};
    if (type == "edit_light")
        return {conflict_signature("light_edit",
                                   {target("entry_key"), target("light_key")})};
    if (type == "modify_gao_flags")
        return {conflict_signature("gao_flags",
                                   {target("entry_key"), target("gao_key")})};
    if (type == "remove_object")
        return {conflict_signature("remove_object",
                                   {target("entry_key"), target("gao_key")})};
    if (type == "add_object") {
        std::string kind = vals::str_or(vals::P(operation, "kind"), "cube");
        std::transform(kind.begin(), kind.end(), kind.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });
        std::string name = vals::str_or(vals::P(operation, "name"), "");
        if (name.empty()) name = "JadePlaced_" + kind;
        return {conflict_signature(
            "zone_add",
            {target("entry_key"), ConflictValue::from_string(name)})};
    }
    if (type == "retarget_material_texture")
        return {conflict_signature(
            "sub",
            {target("entry_key"), target("sub_key"),
             ConflictValue::from_integer(std::max<int64_t>(
                 0, decimal_integer(vals::P(operation, "layer"))))})};
    if (type == "set_multi_material_slot")
        return {conflict_signature(
            "multi_slot",
            {target("entry_key"), target("multi_key"),
             decimal_param("slot")})};
    if (type == "set_element_material")
        return {conflict_signature(
            "geo_elem_mat",
            {target("entry_key"), target("geo_key"),
             decimal_param("element")})};
    if (type == "set_material_flags")
        return {conflict_signature(
            "sub_flags",
            {target("entry_key"), target("sub_key"),
             ConflictValue::from_integer(std::max<int64_t>(
                 0, decimal_integer(vals::P(operation, "layer"))))})};
    if (type == "modify_gao_material")
        return {conflict_signature("gao_material",
                                   {target("entry_key"), target("gao_key")})};
    if (type == "replace_texture" || type == "replace_mesh" ||
        type == "replace_animation")
        return {conflict_signature("sub",
                                   {target("entry_key"), target("sub_key")})};
    if (type == "add_texture" || type == "add_material")
        return {conflict_signature("sub",
                                   {target("entry_key"), key_param("new_key")})};
    if (type == "replace_entry_raw")
        return {conflict_signature("entry", {target("entry_key")})};
    if (type == "add_zone_dependency") {
        std::vector<ConflictSignature> signatures;
        const json::Value* dep_keys = vals::P(operation, "dep_keys");
        if (dep_keys && dep_keys->is_arr()) {
            for (const json::Value& dep_key : dep_keys->arr) {
                ConflictSignature signature = conflict_signature(
                    "wol_deps",
                    {target("wol_entry_key"), ConflictValue::from_integer(
                                                   project_key(&dep_key))});
                if (std::find(signatures.begin(), signatures.end(), signature) ==
                    signatures.end()) {
                    signatures.push_back(std::move(signature));
                }
            }
        }
        return signatures;
    }
    if (type == "create_zone")
        return {conflict_signature("create_zone", {text("new_name")})};
    if (type == "rescale_rli")
        return {conflict_signature("rescale_rli", {text("district")})};
    return {};
}

std::vector<ProjectConflict> project_conflicts(const ModProject& project) {
    std::vector<ProjectConflict> groups;
    for (const json::Value* operation : project.enabled_operations()) {
        for (const ConflictSignature& signature :
             operation_conflict_signatures(*operation)) {
            auto found = std::find_if(
                groups.begin(), groups.end(),
                [&](const ProjectConflict& conflict) {
                    return conflict.signature == signature;
                });
            if (found == groups.end()) {
                groups.push_back({signature, {op_id_of(*operation)}});
            } else {
                found->op_ids.push_back(op_id_of(*operation));
            }
        }
    }
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                                [](const ProjectConflict& conflict) {
                                    return conflict.op_ids.size() < 2;
                                }),
                 groups.end());
    return groups;
}

std::string format_conflict_signature(const ConflictSignature& signature) {
    std::string formatted = signature.kind + "(";
    for (size_t index = 0; index < signature.values.size(); ++index) {
        if (index) formatted += ", ";
        const ConflictValue& value = signature.values[index];
        if (value.type == ConflictValue::Type::String) {
            formatted += conflict_python_repr(value.string);
        } else {
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%08x",
                          static_cast<uint32_t>(value.integer));
            formatted += hex;
        }
    }
    return formatted + ")";
}

std::string operation_target_summary(const json::Value& operation) {
    const std::string type = vals::str_or(operation.find("op"), "");
    auto target_key = [&](const char* field) {
        return project_key(vals::T(operation, field));
    };
    auto param_key = [&](const char* field) {
        return project_key(vals::P(operation, field));
    };
    auto param_int = [&](const char* field) {
        return decimal_integer(vals::P(operation, field));
    };
    auto param_text = [&](const char* field, const char* fallback = "") {
        return vals::str_or(vals::P(operation, field), fallback);
    };

    const uint32_t entry = target_key("entry_key");
    if (type == "replace_texture")
        return "texture in " + project_hex(entry) + " sub " +
               project_hex(target_key("sub_key"));
    if (type == "replace_mesh")
        return "mesh in " + project_hex(entry) + " sub " +
               project_hex(target_key("sub_key"));
    if (type == "stub_mesh")
        return "stub mesh in " + project_hex(entry) + " sub " +
               project_hex(target_key("sub_key"));
    if (type == "replace_animation")
        return "anim in " + project_hex(entry) + " sub " +
               project_hex(target_key("sub_key"));
    if (type == "edit_light")
        return "light " + project_hex(target_key("light_key")) +
               " in entry " + project_hex(entry);
    if (type == "modify_transform")
        return "transform GAO " + project_hex(target_key("gao_key")) +
               " in entry " + project_hex(entry);
    if (type == "add_object_collision")
        return "add collision to GAO " +
               project_hex(target_key("gao_key")) + " in entry " +
               project_hex(entry);
    if (type == "modify_gao_flags") {
        const uint32_t set_mask = param_key("set_mask");
        const uint32_t clear_mask = param_key("clear_mask");
        return "flags GAO " + project_hex(target_key("gao_key")) +
               " in entry " + project_hex(entry) + " (+" +
               project_hex(set_mask) + " -" + project_hex(clear_mask) + ")";
    }
    if (type == "add_object") {
        std::string kind = param_text("kind", "cube");
        std::transform(kind.begin(), kind.end(), kind.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });
        std::string name = param_text("name");
        if (name.empty()) name = "JadePlaced_" + kind;
        std::string body = kind;
        const json::Value* source = vals::P(operation, "source_gao_key");
        const bool has_source = source &&
            !((source->is_num() && source->num == 0.0) ||
              (source->is_str() && source->str.empty()));
        if (kind == "clone" && has_source)
            body = "clone " + project_hex(project_key(source));
        return "add " + body + " " + conflict_python_repr(name) +
               " -> entry " + project_hex(entry);
    }
    if (type == "retarget_material_texture") {
        const int64_t layer = std::max<int64_t>(0, param_int("layer"));
        return "material in " + project_hex(entry) + " sub " +
               project_hex(target_key("sub_key")) +
               (layer ? " layer " + std::to_string(layer) : "") +
               " -> tex " + project_hex(param_key("new_texture_key"));
    }
    if (type == "set_multi_material_slot")
        return "multi-mat " + project_hex(target_key("multi_key")) +
               " slot " + std::to_string(param_int("slot")) + " -> mat " +
               project_hex(param_key("new_material_key")) + " in " +
               project_hex(entry);
    if (type == "set_element_material") {
        const json::Value* raw_container =
            vals::T(operation, "container_entry_key");
        const bool has_container = raw_container &&
            ((raw_container->is_str() && !raw_container->str.empty()) ||
             (raw_container->is_num() && raw_container->num != 0.0));
        const uint32_t container =
            has_container ? project_key(raw_container) : entry;
        const std::string cross_bin = container == entry
            ? std::string()
            : " (container in " + project_hex(container) + ")";
        return "GEO " + project_hex(target_key("geo_key")) + " element " +
               std::to_string(param_int("element")) + " -> mat " +
               project_hex(param_key("new_material_key")) + " in " +
               project_hex(entry) + cross_bin;
    }
    if (type == "set_material_flags") {
        const uint32_t flags = param_key("ul_flags");
        const int64_t layer = std::max<int64_t>(0, param_int("layer"));
        const MatRenderFlags decoded = decode_render_flags(flags);
        return "material " + project_hex(target_key("sub_key")) +
               (layer ? " L" + std::to_string(layer) : "") + " in " +
               project_hex(entry) + " -> " + decoded.mode +
               " (ul_Flags " + project_hex(flags) + ")";
    }
    if (type == "modify_gao_material")
        return "material GAO " + project_hex(target_key("gao_key")) +
               " in entry " + project_hex(entry) + " -> " +
               project_hex(param_key("material_key"));
    if (type == "add_texture")
        return "new texture " + project_hex(param_key("new_key")) +
               " in " + project_hex(entry);
    if (type == "add_material")
        return "new material " + project_hex(param_key("new_key")) +
               " -> tex " + project_hex(param_key("texture_key")) +
               " in " + project_hex(entry);
    if (type == "replace_entry_raw")
        return "entry " + project_hex(entry) + " (raw)";
    if (type == "create_zone")
        return "new zone " + conflict_python_repr(param_text("new_name")) +
               " from template " +
               conflict_python_repr(param_text("template_zone"));
    if (type == "add_zone_dependency") {
        const json::Value* deps = vals::P(operation, "dep_keys");
        const size_t count = deps && deps->is_arr() ? deps->arr.size() : 0;
        return "+" + std::to_string(count) + " dep(s) -> wol " +
               project_hex(target_key("wol_entry_key"));
    }
    return "(unknown: " + type + ")";
}

json::Value operation_to_dict(const json::Value& operation) {
    const std::string type = vals::str_or(operation.find("op"), "");
    if (!known_project_operation(type)) {
        json::Value result = operation.is_obj() ? operation : json::make_obj();
        const json::Value* id = operation.find("id");
        const json::Value* enabled = operation.find("enabled");
        const json::Value* label = operation.find("label");
        const json::Value* created = operation.find("created");
        result.obj["id"] = id ? *id : json::make_str("");
        result.obj["enabled"] = json::make_bool(
            enabled == nullptr ? true : project_truthy(enabled));
        result.obj["label"] = label ? *label : json::make_str("");
        result.obj["created"] = created && project_truthy(created)
                                    ? *created
                                    : json::make_str(project_iso_now());
        return result;
    }

    json::Value result = project_common_operation(operation, type);
    auto target_key = [&](const char* field) {
        return project_key(vals::T(operation, field));
    };
    auto param_key = [&](const char* field) {
        return project_key(vals::P(operation, field));
    };
    auto param_integer = [&](const char* field) {
        const json::Value* value = vals::P(operation, field);
        return value ? *value : json::make_integer("0");
    };
    auto param_text = [&](const char* field, const char* fallback = "") {
        const json::Value* value = vals::P(operation, field);
        return value ? project_string(value) : std::string(fallback);
    };
    auto param_value = [&](const char* field, json::Value fallback) {
        const json::Value* value = vals::P(operation, field);
        return value ? *value : std::move(fallback);
    };
    auto finish = [&](json::Value target, json::Value params,
                      bool include_params = true) {
        result.obj["target"] = std::move(target);
        if (include_params) result.obj["params"] = std::move(params);
        return result;
    };

    const uint32_t entry = target_key("entry_key");
    if (type == "replace_texture") {
        json::Value params = json::make_obj();
        params.obj["source"] = param_value("source", json::make_str(""));
        params.obj["encode"] = param_value("encode", json::make_str("auto"));
        const json::Value* mips = vals::P(operation, "mips");
        params.obj["mips"] = json::make_bool(
            mips == nullptr ? true : project_truthy(mips));
        return finish(project_target({{"entry_key", entry},
                                      {"sub_key", target_key("sub_key")}}),
                      std::move(params));
    }
    if (type == "replace_animation") {
        json::Value params = json::make_obj();
        params.obj["source"] = param_value("source", json::make_str(""));
        return finish(project_target({{"entry_key", entry},
                                      {"sub_key", target_key("sub_key")}}),
                      std::move(params));
    }
    if (type == "stub_mesh")
        return finish(project_target({{"entry_key", entry},
                                      {"sub_key", target_key("sub_key")}}),
                      json::make_obj(), false);
    if (type == "replace_entry_raw") {
        json::Value params = json::make_obj();
        params.obj["source"] = param_value("source", json::make_str(""));
        return finish(project_target({{"entry_key", entry}}),
                      std::move(params));
    }
    if (type == "modify_gao_flags") {
        json::Value params = json::make_obj();
        params.obj["set_mask"] =
            json::make_str(project_hex(param_key("set_mask")));
        params.obj["clear_mask"] =
            json::make_str(project_hex(param_key("clear_mask")));
        return finish(project_target({{"entry_key", entry},
                                      {"gao_key", target_key("gao_key")}}),
                      std::move(params));
    }
    if (type == "modify_gao_material") {
        json::Value params = json::make_obj();
        params.obj["material_key"] =
            json::make_str(project_hex(param_key("material_key")));
        return finish(project_target({{"entry_key", entry},
                                      {"gao_key", target_key("gao_key")}}),
                      std::move(params));
    }
    if (type == "add_texture") {
        json::Value params = json::make_obj();
        params.obj["new_key"] =
            json::make_str(project_hex(param_key("new_key")));
        params.obj["source"] = param_value("source", json::make_str(""));
        params.obj["encode"] =
            json::make_str(param_text("encode", "7"));
        return finish(project_target({{"entry_key", entry}}),
                      std::move(params));
    }
    if (type == "add_material") {
        json::Value params = json::make_obj();
        for (const char* field : {"new_key", "texture_key", "donor_key"})
            params.obj[field] =
                json::make_str(project_hex(param_key(field)));
        return finish(project_target({{"entry_key", entry}}),
                      std::move(params));
    }
    if (type == "retarget_material_texture") {
        json::Value params = json::make_obj();
        params.obj["new_texture_key"] =
            json::make_str(project_hex(param_key("new_texture_key")));
        const json::Value layer = param_integer("layer");
        if (project_truthy(&layer)) params.obj["layer"] = layer;
        return finish(project_target({{"entry_key", entry},
                                      {"sub_key", target_key("sub_key")}}),
                      std::move(params));
    }
    if (type == "set_multi_material_slot") {
        json::Value params = json::make_obj();
        params.obj["slot"] = param_integer("slot");
        params.obj["new_material_key"] =
            json::make_str(project_hex(param_key("new_material_key")));
        return finish(project_target({{"entry_key", entry},
                                      {"multi_key", target_key("multi_key")}}),
                      std::move(params));
    }
    if (type == "set_material_flags") {
        json::Value params = json::make_obj();
        params.obj["ul_flags"] =
            json::make_str(project_hex(param_key("ul_flags")));
        const json::Value layer = param_integer("layer");
        if (project_truthy(&layer)) params.obj["layer"] = layer;
        return finish(project_target({{"entry_key", entry},
                                      {"sub_key", target_key("sub_key")}}),
                      std::move(params));
    }
    if (type == "add_zone_dependency") {
        json::Value params = json::make_obj();
        json::Value deps = json::make_arr();
        const json::Value* raw_deps = vals::P(operation, "dep_keys");
        if (raw_deps && raw_deps->is_arr())
            for (const json::Value& dep : raw_deps->arr)
                deps.arr.push_back(json::make_str(
                    project_hex(project_key(&dep))));
        params.obj["dep_keys"] = std::move(deps);
        return finish(project_target(
                          {{"wol_entry_key", target_key("wol_entry_key")}}),
                      std::move(params));
    }
    if (type == "replace_mesh") {
        json::Value params = json::make_obj();
        params.obj["source"] = param_value("source", json::make_str(""));
        const json::Value* strict = vals::P(operation, "strict_skin");
        params.obj["strict_skin"] = json::make_bool(
            strict == nullptr ? true : project_truthy(strict));

        auto normalize_int_map = [&](const char* field, bool string_values) {
            json::Value normalized = json::make_obj();
            const json::Value* raw = vals::P(operation, field);
            if (raw && raw->is_obj()) {
                for (const auto& [key, value] : raw->obj) {
                    normalized.obj[key] = string_values
                        ? json::make_str(project_string(&value))
                        : value;
                }
            }
            return normalized;
        };
        json::Value bone_map = normalize_int_map("bone_map", false);
        if (!bone_map.obj.empty())
            params.obj["bone_map"] = std::move(bone_map);
        json::Value bone_sources = normalize_int_map("bone_map_source", true);
        if (!bone_sources.obj.empty())
            params.obj["bone_map_source"] = std::move(bone_sources);

        const json::Value* raw_drops = vals::P(operation, "bone_drops");
        if (raw_drops && raw_drops->is_arr() && !raw_drops->arr.empty()) {
            std::vector<json::Value> drops;
            for (const json::Value& value : raw_drops->arr)
                drops.push_back(value);
            std::sort(drops.begin(), drops.end(),
                      [](const json::Value& left, const json::Value& right) {
                          return compare_exact_integers(left, right) < 0;
                      });
            drops.erase(std::unique(
                            drops.begin(), drops.end(),
                            [](const json::Value& left,
                               const json::Value& right) {
                                return compare_exact_integers(left, right) == 0;
                            }),
                        drops.end());
            json::Value normalized = json::make_arr();
            for (const json::Value& value : drops)
                normalized.arr.push_back(value);
            params.obj["bone_drops"] = std::move(normalized);
        }
        const json::Value* rigid = vals::P(operation, "rigid_bind_bone");
        if (rigid && rigid->type != json::Value::Type::Null &&
            project_truthy(rigid))
            params.obj["rigid_bind_bone"] = *rigid;
        json::Value drop_targets = normalize_int_map("drop_targets", false);
        if (!drop_targets.obj.empty())
            params.obj["drop_targets"] = std::move(drop_targets);

        for (const char* field : {"auto_rig", "import_vertex_colors",
                                  "keep_original_skin"}) {
            if (project_truthy(vals::P(operation, field)))
                params.obj[field] = json::make_bool(true);
        }
        const json::Value* diagnose = vals::P(operation, "diagnose_rest_pose");
        const bool diagnose_value = diagnose
            ? project_truthy(diagnose)
            : project_truthy(vals::P(operation, "rebind_ibm"));
        if (diagnose_value)
            params.obj["diagnose_rest_pose"] = json::make_bool(true);
        return finish(project_target({{"entry_key", entry},
                                      {"sub_key", target_key("sub_key")}}),
                      std::move(params));
    }
    if (type == "edit_light") {
        json::Value params = json::make_obj();
        const json::Value* light_type = vals::P(operation, "light_type");
        if (light_type && light_type->type != json::Value::Type::Null)
            params.obj["light_type"] = json::make_integer(std::to_string(
                exact_integer_modulo(*light_type, 8)));
        for (const char* field : {"diffuse", "specular"}) {
            const json::Value* raw = vals::P(operation, field);
            if (!raw || raw->type == json::Value::Type::Null) continue;
            json::Value color = json::make_arr();
            if (raw->is_arr())
                for (const json::Value& channel : raw->arr)
                    color.arr.push_back(json::make_integer(std::to_string(
                        exact_integer_modulo(channel, 256))));
            params.obj[field] = std::move(color);
        }
        for (const char* field : {"near", "far", "inner_angle",
                                  "outer_angle", "intensity"}) {
            const json::Value* raw = vals::P(operation, field);
            if (!raw || raw->type == json::Value::Type::Null) continue;
            const double value = raw->is_num()
                ? raw->num
                : std::strtod(project_string(raw).c_str(), nullptr);
            params.obj[field] = json::make_num(value);
        }
        return finish(project_target({{"entry_key", entry},
                                      {"light_key", target_key("light_key")},
                                      {"gao_key", target_key("gao_key")}}),
                      std::move(params));
    }
    if (type == "modify_transform") {
        json::Value params = json::make_obj();
        params.obj["position"] = project_number_array(
            vals::P(operation, "position"), {0.0, 0.0, 0.0});
        params.obj["rotation_euler_deg"] = project_number_array(
            vals::P(operation, "rotation_euler_deg"), {0.0, 0.0, 0.0});
        params.obj["scale"] = project_number_array(
            vals::P(operation, "scale"), {1.0, 1.0, 1.0});
        const json::Value* world = vals::P(operation, "world_matrix");
        if (world && world->type != json::Value::Type::Null)
            params.obj["world_matrix"] = project_number_array(world, {});
        const json::Value* follow = vals::P(operation, "collision_follow");
        if (project_truthy(follow)) params.obj["collision_follow"] = *follow;
        return finish(project_target({{"entry_key", entry},
                                      {"gao_key", target_key("gao_key")}}),
                      std::move(params));
    }
    if (type == "add_object_collision") {
        json::Value params = json::make_obj();
        const json::Value* raw_profile = vals::P(operation,
                                                 "collision_profile");
        params.obj["collision_profile"] =
            raw_profile ? *raw_profile : json::make_str("simple_box");
        std::string shape = param_text("collision_shape", "mesh");
        if (shape.empty()) shape = "mesh";
        std::transform(shape.begin(), shape.end(), shape.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });
        params.obj["collision_shape"] = json::make_str(shape);
        const json::Value* room = vals::P(operation, "room_cob_key");
        if (room && room->type != json::Value::Type::Null)
            params.obj["room_cob_key"] =
                json::make_str(project_hex(project_key(room)));
        return finish(project_target({{"entry_key", entry},
                                      {"gao_key", target_key("gao_key")}}),
                      std::move(params));
    }
    if (type == "set_element_material") {
        json::Value target = project_target(
            {{"entry_key", entry}, {"geo_key", target_key("geo_key")},
             {"multi_key", target_key("multi_key")}});
        const json::Value* raw_container =
            vals::T(operation, "container_entry_key");
        const uint32_t container = project_truthy(raw_container)
            ? project_key(raw_container)
            : entry;
        if (container != entry)
            target.obj["container_entry_key"] =
                json::make_str(project_hex(container));
        json::Value params = json::make_obj();
        params.obj["element"] = param_integer("element");
        params.obj["new_material_key"] =
            json::make_str(project_hex(param_key("new_material_key")));
        return finish(std::move(target), std::move(params));
    }
    if (type == "create_zone") {
        json::Value params = json::make_obj();
        params.obj["template_zone"] =
            json::make_str(param_text("template_zone"));
        params.obj["new_name"] = json::make_str(param_text("new_name"));
        uint32_t prefix = param_key("new_map_prefix");
        if (prefix == 0) prefix = 0x27;
        prefix &= 0xff;
        if (prefix != 0x27)
            params.obj["new_map_prefix"] =
                json::make_str(project_hex(prefix));
        const json::Value* target_dir = vals::P(operation, "target_dir_idx");
        if (target_dir && target_dir->type != json::Value::Type::Null)
            params.obj["target_dir_idx"] = *target_dir;
        const json::Value* raw_deps = vals::P(operation, "extra_deps");
        if (project_truthy(raw_deps) && raw_deps->is_arr()) {
            json::Value deps = json::make_arr();
            for (const json::Value& dep : raw_deps->arr)
                deps.arr.push_back(json::make_str(
                    project_hex(project_key(&dep))));
            params.obj["extra_deps"] = std::move(deps);
        }
        result.obj["params"] = std::move(params);
        return result;
    }
    if (type == "add_object") {
        json::Value params = json::make_obj();
        std::string kind = param_text("kind", "cube");
        std::transform(kind.begin(), kind.end(), kind.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });
        const json::Value* raw_name = vals::P(operation, "name");
        json::Value name = raw_name ? *raw_name
                                   : json::make_str("JadePlaced_" + kind);
        params.obj["kind"] = json::make_str(kind);
        params.obj["name"] = std::move(name);
        params.obj["position"] = project_number_array(
            vals::P(operation, "position"), {0.0, 0.0, 0.0});

        json::Value rotation = project_number_array(
            vals::P(operation, "rotation_euler_deg"), {0.0, 0.0, 0.0});
        if (project_array_any_nonzero(rotation))
            params.obj["rotation_euler_deg"] = std::move(rotation);
        json::Value scale = project_number_array(
            vals::P(operation, "scale_xform"), {1.0, 1.0, 1.0});
        if (project_array_any_not_one(scale))
            params.obj["scale_xform"] = std::move(scale);
        const json::Value* world = vals::P(operation, "world_matrix");
        if (world && world->type != json::Value::Type::Null)
            params.obj["world_matrix"] = project_number_array(world, {});

        auto has_optional_key = [](const json::Value* value) {
            if (!value || value->type == json::Value::Type::Null) return false;
            if (value->is_num() &&
                (value->number_is_integer && !value->integer_text.empty()
                     ? exact_integer_zero(*value)
                     : value->num == 0.0))
                return false;
            if (value->is_str() && value->str.empty()) return false;
            return true;
        };
        std::string profile = param_text("collision_profile", "simple_box");
        if (profile.empty()) profile = "simple_box";
        std::transform(profile.begin(), profile.end(), profile.begin(),
                       [](unsigned char ch) { return char(std::tolower(ch)); });

        if (kind == "clone") {
            const json::Value* source_gao =
                vals::P(operation, "source_gao_key");
            const json::Value* source_entry =
                vals::P(operation, "source_entry_key");
            if (has_optional_key(source_gao))
                params.obj["source_gao_key"] =
                    json::make_str(project_hex(project_key(source_gao)));
            if (has_optional_key(source_entry))
                params.obj["source_entry_key"] =
                    json::make_str(project_hex(project_key(source_entry)));
            if (project_truthy(vals::P(operation, "clone_with_collision"))) {
                params.obj["clone_with_collision"] = json::make_bool(true);
                if (profile != "simple_box")
                    params.obj["collision_profile"] =
                        json::make_str(profile);
                const json::Value* room =
                    vals::P(operation, "room_cob_key");
                if (has_optional_key(room))
                    params.obj["room_cob_key"] =
                        json::make_str(project_hex(project_key(room)));
            }
        } else {
            params.obj["size"] = project_number_array(
                vals::P(operation, "size"), {1.0, 1.0, 1.0});
            const json::Value* raw_collision = vals::P(operation, "collision");
            const bool collision = raw_collision == nullptr
                ? true
                : project_truthy(raw_collision);
            params.obj["collision"] = json::make_bool(collision);
            if (collision && profile != "simple_box")
                params.obj["collision_profile"] = json::make_str(profile);
            if (kind == "model") {
                const json::Value* source = vals::P(operation, "source");
                const json::Value* model_path = vals::P(operation,
                                                        "model_path");
                if (project_truthy(source))
                    params.obj["source"] = *source;
                else if (project_truthy(model_path))
                    params.obj["model_path"] = *model_path;
                if (project_truthy(
                        vals::P(operation, "import_vertex_colors")))
                    params.obj["import_vertex_colors"] = json::make_bool(true);
            } else {
                const json::Value* raw_color =
                    vals::P(operation, "vertex_color");
                if (raw_color && raw_color->type != json::Value::Type::Null &&
                    raw_color->is_arr()) {
                    json::Value color = json::make_arr();
                    for (const json::Value& channel : raw_color->arr)
                        color.arr.push_back(json::make_integer(std::to_string(
                            exact_integer_modulo(channel, 256))));
                    if (color.arr.size() == 3)
                        color.arr.push_back(json::make_num(255));
                    params.obj["vertex_color"] = std::move(color);
                }
            }
        }
        const json::Value* material = vals::P(operation, "material_key");
        if (has_optional_key(material))
            params.obj["material_key"] =
                json::make_str(project_hex(project_key(material)));
        return finish(project_target({{"entry_key", entry}}),
                      std::move(params));
    }

    return result;
}

json::Value project_to_dict(const ModProject& project) {
    if (project.loaded_dict.is_obj()) return project.loaded_dict;
    json::Value result = json::make_obj();
    result.obj["format"] = json::make_str("jade-mod-project");
    result.obj["format_version"] = json::make_num(1);
    result.obj["name"] = json::make_str(project.name);
    result.obj["author"] = json::make_str(project.author);
    result.obj["description"] = json::make_str(project.description);
    result.obj["created"] = json::make_str(project.created);
    result.obj["modified"] = json::make_str(project.modified);
    json::Value base = json::make_obj();
    base.obj["game"] = json::make_str(project.base.game);
    base.obj["archive_name"] = json::make_str(project.base.archive_name);
    base.obj["archive_sha256"] = json::make_str(project.base.archive_sha256);
    base.obj["archive_size"] = json::make_num(double(project.base.archive_size));
    result.obj["base"] = std::move(base);
    json::Value build = json::make_obj();
    build.obj["output_name"] = json::make_str(project.build.output_name);
    build.obj["strict_inplace"] = json::make_bool(project.build.strict_inplace);
    result.obj["build"] = std::move(build);
    result.obj["next_op_serial"] = json::make_num(project.next_op_serial);
    json::Value operations = json::make_arr();
    operations.arr.reserve(project.operations.size());
    for (const json::Value& operation : project.operations)
        operations.arr.push_back(operation_to_dict(operation));
    result.obj["operations"] = std::move(operations);
    return result;
}

std::vector<BuildIssue> validate_project(const ModProject& project,
                                         const std::string& base_archive_path,
                                         std::vector<std::string>& log,
                                         bool live) {
    (void)log;  // Python validate_project has no log callback/output.
    std::vector<BuildIssue> issues;
    BigFile bf;
    try { bf.open(base_archive_path); } catch (const std::exception& e) {
        issues.push_back({"error",
                          std::string("cannot open base archive: ") + e.what(),
                          ""});
        return issues;
    }
    EntrySet es(bf);
    std::vector<const json::Value*> enabled = project.enabled_operations();

    // Per-operation validation (op.validate(ctx, live=False)).
    for (const json::Value* op : enabled) {
        try {
            vals::validate_one(*op, es, project, /*has_assets=*/true,
                               project.jmod_dir, issues, live);
        } catch (const std::exception& e) {
            issues.push_back({"error",
                              std::string("validate() raised: ") + e.what(),
                              op_id_of(*op)});
        }
    }

    // Asset-store existence pass (op.asset_refs()).
    for (const json::Value* op : enabled)
        for (const std::string& ref : vals::asset_refs_of(*op))
            if (resolve_asset(project.jmod_dir, ref).empty())
                issues.push_back({"error", "missing asset " + ref,
                                  op_id_of(*op)});

    // Conflict signatures (warnings: later op wins), shared with the public
    // ModProject.conflicts() equivalent used by the Qt document model.
    for (const ProjectConflict& conflict : project_conflicts(project)) {
        const std::vector<std::string>& ids = conflict.op_ids;
        std::string joined;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (i) joined += ", ";
            joined += ids[i];
        }
        issues.push_back(
            {"warning",
             "conflict on " + format_conflict_signature(conflict.signature) +
                 ": ops [" + joined + "] (later op wins)",
             ""});
    }

    return issues;
}

namespace {
// One op through the typed dispatch; shared by build_project and the jtmod
// exporter (which applies against a read-only base view).
void apply_ops(const std::vector<const json::Value*>& ops, EntrySet& entries,
               const std::string& jmod_dir, BuildState& st,
               std::vector<std::string>& log_out,
               const BuildProgressFn& progress = {}) {
    size_t done = 0;
    for (const json::Value* op : ops) {
        std::string op_type;
        const json::Value* t = op->find("op");
        if (t && t->is_str()) op_type = t->str;
        try {
            if (op_type == "modify_transform") apply_modify_transform(*op, entries, st);
            else if (op_type == "stub_mesh") apply_stub_mesh(*op, entries, st, log_out);
            else if (op_type == "edit_light") apply_edit_light(*op, entries);
            else if (op_type == "remove_object")
                apply_remove_object(*op, entries, log_out);
            else if (op_type == "rescale_rli")
                apply_rescale_rli(*op, entries, log_out);
            else if (op_type == "modify_gao_flags")
                apply_modify_gao_flags(*op, entries, log_out);
            else if (op_type == "add_object")
                apply_add_object(*op, entries, jmod_dir, log_out);
            else if (op_type == "retarget_material_texture")
                apply_retarget_material_texture(*op, entries, log_out);
            else if (op_type == "set_multi_material_slot")
                apply_set_multi_material_slot(*op, entries, log_out);
            else if (op_type == "set_material_flags")
                apply_set_material_flags(*op, entries, log_out);
            else if (op_type == "modify_gao_material")
                apply_modify_gao_material(*op, entries, log_out);
            else if (op_type == "replace_texture")
                apply_replace_texture(*op, entries, jmod_dir, log_out);
            else if (op_type == "add_texture")
                apply_add_texture(*op, entries, jmod_dir, log_out);
            else if (op_type == "add_material")
                apply_add_material(*op, entries, log_out);
            else if (op_type == "replace_entry_raw")
                apply_replace_entry_raw(*op, entries, jmod_dir, log_out);
            else if (op_type == "replace_animation")
                apply_replace_animation(*op, entries, jmod_dir, log_out);
            else if (op_type == "add_zone_dependency")
                apply_add_zone_dependency(*op, entries, log_out);
            else if (op_type == "set_element_material")
                apply_set_element_material(*op, entries, log_out);
            else if (op_type == "create_zone")
                apply_create_zone(*op, entries, log_out);
            else if (op_type == "add_object_collision")
                apply_add_object_collision(*op, entries, log_out);
            else if (op_type == "replace_mesh")
                apply_replace_mesh(*op, entries, jmod_dir, log_out);
            else {
                log_out.push_back("[unknown op] skipped: id=" + op_id_of(*op) +
                                  " type=" + python_repr_string(op_type));
                // UnknownOperation.apply only logs. Its warning was already
                // produced during pre-build validation, so adding another
                // issue here would duplicate it in BuildResult.issues.
            }
        } catch (const std::exception& e) {
            st.error(op, std::string("apply failed: ") + e.what());
        }
        ++done;
        if (progress)
            progress(done, ops.size(), "Applying operations");
    }

}
}  // namespace

BuildResult build_project(const ModProject& project,
                          const std::string& base_archive_path,
                          const std::string& output_path_in, bool dry_run,
                          BuildProgressFn progress,
                          const BuildFaultInjection* fault) {
    BuildResult res;
    BuildState st{res.issues, res.log};
    auto log = [&](const std::string& m) { res.log.push_back(m); };
    auto finish = [&]() {
        res.report = format_build_report(project, res);
        return res;
    };

    std::error_code base_ec;
    if (!std::filesystem::is_regular_file(
            std::filesystem::u8path(base_archive_path), base_ec)) {
        st.error(nullptr, "base archive not found: " + base_archive_path);
        return finish();
    }
    if (!project.base.matches(base_archive_path)) {
        st.error(nullptr,
                 "base archive does not match project (expected " +
                     std::to_string(project.base.archive_size) +
                     "B / sha256 " +
                     project.base.archive_sha256.substr(0, 12) +
                     "\xE2\x80\xA6)");
        return finish();
    }
    log("base archive verified: " + base_archive_path);

    std::string output_path = output_path_in;
    if (output_path.empty()) {
        std::string name = project.build.output_name;
        if (name.empty()) {
            std::filesystem::path archive_name =
                std::filesystem::u8path(project.base.archive_name);
            std::string stem = archive_name.stem().u8string();
            std::string ext = archive_name.extension().u8string();
            name = stem + ".modded" + (ext.empty() ? ".bf" : ext);
        }
        size_t slash = base_archive_path.find_last_of("/\\");
        std::string dir = slash == std::string::npos
                              ? "."
                              : base_archive_path.substr(0, slash);
        output_path = dir + "/" + name;
    }

    // Pre-build validation against the BASE view (builder.py step 4):
    // errors abort before anything is copied or written.
    {
        if (progress) progress(0, 1, "Validating");
        std::vector<BuildIssue> v_issues =
            validate_project(project, base_archive_path, res.log);
        res.issues.insert(res.issues.end(), v_issues.begin(), v_issues.end());
        size_t errors = 0;
        for (const BuildIssue& i : v_issues)
            if (i.level == "error") ++errors;
        if (errors) {
            char b[72];
            std::snprintf(b, sizeof b,
                          "validation produced %zu error(s); aborting build",
                          errors);
            log(b);
            return finish();
        }
    }

    // Copy base -> output (dry runs apply against the BASE, read-only).
    std::string work_path = base_archive_path;
    if (!dry_run) {
        if (progress) progress(0, 1, "Copying base archive");
        const FileCopyResult copied =
            copy_file_bytes(base_archive_path, output_path);
        if (!copied.ok) {
            st.error(nullptr, "could not write output: " + copied.error);
            return finish();
        }
        log("copied base -> " + output_path);
        work_path = output_path;
    }

    BigFile bf;
    try { bf.open(work_path); } catch (const std::exception& e) {
        st.error(nullptr, e.what());
        return finish();
    }
    EntrySet entries(bf);

    const std::vector<const json::Value*> enabled =
        project.enabled_operations();
    if (progress)
        progress(0, std::max<size_t>(1, enabled.size()),
                 "Applying operations");
    apply_ops(enabled, entries, project.jmod_dir, st, res.log, progress);

    // Write phase (skipped on dry runs — sizes still reported).
    auto& modified = entries.modified;
    if (!modified.empty()) {
        char b[64];
        std::snprintf(b, sizeof b, "writing %zu modified entrie(s)", modified.size());
        log(b);
    }
    if (progress)
        progress(0, std::max<size_t>(1, modified.size()),
                 "Writing entries");
    std::fstream f;
    if (!dry_run) {
        f.open(std::filesystem::u8path(output_path),
               std::ios::in | std::ios::out | std::ios::binary);
        if (!f) { st.error(nullptr, "cannot reopen output"); return finish(); }
    }
    // NOTE: Python iterates its modified dict in first-get order; the cache
    // here is key-ordered. Only the WRITE ORDER differs — each entry's bytes
    // and destination are identical, and write_entry slots are independent,
    // so the output archive is byte-identical unless two entries both
    // overflow into the append region (flagged below).
    bool appended_any = false;
    size_t write_index = 0;
    for (auto& kv : entries.cache) {
        uint32_t key = kv.first;
        if (!modified.count(key)) continue;
        ++write_index;
        BFFile* fi = entries.file_for(key);
        if (fi == nullptr) continue;
        std::vector<uint8_t> compressed = compress_lzo(kv.second, 9);
        bool fits = compressed.size() <= fi->length;
        char b[128];
        if (!fits && project.build.strict_inplace) {
            std::snprintf(b, sizeof b,
                          "entry 0x%08x: compressed %zuB does not fit slot %uB "
                          "(strict_inplace=true)", key, compressed.size(),
                          fi->length);
            st.error(nullptr, b);
            continue;
        }
        if (!dry_run) {
            if (fault && fault->fail_write_entry_at == write_index) {
                std::snprintf(b, sizeof b,
                              "entry 0x%08x: write_entry failed: %s", key,
                              fault->message.c_str());
                st.error(nullptr, b);
                continue;
            }
            try {
                bf.write_entry(f, compressed, *fi);
            } catch (const std::exception& e) {
                std::snprintf(b, sizeof b,
                              "entry 0x%08x: write_entry failed: %s", key,
                              e.what());
                st.error(nullptr, b);
                continue;
            }
        }
        if (!fits) {
            if (appended_any)
                st.warning(nullptr,
                           "multiple appended entries: append ORDER is "
                           "key-sorted here vs first-get order in the Python "
                           "builder; archives may differ in layout");
            appended_any = true;
            res.bytes_appended += compressed.size();
            ++res.entries_appended;
        }
        ++res.entries_changed;
        res.modified_keys.push_back(key);
        std::snprintf(b, sizeof b, "  entry 0x%08x: %zuB dec -> %zuB %s", key,
                      kv.second.size(), compressed.size(),
                      fits ? "in-place" : "APPENDED");
        log(b);
        if (progress)
            progress(write_index, modified.size(), "Writing entries");
    }
    // Create staged new FAT entries (CreateZone) after the modified pass.
    std::unordered_set<uint32_t> added_keys;
    for (auto& kv : entries.new_entries) {
        added_keys.insert(kv.first);
        char nb[128];
        if (dry_run) {
            ++res.entries_changed;
            ++res.entries_appended;
            res.bytes_appended += compress_lzo(kv.second.data, 9).size();
            continue;
        }
        std::vector<uint8_t> compressed = compress_lzo(kv.second.data, 9);
        try {
            bf.add_entry(f, kv.second.name, kv.first,
                         kv.second.parent_dir_idx, compressed);
        } catch (const std::exception& e) {
            std::snprintf(nb, sizeof nb, "new entry 0x%08x: add_entry failed: %s",
                          kv.first, e.what());
            st.error(nullptr, nb);
            continue;
        }
        ++res.entries_changed;
        ++res.entries_appended;
        res.bytes_appended += compressed.size();
        std::snprintf(nb, sizeof nb, "  new entry 0x%08x: %zuB dec -> %zuB",
                      kv.first, kv.second.data.size(), compressed.size());
        log(nb);
    }
    if (!dry_run && f) bf.flush_size_grs(f);
    if (!dry_run) {
        f.close();
        if (progress) progress(0, 1, "Verifying output");
        // Post-build integrity verifier (Python builder step 8) — proves the
        // pipeline did not break the .bf.
        // Python passes every scheduled modified key to verification, even if
        // a particular write failed and the copied base bytes remain there.
        log("verifying output integrity\xE2\x80\xA6");
        std::unordered_set<uint32_t> touched(modified.begin(), modified.end());
        auto vi = verify_build_output(base_archive_path, output_path, touched,
                                      added_keys, res.log);
        res.issues.insert(res.issues.end(), vi.begin(), vi.end());

        // Python builder steps 9/10: semantic checks which structural FAT and
        // sub-entry validation cannot catch.
        try {
            auto texture_issues =
                verify_replaced_textures(project, output_path, res.log);
            res.issues.insert(res.issues.end(), texture_issues.begin(),
                              texture_issues.end());
        } catch (const std::exception& e) {
            log(std::string("verify-tex: skipped (") + e.what() + ")");
        }
        try {
            auto asset_issues = verify_added_assets(
                project, base_archive_path, output_path, res.log);
            res.issues.insert(res.issues.end(), asset_issues.begin(),
                              asset_issues.end());
        } catch (const std::exception& e) {
            log(std::string("verify-add: skipped (") + e.what() + ")");
        }
    }

    res.ok = std::none_of(res.issues.begin(), res.issues.end(),
                          [](const BuildIssue& i) { return i.level == "error"; });
    res.output_path = res.ok && !dry_run ? output_path : "";
    return finish();
}

// ── .jtmod export (build/jtmod_export.py + jtmod.py pack/seal) ──────────────
namespace jtx {

// CPython difflib.SequenceMatcher (isjunk=None, autojunk=False) over record
// lists — the hunks must match the Python exporter's byte-for-byte.
struct OpCode { char tag; size_t i1, i2, j1, j2; };   // tag r/d/i/e

std::vector<OpCode> get_opcodes(const std::vector<jtmod::Record>& a,
                                const std::vector<jtmod::Record>& b) {
    // b2j: element -> ascending j positions.
    std::map<jtmod::Record, std::vector<size_t>> b2j;
    for (size_t j = 0; j < b.size(); ++j) b2j[b[j]].push_back(j);

    struct Match { size_t i, j, size; };
    auto find_longest = [&](size_t alo, size_t ahi, size_t blo, size_t bhi) {
        size_t besti = alo, bestj = blo, bestsize = 0;
        std::unordered_map<size_t, size_t> j2len;
        for (size_t i = alo; i < ahi; ++i) {
            std::unordered_map<size_t, size_t> newj2len;
            auto it = b2j.find(a[i]);
            if (it != b2j.end())
                for (size_t j : it->second) {
                    if (j < blo) continue;
                    if (j >= bhi) break;
                    size_t k = (j > 0 && j2len.count(j - 1) ? j2len[j - 1] : 0) + 1;
                    newj2len[j] = k;
                    if (k > bestsize) {
                        besti = i - k + 1;
                        bestj = j - k + 1;
                        bestsize = k;
                    }
                }
            j2len = std::move(newj2len);
        }
        return Match{besti, bestj, bestsize};
    };

    // matching_blocks: LIFO queue, collect, sort, coalesce adjacent.
    std::vector<std::array<size_t, 4>> queue{{0, a.size(), 0, b.size()}};
    std::vector<Match> blocks;
    while (!queue.empty()) {
        auto q = queue.back();
        queue.pop_back();
        Match m = find_longest(q[0], q[1], q[2], q[3]);
        if (m.size) {
            blocks.push_back(m);
            if (q[0] < m.i && q[2] < m.j)
                queue.push_back({q[0], m.i, q[2], m.j});
            if (m.i + m.size < q[1] && m.j + m.size < q[3])
                queue.push_back({m.i + m.size, q[1], m.j + m.size, q[3]});
        }
    }
    std::sort(blocks.begin(), blocks.end(), [](const Match& x, const Match& y) {
        return x.i != y.i ? x.i < y.i : x.j < y.j;
    });
    std::vector<Match> merged;
    size_t i1 = 0, j1 = 0, k1 = 0;
    for (const Match& m : blocks) {
        if (i1 + k1 == m.i && j1 + k1 == m.j) {
            k1 += m.size;
        } else {
            if (k1) merged.push_back({i1, j1, k1});
            i1 = m.i; j1 = m.j; k1 = m.size;
        }
    }
    if (k1) merged.push_back({i1, j1, k1});
    merged.push_back({a.size(), b.size(), 0});

    std::vector<OpCode> ops;
    size_t ci = 0, cj = 0;
    for (const Match& m : merged) {
        char tag = 0;
        if (ci < m.i && cj < m.j) tag = 'r';
        else if (ci < m.i) tag = 'd';
        else if (cj < m.j) tag = 'i';
        if (tag) ops.push_back({tag, ci, m.i, cj, m.j});
        ci = m.i + m.size;
        cj = m.j + m.size;
        if (m.size) ops.push_back({'e', m.i, ci, m.j, cj});
    }
    return ops;
}

void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
}
void put_str(std::vector<uint8_t>& v, const std::string& s) {
    put_u32(v, uint32_t(s.size()));
    v.insert(v.end(), s.begin(), s.end());
}
void put_blob(std::vector<uint8_t>& v, const std::vector<uint8_t>& b) {
    put_u32(v, uint32_t(b.size()));
    v.insert(v.end(), b.begin(), b.end());
}
uint32_t key_of_record(const jtmod::Record& r) {
    if (r.size() < 12) return 0;
    return uint32_t(r[8]) | (uint32_t(r[9]) << 8) | (uint32_t(r[10]) << 16) |
           (uint32_t(r[11]) << 24);
}
bool is_modded_key(uint32_t k) { return (k & 0x7F000000u) == 0x7A000000u; }

std::vector<uint8_t> sha256_bytes(const std::vector<uint8_t>& data) {
    Sha256 s;
    s.update(data.data(), data.size());
    std::string hex = s.hex();
    std::vector<uint8_t> out(32);
    for (int i = 0; i < 32; ++i)
        out[size_t(i)] = uint8_t(std::strtoul(hex.substr(size_t(i) * 2, 2).c_str(),
                                              nullptr, 16));
    return out;
}

namespace {

std::string python_string_repr(const std::string& value) {
    const bool use_double = value.find('\'') != std::string::npos &&
                            value.find('"') == std::string::npos;
    const char quote = use_double ? '"' : '\'';
    std::string out(1, quote);
    static const char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        if (ch == '\\' || ch == static_cast<unsigned char>(quote)) {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch < 0x20 || ch == 0x7f) {
            out += "\\x";
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0f]);
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    out.push_back(quote);
    return out;
}

std::string comma_uint(uint64_t value) {
    std::string out = std::to_string(value);
    for (std::ptrdiff_t pos = static_cast<std::ptrdiff_t>(out.size()) - 3;
         pos > 0; pos -= 3)
        out.insert(static_cast<size_t>(pos), 1, ',');
    return out;
}

std::string issue_string(const BuildIssue& issue) {
    std::string out = "[" + issue.level + "]";
    if (!issue.op_id.empty()) out += " " + issue.op_id;
    return out + " " + issue.message;
}

}  // namespace

std::string format_build_report_impl(const ModProject& project,
                                     const BuildResult& result) {
    size_t errors = 0, warnings = 0;
    for (const BuildIssue& issue : result.issues) {
        if (issue.level == "error") ++errors;
        if (issue.level == "warning") ++warnings;
    }
    const int64_t in_place = int64_t(result.entries_changed) -
                             int64_t(result.entries_appended);
    std::vector<std::string> lines = {
        "Build report \xE2\x80\x94 " + python_string_repr(project.name),
        "  operations enabled : " +
            std::to_string(project.enabled_operations().size()),
        std::string("  strict_inplace     : ") +
            (project.build.strict_inplace ? "True" : "False"),
        "  entries changed    : " + std::to_string(result.entries_changed) +
            " (in-place: " + std::to_string(in_place) + ", appended: " +
            std::to_string(result.entries_appended) + ")",
        "  bytes appended     : " + comma_uint(result.bytes_appended),
        "  errors / warnings  : " + std::to_string(errors) + " / " +
            std::to_string(warnings),
        "",
    };
    if (!result.issues.empty()) {
        lines.push_back("Issues:");
        for (const BuildIssue& issue : result.issues)
            lines.push_back("  " + issue_string(issue));
        lines.push_back("");
    }
    if (!result.log.empty()) {
        lines.push_back("Log:");
        for (const std::string& line : result.log)
            lines.push_back("  " + line);
    }
    std::string report;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) report.push_back('\n');
        report += lines[i];
    }
    return report;
}

std::string utc_iso_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char text[32]{};
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return text;
}

}  // namespace jtx

std::string format_build_report(const ModProject& project,
                                const BuildResult& result) {
    return jtx::format_build_report_impl(project, result);
}

JtmodExportResult export_jtmod(const ModProject& project,
                               const std::string& base_archive_path,
                               const std::string& out_path,
                               const JtmodExportOptions& options) {
    JtmodExportResult res;
    BuildState st{res.issues, res.log};
    auto fail = [&](const std::string& m) {
        st.error(nullptr, m);
        return res;
    };
    if (!project.base.matches(base_archive_path))
        return fail("base archive does not match project");

    BigFile base_view;
    try { base_view.open(base_archive_path); } catch (const std::exception& e) {
        return fail(e.what());
    }
    if (options.validate) {
        std::vector<BuildIssue> validation =
            validate_project(project, base_archive_path, res.log);
        bool has_error = false;
        for (const BuildIssue& issue : validation)
            if (issue.level == "error") has_error = true;
        res.issues.insert(res.issues.end(), validation.begin(),
                          validation.end());
        if (has_error) {
            res.report = "harvest failed";
            return res;
        }
    }
    EntrySet entries(base_view);
    apply_ops(project.enabled_operations(), entries, project.jmod_dir, st,
              res.log);
    for (const BuildIssue& i : res.issues)
        if (i.level == "error") return res;

    // Diff each modified bin (build/jtmod_export._diff_bin).
    std::vector<uint8_t> out;
    std::vector<uint8_t> bins_blob;
    uint32_t n_bins = 0;
    std::vector<uint32_t> minted;
    auto mint = [&](uint32_t k) {
        if (std::find(minted.begin(), minted.end(), k) == minted.end())
            minted.push_back(k);
    };
    for (auto& kv : entries.cache) {
        uint32_t key = kv.first;
        if (!entries.modified.count(key)) continue;
        BFFile* fi = entries.file_for(key);
        if (fi == nullptr) return fail("modified entry not found in base");
        LzoResult br = decompress_lzo(base_view.read_data(fi->index));
        if (!br.ok) return fail("base entry did not decompress");
        const std::vector<uint8_t>& base_dec = br.data;
        const std::vector<uint8_t>& mod_dec = kv.second;

        jtmod::SplitResult bs = jtmod::split_records(base_dec);
        jtmod::SplitResult ms = jtmod::split_records(mod_dec);
        ++n_bins;
        jtx::put_u32(bins_blob, key);
        if (!(bs.clean && ms.clean && bs.prefix == ms.prefix)) {
            for (const jtmod::Record& r : ms.records)
                if (jtx::is_modded_key(jtx::key_of_record(r)))
                    mint(jtx::key_of_record(r));
            bins_blob.push_back(jtmod::MODE_WHOLEBIN);
            jtx::put_blob(bins_blob, mod_dec);
            ++res.bins_wholebin;
            continue;
        }
        std::vector<jtmod::Hunk> hunks;
        for (const jtx::OpCode& oc :
             jtx::get_opcodes(bs.records, ms.records)) {
            if (oc.tag == 'e') continue;
            jtmod::Hunk h;
            h.base_start = uint32_t(oc.i1);
            h.base_end = uint32_t(oc.i2);
            h.records.assign(ms.records.begin() + long(oc.j1),
                             ms.records.begin() + long(oc.j2));
            hunks.push_back(std::move(h));
        }
        // Defensive rebuild check (never ship a delta that can't reproduce).
        std::vector<jtmod::Record> rebuilt_recs =
            jtmod::apply_hunks(bs.records, hunks);
        std::vector<uint8_t> rebuilt = bs.prefix;
        for (const jtmod::Record& r : rebuilt_recs)
            rebuilt.insert(rebuilt.end(), r.begin(), r.end());
        if (rebuilt != mod_dec) {
            for (const jtmod::Record& r : ms.records)
                if (jtx::is_modded_key(jtx::key_of_record(r)))
                    mint(jtx::key_of_record(r));
            bins_blob.push_back(jtmod::MODE_WHOLEBIN);
            jtx::put_blob(bins_blob, mod_dec);
            ++res.bins_wholebin;
            continue;
        }
        std::unordered_set<uint32_t> base_keys;
        for (const jtmod::Record& r : bs.records)
            base_keys.insert(jtx::key_of_record(r));
        for (const jtmod::Hunk& h : hunks)
            for (const jtmod::Record& r : h.records) {
                uint32_t k = jtx::key_of_record(r);
                if (jtx::is_modded_key(k) && !base_keys.count(k)) mint(k);
            }
        bins_blob.push_back(jtmod::MODE_SUBENTRY);
        jtx::put_u32(bins_blob, uint32_t(bs.records.size()));
        std::vector<uint8_t> bh = jtx::sha256_bytes(base_dec);
        bins_blob.insert(bins_blob.end(), bh.begin(), bh.end());
        jtx::put_u32(bins_blob, uint32_t(hunks.size()));
        for (const jtmod::Hunk& h : hunks) {
            jtx::put_u32(bins_blob, h.base_start);
            jtx::put_u32(bins_blob, h.base_end);
            jtx::put_u32(bins_blob, uint32_t(h.records.size()));
            for (const jtmod::Record& r : h.records) jtx::put_blob(bins_blob, r);
        }
        ++res.bins_subentry;
    }

    for (auto& kv : entries.new_entries) {
        ++n_bins;
        jtx::put_u32(bins_blob, kv.first);
        bins_blob.push_back(jtmod::MODE_NEWBIN);
        jtx::put_str(bins_blob, kv.second.name);
        jtx::put_u32(bins_blob, kv.second.parent_dir_idx);
        jtx::put_blob(bins_blob, kv.second.data);
        if (jtx::is_modded_key(kv.first)) mint(kv.first);
        jtmod::SplitResult ns = jtmod::split_records(kv.second.data);
        for (const jtmod::Record& r : ns.records)
            if (jtx::is_modded_key(jtx::key_of_record(r)))
                mint(jtx::key_of_record(r));
        ++res.bins_new;
    }

    std::vector<uint8_t> image;
    if (!options.image_path.empty()) {
        std::string image_error;
        image = image_to_square_thumbnail_png(options.image_path, 128,
                                              &image_error);
        if (image.empty())
            st.warning(nullptr, "could not read mod image (" + image_error
                                    + "); skipped");
    }

    // pack_jtmod (v3) + integrity trailer.
    static const char kMagic[] = "JADEJTM1";
    out.insert(out.end(), kMagic, kMagic + 8);
    jtx::put_u32(out, 3);
    jtx::put_u32(out, 0);
    jtx::put_str(out, project.base.game);
    jtx::put_str(out, project.base.archive_name);
    uint64_t bs64 = project.base.archive_size;
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(bs64 >> (8 * i)));
    for (size_t i = 0; i + 1 < project.base.archive_sha256.size() && i < 64;
         i += 2)
        out.push_back(uint8_t(std::strtoul(
            project.base.archive_sha256.substr(i, 2).c_str(), nullptr, 16)));
    jtx::put_str(out, options.title.empty() ? project.name : options.title);
    jtx::put_str(out, options.author.empty() ? project.author : options.author);
    jtx::put_str(out, options.version);
    jtx::put_str(out, options.description.empty()
                          ? project.description : options.description);
    jtx::put_str(out, options.created_iso.empty()
                          ? jtx::utc_iso_now() : options.created_iso);
    jtx::put_str(out, "jade_explorer");
    jtx::put_blob(out, image);
    std::sort(minted.begin(), minted.end());
    jtx::put_u32(out, uint32_t(minted.size()));
    for (uint32_t k : minted) jtx::put_u32(out, k);
    jtx::put_u32(out, n_bins);
    out.insert(out.end(), bins_blob.begin(), bins_blob.end());

    // seal_jtmod: MAGIC + sha256(SALT + core).
    static const char kSalt[] = "jtmod-integrity-v1";
    Sha256 s;
    s.update(reinterpret_cast<const uint8_t*>(kSalt), sizeof kSalt - 1);
    s.update(out.data(), out.size());
    std::string hex = s.hex();
    static const char kTrailerMagic[] = "JTMHASH1";
    out.insert(out.end(), kTrailerMagic, kTrailerMagic + 8);
    for (int i = 0; i < 32; ++i)
        out.push_back(uint8_t(std::strtoul(hex.substr(size_t(i) * 2, 2).c_str(),
                                           nullptr, 16)));

    if (!out_path.empty()) {
        const std::filesystem::path output =
            std::filesystem::u8path(out_path);
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path());
        std::ofstream f(output, std::ios::binary);
        if (!f) return fail("cannot write " + out_path);
        f.write(reinterpret_cast<const char*>(out.data()),
                std::streamsize(out.size()));
    }
    res.blob = std::move(out);
    res.output_path = out_path;
    res.minted_keys = std::move(minted);
    res.ok = true;
    std::ostringstream report;
    report << ".jtmod export — '" << project.name << "'\n"
           << "  bins: " << res.bins_subentry << " sub-entry, "
           << res.bins_wholebin << " whole-bin, " << res.bins_new
           << " new\n"
           << "  minted 0x7A keys: " << res.minted_keys.size() << "\n"
           << "  payload: " << res.blob.size() << " bytes";
    res.report = report.str();
    return res;
}

// verify_build_output (build/verify.py): the post-build integrity check.
// FAT re-open, key-set equality (modulo added keys), untouched entries
// byte-identical (size.grs exempt when anything changed), size.grs row
// audit (only touched/added rows may differ; slack fills allowed), touched
// entries round-trip (decompress + size-walk reaches every cookie-scanned
// sub-entry — a stale size field desyncs the game's navigation).
std::vector<BuildIssue> verify_build_output(
    const std::string& base_path, const std::string& output_path,
    const std::unordered_set<uint32_t>& touched_keys,
    const std::unordered_set<uint32_t>& added_keys,
    std::vector<std::string>& log) {
    std::vector<BuildIssue> issues;
    auto err = [&](const std::string& m) { issues.push_back({"error", m, ""}); };

    BigFile out_bf;
    try { out_bf.open(output_path); } catch (const std::exception& e) {
        err(std::string("output archive failed to re-open: ") + e.what());
        return issues;
    }
    BigFile base_bf;
    try { base_bf.open(base_path); } catch (const std::exception& e) {
        err(e.what());
        return issues;
    }
    std::unordered_map<uint32_t, const BFFile*> base_by_key, out_by_key;
    for (const auto& kv : base_bf.files)
        if (kv.second.key != INVALID_KEY) base_by_key[kv.second.key] = &kv.second;
    for (const auto& kv : out_bf.files)
        if (kv.second.key != INVALID_KEY) out_by_key[kv.second.key] = &kv.second;

    size_t only_base = 0, only_out = 0, missing_added = 0;
    for (const auto& kv : base_by_key)
        if (!out_by_key.count(kv.first)) ++only_base;
    for (const auto& kv : out_by_key)
        if (!base_by_key.count(kv.first) && !added_keys.count(kv.first))
            ++only_out;
    for (uint32_t k : added_keys)
        if (!out_by_key.count(k)) ++missing_added;
    char b[192];
    if (only_base) {
        std::snprintf(b, sizeof b,
                      "%zu entries present in base are missing in output",
                      only_base);
        err(b);
    }
    if (only_out) {
        std::snprintf(b, sizeof b,
                      "%zu entries present in output are missing in base "
                      "(operations should not silently add entries)", only_out);
        err(b);
    }
    if (missing_added) {
        std::snprintf(b, sizeof b,
                      "%zu entries scheduled as added were not actually written "
                      "to the output", missing_added);
        err(b);
    }

    // Untouched entries byte-identical (size.grs exempt when anything changed).
    std::unordered_set<uint32_t> size_grs_keys;
    for (const auto& kv : base_by_key)
        if (kv.second->name == "size.grs") size_grs_keys.insert(kv.first);
    bool sg_expected = !touched_keys.empty() || !added_keys.empty();
    size_t untouched_mismatches = 0;
    struct UntouchedMismatch {
        uint32_t key;
        std::string name;
        uint32_t base_length;
        uint32_t output_length;
    };
    std::vector<UntouchedMismatch> sample_mismatches;
    // Python's dict comprehension preserves FAT traversal order. Iterate the
    // FAT here as well so the diagnostic's first three samples are stable.
    std::unordered_set<uint32_t> visited_base_keys;
    for (const auto& indexed : base_bf.files) {
        uint32_t key = indexed.second.key;
        if (key == INVALID_KEY || !visited_base_keys.insert(key).second) continue;
        auto bit = base_by_key.find(key);
        if (bit == base_by_key.end()) continue;
        if (touched_keys.count(key)) continue;
        if (sg_expected && size_grs_keys.count(key)) continue;
        auto oit = out_by_key.find(key);
        if (oit == out_by_key.end()) continue;   // reported above
        const BFFile* fb = bit->second;
        const BFFile* fo = oit->second;
        if (fo->length != fb->length) {
            ++untouched_mismatches;
            if (sample_mismatches.size() < 5)
                sample_mismatches.push_back(
                    {key, fb->name, fb->length, fo->length});
            continue;
        }
        if (fb->length > 0 &&
            base_bf.read_data(fb->index) != out_bf.read_data(fo->index)) {
            ++untouched_mismatches;
            if (sample_mismatches.size() < 5)
                sample_mismatches.push_back(
                    {key, fb->name, fb->length, fo->length});
        }
    }
    if (untouched_mismatches) {
        std::string message = std::to_string(untouched_mismatches)
            + " untouched entries differ between base and output "
              "(the build silently changed them)";
        if (!sample_mismatches.empty()) {
            message += ". First: ";
            for (size_t i = 0; i < sample_mismatches.size() && i < 3; ++i) {
                if (i) message += ", ";
                const UntouchedMismatch& mismatch = sample_mismatches[i];
                char key_text[16];
                std::snprintf(key_text, sizeof key_text, "0x%08x", mismatch.key);
                message += key_text + std::string(" (")
                    + python_repr_string(mismatch.name) + ": "
                    + std::to_string(mismatch.base_length) + "->"
                    + std::to_string(mismatch.output_length) + "B)";
            }
        }
        err(message);
    }

    // size.grs row audit.
    if (sg_expected) {
        for (uint32_t sgk : size_grs_keys) {
            auto bit = base_by_key.find(sgk);
            auto oit = out_by_key.find(sgk);
            if (bit == base_by_key.end() || oit == out_by_key.end() ||
                bit->second->length != oit->second->length)
                continue;
            std::vector<uint8_t> base_sg = base_bf.read_data(bit->second->index);
            std::vector<uint8_t> out_sg = out_bf.read_data(oit->second->index);
            for (size_t off = 4; off + 8 <= base_sg.size(); off += 8) {
                uint32_t bk = matops::rd32(base_sg, off);
                uint32_t bs = matops::rd32(base_sg, off + 4);
                uint32_t ok_ = matops::rd32(out_sg, off);
                uint32_t os_ = matops::rd32(out_sg, off + 4);
                if (bk != ok_) {
                    if (bk == 0 && bs == 0 &&
                        (touched_keys.count(ok_) || added_keys.count(ok_)))
                        continue;   // slack fill for a new streaming row
                    std::snprintf(b, sizeof b,
                                  "size.grs has unexpected rewrites: row %zu: "
                                  "key changed 0x%08x->0x%08x", off, bk, ok_);
                    err(b);
                    break;
                }
                if (bs != os_ && !touched_keys.count(bk) &&
                    !added_keys.count(bk)) {
                    std::snprintf(b, sizeof b,
                                  "size.grs has unexpected rewrites: 0x%08x: "
                                  "%u->%u (key not in touched/added set)", bk,
                                  bs, os_);
                    err(b);
                    break;
                }
            }
        }
    }

    // Touched entries round-trip cleanly.
    size_t touched_ok = 0;
    std::vector<uint32_t> sorted_touched(touched_keys.begin(),
                                         touched_keys.end());
    std::sort(sorted_touched.begin(), sorted_touched.end());
    for (uint32_t key : sorted_touched) {
        auto oit = out_by_key.find(key);
        if (oit == out_by_key.end()) {
            std::snprintf(b, sizeof b, "touched entry 0x%08x did not "
                          "round-trip cleanly: missing in output", key);
            err(b);
            continue;
        }
        LzoResult dr = decompress_lzo(out_bf.read_data(oit->second->index));
        if (!dr.ok) {
            auto bit = base_by_key.find(key);
            bool base_ok = false;
            if (bit != base_by_key.end())
                base_ok = decompress_lzo(base_bf.read_data(bit->second->index)).ok;
            if (base_ok) {
                std::snprintf(b, sizeof b,
                              "touched entry 0x%08x did not round-trip "
                              "cleanly: output failed to decompress while "
                              "base did (LZO regression)", key);
                err(b);
                continue;
            }
            ++touched_ok;                        // both non-LZO (size.grs etc.)
            continue;
        }
        // Cookie-scan count (both cookie variants) vs the size-walk.
        size_t cookies = 0;
        static const uint8_t kC1[4] = {0x99, 0xC0, 0xFF, 0xEE};
        static const uint8_t kC2[4] = {0x99, 0xC0, 0xFF, 0xFE};
        for (size_t i = 0; i + 4 <= dr.data.size(); ++i)
            if (std::memcmp(dr.data.data() + i, kC1, 4) == 0 ||
                std::memcmp(dr.data.data() + i, kC2, 4) == 0)
                ++cookies;
        size_t walked = walk_sub_entries(dr.data).size();
        if (walked < cookies) {
            std::snprintf(b, sizeof b,
                          "touched entry 0x%08x did not round-trip cleanly: "
                          "size-walk reaches only %zu of %zu sub-entries — a "
                          "sub-entry size field is stale; the game's navigation "
                          "will break", key, walked, cookies);
            err(b);
            continue;
        }
        ++touched_ok;
    }
    const size_t untouched_total = base_by_key.size() - touched_keys.size();
    const size_t untouched_ok = untouched_total - untouched_mismatches;
    std::snprintf(b, sizeof b,
                  "verify: untouched entries identical: %zu of %zu",
                  untouched_ok, untouched_total);
    log.push_back(b);
    std::snprintf(b, sizeof b, "verify: touched entries reparse OK: %zu of %zu",
                  touched_ok, touched_keys.size());
    log.push_back(b);
    return issues;
}

namespace {

struct VerifyArchiveView {
    BigFile bf;
    std::unordered_map<uint32_t, const BFFile*> by_key;
    std::unordered_map<uint32_t, std::vector<uint8_t>> dec_cache;
    std::unordered_set<uint32_t> unreadable;

    explicit VerifyArchiveView(const std::string& path) {
        bf.open(path);
        for (const auto& [index, file] : bf.files) {
            (void)index;
            if (file.key != INVALID_KEY) by_key[file.key] = &file;
        }
    }

    const std::vector<uint8_t>* decompressed(uint32_t key) {
        const auto cached = dec_cache.find(key);
        if (cached != dec_cache.end()) return &cached->second;
        if (unreadable.count(key)) return nullptr;
        const auto file = by_key.find(key);
        if (file == by_key.end()) {
            unreadable.insert(key);
            return nullptr;
        }
        try {
            LzoResult result = decompress_lzo(bf.read_data(file->second->index));
            if (!result.ok) {
                unreadable.insert(key);
                return nullptr;
            }
            return &dec_cache.emplace(key, std::move(result.data)).first->second;
        } catch (...) {
            unreadable.insert(key);
            return nullptr;
        }
    }
};

bool texture_full(const SubEntry& sub, TexInfo* info = nullptr) {
    if (!is_texture_entry(sub.data.data(), sub.data.size())) return false;
    TexInfo parsed = parse_texture(sub.data.data(), sub.data.size());
    if (!parsed.valid) return false;
    const size_t pixel_bytes =
        sub.data.size() - std::min(sub.data.size(), parsed.pix_start);
    if (is_placeholder(parsed, pixel_bytes)) return false;
    if (info != nullptr) *info = std::move(parsed);
    return true;
}

double mean_abs_difference(const std::vector<uint8_t>& left,
                           const std::vector<uint8_t>& right) {
    if (left.size() != right.size() || left.empty())
        throw std::runtime_error("RGBA buffers have different sizes");
    uint64_t total = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        const int delta = int(left[i]) - int(right[i]);
        total += uint64_t(delta < 0 ? -delta : delta);
    }
    return double(total) / double(left.size());
}

double texture_tolerance(uint32_t format, bool added) {
    if (format == 0) return 4.0;
    if (format == 5 || format == 7) return 28.0;
    if (!added && (format == 1 || format == 11)) return 48.0;
    if (!added && format == 6) return 28.0;
    return 32.0;
}

std::string source_path_for(const ModProject& project,
                            const json::Value& op) {
    const std::string source = vals::str_or(vals::P(op, "source"), "");
    return source.empty() ? std::string()
                          : resolve_asset(project.jmod_dir, source);
}

std::vector<uint8_t> source_rgba_at_size(const std::string& source_path,
                                         uint32_t width, uint32_t height,
                                         std::string* error) {
    RgbaImage source = load_rgba_image(source_path);
    if (!source.ok) {
        if (error != nullptr) *error = source.error;
        return {};
    }
    if (source.width == width && source.height == height)
        return std::move(source.rgba);
    std::vector<uint8_t> resized = resize_rgba_lanczos(
        source.rgba.data(), source.width, source.height, width, height);
    if (resized.empty() && error != nullptr) *error = "image resize failed";
    return resized;
}

std::vector<VerifyLeafIssue> check_texture_insertion(
    const std::vector<uint8_t>& old_dec,
    const std::vector<uint8_t>& new_dec, uint32_t new_key,
    size_t expected_new_records) {
    std::vector<VerifyLeafIssue> issues;
    const std::vector<SubEntry> old_subs = walk_sub_entries(old_dec);
    const std::vector<SubEntry> new_subs = walk_sub_entries(new_dec);
    const size_t inserted = std::count_if(
        new_subs.begin(), new_subs.end(),
        [new_key](const SubEntry& sub) { return sub.key == new_key; });
    char message[320];
    if (inserted == 0) {
        std::snprintf(message, sizeof message,
                      "key 0x%08X not found after insert", new_key);
        issues.push_back({"error", message});
        return issues;
    }
    if (expected_new_records != std::numeric_limits<size_t>::max() &&
        new_subs.size() != old_subs.size() + expected_new_records) {
        std::snprintf(message, sizeof message,
                      "size-walk desync: %zu records before, %zu after "
                      "inserting %zu", old_subs.size(), new_subs.size(),
                      expected_new_records);
        issues.push_back({"error", message});
        return issues;
    }

    const size_t scanned = parse_sub_entries(new_dec).size();
    if (new_subs.size() < scanned) {
        std::snprintf(message, sizeof message,
                      "size-walk reaches only %zu of %zu cookie-scanned "
                      "records", new_subs.size(), scanned);
        issues.push_back({"error", message});
        return issues;
    }

    auto check_wave = [&](const char* wave_name, bool stub) {
        auto predicate = [stub](const SubEntry& sub) {
            return stub ? assetadd::is_tex_stub(sub.data)
                        : assetadd::is_tex_full(sub.data);
        };
        std::vector<uint32_t> wave;
        for (const SubEntry& sub : new_subs)
            if (predicate(sub)) wave.push_back(sub.key);
        const auto position = std::find(wave.begin(), wave.end(), new_key);
        if (position == wave.end()) {
            if (!stub) {
                issues.push_back(
                    {"error", std::string("new key missing from the ") +
                                  wave_name + " wave"});
            }
            return;
        }
        const size_t index = size_t(position - wave.begin());
        const bool previous_ok = index == 0 || wave[index - 1] <= new_key;
        const bool next_ok = index + 1 == wave.size() ||
                             new_key <= wave[index + 1];
        if (previous_ok && next_ok) return;

        std::vector<uint32_t> shipped;
        for (const SubEntry& sub : old_subs)
            if (predicate(sub)) shipped.push_back(sub.key);
        const bool shipped_sorted = std::is_sorted(shipped.begin(), shipped.end());
        const uint32_t previous = index == 0 ? 0 : wave[index - 1];
        const uint32_t next = index + 1 == wave.size()
                                  ? INVALID_KEY
                                  : wave[index + 1];
        std::snprintf(message, sizeof message,
                      "new key 0x%08X is out of order in the %s wave "
                      "(neighbours 0x%08X / 0x%08X)%s",
                      new_key, wave_name, previous, next,
                      shipped_sorted
                          ? ""
                          : " — shipped wave was already unsorted");
        issues.push_back({shipped_sorted ? "error" : "warning", message});
    };
    check_wave("stub", true);
    check_wave("full", false);
    return issues;
}

}  // namespace

std::vector<BuildIssue> verify_replaced_textures(
    const ModProject& project, const std::string& output_path,
    std::vector<std::string>& log) {
    std::vector<const json::Value*> operations;
    for (const json::Value* op : project.enabled_operations())
        if (vals::str_or(op->find("op"), "") == "replace_texture")
            operations.push_back(op);
    if (operations.empty()) return {};

    VerifyArchiveView output(output_path);
    std::vector<BuildIssue> issues;
    size_t checked = 0;
    char message[384];
    for (const json::Value* op : operations) {
        const uint32_t entry_key = key_of(vals::T(*op, "entry_key"));
        const uint32_t sub_key = key_of(vals::T(*op, "sub_key"));
        const std::string source_path = source_path_for(project, *op);
        std::error_code path_error;
        if (source_path.empty() || !std::filesystem::exists(
                                       std::filesystem::u8path(source_path),
                                       path_error)) {
            continue;
        }
        if (!output.by_key.count(entry_key)) {
            std::snprintf(message, sizeof message,
                          "replaced texture: entry 0x%08x not in output",
                          entry_key);
            issues.push_back({"warning", message, op_id_of(*op)});
            continue;
        }
        const std::vector<uint8_t>* dec = output.decompressed(entry_key);
        if (dec == nullptr) {
            std::snprintf(message, sizeof message,
                          "replaced texture 0x%08x: entry did not decompress",
                          sub_key);
            issues.push_back({"warning", message, op_id_of(*op)});
            continue;
        }
        const std::vector<SubEntry> subs = walk_sub_entries(*dec);
        const SubEntry* texture = nullptr;
        TexInfo info;
        for (const SubEntry& sub : subs) {
            if (sub.key == sub_key && texture_full(sub, &info)) {
                texture = &sub;
                break;
            }
        }
        if (texture == nullptr) {
            std::snprintf(message, sizeof message,
                          "replaced texture 0x%08x: not found in output entry",
                          sub_key);
            issues.push_back({"warning", message, op_id_of(*op)});
            continue;
        }
        const std::vector<uint8_t>* palette = palette_for_texture(info, subs);
        const std::vector<uint8_t> output_rgba = decode_texture(
            texture->data.data(), texture->data.size(), info,
            palette == nullptr ? nullptr : palette->data(),
            palette == nullptr ? 0 : palette->size());
        if (output_rgba.empty()) {
            std::snprintf(message, sizeof message,
                          "replaced texture 0x%08x: could not decode from output",
                          sub_key);
            issues.push_back({"warning", message, op_id_of(*op)});
            continue;
        }
        std::string source_error;
        const std::vector<uint8_t> source_rgba = source_rgba_at_size(
            source_path, info.width, info.height, &source_error);
        if (source_rgba.empty()) {
            std::snprintf(message, sizeof message,
                          "replaced texture 0x%08x: source unreadable: %s",
                          sub_key, source_error.c_str());
            issues.push_back({"warning", message, op_id_of(*op)});
            continue;
        }
        const double difference =
            mean_abs_difference(output_rgba, source_rgba);
        const double tolerance = texture_tolerance(info.format, false);
        ++checked;
        if (difference > tolerance) {
            std::snprintf(
                message, sizeof message,
                "replaced texture 0x%08x: rebuilt image differs from source "
                "— mean abs diff %.1f exceeds %.0f for format %u (possible "
                "encode/decode regression)",
                sub_key, difference, tolerance, info.format);
            issues.push_back({"warning", message, op_id_of(*op)});
        } else {
            std::snprintf(message, sizeof message,
                          "verify-tex: 0x%08x matches source (delta %.1f)",
                          sub_key, difference);
            log.push_back(message);
        }
    }
    std::snprintf(message, sizeof message,
                  "verify-tex: %zu replaced texture(s) pixel-checked", checked);
    log.push_back(message);
    return issues;
}

std::vector<BuildIssue> verify_added_assets(
    const ModProject& project, const std::string& base_path,
    const std::string& output_path, std::vector<std::string>& log) {
    std::vector<const json::Value*> textures;
    std::vector<const json::Value*> materials;
    for (const json::Value* op : project.enabled_operations()) {
        const std::string type = vals::str_or(op->find("op"), "");
        if (type == "add_texture") textures.push_back(op);
        else if (type == "add_material") materials.push_back(op);
    }
    if (textures.empty() && materials.empty()) return {};

    VerifyArchiveView output(output_path);
    VerifyArchiveView base(base_path);
    std::vector<BuildIssue> issues;
    char message[384];
    for (const json::Value* op : textures) {
        const uint32_t entry_key = key_of(vals::T(*op, "entry_key"));
        const uint32_t new_key = key_of(vals::P(*op, "new_key"));
        const std::vector<uint8_t>* dec = output.decompressed(entry_key);
        if (dec == nullptr) {
            std::snprintf(message, sizeof message,
                          "added texture: entry 0x%08x unreadable in output",
                          entry_key);
            issues.push_back({"error", message, op_id_of(*op)});
            continue;
        }
        if (const std::vector<uint8_t>* old_dec = base.decompressed(entry_key)) {
            for (const VerifyLeafIssue& issue :
                 check_texture_insertion(*old_dec, *dec, new_key)) {
                issues.push_back(
                    {issue.level, issue.message, op_id_of(*op)});
            }
        }

        const std::vector<SubEntry> subs = walk_sub_entries(*dec);
        const SubEntry* texture = nullptr;
        TexInfo info;
        for (const SubEntry& sub : subs) {
            if (sub.key == new_key && texture_full(sub, &info)) {
                texture = &sub;
                break;
            }
        }
        if (texture == nullptr) {
            std::snprintf(message, sizeof message,
                          "added texture 0x%08x has no full record in output "
                          "entry 0x%08x", new_key, entry_key);
            issues.push_back({"error", message, op_id_of(*op)});
            continue;
        }
        const std::vector<uint8_t> output_rgba = decode_texture(
            texture->data.data(), texture->data.size(), info);
        const std::string source_path = source_path_for(project, *op);
        std::string source_error;
        const std::vector<uint8_t> source_rgba = source_path.empty()
            ? std::vector<uint8_t>()
            : source_rgba_at_size(source_path, info.width, info.height,
                                  &source_error);
        if (output_rgba.empty() || source_rgba.empty()) {
            std::snprintf(message, sizeof message,
                          "verify-add: 0x%08x present (pixel check skipped)",
                          new_key);
            log.push_back(message);
            continue;
        }
        const double difference =
            mean_abs_difference(output_rgba, source_rgba);
        const double tolerance = texture_tolerance(info.format, true);
        if (difference > tolerance) {
            std::snprintf(message, sizeof message,
                          "added texture 0x%08x: rebuilt image differs from "
                          "source — mean abs diff %.1f exceeds %.0f for "
                          "format %u", new_key, difference, tolerance,
                          info.format);
            issues.push_back({"warning", message, op_id_of(*op)});
        } else {
            std::snprintf(message, sizeof message,
                          "verify-add: texture 0x%08x matches source (delta "
                          "%.1f)", new_key, difference);
            log.push_back(message);
        }
    }

    for (const json::Value* op : materials) {
        const uint32_t entry_key = key_of(vals::T(*op, "entry_key"));
        const uint32_t new_key = key_of(vals::P(*op, "new_key"));
        const uint32_t expected_texture =
            key_of(vals::P(*op, "texture_key"));
        const std::vector<uint8_t>* dec = output.decompressed(entry_key);
        if (dec == nullptr) {
            std::snprintf(message, sizeof message,
                          "added material: entry 0x%08x unreadable in output",
                          entry_key);
            issues.push_back({"error", message, op_id_of(*op)});
            continue;
        }
        const std::vector<SubEntry> subs = walk_sub_entries(*dec);
        const auto record = std::find_if(
            subs.begin(), subs.end(),
            [new_key](const SubEntry& sub) { return sub.key == new_key; });
        if (record == subs.end()) {
            std::snprintf(message, sizeof message,
                          "added material 0x%08x not in output entry 0x%08x",
                          new_key, entry_key);
            issues.push_back({"error", message, op_id_of(*op)});
            continue;
        }
        const uint32_t actual = resolve_texture_key(
            record->data.data(), record->data.size());
        if (actual != expected_texture) {
            if (actual != 0) {
                std::snprintf(message, sizeof message,
                              "added material 0x%08x points at 0x%08x, "
                              "expected 0x%08x", new_key, actual,
                              expected_texture);
            } else {
                std::snprintf(message, sizeof message,
                              "added material 0x%08x points at nothing, "
                              "expected 0x%08x", new_key, expected_texture);
            }
            issues.push_back({"error", message, op_id_of(*op)});
        } else {
            std::snprintf(message, sizeof message,
                          "verify-add: material 0x%08x -> 0x%08x OK",
                          new_key, expected_texture);
            log.push_back(message);
        }
    }
    return issues;
}

std::vector<BuildIssue> validate_spins(const std::string& base_archive_path,
                                       const std::string& output_path,
                                       const std::vector<uint32_t>& touched_keys,
                                       std::vector<std::string>& log) {
    std::vector<BuildIssue> issues;
    if (touched_keys.empty()) {
        log.push_back("spin check: no touched entries; skipped");
        return issues;
    }
    BigFile out_bf;
    try { out_bf.open(output_path); } catch (const std::exception& e) {
        issues.push_back({"warning", std::string("spin check: ") + e.what(), ""});
        return issues;
    }
    const BFFile* wolinfo_fi = nullptr;
    for (const auto& kv : out_bf.files)
        if (kv.second.key == WOLINFO_KEY) { wolinfo_fi = &kv.second; break; }
    if (wolinfo_fi == nullptr) {
        log.push_back("spin check: archive has no WOLInfo table (SoT) — skipped");
        return issues;
    }
    WolInfo wi = parse_wolinfo(out_bf.read_data(wolinfo_fi->index));
    if (!wi.ok) {
        issues.push_back({"warning", "spin check: WOLInfo did not parse", ""});
        return issues;
    }

    // WOLs whose dep closure contains a touched entry key.
    std::unordered_set<uint32_t> touched(touched_keys.begin(), touched_keys.end());
    std::vector<uint32_t> affected;
    for (const WolEntry& e : wi.entries)
        for (uint32_t d : e.deps)
            if (touched.count(internal_to_bf_key(d))) {
                affected.push_back(e.wol_key);
                break;
            }
    if (affected.empty()) {
        log.push_back("spin check: no WOL's dep closure touches a modified "
                      "entry; skipped");
        return issues;
    }
    {
        char b[80];
        std::snprintf(b, sizeof b,
                      "spin check: %zu affected WOL(s); building provider "
                      "indexes (slow)…", affected.size());
        log.push_back(b);
    }

    BigFile base_bf;
    try { base_bf.open(base_archive_path); } catch (const std::exception& e) {
        issues.push_back({"warning", std::string("spin check: ") + e.what(), ""});
        return issues;
    }
    loadsim::KeyIndex out_idx = loadsim::build_key_index(out_bf);
    loadsim::KeyIndex base_idx = loadsim::build_key_index(base_bf);

    for (uint32_t wol : affected) {
        loadsim::SimReport base_r = loadsim::simulate_zone(base_bf, wol, base_idx);
        loadsim::SimReport out_r = loadsim::simulate_zone(out_bf, wol, out_idx);
        std::unordered_set<uint32_t> base_unres;
        for (const loadsim::Unresolved& u : base_r.unresolved)
            base_unres.insert(u.ref);
        for (const loadsim::Unresolved& u : out_r.unresolved) {
            if (base_unres.count(u.ref)) continue;   // shipped behaviour
            char b[160];
            std::snprintf(b, sizeof b,
                          "spin check: WOL 0x%08x gained an unresolved ref "
                          "0x%08x (%zu provider bin(s) elsewhere) — possible "
                          "load freeze", wol, u.ref, u.providers.size());
            issues.push_back({"warning", b, ""});
        }
        if (out_r.missing_dep_bins.size() > base_r.missing_dep_bins.size()) {
            char b[120];
            std::snprintf(b, sizeof b,
                          "spin check: WOL 0x%08x lost dep bin(s): %zu missing "
                          "(base had %zu)", wol, out_r.missing_dep_bins.size(),
                          base_r.missing_dep_bins.size());
            issues.push_back({"warning", b, ""});
        }
    }
    char b[96];
    std::snprintf(b, sizeof b,
                  "spin check: %zu WOL(s) simulated, %zu new warning(s)",
                  affected.size(), issues.size());
    log.push_back(b);
    return issues;
}

}  // namespace project
}  // namespace jade
