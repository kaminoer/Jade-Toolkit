// ObjectKinds.hpp — classify non-visual GAOs into editor categories (read path).
//
// Port of jade_explorer/core/object_kinds.py (classify_object + is_bone). The
// classification is by the level designers' naming convention (regexes) plus a
// few identity-flag signals; it's a best-effort bucket for iconography/filtering.
// The GUI-only presentation helpers (category_label/color, marker SDF icons) are
// deferred with the rest of the GUI layer.
#pragma once

#include <cstdint>
#include <string>

namespace jade {

// gro_type of a GAO's visual-block target: 6 = camera, 11 = particle FX.
constexpr int GRO_CAMERA = 6;
constexpr int GRO_FX = 11;

// Editor category for a non-mesh GAO. `gro_type` is the gro_type of the GAO's
// visual-block target when it has one, else -1 (Python None). `father_in_bin`
// is accepted for parity (used only by is_bone).
std::string classify_object(const std::string& name, uint32_t identity = 0,
                            int gro_type = -1, bool father_in_bin = false);

// True if this GAO is a skeleton joint (callers hide these from the object list).
bool is_bone(const std::string& name, uint32_t identity, bool father_in_bin);

}  // namespace jade
