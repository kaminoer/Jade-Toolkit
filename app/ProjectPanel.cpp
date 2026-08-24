#include "ProjectPanel.hpp"

#include <QAbstractItemView>
#include <QBuffer>
#include <QByteArray>
#include <QCheckBox>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QTextEdit>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Deploy.hpp"
#include "jade/GameProfiles.hpp"
#include "jade/PatcherModel.hpp"
#include "jade/Project.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

#include "BoneRemapDialog.hpp"
#include "ExePatchDialog.hpp"
#include "GuiUtil.hpp"
#include "JtmodDialog.hpp"
#include "ProjectDoc.hpp"
#include "Theme.hpp"
#include "Tooltips.hpp"

using jade::json::Value;

namespace {

const char* const COLUMNS[] = {"On", "!", "Id", "Op", "Target", "Label"};
constexpr int N_COLUMNS = int(sizeof(COLUMNS) / sizeof(COLUMNS[0]));
// firebrick / copper / grey — theme-adapted issue text colours.
const char* ROW_COLOR_ERROR() { return "#ff7878"; }
const char* ROW_COLOR_WARN() { return "#e0a878"; }
const char* ROW_COLOR_DISABLED() { return theme::DIM_TEXT; }

QString op_str(const Value& op, const char* key) {
    const Value* v = op.find(key);
    return v && v->is_str() ? qs(v->str) : QString();
}

bool op_enabled(const Value& op) {
    const Value* v = op.find("enabled");
    return !v || v->type != Value::Type::Bool || v->b;
}

// Hex string ("0x…") or number → u32; ok=false when absent.
bool op_key_field(const Value* container, const char* key, quint32& out) {
    if (!container) return false;
    const Value* v = container->find(key);
    if (!v) return false;
    if (v->is_str()) {
        bool okp = false;
        out = QString::fromStdString(v->str).toUInt(&okp, 16);
        return okp;
    }
    if (v->is_num()) {
        out = quint32(v->num);
        return true;
    }
    return false;
}

// Shared exact port of each Python operation class's target_summary().
QString target_summary(const Value& op) {
    return qs(jade::project::operation_target_summary(op));
}

// build/deploy.py — managed stock backups at <toolkit dir>/backups.
QString default_backup_dir() {
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("backups"));
}

std::filesystem::path fs_path(const QString& value) {
#ifdef _WIN32
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toStdString());
#endif
}

QString q_path(const std::filesystem::path& value) {
#ifdef _WIN32
    return QString::fromStdWString(value.wstring());
#else
    return QString::fromStdString(value.string());
#endif
}

// _guess_game_dir: best-effort default game-folder path for the picker.
QString guess_game_dir(const QString& code) {
    const QString base = QStringLiteral(
        "C:/Program Files (x86)/Ubisoft/Ubisoft Game Launcher/games");
    QString cand;
    if (code == QLatin1String("SoT"))
        cand = QStringLiteral("Prince of Persia Sands of Time");
    else if (code == QLatin1String("WW"))
        cand = QStringLiteral("Prince of Persia Warrior Within");
    else if (code == QLatin1String("T2T"))
        cand = QStringLiteral("Prince of Persia The Two Thrones");
    if (!cand.isEmpty()) {
        const QString full = QDir(base).filePath(cand);
        if (QFileInfo(full).isDir()) return full;
    }
    return QString();
}

}  // namespace

// ── BuildScanner ──

BuildScanner::BuildScanner(jade::project::ModProject project,
                           QString base_archive_path, QString output_path,
                           QObject* parent)
    : QThread(parent),
      project_(std::move(project)),
      base_(std::move(base_archive_path)),
      out_(std::move(output_path)) {}

void BuildScanner::run() {
    try {
        result_ = jade::project::build_project(
            project_, base_.toStdString(), out_.toStdString(), false,
            [this](uint64_t done, uint64_t total, const std::string& phase) {
                emit progress(int(done), int(total), qs(phase));
            });
    } catch (const std::exception& e) {
        result_ = jade::project::BuildResult();
        result_.ok = false;
        result_.issues.push_back(
            {"error", std::string("build raised: ") + e.what(), ""});
        result_.report = std::string("unexpected exception: ") + e.what();
    }
    emit result();
}

// ── JtmodScanner ──

ExePatchScanner::ExePatchScanner(jade::project::ModProject project,
                                 QString base_archive_path,
                                 jade::exepatch::Options options,
                                 QObject* parent)
    : QThread(parent),
      project_(std::move(project)),
      base_(std::move(base_archive_path)),
      options_(std::move(options)) {}

void ExePatchScanner::run() {
    try {
        result_ = jade::exepatch::make_exe_patch(
            project_, base_.toStdString(), options_,
            [this](uint64_t done, uint64_t total, const std::string& phase) {
                emit progress(int(done), int(total), qs(phase));
            },
            [this](const std::string& line) {
                emit progress(0, 0, qs(line));
            });
    } catch (const std::exception& e) {
        result_ = jade::exepatch::Result();
        result_.issues.push_back(
            {"error", std::string("exe patch raised: ") + e.what(), ""});
        result_.report = std::string("unexpected exception: ") + e.what();
    }
    emit result();
}

JtmodScanner::JtmodScanner(jade::project::ModProject project,
                           QString base_archive_path, QString out_path,
                           jade::project::JtmodExportOptions options,
                           QObject* parent)
    : QThread(parent),
      project_(std::move(project)),
      base_(std::move(base_archive_path)),
      out_(std::move(out_path)),
      options_(std::move(options)) {}

void JtmodScanner::run() {
    try {
        result_ = jade::project::export_jtmod(
            project_, base_.toStdString(), out_.toStdString(),
            options_);
    } catch (const std::exception& e) {
        result_ = jade::project::JtmodExportResult();
        result_.ok = false;
        result_.issues.push_back(
            {"error", std::string(".jtmod export raised: ") + e.what(), ""});
    }
    emit result();
}

// ── ProjectPanel ──

