#include "ElementThumb.hpp"

#include <QColor>
#include <QImage>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QPolygonF>

#include <algorithm>
#include <cmath>
#include <numeric>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace element_thumb {

namespace {

// Fixed isometric-ish view: yaw about the up axis, then pitch toward the
// camera. Good enough to locate a part on any mesh without controls.
const double YAW = -35.0 * M_PI / 180.0;
const double PITCH = 22.0 * M_PI / 180.0;

}  // namespace

QPixmap render_element_highlight(const std::vector<float>& vertices,
                                 const std::vector<uint32_t>& faces,
                                 const std::vector<int32_t>& face_elems,
                                 int elem_idx, int size) {
    const size_t n = vertices.size() / 3;
    const size_t n_faces = faces.size() / 3;
    if (n == 0 || n_faces == 0 || face_elems.size() != n_faces)
        return QPixmap();

    // Jade is Z-up: yaw about Z, then pitch about X. Camera looks down +Y:
    // screen_x = x, screen_y = -z, depth = y.
    const double cy = std::cos(YAW), sy = std::sin(YAW);
    const double cp = std::cos(PITCH), sp = std::sin(PITCH);
    std::vector<double> v(n * 3);
    for (size_t i = 0; i < n; ++i) {
        const double x = vertices[i * 3], y = vertices[i * 3 + 1],
                     z = vertices[i * 3 + 2];
        // rot_z then rot_x (verts @ rot_z.T @ rot_x.T).
        const double x1 = cy * x - sy * y;
        const double y1 = sy * x + cy * y;
        const double z1 = z;
        v[i * 3 + 0] = x1;
        v[i * 3 + 1] = cp * y1 - sp * z1;
        v[i * 3 + 2] = sp * y1 + cp * z1;
    }

    double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) {
            mn[c] = std::min(mn[c], v[i * 3 + size_t(c)]);
            mx[c] = std::max(mx[c], v[i * 3 + size_t(c)]);
        }
    const double center[3] = {(mn[0] + mx[0]) / 2.0, (mn[1] + mx[1]) / 2.0,
                              (mn[2] + mx[2]) / 2.0};
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < 3; ++c) v[i * 3 + size_t(c)] -= center[c];
    const double extent =
        std::max({mx[0] - mn[0], mx[2] - mn[2], 1e-6});
    const double scale = (size * 0.92) / extent;
    std::vector<double> sx(n), s_y(n), depth(n);
    for (size_t i = 0; i < n; ++i) {
        sx[i] = v[i * 3] * scale + size / 2.0;
        s_y[i] = size / 2.0 - v[i * 3 + 2] * scale;
        depth[i] = v[i * 3 + 1];
    }

    // Flat shade per face. Light roughly from the camera; abs() so flipped
    // winding doesn't render black (Jade winding varies per mesh).
    const double light[3] = {0.25, -0.85, 0.47};
    std::vector<double> shade(n_faces);
    std::vector<double> face_depth(n_faces);
    for (size_t f = 0; f < n_faces; ++f) {
        const size_t i0 = faces[f * 3], i1 = faces[f * 3 + 1],
                     i2 = faces[f * 3 + 2];
        if (i0 >= n || i1 >= n || i2 >= n) {
            shade[f] = 0.0;
            face_depth[f] = 0.0;
            continue;
        }
        const double* p0 = &v[i0 * 3];
        const double* p1 = &v[i1 * 3];
        const double* p2 = &v[i2 * 3];
        const double e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const double e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        const double nv[3] = {e1[1] * e2[2] - e1[2] * e2[1],
                              e1[2] * e2[0] - e1[0] * e2[2],
                              e1[0] * e2[1] - e1[1] * e2[0]};
        double n_len = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1]
                                 + nv[2] * nv[2]);
        if (n_len == 0) n_len = 1.0;
        double s = std::abs(nv[0] * light[0] + nv[1] * light[1]
                            + nv[2] * light[2])
                   / n_len;
        s = 0.30 + 0.65 * std::clamp(s, 0.0, 1.0);
        shade[f] = s;
        face_depth[f] = (depth[i0] + depth[i1] + depth[i2]) / 3.0;
    }

    // far → near
    std::vector<size_t> order(n_faces);
    std::iota(order.begin(), order.end(), size_t(0));
    std::stable_sort(order.begin(), order.end(),
                     [&face_depth](size_t a, size_t b) {
                         return face_depth[a] > face_depth[b];
                     });

    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(QColor(34, 34, 34));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);

    std::vector<QPolygonF> polys(n_faces);
    for (size_t f = 0; f < n_faces; ++f) {
        const size_t i0 = faces[f * 3], i1 = faces[f * 3 + 1],
                     i2 = faces[f * 3 + 2];
        if (i0 >= n || i1 >= n || i2 >= n) continue;
        polys[f] = QPolygonF({QPointF(sx[i0], s_y[i0]),
                              QPointF(sx[i1], s_y[i1]),
                              QPointF(sx[i2], s_y[i2])});
    }

    bool any_hl = false;
    for (size_t f = 0; f < n_faces; ++f)
        if (face_elems[f] == elem_idx) { any_hl = true; break; }

    for (size_t fi : order) {
        if (polys[fi].isEmpty()) continue;
        const double s = shade[fi];
        QColor col;
        if (face_elems[fi] == elem_idx) {
            col = QColor(int(255 * s), int(145 * s), int(40 * s));
        } else {
            const int g = int(70 + 150 * s);
            col = QColor(g, g, g);
        }
        // Hairline pen in the fill colour closes the cracks AA leaves
        // between adjacent triangles.
        p.setPen(QPen(col, 1.0));
        p.setBrush(col);
        p.drawPolygon(polys[fi]);
    }

    // X-ray pass: the element shows through everything that occludes it.
    if (any_hl) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 100, 0, 70));
        for (size_t f = 0; f < n_faces; ++f)
            if (face_elems[f] == elem_idx && !polys[f].isEmpty())
                p.drawPolygon(polys[f]);
    }
    p.end();
    return QPixmap::fromImage(img);
}

}  // namespace element_thumb
