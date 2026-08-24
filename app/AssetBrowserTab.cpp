#include "AssetBrowserTab.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMutex>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableView>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/GltfBuilder.hpp"
#include "jade/Json.hpp"
#include "jade/MeshSwap.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

#include "GuiUtil.hpp"
#include "PreviewPanel.hpp"
#include "ProjectDoc.hpp"
#include "TextureSwapTab.hpp"
#include "Theme.hpp"

using jade::json::Value;

namespace {

// ── Visual constants ──

QColor category_color(const QString& cat, bool* found = nullptr) {
    static const QMap<QString, QColor> COLORS = {
        {jade::CAT_TEXTURE, QColor(100, 180, 100)},
        {jade::CAT_TEXTURE_DATA, QColor(80, 140, 90)},
        {jade::CAT_GEOMETRY, QColor(100, 150, 220)},
        {jade::CAT_MATERIAL, QColor(200, 160, 80)},
        {jade::CAT_GAO, QColor(180, 130, 200)},
        {jade::CAT_ANIMATION, QColor(200, 100, 100)},
        {jade::CAT_AI, QColor(180, 180, 100)},
        {jade::CAT_PALETTE, QColor(100, 200, 160)},
        {jade::CAT_REFERENCE, QColor(110, 120, 140)},
        {jade::CAT_DATATABLE, QColor(170, 150, 120)},
        {jade::CAT_OTHER, QColor(160, 160, 160)},
        // Marker categories — colours match the level-editor markers.
        {jade::CAT_CAMERA, QColor(76, 140, 255)},
        {jade::CAT_LIGHT, QColor(255, 217, 51)},
        {jade::CAT_SOUND, QColor(51, 230, 230)},
        {jade::CAT_FX, QColor(255, 102, 230)},
        {jade::CAT_TRIGGER, QColor(76, 230, 102)},
        {jade::CAT_TRAP, QColor(255, 64, 51)},
        {jade::CAT_ACTOR, QColor(255, 140, 26)},
        {jade::CAT_SPAWNER, QColor(255, 89, 26)},
        {jade::CAT_WAYPOINT, QColor(140, 255, 115)},
        {jade::CAT_LOGIC, QColor(153, 153, 166)},
    };
    auto it = COLORS.find(cat);
    if (found) *found = it != COLORS.end();
    return it != COLORS.end() ? it.value() : QColor(160, 160, 160);
}

// CATEGORY_ORDER (asset_index.py) — sidebar order.
const char* const CATEGORY_ORDER[] = {
    jade::CAT_TEXTURE, jade::CAT_GEOMETRY, jade::CAT_MATERIAL,
    jade::CAT_GAO, jade::CAT_CAMERA, jade::CAT_LIGHT, jade::CAT_SOUND,
    jade::CAT_FX, jade::CAT_TRIGGER, jade::CAT_TRAP, jade::CAT_ACTOR,
    jade::CAT_SPAWNER, jade::CAT_WAYPOINT, jade::CAT_LOGIC,
    jade::CAT_ANIMATION, jade::CAT_AI, jade::CAT_PALETTE,
    jade::CAT_TEXTURE_DATA, jade::CAT_DATATABLE, jade::CAT_REFERENCE,
    jade::CAT_OTHER,
};

// ORPHAN_CATEGORIES — categories the "Unused" scope applies to.
bool is_orphan_category(const QString& cat) {
    return cat == QLatin1String(jade::CAT_TEXTURE)
           || cat == QLatin1String(jade::CAT_GEOMETRY)
           || cat == QLatin1String(jade::CAT_MATERIAL)
           || cat == QLatin1String(jade::CAT_PALETTE);
}

// Scope filter options.
const char* SCOPE_ALL = "All entries";
const char* SCOPE_MODIFIED = "Modified in project";
const char* SCOPE_UNUSED = "Unused (orphan)";

constexpr int DISPLAY_TABLE = 0;
constexpr int DISPLAY_GRID = 1;

// Filesystem-safe slug for an asset name used in batch export.
QString safe_filename(const QString& name) {
    QString out;
    for (const QChar c : name)
        out += (c.isLetterOrNumber() || c == QLatin1Char('.')
                || c == QLatin1Char('_') || c == QLatin1Char('-'))
                   ? c
                   : QLatin1Char('_');
    while (out.startsWith(QLatin1Char('_'))) out.remove(0, 1);
    while (out.endsWith(QLatin1Char('_'))) out.chop(1);
    return out.left(80);
}

QIcon category_placeholder_icon(const QString& cat, int size = 96) {
    const QColor color = category_color(cat);
    QPixmap pm(size, size);
    pm.fill(QColor(45, 45, 45));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(4, 4, size - 8, size - 8, color);
    p.setPen(QColor(20, 20, 20));
    QFont font = p.font();
    font.setBold(true);
    font.setPointSize(28);
    p.setFont(font);
    p.drawText(pm.rect(), Qt::AlignCenter, cat.left(1));
    p.end();
    return QIcon(pm);
}

QString rec_tooltip(const jade::AssetRecord& rec, const QString& pname) {
    return QStringLiteral("%1\n%2  [%3]\n%4\nfrom %5\nasset_id=(%6, %7)")
        .arg(assetbrowser::record_name(rec), hex_key(rec.key),
             qs(rec.category), qs(rec.detail), pname)
        .arg(rec.parent_index)
        .arg(rec.sub_index);
}

}  // namespace

namespace assetbrowser {

// The core resolves display names like the Python index (GAO names
// propagated down the reference chain to geometry / materials /
// textures — asset_index resolve_referrer_names). The old
// "<ext-or-category> 0xKEY" form remains only as a last-resort fallback
// for a record with no name at all.
QString record_name(const jade::AssetRecord& rec) {
    if (!rec.name.empty()) return qs(rec.name);
    if (!rec.ext.empty())
        return QStringLiteral("%1 %2").arg(qs(rec.ext), hex_key(rec.key));
    return QStringLiteral("%1 %2").arg(qs(rec.category), hex_key(rec.key));
}

QString parent_name_of(const jade::BigFile* bf, quint32 parent_index) {
    if (!bf) return QStringLiteral("file%1").arg(parent_index);
    auto it = bf->files.find(parent_index);
    if (it == bf->files.end() || it->second.name.empty())
        return QStringLiteral("file%1").arg(parent_index);
    return qs(it->second.name);
}

}  // namespace assetbrowser

// ── AssetTableModel ──

AssetTableModel::AssetTableModel(QObject* parent)
    : QAbstractTableModel(parent) {}

void AssetTableModel::set_index(std::shared_ptr<jade::AssetIndex> idx,
                                const jade::BigFile* bf) {
    beginResetModel();
    idx_ = std::move(idx);
    bf_ = bf;
    view_.reset();
    endResetModel();
    if (sort_col_ >= 0) apply_sort_inplace();
}

void AssetTableModel::set_filter(
    std::optional<std::vector<int>> view_indices) {
    beginResetModel();
    view_ = std::move(view_indices);
    endResetModel();
    if (sort_col_ >= 0) apply_sort_inplace();
}

// QTableView calls this when the user clicks a header.
// setSortingEnabled(true) triggers a spurious initial call before any data
// is loaded; ignore it.
void AssetTableModel::sort(int column, Qt::SortOrder order) {
    if (column < 0 || column >= 5) return;
    if (!idx_) return;
    sort_col_ = column;
    sort_order_ = order;
    apply_sort_inplace();
}

// Re-order view_ by the current sort column/order. The no-filter case
// materialises a fresh index list on an explicit header click.
void AssetTableModel::apply_sort_inplace() {
    if (!idx_ || sort_col_ < 0) return;
    const auto& recs = idx_->records;
    std::vector<int> view;
    if (view_) {
        view = *view_;
    } else {
        view.resize(recs.size());
        for (size_t i = 0; i < recs.size(); ++i) view[i] = int(i);
    }
    const int col = sort_col_;
    const jade::BigFile* bf = bf_;
    auto key_of = [col, bf, &recs](int i) -> QString {
        const jade::AssetRecord& r = recs[size_t(i)];
        switch (col) {
            case 0: return assetbrowser::record_name(r).toLower();
            case 1: return QString();  // numeric — handled below
            case 2: return qs(r.category).toLower();
            case 3: return qs(r.detail).toLower();
            case 4:
                return assetbrowser::parent_name_of(bf, r.parent_index)
                    .toLower();
        }
        return QString();
    };
    const bool reverse = sort_order_ == Qt::DescendingOrder;
    if (col == 1) {
        std::stable_sort(view.begin(), view.end(),
                         [&recs](int a, int b) {
                             return recs[size_t(a)].key
                                    < recs[size_t(b)].key;
                         });
    } else {
        std::stable_sort(view.begin(), view.end(),
                         [&key_of](int a, int b) {
                             return key_of(a) < key_of(b);
                         });
    }
    if (reverse) std::reverse(view.begin(), view.end());
    beginResetModel();
    view_ = std::move(view);
    endResetModel();
}

void AssetTableModel::set_modified(const std::set<quint32>& modified_keys) {
    if (modified_keys == modified_) return;
    modified_ = modified_keys;
    if (rowCount())
        emit dataChanged(index(0, 0), index(rowCount() - 1, 4),
                         {Qt::FontRole, Qt::ForegroundRole});
}

const jade::AssetRecord* AssetTableModel::record_at(int row) const {
    if (!idx_) return nullptr;
    if (!view_) {
        if (row >= 0 && size_t(row) < idx_->records.size())
            return &idx_->records[size_t(row)];
        return nullptr;
    }
    if (row >= 0 && size_t(row) < view_->size())
        return &idx_->records[size_t((*view_)[size_t(row)])];
    return nullptr;
}

int AssetTableModel::rowCount(const QModelIndex&) const {
    if (!idx_) return 0;
    return view_ ? int(view_->size()) : int(idx_->records.size());
}

