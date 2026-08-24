// ZonesTab.hpp — Zones tab (port of gui/zones_tab.py): lists every
// _wow_/_wol_ zone and its health.
//
// Surfaces which maps are standalone-loadable (have a player CheckPoint
// GAO) and which have missing streamed dependencies (FORMAT_RESEARCH.md
// §3, §7). The scan decompresses every zone, so it runs in a background
// thread.
#pragma once

#include <QThread>
#include <QWidget>
#include <memory>
#include <vector>

#include "jade/Zone.hpp"

namespace jade { class BigFile; }
class ProjectDoc;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

// Background scan: discover zones, build the resource index, analyse.
//
// A QThread subclass (not a moved worker object): the task only emits
// signals and never receives any, so it needs no event loop. The work runs
// in run(); when run() returns, QThread::finished fires on its own — no
// quit() is needed.
class ZoneScanner : public QThread {
    Q_OBJECT
public:
    explicit ZoneScanner(std::shared_ptr<jade::BigFile> bf,
                         QObject* parent = nullptr)
        : QThread(parent), bf_(std::move(bf)) {}

    // Results are pulled by the receiver after `result` fires (the zone
    // list is too rich for a queued-signal payload).
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

// Tab listing all zones with loadability and dependency health.
class ZonesTab : public QWidget {
    Q_OBJECT
public:
    explicit ZonesTab(QWidget* parent = nullptr);
    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& path);
    void set_project(ProjectDoc* proj);
    void receive_asset(quint32 parent_index, quint32 key);

private slots:
    void on_scan();
    void on_progress(int done, int total, const QString& phase);
    void on_finished(const QString& err);
    void on_thread_done();
    void apply_filter(const QString& text);

private:
    void populate();

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    ProjectDoc* project_ = nullptr;
    std::vector<jade::ZoneInfo> zones_;
    ZoneScanner* scanner_ = nullptr;

    QPushButton* scan_btn_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QLabel* status_ = nullptr;
    QTableWidget* table_ = nullptr;
};
