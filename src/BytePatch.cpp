// BytePatch.cpp - JADEBPT3 codec/diff/runtime.
#include "jade/BytePatch.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

#include "jade/GameProfiles.hpp"
#include "jade/Image.hpp"

namespace jade {
namespace bytepatch {
namespace {

const uint8_t MAGIC[8] = {'J', 'A', 'D', 'E', 'B', 'P', 'T', '3'};
const uint8_t FOOTER_MAGIC[8] = {'J', 'D', 'P', 'A', 'T', 'F', 'T', 'R'};

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v)); out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 24));
}
void put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
uint32_t get32(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 4 > data.size()) throw std::runtime_error("truncated patch payload");
    uint32_t v = uint32_t(data[off]) | (uint32_t(data[off + 1]) << 8) |
                 (uint32_t(data[off + 2]) << 16) |
                 (uint32_t(data[off + 3]) << 24);
    off += 4;
    return v;
}
uint64_t get64(const std::vector<uint8_t>& data, size_t& off) {
    if (off + 8 > data.size()) throw std::runtime_error("truncated patch payload");
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(data[off + i]) << (8 * i);
    off += 8;
    return v;
}
void put_string(std::vector<uint8_t>& out, const std::string& text) {
    put32(out, uint32_t(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}
std::string get_string(const std::vector<uint8_t>& data, size_t& off) {
    uint32_t n = get32(data, off);
    if (off + n > data.size()) throw std::runtime_error("truncated patch string");
    std::string value(reinterpret_cast<const char*>(data.data() + off), n);
    off += n;
    return value;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::vector<uint8_t> unhex(const std::string& value) {
    if (value.size() % 2) throw std::runtime_error("non-even hex digest");
    std::vector<uint8_t> out;
    out.reserve(value.size() / 2);
    for (size_t i = 0; i < value.size(); i += 2) {
        int hi = hex_digit(value[i]), lo = hex_digit(value[i + 1]);
        if (hi < 0 || lo < 0) throw std::runtime_error("invalid hex digest");
        out.push_back(uint8_t((hi << 4) | lo));
    }
    return out;
}
std::string hex(const uint8_t* data, size_t size) {
    static const char* digits = "0123456789abcdef";
    std::string out(size * 2, '0');
    for (size_t i = 0; i < size; ++i) {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }
    return out;
}

// Compact SHA-256, shared semantically with project::sha256_of_file but kept
// here so patcher_runtime's per-1MiB progress callback remains exact.
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buf[64]{};
    uint64_t len = 0;
    size_t fill = 0;
    static uint32_t rotr(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,
            0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,
            0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,
            0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
            0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,
            0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
            0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,
            0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,
            0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,
            0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,
            0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
            0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(p[i*4]) << 24 | uint32_t(p[i*4+1]) << 16 |
                   uint32_t(p[i*4+2]) << 8 | uint32_t(p[i*4+3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15]>>3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2]>>10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],z=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t s1=rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch=(e&f)^(~e&g);
            uint32_t t1=z+s1+ch+K[i]+w[i];
            uint32_t s0=rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj=(a&b)^(a&c)^(b&c);
            uint32_t t2=s0+maj;
            z=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;
        h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=z;
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
    std::string digest() {
        uint64_t bits = len * 8;
        uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (fill != 56) update(&zero, 1);
        uint8_t tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = uint8_t(bits >> (56 - i*8));
        update(tail, 8);
        uint8_t raw[32];
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j)
                raw[i*4+j] = uint8_t(h[i] >> (24 - j*8));
        return hex(raw, 32);
    }
};

uint64_t file_size(const std::string& path) {
    std::error_code ec;
    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) throw std::runtime_error("could not stat " + path);
    return size;
}

