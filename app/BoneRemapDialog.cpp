#include "BoneRemapDialog.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QSaveFile>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

#include "jade/Json.hpp"
#include "jade/PatcherModel.hpp"

#include "GuiUtil.hpp"

namespace {

// ── theme-aware palette ──
//
// The pale-yellow link highlight + saturated-light source tag colours
// work on a light-theme app palette but collide on dark themes. Pick
// colours from one of two palettes based on the active app palette's
// window luminance. Detection happens on first access and is cached.
struct ThemeColors {
    std::map<std::string, QString> src;
    QString sev_red, sev_copper, sev_green, sev_grey, sev_dark;
    QColor link_bg, link_fg_force, text;
    QString issue_error, issue_warn, issue_info, issue_ok, panel_bg;
};

const ThemeColors LIGHT_THEME = {
    {{"user", "#9d4edd"},       // violet
     {"name", "#2e8b57"},       // sea green
     {"geometric", "#1e88e5"},  // blue
     {"drop", "#b22222"},       // firebrick
     {"unmapped", "#777777"}},  // grey
    "#b22222", "#b87333", "#2e8b57", "#888888", "#444444",
    QColor("#fff0c0"), QColor("#222222"), QColor("#222222"),
    "#b22222", "#b87333", "#555555", "#2e8b57", "#fafafa",
};

const ThemeColors DARK_THEME = {
    // All bumped toward higher value/lightness so they read on the dark
    // Qt palette window colour.
    {{"user", "#d4a4ff"},
     {"name", "#7fdba6"},
     {"geometric", "#6fb5f5"},
     {"drop", "#f88080"},
     {"unmapped", "#bbbbbb"}},
    "#ff7878", "#e0a878", "#7fdba6", "#aaaaaa", "#cccccc",
    // Subtle warm-amber tint that stands out on dark backgrounds without
    // washing out the per-row foreground.
    QColor(140, 100, 40, 140), QColor("#ffffff"), QColor("#e0e0e0"),
    "#ff7878", "#e0a878", "#cccccc", "#7fdba6", "#2b2b2b",
};

const ThemeColors& theme_colors() {
    static const ThemeColors* cache = [] {
        const ThemeColors* palette = &LIGHT_THEME;
        if (QApplication::instance()) {
            const QColor bg =
                QApplication::palette().color(QPalette::Window);
            const double lum = 0.299 * bg.red() + 0.587 * bg.green()
                               + 0.114 * bg.blue();
            if (lum < 128) palette = &DARK_THEME;
        }
        return palette;
    }();
    return *cache;
}

const char* const ACTIONS[] = {"map", "drop", "unmapped"};

QString action_label(const std::string& a) {
    if (a == "map") return QStringLiteral("Map");
    if (a == "drop") return QStringLiteral("Drop");
    return QStringLiteral("Unmapped");
}

QString src_label(const std::string& s) {
    if (s == "geometric") return QStringLiteral("geom");
    if (s == "drop") return QStringLiteral("drop");
    if (s == "unmapped") return QStringLiteral("unmap");
    return qs(s);  // user / name pass through
}

// Build a non-editable table cell (_ro).
QTableWidgetItem* ro(const QString& text, bool align_right = false) {
    auto* item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (align_right)
        item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

// ── presets (io_ops/patcher.py _load_presets/_save_presets) ──
// {preset name -> {normalised joint name -> normalised bone name}} at
// ~/.jade_explorer/bone_remap_presets.json.

QString presets_path() {
    return QDir::home().filePath(
        QStringLiteral(".jade_explorer/bone_remap_presets.json"));
}

std::map<QString, std::map<QString, QString>> load_presets() {
    std::map<QString, std::map<QString, QString>> out;
    QFile f(presets_path());
    if (!f.open(QIODevice::ReadOnly)) return out;
    const QByteArray raw = f.readAll();
    try {
        const jade::json::Value v =
            jade::json::parse(raw.constData(), size_t(raw.size()));
        if (!v.is_obj()) return out;
        for (const auto& [name, mapping] : v.obj) {
            if (!mapping.is_obj()) continue;
            std::map<QString, QString>& m = out[qs(name)];
            for (const auto& [jn, bn] : mapping.obj)
                if (bn.is_str()) m[qs(jn)] = qs(bn.str);
        }
    } catch (const std::exception&) {
    }
    return out;
}

bool save_presets(
    const std::map<QString, std::map<QString, QString>>& presets) {
    jade::json::Value root = jade::json::make_obj();
    for (const auto& [name, mapping] : presets) {
        jade::json::Value m = jade::json::make_obj();
        for (const auto& [jn, bn] : mapping)
            m.obj[jn.toStdString()] = jade::json::make_str(bn.toStdString());
        root.obj[name.toStdString()] = std::move(m);
    }
    QDir().mkpath(QDir::home().filePath(QStringLiteral(".jade_explorer")));
    const std::string text = jade::json::dump(root, 2) + "\n";
    QSaveFile f(presets_path());
    if (!f.open(QIODevice::WriteOnly)) return false;
    if (f.write(text.data(), qint64(text.size())) != qint64(text.size()))
        return false;
    return f.commit();
}

// normalise_bone_name (through the user's alias map).
QString norm(const QString& s) {
    return qs(jade::patcher::norm_bone_name(s.toStdString()));
}

// ── live issue panel helpers ──

QString issue_glyph(const QString& severity) {
    if (severity == QLatin1String("error")) return QStringLiteral("✖");
    if (severity == QLatin1String("warn")) return QStringLiteral("⚠");
    if (severity == QLatin1String("ok")) return QStringLiteral("✓");
    return QStringLiteral("ℹ");
}

QString issue_color(const QString& severity) {
    const ThemeColors& theme = theme_colors();
    if (severity == QLatin1String("error")) return theme.issue_error;
    if (severity == QLatin1String("warn")) return theme.issue_warn;
    if (severity == QLatin1String("ok")) return theme.issue_ok;
    return theme.issue_info;
}

// Render one issue row as HTML for the issue panel (_issue_row).
QString issue_row(const QString& severity, const QString& html_message) {
    return QStringLiteral(
               "<div style='margin:1px 0'>"
               "<span style='color:%1; font-weight:bold'>%2</span> "
               "<span style='color:%1'>%3</span>"
               "</div>")
        .arg(issue_color(severity), issue_glyph(severity), html_message);
}

// HTML-escape a plain-text fragment for inline use in issue rows (_esc).
QString esc(const QString& s) { return s.toHtmlEscaped(); }

double dist3(const std::array<double, 3>& a, const std::array<double, 3>& b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0])
                     + (a[1] - b[1]) * (a[1] - b[1])
                     + (a[2] - b[2]) * (a[2] - b[2]));
}

}  // namespace