ProjectPanel::ProjectPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    // ── Header ──
    header_ = new QLabel(tr("No project open."));
    header_->setWordWrap(true);
    root->addWidget(header_);

    // ── Build settings row ──
    auto* settings_row = new QHBoxLayout();
    strict_inplace_cb_ = new QCheckBox(
        tr("Strict in-place (refuse builds that would grow entries past "
           "their slot)"));
    strict_inplace_cb_->setToolTip(
        tr("When ON, the build aborts if any modified entry recompresses "
           "to more bytes than its original FAT slot. Safest default.\n\n"
           "When OFF, oversize entries are appended past the original EOF "
           "and the FAT is updated to point there — the same strategy "
           "pop3KB.bf uses (sector-aligned at 0x800). The output archive "
           "grows."));
    connect(strict_inplace_cb_, &QCheckBox::checkStateChanged, this,
            &ProjectPanel::on_strict_changed);
    settings_row->addWidget(strict_inplace_cb_);
    settings_row->addStretch(1);
    root->addLayout(settings_row);

    auto* sep = new QFrame();
    sep->setFrameShape(QFrame::HLine);
    sep->setFrameShadow(QFrame::Sunken);
    root->addWidget(sep);

    // ── Toolbar ──
    auto* bar = new QHBoxLayout();
    undo_btn_ = new QPushButton(tr("Undo"));
    undo_btn_->setShortcut(QStringLiteral("Ctrl+Z"));
    connect(undo_btn_, &QPushButton::clicked, this, &ProjectPanel::on_undo);
    bar->addWidget(undo_btn_);
    redo_btn_ = new QPushButton(tr("Redo"));
    redo_btn_->setShortcut(QStringLiteral("Ctrl+Y"));
    connect(redo_btn_, &QPushButton::clicked, this, &ProjectPanel::on_redo);
    bar->addWidget(redo_btn_);

    bar->addSpacing(12);

    up_btn_ = new QPushButton(tr("Move ↑"));
    connect(up_btn_, &QPushButton::clicked, this,
            [this] { move_selected(-1); });
    bar->addWidget(up_btn_);
    down_btn_ = new QPushButton(tr("Move ↓"));
    connect(down_btn_, &QPushButton::clicked, this,
            [this] { move_selected(+1); });
    bar->addWidget(down_btn_);
    del_btn_ = new QPushButton(tr("Remove"));
    connect(del_btn_, &QPushButton::clicked, this,
            &ProjectPanel::on_remove);
    bar->addWidget(del_btn_);

    bar->addStretch(1);

    build_btn_ = new QPushButton(tr("Build…"));
    build_btn_->setMinimumWidth(120);
    connect(build_btn_, &QPushButton::clicked, this,
            &ProjectPanel::on_build);
    bar->addWidget(build_btn_);
    patcher_btn_ = new QPushButton(tr("Make EXE patch…"));
    patcher_btn_->setToolTip(
        tr("Build the patched archive, byte-compare it against the stock "
           "archive, and freeze the difference into a standalone .exe "
           "patcher with a Title / description you supply. The patcher "
           "carries no toolkit code — it just verifies the file and writes "
           "the changed bytes."));
    connect(patcher_btn_, &QPushButton::clicked, this,
            &ProjectPanel::on_make_exe_patch);
    bar->addWidget(patcher_btn_);
    jtmod_btn_ = new QPushButton(tr("Create .jtmod…"));
    jtmod_btn_->setToolTip(
        tr("Export this project as a composable .jtmod mod file for the "
           "PoP Mod Manager. Unlike the EXE patch (one frozen build), a "
           ".jtmod records what each bin's sub-entries change to, so the "
           "manager can stack several mods and flag the ones that "
           "conflict."));
    connect(jtmod_btn_, &QPushButton::clicked, this,
            &ProjectPanel::on_make_jtmod);
    bar->addWidget(jtmod_btn_);
    deploy_btn_ = new QPushButton(tr("Deploy…"));
    connect(deploy_btn_, &QPushButton::clicked, this,
            &ProjectPanel::on_deploy);
    bar->addWidget(deploy_btn_);
    restore_btn_ = new QPushButton(tr("Restore stock"));
    connect(restore_btn_, &QPushButton::clicked, this,
            &ProjectPanel::on_restore_stock);
    bar->addWidget(restore_btn_);

    root->addLayout(bar);

    // ── Splitter: ops table on top, build log on bottom ──
    auto* splitter = new QSplitter(Qt::Vertical);

    auto* ops_box = new QGroupBox(tr("Operations (applied top-to-bottom)"));
    auto* ops_lay = new QVBoxLayout(ops_box);
    table_ = new QTableWidget(0, N_COLUMNS);
    QStringList headers;
    for (const char* c : COLUMNS) headers << tr(c);
    table_->setHorizontalHeaderLabels(headers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    QHeaderView* hh = table_->horizontalHeader();
    hh->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(3, QHeaderView::Stretch);
    hh->setSectionResizeMode(4, QHeaderView::Stretch);
    // Re-evaluate Move/Remove enablement when the row selection changes.
    connect(table_, &QTableWidget::itemSelectionChanged, this,
            &ProjectPanel::update_enabled);
    // Hover an operation row for an old → new asset preview.
    table_->setMouseTracking(true);
    connect(table_, &QTableWidget::cellEntered, this,
            &ProjectPanel::on_op_hover);
    // Right-click → per-op actions (T1.4: "Edit bone map…").
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableWidget::customContextMenuRequested, this,
            &ProjectPanel::on_op_context_menu);
    ops_lay->addWidget(table_);
    splitter->addWidget(ops_box);

    auto* log_box = new QGroupBox(tr("Build log"));
    auto* log_lay = new QVBoxLayout(log_box);
    log_ = new QTextEdit();
    log_->setReadOnly(true);
    log_->setFont(QFont(QStringLiteral("Consolas"), 9));
    log_lay->addWidget(log_);
    splitter->addWidget(log_box);

    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    // ── Status footer ──
    status_ = new QLabel(QString());
    root->addWidget(status_);

    refresh();
}

// ────────────────────────── public ──────────────────────────

void ProjectPanel::set_project(ProjectDoc* project,
                               const QString& base_archive_path) {
    if (project_)
        disconnect(project_, &ProjectDoc::changed, this,
                   &ProjectPanel::refresh);
    project_ = project;
    if (!base_archive_path.isEmpty() || !project)
        base_archive_path_ = base_archive_path;
    base_match_cache_valid_ = false;
    op_tooltip_cache_.clear();
    if (project)
        connect(project, &ProjectDoc::changed, this,
                &ProjectPanel::refresh);
    refresh();
}

void ProjectPanel::set_base_archive_path(const QString& path) {
    base_archive_path_ = path;
    base_match_cache_valid_ = false;
    refresh();
}

void ProjectPanel::set_bigfile(std::shared_ptr<jade::BigFile> bigfile) {
    bigfile_ = std::move(bigfile);
    refresh();
}

// ────────────────────────── refresh ──────────────────────────

