// AssetBrowserTab.hpp — Asset Browser, sub-entry granularity (port of
// gui/asset_browser_tab.py, Phase 2 redux).
//
// Drives off jade::AssetIndex: every BF sub-entry is a typed asset row.
// Model-based QTableView / QListView views virtualize rendering so even
// WW's 2.5 M-row archive renders fine.
//
// Layout: left sidebar (By Type / By Source trees), toolbar (search, scope
// filter, display toggle, Export Selected), main view (table or icon
// grid with lazy texture thumbnails), references panel, and the right
// PreviewPanel showing the parent entry with the selected sub-entry
// pre-highlighted.
//
// Workers: three background QThread subclasses (the moved-worker pattern
// caused teardown deadlocks earlier — see zones_tab._ZoneScanner):
// IndexScanner (builds the AssetIndex once per archive), EntryLoader
// (decompresses one parent entry for the preview pane), ThumbnailWorker
// (decodes texture sub-entries to small QImages, batched by parent).
//
// PORT NOTE: the native jade::AssetRecord deliberately omits the Python's
// resolved .name / .parent_name display strings (core gap — see
// AssetIndex.hpp). The browser derives parent names from the BigFile FAT
// and shows "<category> 0xKEY" as the record name until the core gains
// name resolution.
#pragma once

#include <QAbstractListModel>
#include <QAbstractTableModel>
#include <QIcon>
#include <QImage>
#include <QMutex>
#include <QThread>
#include <QWidget>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include "jade/AssetIndex.hpp"

namespace jade { class BigFile; }
class PreviewPanel;
class ProjectDoc;
class QComboBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListView;
class QPushButton;
class QStackedWidget;
class QTabWidget;
class QTableView;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;

namespace assetbrowser {

// Sidebar filter key: ('all') / ('cat', category) / ('gro', gro_type) /
// ('parent', parent_index).
struct Filter {
    enum Kind { All, Cat, Gro, Parent } kind = All;
    QString cat;
    quint32 gro = 0;
    quint32 parent = 0;
    bool operator==(const Filter& o) const {
        return kind == o.kind && cat == o.cat && gro == o.gro
               && parent == o.parent;
    }
};

// Display-name helpers shared by both models (the PORT NOTE derivations).
QString record_name(const jade::AssetRecord& rec);
QString parent_name_of(const jade::BigFile* bf, quint32 parent_index);

}  // namespace assetbrowser

// Flat 5-column model over either *all* asset records or a subset. The
// subset is a list of integer row indices into AssetIndex.records;
// nullopt means "every record" (saves materialising a 2.5 M-element list).
class AssetTableModel : public QAbstractTableModel {
    Q_OBJECT
public:
    explicit AssetTableModel(QObject* parent = nullptr);

    void set_index(std::shared_ptr<jade::AssetIndex> idx,
                   const jade::BigFile* bf);
    // nullopt shows everything; else a list of record indices.
    void set_filter(std::optional<std::vector<int>> view_indices);
    void sort(int column, Qt::SortOrder order) override;
    void set_modified(const std::set<quint32>& modified_keys);
    const jade::AssetRecord* record_at(int row) const;

    int rowCount(const QModelIndex& = QModelIndex()) const override;
    int columnCount(const QModelIndex& = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    void apply_sort_inplace();

    std::shared_ptr<jade::AssetIndex> idx_;
    const jade::BigFile* bf_ = nullptr;
    std::optional<std::vector<int>> view_;
    std::set<quint32> modified_;
    int sort_col_ = -1;
    Qt::SortOrder sort_order_ = Qt::AscendingOrder;
};

// Grid model: one item per row, DecorationRole returns the icon.
// Thumbnails are cached by row index in the current view.
class AssetGridModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit AssetGridModel(QObject* parent = nullptr);