BoneRemapDialog::BoneRemapDialog(const BoneRemapInit& init, QWidget* parent)
    : QDialog(parent),
      glb_names_(init.glb_joint_names),
      dp_names_(init.dp_bone_names),
      joint_stats_(init.joint_stats),
      joint_centroids_(init.joint_centroids),
      bone_centroids_(init.bone_centroids),
      joint_rest_(init.joint_rest_positions),
      bone_rest_(init.bone_rest_positions),
      orig_shares_(init.orig_bone_weight_shares),
      // Guard against zero / negative diagonals — divisor in the bind-
      // dist percentage.
      mesh_diag_(init.mesh_diagonal > 0 ? init.mesh_diagonal : 0.0),
      auto_map_(init.auto_map),
      auto_source_(init.auto_map_source),
      initial_source_(init.initial_source) {
    setWindowTitle(tr("Bone remapping — imported mesh"));
    resize(1480, 760);

    // ── Per-joint action model. Built from persisted state (re-open) or
    // auto-map (fresh use).
    const bool have_initial =
        !init.initial_map.empty() || !init.initial_drops.empty();
    for (int j = 0; j < glb_names_.size(); ++j) {
        JointAction act;
        auto imap = init.initial_map.find(j);
        if (init.initial_drops.count(j)) {
            // Restore the per-drop rigid target if the op carried one;
            // nullopt falls through to the global fallback at build time.
            act.action = "drop";
            auto dt = init.initial_drop_targets.find(j);
            if (dt != init.initial_drop_targets.end()) act.target = dt->second;
        } else if (imap != init.initial_map.end()) {
            act.action = "map";
            act.target = imap->second;
        } else if (have_initial) {
            // Re-open of an existing op: joints not in the persisted state
            // were explicitly omitted, treat as Unmapped.
            act.action = "unmapped";
        } else if (auto_map_.count(j)) {
            act.action = "map";
            act.target = auto_map_.at(j);
        } else {
            act.action = "unmapped";
        }
        actions_.push_back(act);
    }

    // ── Layout
    auto* root = new QVBoxLayout(this);

    // Live issue panel — re-checks the current mapping on every change so
    // the modder sees impact without having to read the build log first.
    issue_panel_ = new QTextEdit();
    issue_panel_->setReadOnly(true);
    issue_panel_->setMinimumHeight(90);
    issue_panel_->setMaximumHeight(170);
    issue_panel_->setStyleSheet(
        QStringLiteral("QTextEdit { background-color: %1; }")
            .arg(theme_colors().panel_bg));
    root->addWidget(issue_panel_);

    // Search field — filters both tables.
    auto* search_row = new QHBoxLayout();
    search_row->addWidget(new QLabel(tr("Filter:")));
    search_ = new QLineEdit();
    search_->setPlaceholderText(tr(
        "Type to filter by joint name, action, target bone, or bone name…"));
    connect(search_, &QLineEdit::textChanged, this,
            &BoneRemapDialog::apply_filter);
    search_row->addWidget(search_, 1);
    root->addLayout(search_row);

    // T2.4 — presets row. Saves/loads mappings keyed by normalised bone
    // names so a preset built from one character carries over to any
    // sibling that follows the same rig convention.
    auto* preset_row = new QHBoxLayout();
    preset_row->addWidget(new QLabel(tr("Preset:")));
    preset_combo_ = new QComboBox();
    preset_combo_->setMinimumWidth(220);
    preset_row->addWidget(preset_combo_, 1);
    auto* apply_btn = new QPushButton(tr("Apply"));
    apply_btn->setToolTip(
        tr("Apply the selected preset: for each GLB joint, look up its "
           "normalised name in the preset; if a target bone exists in this "
           "skeleton with the matching normalised name, set the joint's "
           "Action to Map and target it. Joints with no preset entry are "
           "left untouched."));
    connect(apply_btn, &QPushButton::clicked, this,
            &BoneRemapDialog::on_apply_preset);
    preset_row->addWidget(apply_btn);
    auto* save_btn = new QPushButton(tr("Save as…"));
    save_btn->setToolTip(
        tr("Save the current Map decisions as a named preset. Stored at "
           "~/.jade_explorer/bone_remap_presets.json. Drops and Unmapped "
           "joints are not persisted — only joint-to-bone routings."));
    connect(save_btn, &QPushButton::clicked, this,
            &BoneRemapDialog::on_save_preset);
    preset_row->addWidget(save_btn);
    auto* del_btn = new QPushButton(tr("Delete"));
    del_btn->setToolTip(
        tr("Remove the selected preset from the global presets file."));
    connect(del_btn, &QPushButton::clicked, this,
            &BoneRemapDialog::on_delete_preset);
    preset_row->addWidget(del_btn);
    root->addLayout(preset_row);
    reload_preset_combo();

    // ── Three-pane splitter: joints | bones | 3D outcome view. The
    // viewer is collapsible — drag its handle closed for tables only.
    auto* split = new QSplitter(Qt::Horizontal);
    split->addWidget(build_left_pane());
    split->addWidget(build_right_pane());
    view_ = new BoneMapView();
    connect(view_, &BoneMapView::bone_clicked, this,
            &BoneRemapDialog::on_view_bone_clicked);
    connect(view_, &BoneMapView::joint_clicked, this,
            &BoneRemapDialog::on_view_joint_clicked);
    split->addWidget(view_);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);
    split->setStretchFactor(2, 3);
    split->setCollapsible(2, true);
    root->addWidget(split, 1);

    // T3.2 / T3.3 — fallback bone + auto-rig affordances.
    auto* opts_row = new QHBoxLayout();
    opts_row->addWidget(new QLabel(tr("Rigid-bind fallback bone:")));
    rb_combo_ = new QComboBox();
    for (int bi = 0; bi < dp_names_.size(); ++bi)
        rb_combo_->addItem(QStringLiteral("%1  %2")
                               .arg(bi, 3)
                               .arg(dp_names_.at(bi)),
                           bi);
    const int ri = rb_combo_->findData(init.initial_rigid_bind_bone);
    rb_combo_->setCurrentIndex(std::max(0, ri));
    rb_combo_->setToolTip(
        tr("The GLOBAL fallback bone: skinless GLB, Unmapped joints, and "
           "any Dropped joint that has no per-drop Target. For different "
           "drop groups (L-hand vs R-hand fingers), set each Drop row's "
           "Target bone — or use 'Drop + rigid-bind selected' below the "
           "joints table — so each group binds to its own bone instead of "
           "all stretching here. Defaults to bone 0."));
    connect(rb_combo_, &QComboBox::currentIndexChanged, this,
            &BoneRemapDialog::refresh_diagnostics);
    connect(rb_combo_, &QComboBox::currentIndexChanged, this,
            &BoneRemapDialog::refresh_issue_panel);
    connect(rb_combo_, &QComboBox::currentIndexChanged, this,
            &BoneRemapDialog::refresh_view);
    opts_row->addWidget(rb_combo_, 1);
    auto_rig_cb_ = new QCheckBox(tr("Auto-rig skinless imports"));
    auto_rig_cb_->setChecked(init.initial_auto_rig);
    auto_rig_cb_->setToolTip(
        tr("When the GLB carries no JOINTS_0/WEIGHTS_0 at all, synthesise "
           "a skin by inverse-distance weighting each vertex to its 4 "
           "nearest original bones. Useful for static props on animated "
           "characters; not a substitute for proper weight-paint."));
    connect(auto_rig_cb_, &QCheckBox::checkStateChanged, this,
            &BoneRemapDialog::refresh_diagnostics);
    opts_row->addWidget(auto_rig_cb_);
    diag_rest_cb_ = new QCheckBox(tr("Diagnose rest-pose mismatch"));
    diag_rest_cb_->setChecked(init.initial_diagnose_rest_pose);
    diag_rest_cb_->setToolTip(
        tr("Print a per-bone table to the build log comparing each GLB "
           "joint's rest-pose position against the matched original "
           "bone's rest-pose position. Helps identify when the GLB was "
           "authored in a different rest pose than the orig (T-pose vs "
           "A-pose), which is the most common cause of animation "
           "distortion after a foreign swap.\n\n"
           "Does NOT modify the mesh — purely diagnostic. The earlier "
           "prototype that actually swapped IBMs produced exploding / "
           "shifted meshes because an IBM-only swap can't reconcile a "
           "rest-pose mismatch without also rewriting the actor's shared "
           "skeleton."));
    opts_row->addWidget(diag_rest_cb_);
    root->addLayout(opts_row);

    // Diagnostics summary (multi-line).
    auto* diag_frame = new QFrame();
    diag_frame->setFrameShape(QFrame::StyledPanel);
    auto* diag_lay = new QVBoxLayout(diag_frame);
    diag_lay->setContentsMargins(8, 6, 8, 6);
    diag_ = new QLabel(QString());
    diag_->setFont(QFont(QStringLiteral("Consolas"), 9));
    diag_->setWordWrap(true);
    diag_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    diag_lay->addWidget(diag_);
    root->addWidget(diag_frame);

    // Buttons
    auto* btns = new QHBoxLayout();
    auto* reset = new QPushButton(tr("Reset all to auto"));
    reset->setToolTip(
        tr("Reset every joint to the freshly-computed auto-map (joints "
           "with no auto entry become Unmapped, no Drops)."));
    connect(reset, &QPushButton::clicked, this,
            &BoneRemapDialog::reset_all_to_auto);
    btns->addWidget(reset);
    auto* fold = new QPushButton(tr("Fold unmapped → nearest bone"));
    fold->setToolTip(
        tr("For every joint currently set to Unmapped that has weighted "
           "vertices, switch it to Map and target the original bone whose "
           "vertex centroid is closest. Solves the common 'hair/skirt/prop "
           "joints have no name match' case without manual clicking. "
           "Joints in Drop are left alone."));
    connect(fold, &QPushButton::clicked, this,
            &BoneRemapDialog::fold_unmapped_to_nearest);
    btns->addWidget(fold);
    btns->addStretch(1);
    auto* cancel = new QPushButton(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    auto* keep_skin = new QPushButton(tr("Keep original skinning"));
    keep_skin->setToolTip(
        tr("Import ONLY the GLB's geometry, UVs and material assignment "
           "and KEEP the original mesh's bones + weights, copied onto the "
           "new vertices by position. Use this for UV / material / "
           "retexture edits that don't change geometry — it sidesteps GLB "
           "skin round-trip damage (wrong bones / under-weighting that "
           "collapses far-from-root parts like hands). The bone map above "
           "is ignored. Exact when geometry is unchanged; "
           "nearest-neighbour if the mesh was retopologised."));
    connect(keep_skin, &QPushButton::clicked, this,
            &BoneRemapDialog::on_keep_original_skin);
    auto* ok = new QPushButton(tr("OK"));
    ok->setDefault(true);
    connect(ok, &QPushButton::clicked, this, &QDialog::accept);
    btns->addWidget(cancel);
    btns->addWidget(keep_skin);
    btns->addWidget(ok);
    root->addLayout(btns);

    refresh_left();  // paints cells from actions_
    refresh_right();
    refresh_diagnostics();
    refresh_issue_panel();
    refresh_view();
}

