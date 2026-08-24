// BoneMapView.cpp — 2D orthographic viewer for bone remapping outcome
// (port of gui/bone_map_view.py). See BoneMapView.hpp for the legend.
#include "BoneMapView.hpp"

#include <QApplication>
#include <QBrush>
#include <QComboBox>
#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPolygonF>
#include <QPushButton>
#include <QRectF>
#include <QSizePolicy>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#include "Theme.hpp"

namespace {

// ── theme-aware palette ──
//
// Two palettes — picked once per session from the active Qt app palette.
// The light variant is the original (#fafafa background, etc.); the
// dark variant uses a near-black canvas with brighter foreground
// colours so the bones/joints stay readable on dark Qt themes (the
// user's setup is dark-theme based, where #fafafa is jarring and the
// original colours wash out). Under the native app's jade theme the
// dark palette's neutral colours are drawn from Theme.hpp so the canvas
// matches the rest of the app; the series colours stay the tuned
// Python-dark values so bones / joints / arcs remain visually distinct.
struct ViewPalette {
    QColor bg;
    QColor grid;
    QColor axes;
    QColor bone;
    QColor bone_idle;
    QColor joint;
    QColor arc_green;
    QColor arc_copper;
    QColor arc_red;
    QColor drop;
    QColor unmap;
    QColor select_halo;
    QColor label_fg;
    QColor label_bg;
    QColor overlay;
    QColor empty;
};

const ViewPalette& light_palette() {
    static const ViewPalette pal = {
        QColor("#fafafa"),           // bg
        QColor("#e8e8e8"),           // grid
        QColor("#cccccc"),           // axes
        QColor("#1e88e5"),           // bone
        QColor("#90caf9"),           // bone_idle
        QColor("#f57c00"),           // joint
        QColor("#2e8b57"),           // arc_green
        QColor("#b87333"),           // arc_copper
        QColor("#b22222"),           // arc_red
        QColor("#b22222"),           // drop
        QColor("#9e9e9e"),           // unmap
        QColor(255, 200, 0, 220),    // select_halo
        QColor("#333333"),           // label_fg
        QColor(255, 255, 255, 220),  // label_bg
        QColor("#555555"),           // overlay
        QColor("#888888"),           // empty
    };
    return pal;
}

const ViewPalette& dark_palette() {
    QColor label_bg(theme::WINDOW_BG);  // was (30, 30, 30, 220)
    label_bg.setAlpha(220);
    static const ViewPalette pal = {
        QColor(theme::BASE_BG),    // bg    (was #2a2a2a)
        QColor(theme::BORDER),     // grid  (was #3a3a3a)
        QColor("#4f4f4f"),         // axes
        QColor("#6fb5f5"),         // bone
        QColor("#39597c"),         // bone_idle
        QColor("#ffa050"),         // joint
        QColor("#7fdba6"),         // arc_green
        QColor("#e0a878"),         // arc_copper
        QColor("#ff7878"),         // arc_red
        QColor("#ff7878"),         // drop
        QColor("#909090"),         // unmap
        QColor(255, 215, 80, 220),  // select_halo
        QColor(theme::TEXT),       // label_fg  (was #f0f0f0)
        label_bg,                  // label_bg
        QColor(theme::DIM_TEXT),   // overlay   (was #bbbbbb)
        QColor(theme::DIM_TEXT),   // empty     (was #bbbbbb)
    };
    return pal;
}

// Return the active palette for the bone-map view (cached) (_view_palette).
const ViewPalette& view_palette() {
    static const ViewPalette* cache = nullptr;
    if (cache != nullptr) return *cache;
    const ViewPalette* chosen = &light_palette();
    if (QApplication::instance() != nullptr) {
        const QColor bg =
            QApplication::palette().color(QPalette::ColorRole::Window);
        const double lum =
            0.299 * bg.red() + 0.587 * bg.green() + 0.114 * bg.blue();
        if (lum < 128) chosen = &dark_palette();
    }
    cache = chosen;
    return *cache;
}

constexpr Qt::MouseButton PAN_BUTTON = Qt::LeftButton;

// Severity thresholds — match the per-row colours in the dialog table.
constexpr double DELTA_GREEN = 0.1;
constexpr double DELTA_COPPER = 0.5;

// ── module helpers ──

QPolygonF diamond(const QPointF& centre, double r) {
    return QPolygonF({
        QPointF(centre.x(), centre.y() - r),
        QPointF(centre.x() + r, centre.y()),
        QPointF(centre.x(), centre.y() + r),
        QPointF(centre.x() - r, centre.y()),
    });
}

// Return a 1/2/5×10ⁿ value near `span` for a clean grid spacing (_nice_step).
double nice_step(double span) {
    if (span <= 0) return 1.0;
    const double exp = std::floor(std::log10(span));
    const double base = span / std::pow(10.0, exp);
    for (double cand : {1.0, 2.0, 5.0, 10.0}) {
        if (base <= cand) return cand * std::pow(10.0, exp);
    }
    return std::pow(10.0, exp + 1);
}

}  // namespace

