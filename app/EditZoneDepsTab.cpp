#include "EditZoneDepsTab.hpp"

#include <QAbstractItemView>
#include <QComboBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <stdexcept>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Json.hpp"
#include "jade/Keys.hpp"
#include "jade/Wol.hpp"

#include "BinPickerDialog.hpp"
#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"

namespace {

const char* const DEP_COLUMNS[] = {"Internal key", "Resolved bin name"};
constexpr int N_DEP_COLUMNS =
    int(sizeof(DEP_COLUMNS) / sizeof(DEP_COLUMNS[0]));

// f"0x{key:08x}" as a QString — lowercase-hex key for display (dep table,
// staged list, log line).
QString hex_key_lo(quint32 key) { return qs(hex_key_lower(key)); }

}  // namespace

// ── _ZoneScanner ──

void DepsZoneScanner::run() {
    try {
        zones_ = jade::discover_zones(*bf_);
        emit result(QString());
    } catch (const std::exception& e) {
        zones_.clear();
        emit result(QString::fromUtf8(e.what()));
    }
}

// ── _IndexBuilder ──

void DepsIndexBuilder::run() {
    try {
        index_ = jade::build_wow_resource_index_map(*bf_);
        emit result(QString());
    } catch (const std::exception& e) {
        index_.clear();
        emit result(QString::fromUtf8(e.what()));
    }
}

// ── EditZoneDepsTab ──

EditZoneDepsTab::EditZoneDepsTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    project_hint_ = new QLabel(
        tr("<i>Open or create a Mod Project (File menu) to record edits.</i>"));
    project_hint_->setWordWrap(true);
    root->addWidget(project_hint_);

    // ── Zone picker ──
    auto* z_row = new QHBoxLayout();
    z_row->addWidget(new QLabel(tr("Target zone:")));
    zone_combo_ = new QComboBox();
    zone_combo_->setMinimumWidth(280);
    connect(zone_combo_, &QComboBox::currentIndexChanged, this,
            &EditZoneDepsTab::on_zone_changed);
    z_row->addWidget(zone_combo_, 1);
    scan_btn_ = new QPushButton(tr("Scan Zones"));
    connect(scan_btn_, &QPushButton::clicked, this, &EditZoneDepsTab::on_scan);
    scan_btn_->setEnabled(false);
    z_row->addWidget(scan_btn_);
    root->addLayout(z_row);

    // ── Current deps ──
    auto* cur_box = new QGroupBox(tr("Current deps for this zone"));
    auto* cur_lay = new QVBoxLayout(cur_box);
    cur_status_ = new QLabel(tr("Pick a zone above to inspect its deps."));
    cur_lay->addWidget(cur_status_);
    cur_table_ = new QTableWidget(0, N_DEP_COLUMNS);
    QStringList headers;
    for (const char* c : DEP_COLUMNS) headers << tr(c);
    cur_table_->setHorizontalHeaderLabels(headers);
    cur_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    cur_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    cur_table_->verticalHeader()->setVisible(false);
    cur_table_->setSortingEnabled(true);
    cur_table_->horizontalHeader()->setSectionResizeMode(1,
                                                         QHeaderView::Stretch);
    cur_lay->addWidget(cur_table_, 1);
    root->addWidget(cur_box, 2);

    // ── New deps staging ──
    auto* new_box = new QGroupBox(tr("Deps to add (queued — not yet applied)"));
    auto* new_lay = new QVBoxLayout(new_box);
    auto* n_row = new QHBoxLayout();
    pick_btn_ = new QPushButton(tr("Pick bins…"));
    connect(pick_btn_, &QPushButton::clicked, this, &EditZoneDepsTab::on_pick);
    pick_btn_->setEnabled(false);
    n_row->addWidget(pick_btn_);
    remove_btn_ = new QPushButton(tr("Remove selected"));
    connect(remove_btn_, &QPushButton::clicked, this,
            &EditZoneDepsTab::on_remove);
    n_row->addWidget(remove_btn_);
    clear_btn_ = new QPushButton(tr("Clear"));
    connect(clear_btn_, &QPushButton::clicked, this,
            [this] { new_list_->clear(); });
    n_row->addWidget(clear_btn_);
    n_row->addStretch(1);
    new_lay->addLayout(n_row);

    new_list_ = new QListWidget();
    new_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    new_list_->setMaximumHeight(140);
    new_lay->addWidget(new_list_);
    root->addWidget(new_box);

    // ── Add to project ──
    auto* btn_row = new QHBoxLayout();
    add_btn_ = new QPushButton(tr("Add AddZoneDependency to Project"));
    connect(add_btn_, &QPushButton::clicked, this, &EditZoneDepsTab::on_add);
    add_btn_->setEnabled(false);
    btn_row->addWidget(add_btn_);
    btn_row->addStretch(1);
    log_ = new QLabel(QString());
    log_->setWordWrap(true);
    btn_row->addWidget(log_, 2);
    root->addLayout(btn_row);
}

// ───────────────────────── public ─────────────────────────

void EditZoneDepsTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                                  const QString& path) {
    bf_ = std::move(bf);
    bf_path_ = path;
    zones_.clear();
    zone_combo_->clear();
    cur_table_->setRowCount(0);
    new_list_->clear();
    resource_index_.clear();
    scan_btn_->setEnabled(bf_ != nullptr);
    pick_btn_->setEnabled(bf_ != nullptr);
    cur_status_->setText(bf_ ? tr("Pick a zone above to inspect its deps.")
                             : tr("No BigFile loaded."));
    update_add_enabled();
}

void EditZoneDepsTab::set_project(ProjectDoc* proj) {
    project_ = proj;
    if (project_ == nullptr) {
        project_hint_->setText(
            tr("<i>Open or create a Mod Project (File menu) to record "
               "edits.</i>"));
    } else {
        project_hint_->setText(
            tr("<i>Recording into project: <b>%1</b></i>")
                .arg(project_->name.isEmpty() ? tr("(unnamed)")
                                              : project_->name));
    }
    update_add_enabled();
}

// ───────────────────────── scan ───────────────────────────

void EditZoneDepsTab::on_scan() {
    if (!bf_ || scanner_) return;
    scan_btn_->setEnabled(false);
    cur_status_->setText(tr("Scanning zones…"));
    scanner_ = new DepsZoneScanner(bf_, this);
    connect(scanner_, &DepsZoneScanner::result, this,
            &EditZoneDepsTab::on_scan_done);
    connect(scanner_, &DepsZoneScanner::finished, this,
            &EditZoneDepsTab::on_thread_done);
    scanner_->start();
    // In parallel, build the internal-key index for nicer dep display.
    index_thread_ = new DepsIndexBuilder(bf_, this);
    connect(index_thread_, &DepsIndexBuilder::result, this,
            &EditZoneDepsTab::on_index_done);
    connect(index_thread_, &DepsIndexBuilder::finished, this,
            &EditZoneDepsTab::on_index_thread_done);
    index_thread_->start();
}

void EditZoneDepsTab::on_scan_done(const QString& err) {
    if (!err.isEmpty()) {
        cur_status_->setText(tr("Scan failed: %1").arg(err));
        return;
    }
    zones_ = scanner_->take_zones();
    // sorted(zones, key=lambda z: z.name.lower())
    std::sort(zones_.begin(), zones_.end(),
              [](const jade::ZoneInfo& a, const jade::ZoneInfo& b) {
                  return QString::fromStdString(a.name).toLower()
                         < QString::fromStdString(b.name).toLower();
              });
    zone_combo_->blockSignals(true);
    zone_combo_->clear();
    for (const jade::ZoneInfo& z : zones_)
        zone_combo_->addItem(QStringLiteral("%1  (wol %2)")
                                 .arg(qs(z.name), hex_key_lo(z.wol_key)),
                             QVariant(quint32(z.wol_key)));
    zone_combo_->blockSignals(false);
    cur_status_->setText(
        tr("%1 zones — pick one to view/edit deps.").arg(zones_.size()));
    on_zone_changed();
}

void EditZoneDepsTab::on_index_done(const QString& err) {
    if (err.isEmpty() && index_thread_) {
        std::map<quint32, quint32> idx = index_thread_->take_index();
        if (!idx.empty()) {
            resource_index_ = std::move(idx);
            // If a zone is already selected, refresh the deps table now
            // that we can resolve names.
            on_zone_changed();
        }
    }
}

void EditZoneDepsTab::on_thread_done() {
    if (scanner_ != nullptr) scanner_->deleteLater();
    scanner_ = nullptr;
    scan_btn_->setEnabled(true);
}

void EditZoneDepsTab::on_index_thread_done() {
    if (index_thread_ != nullptr) index_thread_->deleteLater();
    index_thread_ = nullptr;
}

// ───────────────────────── zone selection ─────────────────

void EditZoneDepsTab::on_zone_changed() {
    const QVariant data = zone_combo_->currentData();
    if (!data.isValid()) {
        cur_table_->setRowCount(0);
        update_add_enabled();
        return;
    }
    const quint32 wol_bf_key = data.toUInt();
    const jade::ZoneInfo* z = nullptr;
    for (const jade::ZoneInfo& zi : zones_) {
        if (zi.wol_key == wol_bf_key) { z = &zi; break; }
    }
    if (z == nullptr || !bf_) return;
    std::vector<uint8_t> wol_dec;
    try {
        jade::LzoResult res = jade::decompress_lzo(bf_->read_data(z->wol_index));
        if (!res.ok) throw std::runtime_error("no decompressed data");
        wol_dec = std::move(res.data);
    } catch (const std::exception& e) {
        cur_status_->setText(
            tr("wol decompress failed: %1").arg(QString::fromUtf8(e.what())));
        return;
    }
    const jade::Wol w = jade::parse_wol(wol_dec);
    if (!w.ok) {
        cur_status_->setText(tr("wol parse failed"));
        return;
    }
    cur_table_->setSortingEnabled(false);
    cur_table_->setRowCount(int(w.deps.size()));
    for (int r = 0; r < int(w.deps.size()); ++r) {
        const quint32 k = w.deps[size_t(r)];
        auto* ki = new QTableWidgetItem(hex_key_lo(k));
        ki->setData(Qt::UserRole, QVariant(k));
        auto* ni = new QTableWidgetItem(resolved_name(k));
        cur_table_->setItem(r, 0, ki);
        cur_table_->setItem(r, 1, ni);
    }
    cur_table_->setSortingEnabled(true);
    cur_status_->setText(
        tr("'%1': %2 dep(s). Picker greys out deps already present.")
            .arg(qs(z->name))
            .arg(w.deps.size()));
    update_add_enabled();
}

