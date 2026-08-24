// BigFile.cpp — implementation. Faithful port of core/bigfile.py (read + write).
#include "jade/BigFile.hpp"

#include <cstring>
#include <cstdio>
#include <filesystem>
#include <set>
#include <stdexcept>

#include "jade/Compression.hpp"  // lzo_terminator_offset (size.grs declared len)

namespace jade {

namespace {

// Read n bytes at an absolute offset; returns fewer bytes at EOF, mirroring
// Python's f.read(n). Never loads the whole (up to ~500 MB) file.
std::vector<uint8_t> read_region(std::ifstream& f, uint64_t off, size_t n) {
    std::vector<uint8_t> buf(n);
    f.clear();
    f.seekg(static_cast<std::streamoff>(off), std::ios::beg);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
    buf.resize(static_cast<size_t>(f.gcount()));
    return buf;
}

// Mirror Python bytes.decode('ascii', errors='replace'): bytes < 0x80 pass
// through; bytes >= 0x80 become U+FFFD (UTF-8 EF BF BD). bigfile.py stores names
// this way, so reproducing it keeps our in-memory names byte-identical to the
// oracle's — including the 0xDD heap-fill in uninitialized FAT slots.
std::string decode_ascii_replace(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (unsigned char b : raw) {
        if (b < 0x80) out.push_back(static_cast<char>(b));
        else { out.push_back('\xEF'); out.push_back('\xBF'); out.push_back('\xBD'); }
    }
    return out;
}

// Mirror Python str.isprintable() over such a decoded field: 0x20..0x7e are
// printable, 0x7f (DEL) is not, and U+FFFD (bytes >= 0x80) counts as printable.
// At the UTF-8 byte level this reduces to: b >= 0x20 && b != 0x7f. Empty is not
// printable here because the caller also requires `name` non-empty.
bool field_is_printable(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char b : s) {
        if (b < 0x20 || b == 0x7f) return false;
    }
    return true;
}

// BF entry names are required to be ASCII on the write path.  This covers
// Python's repr spelling for that domain, including its quote-selection rule.
std::string python_ascii_repr(const std::string& value) {
    const bool use_double = value.find('\'') != std::string::npos &&
                            value.find('"') == std::string::npos;
    const char quote = use_double ? '"' : '\'';
    std::string out(1, quote);
    char escaped[5];
    for (unsigned char ch : value) {
        if (ch == static_cast<unsigned char>(quote) || ch == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(ch));
        } else if (ch == '\t') {
            out += "\\t";
        } else if (ch == '\n') {
            out += "\\n";
        } else if (ch == '\r') {
            out += "\\r";
        } else if (ch < 0x20 || ch == 0x7f) {
            std::snprintf(escaped, sizeof escaped, "\\x%02x", ch);
            out += escaped;
        } else {
            out.push_back(static_cast<char>(ch));
        }
    }
    out.push_back(quote);
    return out;
}

}  // namespace

size_t BigFile::detect_ext_size(std::ifstream& f, uint32_t pos_fat) const {
    for (size_t sz : {FILEEXT_OLD, FILEEXT_NEW}) {
        uint64_t off = static_cast<uint64_t>(pos_fat)
                     + static_cast<uint64_t>(size_of_fat) * FILE_ENTRY_SZ
                     + static_cast<uint64_t>(size_of_fat) * sz;
        std::vector<uint8_t> d = read_region(f, off, DIR_ENTRY_SZ);
        if (d.size() < DIR_ENTRY_SZ) continue;
        Reader r(d);
        uint32_t parent = r.u32(16);
        std::string name = decode_ascii_replace(r.str(20, MAX_LEN_NAME));
        if (parent == INVALID_INDEX && field_is_printable(name)) {
            return sz;
        }
    }
    return FILEEXT_OLD;
}