// ── BoneMapView ──

BoneMapView::BoneMapView(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(2, 2, 2, 2);
    root->setSpacing(2);

    auto* bar = new QHBoxLayout();
    bar->setContentsMargins(4, 0, 4, 0);
    bar->addWidget(new QLabel(tr("<b>3D outcome view</b>")));
    bar->addStretch(1);
    bar->addWidget(new QLabel(tr("Projection:")));
    view_combo_ = new QComboBox();
    view_combo_->addItem(tr("Front (X / Z)"), QStringLiteral("front"));
    view_combo_->addItem(tr("Side  (Y / Z)"), QStringLiteral("side"));
    view_combo_->addItem(tr("Top   (X / Y)"), QStringLiteral("top"));
    connect(view_combo_, &QComboBox::currentIndexChanged, this,
            &BoneMapView::on_view_changed);
    bar->addWidget(view_combo_);
    auto* fit = new QPushButton(tr("Fit"));
    fit->setToolTip(tr("Reset pan and zoom to fit all bones + joints."));
    fit->setMaximumWidth(48);
    connect(fit, &QPushButton::clicked, this,
            [this] { canvas_->reset_view(); });
    bar->addWidget(fit);
    root->addLayout(bar);

    canvas_ = new BoneCanvas(this);
    connect(canvas_, &BoneCanvas::bone_clicked, this,
            &BoneMapView::bone_clicked);
    connect(canvas_, &BoneCanvas::joint_clicked, this,
            &BoneMapView::joint_clicked);
    root->addWidget(canvas_, 1);

    // Legend strip — explains the colour code so the user doesn't
    // have to read the docstring. (The Python hardcoded the light-theme
    // series colours here; the port pulls the active palette so the
    // legend matches the canvas on the jade dark theme.)
    const ViewPalette& pal = view_palette();
    auto* leg = new QLabel(
        QStringLiteral(
            "<span style='color:%1'>● bone</span> &nbsp; "
            "<span style='color:%2'>◆ joint</span> &nbsp; "
            "<span style='color:%3'>━ aligned</span> &nbsp; "
            "<span style='color:%4'>━ caution</span> &nbsp; "
            "<span style='color:%5'>━ mismatch</span> &nbsp; "
            "<span style='color:%6'>✕ drop</span> &nbsp; "
            "<span style='color:%7'>○ unmapped</span>")
            .arg(pal.bone.name(), pal.joint.name(), pal.arc_green.name(),
                 pal.arc_copper.name(), pal.arc_red.name(), pal.drop.name(),
                 pal.unmap.name()));
    leg->setStyleSheet(QStringLiteral("padding: 2px 4px;"));
    root->addWidget(leg);
}

void BoneMapView::on_view_changed() {
    QString ax = view_combo_->currentData().toString();
    if (ax.isEmpty()) ax = QStringLiteral("front");
    canvas_->set_view_axis(ax);
}

// ── data API: the dialog calls this on every state change ──

void BoneMapView::set_data(const BoneMapData& data) {
    canvas_->update_data(data);
}