int AssetTableModel::columnCount(const QModelIndex&) const { return 5; }

QVariant AssetTableModel::headerData(int section,
                                     Qt::Orientation orientation,
                                     int role) const {
    static const char* const COLUMNS[] = {"Name", "Key", "Type", "Detail",
                                          "Parent"};
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole
        && section >= 0 && section < 5)
        return QString::fromLatin1(COLUMNS[section]);
    return {};
}

QVariant AssetTableModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const jade::AssetRecord* rec = record_at(index.row());
    if (!rec) return {};
    const int col = index.column();
    if (role == Qt::DisplayRole) {
        switch (col) {
            case 0: return assetbrowser::record_name(*rec);
            case 1: return hex_key(rec->key);
            case 2: return qs(rec->category);
            case 3: return qs(rec->detail);
            case 4:
                return assetbrowser::parent_name_of(bf_,
                                                    rec->parent_index);
        }
        return {};
    }
    if (role == Qt::ForegroundRole) {
        if (modified_.count(rec->key)) return QColor(255, 180, 80);
        if (col == 2) {
            bool found = false;
            const QColor c = category_color(qs(rec->category), &found);
            if (found) return c;
        }
        return {};
    }
    if (role == Qt::FontRole && modified_.count(rec->key)) {
        QFont f;
        f.setBold(true);
        return f;
    }
    if (role == Qt::ToolTipRole)
        return rec_tooltip(
            *rec, assetbrowser::parent_name_of(bf_, rec->parent_index));
    return {};
}

// ── AssetGridModel ──

AssetGridModel::AssetGridModel(QObject* parent)
    : QAbstractListModel(parent) {}

void AssetGridModel::set_index(std::shared_ptr<jade::AssetIndex> idx,
                               const jade::BigFile* bf) {
    beginResetModel();
    idx_ = std::move(idx);
    bf_ = bf;
    view_.reset();
    thumb_cache_.clear();
    endResetModel();
}

void AssetGridModel::set_filter(
    std::optional<std::vector<int>> view_indices) {
    beginResetModel();
    view_ = std::move(view_indices);
    thumb_cache_.clear();
    endResetModel();
}

void AssetGridModel::set_modified(const std::set<quint32>& modified_keys) {
    if (modified_keys == modified_) return;
    modified_ = modified_keys;
    if (rowCount())
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0),
                         {Qt::FontRole, Qt::ForegroundRole});
}

void AssetGridModel::set_thumb(int row, const QIcon& icon, bool failed) {
    thumb_cache_[row] = {icon, failed};
    const QModelIndex mi = index(row, 0);
    emit dataChanged(mi, mi, {Qt::DecorationRole});
}

int AssetGridModel::thumb_state(int row) const {
    auto it = thumb_cache_.find(row);
    if (it == thumb_cache_.end()) return 2;  // absent
    return it->second.second ? 1 : 0;        // failed : real
}

const jade::AssetRecord* AssetGridModel::record_at(int row) const {
    if (!idx_) return nullptr;
    if (!view_) {
        if (row >= 0 && size_t(row) < idx_->records.size())
            return &idx_->records[size_t(row)];
        return nullptr;
    }
    if (row >= 0 && size_t(row) < view_->size())
        return &idx_->records[size_t((*view_)[size_t(row)])];
    return nullptr;
}

int AssetGridModel::rowCount(const QModelIndex&) const {
    if (!idx_) return 0;
    return view_ ? int(view_->size()) : int(idx_->records.size());
}

QVariant AssetGridModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid()) return {};
    const jade::AssetRecord* rec = record_at(index.row());
    if (!rec) return {};
    if (role == Qt::DisplayRole) return assetbrowser::record_name(*rec);
    if (role == Qt::DecorationRole) {
        auto it = thumb_cache_.find(index.row());
        if (it != thumb_cache_.end() && !it->second.second)
            return it->second.first;
        return placeholder_for(qs(rec->category));
    }
    if (role == Qt::ToolTipRole)
        return rec_tooltip(
            *rec, assetbrowser::parent_name_of(bf_, rec->parent_index));
    if (role == Qt::ForegroundRole && modified_.count(rec->key))
        return QColor(255, 180, 80);
    if (role == Qt::FontRole && modified_.count(rec->key)) {
        QFont f;
        f.setBold(true);
        return f;
    }
    return {};
}

QIcon AssetGridModel::placeholder_for(const QString& cat) const {
    auto it = placeholder_.find(cat);
    if (it == placeholder_.end())
        it = placeholder_.emplace(cat, category_placeholder_icon(cat))
                 .first;
    return it->second;
}

// ── Workers ──

void AssetIndexScanner::run() {
    try {
        auto idx = std::make_shared<jade::AssetIndex>(jade::build_asset_index(
            *bf_, 0, [this](int d, int t) { emit progress(d, t); },
            [this] { return bool(cancel_); }));
        if (cancel_) {
            emit done(tr("cancelled"));
            return;
        }
        index_ = std::move(idx);
        emit done(QString());
    } catch (const std::exception& e) {
        emit done(QString::fromUtf8(e.what()));
    }
}

void AssetEntryLoader::run() {
    try {
        const std::vector<uint8_t> raw = bf_->read_data(fi_);
        if (raw.empty()) {
            emit loaded(tr("Could not read entry data"));
            return;
        }
        const jade::LzoResult r = jade::decompress_lzo(raw);
        if (!r.ok) {
            emit loaded(tr("Could not decompress entry"));
            return;
        }
        subs = jade::walk_sub_entries(r.data);
        raw_size = raw.size();
        dec_size = r.data.size();
        result_ok = true;
        emit loaded(QString());
    } catch (const std::exception& e) {
        emit loaded(QString::fromUtf8(e.what()));
    }
}

AssetThumbnailWorker::AssetThumbnailWorker(std::shared_ptr<jade::BigFile> bf,
                                           std::vector<Job> jobs,
                                           QObject* parent)
    : QThread(parent), bf_(std::move(bf)), jobs_(std::move(jobs)),
      jobs_mutex_(new QMutex()) {
    std::sort(jobs_.begin(), jobs_.end(), [](const Job& a, const Job& b) {
        return std::tie(a.parent_index, a.sub_index)
               < std::tie(b.parent_index, b.sub_index);
    });
}

void AssetThumbnailWorker::add_jobs(std::vector<Job> more) {
    std::sort(more.begin(), more.end(), [](const Job& a, const Job& b) {
        return std::tie(a.parent_index, a.sub_index)
               < std::tie(b.parent_index, b.sub_index);
    });
    QMutexLocker lock(jobs_mutex_);
    jobs_.insert(jobs_.end(), more.begin(), more.end());
}

void AssetThumbnailWorker::run() {
    bool have_parent = false;
    quint32 current_parent = 0;
    std::vector<jade::SubEntry> current_subs;
    size_t i = 0;
    // We tolerate the list growing while we run (single producer).
    while (true) {
        Job job{};
        {
            QMutexLocker lock(jobs_mutex_);
            if (i >= jobs_.size()) break;
            job = jobs_[i];
        }
        if (cancel_) return;
        ++i;
        QImage qimg;
        try {
            if (!have_parent || job.parent_index != current_parent) {
                current_subs.clear();
                const std::vector<uint8_t> raw =
                    bf_->read_data(job.parent_index);
                if (!raw.empty()) {
                    const jade::LzoResult r = jade::decompress_lzo(raw);
                    if (r.ok)
                        current_subs = jade::walk_sub_entries(r.data);
                }
                current_parent = job.parent_index;
                have_parent = true;
            }
            if (job.sub_index < current_subs.size())
                qimg = decode_sub(current_subs[job.sub_index],
                                  current_subs);
        } catch (const std::exception&) {
            qimg = QImage();
        }
        if (!cancel_) emit thumb_ready(job.row, qimg);
    }
}

