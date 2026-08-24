#include "CreateZoneTab.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>

#include <stdexcept>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Json.hpp"
#include "jade/Keys.hpp"
#include "jade/Wol.hpp"
#include "jade/ZoneCreate.hpp"

#include "BinPickerDialog.hpp"
#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"

namespace {

// Internal-key top byte for a new zone wow. Has to combine a recognised
// class_id (upper nibble: 0x2 or 0x4 for shipped zone wows) with a
// bf_middle byte (lower nibble) that's unused in SoT's 0xff-prefix FAT.
// 0x07 and 0x09 are completely free, so 0x27/0x29/0x47/0x49 are safe.
// 0x7A (the toolkit's KeyAllocator sub-entry prefix) DOES NOT work for
// wow-file top-level keys: class_id 0x7 is unrecognised by the engine
// and loading silently hangs at a black screen.
constexpr quint32 DEFAULT_NEW_PREFIX = 0x27;

const char* const COLUMNS[] = {"Template zone", "GAOs",
                               "Records",       "Wow size",
                               "Deps",          "Loadable"};
constexpr int N_COLUMNS = int(sizeof(COLUMNS) / sizeof(COLUMNS[0]));

// Zone.standalone_loadable (property on the Python Zone).
bool standalone_loadable(const jade::ZoneInfo& z) {
    return z.checkpoint_key >= 0;
}

// f"0x{key:08x}" as a QString — lowercase-hex key for display.
QString hex_key_lo(quint32 key) { return qs(hex_key_lower(key)); }

// f"{key:08x}" — bare lowercase-hex key (filename composition).
QString hex8(quint32 key) {
    return QString::number(key, 16).rightJustified(8, QLatin1Char('0'));
}

// f"0x{p:02X}" — uppercase 2-digit hex byte (prefix display).
QString hex_byte(quint32 p) {
    return QString::number(p, 16).rightJustified(2, QLatin1Char('0'))
        .toUpper();
}

// f"{n:,}" — comma-grouped number (preview byte sizes).
QString commas(quint32 n) {
    return QLocale(QLocale::English).toString(qulonglong(n));
}

// _num_item: right-aligned cell that also carries its numeric value in
// UserRole (a plain QTableWidgetItem in the Python too — sorting on these
// columns is textual, mirrored exactly).
QTableWidgetItem* num_item(quint32 value) {
    auto* item = new QTableWidgetItem(QString::number(value));
    item->setData(Qt::UserRole, QVariant(value));
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

// _mono_font
QFont mono_font() {
    QFont f(QStringLiteral("Consolas"));
    f.setStyleHint(QFont::Monospace);
    f.setFixedPitch(true);
    return f;
}

}  // namespace

// ── _ZoneScanner ──

void CreateZoneScanner::run() {
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

// ── CreateZoneTab ──

CreateZoneTab::CreateZoneTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    // ── Header / project state ──
    project_hint_ = new QLabel(
        tr("<i>Open or create a Mod Project (File menu) to record edits.</i>"));
    project_hint_->setWordWrap(true);
    root->addWidget(project_hint_);

    // ── Template picker ──
    auto* tpl_box = new QGroupBox(tr("1. Pick a template zone (cloned + rekeyed)"));
    auto* tpl_lay = new QVBoxLayout(tpl_box);

    auto* scan_row = new QHBoxLayout();
    scan_btn_ = new QPushButton(tr("Scan Zones"));
    connect(scan_btn_, &QPushButton::clicked, this, &CreateZoneTab::on_scan);
    scan_btn_->setEnabled(false);
    scan_row->addWidget(scan_btn_);

    loadable_only_ = new QCheckBox(
        tr("Only show standalone-loadable zones "
           "(those with a player CheckPoint GAO)"));
    loadable_only_->setChecked(true);
    connect(loadable_only_, &QCheckBox::checkStateChanged, this,
            &CreateZoneTab::apply_filter);
    scan_row->addWidget(loadable_only_);

    scan_row->addStretch(1);
    scan_status_ = new QLabel(tr("Open a .bf, then Scan Zones."));
    scan_row->addWidget(scan_status_);
    tpl_lay->addLayout(scan_row);

    table_ = new QTableWidget(0, N_COLUMNS);
    QStringList headers;
    for (const char* c : COLUMNS) headers << tr(c);
    table_->setHorizontalHeaderLabels(headers);
    table_->setSortingEnabled(true);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->verticalHeader()->setVisible(false);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    connect(table_, &QTableWidget::itemSelectionChanged, this,
            &CreateZoneTab::on_select);
    tpl_lay->addWidget(table_, 1);
    root->addWidget(tpl_box, 2);

    // ── Naming + key allocation ──
    auto* name_box = new QGroupBox(tr("2. Name the new zone and allocate keys"));
    auto* name_lay = new QVBoxLayout(name_box);

    auto* n_row = new QHBoxLayout();
    n_row->addWidget(new QLabel(tr("New zone name:")));
    name_edit_ = new QLineEdit();
    name_edit_->setPlaceholderText(tr("e.g. MyCustomRoom01"));
    name_edit_->setMaxLength(31);  // .wow record's name slot is 32 (incl. NUL)
    connect(name_edit_, &QLineEdit::textChanged, this,
            &CreateZoneTab::update_preview);
    connect(name_edit_, &QLineEdit::textChanged, this,
            &CreateZoneTab::update_add_enabled);
    n_row->addWidget(name_edit_, 1);
    name_lay->addLayout(n_row);

    auto* p_row = new QHBoxLayout();
    p_row->addWidget(
        new QLabel(tr("New map prefix (top byte of internal keys):")));
    prefix_combo_ = new QComboBox();
    // Only validated combinations: class_id is the upper nibble
    // (0x2/0x4 = recognised zone-wow class), lower nibble is the
    // BF FAT middle byte. 0x07 and 0x09 are the only middle-byte
    // values completely free in SoT's 0xff FAT prefix, so the four
    // safe prefixes are 0x27, 0x29, 0x47, 0x49.
    for (quint32 p : {0x27u, 0x29u, 0x47u, 0x49u})
        prefix_combo_->addItem(QStringLiteral("0x%1").arg(hex_byte(p)),
                               QVariant(p));
    connect(prefix_combo_, &QComboBox::currentIndexChanged, this,
            &CreateZoneTab::update_preview);
    p_row->addWidget(prefix_combo_);
    p_row->addStretch(1);
    name_lay->addLayout(p_row);

    preview_ = new QTextEdit();
    preview_->setReadOnly(true);
    preview_->setMaximumHeight(180);
    preview_->setFont(mono_font());
    name_lay->addWidget(preview_);

    root->addWidget(name_box);

    // ── Extra deps (optional) ──
    auto* deps_box = new QGroupBox(
        tr("3. (optional) Make objects from other bins loadable in this zone"));
    auto* deps_lay = new QVBoxLayout(deps_box);
    auto* deps_help = new QLabel(
        tr("These bins are appended to the new wol's dep list. The "
           "engine resolves resource references globally once a bin is "
           "in the dep list, so any GAO can reference resources from "
           "added bins by key (e.g. mesh, material, ColMap)."));
    deps_help->setWordWrap(true);
    deps_lay->addWidget(deps_help);

    auto* dep_row = new QHBoxLayout();
    add_deps_btn_ = new QPushButton(tr("Pick bins to add…"));
    connect(add_deps_btn_, &QPushButton::clicked, this,
            &CreateZoneTab::on_pick_deps);
    add_deps_btn_->setEnabled(false);
    dep_row->addWidget(add_deps_btn_);
    remove_dep_btn_ = new QPushButton(tr("Remove selected"));
    connect(remove_dep_btn_, &QPushButton::clicked, this,
            &CreateZoneTab::on_remove_dep);
    dep_row->addWidget(remove_dep_btn_);
    clear_deps_btn_ = new QPushButton(tr("Clear"));
    connect(clear_deps_btn_, &QPushButton::clicked, this,
            [this] { deps_list_->clear(); });
    dep_row->addWidget(clear_deps_btn_);
    dep_row->addStretch(1);
    deps_lay->addLayout(dep_row);

    deps_list_ = new QListWidget();
    deps_list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    deps_list_->setMaximumHeight(120);
    deps_lay->addWidget(deps_list_);

    root->addWidget(deps_box);

    // ── Add to project ──
    auto* btn_row = new QHBoxLayout();
    add_btn_ = new QPushButton(tr("Add CreateZone to Project"));
    connect(add_btn_, &QPushButton::clicked, this, &CreateZoneTab::on_add);
    add_btn_->setEnabled(false);
    btn_row->addWidget(add_btn_);
    btn_row->addStretch(1);
    log_ = new QLabel(QString());
    log_->setWordWrap(true);
    btn_row->addWidget(log_, 2);
    root->addLayout(btn_row);

    update_preview();
}

// ───────────────────────── public ─────────────────────────

void CreateZoneTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                                const QString& path) {
    bf_ = std::move(bf);
    bf_path_ = path;
    zones_.clear();
    table_->setRowCount(0);
    selected_zone_name_.clear();
    deps_list_->clear();
    scan_btn_->setEnabled(bf_ != nullptr);
    add_deps_btn_->setEnabled(bf_ != nullptr);
    scan_status_->setText(bf_ ? tr("Ready — click Scan Zones.")
                              : tr("No BigFile loaded."));
    update_preview();
    update_add_enabled();
}