void ProjectPanel::refresh() {
    ProjectDoc* proj = project_;
    if (!proj) {
        header_->setText(tr("No project open.\nFile → New Project / Open "
                            "Project to begin."));
        table_->setRowCount(0);
        status_->setText(QString());
        update_enabled();
        return;
    }

    const QString dirty =
        proj->is_dirty() ? QStringLiteral("*") : QString();
    const QString loc =
        proj->path.isEmpty() ? tr("  ·  unsaved")
                             : QStringLiteral("  ·  %1").arg(proj->path);
    const QString base =
        proj->base.archive_name.empty()
            ? QString()
            : QStringLiteral("  ·  base: %1  (%2)")
                  .arg(qs(proj->base.archive_name),
                       qs(proj->base.game));
    QString base_ok;
    if (!base_archive_path_.isEmpty())
        base_ok = QStringLiteral("  ·  base archive: %1 (%2)")
                      .arg(QFileInfo(base_archive_path_).fileName(),
                           base_archive_matches() ? tr("matches")
                                                  : tr("MISMATCH"));
    else
        base_ok = tr("  ·  base archive: not loaded");
    header_->setText(QStringLiteral("<b>%1%2</b>%3%4%5")
                         .arg(proj->name.isEmpty() ? tr("(unnamed)")
                                                   : proj->name,
                              dirty, loc, base, base_ok));

    // Sync strict_inplace checkbox without re-emitting changed
    strict_inplace_cb_->blockSignals(true);
    strict_inplace_cb_->setChecked(proj->build.strict_inplace);
    strict_inplace_cb_->blockSignals(false);

    // Run cheap live validation so per-row badges reflect current state.
    run_validation();

    // Rebuild table
    const QString prev_sel = selected_op_id();
    table_->setRowCount(int(proj->operations.size()));
    for (int row = 0; row < int(proj->operations.size()); ++row) {
        const Value& op = proj->operations[size_t(row)];
        const QString op_id = op_str(op, "id");
        const bool enabled = op_enabled(op);
        // 0 — enabled checkbox (real QCheckBox in a wrapper widget).
        auto* cb = new QCheckBox();
        cb->setChecked(enabled);
        connect(cb, &QCheckBox::toggled, this,
                [this, op_id](bool checked) { on_toggle(op_id, checked); });
        auto* holder = new QWidget();
        auto* hl = new QHBoxLayout(holder);
        hl->setContentsMargins(8, 0, 0, 0);
        hl->addWidget(cb);
        hl->addStretch(1);
        table_->setCellWidget(row, 0, holder);
        // 1 — issue badge: ✗ (error) / ⚠ (warning) / nothing.
        std::vector<jade::project::BuildIssue> op_issues;
        if (enabled) {
            auto it = issues_by_op_.find(op_id);
            if (it != issues_by_op_.end()) op_issues = it.value();
        }
        int n_err = 0, n_warn = 0;
        for (const auto& i : op_issues) {
            if (i.level == "error") ++n_err;
            else if (i.level == "warning") ++n_warn;
        }
        const QString badge_text = n_err ? QStringLiteral("✗")
                                   : n_warn ? QStringLiteral("⚠")
                                            : QString();
        auto* badge = new QTableWidgetItem(badge_text);
        badge->setTextAlignment(Qt::AlignCenter);
        if (!op_issues.empty()) {
            QStringList tip;
            for (const auto& i : op_issues)
                tip << QStringLiteral("[%1] %2").arg(qs(i.level),
                                                     qs(i.message));
            badge->setToolTip(tip.join(QLatin1Char('\n')));
        }
        table_->setItem(row, 1, badge);
        table_->setItem(row, 2, new QTableWidgetItem(op_id));
        table_->setItem(row, 3, new QTableWidgetItem(op_str(op, "op")));
        table_->setItem(row, 4, new QTableWidgetItem(target_summary(op)));
        table_->setItem(row, 5,
                        new QTableWidgetItem(op_str(op, "label")));

        // Row colouring: disabled → grey; error → red; warning → copper.
        QColor colour;
        bool have_colour = true;
        if (!enabled) colour = QColor(ROW_COLOR_DISABLED());
        else if (n_err) colour = QColor(ROW_COLOR_ERROR());
        else if (n_warn) colour = QColor(ROW_COLOR_WARN());
        else have_colour = false;
        if (have_colour)
            for (int c = 1; c < N_COLUMNS; ++c)
                table_->item(row, c)->setForeground(colour);
        // Per-row tooltip so the modder doesn't have to find the badge:
        if (!op_issues.empty())
            for (int c = 1; c < N_COLUMNS; ++c)
                table_->item(row, c)->setToolTip(badge->toolTip());
    }

    if (!prev_sel.isEmpty()) select_op(prev_sel);

    const int loadable = int(proj->enabled_operations().size());
    int total_err = 0, total_warn = 0;
    for (auto it = issues_by_op_.cbegin(); it != issues_by_op_.cend(); ++it)
        for (const auto& i : it.value()) {
            if (i.level == "error") ++total_err;
            else if (i.level == "warning") ++total_warn;
        }
    for (const auto& i : global_issues_) {
        if (i.level == "error") ++total_err;
        else if (i.level == "warning") ++total_warn;
    }
    const QString val_part =
        (bigfile_ || !issues_by_op_.isEmpty() || !global_issues_.empty())
            ? tr("  ·  validation: %1 error(s), %2 warning(s)")
                  .arg(total_err)
                  .arg(total_warn)
            : tr("  ·  validation: open base archive to enable");
    status_->setText(
        tr("%1 operation(s), %2 enabled. Undo: %3  ·  Redo: %4%5")
            .arg(proj->operations.size())
            .arg(loadable)
            .arg(proj->can_undo() ? tr("available") : tr("empty"),
                 proj->can_redo() ? tr("available") : tr("empty"),
                 val_part));

    // Global (non-op-scoped) issue summary as a status tooltip.
    if (!global_issues_.empty()) {
        QStringList tips;
        for (const auto& i : global_issues_)
            tips << QStringLiteral("[%1] %2").arg(qs(i.level),
                                                  qs(i.message));
        status_->setToolTip(tips.join(QLatin1Char('\n')));
    } else {
        status_->setToolTip(QString());
    }

    update_enabled();
}

// ────────────────────────── validation ──────────────────────────

// Run the cheap live=True validator and store results for the renderer.
void ProjectPanel::run_validation() {
    ProjectDoc* proj = project_;
    if (!proj || !bigfile_ || base_archive_path_.isEmpty()) {
        issues_by_op_.clear();
        global_issues_.clear();
        return;
    }
    static QString last_sig;
    Value ops_arr = jade::json::make_arr();
    ops_arr.arr = proj->operations;
    const QString sig =
        base_archive_path_ + QLatin1Char('|')
        + QString::fromStdString(jade::json::dump(ops_arr));
    if (sig == last_sig) return;  // cached — ops unchanged
    last_sig = sig;
    issues_by_op_.clear();
    global_issues_.clear();
    std::vector<std::string> log;
    std::vector<jade::project::BuildIssue> issues;
    try {
        issues = jade::project::validate_project(
            proj->to_core(), base_archive_path_.toStdString(), log,
            /*live=*/true);
    } catch (const std::exception&) {
        return;
    }
    for (const auto& i : issues) {
        if (i.op_id.empty())
            global_issues_.push_back(i);
        else
            issues_by_op_[qs(i.op_id)].push_back(i);
    }
}