void BoneMapView::set_selection(std::optional<int> bone,
                                std::optional<int> joint) {
    canvas_->set_selection(bone, joint);
}

// ── BoneCanvas ──

BoneCanvas::BoneCanvas(QWidget* parent) : QWidget(parent) {
    setMinimumSize(280, 280);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // The fill happens in paintEvent so it always reflects the
    // active theme palette; no widget-level stylesheet needed.
}

// ── data API ──

void BoneCanvas::update_data(const BoneMapData& data) {
    bone_pos_ = data.bone_positions;
    joint_pos_ = data.joint_positions;
    bone_names_ = data.bone_names;
    joint_names_ = data.joint_names;
    actions_ = data.actions;
    joint_stats_ = data.joint_stats;
    bone_old_shares_ = data.orig_bone_weight_shares;
    rb_bone_ = data.rigid_bind_bone;
    mesh_diag_ = data.mesh_diagonal;
    if (auto_fit_needed_)
        reset_view();
    else
        update();
}

void BoneCanvas::set_view_axis(const QString& axis) {
    if (axis == view_axis_) return;
    view_axis_ = axis;
    reset_view();
}

void BoneCanvas::set_selection(std::optional<int> bone,
                               std::optional<int> joint) {
    if (bone.has_value()) {
        selected_bone_ = *bone;
        selected_joint_ = -1;
    } else if (joint.has_value()) {
        selected_joint_ = *joint;
        selected_bone_ = -1;
    } else {
        selected_bone_ = -1;
        selected_joint_ = -1;
    }
    update();
}

// Auto-fit the current data into the widget rect with margin.
void BoneCanvas::reset_view() {
    const std::vector<std::pair<double, double>> pts = all_projected_points();
    if (pts.empty()) {
        zoom_ = 1.0;
        pan_ = QPointF(0, 0);
        auto_fit_needed_ = false;
        update();
        return;
    }
    double wx0 = pts[0].first, wx1 = pts[0].first;
    double wy0 = pts[0].second, wy1 = pts[0].second;
    for (const auto& p : pts) {
        wx0 = std::min(wx0, p.first);
        wx1 = std::max(wx1, p.first);
        wy0 = std::min(wy0, p.second);
        wy1 = std::max(wy1, p.second);
    }
    // Always reserve some range to avoid div-by-zero on collapsed
    // axes (e.g. a top view of a vertical-only skeleton).
    const double wx_span = std::max(wx1 - wx0, 0.1);
    const double wy_span = std::max(wy1 - wy0, 0.1);
    // Centre the data; world origin at widget centre via pan.
    const double cx = (wx0 + wx1) / 2.0;
    const double cy = (wy0 + wy1) / 2.0;
    const QRect rect = contentsRect();
    const double margin = 32;
    const double sx = (rect.width() - margin * 2) / wx_span;
    const double sy = (rect.height() - margin * 2) / wy_span;
    zoom_ = std::max(std::min(sx, sy), 1e-3);
    // Pan stored in world coords (subtracted before scaling).
    pan_ = QPointF(cx, cy);
    auto_fit_needed_ = false;
    update();
}

// ── projection ──

// Return (wx, wy) world-2D for a 3-vector under the current axis.
std::optional<std::pair<double, double>> BoneCanvas::project(
    const OptPos3& p3) const {
    if (!p3.first) return std::nullopt;
    const std::array<double, 3>& p = p3.second;
    if (view_axis_ == QLatin1String("front"))
        return std::make_pair(p[0], -p[2]);  // X right, Z up (negate so up is up)
    if (view_axis_ == QLatin1String("side"))
        return std::make_pair(p[1], -p[2]);
    // top
    return std::make_pair(p[0], -p[1]);
}

QPointF BoneCanvas::world_to_screen(double wx, double wy) const {
    const QRect rect = contentsRect();
    const double cx = rect.center().x();
    const double cy = rect.center().y();
    const double x = cx + (wx - pan_.x()) * zoom_;
    const double y = cy + (wy - pan_.y()) * zoom_;
    return QPointF(x, y);
}