void BigFile::open(const std::string& p) {
    path = p;
    files.clear();
    dirs.clear();
    fat_list.clear();

    std::ifstream f(std::filesystem::u8path(p), std::ios::binary);
    if (!f) throw std::runtime_error("BigFile: cannot open " + p);

    std::vector<uint8_t> h = read_region(f, 0, HEADER_SIZE);
    if (h.size() < HEADER_SIZE)
        throw std::runtime_error("File too small for BigFile header");

    // Magic: 'BIG\0' normal, 'BUG\0' encrypted (unsupported).
    if (!(h[0] == 'B' && h[1] == 'I' && h[2] == 'G' && h[3] == 0)) {
        if (h[0] == 'B' && h[1] == 'U' && h[2] == 'G' && h[3] == 0)
            throw std::runtime_error("Encrypted BigFile (BUG header) not supported");
        throw std::runtime_error("Not a BigFile (bad magic)");
    }
    encrypted = false;

    Reader hr(h);
    version         = hr.u32(4);
    max_file        = hr.u32(8);
    max_dir         = hr.u32(12);
    max_key         = hr.u32(16);
    root            = hr.u32(20);
    first_free_file = hr.u32(24);
    first_free_dir  = hr.u32(28);
    size_of_fat     = hr.u32(32);
    num_fat         = hr.u32(36);
    universe_key    = hr.u32(40);

    // Read FAT descriptors.
    bool have_first_fat_pos = false;
    uint32_t first_fat_pos = 0;
    for (uint32_t i = 0; i < num_fat; ++i) {
        uint64_t pos = HEADER_SIZE + static_cast<uint64_t>(i) * FATDES_SIZE;
        if (have_first_fat_pos && pos + FATDES_SIZE > first_fat_pos) break;
        std::vector<uint8_t> fd = read_region(f, pos, FATDES_SIZE);
        if (fd.size() < FATDES_SIZE) break;
        Reader r(fd);
        FatDesc e;
        e.max_file     = r.u32(0);
        e.max_dir      = r.u32(4);
        e.pos_fat      = r.u32(8);
        e.next_pos_fat = r.u32(12);
        e.first_index  = r.u32(16);
        e.last_index   = r.u32(20);
        if (e.max_file > 10000000u || e.max_dir > 1000000u) break;
        if (!have_first_fat_pos) {
            first_fat_pos = e.pos_fat;
            have_first_fat_pos = true;
        }
        fat_list.push_back(e);
    }

    if (!fat_list.empty()) {
        ext_size = detect_ext_size(f, fat_list[0].pos_fat);
    }

    // Read each FAT.
    for (const FatDesc& fat : fat_list) {
        uint32_t fi  = fat.first_index;
        uint32_t mf  = fat.max_file;
        uint32_t md  = fat.max_dir;
        uint32_t pf  = fat.pos_fat;
        size_t   esz = ext_size;

        // FileTable (bulk read): (pos, key) pairs.
        {
            std::vector<uint8_t> ft = read_region(f, pf,
                static_cast<size_t>(mf) * FILE_ENTRY_SZ);
            Reader r(ft);
            for (uint32_t j = 0; j < mf; ++j) {
                size_t off = static_cast<size_t>(j) * FILE_ENTRY_SZ;
                if (off + FILE_ENTRY_SZ > ft.size()) break;
                BFFile e;
                e.index = fi + j;
                e.pos = r.u32(off);
                e.key = r.u32(off + 4);
                files[e.index] = std::move(e);
            }
        }

        // FileTableExt (bulk read): length, prev, next, parent, time, name.
        {
            uint64_t base = static_cast<uint64_t>(pf)
                          + static_cast<uint64_t>(size_of_fat) * FILE_ENTRY_SZ;
            std::vector<uint8_t> fe = read_region(f, base,
                static_cast<size_t>(mf) * esz);
            Reader r(fe);
            for (uint32_t j = 0; j < mf; ++j) {
                size_t off = static_cast<size_t>(j) * esz;
                if (off + 20 > fe.size()) break;
                uint32_t idx = fi + j;
                auto it = files.find(idx);
                if (it == files.end()) continue;
                BFFile& e = it->second;
                e.length = r.u32(off);
                e.prev   = r.u32(off + 4);
                e.next   = r.u32(off + 8);
                e.parent = r.u32(off + 12);
                e.time   = r.u32(off + 16);
                e.name   = decode_ascii_replace(r.str(off + 20, MAX_LEN_NAME));
            }
        }

        // DirTable (bulk read).
        {
            uint64_t base = static_cast<uint64_t>(pf)
                          + static_cast<uint64_t>(size_of_fat) * (FILE_ENTRY_SZ + esz);
            std::vector<uint8_t> dt = read_region(f, base,
                static_cast<size_t>(md) * DIR_ENTRY_SZ);
            Reader r(dt);
            for (uint32_t j = 0; j < md; ++j) {
                size_t off = static_cast<size_t>(j) * DIR_ENTRY_SZ;
                if (off + DIR_ENTRY_SZ > dt.size()) break;
                BFDir d;
                d.index        = fi + j;
                d.first_file   = r.u32(off);
                d.first_subdir = r.u32(off + 4);
                d.prev         = r.u32(off + 8);
                d.next         = r.u32(off + 12);
                d.parent       = r.u32(off + 16);
                d.name         = decode_ascii_replace(r.str(off + 20, MAX_LEN_NAME));
                dirs[d.index] = std::move(d);
            }
        }
    }
}