QImage AssetThumbnailWorker::decode_sub(
    const jade::SubEntry& sub,
    const std::vector<jade::SubEntry>& all_subs) const {
    if (sub.data.empty()
        || !jade::is_texture_entry(sub.data.data(), sub.data.size()))
        return QImage();
    const jade::TexInfo ti =
        jade::parse_texture(sub.data.data(), sub.data.size());
    if (!ti.valid) return QImage();
    const std::vector<uint8_t>* pal =
        jade::palette_for_texture(ti, all_subs);
    const std::vector<uint8_t> rgba = jade::decode_texture(
        sub.data.data(), sub.data.size(), ti, pal ? pal->data() : nullptr,
        pal ? pal->size() : 0);
    if (rgba.empty()) return QImage();
    QImage img(rgba.data(), int(ti.width), int(ti.height),
               int(ti.width) * 4, QImage::Format_RGBA8888);
    return img.copy().scaled(SIZE, SIZE, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
}

// ── The tab ──

AssetBrowserTab::AssetBrowserTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QHBoxLayout(this);
    auto* main_split = new QSplitter(Qt::Horizontal);

    // ----- Left column (sidebar + main view + refs) -----
    auto* left = new QWidget();
    auto* left_layout = new QVBoxLayout(left);
    left_layout->setContentsMargins(4, 4, 4, 0);

    // Toolbar
    auto* trow = new QHBoxLayout();
    trow->addWidget(new QLabel(tr("Filter:")));
    search_ = new QLineEdit();
    search_->setPlaceholderText(tr("Search by name, key, or detail…"));
    search_->setClearButtonEnabled(true);
    connect(search_, &QLineEdit::textChanged, this,
            &AssetBrowserTab::on_filter_changed);
    trow->addWidget(search_, 2);

    trow->addWidget(new QLabel(tr("Scope:")));
    scope_combo_ = new QComboBox();
    scope_combo_->addItems({tr(SCOPE_ALL), tr(SCOPE_MODIFIED),
                            tr(SCOPE_UNUSED)});
    connect(scope_combo_, &QComboBox::currentIndexChanged, this,
            &AssetBrowserTab::on_filter_changed);
    trow->addWidget(scope_combo_);

    trow->addWidget(new QLabel(tr("Display:")));
    display_combo_ = new QComboBox();
    display_combo_->addItems({tr("Table"), tr("Grid")});
    connect(display_combo_, &QComboBox::currentIndexChanged, this,
            &AssetBrowserTab::on_display_changed);
    trow->addWidget(display_combo_);
    trow->addStretch();

    // Export the selected rows. Multi-select is enabled on both views.
    export_sel_btn_ = new QPushButton(tr("Export Selected…"));
    export_sel_btn_->setToolTip(
        tr("Export every selected row. Textures export as PNG and "
           "geometry as GLB through the same pipelines as the "
           "Texture/Mesh Swap tabs; select several rows to export them in "
           "one batch."));
    connect(export_sel_btn_, &QPushButton::clicked, this,
            &AssetBrowserTab::on_export_selected);
    trow->addWidget(export_sel_btn_);
    left_layout->addLayout(trow);

    // Sidebar + main view, split horizontally
    auto* inner = new QSplitter(Qt::Horizontal);

    type_tree_ = new QTreeWidget();
    type_tree_->setHeaderLabels({tr("Type"), tr("Count")});
    type_tree_->setColumnWidth(0, 170);
    type_tree_->setColumnWidth(1, 70);
    type_tree_->setAlternatingRowColors(true);
    connect(type_tree_, &QTreeWidget::itemSelectionChanged, this,
            [this] { on_sidebar_changed(type_tree_); });

    source_tree_ = new QTreeWidget();
    source_tree_->setHeaderLabels({tr("Source"), tr("Subs")});
    source_tree_->setColumnWidth(0, 200);
    source_tree_->setColumnWidth(1, 60);
    source_tree_->setAlternatingRowColors(true);
    connect(source_tree_, &QTreeWidget::itemSelectionChanged, this,
            [this] { on_sidebar_changed(source_tree_); });

    sidebar_tabs_ = new QTabWidget();
    sidebar_tabs_->setDocumentMode(true);
    sidebar_tabs_->addTab(type_tree_, tr("Type"));
    sidebar_tabs_->addTab(source_tree_, tr("Source"));

    // Sidebar filter — hides non-matching rows in *both* trees.
    sidebar_filter_ = new QLineEdit();
    sidebar_filter_->setPlaceholderText(tr("Filter category / source…"));
    sidebar_filter_->setClearButtonEnabled(true);
    connect(sidebar_filter_, &QLineEdit::textChanged, this,
            &AssetBrowserTab::apply_sidebar_filter);

    auto* sidebar_wrap = new QWidget();
    auto* sw_lay = new QVBoxLayout(sidebar_wrap);
    sw_lay->setContentsMargins(0, 0, 0, 0);
    sw_lay->setSpacing(2);
    sw_lay->addWidget(sidebar_filter_);
    sw_lay->addWidget(sidebar_tabs_, 1);
    inner->addWidget(sidebar_wrap);

    // Main view = stacked (table | grid)
    auto* center = new QWidget();
    auto* center_lay = new QVBoxLayout(center);
    center_lay->setContentsMargins(0, 0, 0, 0);

    table_model_ = new AssetTableModel(this);
    grid_model_ = new AssetGridModel(this);

    table_ = new QTableView();
    table_->setModel(table_model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    table_->setSortingEnabled(true);
    table_->setAlternatingRowColors(true);
    table_->setShowGrid(false);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(20);
    QHeaderView* hdr = table_->horizontalHeader();
    for (int col = 0; col < table_model_->columnCount(); ++col)
        hdr->setSectionResizeMode(col, QHeaderView::Interactive);
    hdr->setStretchLastSection(true);
    hdr->resizeSection(0, 240);  // Name
    hdr->resizeSection(1, 90);   // Key
    hdr->resizeSection(2, 80);   // Type
    hdr->resizeSection(3, 200);  // Detail
    connect(table_, &QTableView::clicked, this,
            &AssetBrowserTab::on_table_clicked);
    table_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table_, &QTableView::customContextMenuRequested, this,
            [this](const QPoint& pos) { on_context_menu(table_, pos); });

    grid_ = new QListView();
    grid_->setModel(grid_model_);
    grid_->setViewMode(QListView::IconMode);
    grid_->setIconSize(QSize(96, 96));
    grid_->setGridSize(QSize(112, 128));
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setUniformItemSizes(true);
    grid_->setWordWrap(true);
    grid_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    connect(grid_, &QListView::clicked, this,
            &AssetBrowserTab::on_grid_clicked);
    grid_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grid_, &QListView::customContextMenuRequested, this,
            [this](const QPoint& pos) { on_context_menu(grid_, pos); });
    connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &AssetBrowserTab::defer_viewport_thumbs);

    stack_ = new QStackedWidget();
    stack_->addWidget(table_);
    stack_->addWidget(grid_);
    center_lay->addWidget(stack_, 1);

    status_ = new QLabel(QString());
    status_->setStyleSheet(
        QStringLiteral("color: %1; font-size: 9pt;").arg(theme::DIM_TEXT));
    status_->setMaximumHeight(16);
    center_lay->addWidget(status_);

    inner->addWidget(center);
    inner->setStretchFactor(0, 1);
    inner->setStretchFactor(1, 3);

    // References below
    refs_tree_ = new QTreeWidget();
    refs_tree_->setHeaderLabels({QString(), tr("Key"), tr("Type")});
    refs_tree_->setColumnWidth(0, 220);
    refs_tree_->setColumnWidth(1, 100);
    refs_tree_->setRootIsDecorated(true);
    refs_tree_->setMaximumHeight(220);
    connect(refs_tree_, &QTreeWidget::itemDoubleClicked, this,
            &AssetBrowserTab::on_ref_clicked);
    auto* refs_box = new QGroupBox(tr("References"));
    auto* refs_lay = new QVBoxLayout(refs_box);
    refs_lay->setContentsMargins(4, 4, 4, 4);
    refs_lay->addWidget(refs_tree_);

    auto* outer = new QSplitter(Qt::Vertical);
    outer->addWidget(inner);
    outer->addWidget(refs_box);
    outer->setStretchFactor(0, 4);
    outer->setStretchFactor(1, 1);
    left_layout->addWidget(outer, 1);

    // Right preview
    preview_ = new PreviewPanel();
    main_split->addWidget(left);
    main_split->addWidget(preview_);
    main_split->setStretchFactor(0, 3);
    main_split->setStretchFactor(1, 2);
    root->addWidget(main_split);

    // Debounce timers
    filter_timer_ = new QTimer(this);
    filter_timer_->setSingleShot(true);
    filter_timer_->setInterval(200);
    connect(filter_timer_, &QTimer::timeout, this,
            &AssetBrowserTab::apply_filter);

    thumb_timer_ = new QTimer(this);
    thumb_timer_->setSingleShot(true);
    thumb_timer_->setInterval(140);
    connect(thumb_timer_, &QTimer::timeout, this,
            &AssetBrowserTab::enqueue_visible_thumbs);
}

// ── Public API ──

void AssetBrowserTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                                  const QString& bf_path) {
    cancel_scan();
    cancel_thumbs();
    bf_ = std::move(bf);
    bf_path_ = bf_path;
    index_.reset();
    modified_keys_.clear();
    refs_tree_->clear();
    type_tree_->clear();
    source_tree_->clear();
    table_model_->set_index(nullptr, nullptr);
    grid_model_->set_index(nullptr, nullptr);
    preview_->set_asset_index(nullptr);
    status_->setText(QString());
    if (!bf_) return;
    start_scan();
}

void AssetBrowserTab::set_project(ProjectDoc* proj) {
    if (project_)
        disconnect(project_, &ProjectDoc::changed, this,
                   &AssetBrowserTab::refresh_modified);
    project_ = proj;
    if (proj)
        connect(proj, &ProjectDoc::changed, this,
                &AssetBrowserTab::refresh_modified);
    refresh_modified();
}

// ── Scan ──

void AssetBrowserTab::start_scan() {
    // Progress and completion are surfaced on scan_progress /
    // scan_finished so the main window's modal can drive one bar.
    emit scan_progress(0, 0);
    scan_thread_ = new AssetIndexScanner(bf_, this);
    connect(scan_thread_, &AssetIndexScanner::progress, this,
            &AssetBrowserTab::on_scan_progress);
    connect(scan_thread_, &AssetIndexScanner::done, this,
            &AssetBrowserTab::on_scan_done);
    scan_thread_->start();
}

void AssetBrowserTab::cancel_scan() {
    if (scan_thread_) {
        scan_thread_->cancel();
        scan_thread_->wait(2000);
        scan_thread_->deleteLater();
        scan_thread_ = nullptr;
    }
}

void AssetBrowserTab::on_scan_progress(int current, int total) {
    emit scan_progress(current, total);
}

void AssetBrowserTab::on_scan_done(const QString& err) {
    std::shared_ptr<jade::AssetIndex> idx;
    if (scan_thread_) {
        idx = scan_thread_->take_index();
        scan_thread_->deleteLater();
        scan_thread_ = nullptr;
    }
    if (err.isEmpty() && idx) {
        index_ = idx;
        table_model_->set_index(idx, bf_.get());
        grid_model_->set_index(idx, bf_.get());
        // The preview uses the index for cross-bin material resolution.
        preview_->set_asset_index(idx);
        build_sidebar();
        refresh_modified();
        apply_filter();
    } else {
        status_->setText(tr("Scan failed: %1").arg(err));
    }
    // Always emit so the modal can close, even when the scan errored.
    emit scan_finished();
}

// ── Sidebar ──

