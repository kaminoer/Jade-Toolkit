// BoneRemapDialog.hpp — bone-remapping dialog for foreign-mesh imports
// (port of gui/bone_remap_dialog.py) — Tier 2 dual-pane editor.
//
// When a mesh swap brings in a model rigged to a different skeleton, each
// GLB joint must be told what to do with the weights it carries. This
// modal shows two synchronised tables:
//
//   * Left  — GLB joints. One row per imported joint with an Action picker
//     (Map / Drop / Unmapped) and a target-bone picker. Coverage columns
//     show how many vertices the joint touches and its share of mesh
//     weight, so the modder knows which mappings actually matter.
//   * Right — Original bones. One row per target-skeleton bone, showing
//     which joints currently feed it and the rolled-up coverage.
//
// Selecting a row on either side highlights the linked rows on the other.
//
// Persistence model (lines up with ReplaceMesh): bone_map {joint -> bone}
// for Map rows; bone_map_source (user/name/geometric); bone_drops;
// drop_targets {joint -> orig bone} for Drop rows with a specific rigid
// target. Unmapped joints are deliberately NOT persisted, so they fall
// through to the build's "no entry" branch → rigid-bound to bone 0.
//
// See [[user-prefers-deterministic-over-heuristic]] for why decisions are
// saved as concrete (joint, target) pairs rather than re-derived at build.
#pragma once

#include <QDialog>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "BoneMapView.hpp"

class QCheckBox;
class QComboBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QTextEdit;

// Everything the dialog needs — the Python __init__'s keyword bundle.
// Populated from jade::patcher::AnalyzeBoneResult plus any persisted op
// state when re-opening an existing ReplaceMesh op.
struct BoneRemapInit {
    QStringList glb_joint_names;
    QStringList dp_bone_names;
    std::map<int, int> auto_map;
    std::map<int, std::string> auto_map_source;
    std::vector<jade::patcher::JointStats> joint_stats;
    std::vector<OptPos3> joint_centroids;
    std::vector<OptPos3> bone_centroids;
    double mesh_diagonal = 0.0;
    std::vector<OptPos3> joint_rest_positions;
    std::vector<OptPos3> bone_rest_positions;
    std::vector<double> orig_bone_weight_shares;
    std::map<int, int> initial_map;
    std::map<int, std::string> initial_source;
    std::set<int> initial_drops;
    int initial_rigid_bind_bone = 0;
    bool initial_auto_rig = false;
    bool initial_diagnose_rest_pose = false;
    std::map<int, int> initial_drop_targets;
};

// The full dialog state after Accepted (the Python result() tuple).
struct BoneRemapResult {
    std::map<int, int> bone_map;               // Action=Map rows
    std::map<int, std::string> source_map;     // provenance per Map row
    std::set<int> drops;                       // Action=Drop rows
    int rigid_bind_bone = 0;                   // global fallback bone
    bool auto_rig = false;
    bool diagnose_rest_pose = false;
    std::map<int, int> drop_targets;           // Drop rows with a target
};

// Dual-pane bone-mapping editor.
class BoneRemapDialog : public QDialog {
    Q_OBJECT
public:
    explicit BoneRemapDialog(const BoneRemapInit& init,
                             QWidget* parent = nullptr);

    // Full dialog state (bone_map, source_map, drops, rigid_bind_bone,
    // auto_rig, diagnose_rest_pose, drop_targets).
    BoneRemapResult result() const;
    // True if the modder accepted via "Keep original skinning".
    bool keep_original_skin() const { return keep_original_skin_; }

private slots:
    void on_bulk_drop();
    void on_action_changed(int joint);
    void on_target_changed(int joint);
    void refresh_diagnostics();
    void refresh_issue_panel();
    void refresh_view();
    void on_view_bone_clicked(int bi);
    void on_view_joint_clicked(int j);
    void on_left_selected();
    void on_right_selected();
    void apply_filter();
    void on_apply_preset();
    void on_save_preset();
    void on_delete_preset();
    void fold_unmapped_to_nearest();
    void reset_all_to_auto();
    void on_keep_original_skin();

private:
    QWidget* build_left_pane();
    QWidget* build_right_pane();
    std::vector<int> selected_joint_rows() const;
    void refresh_left();
    void refresh_row_visuals(int j);
    void refresh_right();
    int current_rigid_bind_bone() const;
    // [(severity, html_message), …] for the current state.
    std::vector<std::pair<QString, QString>> compute_live_issues() const;
    void set_left_row_highlight(int j, bool on);
    void set_right_row_highlight(int bi, bool on);
    void clear_left_link_highlight();
    void clear_right_link_highlight();
    // "user" | "name" | "geometric" | "drop" | "unmapped".
    std::string current_source(int j) const;
    void reload_preset_combo();

    QStringList glb_names_;
    QStringList dp_names_;
    std::vector<jade::patcher::JointStats> joint_stats_;
    std::vector<OptPos3> joint_centroids_;
    std::vector<OptPos3> bone_centroids_;
    std::vector<OptPos3> joint_rest_;
    std::vector<OptPos3> bone_rest_;
    std::vector<double> orig_shares_;
    double mesh_diag_ = 0.0;
    std::map<int, int> auto_map_;
    std::map<int, std::string> auto_source_;
    std::map<int, std::string> initial_source_;

    // Per-joint action model ({'action', 'target'} per GLB joint).
    std::vector<JointAction> actions_;
    // T3.1: joints whose current Map target came from the in-dialog fold
    // algorithm (source reports 'geometric' for them).
    std::map<int, int> fold_picks_;

    bool keep_original_skin_ = false;

    QTextEdit* issue_panel_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* preset_combo_ = nullptr;
    BoneMapView* view_ = nullptr;
    QComboBox* rb_combo_ = nullptr;
    QCheckBox* auto_rig_cb_ = nullptr;
    QCheckBox* diag_rest_cb_ = nullptr;
    QLabel* diag_ = nullptr;
    QTableWidget* left_ = nullptr;
    QTableWidget* right_ = nullptr;
    QComboBox* bulk_target_ = nullptr;
    std::vector<QComboBox*> action_combos_;
    std::vector<QComboBox*> target_combos_;
};
