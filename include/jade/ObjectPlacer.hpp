// ObjectPlacer.hpp — object placement (io_ops/object_placer.py), staged port.
//
// Stage B1: the key allocator. Generated keys MUST match the Python toolkit
// bit-for-bit (they end up inside written archives and in .jtmod diffs), so
// CPythonRandom reproduces CPython's random.Random exactly: MT19937 seeded via
// init_by_array on the seed's 32-bit words, randrange via the getrandbits
// rejection loop.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/SubEntry.hpp"

namespace jade {

// CPython random.Random for a single-word integer seed (all allocator seeds are
// masked to 32 bits; seed 0 is the caller's job to remap, like `seed or ...`).
class CPythonRandom {
public:
    explicit CPythonRandom(uint32_t seed) { seed_with(seed); }

    // getrandbits(k) for 1 <= k <= 32.
    uint32_t getrandbits(int k) { return genrand() >> (32 - k); }

    // random.Random.randrange(0, n) == _randbelow(n): k = n.bit_length(),
    // draw getrandbits(k) until < n.
    uint32_t randbelow(uint32_t n) {
        int k = 0;
        for (uint32_t v = n; v; v >>= 1) ++k;
        uint32_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }

private:
    uint32_t mt_[624];
    int idx_ = 624;

    void init_genrand(uint32_t s) {
        mt_[0] = s;
        for (int i = 1; i < 624; ++i)
            mt_[i] = 1812433253u * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + uint32_t(i);
        idx_ = 624;
    }
    // MT19937-2002 init_by_array with a single-word key (CPython random_seed
    // splits the |seed| into little-endian u32 words; ours fit one word).
    void seed_with(uint32_t key) {
        init_genrand(19650218u);
        int i = 1;
        uint32_t j = 0;
        for (int k = 624; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + key + j;
            ++i;
            ++j;
            if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
            if (j >= 1) j = 0;                     // key_length == 1
        }
        for (int k = 623; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - uint32_t(i);
            ++i;
            if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
        }
        mt_[0] = 0x80000000u;
        idx_ = 624;
    }
    uint32_t genrand() {
        if (idx_ >= 624) {
            for (int i = 0; i < 624; ++i) {
                uint32_t y = (mt_[i] & 0x80000000u) | (mt_[(i + 1) % 624] & 0x7FFFFFFFu);
                mt_[i] = mt_[(i + 397) % 624] ^ (y >> 1);
                if (y & 1) mt_[i] ^= 2567483615u;
            }
            idx_ = 0;
        }
        uint32_t y = mt_[idx_++];
        y ^= y >> 11;
        y ^= (y << 7) & 2636928640u;
        y ^= (y << 15) & 4022730752u;
        y ^= y >> 18;
        return y;
    }
};

// Generate high-range (0x7A......) resource keys avoiding known collisions —
// port of object_placer.KeyAllocator (seed 0 remaps to 0x4A414445 "JADE").
class KeyAllocator {
public:
    KeyAllocator(std::unordered_set<uint32_t> used, uint32_t seed = 0)
        : used_(std::move(used)), rng_(seed ? seed : 0x4A414445u) {}

    uint32_t next() {
        for (;;) {
            uint32_t key = 0x7A000000u | (rng_.randbelow(0x00FFFFFFu) ^ counter_);
            ++counter_;
            key &= 0x7FFFFFFFu;
            if (used_.find(key) == used_.end() && key != 0xFFFFFFFFu) {
                used_.insert(key);
                return key;
            }
        }
    }

private:
    std::unordered_set<uint32_t> used_;
    CPythonRandom rng_;
    uint32_t counter_ = 0;
};

// core/asset_add.py uses a similar-looking but distinct allocator. A non-zero
// seed is byte-for-byte CPython random.Random(seed); seed 0 mirrors
// random.Random(None) by drawing fresh process/OS entropy. Unlike the placement
// allocator there is no counter XOR and randrange starts at one.
class AssetKeyAllocator {
public:
    AssetKeyAllocator(std::unordered_set<uint32_t> used, uint32_t seed = 0)
        : used_(std::move(used)), rng_(seed ? seed : entropy_seed()) {}

