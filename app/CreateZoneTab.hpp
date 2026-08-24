// CreateZoneTab.hpp — Create Zone tab (port of gui/create_zone_tab.py):
// author a brand-new playable zone via template clone.
//
// This is the *only* tab that produces an operation which adds **new** BF
// FAT entries (every other tab modifies existing entries in place). It
// clones a shipped, known-working zone (the user picks the template),
// rekeys it into the toolkit's modded namespace (top byte 0x7A), and
// appends both the new ``_wow_`` and ``_wol_`` as new FAT entries.
//
// The tab itself records nothing destructive — clicking *Add to Project*
// appends a project.ops_zone.CreateZone op ("create_zone"). The actual
// write is the Project panel's *Build* button, like every other operation.
//
// A background scan analyses every zone to surface only standalone-loadable
// templates by default (those carrying a player ``CheckPoint`` GAO). The
// "loadable only" filter can be turned off so the user can pick *any*
// zone — but the default protects the modder from cloning a sub-world
// fragment that the engine can't load on its own.
#pragma once

#include <QThread>
#include <QWidget>
#include <memory>
#include <set>
#include <vector>

#include "jade/Zone.hpp"

namespace jade { class BigFile; }
class ProjectDoc;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTextEdit;

// Background scan: discover zones and analyse them.
//
// Runs analyze over every pair so the table can show standalone_loadable
// (CheckPoint-bearing) zones — the only ones safe to use as templates.
// Mirrors gui.zones_tab._ZoneScanner but exposes results in this tab's
// signal shape. (Python: module-private _ZoneScanner; renamed to avoid
// clashing with ZonesTab's ZoneScanner.)
class CreateZoneScanner : public QThread {
    Q_OBJECT
public:
    explicit CreateZoneScanner(std::shared_ptr<jade::BigFile> bf,
                               QObject* parent = nullptr)
        : QThread(parent), bf_(std::move(bf)) {}

    // Results are pulled by the receiver after `result` fires (the Python
    // signal carried the zone list as a payload object).
    std::vector<jade::ZoneInfo> take_zones() { return std::move(zones_); }

signals:
    void progress(int done, int total, const QString& phase);
    void result(const QString& error);  // empty error == success

protected:
    void run() override;

private:
    std::shared_ptr<jade::BigFile> bf_;
    std::vector<jade::ZoneInfo> zones_;
};

// Pick a template, name a new zone, queue a CreateZone op.
class CreateZoneTab : public QWidget {
    Q_OBJECT
public:
    explicit CreateZoneTab(QWidget* parent = nullptr);
    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& path);
    void set_project(ProjectDoc* proj);

private slots:
    void on_scan();
    void on_progress(int done, int total, const QString& phase);
    void on_finished(const QString& err);
    void on_thread_done();
    void apply_filter();
    void on_select();
    void on_pick_deps();
    void on_remove_dep();
    void on_add();

private:
    void populate();
    const jade::ZoneInfo* selected_zone() const;
    void update_preview();
    void update_add_enabled();
    std::vector<quint32> current_extra_deps() const;
    // Resolve the template's *current* deps so the picker can grey them
    // out (they would be deduped anyway, but the user shouldn't waste a
    // click).
    std::set<quint32> template_existing_deps() const;
    // Best-effort name lookup for a wow bin by internal key.
    QString bin_name_for(quint32 internal_key) const;

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    ProjectDoc* project_ = nullptr;
    CreateZoneScanner* scanner_ = nullptr;
    std::vector<jade::ZoneInfo> zones_;
    QString selected_zone_name_;  // empty == None

    QLabel* project_hint_ = nullptr;
    QPushButton* scan_btn_ = nullptr;
    QCheckBox* loadable_only_ = nullptr;
    QLabel* scan_status_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLineEdit* name_edit_ = nullptr;
    QComboBox* prefix_combo_ = nullptr;
    QTextEdit* preview_ = nullptr;
    QPushButton* add_deps_btn_ = nullptr;
    QPushButton* remove_dep_btn_ = nullptr;
    QPushButton* clear_deps_btn_ = nullptr;
    QListWidget* deps_list_ = nullptr;
    QPushButton* add_btn_ = nullptr;
    QLabel* log_ = nullptr;
};