void CreateZoneTab::set_project(ProjectDoc* proj) {
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

void CreateZoneTab::on_scan() {
    if (!bf_ || scanner_) return;
    scan_btn_->setEnabled(false);
    scan_status_->setText(tr("Scanning…"));
    scanner_ = new CreateZoneScanner(bf_);
    connect(scanner_, &CreateZoneScanner::progress, this,
            &CreateZoneTab::on_progress);
    connect(scanner_, &CreateZoneScanner::result, this,
            &CreateZoneTab::on_finished);
    connect(scanner_, &CreateZoneScanner::finished, this,
            &CreateZoneTab::on_thread_done);
    scanner_->start();
}

void CreateZoneTab::on_progress(int done, int total, const QString& phase) {
    scan_status_->setText(
        QStringLiteral("%1… %2/%3").arg(phase).arg(done).arg(total));
}

void CreateZoneTab::on_finished(const QString& err) {
    if (!err.isEmpty()) {
        scan_status_->setText(tr("Scan failed: %1").arg(err));
        return;
    }
    zones_ = scanner_->take_zones();
    populate();
}

void CreateZoneTab::on_thread_done() {
    if (scanner_ != nullptr) scanner_->deleteLater();
    scanner_ = nullptr;
    scan_btn_->setEnabled(true);
}

void CreateZoneTab::populate() {
    table_->setSortingEnabled(false);
    table_->setRowCount(int(zones_.size()));
    for (int row = 0; row < int(zones_.size()); ++row) {
        const jade::ZoneInfo& z = zones_[size_t(row)];
        auto* name_item = new QTableWidgetItem(qs(z.name));
        // Pack the standalone_loadable flag into UserRole so the
        // filter can read it without indexing zones_ by view
        // row — that index goes stale the moment sorting kicks in.
        name_item->setData(Qt::UserRole, standalone_loadable(z));
        const jade::BFFile& wf = bf_->files.at(z.wow_index);
        QTableWidgetItem* cells[N_COLUMNS] = {
            name_item,
            num_item(z.gao_count),
            num_item(z.record_count),
            num_item(wf.length),
            num_item(z.dep_total),
            new QTableWidgetItem(standalone_loadable(z) ? tr("yes")
                                                        : tr("no")),
        };
        if (z.error)
            // PORT NOTE: jade::ZoneInfo carries only an error flag, not
            // the Python's message string.
            cells[0]->setToolTip(tr("Analysis error: zone failed to parse"));
        for (int col = 0; col < N_COLUMNS; ++col)
            table_->setItem(row, col, cells[col]);
    }
    table_->setSortingEnabled(true);
    int loadable = 0;
    for (const jade::ZoneInfo& z : zones_)
        if (standalone_loadable(z)) ++loadable;
    scan_status_->setText(
        tr("%1 zones — %2 standalone-loadable. "
           "Pick one to use as the template.")
            .arg(zones_.size())
            .arg(loadable));
    apply_filter();
}

void CreateZoneTab::apply_filter() {
    const bool only_loadable = loadable_only_->isChecked();
    for (int row = 0; row < table_->rowCount(); ++row) {
        QTableWidgetItem* item = table_->item(row, 0);
        const bool is_loadable =
            item != nullptr ? item->data(Qt::UserRole).toBool() : true;
        const bool hide = only_loadable && !is_loadable;
        table_->setRowHidden(row, hide);
    }
}

// ───────────────────────── selection / preview ────────────

void CreateZoneTab::on_select() {
    const QModelIndexList sel = table_->selectionModel()->selectedRows();
    if (sel.isEmpty()) {
        selected_zone_name_.clear();
    } else {
        const int row = sel[0].row();
        QTableWidgetItem* item = table_->item(row, 0);
        selected_zone_name_ = item != nullptr ? item->text() : QString();
    }
    update_preview();
    update_add_enabled();
}

const jade::ZoneInfo* CreateZoneTab::selected_zone() const {
    if (selected_zone_name_.isEmpty()) return nullptr;
    for (const jade::ZoneInfo& z : zones_)
        if (qs(z.name) == selected_zone_name_) return &z;
    return nullptr;
}

void CreateZoneTab::update_preview() {
    const jade::ZoneInfo* z = selected_zone();
    const QString new_name = name_edit_->text().trimmed();
    const QVariant pd = prefix_combo_->currentData();
    const quint32 prefix = pd.isValid() ? pd.toUInt() : DEFAULT_NEW_PREFIX;

    if (!bf_) {
        preview_->setPlainText(
            tr("Load a base archive (File → Open Base Archive) to begin."));
        return;
    }
    if (z == nullptr) {
        preview_->setPlainText(tr("Pick a template zone in the table above."));
        return;
    }

    // Cheap preview: don't decompress, just show what the op WOULD do.
    jade::zonecreate::AllocatedZoneKeys ak;
    try {
        ak = jade::zonecreate::alloc_new_zone_bf_keys(*bf_, prefix);
    } catch (const std::exception& e) {
        preview_->setPlainText(
            tr("Key allocation failed: %1").arg(QString::fromUtf8(e.what())));
        return;
    }

    const quint32 new_internal_wow = (prefix << 24) | (ak.low16 & 0xFFFF);
    const quint32 new_internal_wol = (prefix << 24) | ((ak.low16 + 1) & 0xFFFF);
    const QString new_name_display =
        !new_name.isEmpty() ? new_name : tr("<set a name>");

    const QString wow_filename =
        !new_name.isEmpty()
            ? QStringLiteral("%1_wow_%2.bin").arg(new_name, hex8(ak.wow_key))
            : tr("<name>_wow_<key>.bin");
    const QString wol_filename =
        !new_name.isEmpty()
            ? QStringLiteral("%1_wol_%2.bin").arg(new_name, hex8(ak.wol_key))
            : tr("<name>_wol_<key>.bin");

    const jade::BFFile& wf = bf_->files.at(z->wow_index);
    const jade::BFFile& lf = bf_->files.at(z->wol_index);
    QStringList lines;
    lines << tr("Template       : '%1'").arg(qs(z->name))
          << tr("  wow BF key   : %1  (%2 B)")
                 .arg(hex_key_lo(wf.key), commas(wf.length))
          << tr("  wol BF key   : %1  (%2 B)")
                 .arg(hex_key_lo(lf.key), commas(lf.length))
          << tr("  GAOs         : %1    records: %2")
                 .arg(z->gao_count)
                 .arg(z->record_count)
          << tr("  loadable     : %1")
                 .arg(standalone_loadable(*z)
                          ? tr("yes")
                          : tr("no (sub-world; clone may not load on its own)"))
          << QString()
          << tr("New zone       : '%1'").arg(new_name_display)
          << tr("  map prefix   : 0x%1  (modded namespace)").arg(hex_byte(prefix))
          << tr("  new wow BF   : %1  internal=%2")
                 .arg(hex_key_lo(ak.wow_key), hex_key_lo(new_internal_wow))
          << tr("  new wol BF   : %1  internal=%2")
                 .arg(hex_key_lo(ak.wol_key), hex_key_lo(new_internal_wol))
          << tr("  wow filename : %1").arg(wow_filename)
          << tr("  wol filename : %1").arg(wol_filename)
          << tr("  parent dir   : '%1'").arg(qs(bf_->dir_path(wf.parent)));
    preview_->setPlainText(lines.join(QLatin1Char('\n')));
}

// ───────────────────────── add op ─────────────────────────

void CreateZoneTab::update_add_enabled() {
    const bool ok = (bf_ != nullptr
                     && project_ != nullptr
                     && selected_zone() != nullptr
                     && !name_edit_->text().trimmed().isEmpty());
    add_btn_->setEnabled(ok);
}

void CreateZoneTab::on_add() {
    const jade::ZoneInfo* z = selected_zone();
    if (z == nullptr || project_ == nullptr) return;
    const QString new_name = name_edit_->text().trimmed();
    if (new_name.isEmpty()) return;
    // Reject duplicate names within the same project — would create
    // two CreateZone ops with identical new_name, which both want the
    // same FAT slot at build time.
    const std::string new_name_s = new_name.toStdString();
    for (const jade::json::Value& existing : project_->operations) {
        const jade::json::Value* t = existing.find("op");
        if (t == nullptr || !t->is_str() || t->str != "create_zone") continue;
        const jade::json::Value* prm = existing.find("params");
        const jade::json::Value* nn =
            prm != nullptr ? prm->find("new_name") : nullptr;
        if (nn != nullptr && nn->is_str() && nn->str == new_name_s) {
            QMessageBox::warning(
                this, tr("Create Zone"),
                tr("A CreateZone op for '%1' already exists.").arg(new_name));
            return;
        }
    }
    const QVariant pd = prefix_combo_->currentData();
    const quint32 prefix = pd.isValid() ? pd.toUInt() : DEFAULT_NEW_PREFIX;
    const std::vector<quint32> extra_deps = current_extra_deps();
    // CreateZone(template_zone=…, new_name=…, new_map_prefix=…,
    // extra_deps=…) — dict shape from project/ops_zone.py
    // CreateZone._params_to_dict():
    //   {"op": "create_zone",
    //    "params": {"template_zone": …, "new_name": …,
    //               "new_map_prefix": "0x%08x" (only if != 0x27),
    //               "extra_deps": ["0x%08x", …] (only if non-empty)}}
    // (id/enabled/label/created are filled in by ProjectDoc::add_operation.)
    jade::json::Value op = jade::json::make_obj();
    op.obj["op"] = jade::json::make_str("create_zone");
    jade::json::Value params = jade::json::make_obj();
    params.obj["template_zone"] = jade::json::make_str(z->name);
    params.obj["new_name"] = jade::json::make_str(new_name_s);
    if (prefix != DEFAULT_NEW_PREFIX)
        params.obj["new_map_prefix"] =
            jade::json::make_str(hex_key_lower(prefix));
    if (!extra_deps.empty()) {
        jade::json::Value deps = jade::json::make_arr();
        for (quint32 k : extra_deps)
            deps.arr.push_back(jade::json::make_str(hex_key_lower(k)));
        params.obj["extra_deps"] = std::move(deps);
    }
    op.obj["params"] = std::move(params);
    const QString op_id = project_->add_operation(std::move(op));
    log_->setText(
        tr("[+] queued create_zone %1: template='%2' -> "
           "new='%3' (prefix 0x%4), "
           "extra deps: %5.")
            .arg(op_id, qs(z->name), new_name, hex_byte(prefix))
            .arg(extra_deps.size()));
    // Clear the name field to discourage accidentally adding the same
    // op twice. The selection persists so the user can quickly queue
    // several variants of the same template. Extra deps stay too — a
    // common workflow is to clone the same template several times
    // with the same dep set.
    name_edit_->clear();
    update_add_enabled();
}

// ───────────────────────── extra deps ─────────────────────

std::vector<quint32> CreateZoneTab::current_extra_deps() const {
    std::vector<quint32> out;
    for (int i = 0; i < deps_list_->count(); ++i) {
        const QVariant k = deps_list_->item(i)->data(Qt::UserRole);
        if (k.isValid()) out.push_back(k.toUInt());
    }
    return out;
}

std::set<quint32> CreateZoneTab::template_existing_deps() const {
    std::set<quint32> out;
    const jade::ZoneInfo* z = selected_zone();
    if (z == nullptr || !bf_) return out;
    jade::LzoResult res;
    try {
        res = jade::decompress_lzo(bf_->read_data(z->wol_index));
    } catch (const std::exception&) {
        return out;
    }
    if (!res.ok || res.data.empty()) return out;
    const jade::Wol w = jade::parse_wol(res.data);
    if (!w.ok) return out;
    for (quint32 k : w.deps) out.insert(k);
    return out;
}

void CreateZoneTab::on_pick_deps() {
    if (!bf_) return;
    std::set<quint32> already = template_existing_deps();
    for (quint32 k : current_extra_deps()) already.insert(k);
    BinPickerDialog dlg(
        bf_,
        tr("Pick bins to add as deps for the new zone"),
        already,
        this);
    if (dlg.exec() != QDialog::Accepted) return;
    // Resolve picked keys → display name via the picker's index.
    std::set<quint32> chosen_existing;
    for (int i = 0; i < deps_list_->count(); ++i)
        chosen_existing.insert(
            deps_list_->item(i)->data(Qt::UserRole).toUInt());
    for (quint32 k : dlg.selected_keys()) {
        if (chosen_existing.count(k)) continue;
        QString name = bin_name_for(k);
        if (name.isEmpty()) name = tr("<unnamed>");
        auto* item = new QListWidgetItem(
            QStringLiteral("%1   %2").arg(hex_key_lo(k), name));
        item->setData(Qt::UserRole, QVariant(k));
        deps_list_->addItem(item);
    }
}

void CreateZoneTab::on_remove_dep() {
    const QList<QListWidgetItem*> items = deps_list_->selectedItems();
    for (QListWidgetItem* it : items)
        delete deps_list_->takeItem(deps_list_->row(it));
}

// Best-effort name lookup for a wow bin by internal key.
//
// Uses the BF FAT's low-16 bits to find a candidate name; cheap
// and good enough for a display column.
QString CreateZoneTab::bin_name_for(quint32 internal_key) const {
    const quint32 low = internal_key & 0xFFFF;
    for (const auto& kv : bf_->files) {
        const jade::BFFile& fi = kv.second;
        if (fi.key != jade::INVALID_KEY && (fi.key & 0xFFFF) == low
            && fi.name.find("_wow_") != std::string::npos)
            return qs(fi.name);
    }
    return QString();
}
