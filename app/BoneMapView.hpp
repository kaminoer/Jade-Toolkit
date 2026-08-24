// BoneMapView.hpp — 2D orthographic viewer for bone remapping outcome
// (port of gui/bone_map_view.py) — Phase 5.
//
// Pairs the Bone Remap dialog's tabular view with a visual one: orig
// skeleton bones and GLB joints rendered in their rest positions, with
// mapping arcs colour-coded by severity. Designed to make rest-pose
// mismatches and miswiring obvious at a glance:
//
//   * Blue dots         — original skeleton bones (size scales with the
//                         bone's current incoming weight share).
//   * Orange diamonds   — GLB joints (size scales with the joint's
//                         weight share in the new mesh).
//   * Green arcs        — Map with rest delta < 0.1 (well aligned).
//   * Copper arcs       — Map with rest delta 0.1–0.5 (caution).
//   * Red arcs          — Map with rest delta ≥ 0.5 (mismatch).
//   * Red X marker      — Drop (weights renormalised away).
//   * Grey ring         — Unmapped joint (will rigid-bind to fallback).
//   * Yellow halo       — current selection (synced from the dialog).
//
// Uses QPainter on a plain QWidget so the dialog stays Qt-only (no
// OpenGL dependency). The view is orthographic — pick Front / Side / Top
// from the toolbar at the top of the widget. Mouse drag pans, wheel
// zooms, click selects nearest bone or joint.
#pragma once

#include <QColor>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QWidget>
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "jade/PatcherModel.hpp"

class QComboBox;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QWheelEvent;

// One per-joint action entry — the Python model dict
// {'action': 'map'|'drop'|'unmapped', 'target': int|None}.
struct JointAction {
    std::string        action = "unmapped";
    std::optional<int> target;  // orig bone idx (nullopt == Python None)
};

// A possibly-absent 3-position — same (has, xyz) shape as
// jade::patcher::AnalyzeBoneResult (a Python None has .first == false).
using OptPos3 = std::pair<bool, std::array<double, 3>>;

// The keyword bundle BoneMapView::set_data receives from the dialog.
struct BoneMapData {
    std::vector<OptPos3>                   bone_positions;
    std::vector<OptPos3>                   joint_positions;
    QStringList                            bone_names;
    QStringList                            joint_names;
    std::vector<JointAction>               actions;
    std::vector<jade::patcher::JointStats> joint_stats;
    std::vector<double>                    orig_bone_weight_shares;
    int    rigid_bind_bone = 0;
    double mesh_diagonal = 0.0;
};

class BoneCanvas;

// Container holding the projection picker, legend, and canvas.
class BoneMapView : public QWidget {
    Q_OBJECT
public:
    explicit BoneMapView(QWidget* parent = nullptr);

    // ── data API: the dialog calls this on every state change ──
    void set_data(const BoneMapData& data);
    void set_selection(std::optional<int> bone = std::nullopt,
                       std::optional<int> joint = std::nullopt);

signals:
    void bone_clicked(int idx);   // orig bone idx
    void joint_clicked(int idx);  // GLB joint idx

private slots:
    void on_view_changed();

private:
    QComboBox*  view_combo_ = nullptr;
    BoneCanvas* canvas_ = nullptr;
};

// Paint-only canvas. Owns its projection, pan/zoom, and selection
// (_BoneCanvas).
class BoneCanvas : public QWidget {
    Q_OBJECT
public:
    explicit BoneCanvas(QWidget* parent = nullptr);

    // ── data API ──
    void update_data(const BoneMapData& data);
    void set_view_axis(const QString& axis);
    void set_selection(std::optional<int> bone = std::nullopt,
                       std::optional<int> joint = std::nullopt);

public slots:
    // Auto-fit the current data into the widget rect with margin.
    void reset_view();

signals:
    void bone_clicked(int idx);
    void joint_clicked(int idx);

protected:
    void paintEvent(QPaintEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;

private:
    // ── projection ──
    std::optional<std::pair<double, double>> project(const OptPos3& p3) const;
    QPointF world_to_screen(double wx, double wy) const;
    std::pair<double, double> screen_to_world(double sx, double sy) const;
    std::vector<std::pair<double, double>> all_projected_points() const;

    // ── helpers ──
    std::optional<double> rest_delta(int joint_idx, int bone_idx) const;
    double bone_incoming_share(int bi) const;
    QColor arc_color(const std::optional<double>& delta) const;

    // ── painting ──
    void draw_grid(QPainter& painter, const QRect& rect);
    void draw_mapping_arcs(QPainter& painter);
    void draw_bones(QPainter& painter);
    void draw_joints(QPainter& painter);
    void draw_selection(QPainter& painter);
    void draw_label(QPainter& painter, const QPointF& sp, const QString& text);
    void draw_overlay(QPainter& painter, const QRect& rect);

    // ── interaction ──
    // ('bone'|'joint', idx) under (sx, sy), else nullopt (_hit_test).
    struct Hit {
        bool is_bone = false;
        int  idx = 0;
    };
    std::optional<Hit> hit_test(double sx, double sy) const;

    std::vector<OptPos3>                   bone_pos_;
    std::vector<OptPos3>                   joint_pos_;
    QStringList                            bone_names_;
    QStringList                            joint_names_;
    std::vector<JointAction>               actions_;
    std::vector<jade::patcher::JointStats> joint_stats_;
    std::vector<double>                    bone_old_shares_;
    int    rb_bone_ = 0;
    double mesh_diag_ = 0.0;

    QString view_axis_ = QStringLiteral("front");
    double  zoom_ = 1.0;
    QPointF pan_{0, 0};
    bool    dragging_ = false;
    bool    has_drag_start_ = false;  // Python: _drag_start is None
    QPointF drag_start_;
    bool    auto_fit_needed_ = true;

    int selected_bone_ = -1;
    int selected_joint_ = -1;
};