std::vector<uint8_t> BigFile::read_data(uint32_t idx) const {
    auto it = files.find(idx);
    if (it == files.end() || it->second.length == 0) return {};
    const BFFile& e = it->second;
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return {};
    return read_region(f, e.pos, e.length);
}

std::string BigFile::dir_path(uint32_t idx) const {
    std::vector<std::string> parts;
    std::set<uint32_t> visited;
    while (idx != INVALID_INDEX && dirs.count(idx) && !visited.count(idx)) {
        visited.insert(idx);
        const BFDir& d = dirs.at(idx);
        parts.push_back(d.name);
        idx = d.parent;
    }
    std::string out;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!out.empty()) out += '/';
        out += *it;
    }
    return out;
}

std::vector<uint32_t> BigFile::subdirs(uint32_t idx) const {
    std::vector<uint32_t> result;
    std::set<uint32_t> seen;
    auto it = dirs.find(idx);
    if (it == dirs.end()) return result;
    uint32_t s = it->second.first_subdir;
    while (s != INVALID_INDEX && dirs.count(s) && !seen.count(s)) {
        result.push_back(s);
        seen.insert(s);
        s = dirs.at(s).next;
    }
    return result;
}

std::vector<uint32_t> BigFile::dir_files(uint32_t idx) const {
    std::vector<uint32_t> result;
    std::set<uint32_t> seen;
    auto it = dirs.find(idx);
    if (it == dirs.end()) return result;
    uint32_t fi = it->second.first_file;
    while (fi != INVALID_INDEX && files.count(fi) && !seen.count(fi)) {
        result.push_back(fi);
        seen.insert(fi);
        fi = files.at(fi).next;
    }
    return result;
}

size_t BigFile::active_file_count() const {
    size_t n = 0;
    for (const auto& kv : files)
        if (!kv.second.name.empty() && kv.second.key != INVALID_KEY) ++n;
    return n;
}

size_t BigFile::active_dir_count() const {
    size_t n = 0;
    for (const auto& kv : dirs)
        if (!kv.second.name.empty()) ++n;
    return n;
}

// ───────────────────────── write path ─────────────────────────

namespace {

constexpr const char* kSizeGrsName = "size.grs";

void seek_write(std::fstream& f, uint64_t off, const void* data, size_t n) {
    f.seekp(static_cast<std::streamoff>(off), std::ios::beg);
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
}

void write_u32_at(std::fstream& f, uint64_t off, uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                    static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    seek_write(f, off, b, 4);
}

// Append `data` at a sector-aligned EOF, zero-padding the gap; return the
// aligned position written to (mirrors the append branch in bigfile.py).
uint64_t append_sector_aligned(std::fstream& f, const uint8_t* data, size_t n) {
    f.seekp(0, std::ios::end);
    uint64_t cur_end = static_cast<uint64_t>(f.tellp());
    uint64_t aligned = (cur_end + SECTOR_ALIGN - 1) &
                       ~(static_cast<uint64_t>(SECTOR_ALIGN) - 1);
    if (aligned > cur_end) {
        std::vector<uint8_t> pad(static_cast<size_t>(aligned - cur_end), 0);
        f.write(reinterpret_cast<const char*>(pad.data()),
                static_cast<std::streamsize>(pad.size()));
    }
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(n));
    return aligned;
}

}  // namespace