    uint32_t next() {
        for (uint32_t attempt = 0; attempt < (1u << 16); ++attempt) {
            // random.Random.randrange(1, 0x00FFFFFF): [1, 0x00FFFFFE].
            const uint32_t key = 0x7A000000u
                               | (1u + rng_.randbelow(0x00FFFFFEu));
            if (used_.insert(key).second) return key;
        }
        throw std::runtime_error("modded key space exhausted (?)");
    }

private:
    static uint32_t entropy_seed() {
        std::random_device rd;
        uint32_t seed = uint32_t(rd());
        seed ^= uint32_t(rd()) * 0x9E3779B9u;
        return seed;
    }

    std::unordered_set<uint32_t> used_;
    CPythonRandom rng_;
};

// ── Stage B2: byte-builder leaf helpers ────────────────────────────────────

namespace placer {

using Vec3 = std::array<double, 3>;
using Mat16 = std::array<double, 16>;   // column-major, translation in col 3

constexpr uint32_t INVALID_KEY = 0xFFFFFFFFu;
constexpr uint32_t FLAG_OBBOX = 0x00080000u;
constexpr uint32_t MATRIX_TYPE = 2;
constexpr uint32_t DEFAULT_VISUAL_DRAW_MASK = 0x0D2255FFu;

// _vec3(value, minimum): pad/truncate to 3; with a minimum, max(min, |v|).
Vec3 vec3(const Vec3& v);
Vec3 vec3_min(const Vec3& v, double minimum);

// _clean_gao_name: sanitize to [alnum _-.@ ], default JadePlacedObject,
// ensure .gao suffix, cap at 120 chars.
std::string clean_gao_name(const std::string& name);

// ops_transform: intrinsic-XYZ euler (degrees) -> quaternion (x,y,z,w).
std::array<double, 4> euler_xyz_deg_to_quat(const Vec3& deg);
// T(p)·R(euler)·S(scale) as a column-major Jade-storage matrix.
Mat16 trs_to_matrix(const Vec3& position, const Vec3& euler_deg, const Vec3& scale);
// Column-major 4x4 multiply a·b.
Mat16 col_major_mul(const Mat16& a, const Mat16& b);

// Identity matrix with translation (=_matrix_values).
Mat16 matrix_values(const Vec3& position);

// 68-byte matrix record: 16 f32 + MATRIX_TYPE u32.
std::vector<uint8_t> matrix68(const Vec3& position,
                              const Vec3& rotation_euler_deg = {0, 0, 0},
                              const Vec3& scale = {1, 1, 1});

// Colour source for visual_bytes: none (flat white), a single flat tint, or a
// per-vertex list (shorter lists pad with white).
struct VisualColors {
    bool flat = false;
    std::array<double, 3> tint{};
    bool per_vertex = false;
    std::vector<std::array<double, 3>> list;
};

// Visual block for an identity-0x95000 GAO (nb_vertices==0 collapses to the
// minimal legacy tail).
std::vector<uint8_t> visual_bytes(uint32_t geometry_key, uint32_t material_key,
                                  int nb_vertices = 0,
                                  const VisualColors* colors = nullptr);

std::vector<uint8_t> ode_box_bytes(const Vec3& dimensions);
std::vector<uint8_t> extended_stub_bytes();
std::vector<uint8_t> runtime_editor_tail(const std::string& name);

// 48-byte OBBox BV block: [GMin][GMax][LMin][LMax], LOCAL space (GMin/GMax are
// the R·S-oriented corners; translation intentionally NOT baked in).
std::vector<uint8_t> obbox_bytes(const Vec3& lmin, const Vec3& lmax,
                                 const Vec3& rotation_euler_deg = {0, 0, 0},
                                 const Vec3& scale = {1, 1, 1});

// Replace a GAO payload's name block. Empty result = the Python PlacementError.
std::vector<uint8_t> replace_gao_name(const uint8_t* payload, size_t n,
                                      const std::string& new_name);

struct GaoOffsets {
    bool ok = false;    // false = the Python PlacementError
    uint32_t identity = 0;
    size_t status = 0, global_matrix = 0, bv = 0;
};
GaoOffsets gao_offsets(const uint8_t* d, size_t n);

// In-place patches (mirror the Python bytearray mutators). Return false on the
// Python PlacementError (offsets/length invalid).
bool patch_gao_position(std::vector<uint8_t>& data, const Vec3& position);
bool patch_gao_world_matrix(std::vector<uint8_t>& data, const Mat16& world_matrix,
                            const Vec3* lb_min = nullptr, const Vec3* lb_max = nullptr);
bool patch_gao_trs(std::vector<uint8_t>& data, const Vec3& position,
                   const Vec3& rotation_euler_deg, const Vec3& scale);

// Jade native matrix (I/J/K cols + S? slots) -> standard math 4x4.
Mat16 jade_matrix_to_math_4x4(const Mat16& m16);
// Jade lType flag word (Translation 2 | Rotation 4 | Scale 8).
uint32_t ltype_from_jade_matrix(const Mat16& m16);

// ── Stage B3: primitive geometry + GEO payload serialization ──────────────

// _geo_dict's fields (primitive/model geometry ready for serialization).
struct PlacerElement {
    uint32_t n_tri = 0;
    uint32_t mat_id = 0;
};
struct PlacerGeo {
    std::vector<std::array<double, 3>> vertices;
    std::vector<std::array<double, 3>> normals;
    std::vector<std::array<double, 2>> uvs;
    std::vector<std::array<uint32_t, 3>> faces;
    std::vector<std::array<uint32_t, 3>> face_uvs;
    std::vector<uint32_t> face_flags;       // empty -> GEO_V7_TRIANGLE_FLAGS
    uint32_t material_key = 0;
    std::vector<PlacerElement> elements;
    bool has_colors = false;                // dul_PointColors source
    std::vector<std::array<int, 4>> colors; // (r,g,b,a) 0-255
};

// _norm: numpy float32 arithmetic — inputs cast to f32, ((x²+y²)+z²) in f32,
// sqrtf, f32 divisions; |len| < 1e-8 -> (0,0,1).
std::array<double, 3> norm3(double x, double y, double z);

PlacerGeo build_cube_geometry(double sx, double sy, double sz, uint32_t material_key);
PlacerGeo build_cylinder_geometry(double sx, double sy, double sz,
                                  uint32_t material_key, int segments = 24);
PlacerGeo build_sphere_geometry(double sx, double sy, double sz,
                                uint32_t material_key, int slices = 24, int stacks = 12);

// geometry_to_payload: the simple unskinned parser-only GEO (v7 flags 0x1008 /
// v8 flags 0x10, no trailing block, explicit (0,0) terminator).
std::vector<uint8_t> geometry_to_payload(const PlacerGeo& geo, uint32_t geo_version = 7);

// geometry_to_payload_with_vb: renderable profile (flags1 0x4 + sniffed
// platform word) + the trailing element table / cooked stride-20 VB /
// u16 IB (byte-size field, no pad).
std::vector<uint8_t> geometry_to_payload_with_vb(const PlacerGeo& geo,
                                                 uint32_t geo_version = 7,
                                                 uint32_t platform_flags = 0x4);

// build_primitive_geometry: cube/sphere/cylinder + optional flat vertex colour.
PlacerGeo build_primitive_geometry(const std::string& kind, const Vec3& size,
                                   uint32_t material_key = 0,
                                   const std::array<int, 4>* color = nullptr);

// core/object_placer.py::load_model_geometry. Static model data is converted
// into the same simple PlacerGeo profile used by generated primitives. The
// current built-in readers cover DAE, binary FBX, glTF/GLB, OBJ, OFF, STL,
// and PLY without Qt or external runtime dependencies; unsupported formats
// fail.
PlacerGeo load_model_geometry(const std::string& model_path,
                              const Vec3& scale = {1, 1, 1},
                              uint32_t material_key = 0,
                              bool import_vertex_colors = false);
void clear_model_geometry_cache();

// build_gao_payload: a minimal root visual GAO (identity 0x95000; optionally
// + Extended/ColMap when collision_box).
std::vector<uint8_t> build_gao_payload(
    const std::string& name, uint32_t geometry_key, uint32_t material_key,
    const Vec3& position, const Vec3& lb_min, const Vec3& lb_max,
    bool collision_box = true, uint32_t colmap_key = INVALID_KEY,
    const Vec3& rotation_euler_deg = {0, 0, 0}, const Vec3& scale = {1, 1, 1},
    int nb_vertices = 0, const VisualColors* colors = nullptr);

// clone_gao_payload: rename + retarget translation (legacy fast path) or
// compose T(p)·R·S·M_src. Empty = the Python PlacementError (bad source name).
std::vector<uint8_t> clone_gao_payload(const uint8_t* src, size_t n,
                                       const std::string& new_name, const Vec3& position,
                                       const Vec3& rotation_euler_deg = {0, 0, 0},
                                       const Vec3& scale = {1, 1, 1});

}  // namespace placer

// ── Stage B4: apply_placements_to_dec (the placement orchestrator) ─────────

namespace placer {

// One placement op (the Python op dict).
// One entry of a cross-bin source resource chain (BFS-by-level body order —
// the order the engine's FIFO fetch queue processes sub-resource refs).
struct ChainEntry {
    uint32_t key = 0;
    std::string ext;                 // "" when the sub-entry is gro-typed
    bool has_gro_type = false;
    uint32_t gro_type = 0;
    std::vector<uint8_t> data;       // payload trimmed to declared size-4
};

struct PlaceOp {
    std::string kind = "cube";            // cube | sphere | cylinder | model | clone
    std::string name;                     // empty -> "JadePlaced_<kind>"
    Vec3 position{0, 0, 0};
    Vec3 rotation_euler_deg{0, 0, 0};
    Vec3 scale_xform{1, 1, 1};
    bool has_world_matrix = false;
    Mat16 world_matrix{};
    uint32_t source_key = 0;              // clone source (same bin)
    Vec3 size{1, 1, 1};
    bool has_material_key = false;        // absent/0 -> the auto-pick chain
    uint32_t material_key = 0;
    bool collision = false;
    std::string collision_profile;        // "" | "simple_box" | "ledge_openbox"
    bool has_room_cob_key = false;
    uint32_t room_cob_key = 0;            // explicit host-COB override
    bool has_vertex_color = false;
    std::array<int, 4> vertex_color{};
    std::string model_path;                // kind=="model", resolved asset/path
    bool import_vertex_colors = false;
    // kind=="replace" (ReplaceMesh): source GAO or imported JGAO bundle.
    bool has_target_key = false;
    uint32_t target_key = 0;
    bool has_source_gao_key = false;
    uint32_t source_gao_key = 0;
    bool has_source_entry_index = false;
    uint32_t source_entry_index = 0;
    bool has_imported_jgao = false;
    uint32_t imported_mat_key = INVALID_KEY;
    std::vector<uint8_t> imported_geo_data;
    std::vector<uint8_t> imported_mat_data;
    std::vector<ChainEntry> imported_mat_children;
    // Cross-bin clone (kind=="clone" with a pre-read source): the source
    // GAO's payload bytes + its transitive resource chain, as collected by
    // collect_xbin_source_chain (Python: AddObject.apply's BFS walk).
    bool has_source_gao_data = false;
    std::vector<uint8_t> source_gao_data;
    std::vector<ChainEntry> source_resource_chain;
};

struct BuiltOperation {
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> additions;
    std::vector<uint32_t> object_keys;
    bool generated = false;
    std::vector<uint32_t> dependency_keys;
    std::vector<uint32_t> nested_dependency_keys;
    bool has_material_key = false;
    uint32_t material_key = 0;
    bool contiguous = false;
    bool front_of_stream = false;
    bool per_item_donor = false;
};

struct PlacementResult {
    bool ok = false;
    std::string error;                    // the Python PlacementError message
    std::vector<uint8_t> patched;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> additions;
    bool has_world_list_key = false;
    uint32_t world_list_key = 0;
    uint32_t objects_registered = 0;
};

// collision.make_sub_entry: [size][99C0FFEE][key][type/ext][payload].
std::vector<uint8_t> make_sub_entry(uint32_t key, uint32_t gro_type,
                                    const std::vector<uint8_t>& payload);
std::vector<uint8_t> make_sub_entry_ext(uint32_t key, const char ext[4],
                                        const std::vector<uint8_t>& payload);

// _detect_geo_version (dec-only variant; 7 when unknown).
uint32_t detect_geo_version_from_dec(const std::vector<SubEntry>& subs);
// _detect_geo_platform_flags: most-common non-zero header word[2] of the
// zone's renderable GEOs (first-seen wins a tie, like Counter.most_common).
uint32_t detect_geo_platform_flags(const std::vector<SubEntry>& subs,
                                   uint32_t default_flags = 0x4);

// collision.gao_colmap_key_offsets: every dword offset in a GAO payload whose
// u32 names a ColMap sub-entry (unaligned scan; ColMap refs sit at odd
// offsets). Used by clone re-keying and by remove_object's collision sever.
std::vector<std::pair<size_t, uint32_t>> gao_colmap_key_offsets(
    const std::vector<uint8_t>& gao,
    const std::unordered_map<uint32_t, const SubEntry*>& by_key);

// loa_validate._world_list_subs: the ordered world-object-list GAO keys of a
// parsed zone entry. found == false when no list scores well enough.
struct WorldListKeysResult {
    bool found = false;
    std::vector<uint32_t> keys;
};
WorldListKeysResult world_object_list_keys(const std::vector<SubEntry>& subs);

// add_object_collision_box: give an EXISTING object collision without moving
// it, by extending the host room's master COB(s) at the object's current
// pose. shape "mesh" (default) reuses the object's visual triangles so
// concave shapes keep their openings; "box" appends its oriented OBBox (via
// the clone-with-collision path). Host COBs are picked from the object's
// WORLD geometry centre unless room_cob_key overrides. ok == false carries
// the Python PlacementError message.
struct AddCollisionResult {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> dec;
};
AddCollisionResult add_object_collision_box(
    const std::vector<uint8_t>& dec, uint32_t gao_key,
    const std::string& collision_profile,   // "" -> "simple_box"
    bool has_room_cob_key, uint32_t room_cob_key,
    const std::string& shape);              // "" -> "mesh"

// _collect_materials / _first_material_key: the static-renderable material
// auto-pick (used when a primitive op carries no explicit material_key).
// gro-5 single-materials count only when a STATIC SHIPPED visual GAO
// references them (name not Fake_/GFX_/B_/M_, key not 0x7Axxxxxx); sorted by
// (-visual_refs, gro_type != 4, stream order). Returns keys in pick order.
std::vector<uint32_t> collect_material_keys(const std::vector<SubEntry>& subs);
// 0-return = Python None (no candidate).
uint32_t first_material_key(const std::vector<SubEntry>& subs);

// apply_placements_to_dec: the non-destructive placement core (no BF I/O).
// geo_version 0 = sniff from the dec. Ops with collision=true return an error
// until the stage-C COB-extension pass is ported.
PlacementResult apply_placements_to_dec(const std::vector<uint8_t>& dec,
                                        const std::vector<PlaceOp>& ops,
                                        uint32_t geo_version = 0,
                                        const std::vector<uint32_t>& extra_used_keys = {},
                                        uint32_t allocator_seed = 0);

// ── ReplaceMesh flow (_apply_geo_replacements, GAO-to-GAO branch) ───────────
// _scale_raw_geo: scale vertex positions in a raw GEO payload — the parser
// vertex array AND the trailing cooked VB (located by a stride-value scan
// over the first 200 trailing bytes; strides 52/32/36/40/44/48).
std::vector<uint8_t> scale_raw_geo(const std::vector<uint8_t>& raw, const Vec3& scale);

// The _apply_geo_replacements splice pass itself is internal to
// ObjectPlacer.cpp (like the Python underscore function) — replace ops enter
// through apply_placement_plan. Native port covers the source_gao_key +
// source_entry_index branch (raw cross-entry GEO copy incl. trailing VB,
// optional scale, material child patch-in-place + missing-material append),
// plus imported_jgao raw GEO/material replacement and static model_path
// replacement through the native model importer.

// ── cross-bin clone source collection (AddObject.apply's BFS walk) ──────────
// Reads a source GAO from another BF entry and BFS-walks its transitive u32
// reference closure in body-parse order (the engine's FIFO fetch order).
// gao_data is the source GAO payload trimmed to its declared size.
struct XbinSource {
    bool ok = false;
    std::string error;
    std::vector<uint8_t> gao_data;
    std::vector<ChainEntry> chain;
};
XbinSource collect_xbin_source_chain(BigFile& bf, uint32_t source_entry_index,
                                     uint32_t source_gao_key);

// project.ops_object.AddObject._simulate_loa deliberately differs from the
// build collector above: it starts from the legacy cookie-scan payload,
// traverses with a LIFO stack, then restores source-offset order. Keep that
// validation-only behavior separate so full validation matches Python without
// changing the engine-safe BFS layout used by actual builds.
XbinSource collect_xbin_validation_source_chain(
    BigFile& bf, uint32_t source_entry_index, uint32_t source_gao_key);

// ── apply_placement_plan (the BF-level writer around the dec core) ──────────
// Copies bf_path to out_path and writes the patched entry there: in place
// (zero-padded) when it fits its compressed slot, else appended
// sector-aligned with the FAT pos + FileExt length repointed (only with
// strict_fit=false — the Python default true refuses to grow the slot).
// Also patches the entry's size.grs declared-length row in place. Replace
// ops (kind=="replace") run first via apply_geo_replacements (GAO-to-GAO
// sources, imported-JGAO raw GEO/material bundles, and model_path sources.
struct PlacementSummary {
    bool ok = false;
    std::string error;                    // the Python PlacementError message
    uint32_t entry_index = 0;
    uint32_t objects_added = 0;
    uint32_t sub_entries_added = 0;
    uint32_t objects_registered = 0;
    uint32_t original_compressed_size = 0;
    uint32_t new_compressed_size = 0;
    std::string output_path;
    bool wrote_in_place = false;
    bool has_world_list_key = false;
    uint32_t world_list_key = 0;
    uint32_t meshes_replaced = 0;
};

// _detect_geo_version with the BF-sampling fallback (dec scan first, then
// version<38 -> 7, then sample up to 5 entries for a gro-1 header word).
uint32_t detect_geo_version(BigFile& bf, const std::vector<uint8_t>& dec);

PlacementSummary apply_placement_plan(const std::string& bf_path,
                                      const std::string& out_path,
                                      uint32_t entry_index,
                                      const std::vector<PlaceOp>& ops,
                                      bool strict_fit = true);

// ── apply_bin_replacement (role-matching visual transplant) ─────────────────
// Replace a BF entry's visual meshes with another entry's: keep the target's
// structure (skeleton, hierarchy, FX GAOs) and transplant only the source's
// GEO data, materials and rekeyed visual GAOs — visuals matched by ROLE
// (body/face/arms/eyes/thighs from GAO names), GizmoPtr bone refs remapped by
// normalized bone name, unmatched same-role target visuals hidden, source
// bone-visuals injected into bare target bones. Ports
// apply_bin_replacement/_transplant_visuals. meshes_replaced = GEO swaps.
PlacementSummary apply_bin_replacement(const std::string& bf_path,
                                       const std::string& out_path,
                                       uint32_t target_entry_index,
                                       uint32_t source_entry_index,
                                       bool strict_fit = true);

}  // namespace placer

}  // namespace jade