void chunked_copy(const std::string& src, const std::string& dst,
                  const ProgressFn& progress) {
    const uint64_t total = file_size(src);
    std::ifstream in(src, std::ios::binary);
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!in || !out) throw std::runtime_error("could not copy patch target");
    std::vector<uint8_t> buf(IO_CHUNK);
    uint64_t done = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(buf.data()),
                std::streamsize(buf.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;
        out.write(reinterpret_cast<const char*>(buf.data()), got);
        done += uint64_t(got);
        if (progress) progress(done, total);
    }
}

void apply_segments(const std::string& path,
                    const std::vector<Segment>& segments,
                    const std::vector<uint8_t>& body,
                    uint64_t target_size) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!f) throw std::runtime_error("could not open patch target");
    size_t body_pos = 0;
    for (const Segment& segment : segments) {
        f.seekp(std::streamoff(segment.offset), std::ios::beg);
        size_t available = body_pos < body.size() ? body.size() - body_pos : 0;
        size_t take = std::min<size_t>(segment.length, available);
        if (take)
            f.write(reinterpret_cast<const char*>(body.data() + body_pos),
                    std::streamsize(take));
        body_pos += segment.length;
    }
    f.close();
    std::filesystem::resize_file(path, target_size);
}

uint32_t round_half_even(double value) {
    double floor_v = std::floor(value);
    double frac = value - floor_v;
    if (frac < 0.5) return uint32_t(floor_v);
    if (frac > 0.5) return uint32_t(floor_v + 1.0);
    uint64_t base = uint64_t(floor_v);
    return uint32_t((base & 1u) ? base + 1 : base);
}

std::vector<uint8_t> bmp24(const uint8_t* rgba, uint32_t w, uint32_t h) {
    const uint32_t stride = (w * 3 + 3) & ~3u;
    const uint32_t image_size = stride * h;
    std::vector<uint8_t> out(54 + image_size, 0);
    out[0] = 'B'; out[1] = 'M';
    auto at32 = [&](size_t o, uint32_t v) {
        out[o] = uint8_t(v); out[o+1] = uint8_t(v>>8);
        out[o+2] = uint8_t(v>>16); out[o+3] = uint8_t(v>>24);
    };
    auto at16 = [&](size_t o, uint16_t v) {
        out[o] = uint8_t(v); out[o+1] = uint8_t(v>>8);
    };
    at32(2, uint32_t(out.size())); at32(10, 54); at32(14, 40);
    at32(18, w); at32(22, h); at16(26, 1); at16(28, 24);
    at32(34, image_size); at32(38, 3780); at32(42, 3780);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* dst = out.data() + 54 + size_t(h - 1 - y) * stride;
        const uint8_t* src = rgba + size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            dst[x*3] = src[x*4+2];
            dst[x*3+1] = src[x*4+1];
            dst[x*3+2] = src[x*4];
        }
    }
    return out;
}

std::string utc_now() {
    std::time_t now = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return b;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return char(std::tolower(c)); });
    return value;
}

}  // namespace

std::vector<uint8_t> pack_patch(const Patch& patch) {
    std::vector<uint8_t> out(MAGIC, MAGIC + 8);
    put32(out, FORMAT_VERSION);
    put32(out, 0);
    put64(out, patch.base_size);
    std::vector<uint8_t> base_sha = unhex(patch.base_sha256);
    out.insert(out.end(), base_sha.begin(), base_sha.end());
    put64(out, patch.result_size);
    std::vector<uint8_t> result_sha = unhex(patch.result_sha256);
    out.insert(out.end(), result_sha.begin(), result_sha.end());
    for (const std::string* value :
         {&patch.title, &patch.description, &patch.author,
          &patch.version_text, &patch.created, &patch.tool,
          &patch.default_target})
        put_string(out, *value);
    put32(out, uint32_t(patch.accepted_names.size()));
    for (const std::string& name : patch.accepted_names) put_string(out, name);
    auto direction = [&](const std::vector<Segment>& segs,
                         const std::vector<uint8_t>& body) {
        put32(out, uint32_t(segs.size()));
        for (const Segment& segment : segs) {
            put64(out, segment.offset);
            put32(out, segment.length);
        }
        put64(out, body.size());
        out.insert(out.end(), body.begin(), body.end());
    };
    direction(patch.fwd_segs, patch.fwd_body);
    direction(patch.rev_segs, patch.rev_body);
    put32(out, patch.image_w);
    put32(out, patch.image_h);
    put32(out, uint32_t(patch.image.size()));
    out.insert(out.end(), patch.image.begin(), patch.image.end());
    return out;
}