// ───────────────────── pane builders ─────────────────────

QWidget* BoneRemapDialog::build_left_pane() {
    auto* wrap = new QWidget();
    auto* lay = new QVBoxLayout(wrap);
    lay->setContentsMargins(0, 0, 4, 0);
    lay->addWidget(new QLabel(QStringLiteral("<b>GLB joints</b>")));

    left_ = new QTableWidget(glb_names_.size(), 8);
    left_->setHorizontalHeaderLabels(
        {tr("#"), tr("Joint name"), tr("Action"), tr("Target bone"),
         tr("Verts"), tr("Weight%"), tr("Bind dist"), tr("Rest Δ")});
    left_->verticalHeader()->setVisible(false);
    left_->setEditTriggers(QTableWidget::NoEditTriggers);
    left_->setSelectionBehavior(QTableWidget::SelectRows);
    // Multi-row select so the modder can bulk-drop a whole finger group
    // to one rigid target in a single action (ctrl/shift-click).
    left_->setSelectionMode(QTableWidget::ExtendedSelection);
    QHeaderView* hdr = left_->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    hdr->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(3, QHeaderView::Stretch);
    hdr->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(7, QHeaderView::ResizeToContents);
    connect(left_, &QTableWidget::itemSelectionChanged, this,
            &BoneRemapDialog::on_left_selected);

    // Pre-populate static cells + per-row action/target combos.
    for (int j = 0; j < glb_names_.size(); ++j) {
        left_->setItem(j, 0, ro(QString::number(j)));
        // Joint name (carries the source-tag suffix; rewritten on refresh)
        left_->setItem(j, 1, ro(glb_names_.at(j)));
        // Action combo
        auto* ac = new QComboBox();
        for (const char* a : ACTIONS) ac->addItem(action_label(a), a);
        connect(ac, &QComboBox::currentIndexChanged, this,
                [this, j] { on_action_changed(j); });
        left_->setCellWidget(j, 2, ac);
        action_combos_.push_back(ac);
        // Target-bone combo
        auto* tc = new QComboBox();
        for (int bi = 0; bi < dp_names_.size(); ++bi)
            tc->addItem(
                QStringLiteral("%1  %2").arg(bi, 3).arg(dp_names_.at(bi)),
                bi);
        connect(tc, &QComboBox::currentIndexChanged, this,
                [this, j] { on_target_changed(j); });
        left_->setCellWidget(j, 3, tc);
        target_combos_.push_back(tc);
        // Verts / Weight% / Bind dist / Rest Δ — filled in refresh_left
        left_->setItem(j, 4, ro(QString(), true));
        left_->setItem(j, 5, ro(QString(), true));
        left_->setItem(j, 6, ro(QString(), true));
        left_->setItem(j, 7, ro(QString(), true));
    }
    lay->addWidget(left_);

    // Bulk-drop helper: select a group of joints, pick a rigid target
    // bone, and Drop them all to it at once.
    auto* bulk = new QHBoxLayout();
    bulk->addWidget(new QLabel(tr("Selected →")));
    bulk_target_ = new QComboBox();
    for (int bi = 0; bi < dp_names_.size(); ++bi)
        bulk_target_->addItem(
            QStringLiteral("%1  %2").arg(bi, 3).arg(dp_names_.at(bi)), bi);
    bulk_target_->setToolTip(
        tr("The original bone that the selected joints — once Dropped — "
           "rigid-bind their orphaned vertices to."));
    bulk->addWidget(bulk_target_, 1);
    auto* drop_btn = new QPushButton(tr("Drop + rigid-bind selected"));
    drop_btn->setToolTip(
        tr("Set every selected joint's Action to Drop and its rigid "
           "target to the bone above. Use it per group: select the "
           "left-hand fingers → L Hand, then the right-hand fingers → R "
           "Hand."));
    connect(drop_btn, &QPushButton::clicked, this,
            &BoneRemapDialog::on_bulk_drop);
    bulk->addWidget(drop_btn);
    lay->addLayout(bulk);
    return wrap;
}

std::vector<int> BoneRemapDialog::selected_joint_rows() const {
    std::set<int> rows;
    for (const QTableWidgetItem* it : left_->selectedItems())
        rows.insert(it->row());
    return std::vector<int>(rows.begin(), rows.end());
}

// Drop every selected joint and point its rigid target at the bulk bone —
// the multi-target workflow in one click.
void BoneRemapDialog::on_bulk_drop() {
    const std::vector<int> rows = selected_joint_rows();
    if (rows.empty()) return;
    const QVariant v = bulk_target_->currentData();
    const int tgt = v.isValid() ? v.toInt() : 0;
    for (int j : rows) {
        actions_[size_t(j)].action = "drop";
        actions_[size_t(j)].target = tgt;
        fold_picks_.erase(j);
    }
    // Repaint the affected rows' combos from the model, then refresh.
    refresh_left();
    refresh_right();
    refresh_diagnostics();
    refresh_issue_panel();
    refresh_view();
    apply_filter();
}

QWidget* BoneRemapDialog::build_right_pane() {
    auto* wrap = new QWidget();
    auto* lay = new QVBoxLayout(wrap);
    lay->setContentsMargins(4, 0, 0, 0);
    lay->addWidget(new QLabel(QStringLiteral("<b>Original bones</b>")));

    right_ = new QTableWidget(dp_names_.size(), 7);
    right_->setHorizontalHeaderLabels({tr("#"), tr("Bone name"),
                                       tr("Driven by"), tr("Verts"),
                                       tr("Old W%"), tr("New W%"),
                                       tr("Δ pp")});
    right_->verticalHeader()->setVisible(false);
    right_->setEditTriggers(QTableWidget::NoEditTriggers);
    right_->setSelectionBehavior(QTableWidget::SelectRows);
    right_->setSelectionMode(QTableWidget::SingleSelection);
    QHeaderView* hdr = right_->horizontalHeader();
    hdr->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(1, QHeaderView::Stretch);
    hdr->setSectionResizeMode(2, QHeaderView::Stretch);
    hdr->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    hdr->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    connect(right_, &QTableWidget::itemSelectionChanged, this,
            &BoneRemapDialog::on_right_selected);
    for (int bi = 0; bi < dp_names_.size(); ++bi) {
        right_->setItem(bi, 0, ro(QString::number(bi)));
        right_->setItem(bi, 1, ro(dp_names_.at(bi)));
        right_->setItem(bi, 2, ro(QString()));
        right_->setItem(bi, 3, ro(QString(), true));
        right_->setItem(bi, 4, ro(QString(), true));
        right_->setItem(bi, 5, ro(QString(), true));
        right_->setItem(bi, 6, ro(QString(), true));
    }
    lay->addWidget(right_);
    return wrap;
}

// ───────────────────── action-model bridge ─────────────────────

void BoneRemapDialog::on_action_changed(int joint) {
    const std::string ac =
        action_combos_[size_t(joint)]->currentData().toString().toStdString();
    actions_[size_t(joint)].action = ac;
    // A manual edit invalidates any fold tag on this joint.
    fold_picks_.erase(joint);
    // The Target combo is used by Map (the bone to map onto) AND Drop
    // (the rigid target for this drop's orphaned verts). Seed a sensible
    // default when switching in with no target: Map → its auto-map bone,
    // Drop → the current global rigid-bind fallback bone.
    if (!actions_[size_t(joint)].target
        && (ac == "map" || ac == "drop")) {
        int tgt = current_rigid_bind_bone();
        if (ac == "map") {
            auto it = auto_map_.find(joint);
            tgt = it != auto_map_.end() ? it->second : 0;
        }
        actions_[size_t(joint)].target = tgt;
        QComboBox* tc = target_combos_[size_t(joint)];
        tc->blockSignals(true);
        const int ti = tc->findData(tgt);
        tc->setCurrentIndex(std::max(0, ti));
        tc->blockSignals(false);
    }
    refresh_row_visuals(joint);
    refresh_right();
    refresh_diagnostics();
    refresh_issue_panel();
    refresh_view();
    apply_filter();
}