void AssetBrowserTab::build_sidebar() {
    type_tree_->clear();
    source_tree_->clear();
    if (!index_) return;

    const auto counts = index_->count_by_category();

    // ── Type tab ──
    auto* all_item = new QTreeWidgetItem(type_tree_);
    all_item->setText(0, tr("All assets"));
    all_item->setText(1, QStringLiteral("%L1").arg(index_->records.size()));
    all_item->setData(0, Qt::UserRole,
                      QVariant::fromValue(QStringLiteral("all")));
    QFont f = all_item->font(0);
    f.setBold(true);
    all_item->setFont(0, f);

    for (const char* cat : CATEGORY_ORDER) {
        auto cit = counts.find(cat);
        const size_t n = cit != counts.end() ? cit->second : 0;
        if (!n) continue;
        auto* ci = new QTreeWidgetItem(type_tree_);
        ci->setText(0, qs(cat));
        ci->setText(1, QStringLiteral("%L1").arg(n));
        ci->setData(0, Qt::UserRole,
                    QStringLiteral("cat:%1").arg(qs(cat)));
        bool found = false;
        const QColor color = category_color(qs(cat), &found);
        if (found) ci->setForeground(0, color);
        // Sub-bucket Other by gro_type — this *is* the RE backlog.
        if (QLatin1String(cat) == QLatin1String(jade::CAT_OTHER)) {
            std::map<quint32, int> cnt;
            for (const jade::AssetRecord& r : index_->records)
                if (r.category == jade::CAT_OTHER) ++cnt[r.gro_type];
            std::vector<std::pair<int, quint32>> ordered;
            for (const auto& [gt, gn] : cnt) ordered.push_back({gn, gt});
            std::sort(ordered.rbegin(), ordered.rend());
            int shown = 0;
            for (const auto& [gn, gt] : ordered) {
                if (shown++ >= 40) break;
                auto* gi = new QTreeWidgetItem(ci);
                gi->setText(0, gt ? hex_key(gt) : tr("(no type)"));
                gi->setText(1, QStringLiteral("%L1").arg(gn));
                gi->setData(0, Qt::UserRole,
                            QStringLiteral("gro:%1").arg(gt));
            }
        }
    }

    // ── Source tab ── — parents by sub-entry count, descending, top 500.
    std::vector<std::pair<quint32, size_t>> parents;
    for (const auto& [pidx, recs] : index_->by_parent)
        parents.push_back({pidx, recs.size()});
    std::stable_sort(parents.begin(), parents.end(),
                     [this](const auto& a, const auto& b) {
                         if (a.second != b.second)
                             return a.second > b.second;
                         return assetbrowser::parent_name_of(bf_.get(),
                                                             a.first)
                                < assetbrowser::parent_name_of(bf_.get(),
                                                               b.first);
                     });
    size_t shown = 0;
    for (const auto& [parent_idx, nrecs] : parents) {
        if (shown++ >= 500) break;
        const QString pname =
            assetbrowser::parent_name_of(bf_.get(), parent_idx);
        auto* it = new QTreeWidgetItem(source_tree_);
        it->setText(0, pname);
        it->setText(1, QStringLiteral("%L1").arg(nrecs));
        it->setData(0, Qt::UserRole,
                    QStringLiteral("parent:%1").arg(parent_idx));
        // Full name on hover — sidebar columns truncate aggressively.
        it->setToolTip(0, pname);
    }
    if (parents.size() > 500) {
        auto* more = new QTreeWidgetItem(source_tree_);
        more->setText(
            0, tr("… +%L1 more sources").arg(parents.size() - 500));
        more->setForeground(0, QColor(120, 120, 120));
        more->setFlags(more->flags() & ~Qt::ItemIsSelectable);
    }

    // Default: "All assets" in the Type tab.
    type_tree_->setCurrentItem(all_item);
    sidebar_tabs_->setCurrentIndex(0);
    apply_sidebar_filter();
}

// ── Sidebar filter ──

void AssetBrowserTab::apply_sidebar_filter() {
    const QString text = sidebar_filter_->text().trimmed().toLower();
    for (int i = 0; i < type_tree_->topLevelItemCount(); ++i)
        filter_sidebar_item(type_tree_->topLevelItem(i), text);
    for (int i = 0; i < source_tree_->topLevelItemCount(); ++i)
        filter_sidebar_item(source_tree_->topLevelItem(i), text);
}

bool AssetBrowserTab::filter_sidebar_item(QTreeWidgetItem* item,
                                          const QString& text) {
    bool any_child_match = false;
    for (int j = 0; j < item->childCount(); ++j)
        if (filter_sidebar_item(item->child(j), text))
            any_child_match = true;
    const bool self_match =
        text.isEmpty() || item->text(0).toLower().contains(text)
        || item->text(1).toLower().contains(text);
    const bool visible = self_match || any_child_match;
    item->setHidden(!visible);
    if (!text.isEmpty() && any_child_match) item->setExpanded(true);
    return visible;
}

void AssetBrowserTab::on_sidebar_changed(QTreeWidget* tree) {
    const auto items = tree->selectedItems();
    if (items.isEmpty()) return;
    const QString data = items.first()->data(0, Qt::UserRole).toString();
    if (data.isEmpty()) return;
    // Clear the other tree's selection so the active filter source is
    // unambiguous.
    QTreeWidget* other =
        tree == type_tree_ ? source_tree_ : type_tree_;
    other->blockSignals(true);
    other->clearSelection();
    other->blockSignals(false);
    assetbrowser::Filter flt;
    if (data == QLatin1String("all")) {
        flt.kind = assetbrowser::Filter::All;
    } else if (data.startsWith(QLatin1String("cat:"))) {
        flt.kind = assetbrowser::Filter::Cat;
        flt.cat = data.mid(4);
    } else if (data.startsWith(QLatin1String("gro:"))) {
        flt.kind = assetbrowser::Filter::Gro;
        flt.gro = data.mid(4).toUInt();
    } else if (data.startsWith(QLatin1String("parent:"))) {
        flt.kind = assetbrowser::Filter::Parent;
        flt.parent = data.mid(7).toUInt();
    }
    filter_ = flt;
    apply_filter();
}

// ── Filter pipeline ──

void AssetBrowserTab::on_filter_changed() { filter_timer_->start(); }

// Compute the view subset and push to both models.
void AssetBrowserTab::apply_filter() {
    if (!index_) return;
    // Step 1: category restriction from sidebar.
    std::vector<int> base;
    bool base_is_all = false;
    switch (filter_.kind) {
        case assetbrowser::Filter::All:
            base_is_all = true;
            break;
        case assetbrowser::Filter::Cat:
            for (int i = 0; i < int(index_->records.size()); ++i)
                if (qs(index_->records[size_t(i)].category) == filter_.cat)
                    base.push_back(i);
            break;
        case assetbrowser::Filter::Gro:
            for (int i = 0; i < int(index_->records.size()); ++i) {
                const jade::AssetRecord& r = index_->records[size_t(i)];
                if (r.category == jade::CAT_OTHER
                    && r.gro_type == filter_.gro)
                    base.push_back(i);
            }
            break;
        case assetbrowser::Filter::Parent:
            for (int i = 0; i < int(index_->records.size()); ++i)
                if (index_->records[size_t(i)].parent_index
                    == filter_.parent)
                    base.push_back(i);
            break;
    }

    // Step 2: scope filter + search.
    const QString scope = scope_combo_->currentText();
    const QString text = search_->text().trimmed().toLower();

    std::optional<std::vector<int>> view;
    if (scope == tr(SCOPE_ALL) && text.isEmpty()) {
        // Fast path: no scope filter, no search.
        if (!base_is_all) view = std::move(base);
    } else {
        auto keep = [&](int i) {
            const jade::AssetRecord& r = index_->records[size_t(i)];
            if (scope == tr(SCOPE_MODIFIED)
                && !modified_keys_.count(r.key))
                return false;
            if (scope == tr(SCOPE_UNUSED)) {
                if (!is_orphan_category(qs(r.category))) return false;
                if (index_->reverse_refs.count(r.key)) return false;
            }
            if (!text.isEmpty()) {
                const QString hay =
                    (assetbrowser::record_name(r) + QLatin1Char(' ')
                     + qs(r.detail) + QLatin1Char(' ')
                     + qs(hex_key_lower(r.key)) + QLatin1Char(' ')
                     + assetbrowser::parent_name_of(bf_.get(),
                                                    r.parent_index))
                        .toLower();
                if (!hay.contains(text)) return false;
            }
            return true;
        };
        std::vector<int> out;
        if (base_is_all) {
            for (int i = 0; i < int(index_->records.size()); ++i)
                if (keep(i)) out.push_back(i);
        } else {
            for (int i : base)
                if (keep(i)) out.push_back(i);
        }
        view = std::move(out);
    }

    // Push to both models (the inactive one is cheap to reset).
    table_model_->set_filter(view);
    grid_model_->set_filter(view);

    // Status
    const size_t shown = view ? view->size() : index_->records.size();
    status_->setText(tr("%L1 of %L2 sub-entries  —  filter: %3")
                         .arg(shown)
                         .arg(index_->records.size())
                         .arg(filter_label()));

    if (stack_->currentIndex() == DISPLAY_GRID) defer_viewport_thumbs();
}

QString AssetBrowserTab::filter_label() const {
    QString label;
    switch (filter_.kind) {
        case assetbrowser::Filter::All:
            label = tr("all");
            break;
        case assetbrowser::Filter::Cat:
            label = filter_.cat;
            break;
        case assetbrowser::Filter::Gro:
            label = tr("type %1").arg(hex_key(filter_.gro));
            break;
        case assetbrowser::Filter::Parent:
            label = tr("source: %1")
                        .arg(assetbrowser::parent_name_of(bf_.get(),
                                                          filter_.parent));
            break;
    }
    const QString search = search_->text().trimmed();
    if (!search.isEmpty())
        label += tr("  ·  search: '%1'").arg(search);
    return label;
}

// ── Display / view-mode toggles ──