std::pair<double, double> BoneCanvas::screen_to_world(double sx,
                                                      double sy) const {
    const QRect rect = contentsRect();
    const double cx = rect.center().x();
    const double cy = rect.center().y();
    const double wx = (sx - cx) / zoom_ + pan_.x();
    const double wy = (sy - cy) / zoom_ + pan_.y();
    return {wx, wy};
}

std::vector<std::pair<double, double>> BoneCanvas::all_projected_points()
    const {
    std::vector<std::pair<double, double>> pts;
    for (const OptPos3& p : bone_pos_) {
        const auto wp = project(p);
        if (wp) pts.push_back(*wp);
    }
    for (const OptPos3& p : joint_pos_) {
        const auto wp = project(p);
        if (wp) pts.push_back(*wp);
    }
    return pts;
}

// ── helpers ──

std::optional<double> BoneCanvas::rest_delta(int joint_idx,
                                             int bone_idx) const {
    if (!(0 <= joint_idx && joint_idx < int(joint_pos_.size())))
        return std::nullopt;
    if (!(0 <= bone_idx && bone_idx < int(bone_pos_.size())))
        return std::nullopt;
    const OptPos3& jp = joint_pos_[size_t(joint_idx)];
    const OptPos3& bp = bone_pos_[size_t(bone_idx)];
    if (!jp.first || !bp.first) return std::nullopt;
    return std::sqrt(std::pow(jp.second[0] - bp.second[0], 2)
                     + std::pow(jp.second[1] - bp.second[1], 2)
                     + std::pow(jp.second[2] - bp.second[2], 2));
}

double BoneCanvas::bone_incoming_share(int bi) const {
    double total = 0.0;
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& a = actions_[size_t(j)];
        if (a.action != "map" || !a.target.has_value()) continue;
        if (*a.target != bi) continue;
        if (0 <= j && j < int(joint_stats_.size()))
            total += joint_stats_[size_t(j)].weight_share;
    }
    return total;
}

QColor BoneCanvas::arc_color(const std::optional<double>& delta) const {
    const ViewPalette& pal = view_palette();
    if (!delta.has_value()) return pal.unmap;
    if (*delta < DELTA_GREEN) return pal.arc_green;
    if (*delta < DELTA_COPPER) return pal.arc_copper;
    return pal.arc_red;
}

// ── painting ──

void BoneCanvas::paintEvent(QPaintEvent*) {
    const ViewPalette& pal = view_palette();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect rect = contentsRect();
    painter.fillRect(rect, pal.bg);
    draw_grid(painter, rect);

    if (bone_pos_.empty() && joint_pos_.empty()) {
        painter.setPen(pal.empty);
        QFont f;
        f.setPointSize(10);
        painter.setFont(f);
        painter.drawText(rect, Qt::AlignCenter,
                         tr("No skeleton / joint data to render."));
        painter.end();
        return;
    }

    // Layer 1 — mapping arcs first so the markers paint over them.
    draw_mapping_arcs(painter);
    // Layer 2 — orig bones.
    draw_bones(painter);
    // Layer 3 — GLB joints (incl. Drop X markers).
    draw_joints(painter);
    // Layer 4 — selection halo.
    draw_selection(painter);
    // Layer 5 — corner overlay with view info.
    draw_overlay(painter, rect);
    painter.end();
}

