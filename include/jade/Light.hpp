// Light.hpp — Jade GRO_Light (gro_type == 2) parser (read path).
//
// Port of jade_explorer/core/light.py. Layout (empirically verified against
// SoT/WW/T2T; the v36 source has diverged):
//   u32 version (9=SoT, 10=WW/T2T), u32 flags (type = flags & 7),
//   u32 diffuse (0x00BBGGRR), [u32 specular — v10 only],
//   f32 near, f32 far, f32 inner_angle, f32 outer_angle, f32 intensity.
// The version-invariant prefix (version..outer) sits at fixed offsets; intensity
// is only meaningful when the payload is exactly base size (32 for v9, 36 v10).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jade {

constexpr uint32_t GRO_LIGHT = 2;

// A field that may be absent (offset out of range, or specular on v9). `bits`
// holds the raw little-endian u32 at the field offset when present.
struct LightField {
    bool     present = false;
    uint32_t bits    = 0;
};

struct LightInfo {
    bool        ok = false;
    uint32_t    version = 0;
    uint32_t    flags   = 0;
    uint32_t    type    = 0;       // flags & 7
    std::string type_name;
    LightField  diffuse, specular, near_, far_, inner, outer, intensity;
    bool        has_specular = false;  // specular *offset* exists (v10)
    bool        has_intensity = false; // payload is exactly base size
    uint32_t    size = 0;
    uint32_t    base_size = 0;      // 32 (v9) / 36 (v10)
};

// version in {9, 10} and at least 12 bytes.
bool is_light_payload(const uint8_t* d, size_t n);

// Display name for the low-3-bits type (0=Omni 1=Directional 2=Spot 3=Fog
// 5=Ambient; otherwise "Type<n>").
std::string light_type_name(uint32_t flags);

// Parse a GRO_Light payload. ok == false mirrors the Python None.
LightInfo parse_light(const uint8_t* d, size_t n);

// Fields to patch in write_light_fields; only set (engaged) members are written.
struct LightEdit {
    std::optional<uint32_t>           light_type;   // low 3 bits of flags
    std::optional<std::array<int, 3>> diffuse;      // (r,g,b) 0-255
    std::optional<std::array<int, 3>> specular;     // (r,g,b); ignored on v9
    std::optional<float>              near_, far_, inner, outer, intensity;
};

// Return a copy of the light payload with the given fields patched in place (same
// length — no reflow), mirroring light.write_light_fields. diffuse/specular
// preserve the stored top (alpha) byte; angles are radians; fields absent for the
// version (specular on v9, intensity on a non-base-size payload) are skipped.
// Returns empty for a non-parseable payload (Python raises ValueError).
std::vector<uint8_t> write_light_fields(const uint8_t* d, size_t n, const LightEdit& edit);

}  // namespace jade
