// EditZoneDepsTab.hpp — Edit Zone Deps tab (port of
// gui/edit_zone_deps_tab.py): add ``.wow`` deps to an existing zone's
// ``_wol_``.
//
// Each dep is the *internal* resource key of a bin whose objects become
// loadable in the target zone. The op recorded here is
// project.ops_zone.AddZoneDependency ("add_zone_dependency"); the actual
// write happens at build time, like every other operation.
//
// The companion of CreateZone's "extra deps" section — same picker dialog,
// same op semantics, but targets an existing wol BF entry instead of one
// the build is about to create.
#pragma once

#include <QThread>
#include <QWidget>
#include <map>
#include <memory>
#include <vector>

#include "jade/Zone.hpp"

namespace jade { class BigFile; }
class ProjectDoc;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;

// Background scan: discover zones (no analyze; we only need names).
// (Python: module-private _ZoneScanner; renamed to avoid clashing with
// ZonesTab's full-analysis ZoneScanner.)
class DepsZoneScanner : public QThread {
    Q_OBJECT
public:
    explicit DepsZoneScanner(std::shared_ptr<jade::BigFile> bf,
                             QObject* parent = nullptr)
        : QThread(parent), bf_(std::move(bf)) {}

    // Results are pulled by the receiver after `result` fires (the Python
    // signal carried the zone list as a payload object).
    std::vector<jade::ZoneInfo> take_zones() { return std::move(zones_); }

signals:
    void result(const QString& error);  // empty error == success

protected:
    void run() override;

private:
    std::shared_ptr<jade::BigFile> bf_;
    std::vector<jade::ZoneInfo> zones_;
};

// Background build of the internal-key index for nicer dep display.
// (Python: module-private _IndexBuilder.)
class DepsIndexBuilder : public QThread {
    Q_OBJECT
public:
    explicit DepsIndexBuilder(std::shared_ptr<jade::BigFile> bf,
                              QObject* parent = nullptr)
        : QThread(parent), bf_(std::move(bf)) {}

    std::map<quint32, quint32> take_index() { return std::move(index_); }

signals:
    void result(const QString& error);  // empty error == success

protected:
    void run() override;

private:
    std::shared_ptr<jade::BigFile> bf_;
    std::map<quint32, quint32> index_;  // internal_key -> BF file index
};

// Pick an existing zone, view its deps, queue new ones.
class EditZoneDepsTab : public QWidget {
    Q_OBJECT
public:
    explicit EditZoneDepsTab(QWidget* parent = nullptr);
    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& path);
    void set_project(ProjectDoc* proj);

private slots:
    void on_scan();
    void on_scan_done(const QString& err);
    void on_index_done(const QString& err);
    void on_thread_done();
    void on_index_thread_done();
    void on_zone_changed();
    void on_pick();
    void on_remove();
    void on_add();

private:
    std::vector<quint32> staged_keys() const;
    void update_add_enabled();
    // resource_index lookup: fi.name if fi else '<unresolved>'.
    QString resolved_name(quint32 key) const;

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    ProjectDoc* project_ = nullptr;
    std::vector<jade::ZoneInfo> zones_;
    std::map<quint32, quint32> resource_index_;  // internal_key -> BF file idx
    DepsIndexBuilder* index_thread_ = nullptr;
    DepsZoneScanner* scanner_ = nullptr;

    QLabel* project_hint_ = nullptr;
    QComboBox* zone_combo_ = nullptr;
    QPushButton* scan_btn_ = nullptr;
    QLabel* cur_status_ = nullptr;
    QTableWidget* cur_table_ = nullptr;
    QPushButton* pick_btn_ = nullptr;
    QPushButton* remove_btn_ = nullptr;
    QPushButton* clear_btn_ = nullptr;
    QListWidget* new_list_ = nullptr;
    QPushButton* add_btn_ = nullptr;
    QLabel* log_ = nullptr;
};