void ProjectPanel::update_enabled() {
    ProjectDoc* proj = project_;
    const bool busy = scanner_ != nullptr || patcher_scanner_ != nullptr
                      || jtmod_scanner_ != nullptr;
    const bool has_proj = proj != nullptr;
    const bool has_sel = selected_row() >= 0;
    undo_btn_->setEnabled(has_proj && proj->can_undo() && !busy);
    redo_btn_->setEnabled(has_proj && proj->can_redo() && !busy);
    up_btn_->setEnabled(has_proj && has_sel && !busy);
    down_btn_->setEnabled(has_proj && has_sel && !busy);
    del_btn_->setEnabled(has_proj && has_sel && !busy);
    const bool can_build = has_proj && !busy
                           && !base_archive_path_.isEmpty()
                           && base_archive_matches()
                           && !proj->enabled_operations().empty();
    build_btn_->setEnabled(can_build);
    patcher_btn_->setEnabled(can_build);
    jtmod_btn_->setEnabled(can_build);
    deploy_btn_->setEnabled(!last_build_output_.isEmpty() && !busy);
    restore_btn_->setEnabled(has_proj && !busy);
}

// ───────────────────────── hover preview ─────────────────────────

long long ProjectPanel::entry_index(quint32 entry_key) const {
    if (!bigfile_) return -1;
    for (const auto& [i, fi] : bigfile_->files)
        if (fi.key == entry_key) return i;
    return -1;
}

QString ProjectPanel::asset_path(const QString& source) const {
    if (source.isEmpty() || !project_) return QString();
    return project_->resolve_asset(source);
}

QString ProjectPanel::img_tag(const QPixmap& pixmap, int maxsz) {
    const QPixmap scaled = pixmap.scaled(maxsz, maxsz, Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation);
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QBuffer::WriteOnly);
    scaled.save(&buf, "PNG");
    return QStringLiteral("<img src=\"data:image/png;base64,%1\">")
        .arg(QString::fromLatin1(ba.toBase64()));
}

QPixmap ProjectPanel::texture_pixmap_from_bf(quint32 entry_key,
                                             quint32 sub_key) const {
    const long long idx = entry_index(entry_key);
    if (idx < 0 || base_archive_path_.isEmpty()) return QPixmap();
    // decode_texture_preview equivalent: decompress the entry, pick the
    // largest texture variant under sub_key, decode to RGBA.
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bigfile_->read_data(uint32_t(idx)));
        if (!r.ok) return QPixmap();
        const std::vector<jade::SubEntry> subs =
            jade::walk_sub_entries(r.data);
        const jade::SubEntry* best = nullptr;
        size_t best_len = 0;
        jade::TexInfo best_ti;
        for (const jade::SubEntry& s : subs) {
            if (s.key != sub_key) continue;
            if (!jade::is_texture_entry(s.data.data(), s.data.size()))
                continue;
            const jade::TexInfo ti =
                jade::parse_texture(s.data.data(), s.data.size());
            if (!ti.valid) continue;
            const size_t pixlen = s.data.size() > ti.pix_start
                                      ? s.data.size() - ti.pix_start
                                      : 0;
            if (!best || pixlen > best_len) {
                best = &s;
                best_len = pixlen;
                best_ti = ti;
            }
        }
        if (!best) return QPixmap();
        const std::vector<uint8_t>* pal =
            jade::palette_for_texture(best_ti, subs);
        const std::vector<uint8_t> rgba = jade::decode_texture(
            best->data.data(), best->data.size(), best_ti,
            pal ? pal->data() : nullptr, pal ? pal->size() : 0);
        if (rgba.empty()) return QPixmap();
        QImage img(rgba.data(), int(best_ti.width), int(best_ti.height),
                   int(best_ti.width) * 4, QImage::Format_RGBA8888);
        return QPixmap::fromImage(img.copy());
    } catch (const std::exception&) {
        return QPixmap();
    }
}

// HTML 'old asset ---> new asset' preview for one operation.
QString ProjectPanel::build_op_tooltip(const Value& op) const {
    const QString ot = op_str(op, "op");
    const Value* tgt = op.find("target");
    const Value* prm = op.find("params");
    quint32 entry_key = 0, sub_key = 0;
    const bool has_entry = op_key_field(tgt, "entry_key", entry_key);
    const bool has_sub = op_key_field(tgt, "sub_key", sub_key);
    const QString source =
        prm && prm->find("source") && prm->find("source")->is_str()
            ? qs(prm->find("source")->str)
            : QString();
    if (ot == QLatin1String("replace_texture") && has_entry && has_sub) {
        const QPixmap old = texture_pixmap_from_bf(entry_key, sub_key);
        const QString new_path = asset_path(source);
        const QPixmap nw =
            new_path.isEmpty() ? QPixmap() : QPixmap(new_path);
        const QString old_html =
            !old.isNull() ? img_tag(old)
                          : QStringLiteral("<i>texture %1</i>")
                                .arg(qs(hex_key_lower(sub_key)));
        const QString new_html =
            !nw.isNull() ? img_tag(nw)
            : !new_path.isEmpty()
                ? QFileInfo(new_path).fileName()
                : QStringLiteral("<i>(new texture)</i>");
        return QStringLiteral("<div><b>Replace texture</b><br>%1"
                              " &nbsp;---&gt;&nbsp; %2</div>")
            .arg(old_html, new_html);
    }
    if (ot == QLatin1String("replace_mesh") && has_sub) {
        const QString new_path = asset_path(source);
        const QString nw = !new_path.isEmpty()
                               ? QFileInfo(new_path).fileName()
                               : QStringLiteral("(new mesh)");
        return QStringLiteral("<div><b>Replace mesh</b><br>geo %1 "
                              "&nbsp;---&gt;&nbsp; %2</div>")
            .arg(qs(hex_key_lower(sub_key)), nw);
    }
    if (ot == QLatin1String("stub_mesh") && has_sub)
        return QStringLiteral("<div><b>Stub mesh</b><br>geo %1 "
                              "&nbsp;---&gt;&nbsp; <i>hidden (null "
                              "template)</i></div>")
            .arg(qs(hex_key_lower(sub_key)));
    const QString label = op_str(op, "label");
    return QStringLiteral("<div>%1</div>")
        .arg(label.isEmpty() ? ot : label);
}

// ───────────────────────── context menu ─────────────────────────

// Right-click an op row → per-op actions menu. Today only replace_mesh
// exposes "Edit bone map…" (T1.4).
void ProjectPanel::on_op_context_menu(const QPoint& pos) {
    ProjectDoc* proj = project_;
    if (!proj) return;
    QTableWidgetItem* item = table_->itemAt(pos);
    if (!item) return;
    const int row = item->row();
    QTableWidgetItem* id_item = table_->item(row, 2);
    if (!id_item) return;
    Value* op = proj->get_operation(id_item->text());
    if (!op) return;
    QMenu menu(this);
    QAction* action_bone_map = nullptr;
    const Value* prm = op->find("params");
    const bool has_source = prm && prm->find("source")
                            && prm->find("source")->is_str()
                            && !prm->find("source")->str.empty();
    if (op_str(*op, "op") == QLatin1String("replace_mesh") && has_source)
        action_bone_map = menu.addAction(tr("Edit bone map…"));
    if (menu.actions().isEmpty()) return;
    QAction* chosen = menu.exec(table_->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == action_bone_map) edit_bone_map(*op);
}

