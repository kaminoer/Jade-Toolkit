// Sha256.hpp — header-only SHA-256 (ported from build/cpp/patcher.cpp).
//
// Used by the .jtmod engine's precheck to verify a target archive matches the
// exact pristine base a mod was authored against. Streams files in chunks so a
// ~500 MB BigFile is hashed without loading it whole.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace jade {
namespace sha_detail {

struct Ctx { uint32_t s[8]; uint64_t len; uint8_t buf[64]; size_t n; };

inline uint32_t ror(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

inline void init(Ctx& c) {
    static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                   0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::memcpy(c.s, iv, sizeof iv); c.len = 0; c.n = 0;
}

inline void block(Ctx& c, const uint8_t* p) {
    static const uint32_t k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|p[i*4+3];
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c.s[0],b=c.s[1],cc=c.s[2],d=c.s[3],e=c.s[4],f=c.s[5],g=c.s[6],h=c.s[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = ror(e,6)^ror(e,11)^ror(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        uint32_t t1 = h + S1 + ch + k[i] + w[i];
        uint32_t S0 = ror(a,2)^ror(a,13)^ror(a,22);
        uint32_t maj = (a&b)^(a&cc)^(b&cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c.s[0]+=a; c.s[1]+=b; c.s[2]+=cc; c.s[3]+=d; c.s[4]+=e; c.s[5]+=f; c.s[6]+=g; c.s[7]+=h;
}

inline void update(Ctx& c, const uint8_t* p, size_t len) {
    c.len += len;
    while (len) {
        size_t take = 64 - c.n; if (take > len) take = len;
        std::memcpy(c.buf + c.n, p, take); c.n += take; p += take; len -= take;
        if (c.n == 64) { block(c, c.buf); c.n = 0; }
    }
}

inline void finish(Ctx& c, uint8_t out[32]) {
    uint64_t bits = c.len * 8;
    uint8_t pad = 0x80; update(c, &pad, 1);
    uint8_t z = 0; while (c.n != 56) update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; ++i) lb[i] = uint8_t(bits >> (56 - 8*i));
    update(c, lb, 8);
    for (int i = 0; i < 8; ++i) {
        out[i*4]   = uint8_t(c.s[i] >> 24);
        out[i*4+1] = uint8_t(c.s[i] >> 16);
        out[i*4+2] = uint8_t(c.s[i] >> 8);
        out[i*4+3] = uint8_t(c.s[i]);
    }
}

}  // namespace sha_detail

inline std::string hex32(const uint8_t b[32]) {
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(64);
    for (int i = 0; i < 32; ++i) { s.push_back(H[b[i]>>4]); s.push_back(H[b[i]&0xf]); }
    return s;
}

inline std::string sha256_hex(const uint8_t* data, size_t len) {
    sha_detail::Ctx c; sha_detail::init(c);
    sha_detail::update(c, data, len);
    uint8_t out[32]; sha_detail::finish(c, out);
    return hex32(out);
}

inline std::string sha256_hex(const std::vector<uint8_t>& v) {
    return sha256_hex(v.data(), v.size());
}

// Streaming file hash; returns "" if the file can't be opened.
inline std::string sha256_file_hex(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return "";
    sha_detail::Ctx c; sha_detail::init(c);
    std::vector<uint8_t> buf(1u << 20);
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), std::streamsize(buf.size()));
        std::streamsize got = f.gcount();
        if (got > 0) sha_detail::update(c, buf.data(), size_t(got));
    }
    uint8_t out[32]; sha_detail::finish(c, out);
    return hex32(out);
}

}  // namespace jade
