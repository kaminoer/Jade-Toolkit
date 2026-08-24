#include "BinPickerDialog.hpp"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "jade/BigFile.hpp"
#include "jade/Keys.hpp"

#include "GuiUtil.hpp"

namespace {
const char* const COLUMNS[] = {"BF name", "Internal key", "BF key"};
constexpr int N_COLUMNS = int(sizeof(COLUMNS) / sizeof(COLUMNS[0]));
}  // namespace

// ── BinIndexBuilder ──

void BinIndexBuilder::run() {
    try {
        index_ = jade::build_wow_resource_index_map(
            *bf_, [this](int d, int t) { emit progress(d, t); });
        emit result(QString());
    } catch (const std::exception& e) {
        index_.clear();
        emit result(QString::fromUtf8(e.what()));
    }
}

// ── BinPickerDialog ──

BinPickerDialog::BinPickerDialog(std::shared_ptr<jade::BigFile> bigfile,
                                 const QString& title,
                                 const std::set<quint32>& already_chosen,
                                 QWidget* parent)
    : QDialog(parent), bf_(std::move(bigfile)), already_(already_chosen) {
    setWindowTitle(title);
    resize(720, 520);

    auto* root = new QVBoxLayout(this);

    auto* top = new QHBoxLayout();
    filter_ = new QLineEdit();
    filter_->setPlaceholderText(tr("Filter by name or hex key…"));
    connect(filter_, &QLineEdit::textChanged, this,
            &BinPickerDialog::apply_filter);
    top->addWidget(new QLabel(tr("Filter:")));
    top->addWidget(filter_, 1);
    status_ = new QLabel(tr("Indexing wow bins…"));
    top->addWidget(status_);
    root->addLayout(top);

    table_ = new QTableWidget(0, N_COLUMNS);
    QStringList headers;
    for (const char* c : COLUMNS) headers << tr(c);
    table_->setHorizontalHeaderLabels(headers);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setSortingEnabled(true);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0,
                                                     QHeaderView::Stretch);
    root->addWidget(table_, 1);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok
                                    | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this,
            &BinPickerDialog::on_accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);

    start_build();
}

void BinPickerDialog::start_build() {
    if (!bf_) {
        status_->setText(tr("No BigFile loaded."));
        return;
    }
    builder_ = new BinIndexBuilder(bf_, this);
    connect(builder_, &BinIndexBuilder::progress, this,
            &BinPickerDialog::on_progress);
    connect(builder_, &BinIndexBuilder::result, this,
            &BinPickerDialog::on_done);
    connect(builder_, &BinIndexBuilder::finished, this,
            &BinPickerDialog::on_thread_done);
    builder_->start();
}

void BinPickerDialog::on_progress(int done, int total) {
    status_->setText(
        tr("Indexing wow bins… %1/%2").arg(done).arg(total));
}

void BinPickerDialog::on_done(const QString& err) {
    if (!err.isEmpty()) {
        status_->setText(tr("Index failed: %1").arg(err));
        return;
    }
    index_ = builder_->take_index();
    have_index_ = true;
    populate();
}

void BinPickerDialog::on_thread_done() {
    if (builder_) {
        builder_->deleteLater();
        builder_ = nullptr;
    }
}

void BinPickerDialog::populate() {
    if (index_.empty()) {
        status_->setText(tr("No wow bins found."));
        return;
    }
    table_->setSortingEnabled(false);
    // Sorted by BF name (lowercased), like the Python.
    struct Row {
        quint32 ikey;
        const jade::BFFile* fi;
    };
    std::vector<Row> rows;
    rows.reserve(index_.size());
    for (const auto& [ikey, fidx] : index_) {
        auto it = bf_->files.find(fidx);
        if (it == bf_->files.end()) continue;
        rows.push_back({ikey, &it->second});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return QString::fromStdString(a.fi->name).toLower()
               < QString::fromStdString(b.fi->name).toLower();
    });
    table_->setRowCount(int(rows.size()));
    for (int r = 0; r < int(rows.size()); ++r) {
        const Row& row = rows[size_t(r)];
        auto* name_item = new QTableWidgetItem(
            row.fi->name.empty() ? tr("<unnamed>") : qs(row.fi->name));
        auto* ikey_item = new QTableWidgetItem(qs(hex_key_lower(row.ikey)));
        auto* bkey_item =
            new QTableWidgetItem(qs(hex_key_lower(row.fi->key)));
        name_item->setData(Qt::UserRole, row.ikey);
        if (already_.count(row.ikey)) {
            name_item->setForeground(Qt::gray);
            name_item->setToolTip(
                tr("Already in the target's dep list — duplicates are "
                   "silently skipped."));
            ikey_item->setForeground(Qt::gray);
            bkey_item->setForeground(Qt::gray);
        }
        table_->setItem(r, 0, name_item);
        table_->setItem(r, 1, ikey_item);
        table_->setItem(r, 2, bkey_item);
    }
    table_->setSortingEnabled(true);
    status_->setText(
        tr("%1 bins  (%2 already in target's deps, greyed)")
            .arg(rows.size())
            .arg(already_.size()));
    apply_filter();
}

void BinPickerDialog::apply_filter() {
    const QString text = filter_->text().trimmed().toLower();
    for (int r = 0; r < table_->rowCount(); ++r) {
        QStringList hay;
        for (int c = 0; c < N_COLUMNS; ++c)
            if (QTableWidgetItem* it = table_->item(r, c))
                hay << it->text().toLower();
        table_->setRowHidden(
            r, !text.isEmpty() && !hay.join(QLatin1Char(' ')).contains(text));
    }
}

void BinPickerDialog::on_accept() {
    std::vector<quint32> out;
    std::set<quint32> seen;
    const auto rows = table_->selectionModel()->selectedRows();
    for (const QModelIndex& idx : rows) {
        QTableWidgetItem* it = table_->item(idx.row(), 0);
        if (!it) continue;
        const quint32 k = it->data(Qt::UserRole).toUInt();
        if (seen.count(k)) continue;
        seen.insert(k);
        out.push_back(k);
    }
    selected_keys_ = out;
    accept();
}