// Open the BoneRemapDialog seeded from the op's persisted map. Saves on
// Accept. Requires a loaded base archive.
void ProjectPanel::edit_bone_map(Value& op) {
    if (base_archive_path_.isEmpty() || !bigfile_) {
        QMessageBox::warning(
            this, tr("Edit bone map"),
            tr("Load the base archive first (File → Open BF). The dialog "
               "needs the target skeleton to compute the auto-map."));
        return;
    }
    const Value* tgt = op.find("target");
    const Value* prm = op.find("params");
    quint32 entry_key = 0, sub_key = 0;
    op_key_field(tgt, "entry_key", entry_key);
    op_key_field(tgt, "sub_key", sub_key);
    const long long entry_idx = entry_index(entry_key);
    if (entry_idx < 0) {
        QMessageBox::warning(
            this, tr("Edit bone map"),
            tr("Entry %1 not found in the loaded BigFile — wrong archive?")
                .arg(qs(hex_key_lower(entry_key))));
        return;
    }
    const QString source =
        prm && prm->find("source") && prm->find("source")->is_str()
            ? qs(prm->find("source")->str)
            : QString();
    const QString glb_path = asset_path(source);
    if (glb_path.isEmpty() || !QFileInfo::exists(glb_path)) {
        QMessageBox::warning(
            this, tr("Edit bone map"),
            tr("GLB asset missing for this op:\n  %1").arg(source));
        return;
    }
    jade::patcher::AnalyzeBoneResult bm;
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bigfile_->read_data(uint32_t(entry_idx)));
        QFile gf(glb_path);
        if (!r.ok || !gf.open(QIODevice::ReadOnly))
            throw std::runtime_error("could not read entry / GLB");
        const QByteArray raw = gf.readAll();
        bm = jade::patcher::analyze_bone_mapping(
            r.data, sub_key,
            std::vector<uint8_t>(raw.constData(),
                                 raw.constData() + raw.size()));
    } catch (const std::exception& e) {
        QMessageBox::critical(
            this, tr("Edit bone map"),
            tr("Could not analyse bone mapping:\n  %1").arg(e.what()));
        return;
    }
    if (!bm.ok || bm.dp_bone_names.empty()) {
        QMessageBox::information(
            this, tr("Edit bone map"),
            tr("Target mesh has no skeleton — no bone mapping to edit."));
        return;
    }

    // Seed the dialog from the analysis + the op's persisted state.
    BoneRemapInit init;
    for (const auto& n : bm.glb_joint_names) init.glb_joint_names << qs(n);
    for (const auto& n : bm.dp_bone_names) init.dp_bone_names << qs(n);
    init.auto_map = bm.auto_map;
    init.auto_map_source = bm.auto_map_source;
    init.joint_stats = bm.joint_stats;
    init.joint_centroids = bm.joint_centroids;
    init.bone_centroids = bm.bone_centroids;
    init.mesh_diagonal = bm.mesh_diagonal;
    init.joint_rest_positions = bm.joint_rest_positions;
    init.bone_rest_positions = bm.bone_rest_positions;
    init.orig_bone_weight_shares = bm.orig_bone_weight_shares;
    auto read_int_map = [prm](const char* key, std::map<int, int>& out) {
        const Value* m = prm ? prm->find(key) : nullptr;
        if (!m || !m->is_obj()) return;
        for (const auto& [k, v] : m->obj)
            if (v.is_num()) out[QString::fromStdString(k).toInt()] =
                int(v.num);
    };
    read_int_map("bone_map", init.initial_map);
    read_int_map("drop_targets", init.initial_drop_targets);
    if (const Value* m = prm ? prm->find("bone_map_source") : nullptr)
        if (m->is_obj())
            for (const auto& [k, v] : m->obj)
                if (v.is_str())
                    init.initial_source[QString::fromStdString(k).toInt()] =
                        v.str;
    if (const Value* d = prm ? prm->find("bone_drops") : nullptr)
        if (d->is_arr())
            for (const Value& v : d->arr)
                if (v.is_num()) init.initial_drops.insert(int(v.num));
    if (prm) {
        init.initial_rigid_bind_bone =
            int(prm->get_int("rigid_bind_bone", 0));
        const Value* ar = prm->find("auto_rig");
        init.initial_auto_rig =
            ar && ar->type == Value::Type::Bool && ar->b;
        const Value* dr = prm->find("diagnose_rest_pose");
        init.initial_diagnose_rest_pose =
            dr && dr->type == Value::Type::Bool && dr->b;
    }

    BoneRemapDialog dlg(init, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const BoneRemapResult res = dlg.result();
    if (res.bone_map == init.initial_map
        && res.source_map == init.initial_source
        && res.drops == init.initial_drops
        && res.rigid_bind_bone == init.initial_rigid_bind_bone
        && res.auto_rig == init.initial_auto_rig
        && res.diagnose_rest_pose == init.initial_diagnose_rest_pose
        && res.drop_targets == init.initial_drop_targets) {
        append_log(tr("[%1] bone map unchanged").arg(op_str(op, "id")));
        return;
    }
    // Mutate the op in-place under undo (the _push_undo + _touch pattern).
    project_->push_undo();
    Value* params = &op.obj["params"];
    if (!params->is_obj()) *params = jade::json::make_obj();
    auto write_int_map = [params](const char* key,
                                  const std::map<int, int>& m) {
        params->obj.erase(key);
        if (m.empty()) return;
        Value obj = jade::json::make_obj();
        for (const auto& [k, v] : m)
            obj.obj[std::to_string(k)] = jade::json::make_num(v);
        params->obj[key] = std::move(obj);
    };
    write_int_map("bone_map", res.bone_map);
    write_int_map("drop_targets", res.drop_targets);
    params->obj.erase("bone_map_source");
    if (!res.source_map.empty()) {
        Value obj = jade::json::make_obj();
        for (const auto& [k, v] : res.source_map)
            obj.obj[std::to_string(k)] = jade::json::make_str(v);
        params->obj["bone_map_source"] = std::move(obj);
    }
    params->obj.erase("bone_drops");
    if (!res.drops.empty()) {
        Value arr = jade::json::make_arr();
        for (int j : res.drops) arr.arr.push_back(jade::json::make_num(j));
        params->obj["bone_drops"] = std::move(arr);
    }
    params->obj.erase("rigid_bind_bone");
    if (res.rigid_bind_bone != 0)
        params->obj["rigid_bind_bone"] =
            jade::json::make_num(res.rigid_bind_bone);
    params->obj.erase("auto_rig");
    if (res.auto_rig) params->obj["auto_rig"] = jade::json::make_bool(true);
    params->obj.erase("diagnose_rest_pose");
    if (res.diagnose_rest_pose)
        params->obj["diagnose_rest_pose"] = jade::json::make_bool(true);
    params->obj.erase("keep_original_skin");
    if (dlg.keep_original_skin())
        params->obj["keep_original_skin"] = jade::json::make_bool(true);
    project_->touch();
    int n_user = 0, n_name = 0, n_geom = 0;
    for (const auto& [j, s] : res.source_map) {
        if (s == "user") ++n_user;
        else if (s == "name") ++n_name;
        else if (s == "geometric") ++n_geom;
    }
    append_log(
        tr("[%1] bone map updated: %2 mapped, %3 dropped (%4 user, %5 "
           "name, %6 geom)   rigid-bind=bone[%7]%8%9")
            .arg(op_str(op, "id"))
            .arg(res.bone_map.size())
            .arg(res.drops.size())
            .arg(n_user)
            .arg(n_name)
            .arg(n_geom)
            .arg(res.rigid_bind_bone)
            .arg(res.auto_rig ? tr("   auto-rig=on") : QString(),
                 res.diagnose_rest_pose ? tr("   diagnose-rest-pose=on")
                                        : QString()));
}