Patch unpack_patch(const std::vector<uint8_t>& data) {
    if (data.size() < 8 || std::memcmp(data.data(), MAGIC, 8) != 0)
        throw std::runtime_error("not a jade byte-patch (bad magic)");
    size_t off = 8;
    uint32_t version = get32(data, off);
    (void)get32(data, off);  // flags
    if (version != FORMAT_VERSION)
        throw std::runtime_error("unsupported patch version " +
                                 std::to_string(version));
    Patch patch;
    patch.base_size = get64(data, off);
    if (off + 32 > data.size()) throw std::runtime_error("truncated patch payload");
    patch.base_sha256 = hex(data.data() + off, 32); off += 32;
    patch.result_size = get64(data, off);
    if (off + 32 > data.size()) throw std::runtime_error("truncated patch payload");
    patch.result_sha256 = hex(data.data() + off, 32); off += 32;
    std::string* strings[] = {
        &patch.title, &patch.description, &patch.author, &patch.version_text,
        &patch.created, &patch.tool, &patch.default_target};
    for (std::string* value : strings) *value = get_string(data, off);
    uint32_t accepted = get32(data, off);
    for (uint32_t i = 0; i < accepted; ++i)
        patch.accepted_names.push_back(get_string(data, off));
    auto direction = [&](std::vector<Segment>& segs,
                         std::vector<uint8_t>& body) {
        uint32_t count = get32(data, off);
        segs.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Segment segment;
            segment.offset = get64(data, off);
            segment.length = get32(data, off);
            segs.push_back(segment);
        }
        uint64_t body_size = get64(data, off);
        if (body_size > std::numeric_limits<size_t>::max() ||
            off + size_t(body_size) > data.size())
            throw std::runtime_error("truncated patch body");
        body.assign(data.begin() + long(off),
                    data.begin() + long(off + size_t(body_size)));
        off += size_t(body_size);
    };
    direction(patch.fwd_segs, patch.fwd_body);
    direction(patch.rev_segs, patch.rev_body);
    patch.image_w = get32(data, off);
    patch.image_h = get32(data, off);
    uint32_t image_size = get32(data, off);
    if (off + image_size > data.size())
        throw std::runtime_error("truncated patch image");
    patch.image.assign(data.begin() + long(off),
                       data.begin() + long(off + image_size));
    return patch;
}

std::string sha256_file(const std::string& path, ProgressFn progress) {
    uint64_t total = file_size(path), done = 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("could not open " + path);
    Sha256 sha;
    std::vector<uint8_t> buf(IO_CHUNK);
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()),
               std::streamsize(buf.size()));
        std::streamsize got = f.gcount();
        if (got <= 0) break;
        sha.update(buf.data(), size_t(got));
        done += uint64_t(got);
        if (progress) progress(done, total);
    }
    return sha.digest();
}