void BoneRemapDialog::on_target_changed(int joint) {
    // Map → the bone the joint's weight is transferred onto.
    // Drop → the bone this dropped joint's orphaned verts rigid-bind to.
    const std::string& a = actions_[size_t(joint)].action;
    if (a != "map" && a != "drop") return;
    const QVariant v = target_combos_[size_t(joint)]->currentData();
    if (!v.isValid()) return;
    actions_[size_t(joint)].target = v.toInt();
    // Manual target pick → drop any fold tag for this joint.
    fold_picks_.erase(joint);
    refresh_row_visuals(joint);
    refresh_right();
    refresh_diagnostics();
    refresh_issue_panel();
    refresh_view();
    apply_filter();
}

// ───────────────────── refresh helpers ─────────────────────

// Paint per-row state from actions_ (combos + stats + tag).
void BoneRemapDialog::refresh_left() {
    for (int j = 0; j < glb_names_.size(); ++j) {
        const JointAction& act = actions_[size_t(j)];
        QComboBox* ac = action_combos_[size_t(j)];
        ac->blockSignals(true);
        const int ai = ac->findData(qs(act.action));
        ac->setCurrentIndex(ai >= 0 ? ai : 0);
        ac->blockSignals(false);
        QComboBox* tc = target_combos_[size_t(j)];
        tc->blockSignals(true);
        if (act.target) {
            const int ti = tc->findData(*act.target);
            tc->setCurrentIndex(ti >= 0 ? ti : 0);
        }
        tc->blockSignals(false);
        refresh_row_visuals(j);
    }
}

// Update one left-pane row's name tag, coverage cells, combo enable.
void BoneRemapDialog::refresh_row_visuals(int j) {
    const JointAction& ent = actions_[size_t(j)];
    const jade::patcher::JointStats* stat =
        (j >= 0 && size_t(j) < joint_stats_.size())
            ? &joint_stats_[size_t(j)]
            : nullptr;
    const ThemeColors& theme = theme_colors();
    // Name + source tag
    const std::string src = current_source(j);
    QTableWidgetItem* name_item = left_->item(j, 1);
    name_item->setText(glb_names_.at(j)
                       + QStringLiteral("   [%1]").arg(src_label(src)));
    name_item->setForeground(QColor(theme.src.at(src)));
    // Target combo enablement: Map and Drop; Unmapped → disabled.
    target_combos_[size_t(j)]->setEnabled(ent.action == "map"
                                          || ent.action == "drop");
    // Coverage cells
    if (stat) {
        left_->item(j, 4)->setText(QString::number(stat->vertex_count));
        left_->item(j, 5)->setText(
            QStringLiteral("%1").arg(stat->weight_share * 100.0, 0, 'f', 1));
    } else {
        left_->item(j, 4)->setText(QStringLiteral("—"));
        left_->item(j, 5)->setText(QStringLiteral("—"));
    }
    // Bind distance — only meaningful when this joint maps onto a bone
    // AND we have centroids for both. Red when the centroid gap exceeds
    // 20% of the mesh diagonal; copper above 10% as a caution band.
    QTableWidgetItem* d_item = left_->item(j, 6);
    d_item->setText(QStringLiteral("—"));
    d_item->setForeground(QColor(theme.sev_grey));
    d_item->setToolTip(QString());
    QTableWidgetItem* r_item = left_->item(j, 7);
    r_item->setText(QStringLiteral("—"));
    r_item->setForeground(QColor(theme.sev_grey));
    r_item->setToolTip(QString());
    if (ent.action == "map" && ent.target) {
        const int tgt = *ent.target;
        const OptPos3* jc = (size_t(j) < joint_centroids_.size()
                             && joint_centroids_[size_t(j)].first)
                                ? &joint_centroids_[size_t(j)]
                                : nullptr;
        const OptPos3* bc = (tgt >= 0
                             && size_t(tgt) < bone_centroids_.size()
                             && bone_centroids_[size_t(tgt)].first)
                                ? &bone_centroids_[size_t(tgt)]
                                : nullptr;
        if (jc && bc) {
            const double dist = dist3(jc->second, bc->second);
            const double pct =
                mesh_diag_ > 0 ? dist / mesh_diag_ * 100.0 : 0.0;
            d_item->setText(QStringLiteral("%1  (%2%)")
                                .arg(dist, 0, 'f', 2)
                                .arg(pct, 0, 'f', 0));
            if (mesh_diag_ > 0 && pct >= 20.0)
                d_item->setForeground(QColor(theme.sev_red));
            else if (mesh_diag_ > 0 && pct >= 10.0)
                d_item->setForeground(QColor(theme.sev_copper));
            else
                d_item->setForeground(QColor(theme.sev_green));
            d_item->setToolTip(
                tr("Shape-relative centroid distance — how differently "
                   "this joint's vertex cluster sits compared to the "
                   "target bone's cluster, measured against each mesh's "
                   "own centre of mass. (The two meshes don't share an "
                   "absolute origin, so each side's centroids are "
                   "recentred before the comparison.)\n"
                   "GLB centroid (relative): (%1, %2, %3)\n"
                   "Orig centroid (relative): (%4, %5, %6)\n"
                   "Mesh diagonal: %7")
                    .arg(jc->second[0], 0, 'f', 2)
                    .arg(jc->second[1], 0, 'f', 2)
                    .arg(jc->second[2], 0, 'f', 2)
                    .arg(bc->second[0], 0, 'f', 2)
                    .arg(bc->second[1], 0, 'f', 2)
                    .arg(bc->second[2], 0, 'f', 2)
                    .arg(mesh_diag_, 0, 'f', 2));
        }
        // T4.2 — Rest Δ. The IBM-based rest-pose delta. Thresholds match
        // the build-time diagnostic: ≥0.5 → red; ≥0.1 → copper; <0.1 →
        // green.
        const OptPos3* jp = (size_t(j) < joint_rest_.size()
                             && joint_rest_[size_t(j)].first)
                                ? &joint_rest_[size_t(j)]
                                : nullptr;
        const OptPos3* bp = (tgt >= 0 && size_t(tgt) < bone_rest_.size()
                             && bone_rest_[size_t(tgt)].first)
                                ? &bone_rest_[size_t(tgt)]
                                : nullptr;
        if (jp && bp) {
            const double rd = dist3(jp->second, bp->second);
            r_item->setText(QStringLiteral("%1").arg(rd, 0, 'f', 2));
            if (rd >= 0.5)
                r_item->setForeground(QColor(theme.sev_red));
            else if (rd >= 0.1)
                r_item->setForeground(QColor(theme.sev_copper));
            else
                r_item->setForeground(QColor(theme.sev_green));
            r_item->setToolTip(
                tr("Rest-pose delta — distance between the GLB joint's "
                   "bind position and the target bone's bind position.\n"
                   "GLB joint:  (%1, %2, %3)\n"
                   "Orig bone:  (%4, %5, %6)\n"
                   "\n"
                   "≥0.5 means the GLB was authored in a different rest "
                   "pose than the orig (T-pose vs A-pose, different bone "
                   "scales). Skinning still renders at rest, but "
                   "animation will deform incorrectly. Fix in Blender by "
                   "retargeting before re-exporting.")
                    .arg(jp->second[0], 0, 'f', 3)
                    .arg(jp->second[1], 0, 'f', 3)
                    .arg(jp->second[2], 0, 'f', 3)
                    .arg(bp->second[0], 0, 'f', 3)
                    .arg(bp->second[1], 0, 'f', 3)
                    .arg(bp->second[2], 0, 'f', 3));
        }
    }
}