uint32_t BigFile::write_entry(std::fstream& f,
                              const std::vector<uint8_t>& compressed,
                              BFFile& fi, bool skip_size_grs) {
    const FatDesc& fat0 = fat_list.at(0);
    const uint64_t fext_base =
        static_cast<uint64_t>(fat0.pos_fat) +
        static_cast<uint64_t>(size_of_fat) * FILE_ENTRY_SZ;
    const uint32_t fext_first_idx = fat0.first_index;
    const size_t esz = ext_size;

    uint32_t new_pos;
    if (compressed.size() <= fi.length) {
        seek_write(f, fi.pos, compressed.data(), compressed.size());
        size_t remaining = fi.length - compressed.size();
        if (remaining > 0) {
            std::vector<uint8_t> z(remaining, 0);
            f.write(reinterpret_cast<const char*>(z.data()),
                    static_cast<std::streamsize>(z.size()));
        }
        new_pos = fi.pos;
    } else {
        new_pos = static_cast<uint32_t>(
            append_sector_aligned(f, compressed.data(), compressed.size()));
    }

    // FAT position.
    write_u32_at(f, static_cast<uint64_t>(fat0.pos_fat) +
                        static_cast<uint64_t>(fi.index - fext_first_idx) *
                            FILE_ENTRY_SZ,
                 new_pos);
    // FileExt length.
    write_u32_at(f, fext_base + static_cast<uint64_t>(fi.index - fext_first_idx) *
                                    esz,
                 static_cast<uint32_t>(compressed.size()));

    fi.pos = new_pos;
    fi.length = static_cast<uint32_t>(compressed.size());

    if (fi.name != kSizeGrsName && !skip_size_grs) {
        long long term = lzo_terminator_offset(compressed);
        if (term >= 0) queue_size_grs(fi.key, term);
    }
    return new_pos;
}

BFFile& BigFile::add_entry(std::fstream& f, const std::string& name,
                           uint32_t key, uint32_t parent_dir_idx,
                           const std::vector<uint8_t>& compressed,
                           const BigFileLogFn& log_fn) {
    for (unsigned char ch : name)
        if (ch >= 0x80)
            throw std::invalid_argument(
                "add_entry name must contain ASCII characters only");
    if (name.size() >= MAX_LEN_NAME)
        throw std::invalid_argument(
            "name " + python_ascii_repr(name) + " too long (max " +
            std::to_string(MAX_LEN_NAME - 1) + " ASCII chars)");
    for (const auto& kv : files)
        if (kv.second.key == key) {
            char message[64];
            std::snprintf(message, sizeof message,
                          "key 0x%08x already in archive", key);
            throw std::invalid_argument(message);
        }
    auto pit = dirs.find(parent_dir_idx);
    if (pit == dirs.end())
        throw std::invalid_argument(
            "parent dir index " + std::to_string(parent_dir_idx) +
            " not in archive");

    FatDesc& fat0 = fat_list.at(0);
    const uint64_t fext_base =
        static_cast<uint64_t>(fat0.pos_fat) +
        static_cast<uint64_t>(size_of_fat) * FILE_ENTRY_SZ;
    const uint32_t fext_first_idx = fat0.first_index;
    const size_t esz = ext_size;
    const uint32_t max_file_used = fat0.max_file;

    if (max_file_used >= size_of_fat)
        throw std::invalid_argument(
            "BigFile FAT is full: " + std::to_string(max_file_used) +
            " of " + std::to_string(size_of_fat) + " slots used");

    const uint32_t new_idx = fext_first_idx + max_file_used;

    // 1. Data at sector-aligned EOF.
    uint32_t new_pos = static_cast<uint32_t>(
        append_sector_aligned(f, compressed.data(), compressed.size()));

    // 2. FAT slot (pos, key).
    uint64_t fat_off = static_cast<uint64_t>(fat0.pos_fat) +
                       static_cast<uint64_t>(new_idx - fext_first_idx) *
                           FILE_ENTRY_SZ;
    write_u32_at(f, fat_off, new_pos);
    write_u32_at(f, fat_off + 4, key);

    // 3. Find the current tail of the parent directory's file chain.
    BFDir& parent = pit->second;
    uint32_t last_idx = INVALID_INDEX;
    if (parent.first_file != INVALID_INDEX) {
        uint32_t cur = parent.first_file;
        std::set<uint32_t> seen;
        while (cur != INVALID_INDEX && files.count(cur) && !seen.count(cur)) {
            seen.insert(cur);
            last_idx = cur;
            cur = files.at(cur).next;
        }
    }
    uint32_t prev_idx = last_idx;
    uint32_t next_idx = INVALID_INDEX;

    // 4. FileExt slot: length, prev, next, parent, time(0), name (null-padded).
    uint64_t ext_off = fext_base +
                       static_cast<uint64_t>(new_idx - fext_first_idx) * esz;
    {
        uint8_t hdr[20];
        auto put = [&](int o, uint32_t v) {
            hdr[o] = static_cast<uint8_t>(v);
            hdr[o + 1] = static_cast<uint8_t>(v >> 8);
            hdr[o + 2] = static_cast<uint8_t>(v >> 16);
            hdr[o + 3] = static_cast<uint8_t>(v >> 24);
        };
        put(0, static_cast<uint32_t>(compressed.size()));
        put(4, prev_idx);
        put(8, next_idx);
        put(12, parent_dir_idx);
        put(16, 0);
        std::vector<uint8_t> namebuf(MAX_LEN_NAME, 0);
        std::memcpy(namebuf.data(), name.data(), name.size());
        seek_write(f, ext_off, hdr, 20);
        f.write(reinterpret_cast<const char*>(namebuf.data()),
                static_cast<std::streamsize>(namebuf.size()));
    }

    // 5. Patch the previous tail's `next`, or the parent's first_file.
    if (prev_idx != INVALID_INDEX) {
        write_u32_at(f, fext_base +
                            static_cast<uint64_t>(prev_idx - fext_first_idx) *
                                esz + 8,
                     new_idx);
        files.at(prev_idx).next = new_idx;
    } else {
        uint64_t dt_base = static_cast<uint64_t>(fat0.pos_fat) +
                           static_cast<uint64_t>(size_of_fat) *
                               (FILE_ENTRY_SZ + esz);
        uint64_t dt_off = dt_base +
                          static_cast<uint64_t>(parent_dir_idx - fext_first_idx) *
                              DIR_ENTRY_SZ;
        write_u32_at(f, dt_off, new_idx);
        parent.first_file = new_idx;
    }

    // 6. Bump FATDes[0].MaxFile (disk + cache).
    write_u32_at(f, HEADER_SIZE + 0 * FATDES_SIZE, max_file_used + 1);
    fat0.max_file = max_file_used + 1;

    // 7. Bump header.max_file.
    write_u32_at(f, 8, max_file + 1);
    max_file += 1;

    // 8. In-memory file index.
    BFFile nf;
    nf.index = new_idx;
    nf.pos = new_pos;
    nf.key = key;
    nf.length = static_cast<uint32_t>(compressed.size());
    nf.prev = prev_idx;
    nf.next = next_idx;
    nf.parent = parent_dir_idx;
    nf.time = 0;
    nf.name = name;
    files[new_idx] = std::move(nf);

    // 9. Queue a size.grs row, same as write_entry.
    if (name != kSizeGrsName) {
        long long term = lzo_terminator_offset(compressed);
        if (term >= 0) queue_size_grs(key, term);
    }
    if (log_fn) {
        char line[512];
        const std::string repr = python_ascii_repr(name);
        std::snprintf(line, sizeof line,
                      "  new entry: name=%s key=0x%08x idx=%u pos=0x%x len=%zu",
                      repr.c_str(), key, new_idx, new_pos, compressed.size());
        log_fn(line);
    }
    return files[new_idx];
}