void AssetBrowserTab::on_display_changed(int idx) {
    stack_->setCurrentIndex(idx);
    if (idx == DISPLAY_GRID) defer_viewport_thumbs();
}

// ── Selection / preview / refs ──

void AssetBrowserTab::on_table_clicked(const QModelIndex& index) {
    const jade::AssetRecord* rec = table_model_->record_at(index.row());
    if (rec) select_record(*rec);
}

void AssetBrowserTab::on_grid_clicked(const QModelIndex& index) {
    const jade::AssetRecord* rec = grid_model_->record_at(index.row());
    if (rec) select_record(*rec);
}

// ── Context menu ──

// Right-click: send-to-tab actions + Export… + Copy key.
void AssetBrowserTab::on_context_menu(QWidget* view, const QPoint& pos) {
    const jade::AssetRecord* rec = nullptr;
    if (view == table_) {
        const QModelIndex idx = table_->indexAt(pos);
        if (!idx.isValid()) return;
        rec = table_model_->record_at(idx.row());
    } else {
        const QModelIndex idx = grid_->indexAt(pos);
        if (!idx.isValid()) return;
        rec = grid_model_->record_at(idx.row());
    }
    if (!rec) return;
    const jade::AssetRecord r = *rec;  // copy — menus may refilter models

    QMenu menu(view);

    // Category-specific "send to" actions.
    if (r.category == jade::CAT_TEXTURE) {
        QAction* a = menu.addAction(tr("Send to Texture Swap"));
        connect(a, &QAction::triggered, this, [this, r] {
            emit send_asset_to_tab(QStringLiteral("texture_swap"),
                                   r.parent_index, r.key, qs(r.category));
        });
        // Fast-path: file dialog → ReplaceTexture op, no manual entry
        // combo dance.
        a = menu.addAction(tr("Replace with PNG / DDS…"));
        connect(a, &QAction::triggered, this,
                [this, r] { replace_texture_inline(r); });
    } else if (r.category == jade::CAT_GEOMETRY) {
        QAction* a = menu.addAction(tr("Send to Mesh Swap"));
        connect(a, &QAction::triggered, this, [this, r] {
            emit send_asset_to_tab(QStringLiteral("mesh_swap"),
                                   r.parent_index, r.key, qs(r.category));
        });
    } else if (r.ext == ".gao") {
        // Every GAO — generic game object or a marker — is placeable /
        // movable in the editor.
        QAction* a = menu.addAction(tr("Send to Place Objects"));
        connect(a, &QAction::triggered, this, [this, r] {
            emit send_asset_to_tab(QStringLiteral("place_objects"),
                                   r.parent_index, r.key, qs(r.category));
        });
    }

    if (!menu.actions().isEmpty()) menu.addSeparator();

    // Export. When the right-clicked row is part of a multi-row
    // selection, export the whole selection at once.
    const std::vector<const jade::AssetRecord*> sel = selected_records();
    bool in_sel = false;
    for (const jade::AssetRecord* s : sel)
        if (s->parent_index == r.parent_index
            && s->sub_index == r.sub_index)
            in_sel = true;
    if (sel.size() > 1 && in_sel) {
        connect(menu.addAction(tr("Export selected (%1)…").arg(sel.size())),
                &QAction::triggered, this,
                [this, sel] { export_records(sel); });
    } else if (r.category == jade::CAT_TEXTURE) {
        connect(menu.addAction(tr("Export as PNG…")), &QAction::triggered,
                this, [this, r] {
                    export_records({&r});
                });
    } else if (r.category == jade::CAT_GEOMETRY) {
        connect(menu.addAction(tr("Export as GLB…")), &QAction::triggered,
                this, [this, r] {
                    export_records({&r});
                });
    } else if (r.category == jade::CAT_MATERIAL
               || r.category == jade::CAT_ANIMATION || r.ext == ".gao") {
        connect(menu.addAction(tr("Export…")), &QAction::triggered, this,
                [this, r] { export_asset(r); });
    }

    // Batch export/import via manifest.json.
    connect(menu.addAction(tr("Export all textures from parent entry…")),
            &QAction::triggered, this,
            [this, r] { export_all_textures(r.parent_index); });
    connect(
        menu.addAction(tr("Import textures from folder… (uses manifest)")),
        &QAction::triggered, this,
        [this, r] { import_textures_from_folder(r.parent_index); });

    menu.addSeparator();
    connect(menu.addAction(tr("Copy key")), &QAction::triggered, this,
            [r] { QApplication::clipboard()->setText(hex_key(r.key)); });
    connect(menu.addAction(tr("Show parent entry (Source filter)")),
            &QAction::triggered, this,
            [this, r] { filter_to_source(r.parent_index); });

    QWidget* vp = view == table_
                      ? static_cast<QWidget*>(table_->viewport())
                      : static_cast<QWidget*>(grid_->viewport());
    menu.exec(vp->mapToGlobal(pos));
}

// Activate the Source tab and select this entry's row.
void AssetBrowserTab::filter_to_source(quint32 parent_index) {
    sidebar_tabs_->setCurrentIndex(1);
    const QString want = QStringLiteral("parent:%1").arg(parent_index);
    for (int i = 0; i < source_tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* it = source_tree_->topLevelItem(i);
        if (it->data(0, Qt::UserRole).toString() == want) {
            source_tree_->setCurrentItem(it);
            source_tree_->scrollToItem(it);
            return;
        }
    }
}

// ── Multi-select export ──

// Records selected in whichever view (table / grid) is active.
std::vector<const jade::AssetRecord*> AssetBrowserTab::selected_records()
    const {
    std::set<int> rows;
    if (stack_->currentIndex() == DISPLAY_TABLE) {
        if (table_->selectionModel())
            for (const QModelIndex& i :
                 table_->selectionModel()->selectedIndexes())
                rows.insert(i.row());
        std::vector<const jade::AssetRecord*> out;
        for (int r : rows)
            if (const jade::AssetRecord* rec = table_model_->record_at(r))
                out.push_back(rec);
        return out;
    }
    if (grid_->selectionModel())
        for (const QModelIndex& i :
             grid_->selectionModel()->selectedIndexes())
            rows.insert(i.row());
    std::vector<const jade::AssetRecord*> out;
    for (int r : rows)
        if (const jade::AssetRecord* rec = grid_model_->record_at(r))
            out.push_back(rec);
    return out;
}

// Preview-panel export key for a record. Any GAO exports through the
// 'gao' path; otherwise map by category.
QString AssetBrowserTab::export_cat_key(
    const jade::AssetRecord& rec) const {
    if (rec.ext == ".gao") return QStringLiteral("gao");
    return EXPORT_CATEGORY_MAP.value(qs(rec.category),
                                     QStringLiteral("unknown"));
}

// Output extension for a record: PNG/GLB for texture/geometry, else the
// category's default-format extension.
QString AssetBrowserTab::ext_for(const jade::AssetRecord& rec) const {
    if (rec.category == jade::CAT_TEXTURE) return QStringLiteral(".png");
    if (rec.category == jade::CAT_GEOMETRY) return QStringLiteral(".glb");
    const auto formats = PreviewPanel::formats_for(export_cat_key(rec));
    return formats.isEmpty() ? QStringLiteral(".bin")
                             : formats.first().second;
}

void AssetBrowserTab::on_export_selected() {
    if (!bf_) return;
    const auto recs = selected_records();
    if (recs.empty()) {
        QMessageBox::information(
            this, tr("Export Selected"),
            tr("Select one or more rows in the list first."));
        return;
    }
    export_records(recs);
}

// Export one or many records. Texture → PNG via the Texture Swap extract
// pipeline, geometry → GLB via the Mesh Swap export pipeline; any other
// type uses its existing default-format exporter.
void AssetBrowserTab::export_records(
    std::vector<const jade::AssetRecord*> recs) {
    recs.erase(std::remove(recs.begin(), recs.end(), nullptr), recs.end());
    if (recs.empty() || !bf_) return;

    std::vector<std::pair<jade::AssetRecord, QString>> jobs;
    if (recs.size() == 1) {
        const jade::AssetRecord& rec = *recs.front();
        const QString ext = ext_for(rec);
        QString flt;
        if (ext == QLatin1String(".png"))
            flt = tr("PNG image (*.png)");
        else if (ext == QLatin1String(".glb"))
            flt = tr("glTF Binary (*.glb)");
        else
            flt = QStringLiteral("(*%1);;All Files (*)").arg(ext);
        const QString def = hex_key(rec.key) + ext;
        const QString path = QFileDialog::getSaveFileName(
            this, tr("Export asset"), def, flt);
        if (path.isEmpty()) return;
        jobs.push_back({rec, path});
    } else {
        const QString out_dir = QFileDialog::getExistingDirectory(
            this, tr("Export %1 assets to folder").arg(recs.size()));
        if (out_dir.isEmpty()) return;
        std::set<QString> used;
        for (const jade::AssetRecord* rec : recs) {
            const QString ext = ext_for(*rec);
            QString base = safe_filename(assetbrowser::record_name(*rec));
            if (base.isEmpty()) base = hex_key(rec->key);
            QString path = QDir(out_dir).filePath(
                QStringLiteral("%1_%2%3").arg(base, hex_key(rec->key), ext));
            int n = 1;
            while (used.count(path))
                path = QDir(out_dir).filePath(
                    QStringLiteral("%1_%2_%3%4")
                        .arg(base, hex_key(rec->key))
                        .arg(n++)
                        .arg(ext));
            used.insert(path);
            jobs.push_back({*rec, path});
        }
    }

    std::map<quint32, std::vector<jade::SubEntry>> subs_cache;
    int ok = 0;
    QStringList errors;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const auto& [rec, path] : jobs) {
        try {
            export_one(rec, path, subs_cache);
            ++ok;
        } catch (const std::exception& e) {
            errors << QStringLiteral("%1 %2: %3")
                          .arg(hex_key(rec.key),
                               assetbrowser::record_name(rec), e.what());
        }
    }
    QApplication::restoreOverrideCursor();

    QString summary =
        tr("Exported %1 of %2 asset(s).").arg(ok).arg(jobs.size());
    if (!errors.isEmpty()) {
        summary += tr("\n\nFailures:\n")
                   + QStringList(errors.mid(0, 12)).join(QLatin1Char('\n'));
        if (errors.size() > 12)
            summary += tr("\n… +%1 more").arg(errors.size() - 12);
        QMessageBox::warning(this, tr("Export Selected"), summary);
    } else {
        QMessageBox::information(this, tr("Export Selected"), summary);
    }
}