// Rebuild per-bone rollup from current actions.
void BoneRemapDialog::refresh_right() {
    const ThemeColors& theme = theme_colors();
    std::map<int, std::vector<int>> per_bone_joints;
    for (int b = 0; b < dp_names_.size(); ++b) per_bone_joints[b] = {};
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& ent = actions_[size_t(j)];
        if (ent.action != "map" || !ent.target) continue;
        const int b = *ent.target;
        if (b >= 0 && b < dp_names_.size())
            per_bone_joints[b].push_back(j);
    }
    for (int b = 0; b < dp_names_.size(); ++b) {
        std::vector<int>& joints = per_bone_joints[b];
        std::sort(joints.begin(), joints.end());
        uint32_t n_v = 0;
        double sum_w = 0.0;
        QStringList jn_parts;
        for (int j : joints) {
            if (size_t(j) < joint_stats_.size()) {
                n_v += joint_stats_[size_t(j)].vertex_count;
                sum_w += joint_stats_[size_t(j)].weight_share;
            }
            const QString gn = j < glb_names_.size()
                                   ? glb_names_.at(j)
                                   : QStringLiteral("joint_%1").arg(j);
            jn_parts << QStringLiteral("%1:%2").arg(j).arg(gn);
        }
        QString shown = QStringList(jn_parts.mid(0, 3))
                            .join(QStringLiteral(", "));
        if (jn_parts.size() > 3)
            shown +=
                QStringLiteral(" (+%1 more)").arg(jn_parts.size() - 3);
        right_->item(b, 2)->setText(shown);
        right_->item(b, 2)->setToolTip(
            jn_parts.isEmpty() ? QString()
                               : jn_parts.join(QStringLiteral("\n")));
        right_->item(b, 3)->setText(
            n_v ? QString::number(n_v) : QStringLiteral("—"));
        // T3.4 — Old / New / Δ. Δ in percentage points, not relative %.
        const double old_share =
            (b >= 0 && size_t(b) < orig_shares_.size())
                ? orig_shares_[size_t(b)]
                : 0.0;
        const double new_share = sum_w;
        const double old_pct = old_share * 100.0;
        const double new_pct = new_share * 100.0;
        const double delta_pp = new_pct - old_pct;
        QTableWidgetItem* old_item = right_->item(b, 4);
        QTableWidgetItem* new_item = right_->item(b, 5);
        QTableWidgetItem* delta_item = right_->item(b, 6);
        old_item->setText(old_share != 0.0
                              ? QStringLiteral("%1").arg(old_pct, 0, 'f', 1)
                              : QStringLiteral("—"));
        new_item->setText(new_share != 0.0
                              ? QStringLiteral("%1").arg(new_pct, 0, 'f', 1)
                              : QStringLiteral("—"));
        if (old_share == 0.0 && new_share == 0.0) {
            delta_item->setText(QStringLiteral("—"));
            delta_item->setForeground(QColor(theme.sev_grey));
        } else {
            const QString sign =
                delta_pp >= 0 ? QStringLiteral("+") : QString();
            delta_item->setText(
                sign + QStringLiteral("%1").arg(delta_pp, 0, 'f', 1));
            // Big shift either way (>10 pp) is noteworthy; >5 pp amber.
            const double mag = std::abs(delta_pp);
            if (mag >= 10.0)
                delta_item->setForeground(QColor(theme.sev_red));
            else if (mag >= 5.0)
                delta_item->setForeground(QColor(theme.sev_copper));
            else
                delta_item->setForeground(QColor(theme.sev_dark));
        }
    }
}

void BoneRemapDialog::refresh_diagnostics() {
    int n_map = 0, n_drop = 0, n_un = 0;
    for (const JointAction& a : actions_) {
        if (a.action == "map") ++n_map;
        else if (a.action == "drop") ++n_drop;
        else ++n_un;
    }
    // Provenance breakdown of the Map actions.
    std::map<std::string, int> src_counts{{"user", 0}, {"name", 0},
                                          {"geometric", 0}};
    for (int j = 0; j < int(actions_.size()); ++j) {
        if (actions_[size_t(j)].action != "map") continue;
        const std::string s = current_source(j);
        auto it = src_counts.find(s);
        if (it != src_counts.end()) ++it->second;
    }
    // Bone-side rollup.
    std::set<int> driven_bones;
    for (const JointAction& a : actions_)
        if (a.action == "map" && a.target) driven_bones.insert(*a.target);
    const int n_driven = int(driven_bones.size());
    const int n_idle = dp_names_.size() - n_driven;
    // Weight gone to drops + unmapped (informational impact line).
    double drop_w = 0.0, un_w = 0.0;
    for (int j = 0; j < int(actions_.size()); ++j) {
        if (size_t(j) >= joint_stats_.size()) continue;
        const double w = joint_stats_[size_t(j)].weight_share;
        if (actions_[size_t(j)].action == "drop") drop_w += w;
        else if (actions_[size_t(j)].action == "unmapped") un_w += w;
    }
    const int rb = current_rigid_bind_bone();
    const QString rb_name = rb >= 0 && rb < dp_names_.size()
                                ? dp_names_.at(rb)
                                : QStringLiteral("bone_%1").arg(rb);
    const QStringList lines{
        QStringLiteral("GLB joints:   %1 mapped  ·  %2 dropped  ·  "
                       "%3 unmapped   |   map source: %4 name, %5 geom, "
                       "%6 user")
            .arg(n_map)
            .arg(n_drop)
            .arg(n_un)
            .arg(src_counts["name"])
            .arg(src_counts["geometric"])
            .arg(src_counts["user"]),
        QStringLiteral("Orig bones:   %1 driven  ·  %2 idle (of %3)")
            .arg(n_driven)
            .arg(n_idle)
            .arg(dp_names_.size()),
        QStringLiteral("Weight impact: %1% will be dropped (renormalised)"
                       "   ·   %2% will route to bone[%3] %4 "
                       "(rigid-bound)")
            .arg(drop_w * 100.0, 0, 'f', 1)
            .arg(un_w * 100.0, 0, 'f', 1)
            .arg(rb)
            .arg(rb_name),
    };
    diag_->setText(lines.join(QLatin1Char('\n')));
}

int BoneRemapDialog::current_rigid_bind_bone() const {
    if (!rb_combo_) return 0;
    const QVariant v = rb_combo_->currentData();
    return v.isValid() ? v.toInt() : 0;
}

// ───────────────────── 3D outcome view sync ─────────────────────

// Push current state into the 3D viewer.
void BoneRemapDialog::refresh_view() {
    if (!view_) return;
    BoneMapData data;
    data.bone_positions = bone_rest_;
    data.joint_positions = joint_rest_;
    data.bone_names = dp_names_;
    data.joint_names = glb_names_;
    data.actions = actions_;
    data.joint_stats = joint_stats_;
    data.orig_bone_weight_shares = orig_shares_;
    data.rigid_bind_bone = current_rigid_bind_bone();
    data.mesh_diagonal = mesh_diag_;
    view_->set_data(data);
}

// 3D view clicked an orig bone → scroll right table to it.
void BoneRemapDialog::on_view_bone_clicked(int bi) {
    if (bi < 0 || bi >= right_->rowCount()) return;
    right_->scrollToItem(right_->item(bi, 0));
    right_->selectRow(bi);
}

// 3D view clicked a GLB joint → scroll left table to it.
void BoneRemapDialog::on_view_joint_clicked(int j) {
    if (j < 0 || j >= left_->rowCount()) return;
    left_->scrollToItem(left_->item(j, 0));
    left_->selectRow(j);
}

// ───────────────────── live issue panel ─────────────────────

// Rebuild the live issue panel from the current action model.
void BoneRemapDialog::refresh_issue_panel() {
    const ThemeColors& theme = theme_colors();
    const auto live = compute_live_issues();
    QStringList rows;
    for (const auto& [sev, msg] : live) rows << issue_row(sev, msg);
    if (rows.isEmpty())
        rows << issue_row(QStringLiteral("ok"),
                          tr("Mapping looks healthy — every joint is "
                             "routed, rest poses are aligned, no "
                             "high-impact drops."));

    int n_problems = 0;
    bool any_error = false;
    for (const auto& [sev, msg] : live) {
        if (sev == QLatin1String("error") || sev == QLatin1String("warn"))
            ++n_problems;
        if (sev == QLatin1String("error")) any_error = true;
    }
    const QString header_color =
        any_error ? theme.issue_error
                  : (n_problems ? theme.issue_warn : theme.issue_ok);
    const QString header_label =
        n_problems ? tr("Live checks — %1 active issue(s)").arg(n_problems)
                   : tr("Live checks — all clear");
    issue_panel_->setHtml(
        QStringLiteral("<div style='color:%1; font-weight:bold; "
                       "margin-bottom:4px'>%2</div>%3")
            .arg(header_color, header_label, rows.join(QString())));
}

