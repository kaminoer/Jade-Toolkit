// ElementThumb.hpp — software-rendered element-highlight thumbnails for
// the Mesh Swap tab (port of gui/element_thumb.py).
//
// Renders a GEO as a small flat-shaded image with one element's triangles
// highlighted in orange — "which part of the mesh does this
// material/texture cover?". Pure QPainter (orthographic projection,
// painter's-algorithm depth sort), so it needs no OpenGL context and works
// inside a tooltip; meshes here are small (Jade zone/actor GEOs are a few
// thousand tris) and the result is cached by the caller.
#pragma once

#include <QPixmap>
#include <cstdint>
#include <vector>

namespace element_thumb {

// Render the mesh flat-shaded gray with element `elem_idx` orange.
//
// vertices: 3*n floats (Jade Z-up), faces: 3*m vertex indices,
// face_elems: per-face element index (size m). Occluded parts of the
// element are overdrawn semi-transparent so the part is locatable even
// when it faces away from the fixed camera. Returns a null QPixmap when
// the GEO has no renderable triangles.
QPixmap render_element_highlight(const std::vector<float>& vertices,
                                 const std::vector<uint32_t>& faces,
                                 const std::vector<int32_t>& face_elems,
                                 int elem_idx, int size = 230);

}  // namespace element_thumb