// Write one record to `path`; throws std::runtime_error on failure.
void AssetBrowserTab::export_one(
    const jade::AssetRecord& rec, const QString& path,
    std::map<quint32, std::vector<jade::SubEntry>>& subs_cache) {
    if (rec.category == jade::CAT_TEXTURE) {
        extract_texture_to_png(bf_path_, rec.parent_index, rec.key, path,
                               bf_.get());
        return;
    }
    if (rec.category == jade::CAT_GEOMETRY) {
        // export_geo_to_glb equivalent (flat single-GEO export).
        auto cit = subs_cache.find(rec.parent_index);
        if (cit == subs_cache.end()) {
            const jade::LzoResult r =
                jade::decompress_lzo(bf_->read_data(rec.parent_index));
            if (!r.ok)
                throw std::runtime_error("could not decompress parent");
            cit = subs_cache
                      .emplace(rec.parent_index,
                               jade::walk_sub_entries(r.data))
                      .first;
        }
        const jade::SubEntry* sub =
            jade::pick_geo_sub(cit->second, rec.key);
        if (!sub) throw std::runtime_error("geo sub-entry not found");
        const jade::GeoInfo geo =
            jade::parse_geometry(sub->data.data(), sub->data.size());
        if (!geo.ok) throw std::runtime_error("could not parse geometry");
        const std::string hex = hex_key_lower(rec.key);
        const std::vector<uint8_t> glb =
            jade::gltfbuild::build_geo_model_glb(geo, "geo_" + hex, hex,
                                                 "geo_" + hex);
        if (glb.empty()) throw std::runtime_error("empty GLB");
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)
            || f.write(reinterpret_cast<const char*>(glb.data()),
                       qint64(glb.size()))
                   != qint64(glb.size()))
            throw std::runtime_error("could not write GLB");
        return;
    }
    // Legacy path: material / gao (incl. markers) / animation / other.
    auto cit = subs_cache.find(rec.parent_index);
    if (cit == subs_cache.end()) {
        const jade::LzoResult r =
            jade::decompress_lzo(bf_->read_data(rec.parent_index));
        if (!r.ok) throw std::runtime_error("could not decompress parent");
        cit = subs_cache
                  .emplace(rec.parent_index, jade::walk_sub_entries(r.data))
                  .first;
    }
    if (rec.sub_index >= cit->second.size())
        throw std::runtime_error("sub-index out of range");
    const jade::SubEntry& sub = cit->second[rec.sub_index];
    const QString ext = QFileInfo(path).suffix().isEmpty()
                            ? ext_for(rec)
                            : "." + QFileInfo(path).suffix();
    QString err;
    if (!preview_->export_sub(sub, cit->second, export_cat_key(rec), path,
                              ext, &err))
        throw std::runtime_error(err.toStdString());
}

// Decompress the parent, find the sub, route through
// PreviewPanel::export_sub with a format picker.
void AssetBrowserTab::export_asset(const jade::AssetRecord& rec) {
    if (!bf_) return;
    const QString cat_key = export_cat_key(rec);
    const auto formats = PreviewPanel::formats_for(cat_key);
    if (formats.isEmpty()) return;
    QStringList filters;
    for (const auto& [label, ext] : formats)
        filters << QStringLiteral("%1 (*%2)").arg(label, ext);
    const QString def = hex_key(rec.key) + formats.first().second;
    QString selected;
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export %1").arg(qs(rec.category)), def,
        filters.join(QStringLiteral(";;")), &selected);
    if (path.isEmpty()) return;
    // Resolve which extension the user picked from the filter label.
    QString fmt_ext = formats.first().second;
    for (const auto& [label, ext] : formats)
        if (selected.startsWith(label)) {
            fmt_ext = ext;
            break;
        }
    // Load parent + extract the sub.
    std::vector<jade::SubEntry> subs;
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bf_->read_data(rec.parent_index));
        if (!r.ok) throw std::runtime_error("decompress failed");
        subs = jade::walk_sub_entries(r.data);
    } catch (const std::exception& e) {
        QMessageBox::warning(
            this, tr("Export"),
            tr("Could not load parent entry:\n%1").arg(e.what()));
        return;
    }
    if (rec.sub_index >= subs.size()) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Sub-index %1 out of range (parent has "
                                "%2).")
                                 .arg(rec.sub_index)
                                 .arg(subs.size()));
        return;
    }
    QString err;
    if (!preview_->export_sub(subs[rec.sub_index], subs, cat_key, path,
                              fmt_ext, &err)) {
        QMessageBox::warning(this, tr("Export"),
                             tr("Export failed:\n%1").arg(err));
        return;
    }
    QMessageBox::information(
        this, tr("Export"),
        tr("Exported to %1").arg(QFileInfo(path).fileName()));
}

// ── Phase 3: end-to-end texture round-trip ──

// Fast-path: PNG/DDS file → ReplaceTexture op in the active project. The
// modder already has the right texture under their cursor in the browser.
void AssetBrowserTab::replace_texture_inline(const jade::AssetRecord& rec) {
    if (!project_) {
        QMessageBox::warning(
            this, tr("Replace texture"),
            tr("Open or create a Mod Project (File menu) first — the\n"
               "replacement is recorded as an operation, not written to\n"
               "the base archive."));
        return;
    }
    if (project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Replace texture"),
            tr("Save the project once before adding assets — the asset\n"
               "store needs a directory to copy the source into."));
        return;
    }

    const QString src = QFileDialog::getOpenFileName(
        this, tr("Replacement image for %1").arg(hex_key(rec.key)),
        QString(),
        tr("Image files (*.png *.dds *.bmp *.tga *.jpg);;All Files (*)"));
    if (src.isEmpty()) return;

    // Eager validation — catches problems before any state is touched.
    const auto [target_dims, issues] =
        validate_texture_replacement(rec, src);
    QStringList errs, warns;
    for (const auto& [lvl, m] : issues) {
        if (lvl == QLatin1String("error")) errs << m;
        else warns << m;
    }
    if (!errs.isEmpty()) {
        QMessageBox::warning(this, tr("Validation failed"),
                             errs.join(QLatin1Char('\n')));
        return;
    }
    if (!warns.isEmpty()) {
        if (QMessageBox::question(
                this, tr("Validation warnings"),
                warns.join(QLatin1Char('\n')) + tr("\n\nProceed anyway?"),
                QMessageBox::Yes | QMessageBox::No)
            != QMessageBox::Yes)
            return;
    }

    // Import + record op.
    QString err;
    const QString asset_ref = project_->import_asset(src, &err);
    if (asset_ref.isEmpty()) {
        QMessageBox::warning(this, tr("Replace texture"),
                             tr("Could not import asset:\n%1").arg(err));
        return;
    }

    auto fit = bf_->files.find(rec.parent_index);
    if (fit == bf_->files.end()) {
        QMessageBox::warning(this, tr("Replace texture"),
                             tr("Parent entry not in archive."));
        return;
    }

    // ReplaceTexture op dict shape (ops_texture.py).
    Value op = jade::json::make_obj();
    op.obj["op"] = jade::json::make_str("replace_texture");
    Value tgt = jade::json::make_obj();
    tgt.obj["entry_key"] = jade::json::make_str(hex_key_lower(fit->second.key));
    tgt.obj["sub_key"] = jade::json::make_str(hex_key_lower(rec.key));
    op.obj["target"] = std::move(tgt);
    Value prm = jade::json::make_obj();
    prm.obj["source"] = jade::json::make_str(asset_ref.toStdString());
    prm.obj["encode"] = jade::json::make_str("auto");
    prm.obj["mips"] = jade::json::make_bool(true);
    op.obj["params"] = std::move(prm);
    const QString dims =
        target_dims.first
            ? QStringLiteral(" (%1×%2)")
                  .arg(target_dims.first)
                  .arg(target_dims.second)
            : QString();
    const QString label = tr("Replace texture %1%2 <-%3")
                              .arg(hex_key(rec.key), dims,
                                   QFileInfo(src).fileName());
    op.obj["label"] = jade::json::make_str(label.toStdString());
    const QString op_id = project_->add_operation(std::move(op));

    QMessageBox::information(
        this, tr("Replace texture"),
        tr("Added %1: %2\n\nUse the Project tab → Build to write the "
           "modded archive.")
            .arg(op_id, label));
}

// Inspect src_path against the target texture metadata.
std::pair<std::pair<quint32, quint32>,
          std::vector<std::pair<QString, QString>>>