    void set_index(std::shared_ptr<jade::AssetIndex> idx,
                   const jade::BigFile* bf);
    void set_filter(std::optional<std::vector<int>> view_indices);
    void set_modified(const std::set<quint32>& modified_keys);
    // Worker pushes a real thumbnail icon (null icon = decode failed).
    void set_thumb(int row, const QIcon& icon, bool failed);
    // 'real' == 0 | 'failed' == 1 | 'absent' == 2.
    int thumb_state(int row) const;
    void mark_thumb_failed(int row) { thumb_cache_[row] = {QIcon(), true}; }
    const jade::AssetRecord* record_at(int row) const;

    int rowCount(const QModelIndex& = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

private:
    QIcon placeholder_for(const QString& cat) const;

    std::shared_ptr<jade::AssetIndex> idx_;
    const jade::BigFile* bf_ = nullptr;
    std::optional<std::vector<int>> view_;
    std::set<quint32> modified_;
    mutable std::map<int, std::pair<QIcon, bool>> thumb_cache_;
    mutable std::map<QString, QIcon> placeholder_;
};

// Build an AssetIndex in a background thread (_IndexScanner).
class AssetIndexScanner : public QThread {
    Q_OBJECT
public:
    explicit AssetIndexScanner(std::shared_ptr<jade::BigFile> bf,
                               QObject* parent = nullptr)
        : QThread(parent), bf_(std::move(bf)) {}
    void cancel() { cancel_ = true; }
    std::shared_ptr<jade::AssetIndex> take_index() {
        return std::move(index_);
    }

signals:
    void progress(int current, int total);
    void done(const QString& error);  // empty == success

protected:
    void run() override;

private:
    std::shared_ptr<jade::BigFile> bf_;
    std::shared_ptr<jade::AssetIndex> index_;
    volatile bool cancel_ = false;
};

// Decompress + sub-parse one parent BF entry (_EntryLoader).
class AssetEntryLoader : public QThread {
    Q_OBJECT
public:
    AssetEntryLoader(std::shared_ptr<jade::BigFile> bf, quint32 parent_index,
                     long long prefer_sub, QObject* parent = nullptr)
        : QThread(parent), bf_(std::move(bf)), fi_(parent_index),
          prefer_sub_(prefer_sub) {}

    quint32 parent_index() const { return fi_; }
    long long prefer_sub() const { return prefer_sub_; }
    // ok + subs/raw_size/dec_size pulled by the receiver after `loaded`.
    bool result_ok = false;
    std::vector<jade::SubEntry> subs;
    size_t raw_size = 0, dec_size = 0;

signals:
    void loaded(const QString& error);  // empty == success

protected:
    void run() override;

private:
    std::shared_ptr<jade::BigFile> bf_;
    quint32 fi_;
    long long prefer_sub_;
};

// Decode texture sub-entries to 96 px QImages, batched by parent
// (_ThumbnailWorker). Jobs are (row, parent_index, sub_index), sorted by
// parent so adjacent jobs reuse one decompress.
class AssetThumbnailWorker : public QThread {
    Q_OBJECT
public:
    static constexpr int SIZE = 96;
    struct Job {
        int row;
        quint32 parent_index;
        quint32 sub_index;
    };

    AssetThumbnailWorker(std::shared_ptr<jade::BigFile> bf,
                         std::vector<Job> jobs, QObject* parent = nullptr);
    void cancel() { cancel_ = true; }
    void add_jobs(std::vector<Job> more);

signals:
    void thumb_ready(int row, const QImage& img);  // null img == failed

protected:
    void run() override;

private:
    QImage decode_sub(const jade::SubEntry& sub,
                      const std::vector<jade::SubEntry>& all_subs) const;