int BigFile::flush_size_grs(std::fstream& f, const BigFileLogFn& log_fn) {
    if (pending_size_grs_.empty()) return 0;

    BFFile* size_fi = nullptr;
    for (auto& kv : files)
        if (kv.second.name == kSizeGrsName) { size_fi = &kv.second; break; }
    if (!size_fi) {
        if (log_fn) {
            log_fn("  size.grs not in BF \xE2\x80\x94 cannot update declared stream "
                   "lengths (" + std::to_string(pending_size_grs_.size()) +
                   " pending)");
        }
        pending_size_grs_.clear();
        return 0;
    }

    std::vector<uint8_t> raw(size_fi->length);
    f.clear();
    f.seekg(static_cast<std::streamoff>(size_fi->pos), std::ios::beg);
    f.read(reinterpret_cast<char*>(raw.data()),
           static_cast<std::streamsize>(raw.size()));
    raw.resize(static_cast<size_t>(f.gcount()));

    auto rd32 = [&](size_t o) -> uint32_t {
        return static_cast<uint32_t>(raw[o]) |
               (static_cast<uint32_t>(raw[o + 1]) << 8) |
               (static_cast<uint32_t>(raw[o + 2]) << 16) |
               (static_cast<uint32_t>(raw[o + 3]) << 24);
    };
    auto wr32 = [&](size_t o, uint32_t v) {
        raw[o] = static_cast<uint8_t>(v);
        raw[o + 1] = static_cast<uint8_t>(v >> 8);
        raw[o + 2] = static_cast<uint8_t>(v >> 16);
        raw[o + 3] = static_cast<uint8_t>(v >> 24);
    };

    int updates = 0;
    // Insertion-ordered, like the Python dict (row-append order is queue order).
    std::vector<std::pair<uint32_t, long long>> remaining = pending_size_grs_;
    auto remaining_erase = [&](uint32_t key) {
        for (auto it = remaining.begin(); it != remaining.end(); ++it)
            if (it->first == key) { remaining.erase(it); return; }
    };
    std::vector<size_t> zero_slots;

    // Layout: u32 total_size, then (key:u32, declared_len:u32) pairs.
    for (size_t i = 4; i + 8 <= raw.size(); i += 8) {
        uint32_t k = rd32(i);
        uint32_t old = rd32(i + 4);
        bool matched = false;
        for (const auto& kv : remaining)
            if (kv.first == k) {
                uint32_t nw = static_cast<uint32_t>(kv.second);
                if (nw != old) {
                    wr32(i + 4, nw);
                    ++updates;
                    if (log_fn) {
                        char line[128];
                        std::snprintf(line, sizeof line,
                                      "  size.grs[0x%08x]: %u -> %u", k, old, nw);
                        log_fn(line);
                    }
                }
                matched = true;
                break;
            }
        if (matched) {
            remaining_erase(k);
        } else if (k == 0 && old == 0) {
            zero_slots.push_back(i);
        }
    }

    // Fill trailing (0,0) slack with rows for brand-new keys (queue order).
    {
        size_t zi = 0;
        while (!remaining.empty() && zi < zero_slots.size()) {
            size_t off = zero_slots[zi++];
            wr32(off, remaining.front().first);
            wr32(off + 4, static_cast<uint32_t>(remaining.front().second));
            if (log_fn) {
                char line[128];
                std::snprintf(line, sizeof line,
                              "  size.grs[+0x%08x]: %lld (new entry)",
                              remaining.front().first, remaining.front().second);
                log_fn(line);
            }
            ++updates;
            remaining.erase(remaining.begin());
        }
    }

    // GROW: more new keys than slack — append rows + bump the total-size field.
    if (!remaining.empty()) {
        const size_t grow_rows = remaining.size();
        for (const auto& kv : remaining) {
            uint8_t row[8];
            uint32_t k = kv.first, t = static_cast<uint32_t>(kv.second);
            row[0] = static_cast<uint8_t>(k); row[1] = static_cast<uint8_t>(k >> 8);
            row[2] = static_cast<uint8_t>(k >> 16); row[3] = static_cast<uint8_t>(k >> 24);
            row[4] = static_cast<uint8_t>(t); row[5] = static_cast<uint8_t>(t >> 8);
            row[6] = static_cast<uint8_t>(t >> 16); row[7] = static_cast<uint8_t>(t >> 24);
            raw.insert(raw.end(), row, row + 8);
            ++updates;
            if (log_fn) {
                char line[128];
                std::snprintf(line, sizeof line,
                              "  size.grs[+0x%08x]: %lld (new entry, grown)",
                              kv.first, kv.second);
                log_fn(line);
            }
        }
        wr32(0, static_cast<uint32_t>(raw.size()));  // total byte-length field
        if (log_fn) {
            log_fn("  size.grs grown: +" + std::to_string(grow_rows) +
                   " row(s) -> " + std::to_string(raw.size()) + " bytes");
        }
    }

    if (updates > 0) {
        if (raw.size() <= size_fi->length) {
            seek_write(f, size_fi->pos, raw.data(), raw.size());
        } else {
            const FatDesc& fat0 = fat_list.at(0);
            uint32_t fext_first_idx = fat0.first_index;
            uint64_t fext_base = static_cast<uint64_t>(fat0.pos_fat) +
                                 static_cast<uint64_t>(size_of_fat) * FILE_ENTRY_SZ;
            size_t esz = ext_size;
            uint32_t aligned = static_cast<uint32_t>(
                append_sector_aligned(f, raw.data(), raw.size()));
            write_u32_at(f, static_cast<uint64_t>(fat0.pos_fat) +
                                static_cast<uint64_t>(size_fi->index -
                                                      fext_first_idx) *
                                    FILE_ENTRY_SZ,
                         aligned);
            write_u32_at(f, fext_base + static_cast<uint64_t>(size_fi->index -
                                                              fext_first_idx) *
                                            esz,
                         static_cast<uint32_t>(raw.size()));
            size_fi->pos = aligned;
            size_fi->length = static_cast<uint32_t>(raw.size());
            if (log_fn) {
                char line[128];
                std::snprintf(line, sizeof line,
                              "  size.grs relocated to 0x%x (%zu bytes)",
                              aligned, raw.size());
                log_fn(line);
            }
        }
    }

    pending_size_grs_.clear();
    return updates;
}

}  // namespace jade
