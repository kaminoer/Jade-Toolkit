// Python-compatible cache/progress surface for io_ops/load_sim.py.
#include "jade/LoadSim.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>

#include "jade/Compression.hpp"
#include "jade/SubEntry.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace jade {
namespace loadsim {
namespace {

namespace fs = std::filesystem;

void emit(const LoadSimLogFn& log, const std::string& message) {
    if (log) log(message);
}

std::string base_name(const std::string& path) {
    const fs::path value = fs::u8path(path).filename();
#ifdef _WIN32
    return value.u8string();
#else
    return value.string();
#endif
}

struct FileSignature {
    uint64_t size = 0;
    int64_t mtime = 0;
};

FileSignature file_signature(const std::string& path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    const fs::path native = fs::u8path(path);
    if (!GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data))
        throw std::runtime_error("cannot stat key-index archive");
    ULARGE_INTEGER length{}, written{};
    length.HighPart = data.nFileSizeHigh;
    length.LowPart = data.nFileSizeLow;
    written.HighPart = data.ftLastWriteTime.dwHighDateTime;
    written.LowPart = data.ftLastWriteTime.dwLowDateTime;
    constexpr uint64_t WINDOWS_TO_UNIX_SECONDS = 11644473600ull;
    FileSignature result;
    result.size = length.QuadPart;
    result.mtime = int64_t(written.QuadPart / 10000000ull -
                           WINDOWS_TO_UNIX_SECONDS);
    return result;
#else
    std::error_code ec;
    FileSignature result;
    result.size = fs::file_size(fs::u8path(path), ec);
    if (ec) throw std::runtime_error("cannot stat key-index archive");
    const auto file_time = fs::last_write_time(fs::u8path(path), ec);
    if (ec) throw std::runtime_error("cannot stat key-index archive");
    const auto system_time = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
        file_time - fs::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    result.mtime = std::chrono::system_clock::to_time_t(system_time);
    return result;
#endif
}

// A small pickle VM for the protocol-4 object emitted by Python's
// pickle.dump({"sig": (size, mtime), "idx": index}). Memo references matter:
// Python reuses each BF filename string across that bin's provider tuples.
struct PickleNode;
using PickleValue = std::shared_ptr<PickleNode>;
struct PickleNode {
    enum class Type { marker, integer, string, list, tuple, dict, none };
    explicit PickleNode(Type value_type) : type(value_type) {}
    Type type;
    int64_t integer = 0;
    std::string string;
    std::vector<PickleValue> sequence;
    std::vector<std::pair<PickleValue, PickleValue>> mapping;
};

PickleValue pickle_value(PickleNode::Type type) {
    return std::make_shared<PickleNode>(type);
}

uint64_t pickle_uint(const std::vector<uint8_t>& bytes, size_t& pos,
                     size_t count) {
    if (count > 8 || pos + count > bytes.size())
        throw std::runtime_error("invalid key-index pickle integer");
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i)
        value |= uint64_t(bytes[pos++]) << (i * 8);
    return value;
}

