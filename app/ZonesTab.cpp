#include "ZonesTab.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include "jade/BigFile.hpp"
#include "jade/Keys.hpp"

#include "GuiUtil.hpp"

namespace {

const char* const COLUMNS[] = {"Zone", "Loadable", "Checkpoint key", "GAOs",
                               "Records", "Deps", "Missing", "Dep health"};
constexpr int N_COLUMNS = int(sizeof(COLUMNS) / sizeof(COLUMNS[0]));

// Table item that sorts by a stored numeric value, not by text (_NumItem).
class NumItem : public QTableWidgetItem {
public:
    NumItem(const QString& text, double value) : QTableWidgetItem(text) {
        setData(Qt::UserRole, value);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }
    bool operator<(const QTableWidgetItem& other) const override {
        if (dynamic_cast<const NumItem*>(&other))
            return data(Qt::UserRole).toDouble()
                   < other.data(Qt::UserRole).toDouble();
        return QTableWidgetItem::operator<(other);
    }
};

// Zone.standalone_loadable / dep_health (properties on the Python Zone).
bool standalone_loadable(const jade::ZoneInfo& z) {
    return z.checkpoint_key >= 0;
}

double dep_health(const jade::ZoneInfo& z) {
    if (z.dep_total == 0) return 1.0;
    return double(z.dep_total - z.dep_missing) / double(z.dep_total);
}

}  // namespace

// ── ZoneScanner ──

void ZoneScanner::run() {
    try {
        zones_ = jade::discover_zones(*bf_);
        const auto index = jade::build_wow_resource_index(
            *bf_, [this](int d, int t) {
                emit progress(d, t, tr("Indexing resources"));
            });
        const int total = int(zones_.size());
        for (int i = 0; i < total; ++i) {
            jade::analyze_zone(*bf_, zones_[size_t(i)], index);
            emit progress(i + 1, total, tr("Analysing zones"));
        }
        emit result(QString());
    } catch (const std::exception& e) {
        zones_.clear();
        emit result(QString::fromUtf8(e.what()));
    }
}

// ── ZonesTab ──

ZonesTab::ZonesTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    auto* top = new QHBoxLayout();
    scan_btn_ = new QPushButton(tr("Scan Zones"));
    connect(scan_btn_, &QPushButton::clicked, this, &ZonesTab::on_scan);
    scan_btn_->setEnabled(false);
    top->addWidget(scan_btn_);

    filter_ = new QLineEdit();
    filter_->setPlaceholderText(tr("Filter by zone name…"));
    connect(filter_, &QLineEdit::textChanged, this, &ZonesTab::apply_filter);
    top->addWidget(filter_, 1);

    status_ = new QLabel(tr("Open a .bf file, then Scan Zones."));
    top->addWidget(status_);
    root->addLayout(top);

    table_ = new QTableWidget(0, N_COLUMNS);
    QStringList headers;
    for (const char* c : COLUMNS) headers << tr(c);
    table_->setHorizontalHeaderLabels(headers);
    table_->setSortingEnabled(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0,
                                                     QHeaderView::Stretch);
    root->addWidget(table_, 1);
}

// -- public --

void ZonesTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                           const QString& path) {
    bf_ = std::move(bf);
    bf_path_ = path;
    zones_.clear();
    table_->setRowCount(0);
    scan_btn_->setEnabled(bf_ != nullptr);
    status_->setText(bf_ ? tr("Ready — click Scan Zones.")
                         : tr("No BigFile loaded."));
}

void ZonesTab::set_project(ProjectDoc* proj) { project_ = proj; }

void ZonesTab::receive_asset(quint32, quint32) {}

// -- scan --

void ZonesTab::on_scan() {
    if (!bf_ || scanner_) return;
    scan_btn_->setEnabled(false);
    status_->setText(tr("Scanning…"));
    scanner_ = new ZoneScanner(bf_);
    connect(scanner_, &ZoneScanner::progress, this, &ZonesTab::on_progress);
    connect(scanner_, &ZoneScanner::result, this, &ZonesTab::on_finished);
    // QThread::finished fires on its own when run() returns — no quit()
    // and no blocking wait() needed; cleanup happens here.
    connect(scanner_, &ZoneScanner::finished, this,
            &ZonesTab::on_thread_done);
    scanner_->start();
}

void ZonesTab::on_progress(int done, int total, const QString& phase) {
    status_->setText(
        QStringLiteral("%1… %2/%3").arg(phase).arg(done).arg(total));
}

// Runs on the main thread when the scan reports results.
void ZonesTab::on_finished(const QString& err) {
    if (!err.isEmpty()) {
        status_->setText(tr("Scan failed: %1").arg(err));
        return;
    }
    zones_ = scanner_->take_zones();
    populate();
}

// Runs once the scan thread has fully stopped.
void ZonesTab::on_thread_done() {
    if (scanner_) scanner_->deleteLater();
    scanner_ = nullptr;
    scan_btn_->setEnabled(true);
}

void ZonesTab::populate() {
    table_->setSortingEnabled(false);
    table_->setRowCount(int(zones_.size()));
    for (int row = 0; row < int(zones_.size()); ++row) {
        const jade::ZoneInfo& z = zones_[size_t(row)];
        const QString ck =
            z.checkpoint_key < 0
                ? QStringLiteral("—")
                : QStringLiteral("0x")
                      + QString::number(z.checkpoint_key, 16)
                            .rightJustified(8, QLatin1Char('0'));
        const int health = int(qRound(dep_health(z) * 100));
        QTableWidgetItem* cells[N_COLUMNS] = {
            new QTableWidgetItem(qs(z.name)),
            new QTableWidgetItem(standalone_loadable(z) ? tr("yes")
                                                        : tr("no")),
            new QTableWidgetItem(ck),
            new NumItem(QString::number(z.gao_count), z.gao_count),
            new NumItem(QString::number(z.record_count), z.record_count),
            new NumItem(QString::number(z.dep_total), z.dep_total),
            new NumItem(QString::number(z.dep_missing), z.dep_missing),
            new NumItem(QStringLiteral("%1%").arg(health), health),
        };
        if (z.error)
            cells[0]->setToolTip(tr("Analysis error: zone failed to parse"));
        for (int col = 0; col < N_COLUMNS; ++col)
            table_->setItem(row, col, cells[col]);
    }
    table_->setSortingEnabled(true);
    int loadable = 0, broken = 0;
    for (const auto& z : zones_) {
        if (standalone_loadable(z)) ++loadable;
        if (z.dep_missing) ++broken;
    }
    status_->setText(tr("%1 zones — %2 loadable, %3 with missing deps.")
                         .arg(zones_.size())
                         .arg(loadable)
                         .arg(broken));
    apply_filter(filter_->text());
}

void ZonesTab::apply_filter(const QString& text) {
    const QString t = text.toLower().trimmed();
    for (int row = 0; row < table_->rowCount(); ++row) {
        QTableWidgetItem* item = table_->item(row, 0);
        const bool hidden =
            !t.isEmpty() && item && !item->text().toLower().contains(t);
        table_->setRowHidden(row, hidden);
    }
}