void BoneCanvas::draw_grid(QPainter& painter, const QRect& rect) {
    const ViewPalette& pal = view_palette();
    painter.setPen(QPen(pal.grid, 1, Qt::DotLine));
    // Compute a step that gives roughly 6 grid lines.
    double step;
    if (zoom_ > 0) {
        const double world_span_x = rect.width() / zoom_;
        const double world_span_y = rect.height() / zoom_;
        step = nice_step(std::max(world_span_x, world_span_y) / 6.0);
    } else {
        step = 1.0;
    }
    // Start at the nearest step below pan-x.
    const auto [wx_left, wy_top] = screen_to_world(rect.left(), rect.top());
    const auto [wx_right, wy_bot] =
        screen_to_world(rect.right(), rect.bottom());
    double x = step * (std::floor(wx_left / step) - 1);
    while (x <= wx_right + step) {
        const QPointF sp = world_to_screen(x, 0);
        painter.drawLine(QPointF(sp.x(), rect.top()),
                         QPointF(sp.x(), rect.bottom()));
        x += step;
    }
    double y = step * (std::floor(wy_top / step) - 1);
    while (y <= wy_bot + step) {
        const QPointF sp = world_to_screen(0, y);
        painter.drawLine(QPointF(rect.left(), sp.y()),
                         QPointF(rect.right(), sp.y()));
        y += step;
    }
    // Axes through (0,0).
    painter.setPen(QPen(pal.axes, 1));
    const QPointF origin = world_to_screen(0, 0);
    painter.drawLine(QPointF(rect.left(), origin.y()),
                     QPointF(rect.right(), origin.y()));
    painter.drawLine(QPointF(origin.x(), rect.top()),
                     QPointF(origin.x(), rect.bottom()));
}

void BoneCanvas::draw_mapping_arcs(QPainter& painter) {
    const ViewPalette& pal = view_palette();
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& a = actions_[size_t(j)];
        if (!(0 <= j && j < int(joint_pos_.size()))) continue;
        const auto jp = project(joint_pos_[size_t(j)]);
        if (!jp) continue;
        const QPointF js = world_to_screen(jp->first, jp->second);
        if (a.action == "map" && a.target.has_value()) {
            const int bi = *a.target;
            if (!(0 <= bi && bi < int(bone_pos_.size()))) continue;
            const auto bp = project(bone_pos_[size_t(bi)]);
            if (!bp) continue;
            const QPointF bs = world_to_screen(bp->first, bp->second);
            const std::optional<double> delta = rest_delta(j, bi);
            const QColor col = arc_color(delta);
            // Thicker stroke for higher-weight mappings.
            double w = 1.0;
            if (0 <= j && j < int(joint_stats_.size()))
                w += std::min(3.0,
                              joint_stats_[size_t(j)].weight_share * 8.0);
            painter.setPen(QPen(col, w));
            painter.drawLine(js, bs);
        } else if (a.action == "drop") {
            // Small red X over the joint position to mark Drop.
            painter.setPen(QPen(pal.drop, 2));
            const double r = 6;
            painter.drawLine(QPointF(js.x() - r, js.y() - r),
                             QPointF(js.x() + r, js.y() + r));
            painter.drawLine(QPointF(js.x() - r, js.y() + r),
                             QPointF(js.x() + r, js.y() - r));
        }
        // action == 'unmapped': arc drawn as faint dashed line to
        // the rigid-bind bone — so the modder sees where the weight
        // will pile.
        else if (a.action == "unmapped") {
            if (0 <= rb_bone_ && rb_bone_ < int(bone_pos_.size())) {
                const auto bp = project(bone_pos_[size_t(rb_bone_)]);
                if (bp) {
                    const QPointF bs = world_to_screen(bp->first, bp->second);
                    QPen pen(pal.unmap, 1, Qt::DashLine);
                    painter.setPen(pen);
                    painter.drawLine(js, bs);
                }
            }
        }
    }
}

void BoneCanvas::draw_bones(QPainter& painter) {
    const ViewPalette& pal = view_palette();
    for (int bi = 0; bi < int(bone_pos_.size()); ++bi) {
        const auto wp = project(bone_pos_[size_t(bi)]);
        if (!wp) continue;
        const QPointF sp = world_to_screen(wp->first, wp->second);
        const double incoming = bone_incoming_share(bi);
        // Size: 5 for idle, grows with incoming weight share.
        double r = 5 + incoming * 18.0;
        r = std::max(4.0, std::min(r, 16.0));
        const QColor color = incoming > 1e-4 ? pal.bone : pal.bone_idle;
        painter.setBrush(QBrush(color));
        painter.setPen(QPen(color.darker(140), 1));
        painter.drawEllipse(sp, r, r);
    }
}