DiffResult compute_segments(const std::string& base_path,
                            const std::string& result_path,
                            size_t merge_gap, ProgressFn progress) {
    DiffResult result;
    uint64_t base_size = file_size(base_path);
    result.result_size = file_size(result_path);
    uint64_t overlap = std::min(base_size, result.result_size);
    std::ifstream base(base_path, std::ios::binary);
    std::ifstream changed(result_path, std::ios::binary);
    if (!base || !changed) throw std::runtime_error("could not open diff input");
    std::vector<uint8_t> a(IO_CHUNK), b(IO_CHUNK);
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    bool active = false;
    uint64_t seg_start = 0, seg_last = 0, pos = 0;
    while (pos < overlap) {
        size_t n = size_t(std::min<uint64_t>(IO_CHUNK, overlap - pos));
        base.read(reinterpret_cast<char*>(a.data()), std::streamsize(n));
        changed.read(reinterpret_cast<char*>(b.data()), std::streamsize(n));
        for (size_t i = 0; i < n; ++i) {
            if (a[i] == b[i]) continue;
            uint64_t at = pos + i;
            if (!active) {
                active = true;
                seg_start = seg_last = at;
            } else if (at - seg_last <= merge_gap) {
                seg_last = at;
            } else {
                ranges.push_back({seg_start, seg_last + 1});
                seg_start = seg_last = at;
            }
        }
        pos += n;
        if (progress) progress(pos, overlap);
    }
    if (active) ranges.push_back({seg_start, seg_last + 1});
    if (result.result_size > base_size)
        ranges.push_back({base_size, result.result_size});

    changed.clear();
    for (const auto& range : ranges) {
        DataSegment segment;
        segment.offset = range.first;
        size_t length = size_t(range.second - range.first);
        segment.data.resize(length);
        changed.seekg(std::streamoff(range.first), std::ios::beg);
        changed.read(reinterpret_cast<char*>(segment.data.data()),
                     std::streamsize(length));
        segment.data.resize(size_t(changed.gcount()));
        result.changed_bytes += segment.data.size();
        result.segments.push_back(std::move(segment));
    }
    return result;
}

PayloadBuild build_patch_payload(const std::string& base_path,
                                 const std::string& result_path,
                                 const PayloadOptions& options,
                                 ProgressFn progress) {
    DiffResult fwd = compute_segments(base_path, result_path, MERGE_GAP,
                                      progress);
    DiffResult rev = compute_segments(result_path, base_path);
    Patch patch;
    patch.base_size = file_size(base_path);
    patch.base_sha256 = sha256_file(base_path);
    patch.result_size = fwd.result_size;
    patch.result_sha256 = sha256_file(result_path);
    patch.title = options.title.empty() ? options.archive_name : options.title;
    patch.description = options.description;
    patch.author = options.author;
    patch.version_text = options.version;
    patch.created = options.created.empty() ? utc_now() : options.created;
    patch.tool = "jade_explorer";
    patch.default_target = options.archive_name;
    patch.accepted_names = options.accepted_names.empty()
                               ? (options.archive_name.empty()
                                      ? std::vector<std::string>{}
                                      : std::vector<std::string>{options.archive_name})
                               : options.accepted_names;
    for (const DataSegment& segment : fwd.segments) {
        patch.fwd_segs.push_back(
            {segment.offset, uint32_t(segment.data.size())});
        patch.fwd_body.insert(patch.fwd_body.end(), segment.data.begin(),
                              segment.data.end());
    }
    for (const DataSegment& segment : rev.segments) {
        patch.rev_segs.push_back(
            {segment.offset, uint32_t(segment.data.size())});
        patch.rev_body.insert(patch.rev_body.end(), segment.data.begin(),
                              segment.data.end());
    }
    if (!options.image_path.empty()) {
        RgbaImage image = load_rgba_image(options.image_path);
        if (!image.ok)
            throw std::runtime_error("could not read splash image: " +
                                     image.error);
        double scale = std::min(
            {520.0 / image.width, 260.0 / image.height, 1.0});
        uint32_t nw = std::max(1u, round_half_even(image.width * scale));
        uint32_t nh = std::max(1u, round_half_even(image.height * scale));
        if (nw != image.width || nh != image.height) {
            image.rgba = resize_rgba_lanczos(image.rgba.data(), image.width,
                                             image.height, nw, nh);
            image.width = nw; image.height = nh;
        }
        patch.image_w = image.width;
        patch.image_h = image.height;
        patch.image = bmp24(image.rgba.data(), image.width, image.height);
    }
    PayloadBuild built;
    built.payload = pack_patch(patch);
    built.stats.fwd_segments = patch.fwd_segs.size();
    built.stats.rev_segments = patch.rev_segs.size();
    built.stats.changed_bytes = fwd.changed_bytes;
    built.stats.payload_size = built.payload.size();
    built.stats.base_size = patch.base_size;
    built.stats.result_size = patch.result_size;
    built.stats.base_sha256 = patch.base_sha256;
    built.stats.result_sha256 = patch.result_sha256;
    return built;
}