AssetBrowserTab::validate_texture_replacement(const jade::AssetRecord& rec,
                                              const QString& src_path) {
    std::vector<std::pair<QString, QString>> issues;
    std::pair<quint32, quint32> target_dims{0, 0};

    // Decode source (QImage handles PNG/BMP/TGA/JPG; DDS via jade).
    quint32 sw = 0, sh = 0;
    if (src_path.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) {
        QFile f(src_path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray raw = f.readAll();
            const jade::DdsImage img = jade::read_dds(
                reinterpret_cast<const uint8_t*>(raw.constData()),
                size_t(raw.size()));
            if (img.ok) {
                sw = img.width;
                sh = img.height;
            }
        }
    } else {
        const QImage img(src_path);
        if (!img.isNull()) {
            sw = quint32(img.width());
            sh = quint32(img.height());
        }
    }
    if (!sw || !sh)
        return {target_dims,
                {{QStringLiteral("error"), tr("cannot open image")}}};

    // Inspect target.
    std::vector<jade::SubEntry> subs;
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bf_->read_data(rec.parent_index));
        if (!r.ok) throw std::runtime_error("decompress failed");
        subs = jade::walk_sub_entries(r.data);
    } catch (const std::exception& e) {
        return {target_dims,
                {{QStringLiteral("error"),
                  tr("could not load parent: %1").arg(e.what())}}};
    }
    if (rec.sub_index >= subs.size())
        return {target_dims,
                {{QStringLiteral("error"),
                  tr("sub-entry index out of range")}}};
    const jade::SubEntry& sub = subs[rec.sub_index];
    const jade::TexInfo ti =
        jade::parse_texture(sub.data.data(), sub.data.size());
    if (!ti.valid)
        return {target_dims,
                {{QStringLiteral("error"),
                  tr("target sub-entry is not a texture")}}};
    const size_t pixlen =
        sub.data.size() > ti.pix_start ? sub.data.size() - ti.pix_start : 0;
    if (jade::is_placeholder(ti, pixlen))
        return {target_dims,
                {{QStringLiteral("error"),
                  tr("target is a placeholder stub (pixel data too "
                     "small); modding it has no effect — the real texture "
                     "lives in a separate sub-entry with the same "
                     "key.")}}};

    target_dims = {ti.width, ti.height};
    if (sw != ti.width || sh != ti.height)
        issues.push_back(
            {QStringLiteral("warning"),
             tr("Source dimensions %1×%2 differ from target %3×%4; the "
                "patcher will resize via Lanczos.")
                 .arg(sw)
                 .arg(sh)
                 .arg(ti.width)
                 .arg(ti.height)});

    const quint32 fmt = ti.format;
    if (fmt != 0 && fmt != 1 && fmt != 5 && fmt != 6 && fmt != 7
        && fmt != 11)
        issues.push_back(
            {QStringLiteral("warning"),
             tr("Target format %1 isn't a well-tested encoder path; the "
                "result may not load in-game.")
                 .arg(fmt)});
    return {target_dims, issues};
}

// Batch: dump every (real) texture from parent_index plus a manifest.json
// mapping key → file → dims/format.
void AssetBrowserTab::export_all_textures(quint32 parent_index) {
    if (!bf_ || !index_) return;
    const QString out_dir = QFileDialog::getExistingDirectory(
        this, tr("Export folder (will contain PNGs + manifest.json)"));
    if (out_dir.isEmpty()) return;

    std::vector<jade::SubEntry> subs;
    try {
        const jade::LzoResult r =
            jade::decompress_lzo(bf_->read_data(parent_index));
        if (!r.ok) throw std::runtime_error("decompress failed");
        subs = jade::walk_sub_entries(r.data);
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Export all"),
                             tr("Could not load entry:\n%1").arg(e.what()));
        return;
    }
    const QString pname =
        assetbrowser::parent_name_of(bf_.get(), parent_index);

    // io_ops/texture_upscale.py: the "actual" dims live at header +40/+44.
    constexpr size_t HDR_W_OFF = 40, HDR_H_OFF = 44;
    Value manifest_entries = jade::json::make_arr();
    int exported = 0;
    for (size_t si = 0; si < subs.size(); ++si) {
        const jade::SubEntry& s = subs[si];
        if (s.data.empty()
            || !jade::is_texture_entry(s.data.data(), s.data.size()))
            continue;
        const jade::TexInfo ti =
            jade::parse_texture(s.data.data(), s.data.size());
        const size_t pixlen =
            s.data.size() > ti.pix_start ? s.data.size() - ti.pix_start : 0;
        if (!ti.valid || jade::is_placeholder(ti, pixlen))
            continue;  // placeholder — skip
        const quint32 key = s.key;
        quint32 aw = 0, ah = 0;
        if (s.data.size() >= 52) {
            std::memcpy(&aw, s.data.data() + HDR_W_OFF, 4);
            std::memcpy(&ah, s.data.data() + HDR_H_OFF, 4);
        }
        const quint32 eff_w = (aw && ah) ? aw : ti.width;
        const quint32 eff_h = (aw && ah) ? ah : ti.height;
        const QString fname = QStringLiteral("%1_%2x%3_fmt%4.png")
                                  .arg(hex_key(key))
                                  .arg(eff_w)
                                  .arg(eff_h)
                                  .arg(ti.format);
        const QString path = QDir(out_dir).filePath(fname);
        Value ent = jade::json::make_obj();
        ent.obj["key"] = jade::json::make_str(hex_key(key).toStdString());
        ent.obj["sub_index"] = jade::json::make_num(double(si));
        try {
            const std::vector<uint8_t>* pal =
                jade::palette_for_texture(ti, subs);
            const auto [ww, hh] = save_texture_png(
                s.data, ti, pal ? pal->data() : nullptr,
                pal ? pal->size() : 0, path, aw, ah);
            ent.obj["file"] = jade::json::make_str(fname.toStdString());
            ent.obj["width"] = jade::json::make_num(double(ww));
            ent.obj["height"] = jade::json::make_num(double(hh));
            ent.obj["format"] = jade::json::make_num(double(ti.format));
            ent.obj["mip_count"] =
                jade::json::make_num(double(ti.mip_count));
            ++exported;
        } catch (const std::exception& e) {
            // One bad texture should not abort the batch.
            ent.obj["file"] = jade::json::Value();  // null
            ent.obj["error"] = jade::json::make_str(e.what());
        }
        manifest_entries.arr.push_back(std::move(ent));
    }

    auto fit = bf_->files.find(parent_index);
    Value manifest = jade::json::make_obj();
    manifest.obj["format"] = jade::json::make_str("jade-texture-manifest");
    manifest.obj["format_version"] = jade::json::make_num(1);
    manifest.obj["source_archive"] = jade::json::make_str(
        QFileInfo(bf_path_).fileName().toStdString());
    manifest.obj["parent_index"] =
        jade::json::make_num(double(parent_index));
    manifest.obj["parent_name"] = jade::json::make_str(pname.toStdString());
    manifest.obj["parent_key"] = jade::json::make_str(
        hex_key(fit != bf_->files.end() ? fit->second.key : 0)
            .toStdString());
    manifest.obj["textures"] = std::move(manifest_entries);
    const std::string text = jade::json::dump(manifest, 2);
    QSaveFile mf(QDir(out_dir).filePath(QStringLiteral("manifest.json")));
    if (mf.open(QIODevice::WriteOnly)) {
        mf.write(text.data(), qint64(text.size()));
        mf.commit();
    }

    QMessageBox::information(
        this, tr("Export all"),
        tr("Wrote %1 textures + manifest.json to\n%2\n\nEdit the PNGs and "
           "use 'Import textures from folder…' to apply them back.")
            .arg(exported)
            .arg(out_dir));
}

// Read manifest.json and create one ReplaceTexture op per file.
void AssetBrowserTab::import_textures_from_folder(quint32 parent_index) {
    if (!project_ || project_->path.isEmpty()) {
        QMessageBox::warning(this, tr("Import textures"),
                             tr("Open + save a Mod Project first."));
        return;
    }

    const QString folder = QFileDialog::getExistingDirectory(
        this, tr("Folder containing manifest.json"));
    if (folder.isEmpty()) return;
    const QString manifest_path =
        QDir(folder).filePath(QStringLiteral("manifest.json"));
    if (!QFileInfo(manifest_path).isFile()) {
        QMessageBox::warning(this, tr("Import textures"),
                             tr("No manifest.json in %1.").arg(folder));
        return;
    }

    Value manifest;
    try {
        QFile f(manifest_path);
        if (!f.open(QIODevice::ReadOnly))
            throw std::runtime_error("could not open");
        const QByteArray raw = f.readAll();
        manifest =
            jade::json::parse(raw.constData(), size_t(raw.size()));
    } catch (const std::exception& e) {
        QMessageBox::warning(
            this, tr("Import textures"),
            tr("manifest.json parse error:\n%1").arg(e.what()));
        return;
    }

    const Value* fmt = manifest.find("format");
    if (!fmt || !fmt->is_str() || fmt->str != "jade-texture-manifest") {
        QMessageBox::warning(
            this, tr("Import textures"),
            tr("manifest.json: not a Jade texture manifest."));
        return;
    }

    auto fit = bf_->files.find(parent_index);
    if (fit == bf_->files.end()) {
        QMessageBox::warning(this, tr("Import textures"),
                             tr("Parent entry no longer in archive."));
        return;
    }
    const quint32 entry_key = fit->second.key;

    int added = 0, skipped = 0;
    const Value* textures = manifest.find("textures");
    if (textures && textures->is_arr()) {
        for (const Value& e : textures->arr) {
            const Value* fname = e.find("file");
            if (!fname || !fname->is_str() || fname->str.empty()) continue;
            const QString path = QDir(folder).filePath(qs(fname->str));
            if (!QFileInfo(path).isFile()) {
                ++skipped;
                continue;
            }
            const Value* kv = e.find("key");
            bool okp = false;
            const quint32 key =
                kv && kv->is_str()
                    ? QString::fromStdString(kv->str).toUInt(&okp, 16)
                    : 0;
            if (!okp) {
                ++skipped;
                continue;
            }
            QString err;
            const QString asset_ref = project_->import_asset(path, &err);
            if (asset_ref.isEmpty()) {
                ++skipped;
                continue;
            }
            Value op = jade::json::make_obj();
            op.obj["op"] = jade::json::make_str("replace_texture");
            Value tgt = jade::json::make_obj();
            tgt.obj["entry_key"] =
                jade::json::make_str(hex_key_lower(entry_key));
            tgt.obj["sub_key"] = jade::json::make_str(hex_key_lower(key));
            op.obj["target"] = std::move(tgt);
            Value prm = jade::json::make_obj();
            prm.obj["source"] =
                jade::json::make_str(asset_ref.toStdString());
            prm.obj["encode"] = jade::json::make_str("auto");
            prm.obj["mips"] = jade::json::make_bool(true);
            op.obj["params"] = std::move(prm);
            op.obj["label"] = jade::json::make_str(
                tr("Replace texture %1 <-%2")
                    .arg(hex_key(key), qs(fname->str))
                    .toStdString());
            project_->add_operation(std::move(op));
            ++added;
        }
    }

    QMessageBox::information(
        this, tr("Import textures"),
        tr("Added %1 replace_texture ops.\nSkipped %2 entries (file "
           "missing or unreadable).")
            .arg(added)
            .arg(skipped));
}