void BoneCanvas::draw_joints(QPainter& painter) {
    const ViewPalette& pal = view_palette();
    for (int j = 0; j < int(joint_pos_.size()); ++j) {
        const auto wp = project(joint_pos_[size_t(j)]);
        if (!wp) continue;
        const QPointF sp = world_to_screen(wp->first, wp->second);
        const std::string action = (0 <= j && j < int(actions_.size()))
                                       ? actions_[size_t(j)].action
                                       : std::string();
        const double share = (0 <= j && j < int(joint_stats_.size()))
                                 ? joint_stats_[size_t(j)].weight_share
                                 : 0.0;
        double r = 4 + share * 14.0;
        r = std::max(3.5, std::min(r, 12.0));
        if (action == "unmapped") {
            // Hollow grey ring to mark unmapped.
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(pal.unmap, 1.5));
        } else if (action == "drop") {
            // Same orange diamond as Map, the X is drawn by
            // draw_mapping_arcs on top so the modder still sees
            // WHERE the dropped joint sits.
            painter.setBrush(QBrush(pal.joint.lighter(160)));
            painter.setPen(QPen(pal.joint.darker(140), 1));
        } else {
            painter.setBrush(QBrush(pal.joint));
            painter.setPen(QPen(pal.joint.darker(140), 1));
        }
        painter.drawPolygon(diamond(sp, r));
    }
}

void BoneCanvas::draw_selection(QPainter& painter) {
    const ViewPalette& pal = view_palette();
    // Selected bone — yellow ring.
    if (0 <= selected_bone_ && selected_bone_ < int(bone_pos_.size())) {
        const auto wp = project(bone_pos_[size_t(selected_bone_)]);
        if (wp) {
            const QPointF sp = world_to_screen(wp->first, wp->second);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(pal.select_halo, 3));
            painter.drawEllipse(sp, 18, 18);
            draw_label(painter, sp,
                       selected_bone_ < bone_names_.size()
                           ? bone_names_.at(selected_bone_)
                           : QStringLiteral("bone_%1").arg(selected_bone_));
        }
    }
    if (0 <= selected_joint_ && selected_joint_ < int(joint_pos_.size())) {
        const auto wp = project(joint_pos_[size_t(selected_joint_)]);
        if (wp) {
            const QPointF sp = world_to_screen(wp->first, wp->second);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(pal.select_halo, 3));
            painter.drawEllipse(sp, 18, 18);
            draw_label(painter, sp,
                       selected_joint_ < joint_names_.size()
                           ? joint_names_.at(selected_joint_)
                           : QStringLiteral("joint_%1").arg(selected_joint_));
        }
    }
}

void BoneCanvas::draw_label(QPainter& painter, const QPointF& sp,
                            const QString& text) {
    const ViewPalette& pal = view_palette();
    painter.setPen(pal.label_fg);
    QFont f;
    f.setPointSize(8);
    painter.setFont(f);
    const QFontMetrics fm(f);
    const double tw = fm.horizontalAdvance(text);
    const double th = fm.height();
    const QRectF bg(sp.x() + 14, sp.y() - th / 2 - 1, tw + 6, th + 2);
    painter.fillRect(bg, pal.label_bg);
    painter.setPen(pal.label_fg);
    painter.drawText(QPointF(bg.left() + 3, bg.bottom() - 4), text);
}