std::vector<std::pair<QString, QString>>
BoneRemapDialog::compute_live_issues() const {
    std::vector<std::pair<QString, QString>> issues;
    const int rb = current_rigid_bind_bone();
    const QString rb_name = rb >= 0 && rb < dp_names_.size()
                                ? dp_names_.at(rb)
                                : QStringLiteral("bone_%1").arg(rb);

    // ── A. Structural signals.
    const int n_glb = glb_names_.size();
    const int n_bones = dp_names_.size();
    double total_glb_weight = 0.0;
    for (const auto& s : joint_stats_) total_glb_weight += s.weight_share;
    // Skin presence.
    if (n_glb == 0 || total_glb_weight <= 1e-6) {
        if (n_bones > 0)
            issues.push_back(
                {QStringLiteral("warn"),
                 tr("GLB carries no skin data. The whole mesh will "
                    "rigid-bind to <b>bone[%1] %2</b> unless you enable "
                    "<b>Auto-rig skinless imports</b> below to weight "
                    "each vertex to its nearest bone.")
                     .arg(rb)
                     .arg(esc(rb_name))});
    } else {
        // Joint-count mismatch.
        if (n_glb > n_bones) {
            int un_or_drop = 0;
            for (const JointAction& a : actions_)
                if (a.action == "unmapped" || a.action == "drop")
                    ++un_or_drop;
            const int surplus = n_glb - n_bones;
            if (un_or_drop < surplus)
                issues.push_back(
                    {QStringLiteral("info"),
                     tr("GLB has %1 more joints than the original "
                        "skeleton (%2 vs %3). Map them onto existing "
                        "bones (hair→head, skirt→pelvis), or set Drop to "
                        "renormalise their weight onto the rest.")
                         .arg(n_glb - n_bones)
                         .arg(n_glb)
                         .arg(n_bones)});
        } else if (n_glb < n_bones) {
            if ((n_bones - n_glb) >= std::max(5, n_bones / 4))
                issues.push_back(
                    {QStringLiteral("info"),
                     tr("GLB has only %1 joints; the original skeleton "
                        "has %2. %3 original bone(s) will not be driven "
                        "by any imported joint.")
                         .arg(n_glb)
                         .arg(n_bones)
                         .arg(n_bones - n_glb)});
        }

        // Name-match coverage.
        int n_name_matches = 0;
        for (const auto& [j, s] : auto_source_)
            if (s == "name") ++n_name_matches;
        if (n_name_matches == 0 && n_glb > 0 && n_bones > 0) {
            issues.push_back(
                {QStringLiteral("warn"),
                 tr("No joint auto-matched by name. Try "
                    "<b>Fold unmapped → nearest bone</b> for a "
                    "centroid-based starting point, apply a saved "
                    "<b>Preset</b>, or fill the alias dictionary at "
                    "<code>~/.jade_explorer/bone_aliases.json</code> so "
                    "future swaps auto-match.")});
        } else if (n_name_matches < n_bones / 3.0 && n_name_matches > 0
                   && n_bones > 6) {
            issues.push_back(
                {QStringLiteral("info"),
                 tr("Only %1 of %2 bones got an auto name match. "
                    "<b>Fold unmapped → nearest bone</b> will fill the "
                    "gaps geometrically.")
                     .arg(n_name_matches)
                     .arg(n_bones)});
        }
    }

    // ── B. Live mapping checks.

    // 1. Unmapped joints that still carry weight (will rigid-bind).
    double un_w = 0.0;
    std::vector<std::pair<double, QString>> un_joints;
    for (int j = 0; j < int(actions_.size()); ++j) {
        if (actions_[size_t(j)].action != "unmapped") continue;
        if (size_t(j) >= joint_stats_.size()) continue;
        const double w = joint_stats_[size_t(j)].weight_share;
        if (w > 0.001) {
            un_w += w;
            un_joints.push_back(
                {w, j < glb_names_.size()
                        ? glb_names_.at(j)
                        : QStringLiteral("joint_%1").arg(j)});
        }
    }
    std::sort(un_joints.rbegin(), un_joints.rend());
    if (un_w >= 0.10) {
        QStringList ex;
        for (size_t i = 0; i < un_joints.size() && i < 3; ++i)
            ex << QStringLiteral("<b>%1</b> (%2%)")
                      .arg(esc(un_joints[i].second))
                      .arg(un_joints[i].first * 100, 0, 'f', 1);
        issues.push_back(
            {QStringLiteral("error"),
             tr("%1% of mesh weight is on unmapped joint(s) — will "
                "rigid-bind to <b>bone[%2] %3</b>. Top: %4. Map them or "
                "set Drop to renormalise that weight away.")
                 .arg(un_w * 100, 0, 'f', 1)
                 .arg(rb)
                 .arg(esc(rb_name), ex.join(QStringLiteral(", ")))});
    } else if (un_w >= 0.02) {
        issues.push_back(
            {QStringLiteral("warn"),
             tr("%1% of mesh weight on unmapped joint(s) will "
                "rigid-bind to <b>bone[%2] %3</b> — minor visible "
                "effect.")
                 .arg(un_w * 100, 0, 'f', 1)
                 .arg(rb)
                 .arg(esc(rb_name))});
    }

    // 2. Drop coverage — informational about the renorm impact.
    double drop_w = 0.0;
    int drop_n = 0;
    for (int j = 0; j < int(actions_.size()); ++j) {
        if (actions_[size_t(j)].action != "drop") continue;
        ++drop_n;
        if (size_t(j) < joint_stats_.size())
            drop_w += joint_stats_[size_t(j)].weight_share;
    }
    if (drop_w >= 0.20) {
        issues.push_back(
            {QStringLiteral("warn"),
             tr("Dropping %1 joint(s) carrying %2% of weight — "
                "surviving influences renormalise to absorb that share.")
                 .arg(drop_n)
                 .arg(drop_w * 100, 0, 'f', 1)});
    } else if (drop_n > 0) {
        issues.push_back(
            {QStringLiteral("info"),
             tr("%1 joint(s) dropped, %2% of weight renormalised.")
                 .arg(drop_n)
                 .arg(drop_w * 100, 0, 'f', 1)});
    }

    // 3. Rest-pose deltas (IBM-based — the real one).
    std::vector<std::tuple<double, QString, QString>> bad_rest_err,
        bad_rest_warn;
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& a = actions_[size_t(j)];
        if (a.action != "map" || !a.target) continue;
        const int tgt = *a.target;
        if (size_t(j) >= joint_rest_.size() || !joint_rest_[size_t(j)].first)
            continue;
        if (tgt < 0 || size_t(tgt) >= bone_rest_.size()
            || !bone_rest_[size_t(tgt)].first)
            continue;
        const double d =
            dist3(joint_rest_[size_t(j)].second, bone_rest_[size_t(tgt)].second);
        if (d >= 0.5) {
            const QString jn = j < glb_names_.size()
                                   ? glb_names_.at(j)
                                   : QStringLiteral("joint_%1").arg(j);
            const QString bn = tgt < dp_names_.size()
                                   ? dp_names_.at(tgt)
                                   : QStringLiteral("bone_%1").arg(tgt);
            (d >= 1.0 ? bad_rest_err : bad_rest_warn)
                .push_back({d, jn, bn});
        }
    }
    auto fmt_rest = [](const std::vector<std::tuple<double, QString,
                                                    QString>>& v) {
        QStringList ex;
        for (size_t i = 0; i < v.size() && i < 3; ++i)
            ex << QStringLiteral("<b>%1</b>→%2 (%3)")
                      .arg(esc(std::get<1>(v[i])), esc(std::get<2>(v[i])))
                      .arg(std::get<0>(v[i]), 0, 'f', 2);
        return ex.join(QStringLiteral(", "));
    };
    if (!bad_rest_err.empty()) {
        std::sort(bad_rest_err.rbegin(), bad_rest_err.rend());
        const QString tail =
            bad_rest_err.size() > 3
                ? QStringLiteral(" …+%1").arg(bad_rest_err.size() - 3)
                : QString();
        issues.push_back(
            {QStringLiteral("error"),
             tr("%1 mapping(s) with rest-pose delta ≥1.0 — animation "
                "will deform incorrectly. %2%3. Fix in Blender by "
                "retargeting the GLB to the orig skeleton's rest pose "
                "before re-exporting.")
                 .arg(bad_rest_err.size())
                 .arg(fmt_rest(bad_rest_err), tail)});
    }
    if (!bad_rest_warn.empty()) {
        std::sort(bad_rest_warn.rbegin(), bad_rest_warn.rend());
        const QString tail =
            bad_rest_warn.size() > 3
                ? QStringLiteral(" …+%1").arg(bad_rest_warn.size() - 3)
                : QString();
        issues.push_back(
            {QStringLiteral("warn"),
             tr("%1 mapping(s) with rest-pose delta 0.5–1.0 — minor "
                "animation drift likely. %2%3.")
                 .arg(bad_rest_warn.size())
                 .arg(fmt_rest(bad_rest_warn), tail)});
    }

    // 4. Bind-distance (centroid-based proxy).
    if (mesh_diag_ > 0) {
        std::vector<std::tuple<double, QString, QString>> bad_bind;
        for (int j = 0; j < int(actions_.size()); ++j) {
            const JointAction& a = actions_[size_t(j)];
            if (a.action != "map" || !a.target) continue;
            const int tgt = *a.target;
            if (size_t(j) >= joint_centroids_.size()
                || !joint_centroids_[size_t(j)].first)
                continue;
            if (tgt < 0 || size_t(tgt) >= bone_centroids_.size()
                || !bone_centroids_[size_t(tgt)].first)
                continue;
            const double d = dist3(joint_centroids_[size_t(j)].second,
                                   bone_centroids_[size_t(tgt)].second);
            const double pct = d / mesh_diag_ * 100.0;
            if (pct >= 20.0) {
                const QString jn = j < glb_names_.size()
                                       ? glb_names_.at(j)
                                       : QStringLiteral("joint_%1").arg(j);
                const QString bn = tgt < dp_names_.size()
                                       ? dp_names_.at(tgt)
                                       : QStringLiteral("bone_%1").arg(tgt);
                bad_bind.push_back({pct, jn, bn});
            }
        }
        if (!bad_bind.empty()) {
            std::sort(bad_bind.rbegin(), bad_bind.rend());
            QStringList ex;
            for (size_t i = 0; i < bad_bind.size() && i < 3; ++i)
                ex << QStringLiteral("<b>%1</b>→%2 (%3%)")
                          .arg(esc(std::get<1>(bad_bind[i])),
                               esc(std::get<2>(bad_bind[i])))
                          .arg(std::get<0>(bad_bind[i]), 0, 'f', 0);
            const QString tail =
                bad_bind.size() > 3
                    ? QStringLiteral(" …+%1").arg(bad_bind.size() - 3)
                    : QString();
            issues.push_back(
                {QStringLiteral("warn"),
                 tr("%1 mapping(s) where the joint's vertex cluster is "
                    "&gt;20% of mesh size away from the target bone — "
                    "probably feeding the wrong body part. %2%3.")
                     .arg(bad_bind.size())
                     .arg(ex.join(QStringLiteral(", ")), tail)});
        }
    }

    // 5. Many-to-one fan-in (informational — sometimes intentional).
    std::map<int, std::vector<int>> fan_in;
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& a = actions_[size_t(j)];
        if (a.action == "map" && a.target) fan_in[*a.target].push_back(j);
    }
    std::vector<std::tuple<int, int, QString>> heavy_merges;
    for (const auto& [bi, joints] : fan_in) {
        if (int(joints.size()) >= 4) {
            const QString bn = bi < dp_names_.size()
                                   ? dp_names_.at(bi)
                                   : QStringLiteral("bone_%1").arg(bi);
            heavy_merges.push_back({int(joints.size()), bi, bn});
        }
    }
    if (!heavy_merges.empty()) {
        std::sort(heavy_merges.rbegin(), heavy_merges.rend());
        QStringList ex;
        for (size_t i = 0; i < heavy_merges.size() && i < 3; ++i)
            ex << QStringLiteral("<b>%1</b> (%2)")
                      .arg(esc(std::get<2>(heavy_merges[i])))
                      .arg(std::get<0>(heavy_merges[i]));
        issues.push_back(
            {QStringLiteral("info"),
             tr("%1 bone(s) absorb 4+ GLB joints each — intentional "
                "merges are fine, but if unintentional the weight gets "
                "piled. Top: %2.")
                 .arg(heavy_merges.size())
                 .arg(ex.join(QStringLiteral(", ")))});
    }

    return issues;
}