// ───────────────────────── new deps staging ───────────────

void EditZoneDepsTab::on_pick() {
    if (!bf_) return;
    // Already-present = current zone's deps + already-staged new deps.
    std::set<quint32> already;
    for (int r = 0; r < cur_table_->rowCount(); ++r) {
        QTableWidgetItem* it = cur_table_->item(r, 0);
        if (it != nullptr) already.insert(it->data(Qt::UserRole).toUInt());
    }
    for (int r = 0; r < new_list_->count(); ++r)
        already.insert(new_list_->item(r)->data(Qt::UserRole).toUInt());
    BinPickerDialog dlg(
        bf_,
        tr("Pick bins to add as deps for the selected zone"),
        already,
        this);
    if (dlg.exec() != QDialog::Accepted) return;
    for (quint32 k : dlg.selected_keys()) {
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   %2").arg(hex_key_lo(k), resolved_name(k)));
        item->setData(Qt::UserRole, QVariant(k));
        new_list_->addItem(item);
    }
    update_add_enabled();
}

void EditZoneDepsTab::on_remove() {
    const QList<QListWidgetItem*> items = new_list_->selectedItems();
    for (QListWidgetItem* it : items)
        delete new_list_->takeItem(new_list_->row(it));
    update_add_enabled();
}

std::vector<quint32> EditZoneDepsTab::staged_keys() const {
    std::vector<quint32> out;
    for (int i = 0; i < new_list_->count(); ++i) {
        const QVariant k = new_list_->item(i)->data(Qt::UserRole);
        if (k.isValid()) out.push_back(k.toUInt());
    }
    return out;
}

// ───────────────────────── add op ─────────────────────────

void EditZoneDepsTab::update_add_enabled() {
    const bool ok = (bf_ != nullptr
                     && project_ != nullptr
                     && zone_combo_->currentData().isValid()
                     && new_list_->count() > 0);
    add_btn_->setEnabled(ok);
}

void EditZoneDepsTab::on_add() {
    const QVariant data = zone_combo_->currentData();
    if (!data.isValid() || project_ == nullptr) return;
    const quint32 wol_bf_key = data.toUInt();
    const std::vector<quint32> keys = staged_keys();
    if (keys.empty()) return;
    // AddZoneDependency(wol_entry_key=wol_bf_key, dep_keys=keys) — dict
    // shape from project/ops_zone.py AddZoneDependency._params_to_dict():
    //   {"op": "add_zone_dependency",
    //    "target": {"wol_entry_key": "0x%08x"},
    //    "params": {"dep_keys": ["0x%08x", …]}}
    // (id/enabled/label/created are filled in by ProjectDoc::add_operation.)
    jade::json::Value op = jade::json::make_obj();
    op.obj["op"] = jade::json::make_str("add_zone_dependency");
    jade::json::Value target = jade::json::make_obj();
    target.obj["wol_entry_key"] = jade::json::make_str(hex_key_lower(wol_bf_key));
    op.obj["target"] = std::move(target);
    jade::json::Value params = jade::json::make_obj();
    jade::json::Value dep_keys = jade::json::make_arr();
    for (quint32 k : keys)
        dep_keys.arr.push_back(jade::json::make_str(hex_key_lower(k)));
    params.obj["dep_keys"] = std::move(dep_keys);
    op.obj["params"] = std::move(params);
    const QString op_id = project_->add_operation(std::move(op));
    QString zname = QStringLiteral("?");
    for (const jade::ZoneInfo& zi : zones_) {
        if (zi.wol_key == wol_bf_key) { zname = qs(zi.name); break; }
    }
    log_->setText(
        tr("[+] queued add_zone_dependency %1: zone='%2' wol=%3 deps=+%4.")
            .arg(op_id, zname, hex_key_lo(wol_bf_key))
            .arg(keys.size()));
    new_list_->clear();
    update_add_enabled();
}

// ───────────────────────── helpers ────────────────────────

QString EditZoneDepsTab::resolved_name(quint32 key) const {
    const auto it = resource_index_.find(key);
    if (it != resource_index_.end() && bf_) {
        const auto fit = bf_->files.find(it->second);
        if (fit != bf_->files.end()) return qs(fit->second.name);
    }
    return tr("<unresolved>");
}
