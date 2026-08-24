// RliRescale.hpp — zone atmosphere grading (io_ops/rli_rescale.py).
//
// Grade a zone's lighting "atmosphere" in the BF, in place: every lit GAO's
// baked RLI (transform_rli), optionally the dynamic GRO_Lights (grade_light)
// and the textures of UNLIT surfaces (skybox / backdrop / static decor) — all
// through the SAME make_transform() so they stay consistent.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <vector>

#include "jade/Rli.hpp"   // RliColorFn
#include "jade/SubEntry.hpp"

namespace jade {

// Build fn((r,g,b)) -> (r,g,b) for a uniform atmosphere grade (port of
// make_transform): `tint` is normalised by its max channel so the brightest
// channel keeps full `brightness`; `contrast` is a per-channel power applied
// before the gain. Output = int(x*255 + 0.5) clamped to 0..255 (the Python
// truncation, not round-half-even).
RliColorFn make_transform(double brightness = 1.0,
                          std::array<int, 3> tint = {255, 255, 255},
                          double contrast = 1.0);

// Apply fn to a GRO_Light's diffuse (and specular) colour. Mirrors the Python:
// a colour is graded whenever its tuple is present (including black, because a
// non-empty Python tuple is truthy).
// Empty result = the Python None (not a light / nothing to grade / no change).
std::vector<uint8_t> grade_light(const uint8_t* d, size_t n, const RliColorFn& fn);

// _classify_zone_textures: which texture keys are used by LIT vs
// UNLIT-static surfaces in a zone (for the unlit-texture grade pass).
void classify_zone_textures_pub(const std::vector<SubEntry>& subs,
                                std::set<uint32_t>& lit,
                                std::set<uint32_t>& unlit_static);

// One entry's grade: RLI on every lit GAO, optionally the GRO_Lights, and
// the given unlit-texture keys. changed=false when nothing was graded.
// Extracted from rescale_zone_in_bf so the rescale_rli project op shares
// the exact same per-entry behaviour.
struct RescaleDecResult {
    bool changed = false;
    std::vector<uint8_t> dec;
    uint32_t gaos = 0, lights = 0, textures = 0;
};
RescaleDecResult rescale_dec(const std::vector<uint8_t>& dec,
                             const RliColorFn& fn, bool grade_lights,
                             const std::set<uint32_t>& target_tex);

// ── before/after preview sampling (the GUI's palette swatches) ─────────────

// sample_zone: read the primary RLI of every lit GAO in the bin and
// summarise the zone's colour distribution. ok == false (Python None) when
// nothing is lit. Fully-unlit GAOs (all-zero RLI, e.g. the skybox) are
// excluded so they don't drag the average to black.
struct PaletteStats {
    bool ok = false;
    size_t count = 0;
    std::array<double, 3> average{};             // (r,g,b), unrounded
    std::vector<std::array<int, 3>> percentiles; // 5 (r,g,b) by luminance
    std::array<int, 3> min{}, max{};
};
PaletteStats sample_zone(const std::vector<SubEntry>& subs,
                         size_t max_colors = 250000);

// preview_after: apply `fn` to a sample_zone result so the GUI can show
// the graded palette without touching the BF.
PaletteStats preview_after(const PaletteStats& stats, const RliColorFn& fn);

struct RescaleStats {
    bool        ok = false;
    std::string error;
    uint32_t bins = 0, gaos = 0, lights = 0, textures = 0;
};

using RescaleLogFn = std::function<void(const std::string&)>;

// Grade the `district_filter` district (substring of entry names; empty = the
// whole archive) of the BF in place (port of rescale_zone_in_bf). Same-size
// in-place splices only; entries are recompressed with LZO1X-999 and size.grs
// is flushed once.
RescaleStats rescale_zone_in_bf(const std::string& bf_path,
                                double brightness,
                                std::array<int, 3> tint,
                                double contrast,
                                const std::string& district_filter = "500",
                                bool grade_lights = false,
                                bool grade_textures = false,
                                RescaleLogFn log = {});

}  // namespace jade