PickleValue read_pickle(const std::string& path) {
    std::ifstream file(fs::u8path(path), std::ios::binary);
    if (!file) throw std::runtime_error("cannot open key-index cache");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
    std::vector<PickleValue> stack, memo;
    size_t pos = 0;
    auto require = [&](size_t count) {
        if (pos + count > bytes.size())
            throw std::runtime_error("truncated key-index pickle");
    };
    auto push_int = [&](int64_t value) {
        PickleValue node = pickle_value(PickleNode::Type::integer);
        node->integer = value;
        stack.push_back(std::move(node));
    };
    auto push_string = [&](size_t count) {
        require(count);
        PickleValue node = pickle_value(PickleNode::Type::string);
        node->string.assign(reinterpret_cast<const char*>(bytes.data() + pos),
                            count);
        pos += count;
        stack.push_back(std::move(node));
    };
    auto marker_pos = [&]() {
        for (size_t i = stack.size(); i-- > 0;)
            if (stack[i]->type == PickleNode::Type::marker) return i;
        throw std::runtime_error("invalid key-index pickle mark");
    };
    auto memo_store = [&](size_t index) {
        if (stack.empty())
            throw std::runtime_error("invalid key-index pickle memo");
        if (memo.size() <= index) memo.resize(index + 1);
        memo[index] = stack.back();
    };

    while (pos < bytes.size()) {
        const uint8_t op = bytes[pos++];
        switch (op) {
        case 0x80: require(1); ++pos; break;                         // PROTO
        case 0x95: require(8); pos += 8; break;                      // FRAME
        case '}': stack.push_back(pickle_value(PickleNode::Type::dict)); break;
        case ']': stack.push_back(pickle_value(PickleNode::Type::list)); break;
        case ')': stack.push_back(pickle_value(PickleNode::Type::tuple)); break;
        case 'N': stack.push_back(pickle_value(PickleNode::Type::none)); break;
        case '(':
            stack.push_back(pickle_value(PickleNode::Type::marker));
            break;
        case 0x94:                                                   // MEMOIZE
            if (stack.empty())
                throw std::runtime_error("invalid key-index pickle memo");
            memo.push_back(stack.back());
            break;
        case 'q': require(1); memo_store(bytes[pos++]); break;       // BINPUT
        case 'r': {                                                  // LONG_BINPUT
            require(4);
            const size_t index = size_t(pickle_uint(bytes, pos, 4));
            memo_store(index);
            break;
        }
        case 'h': {                                                  // BINGET
            require(1);
            const size_t index = bytes[pos++];
            if (index >= memo.size() || !memo[index])
                throw std::runtime_error("invalid key-index pickle get");
            stack.push_back(memo[index]);
            break;
        }
        case 'j': {                                                  // LONG_BINGET
            require(4);
            const size_t index = size_t(pickle_uint(bytes, pos, 4));
            if (index >= memo.size() || !memo[index])
                throw std::runtime_error("invalid key-index pickle get");
            stack.push_back(memo[index]);
            break;
        }
        case 'K': require(1); push_int(bytes[pos++]); break;         // BININT1
        case 'M': {                                                  // BININT2
            require(2);
            push_int(int64_t(pickle_uint(bytes, pos, 2)));
            break;
        }
        case 'J': {                                                  // BININT
            require(4);
            const uint32_t value = uint32_t(pickle_uint(bytes, pos, 4));
            push_int(int32_t(value));
            break;
        }
        case 0x8a:                                                   // LONG1
        case 0x8b: {                                                 // LONG4
            size_t count;
            if (op == 0x8a) { require(1); count = bytes[pos++]; }
            else { require(4); count = size_t(pickle_uint(bytes, pos, 4)); }
            require(count);
            const size_t start = pos;
            uint64_t raw = pickle_uint(bytes, pos, count);
            if (count && count < 8 && (bytes[start + count - 1] & 0x80))
                raw |= (~uint64_t(0)) << (count * 8);
            push_int(int64_t(raw));
            break;
        }
        case 0x8c: {                                                 // SHORT_BINUNICODE
            require(1);
            const size_t count = bytes[pos++];
            push_string(count);
            break;
        }
        case 'X': {                                                  // BINUNICODE
            require(4);
            const size_t count = size_t(pickle_uint(bytes, pos, 4));
            push_string(count);
            break;
        }
        case 0x8d: {                                                 // BINUNICODE8
            require(8);
            const size_t count = size_t(pickle_uint(bytes, pos, 8));
            push_string(count);
            break;
        }
        case 0x85:                                                   // TUPLE1
        case 0x86:                                                   // TUPLE2
        case 0x87: {                                                 // TUPLE3
            const size_t count = size_t(op - 0x84);
            if (stack.size() < count)
                throw std::runtime_error("invalid key-index pickle tuple");
            PickleValue tuple = pickle_value(PickleNode::Type::tuple);
            tuple->sequence.assign(stack.end() - std::ptrdiff_t(count),
                                   stack.end());
            stack.erase(stack.end() - std::ptrdiff_t(count), stack.end());
            stack.push_back(std::move(tuple));
            break;
        }
        case 't': {                                                  // TUPLE
            const size_t mark = marker_pos();
            PickleValue tuple = pickle_value(PickleNode::Type::tuple);
            tuple->sequence.assign(stack.begin() + std::ptrdiff_t(mark + 1),
                                   stack.end());
            stack.erase(stack.begin() + std::ptrdiff_t(mark), stack.end());
            stack.push_back(std::move(tuple));
            break;
        }
        case 'a': {                                                  // APPEND
            if (stack.size() < 2 ||
                stack[stack.size() - 2]->type != PickleNode::Type::list)
                throw std::runtime_error("invalid key-index pickle append");
            PickleValue item = stack.back();
            stack.pop_back();
            stack.back()->sequence.push_back(std::move(item));
            break;
        }
        case 'e': {                                                  // APPENDS
            const size_t mark = marker_pos();
            if (mark == 0 || stack[mark - 1]->type != PickleNode::Type::list)
                throw std::runtime_error("invalid key-index pickle appends");
            auto& list = stack[mark - 1]->sequence;
            list.insert(list.end(),
                        stack.begin() + std::ptrdiff_t(mark + 1), stack.end());
            stack.erase(stack.begin() + std::ptrdiff_t(mark), stack.end());
            break;
        }
        case 's': {                                                  // SETITEM
            if (stack.size() < 3 ||
                stack[stack.size() - 3]->type != PickleNode::Type::dict)
                throw std::runtime_error("invalid key-index pickle setitem");
            PickleValue value = stack.back(); stack.pop_back();
            PickleValue key = stack.back(); stack.pop_back();
            stack.back()->mapping.push_back({std::move(key), std::move(value)});
            break;
        }
        case 'u': {                                                  // SETITEMS
            const size_t mark = marker_pos();
            if (mark == 0 || stack[mark - 1]->type != PickleNode::Type::dict ||
                (stack.size() - mark - 1) % 2)
                throw std::runtime_error("invalid key-index pickle setitems");
            auto& dict = stack[mark - 1]->mapping;
            for (size_t i = mark + 1; i < stack.size(); i += 2)
                dict.push_back({stack[i], stack[i + 1]});
            stack.erase(stack.begin() + std::ptrdiff_t(mark), stack.end());
            break;
        }
        case '.':
            if (stack.empty())
                throw std::runtime_error("empty key-index pickle");
            return stack.back();
        default:
            throw std::runtime_error("unsupported key-index pickle opcode");
        }
    }
    throw std::runtime_error("unterminated key-index pickle");
}

