// MainWindow.hpp — main application window with tab-based interface
// (port of gui/main_window.py).
//
// Holds the current ProjectDoc (or nullptr) and the open base archive
// view. The base archive is loaded read-only — only the builder ever
// writes a .bf.
#pragma once

#include <QDialog>
#include <QMainWindow>
#include <QThread>
#include <memory>

namespace jade { class BigFile; }

class QLabel;
class QProgressBar;
class QStatusBar;
class QTabWidget;

class AssetBrowserTab;
class PlacementTab;
class MeshSwapTab;
class TextureSwapTab;
class AddAssetsTab;
class LevelBlenderTab;
class BakeLightmapsTab;
class ZonesTab;
class CreateZoneTab;
class EditZoneDepsTab;
class ProjectPanel;
class ProjectDoc;

// Worker for loading a BigFile in a background thread (_BFLoader).
class BFLoader : public QObject {
    Q_OBJECT
public:
    explicit BFLoader(const QString& path) : path_(path) {}
    std::shared_ptr<jade::BigFile> take_result() { return std::move(bf_); }

public slots:
    void run();

signals:
    void finished(const QString& error_msg);  // empty on success

private:
    QString path_;
    std::shared_ptr<jade::BigFile> bf_;
};

// Application-modal "please wait" dialog shown during a long load
// (_BusyModal).
//
// The user cannot dismiss it — no close button, Escape is swallowed, and
// a user-initiated close is refused. It closes only when the owner calls
// finish(), so the app stays locked until the load completes.
class BusyModal : public QDialog {
    Q_OBJECT
public:
    explicit BusyModal(const QString& message, QWidget* parent = nullptr);
    void set_message(const QString& text);
    // Switch to a determinate bar; total <= 0 keeps the busy animation.
    void set_progress(int current, int total);
    // Programmatically dismiss the dialog once the load is done.
    void finish();

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void closeEvent(QCloseEvent* e) override;

private:
    bool allow_close_ = false;
    QLabel* label_ = nullptr;
    QProgressBar* bar_ = nullptr;
};

// Top-level window: File menu, project state, tabs, status bar.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

    std::shared_ptr<jade::BigFile> bigfile() const { return bf_; }
    QString bf_path() const { return bf_path_; }
    ProjectDoc* project() const { return project_; }

    // Public so main() can open a .bf passed on the command line.
    void load_bf(const QString& path);

protected:
    void closeEvent(QCloseEvent* ev) override;

private slots:
    void on_open();
    void on_bf_loaded(const QString& err);
    void on_index_progress(int current, int total);
    void on_index_finished();
    void on_new_project();
    void on_open_project();
    void on_save_project();
    void on_save_as();
    void on_close_project();
    void on_send_texture_to_swap(quint32 entry_idx, quint32 texture_key);
    void on_send_asset_to_tab(const QString& target, quint32 parent_index,
                              quint32 key, const QString& category);

private:
    void refresh_title();
    bool confirm_discard_unsaved();
    void dismiss_load_modal();
    void set_project(ProjectDoc* proj);

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    QThread* loader_thread_ = nullptr;
    BFLoader* loader_ = nullptr;
    BusyModal* load_modal_ = nullptr;
    ProjectDoc* project_ = nullptr;

    QTabWidget* tabs_ = nullptr;
    AssetBrowserTab* asset_browser_ = nullptr;
    PlacementTab* placement_tab_ = nullptr;
    MeshSwapTab* mesh_swap_tab_ = nullptr;
    TextureSwapTab* texture_swap_tab_ = nullptr;
    AddAssetsTab* add_assets_tab_ = nullptr;
    LevelBlenderTab* level_blender_tab_ = nullptr;
    BakeLightmapsTab* bake_lightmaps_tab_ = nullptr;
    ZonesTab* zones_tab_ = nullptr;
    CreateZoneTab* create_zone_tab_ = nullptr;
    EditZoneDepsTab* edit_zone_deps_tab_ = nullptr;
    ProjectPanel* project_panel_ = nullptr;
    QStatusBar* status_ = nullptr;
};
