// ProjectPanel.hpp — Project panel (port of gui/project_panel.py): the
// operation list, Build, and Deploy controls.
//
// This is the new keystone tab. Every edit tab (texture-swap, mesh-swap…)
// records its result here as an operation. The user reorders / toggles
// / undoes ops here, then clicks Build to produce a patched archive.
//
// Threading rule (per the lesson from ``zones_tab.py``): the build worker is
// a QThread *subclass* whose ``run()`` does the work and returns. We never
// move a worker QObject into a QThread and rely on its ``exec()`` loop,
// because ``started → run`` is a direct connection — ``run`` finishes before
// ``exec()`` starts, and the subsequent ``quit()`` arrives at a thread with
// no event loop.
#pragma once

#include <QHash>
#include <QThread>
#include <QWidget>
#include <memory>
#include <vector>

#include "jade/ExePatch.hpp"
#include "jade/Project.hpp"

namespace jade { class BigFile; }
class ProjectDoc;
class QCheckBox;
class QLabel;
class QPushButton;
class QTableWidget;
class QTextEdit;
class QPixmap;

// Background thread that runs jade::project::build_project (_BuildScanner).
//
// QThread subclass; the work lives in run(). The `result` signal fires when
// the BuildResult is ready (pulled via take_result — the struct is too rich
// for a queued-signal payload); QThread::finished fires when run() returns
// and the panel uses that for clean-up.
class BuildScanner : public QThread {
    Q_OBJECT
public:
    BuildScanner(jade::project::ModProject project, QString base_archive_path,
                 QString output_path, QObject* parent = nullptr);
    jade::project::BuildResult take_result() { return std::move(result_); }

signals:
    // Python _BuildScanner-compatible per-phase progress.
    void progress(int done, int total, const QString& phase);
    void result();                       // BuildResult ready (take_result)

protected:
    void run() override;

private:
    jade::project::ModProject project_;
    QString base_, out_;
    jade::project::BuildResult result_;
};

class ExePatchScanner : public QThread {
    Q_OBJECT
public:
    ExePatchScanner(jade::project::ModProject project,
                    QString base_archive_path,
                    jade::exepatch::Options options,
                    QObject* parent = nullptr);
    jade::exepatch::Result take_result() { return std::move(result_); }

signals:
    void progress(int done, int total, const QString& phase);
    void result();

protected:
    void run() override;

private:
    jade::project::ModProject project_;
    QString base_;
    jade::exepatch::Options options_;
    jade::exepatch::Result result_;
};

// Background thread that runs jade::project::export_jtmod (_JtmodScanner).
//
// Applies the project's ops against a read-only base view and diffs the
// result into a composable ``.jtmod`` delta for the PoP Mod Manager. No C++
// compile, no full-archive write — just apply + sub-entry diff.
class JtmodScanner : public QThread {
    Q_OBJECT
public:
    JtmodScanner(jade::project::ModProject project, QString base_archive_path,
                 QString out_path, jade::project::JtmodExportOptions options,
                 QObject* parent = nullptr);
    jade::project::JtmodExportResult take_result() {
        return std::move(result_);
    }
    QString out_path() const { return out_; }

signals:
    // Intentionally silent: Python export_jtmod accepts `log` but its current
    // implementation never calls it and exposes no numeric progress callback.
    void progress(int done, int total, const QString& phase);
    void result();                       // JtmodExportResult ready

protected:
    void run() override;

private:
    jade::project::ModProject project_;
    QString base_, out_;
    jade::project::JtmodExportOptions options_;
    jade::project::JtmodExportResult result_;
};

// The Project tab: shows operations, hosts Build and Deploy.
class ProjectPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProjectPanel(QWidget* parent = nullptr);

    // ── public ──

    // Bind the panel to a ProjectDoc and the path of its base archive on
    // this machine (read-only).
    void set_project(ProjectDoc* project,
                     const QString& base_archive_path = QString());
    // Update the base-archive path (read-only) the build will use.
    void set_base_archive_path(const QString& path);
    // Give the panel the open BigFile view of the base archive. Required
    // for live validation — operations check entry presence here.
    void set_bigfile(std::shared_ptr<jade::BigFile> bigfile);

private slots:
    void refresh();
    void update_enabled();
    void on_strict_changed(Qt::CheckState state);
    void on_remove();
    void on_undo();
    void on_redo();
    void on_op_hover(int row, int col);
    void on_op_context_menu(const QPoint& pos);
    void on_build();
    void on_progress(int done, int total, const QString& phase);
    void on_result();
    void on_thread_done();
    void on_make_exe_patch();
    void on_patch_progress(int done, int total, const QString& phase);
    void on_patch_result();
    void on_patch_thread_done();
    void on_make_jtmod();
    void on_jtmod_result();
    void on_jtmod_thread_done();
    void on_deploy();
    void on_restore_stock();

private:
    // ── validation ──
    void run_validation();

    // ── helpers / hover preview ──
    long long entry_index(quint32 entry_key) const;      // -1 == None
    QString asset_path(const QString& source) const;     // empty == None
    static QString img_tag(const QPixmap& pixmap, int maxsz = 148);
    QPixmap texture_pixmap_from_bf(quint32 entry_key, quint32 sub_key) const;
    QString build_op_tooltip(const jade::json::Value& op) const;

    bool base_archive_matches();
    int selected_row() const;
    QString selected_op_id() const;                      // empty == None
    void select_op(const QString& op_id);
    // T1.4 — open the BoneRemapDialog seeded from the op's persisted map.
    void edit_bone_map(jade::json::Value& op);

    void on_toggle(const QString& op_id, bool enabled);
    void move_selected(int delta);

    static void reveal_in_explorer(const QString& path);

    // ── log ──
    void append_log(const QString& line);

    ProjectDoc* project_ = nullptr;
    QString base_archive_path_;              // empty == None
    std::shared_ptr<jade::BigFile> bigfile_; // open BF view for live validation
    BuildScanner* scanner_ = nullptr;
    ExePatchScanner* patcher_scanner_ = nullptr;
    JtmodScanner* jtmod_scanner_ = nullptr;
    QString last_build_output_;              // empty == None
    // Live-validation state (populated by run_validation()).
    QHash<QString, std::vector<jade::project::BuildIssue>> issues_by_op_;
    std::vector<jade::project::BuildIssue> global_issues_;
    // base.matches() SHA-256s the whole archive — cache it so a refresh
    // (every move/toggle) doesn't re-hash a 500 MB file twice.
    bool base_match_cache_valid_ = false;    // (path, bool) tuple, Pythonly
    QString base_match_cache_path_;
    bool base_match_cache_result_ = false;
    QHash<QString, QString> op_tooltip_cache_;   // op_id → html tooltip

    QLabel* header_ = nullptr;
    QCheckBox* strict_inplace_cb_ = nullptr;
    QPushButton* undo_btn_ = nullptr;
    QPushButton* redo_btn_ = nullptr;
    QPushButton* up_btn_ = nullptr;
    QPushButton* down_btn_ = nullptr;
    QPushButton* del_btn_ = nullptr;
    QPushButton* build_btn_ = nullptr;
    QPushButton* patcher_btn_ = nullptr;
    QPushButton* jtmod_btn_ = nullptr;
    QPushButton* deploy_btn_ = nullptr;
    QPushButton* restore_btn_ = nullptr;
    QTableWidget* table_ = nullptr;
    QTextEdit* log_ = nullptr;
    QLabel* status_ = nullptr;
};