const PickleValue* dict_item(const PickleValue& dict, const std::string& key) {
    if (!dict || dict->type != PickleNode::Type::dict) return nullptr;
    for (const auto& item : dict->mapping)
        if (item.first->type == PickleNode::Type::string &&
            item.first->string == key)
            return &item.second;
    return nullptr;
}

bool index_from_pickle(const PickleValue& root,
                       const FileSignature& signature, KeyIndex& index) {
    const PickleValue* sig = dict_item(root, "sig");
    const PickleValue* idx = dict_item(root, "idx");
    if (!sig || !idx || (*sig)->type != PickleNode::Type::tuple ||
        (*sig)->sequence.size() != 2 ||
        (*sig)->sequence[0]->type != PickleNode::Type::integer ||
        (*sig)->sequence[1]->type != PickleNode::Type::integer)
        throw std::runtime_error("invalid key-index cache schema");
    if (uint64_t((*sig)->sequence[0]->integer) != signature.size ||
        (*sig)->sequence[1]->integer != signature.mtime)
        return false;
    if ((*idx)->type != PickleNode::Type::dict)
        throw std::runtime_error("invalid key-index cache schema");
    for (const auto& row : (*idx)->mapping) {
        if (row.first->type != PickleNode::Type::integer ||
            row.second->type != PickleNode::Type::list)
            throw std::runtime_error("invalid key-index cache row");
        const uint32_t key = uint32_t(row.first->integer);
        std::vector<std::pair<uint32_t, std::string>> providers;
        for (const PickleValue& provider : row.second->sequence) {
            if (provider->type != PickleNode::Type::tuple ||
                provider->sequence.size() != 2 ||
                provider->sequence[0]->type != PickleNode::Type::integer ||
                provider->sequence[1]->type != PickleNode::Type::string)
                throw std::runtime_error("invalid key-index cache provider");
            providers.push_back({uint32_t(provider->sequence[0]->integer),
                                 provider->sequence[1]->string});
        }
        index.pos[key] = index.items.size();
        index.items.push_back({key, std::move(providers)});
    }
    return true;
}

void write_u32(std::ostream& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out.put(char(value >> (i * 8)));
}

void pickle_integer(std::ostream& out, uint64_t value) {
    if (value <= 0xff) {
        out.put('K'); out.put(char(value)); return;
    }
    if (value <= 0xffff) {
        out.put('M'); out.put(char(value)); out.put(char(value >> 8)); return;
    }
    if (value <= 0x7fffffff) {
        out.put('J'); write_u32(out, uint32_t(value)); return;
    }
    uint8_t bytes[9]{};
    size_t count = 0;
    do { bytes[count++] = uint8_t(value); value >>= 8; } while (value);
    if (bytes[count - 1] & 0x80) bytes[count++] = 0;
    out.put(char(0x8a)); out.put(char(count));
    out.write(reinterpret_cast<const char*>(bytes), std::streamsize(count));
}