// ───────────────────── selection sync ─────────────────────

void BoneRemapDialog::on_left_selected() {
    std::set<int> rows;
    for (const QTableWidgetItem* it : left_->selectedItems())
        rows.insert(it->row());
    if (rows.empty()) {
        clear_right_link_highlight();
        if (view_) view_->set_selection();
        return;
    }
    const int j = *rows.begin();
    // Clear any previous bone-side link highlight.
    clear_right_link_highlight();
    // Mirror selection into the 3D view.
    if (view_) view_->set_selection(std::nullopt, j);
    const JointAction& ent = actions_[size_t(j)];
    if (ent.action != "map" || !ent.target) return;
    const int bi = *ent.target;
    if (bi >= 0 && bi < right_->rowCount()) {
        right_->scrollToItem(right_->item(bi, 0));
        set_right_row_highlight(bi, true);
    }
}

void BoneRemapDialog::on_right_selected() {
    std::set<int> rows;
    for (const QTableWidgetItem* it : right_->selectedItems())
        rows.insert(it->row());
    // Always clear left-side link highlight first.
    clear_left_link_highlight();
    if (rows.empty()) {
        if (view_) view_->set_selection();
        return;
    }
    const int bi = *rows.begin();
    if (view_) view_->set_selection(bi, std::nullopt);
    int first = -1;
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& ent = actions_[size_t(j)];
        if (ent.action == "map" && ent.target && *ent.target == bi) {
            set_left_row_highlight(j, true);
            if (first < 0) first = j;
        }
    }
    if (first >= 0) left_->scrollToItem(left_->item(first, 0));
}

// When the link highlight is on, the per-cell coloured foregrounds can
// become unreadable against the highlight background. Override the
// foreground to link_fg_force on highlight, then restore the theme
// foregrounds when clearing.

void BoneRemapDialog::set_left_row_highlight(int j, bool on) {
    const ThemeColors& theme = theme_colors();
    const QColor bg = on ? theme.link_bg : QColor(Qt::transparent);
    for (int c = 0; c < left_->columnCount(); ++c)
        if (QTableWidgetItem* it = left_->item(j, c)) it->setBackground(bg);
    const int cols[] = {1, 4, 5, 6, 7};
    if (on) {
        for (int c : cols)
            if (QTableWidgetItem* it = left_->item(j, c))
                it->setForeground(theme.link_fg_force);
    } else {
        for (int c : cols)
            if (QTableWidgetItem* it = left_->item(j, c))
                it->setForeground(theme.text);
        refresh_row_visuals(j);
    }
}

void BoneRemapDialog::set_right_row_highlight(int bi, bool on) {
    const ThemeColors& theme = theme_colors();
    const QColor bg = on ? theme.link_bg : QColor(Qt::transparent);
    for (int c = 0; c < right_->columnCount(); ++c)
        if (QTableWidgetItem* it = right_->item(bi, c))
            it->setBackground(bg);
    const int cols[] = {4, 5, 6};
    if (on) {
        for (int c : cols)
            if (QTableWidgetItem* it = right_->item(bi, c))
                it->setForeground(theme.link_fg_force);
    } else {
        for (int c : cols)
            if (QTableWidgetItem* it = right_->item(bi, c))
                it->setForeground(theme.text);
        refresh_right();
    }
}

void BoneRemapDialog::clear_left_link_highlight() {
    for (int j = 0; j < left_->rowCount(); ++j)
        set_left_row_highlight(j, false);
}

void BoneRemapDialog::clear_right_link_highlight() {
    for (int bi = 0; bi < right_->rowCount(); ++bi)
        set_right_row_highlight(bi, false);
}

// ───────────────────── filter ─────────────────────

void BoneRemapDialog::apply_filter() {
    const QString needle = search_->text().trimmed().toLower();
    if (needle.isEmpty()) {
        for (int j = 0; j < left_->rowCount(); ++j)
            left_->setRowHidden(j, false);
        for (int b = 0; b < right_->rowCount(); ++b)
            right_->setRowHidden(b, false);
        return;
    }
    // Joints
    for (int j = 0; j < left_->rowCount(); ++j) {
        const JointAction& ent = actions_[size_t(j)];
        const QString jn =
            j < glb_names_.size() ? glb_names_.at(j).toLower() : QString();
        const QString action_text = action_label(ent.action).toLower();
        QString target_text;
        if (ent.action == "map" && ent.target) {
            const int bi = *ent.target;
            if (bi >= 0 && bi < dp_names_.size())
                target_text = dp_names_.at(bi).toLower();
        }
        const bool visible = jn.contains(needle)
                             || action_text.contains(needle)
                             || target_text.contains(needle);
        left_->setRowHidden(j, !visible);
    }
    // Bones
    for (int b = 0; b < right_->rowCount(); ++b) {
        const QString bn =
            b < dp_names_.size() ? dp_names_.at(b).toLower() : QString();
        right_->setRowHidden(b, !bn.contains(needle));
    }
}

// ───────────────────── provenance ─────────────────────