void apply_forward_inplace(const Patch& patch, const std::string& target) {
    apply_segments(target, patch.fwd_segs, patch.fwd_body, patch.result_size);
}

void apply_reverse_inplace(const Patch& patch, const std::string& target) {
    apply_segments(target, patch.rev_segs, patch.rev_body, patch.base_size);
}

Patch apply_patch_to_file(const std::vector<uint8_t>& payload,
                          const std::string& base_path,
                          const std::string& out_path,
                          ProgressFn progress) {
    Patch patch = unpack_patch(payload);
    chunked_copy(base_path, out_path, progress);
    apply_forward_inplace(patch, out_path);
    return patch;
}

std::string classify_target(const Patch& patch, const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return "missing";
    uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) return "missing";
    if (size == patch.result_size && sha256_file(path) == patch.result_sha256)
        return "patched";
    if (size != patch.base_size) return "size-mismatch";
    if (sha256_file(path) == patch.base_sha256) return "base";
    return "unknown";
}

std::vector<uint8_t> read_embedded_payload(
    const std::string& executable_path) {
    std::ifstream f(executable_path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    uint64_t size = uint64_t(f.tellg());
    if (size < 16) return {};
    f.seekg(std::streamoff(size - 16), std::ios::beg);
    uint8_t footer[16];
    f.read(reinterpret_cast<char*>(footer), 16);
    if (f.gcount() != 16 || std::memcmp(footer, FOOTER_MAGIC, 8) != 0)
        return {};
    uint64_t length = 0;
    for (int i = 0; i < 8; ++i) length |= uint64_t(footer[8 + i]) << (8*i);
    if (length == 0 || length > size - 16) return {};
    std::vector<uint8_t> payload(static_cast<size_t>(length));
    f.seekg(std::streamoff(size - 16 - length), std::ios::beg);
    f.read(reinterpret_cast<char*>(payload.data()), std::streamsize(length));
    if (uint64_t(f.gcount()) != length) return {};
    return payload;
}

void append_payload(const std::string& stub_path,
                    const std::vector<uint8_t>& payload,
                    const std::string& output_path) {
    chunked_copy(stub_path, output_path, {});
    std::ofstream f(output_path, std::ios::binary | std::ios::app);
    if (!f) throw std::runtime_error("could not append patch payload");
    f.write(reinterpret_cast<const char*>(payload.data()),
            std::streamsize(payload.size()));
    f.write(reinterpret_cast<const char*>(FOOTER_MAGIC), 8);
    uint64_t length = payload.size();
    uint8_t raw[8];
    for (int i = 0; i < 8; ++i) raw[i] = uint8_t(length >> (8*i));
    f.write(reinterpret_cast<const char*>(raw), 8);
}

std::vector<std::string> accepted_target_names(
    const std::string& game, const std::string& loaded_archive_name) {
    std::vector<std::string> names;
    if (!loaded_archive_name.empty()) names.push_back(loaded_archive_name);
    if (const gameprofiles::GameProfile* profile = gameprofiles::get(game))
        names.insert(names.end(), profile->archive_filenames.begin(),
                     profile->archive_filenames.end());
    if (game == "SoT" || game == "WW" || game == "WW_PS2")
        names.push_back("prince.bf");
    else if (game == "T2T")
        names.push_back("pop3.bf");
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& name : names) {
        std::string lowered = lower(name);
        if (!name.empty() && !seen.count(lowered)) {
            seen.insert(lowered);
            out.push_back(name);
        }
    }
    return out;
}

}  // namespace bytepatch
}  // namespace jade