void AssetBrowserTab::select_record(const jade::AssetRecord& rec) {
    populate_refs(rec);
    load_entry(rec.parent_index, rec.sub_index);
}

void AssetBrowserTab::load_entry(quint32 parent_index,
                                 long long prefer_sub) {
    if (loader_ && loader_->isRunning()) {
        pending_load_ = {parent_index, prefer_sub};
        return;
    }
    loader_ = new AssetEntryLoader(bf_, parent_index, prefer_sub, this);
    connect(loader_, &AssetEntryLoader::loaded, this,
            &AssetBrowserTab::on_entry_loaded);
    connect(loader_, &AssetEntryLoader::finished, this,
            &AssetBrowserTab::on_loader_done);
    loader_->start();
}

void AssetBrowserTab::on_entry_loaded(const QString& error) {
    if (!loader_) return;
    if (!error.isEmpty() || !loader_->result_ok) {
        preview_->show_error(error.isEmpty() ? tr("Load failed") : error);
        return;
    }
    // Show in preview, auto-selecting the chosen sub.
    PreviewEntryResult result;
    result.subs = std::move(loader_->subs);
    result.raw_size = loader_->raw_size;
    result.dec_size = loader_->dec_size;
    result.index = loader_->parent_index();
    preview_->show_entry(result, bf_, bf_path_, loader_->prefer_sub());
}

void AssetBrowserTab::on_loader_done() {
    if (loader_) {
        loader_->deleteLater();
        loader_ = nullptr;
    }
    auto pending = pending_load_;
    pending_load_.reset();
    if (pending) load_entry(pending->first, pending->second);
}

void AssetBrowserTab::populate_refs(const jade::AssetRecord& rec) {
    refs_tree_->clear();
    if (!index_) return;

    // Outgoing
    auto* out_root = new QTreeWidgetItem(refs_tree_);
    QFont f = out_root->font(0);
    f.setBold(true);
    out_root->setFont(0, f);
    out_root->setExpanded(true);
    int out_count = 0;
    for (const auto& [ref_type, ref_keys] : rec.refs) {
        std::set<quint32> seen;
        for (quint32 rk : ref_keys) {
            if (!rk || rk == 0xFFFFFFFFu || seen.count(rk)) continue;
            seen.insert(rk);
            const auto matches = index_->find_resolved(rk);
            if (!matches.empty()) {
                for (const jade::AssetRecord* m : matches) {
                    auto* it = new QTreeWidgetItem(out_root);
                    it->setText(0, QStringLiteral("%1: %2").arg(
                                       qs(ref_type),
                                       assetbrowser::record_name(*m)));
                    it->setText(1, hex_key(rk));
                    it->setText(2, qs(m->category));
                    it->setData(0, Qt::UserRole,
                                QStringLiteral("%1:%2")
                                    .arg(m->parent_index)
                                    .arg(m->sub_index));
                    bool found = false;
                    const QColor c =
                        category_color(qs(m->category), &found);
                    if (found) it->setForeground(2, c);
                    ++out_count;
                }
            } else {
                auto* it = new QTreeWidgetItem(out_root);
                it->setText(0, tr("%1: (unresolved)").arg(qs(ref_type)));
                it->setText(1, hex_key(rk));
                it->setForeground(0, QColor(160, 100, 100));
                ++out_count;
            }
        }
    }
    out_root->setText(0, tr("References (%1)").arg(out_count));

    // Reverse
    const auto referrers = index_->referrers(rec.key);
    auto* rev_root = new QTreeWidgetItem(refs_tree_);
    rev_root->setText(0, tr("Referenced by (%1)").arg(referrers.size()));
    f = rev_root->font(0);
    f.setBold(true);
    rev_root->setFont(0, f);
    rev_root->setExpanded(referrers.size() <= 15);
    size_t shown = 0;
    for (const jade::AssetRecord* r : referrers) {
        if (shown++ >= 200) break;  // cap; absurd-count props
        auto* it = new QTreeWidgetItem(rev_root);
        it->setText(0, assetbrowser::record_name(*r));
        it->setText(1, hex_key(r->key));
        it->setText(2, qs(r->category));
        it->setData(0, Qt::UserRole,
                    QStringLiteral("%1:%2")
                        .arg(r->parent_index)
                        .arg(r->sub_index));
        bool found = false;
        const QColor c = category_color(qs(r->category), &found);
        if (found) it->setForeground(2, c);
    }
    if (referrers.size() > 200) {
        auto* it = new QTreeWidgetItem(rev_root);
        it->setText(0, tr("… +%1 more").arg(referrers.size() - 200));
        it->setForeground(0, QColor(120, 120, 120));
        it->setFlags(it->flags() & ~Qt::ItemIsSelectable);
    }
}

void AssetBrowserTab::on_ref_clicked(QTreeWidgetItem* item, int) {
    const QString asset_id = item->data(0, Qt::UserRole).toString();
    if (asset_id.isEmpty() || !index_) return;
    const QStringList parts = asset_id.split(QLatin1Char(':'));
    if (parts.size() != 2) return;
    const quint32 pidx = parts[0].toUInt();
    const quint32 sidx = parts[1].toUInt();
    const jade::AssetRecord* rec = index_->find_by_id(pidx, sidx);
    if (rec) select_record(*rec);
}

// ── Modified-key tracking ──

void AssetBrowserTab::refresh_modified() {
    std::set<quint32> nw;
    if (project_) {
        for (const Value& op : project_->operations) {
            const Value* en = op.find("enabled");
            if (en && en->type == Value::Type::Bool && !en->b) continue;
            const Value* tgt = op.find("target");
            const Value* ek = tgt ? tgt->find("entry_key") : nullptr;
            if (ek && ek->is_str()) {
                bool okp = false;
                const quint32 k =
                    QString::fromStdString(ek->str).toUInt(&okp, 16);
                if (okp) nw.insert(k);
            }
        }
    }
    modified_keys_ = nw;
    table_model_->set_modified(nw);
    grid_model_->set_modified(nw);
    if (scope_combo_->currentText() == tr(SCOPE_MODIFIED)) apply_filter();
}

// ── Thumbnails ──

void AssetBrowserTab::defer_viewport_thumbs() {
    if (stack_->currentIndex() != DISPLAY_GRID) return;
    thumb_timer_->start();
}

void AssetBrowserTab::enqueue_visible_thumbs() {
    if (!index_ || stack_->currentIndex() != DISPLAY_GRID) return;
    AssetGridModel* model = grid_model_;
    if (model->rowCount() == 0) return;
    // Find visible row range via indexAt on the viewport corners.
    QWidget* vp = grid_->viewport();
    const QModelIndex top_idx = grid_->indexAt(vp->rect().topLeft());
    const QModelIndex bot_idx = grid_->indexAt(vp->rect().bottomRight());
    int first = top_idx.isValid() ? top_idx.row() : 0;
    int last = bot_idx.isValid() ? bot_idx.row() : model->rowCount() - 1;
    // Pad a bit so scrolling doesn't immediately show placeholders.
    first = std::max(0, first - 8);
    last = std::min(model->rowCount() - 1, last + 32);

    std::vector<AssetThumbnailWorker::Job> jobs;
    for (int row = first; row <= last; ++row) {
        if (model->thumb_state(row) != 2) continue;  // not absent
        const jade::AssetRecord* rec = model->record_at(row);
        if (!rec || rec->category != jade::CAT_TEXTURE) {
            // Mark non-textures as "failed" so we don't retry every
            // scroll.
            model->mark_thumb_failed(row);
            continue;
        }
        jobs.push_back({row, rec->parent_index, rec->sub_index});
        if (jobs.size() >= 80) break;
    }
    if (!jobs.empty()) start_thumbs(std::move(jobs));
}

void AssetBrowserTab::start_thumbs(
    std::vector<AssetThumbnailWorker::Job> jobs) {
    if (thumb_worker_ && thumb_worker_->isRunning()) {
        thumb_worker_->add_jobs(std::move(jobs));
        return;
    }
    thumb_worker_ = new AssetThumbnailWorker(bf_, std::move(jobs), this);
    connect(thumb_worker_, &AssetThumbnailWorker::thumb_ready, this,
            &AssetBrowserTab::on_thumb_ready);
    connect(thumb_worker_, &AssetThumbnailWorker::finished, this,
            &AssetBrowserTab::on_thumb_done);
    thumb_worker_->start();
}

void AssetBrowserTab::on_thumb_ready(int row, const QImage& img) {
    if (img.isNull())
        grid_model_->set_thumb(row, QIcon(), true);
    else
        grid_model_->set_thumb(row, QIcon(QPixmap::fromImage(img)), false);
}

void AssetBrowserTab::on_thumb_done() {
    if (thumb_worker_) {
        thumb_worker_->deleteLater();
        thumb_worker_ = nullptr;
    }
    // If filters changed, more work may be queued.
    if (stack_->currentIndex() == DISPLAY_GRID) defer_viewport_thumbs();
}

void AssetBrowserTab::cancel_thumbs() {
    if (thumb_worker_) {
        thumb_worker_->cancel();
        thumb_worker_->wait(1500);
        thumb_worker_->deleteLater();
        thumb_worker_ = nullptr;
    }
}