// Provenance for joint j as the dialog currently stands: "user", "name",
// "geometric", "drop", or "unmapped".
std::string BoneRemapDialog::current_source(int j) const {
    const JointAction& ent = actions_[size_t(j)];
    if (ent.action == "drop") return "drop";
    if (ent.action == "unmapped") return "unmapped";
    const std::optional<int>& tgt = ent.target;
    // Kept-auto if target matches auto_map's pick → inherit auto src.
    if (tgt) {
        auto am = auto_map_.find(j);
        if (am != auto_map_.end() && am->second == *tgt) {
            auto as = auto_source_.find(j);
            return as != auto_source_.end() ? as->second : "geometric";
        }
        // T3.1 fold picks are algorithmic, not modder hand-picks.
        auto fp = fold_picks_.find(j);
        if (fp != fold_picks_.end() && fp->second == *tgt)
            return "geometric";
        // Persisted carry-over: trust the prior persisted source.
        auto is = initial_source_.find(j);
        if (is != initial_source_.end()) return is->second;
    }
    return "user";
}

// ───────────────────── presets (T2.4) ─────────────────────

// Repopulate the preset combo from disk; keep selection if it still
// exists, else snap to the first preset.
void BoneRemapDialog::reload_preset_combo() {
    const QString prev =
        preset_combo_ ? preset_combo_->currentText() : QString();
    preset_combo_->blockSignals(true);
    preset_combo_->clear();
    preset_combo_->addItem(tr("(no preset selected)"));
    for (const auto& [name, mapping] : load_presets())
        preset_combo_->addItem(name);
    const int i = preset_combo_->findText(prev);
    preset_combo_->setCurrentIndex(std::max(0, i));
    preset_combo_->blockSignals(false);
}

void BoneRemapDialog::on_apply_preset() {
    const QString name = preset_combo_->currentText();
    const auto presets = load_presets();
    auto pit = presets.find(name);
    if (pit == presets.end() || pit->second.empty()) return;
    const std::map<QString, QString>& mapping = pit->second;
    // Normalised orig bone name -> orig bone index. First win.
    std::map<QString, int> bone_by_norm;
    for (int bi = 0; bi < dp_names_.size(); ++bi)
        bone_by_norm.emplace(norm(dp_names_.at(bi)), bi);
    int n_applied = 0, n_missed = 0;
    for (int j = 0; j < glb_names_.size(); ++j) {
        auto mit = mapping.find(norm(glb_names_.at(j)));
        if (mit == mapping.end() || mit->second.isEmpty()) continue;
        auto bit = bone_by_norm.find(mit->second);
        if (bit == bone_by_norm.end()) {
            ++n_missed;
            continue;
        }
        actions_[size_t(j)] = JointAction{"map", bit->second};
        fold_picks_.erase(j);
        ++n_applied;
    }
    refresh_left();
    refresh_right();
    refresh_diagnostics();
    refresh_issue_panel();
    clear_left_link_highlight();
    clear_right_link_highlight();
    apply_filter();
    QString msg = tr("Applied preset '%1': %2 joint(s) routed")
                      .arg(name)
                      .arg(n_applied);
    if (n_missed)
        msg += tr(". %1 preset entr%2 had no matching bone in this "
                  "skeleton.")
                   .arg(n_missed)
                   .arg(n_missed == 1 ? tr("y") : tr("ies"));
    QMessageBox::information(this, tr("Preset applied"), msg);
}

void BoneRemapDialog::on_save_preset() {
    // Build the normalised mapping from current Map decisions.
    std::map<QString, QString> mapping;
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& ent = actions_[size_t(j)];
        if (ent.action != "map" || !ent.target) continue;
        const int bi = *ent.target;
        if (bi < 0 || bi >= dp_names_.size()) continue;
        const QString jn = j < glb_names_.size() ? glb_names_.at(j)
                                                 : QString();
        mapping[norm(jn)] = norm(dp_names_.at(bi));
    }
    if (mapping.empty()) {
        QMessageBox::information(
            this, tr("Save preset"),
            tr("No Map decisions to save — every joint is currently Drop "
               "or Unmapped."));
        return;
    }
    bool ok = false;
    QString name = QInputDialog::getText(
        this, tr("Save preset"), tr("Preset name:"), QLineEdit::Normal,
        preset_combo_->currentIndex() > 0 ? preset_combo_->currentText()
                                          : QString(),
        &ok);
    name = name.trimmed();
    if (!ok || name.isEmpty()) return;
    auto presets = load_presets();
    if (presets.count(name)) {
        const auto reply = QMessageBox::question(
            this, tr("Overwrite preset"),
            tr("Preset '%1' already exists. Overwrite it?").arg(name),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (reply != QMessageBox::Yes) return;
    }
    presets[name] = mapping;
    if (!save_presets(presets)) {
        QMessageBox::critical(
            this, tr("Save preset"),
            tr("Could not write presets file:\n  %1").arg(presets_path()));
        return;
    }
    reload_preset_combo();
    const int idx = preset_combo_->findText(name);
    if (idx >= 0) preset_combo_->setCurrentIndex(idx);
    QMessageBox::information(
        this, tr("Save preset"),
        tr("Saved preset '%1' with %2 joint mapping(s).")
            .arg(name)
            .arg(mapping.size()));
}

void BoneRemapDialog::on_delete_preset() {
    const QString name = preset_combo_->currentText();
    if (name.isEmpty() || preset_combo_->currentIndex() == 0) return;
    auto presets = load_presets();
    if (!presets.count(name)) return;
    const auto reply = QMessageBox::question(
        this, tr("Delete preset"),
        tr("Remove preset '%1' from the global presets file?").arg(name),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply != QMessageBox::Yes) return;
    presets.erase(name);
    if (!save_presets(presets)) {
        QMessageBox::critical(
            this, tr("Delete preset"),
            tr("Could not write presets file:\n  %1").arg(presets_path()));
        return;
    }
    reload_preset_combo();
}

// T3.1 — fold every Unmapped joint with weight onto the nearest original
// bone by vertex centroid. Joints in Drop are skipped; joints in Map are
// left alone. Joints with no centroid stay Unmapped. The picked target is
// recorded with source 'geometric'.
void BoneRemapDialog::fold_unmapped_to_nearest() {
    if (bone_centroids_.empty()) return;
    int folded = 0;
    for (int j = 0; j < int(actions_.size()); ++j) {
        if (actions_[size_t(j)].action != "unmapped") continue;
        if (size_t(j) >= joint_centroids_.size()
            || !joint_centroids_[size_t(j)].first)
            continue;
        const auto& jc = joint_centroids_[size_t(j)].second;
        int best = -1;
        double best_d = std::numeric_limits<double>::infinity();
        for (int b = 0; b < int(bone_centroids_.size()); ++b) {
            if (!bone_centroids_[size_t(b)].first) continue;
            const auto& bc = bone_centroids_[size_t(b)].second;
            const double d = (jc[0] - bc[0]) * (jc[0] - bc[0])
                             + (jc[1] - bc[1]) * (jc[1] - bc[1])
                             + (jc[2] - bc[2]) * (jc[2] - bc[2]);
            if (d < best_d) {
                best_d = d;
                best = b;
            }
        }
        if (best < 0) continue;
        actions_[size_t(j)] = JointAction{"map", best};
        fold_picks_[j] = best;
        ++folded;
    }
    if (folded == 0) return;
    refresh_left();
    refresh_right();
    refresh_diagnostics();
    refresh_issue_panel();
    clear_left_link_highlight();
    clear_right_link_highlight();
    apply_filter();
}

void BoneRemapDialog::reset_all_to_auto() {
    for (int j = 0; j < int(actions_.size()); ++j) {
        auto am = auto_map_.find(j);
        if (am != auto_map_.end())
            actions_[size_t(j)] = JointAction{"map", am->second};
        else
            actions_[size_t(j)] = JointAction{"unmapped", std::nullopt};
    }
    fold_picks_.clear();
    refresh_left();
    refresh_right();
    refresh_diagnostics();
    refresh_issue_panel();
    clear_left_link_highlight();
    clear_right_link_highlight();
    apply_filter();
}

// ───────────────────── result accessors ─────────────────────

BoneRemapResult BoneRemapDialog::result() const {
    BoneRemapResult out;
    for (int j = 0; j < int(actions_.size()); ++j) {
        const JointAction& ent = actions_[size_t(j)];
        if (ent.action == "map" && ent.target) {
            out.bone_map[j] = *ent.target;
            out.source_map[j] = current_source(j);
        } else if (ent.action == "drop") {
            out.drops.insert(j);
            if (ent.target) out.drop_targets[j] = *ent.target;
        }
    }
    out.rigid_bind_bone = current_rigid_bind_bone();
    out.auto_rig = auto_rig_cb_->isChecked();
    out.diagnose_rest_pose = diag_rest_cb_->isChecked();
    return out;
}

// Alternate accept: import GLB geometry only, keep the original mesh's
// skin (transferred by position).
void BoneRemapDialog::on_keep_original_skin() {
    keep_original_skin_ = true;
    accept();
}