void ProjectPanel::on_op_hover(int row, int col) {
    ProjectDoc* proj = project_;
    if (!proj) return;
    QTableWidgetItem* id_item = table_->item(row, 2);
    if (!id_item) {
        tooltips::hide_persistent_tooltip();
        return;
    }
    Value* op = proj->get_operation(id_item->text());
    if (!op) {
        tooltips::hide_persistent_tooltip();
        return;
    }
    const QString op_id = id_item->text();
    QString html = op_tooltip_cache_.value(op_id);
    if (html.isEmpty()) {
        html = build_op_tooltip(*op);
        op_tooltip_cache_[op_id] = html;
    }
    if (!html.isEmpty()) {
        QTableWidgetItem* item = table_->item(row, col);
        const QRect rect = item ? table_->visualItemRect(item)
                                : table_->viewport()->rect();
        tooltips::show_persistent_tooltip(QCursor::pos(), html,
                                          table_->viewport(), rect);
    }
}

// Whether the base archive on disk matches the project's recorded hash.
// Cached — base.matches() SHA-256s the whole file.
bool ProjectPanel::base_archive_matches() {
    ProjectDoc* proj = project_;
    if (!proj || base_archive_path_.isEmpty()) return false;
    if (base_match_cache_valid_
        && base_match_cache_path_ == base_archive_path_)
        return base_match_cache_result_;
    const bool result =
        proj->base.matches(base_archive_path_.toStdString());
    base_match_cache_valid_ = true;
    base_match_cache_path_ = base_archive_path_;
    base_match_cache_result_ = result;
    return result;
}

int ProjectPanel::selected_row() const {
    const auto ranges = table_->selectedRanges();
    return ranges.isEmpty() ? -1 : ranges.first().topRow();
}

QString ProjectPanel::selected_op_id() const {
    const int r = selected_row();
    if (r < 0) return QString();
    // Column 2 holds op id (0=On, 1=badge, 2=Id, …).
    QTableWidgetItem* it = table_->item(r, 2);
    return it ? it->text() : QString();
}

void ProjectPanel::select_op(const QString& op_id) {
    for (int r = 0; r < table_->rowCount(); ++r) {
        QTableWidgetItem* it = table_->item(r, 2);
        if (it && it->text() == op_id) {
            table_->selectRow(r);
            return;
        }
    }
}

// ────────────────────────── slots ──────────────────────────

void ProjectPanel::on_toggle(const QString& op_id, bool enabled) {
    if (project_) project_->set_enabled(op_id, enabled);
}

void ProjectPanel::on_strict_changed(Qt::CheckState state) {
    if (!project_) return;
    const bool new_val = state != Qt::Unchecked;
    if (project_->build.strict_inplace == new_val) return;
    project_->build.strict_inplace = new_val;
    project_->touch();
}

void ProjectPanel::on_remove() {
    const QString oid = selected_op_id();
    if (!oid.isEmpty() && project_) project_->remove_operation(oid);
}