void BoneCanvas::draw_overlay(QPainter& painter, const QRect& rect) {
    // Counts in the top-right corner.
    const int n_bones = int(bone_pos_.size());
    const int n_joints = int(joint_pos_.size());
    int n_map = 0, n_drop = 0, n_unmap = 0;
    for (const JointAction& a : actions_) {
        if (a.action == "map") ++n_map;
        if (a.action == "drop") ++n_drop;
        if (a.action == "unmapped") ++n_unmap;
    }
    int bad = 0;
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& a = actions_[size_t(j)];
        if (a.action != "map" || !a.target.has_value()) continue;
        const std::optional<double> d = rest_delta(j, *a.target);
        if (d.has_value() && *d >= DELTA_COPPER) ++bad;
    }
    QString txt = QStringLiteral("%1 bones · %2 joints   |   "
                                 "map: %3  drop: %4  unmap: %5")
                      .arg(n_bones)
                      .arg(n_joints)
                      .arg(n_map)
                      .arg(n_drop)
                      .arg(n_unmap);
    if (bad) txt += QStringLiteral("   ⚠ %1 mismatch(es)").arg(bad);
    painter.setPen(view_palette().overlay);
    QFont f;
    f.setPointSize(8);
    painter.setFont(f);
    painter.drawText(rect.adjusted(8, 4, -8, -4),
                     Qt::AlignTop | Qt::AlignLeft, txt);
}

// ── interaction ──

void BoneCanvas::wheelEvent(QWheelEvent* ev) {
    // Zoom around the cursor position.
    const int delta = ev->angleDelta().y();
    if (delta == 0) return;
    double factor = 1.0 + (delta / 1200.0);
    factor = std::max(0.2, std::min(factor, 5.0));
    // Keep the point under the cursor stationary in world coords.
    const auto before = screen_to_world(ev->position().x(),
                                        ev->position().y());
    zoom_ *= factor;
    zoom_ = std::max(0.01, std::min(zoom_, 10000.0));
    const auto after = screen_to_world(ev->position().x(),
                                       ev->position().y());
    pan_ = QPointF(pan_.x() + (before.first - after.first),
                   pan_.y() + (before.second - after.second));
    update();
}

void BoneCanvas::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() == PAN_BUTTON) {
        const std::optional<Hit> hit =
            hit_test(ev->position().x(), ev->position().y());
        if (!hit.has_value()) {
            dragging_ = true;
            drag_start_ = ev->position();
            has_drag_start_ = true;
            setCursor(Qt::ClosedHandCursor);
        } else {
            if (hit->is_bone) {
                selected_bone_ = hit->idx;
                selected_joint_ = -1;
                emit bone_clicked(hit->idx);
            } else {
                selected_joint_ = hit->idx;
                selected_bone_ = -1;
                emit joint_clicked(hit->idx);
            }
            update();
        }
    }
}

void BoneCanvas::mouseMoveEvent(QMouseEvent* ev) {
    if (dragging_ && has_drag_start_) {
        const double dx = (ev->position().x() - drag_start_.x())
                          / std::max(zoom_, 1e-6);
        const double dy = (ev->position().y() - drag_start_.y())
                          / std::max(zoom_, 1e-6);
        pan_ = QPointF(pan_.x() - dx, pan_.y() - dy);
        drag_start_ = ev->position();
        update();
    }
}

void BoneCanvas::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() == PAN_BUTTON) {
        dragging_ = false;
        has_drag_start_ = false;
        setCursor(Qt::ArrowCursor);
    }
}

// Return ('bone'|'joint', idx) under (sx, sy), else None.
std::optional<BoneCanvas::Hit> BoneCanvas::hit_test(double sx,
                                                    double sy) const {
    std::optional<Hit> best;
    double best_d = 18.0;  // px tolerance
    for (int bi = 0; bi < int(bone_pos_.size()); ++bi) {
        const auto wp = project(bone_pos_[size_t(bi)]);
        if (!wp) continue;
        const QPointF ps = world_to_screen(wp->first, wp->second);
        const double d = std::sqrt(std::pow(ps.x() - sx, 2)
                                   + std::pow(ps.y() - sy, 2));
        if (d < best_d) {
            best_d = d;
            best = Hit{true, bi};
        }
    }
    for (int j = 0; j < int(joint_pos_.size()); ++j) {
        const auto wp = project(joint_pos_[size_t(j)]);
        if (!wp) continue;
        const QPointF ps = world_to_screen(wp->first, wp->second);
        const double d = std::sqrt(std::pow(ps.x() - sx, 2)
                                   + std::pow(ps.y() - sy, 2));
        if (d < best_d) {
            best_d = d;
            best = Hit{false, j};
        }
    }
    return best;
}