void pickle_string(std::ostream& out, const std::string& value) {
    out.put('X');
    write_u32(out, uint32_t(value.size()));
    out.write(value.data(), std::streamsize(value.size()));
}

void write_pickle(const std::string& path, const FileSignature& signature,
                  const KeyIndex& index) {
    std::ofstream out(fs::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write key-index cache");
    out.put(char(0x80)); out.put(char(2));                            // PROTO 2
    out.put('}'); out.put('(');                                      // root dict
    pickle_string(out, "sig");
    pickle_integer(out, signature.size);
    pickle_integer(out, uint64_t(signature.mtime));
    out.put(char(0x86));                                             // TUPLE2
    pickle_string(out, "idx");
    out.put('}'); out.put('(');                                      // index dict
    for (const auto& item : index.items) {
        pickle_integer(out, item.first);
        out.put(']'); out.put('(');                                  // providers
        for (const auto& provider : item.second) {
            pickle_integer(out, provider.first);
            pickle_string(out, provider.second);
            out.put(char(0x86));                                     // TUPLE2
        }
        out.put('e');                                                // APPENDS
    }
    out.put('u'); out.put('u'); out.put('.');                        // SETITEMS
    if (!out) throw std::runtime_error("cannot write key-index cache");
}

std::string elapsed_text(const std::chrono::steady_clock::time_point& start) {
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::ostringstream out;
    out << std::fixed << std::setprecision(0) << seconds;
    return out.str();
}

}  // namespace

KeyIndex build_key_index(const BigFile& bf, const std::string& bf_path,
                         bool cache, LoadSimLogFn log) {
    const std::string cache_path = bf_path + ".subkeyidx.pkl";
    if (cache) {
        std::error_code ec;
        if (fs::exists(fs::u8path(cache_path), ec)) {
            const FileSignature signature = file_signature(bf_path);
            KeyIndex cached;
            if (index_from_pickle(read_pickle(cache_path), signature, cached)) {
                emit(log, "  key index: loaded cache (" +
                          std::to_string(cached.items.size()) + " keys)");
                return cached;
            }
        }
    }

    const auto start = std::chrono::steady_clock::now();
    KeyIndex index;
    std::vector<const BFFile*> bins;
    for (const auto& item : bf.files) {
        const BFFile& file = item.second;
        if (!file.name.empty() && file.key != INVALID_KEY)
            bins.push_back(&file);
    }
    for (size_t n = 0; n < bins.size(); ++n) {
        const BFFile& file = *bins[n];
        const LzoResult decompressed = decompress_lzo(bf.read_data(file.index));
        if (!decompressed.ok || decompressed.data.empty()) continue;
        const std::vector<SubEntry> subs = walk_sub_entries(decompressed.data);
        for (const SubEntry& sub : subs) {
            auto found = index.pos.find(sub.key);
            size_t slot;
            if (found == index.pos.end()) {
                slot = index.items.size();
                index.pos[sub.key] = slot;
                index.items.push_back({sub.key, {}});
            } else {
                slot = found->second;
            }
            auto& providers = index.items[slot].second;
            bool present = false;
            for (const auto& provider : providers)
                if (provider.first == file.key && provider.second == file.name) {
                    present = true;
                    break;
                }
            if (!present) providers.push_back({file.key, file.name});
        }
        if (n % 1000 == 0)
            emit(log, "  key index: " + std::to_string(n) + "/" +
                      std::to_string(bins.size()) + " bins, " +
                      std::to_string(index.items.size()) + " keys (" +
                      elapsed_text(start) + "s)");
    }
    emit(log, "  key index: " + std::to_string(index.items.size()) +
              " keys from " + std::to_string(bins.size()) + " bins (" +
              elapsed_text(start) + "s)");
    if (cache) {
        const FileSignature signature = file_signature(bf_path);
        write_pickle(cache_path, signature, index);
        emit(log, "  key index: cached -> " + base_name(cache_path));
    }
    return index;
}

SimReport simulate_zone(const BigFile& bf, const std::string& bf_path,
                        uint32_t wol_key, const KeyIndex* key_index,
                        bool cache, LoadSimLogFn log) {
    if (key_index) return simulate_zone(bf, wol_key, *key_index);
    KeyIndex built = build_key_index(bf, bf_path, cache, std::move(log));
    return simulate_zone(bf, wol_key, built);
}

}  // namespace loadsim
}  // namespace jade
