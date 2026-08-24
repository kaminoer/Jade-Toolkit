// Reader.hpp — endian-aware byte reader.
//
// The C++ equivalent of Python's struct.unpack_from. Jade BigFiles on PC are
// little-endian; the GameCube builds (JadeGC) are big-endian. Endianness is a
// first-class parameter here so the GC path can reuse the same parsers later
// rather than hardcoding little-endian the way a naive port would.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>
#include <stdexcept>

namespace jade {

enum class Endian { Little, Big };

// Read primitive integers out of a raw byte span at an explicit offset,
// mirroring struct.unpack_from('<I', data, off). Bounds-checked: an out-of-range
// read throws rather than reading garbage (the Python guards with len() checks).
class Reader {
public:
    Reader(const uint8_t* data, size_t size, Endian endian = Endian::Little)
        : data_(data), size_(size), endian_(endian) {}

    explicit Reader(const std::vector<uint8_t>& buf, Endian endian = Endian::Little)
        : data_(buf.data()), size_(buf.size()), endian_(endian) {}

    size_t size() const { return size_; }
    Endian endian() const { return endian_; }

    bool can_read(size_t off, size_t n) const { return off + n <= size_; }

    uint8_t  u8 (size_t off) const { check(off, 1); return data_[off]; }
    uint16_t u16(size_t off) const { return read_int<uint16_t>(off); }
    uint32_t u32(size_t off) const { return read_int<uint32_t>(off); }
    uint64_t u64(size_t off) const { return read_int<uint64_t>(off); }
    int32_t  i32(size_t off) const { return static_cast<int32_t>(u32(off)); }
    float    f32(size_t off) const {
        uint32_t v = u32(off);
        float f;
        std::memcpy(&f, &v, sizeof(f));
        return f;
    }

    // Port of bigfile.py BigFile._read_str: decode a fixed-width field as the
    // bytes up to the first NUL (or maxlen, whichever comes first). Returns the
    // raw bytes; callers canonicalize for display / comparison.
    std::string str(size_t off, size_t maxlen) const {
        size_t end = off;
        size_t limit = off + maxlen;
        if (limit > size_) limit = size_;
        while (end < limit && data_[end] != 0) ++end;
        return std::string(reinterpret_cast<const char*>(data_ + off), end - off);
    }

private:
    void check(size_t off, size_t n) const {
        if (off + n > size_)
            throw std::out_of_range("Reader: read past end of buffer");
    }

    template <typename T>
    T read_int(size_t off) const {
        check(off, sizeof(T));
        T v = 0;
        if (endian_ == Endian::Little) {
            for (size_t i = 0; i < sizeof(T); ++i)
                v |= static_cast<T>(data_[off + i]) << (8 * i);
        } else {
            for (size_t i = 0; i < sizeof(T); ++i)
                v = (v << 8) | static_cast<T>(data_[off + i]);
        }
        return v;
    }

    const uint8_t* data_;
    size_t size_;
    Endian endian_;
};

}  // namespace jade