void ProjectPanel::move_selected(int delta) {
    const QString oid = selected_op_id();
    if (oid.isEmpty() || !project_) return;
    int idx = -1;
    for (int i = 0; i < int(project_->operations.size()); ++i) {
        const Value* id = project_->operations[size_t(i)].find("id");
        if (id && id->is_str() && qs(id->str) == oid) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return;
    project_->move_operation(oid, idx + delta);
    select_op(oid);
}

void ProjectPanel::on_undo() {
    if (project_) project_->undo();
}

void ProjectPanel::on_redo() {
    if (project_) project_->redo();
}

// ────────────────────────── build ──────────────────────────

void ProjectPanel::on_build() {
    ProjectDoc* proj = project_;
    if (!proj || scanner_ || patcher_scanner_ || jtmod_scanner_) return;
    if (base_archive_path_.isEmpty()) {
        QMessageBox::warning(
            this, tr("Build"),
            tr("No base archive loaded. Use File → Open BF to load the "
               "game archive this project targets."));
        return;
    }
    if (!proj->base.matches(base_archive_path_.toStdString())) {
        QMessageBox::critical(
            this, tr("Build"),
            tr("The loaded base archive does not match this project (size "
               "or SHA-256 differs). Build aborted."));
        return;
    }
    if (proj->enabled_operations().empty()) {
        QMessageBox::information(this, tr("Build"),
                                 tr("No enabled operations to build."));
        return;
    }

    // Suggest <project dir>/<output_name> if saved, else next to base.
    const QString default_dir =
        !proj->path.isEmpty() ? proj->path
                              : QFileInfo(base_archive_path_).path();
    const QString default_out = QDir(default_dir).filePath(
        proj->build.output_name.empty() ? QStringLiteral("patched.bf")
                                        : qs(proj->build.output_name));
    const QString out_path = QFileDialog::getSaveFileName(
        this, tr("Build patched archive"), default_out,
        tr("BigFile (*.bf)"));
    if (out_path.isEmpty()) return;

    log_->clear();
    append_log(tr("=== Build started for '%1' ===").arg(proj->name));
    append_log(tr("base   : %1").arg(base_archive_path_));
    append_log(tr("output : %1").arg(out_path));

    scanner_ = new BuildScanner(proj->to_core(), base_archive_path_,
                                out_path, this);
    connect(scanner_, &BuildScanner::progress, this,
            &ProjectPanel::on_progress);
    connect(scanner_, &BuildScanner::result, this,
            &ProjectPanel::on_result);
    connect(scanner_, &BuildScanner::finished, this,
            &ProjectPanel::on_thread_done);
    scanner_->start();
    update_enabled();
}

void ProjectPanel::on_progress(int done, int total, const QString& phase) {
    status_->setText(QStringLiteral("%1: %2/%3").arg(phase).arg(done).arg(total));
}

void ProjectPanel::on_result() {
    const jade::project::BuildResult result = scanner_->take_result();
    for (const QString& line : qs(result.report).split(QLatin1Char('\n')))
        append_log(line);
    if (result.ok) {
        last_build_output_ = qs(result.output_path);
        append_log(tr("\nBuild OK → %1").arg(last_build_output_));
        QMessageBox::information(
            this, tr("Build"),
            tr("Build succeeded.\n\n%1").arg(last_build_output_));
    } else {
        last_build_output_.clear();
        QStringList errs;
        for (const auto& i : result.issues) {
            if (i.level != "error") continue;
            QString text = QStringLiteral("  [%1]").arg(qs(i.level));
            if (!i.op_id.empty()) text += QLatin1Char(' ') + qs(i.op_id);
            errs << text + QLatin1Char(' ') + qs(i.message);
        }
        QMessageBox::critical(
            this, tr("Build failed"),
            tr("Build did not complete:\n\n%1")
                .arg(errs.isEmpty() ? tr("(no errors recorded)")
                                    : errs.join(QLatin1Char('\n'))));
    }
}

void ProjectPanel::on_thread_done() {
    if (scanner_) scanner_->deleteLater();
    scanner_ = nullptr;
    status_->setText(QString());
    update_enabled();
}

// ─────────────────────── make exe patch ──────────────────────

void ProjectPanel::on_make_exe_patch() {
    ProjectDoc* proj = project_;
    if (!proj || scanner_ || patcher_scanner_ || jtmod_scanner_) return;
    if (base_archive_path_.isEmpty()) {
        QMessageBox::warning(
            this, tr("Make EXE patch"),
            tr("No base archive loaded. Use File → Open BF to load the "
               "game archive this project targets."));
        return;
    }
    if (!proj->base.matches(base_archive_path_.toStdString())) {
        QMessageBox::critical(
            this, tr("Make EXE patch"),
            tr("The loaded base archive does not match this project (size "
               "or SHA-256 differs). Aborted."));
        return;
    }
    if (proj->enabled_operations().empty()) {
        QMessageBox::information(this, tr("Make EXE patch"),
                                 tr("No enabled operations to patch."));
        return;
    }
    const QString stub = QDir(QCoreApplication::applicationDirPath())
                             .filePath(QStringLiteral(
                                 "jade_bytepatch_stub.exe"));
    if (!QFileInfo(stub).isFile()) {
        QMessageBox::critical(
            this, tr("Make EXE patch"),
            tr("The bundled patcher runtime is missing:\n\n%1\n\n"
               "Rebuild jade_gui so jade_bytepatch_stub.exe is produced "
               "beside it.").arg(stub));
        return;
    }
    ExePatchDialog dlg(proj, base_archive_path_, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const ExePatchDialog::Values v = dlg.values();
    if (v.out_exe_path.isEmpty()) return;

    jade::exepatch::Options options;
    options.title = v.title.toStdString();
    options.description = v.description.toStdString();
    options.author = v.author.toStdString();
    options.version = v.version.toStdString();
    options.out_exe_path = v.out_exe_path.toStdString();
    options.icon_path = v.icon_path.toStdString();
    options.image_path = v.image_path.toStdString();
    options.stub_exe_path = stub.toStdString();
    for (const QString& name : v.accepted_names)
        options.accepted_names.push_back(name.toStdString());

    log_->clear();
    append_log(tr("=== EXE patch build for '%1' ===").arg(proj->name));
    append_log(tr("base   : %1").arg(base_archive_path_));
    append_log(tr("output : %1").arg(v.out_exe_path));
    append_log(tr("title  : %1  ·  author: %2  ·  version: %3")
                   .arg(v.title, v.author, v.version));
    append_log(tr("targets: %1").arg(v.accepted_names.join(", ")));
    if (!v.icon_path.isEmpty()) append_log(tr("icon   : %1").arg(v.icon_path));
    if (!v.image_path.isEmpty())
        append_log(tr("image  : %1").arg(v.image_path));

    patcher_scanner_ = new ExePatchScanner(
        proj->to_core(), base_archive_path_, std::move(options), this);
    connect(patcher_scanner_, &ExePatchScanner::progress, this,
            &ProjectPanel::on_patch_progress);
    connect(patcher_scanner_, &ExePatchScanner::result, this,
            &ProjectPanel::on_patch_result);
    connect(patcher_scanner_, &ExePatchScanner::finished, this,
            &ProjectPanel::on_patch_thread_done);
    patcher_scanner_->start();
    update_enabled();
}

void ProjectPanel::on_patch_progress(int done, int total,
                                     const QString& phase) {
    // A (0, 0, msg) tuple is a log line, not a progress tick.
    if (total == 0 && done == 0)
        append_log(phase);
    else
        status_->setText(
            QStringLiteral("%1: %2/%3").arg(phase).arg(done).arg(total));
}

void ProjectPanel::on_patch_result() {
    const jade::exepatch::Result result = patcher_scanner_->take_result();
    for (const QString& line : qs(result.report).split(QLatin1Char('\n')))
        append_log(line);
    if (result.ok) {
        const QString out = qs(result.exe_path);
        append_log(tr("\nEXE patch OK → %1").arg(out));
        QMessageBox box(this);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle(tr("Make EXE patch"));
        box.setText(
            tr("Standalone patcher built.\n\n%1\n\n"
               "Compiler: %2  ·  changed bytes: %3  ·  segments: %4/%5"
               "  ·  payload: %6 B")
                .arg(out, qs(result.compiler))
                .arg(result.changed_bytes)
                .arg(result.fwd_segments)
                .arg(result.rev_segments)
                .arg(result.patch_size));
        QPushButton* open_btn =
            box.addButton(tr("Open folder"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Ok);
        box.exec();
        if (box.clickedButton() == open_btn && !out.isEmpty())
            reveal_in_explorer(out);
    } else {
        QStringList errs;
        for (const auto& issue : result.issues)
            if (issue.level == "error") errs << qs(issue.message);
        QMessageBox::critical(
            this, tr("Make EXE patch failed"),
            tr("The patcher could not be built:\n\n%1")
                .arg(errs.isEmpty() ? tr("(no errors recorded)")
                                    : errs.join(QLatin1Char('\n'))));
    }
}

void ProjectPanel::on_patch_thread_done() {
    if (patcher_scanner_) patcher_scanner_->deleteLater();
    patcher_scanner_ = nullptr;
    status_->setText(QString());
    update_enabled();
}

// ─────────────────────── create .jtmod ───────────────────────

void ProjectPanel::on_make_jtmod() {
    ProjectDoc* proj = project_;
    if (!proj || scanner_ || patcher_scanner_ || jtmod_scanner_) return;
    if (base_archive_path_.isEmpty()) {
        QMessageBox::warning(
            this, tr("Create .jtmod"),
            tr("No base archive loaded. Use File → Open BF to load the "
               "game archive this project targets."));
        return;
    }
    if (!proj->base.matches(base_archive_path_.toStdString())) {
        QMessageBox::critical(
            this, tr("Create .jtmod"),
            tr("The loaded base archive does not match this project (size "
               "or SHA-256 differs). Aborted."));
        return;
    }
    if (proj->enabled_operations().empty()) {
        QMessageBox::information(this, tr("Create .jtmod"),
                                 tr("No enabled operations to export."));
        return;
    }

    JtmodDialog dlg(proj, base_archive_path_, this);
    if (dlg.exec() != QDialog::Accepted) return;
    const JtmodDialog::Values v = dlg.values();
    if (v.out_path.isEmpty()) return;

    log_->clear();
    append_log(tr("=== .jtmod export for '%1' ===").arg(v.name));
    append_log(tr("game   : %1  ·  %2")
                   .arg(qs(proj->base.game), qs(proj->base.archive_name)));
    append_log(tr("base   : %1").arg(base_archive_path_));
    append_log(tr("output : %1").arg(v.out_path));
    append_log(tr("author : %1  ·  version: %2").arg(v.author, v.version));
    if (!v.image_path.isEmpty())
        append_log(tr("image  : %1").arg(v.image_path));
    jade::project::JtmodExportOptions options;
    options.title = v.name.toStdString();
    options.author = v.author.toStdString();
    options.version = v.version.toStdString();
    options.description = v.description.toStdString();
    options.image_path = v.image_path.toStdString();
    options.created_iso = ProjectDoc::iso_now().toStdString();
    options.validate = true;

    jtmod_scanner_ = new JtmodScanner(proj->to_core(), base_archive_path_,
                                      v.out_path, std::move(options),
                                      this);
    connect(jtmod_scanner_, &JtmodScanner::progress, this,
            &ProjectPanel::on_patch_progress);
    connect(jtmod_scanner_, &JtmodScanner::result, this,
            &ProjectPanel::on_jtmod_result);
    connect(jtmod_scanner_, &JtmodScanner::finished, this,
            &ProjectPanel::on_jtmod_thread_done);
    jtmod_scanner_->start();
    update_enabled();
}

void ProjectPanel::on_jtmod_result() {
    const jade::project::JtmodExportResult result =
        jtmod_scanner_->take_result();
    for (const std::string& ln : result.log) append_log(qs(ln));
    if (!result.report.empty()) append_log(qs(result.report));
    if (result.ok) {
        const QString out = jtmod_scanner_->out_path();
        append_log(tr("\n.jtmod OK → %1").arg(out));
        QMessageBox box(this);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle(tr("Create .jtmod"));
        box.setText(
            tr("Mod file created.\n\n%1\n\nbins: %2 sub-entry, %3 "
               "whole-bin  ·  minted keys: %4\n\nLoad it in the PoP Mod "
               "Manager to stack it with other mods.")
                .arg(out)
                .arg(result.bins_subentry)
                .arg(result.bins_wholebin)
                .arg(result.minted_keys.size()));
        QPushButton* open_btn =
            box.addButton(tr("Open folder"), QMessageBox::ActionRole);
        box.addButton(QMessageBox::Ok);
        box.exec();
        if (box.clickedButton() == open_btn && !out.isEmpty())
            reveal_in_explorer(out);
    } else {
        QStringList errs;
        for (const auto& i : result.issues)
            if (i.level == "error")
                errs << QStringLiteral("  [%1] %2").arg(qs(i.level),
                                                        qs(i.message));
        QMessageBox::critical(
            this, tr("Create .jtmod failed"),
            tr("The mod file could not be created:\n\n%1")
                .arg(errs.isEmpty() ? tr("(no errors recorded)")
                                    : errs.join(QLatin1Char('\n'))));
    }
}

void ProjectPanel::on_jtmod_thread_done() {
    if (jtmod_scanner_) jtmod_scanner_->deleteLater();
    jtmod_scanner_ = nullptr;
    status_->setText(QString());
    update_enabled();
}

void ProjectPanel::reveal_in_explorer(const QString& path) {
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

// ────────────────────────── deploy ──────────────────────────

void ProjectPanel::on_deploy() {
    ProjectDoc* proj = project_;
    if (!proj || last_build_output_.isEmpty()) return;
    // Suggest the canonical game folder for the project's game.
    const QString game_dir = QFileDialog::getExistingDirectory(
        this,
        tr("Pick the game folder (contains %1)")
            .arg(qs(proj->base.archive_name)),
        guess_game_dir(qs(proj->base.game)));
    if (game_dir.isEmpty()) return;
    const QString target =
        QDir(game_dir).filePath(qs(proj->base.archive_name));
    if (!QFileInfo(target).isFile()) {
        QMessageBox::warning(
            this, tr("Deploy"),
            tr("No %1 in '%2'. Pick the game's install folder.")
                .arg(qs(proj->base.archive_name), game_dir));
        return;
    }
    if (QMessageBox::question(
            this, tr("Deploy"),
            tr("Replace '%1' with the built archive?\n\nA managed stock "
               "backup will be created if none exists yet.")
                .arg(target),
            QMessageBox::Yes | QMessageBox::No)
        != QMessageBox::Yes)
        return;
    jade::deploy::Options options;
    options.backup_dir = fs_path(default_backup_dir());
    const jade::deploy::DeployResult info = jade::deploy::deploy(
        fs_path(last_build_output_), fs_path(game_dir),
        proj->base.archive_name, proj->base.game, options);
    if (!info.ok()) {
        QMessageBox::critical(this, tr("Deploy"),
                              tr("Deploy failed: %1")
                                  .arg(QString::fromStdString(info.message)));
        return;
    }
    const QString deployed_target = q_path(info.target);
    QString msg = tr("Deployed to:\n%1\n\n").arg(deployed_target);
    if (info.backed_up_now)
        msg += tr("Stock backup created:\n%1").arg(q_path(info.backup));
    else
        msg += tr("Stock backup already on file.");
    QMessageBox::information(this, tr("Deploy"), msg);
    append_log(tr("\nDeployed → %1").arg(deployed_target));
}

void ProjectPanel::on_restore_stock() {
    ProjectDoc* proj = project_;
    if (!proj) return;
    const QString game_dir = QFileDialog::getExistingDirectory(
        this, tr("Pick the game folder (contains %1)")
                  .arg(qs(proj->base.archive_name)));
    if (game_dir.isEmpty()) return;
    if (QMessageBox::question(
            this, tr("Restore stock"),
            tr("Overwrite %1 in '%2' with the managed stock backup?")
                .arg(qs(proj->base.archive_name), game_dir),
            QMessageBox::Yes | QMessageBox::No)
        != QMessageBox::Yes)
        return;
    const jade::deploy::RestoreResult result = jade::deploy::restore_stock(
        fs_path(game_dir), proj->base.archive_name, proj->base.game,
        fs_path(default_backup_dir()));
    if (!result.ok()) {
        QMessageBox::warning(
            this, tr("Restore stock"),
            QString::fromStdString(result.message));
        return;
    }
    const QString target = q_path(result.target);
    QMessageBox::information(
        this, tr("Restore stock"),
        tr("Stock archive restored to:\n%1").arg(target));
    append_log(tr("\nRestored stock → %1").arg(target));
}

// ────────────────────────── log ──────────────────────────

void ProjectPanel::append_log(const QString& line) { log_->append(line); }