    std::shared_ptr<jade::BigFile> bf_;
    std::vector<Job> jobs_;
    QMutex* jobs_mutex_;
    volatile bool cancel_ = false;
};

// Sub-entry-granular explorer with thumbnails, refs, preview.
class AssetBrowserTab : public QWidget {
    Q_OBJECT
public:
    explicit AssetBrowserTab(QWidget* parent = nullptr);
    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& path);
    // The archive's AssetIndex once the scan has finished, else nullptr.
    // Shared with other tabs (Mesh Swap) for cross-bin material resolution.
    std::shared_ptr<jade::AssetIndex> asset_index() const { return index_; }
    void set_project(ProjectDoc* proj);

signals:
    // Right-click "Send to X" — MainWindow routes to the matching tab and
    // calls its receive_asset slot.
    void send_asset_to_tab(const QString& target, quint32 parent_index,
                           quint32 key, const QString& category);
    // The asset-index scan drives the main window's modal load dialog.
    void scan_progress(int current, int total);
    void scan_finished();

private slots:
    void on_scan_progress(int current, int total);
    void on_scan_done(const QString& err);
    void apply_sidebar_filter();
    void on_filter_changed();
    void apply_filter();
    void on_display_changed(int idx);
    void on_table_clicked(const QModelIndex& index);
    void on_grid_clicked(const QModelIndex& index);
    void on_export_selected();
    void on_ref_clicked(QTreeWidgetItem* item, int col);
    void refresh_modified();
    void defer_viewport_thumbs();
    void enqueue_visible_thumbs();
    void on_thumb_ready(int row, const QImage& img);
    void on_thumb_done();
    void on_entry_loaded(const QString& error);
    void on_loader_done();

private:
    void start_scan();
    void cancel_scan();
    void build_sidebar();
    void on_sidebar_changed(QTreeWidget* tree);
    QString filter_label() const;
    void on_context_menu(QWidget* view, const QPoint& pos);
    void filter_to_source(quint32 parent_index);
    std::vector<const jade::AssetRecord*> selected_records() const;
    QString export_cat_key(const jade::AssetRecord& rec) const;
    QString ext_for(const jade::AssetRecord& rec) const;
    void export_records(std::vector<const jade::AssetRecord*> recs);
    void export_one(const jade::AssetRecord& rec, const QString& path,
                    std::map<quint32, std::vector<jade::SubEntry>>& cache);
    void export_asset(const jade::AssetRecord& rec);
    void replace_texture_inline(const jade::AssetRecord& rec);
    // ((target_w, target_h) [0,0 = unknown], [(level, message), …]).
    std::pair<std::pair<quint32, quint32>,
              std::vector<std::pair<QString, QString>>>
    validate_texture_replacement(const jade::AssetRecord& rec,
                                 const QString& src_path);
    void export_all_textures(quint32 parent_index);
    void import_textures_from_folder(quint32 parent_index);
    void select_record(const jade::AssetRecord& rec);
    void load_entry(quint32 parent_index, long long prefer_sub);
    void populate_refs(const jade::AssetRecord& rec);
    void start_thumbs(std::vector<AssetThumbnailWorker::Job> jobs);
    void cancel_thumbs();
    static bool filter_sidebar_item(QTreeWidgetItem* item,
                                    const QString& text);

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    ProjectDoc* project_ = nullptr;
    std::shared_ptr<jade::AssetIndex> index_;
    std::set<quint32> modified_keys_;
    assetbrowser::Filter filter_;

    AssetIndexScanner* scan_thread_ = nullptr;
    AssetEntryLoader* loader_ = nullptr;
    std::optional<std::pair<quint32, long long>> pending_load_;
    AssetThumbnailWorker* thumb_worker_ = nullptr;

    QLineEdit* search_ = nullptr;
    QComboBox* scope_combo_ = nullptr;
    QComboBox* display_combo_ = nullptr;
    QPushButton* export_sel_btn_ = nullptr;
    QTreeWidget* type_tree_ = nullptr;
    QTreeWidget* source_tree_ = nullptr;
    QTabWidget* sidebar_tabs_ = nullptr;
    QLineEdit* sidebar_filter_ = nullptr;
    AssetTableModel* table_model_ = nullptr;
    AssetGridModel* grid_model_ = nullptr;
    QTableView* table_ = nullptr;
    QListView* grid_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* status_ = nullptr;
    QTreeWidget* refs_tree_ = nullptr;
    PreviewPanel* preview_ = nullptr;
    QTimer* filter_timer_ = nullptr;
    QTimer* thumb_timer_ = nullptr;
};
