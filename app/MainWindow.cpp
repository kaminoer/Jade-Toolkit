#include "MainWindow.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>

#include "jade/BigFile.hpp"
#include "jade/GameProfiles.hpp"

#include "AddAssetsTab.hpp"
#include "AssetBrowserTab.hpp"
#include "BakeLightmapsTab.hpp"
#include "CreateZoneTab.hpp"
#include "EditZoneDepsTab.hpp"
#include "LevelBlenderTab.hpp"
#include "MeshSwapTab.hpp"
#include "PlacementTab.hpp"
#include "ProjectDoc.hpp"
#include "ProjectPanel.hpp"
#include "TextureSwapTab.hpp"
#include "ZonesTab.hpp"

// ── BFLoader ──

void BFLoader::run() {
    try {
        auto bf = std::make_shared<jade::BigFile>();
        bf->open(path_.toStdString());
        bf_ = std::move(bf);
        emit finished(QString());
    } catch (const std::exception& e) {
        emit finished(QString::fromUtf8(e.what()));
    }
}

// ── BusyModal ──

BusyModal::BusyModal(const QString& message, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Please wait"));
    setModal(true);
    setWindowModality(Qt::ApplicationModal);
    setWindowFlags(Qt::Dialog | Qt::CustomizeWindowHint
                   | Qt::WindowTitleHint);
    setMinimumWidth(420);
    auto* lay = new QVBoxLayout(this);
    label_ = new QLabel(message);
    lay->addWidget(label_);
    bar_ = new QProgressBar();
    bar_->setRange(0, 0);  // indeterminate "busy" animation
    bar_->setTextVisible(false);
    lay->addWidget(bar_);
}

void BusyModal::set_message(const QString& text) { label_->setText(text); }

void BusyModal::set_progress(int current, int total) {
    if (total > 0) {
        bar_->setRange(0, total);
        bar_->setValue(current);
        bar_->setTextVisible(true);
        bar_->setFormat(QStringLiteral("%v / %m"));
    } else {
        bar_->setRange(0, 0);
        bar_->setTextVisible(false);
    }
}

void BusyModal::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape) {
        e->ignore();
        return;
    }
    QDialog::keyPressEvent(e);
}

void BusyModal::closeEvent(QCloseEvent* e) {
    if (allow_close_)
        QDialog::closeEvent(e);
    else
        e->ignore();
}

void BusyModal::finish() {
    allow_close_ = true;
    close();
}

// ── MainWindow ──

MainWindow::MainWindow() {
    resize(1400, 850);

    // Menu
    QMenuBar* menu = menuBar();
    QMenu* file_menu = menu->addMenu(tr("&File"));

    QAction* new_proj_act = file_menu->addAction(tr("&New Project…"));
    new_proj_act->setShortcut(QStringLiteral("Ctrl+N"));
    connect(new_proj_act, &QAction::triggered, this,
            &MainWindow::on_new_project);

    QAction* open_proj_act = file_menu->addAction(tr("Open &Project…"));
    open_proj_act->setShortcut(QStringLiteral("Ctrl+Shift+O"));
    connect(open_proj_act, &QAction::triggered, this,
            &MainWindow::on_open_project);

    QAction* save_proj_act = file_menu->addAction(tr("&Save Project"));
    save_proj_act->setShortcut(QStringLiteral("Ctrl+S"));
    connect(save_proj_act, &QAction::triggered, this,
            &MainWindow::on_save_project);

    QAction* save_as_act = file_menu->addAction(tr("Save Project &As…"));
    save_as_act->setShortcut(QStringLiteral("Ctrl+Shift+S"));
    connect(save_as_act, &QAction::triggered, this, &MainWindow::on_save_as);

    QAction* close_proj_act = file_menu->addAction(tr("&Close Project"));
    connect(close_proj_act, &QAction::triggered, this,
            &MainWindow::on_close_project);

    file_menu->addSeparator();
    QAction* open_act = file_menu->addAction(tr("&Open Base Archive…"));
    open_act->setShortcut(QStringLiteral("Ctrl+O"));
    connect(open_act, &QAction::triggered, this, &MainWindow::on_open);

    file_menu->addSeparator();
    QAction* quit_act = file_menu->addAction(tr("&Quit"));
    quit_act->setShortcut(QStringLiteral("Ctrl+Q"));
    connect(quit_act, &QAction::triggered, this, &MainWindow::close);

    // Tabs
    tabs_ = new QTabWidget();
    asset_browser_ = new AssetBrowserTab();
    placement_tab_ = new PlacementTab();
    mesh_swap_tab_ = new MeshSwapTab();
    texture_swap_tab_ = new TextureSwapTab();
    add_assets_tab_ = new AddAssetsTab();
    level_blender_tab_ = new LevelBlenderTab();
    bake_lightmaps_tab_ = new BakeLightmapsTab();
    zones_tab_ = new ZonesTab();
    create_zone_tab_ = new CreateZoneTab();
    edit_zone_deps_tab_ = new EditZoneDepsTab();
    project_panel_ = new ProjectPanel();

    tabs_->addTab(asset_browser_, tr("Asset Browser"));
    tabs_->addTab(project_panel_, tr("Project"));
    tabs_->addTab(zones_tab_, tr("Zones"));
    tabs_->addTab(create_zone_tab_, tr("Create Zone"));
    tabs_->addTab(edit_zone_deps_tab_, tr("Edit Zone Deps"));
    tabs_->addTab(placement_tab_, tr("Level Editor"));
    tabs_->addTab(mesh_swap_tab_, tr("Mesh Swap"));
    tabs_->addTab(texture_swap_tab_, tr("Texture Swap"));
    tabs_->addTab(add_assets_tab_, tr("Add New Assets"));
    tabs_->addTab(level_blender_tab_, tr("Bake lights"));
    tabs_->addTab(bake_lightmaps_tab_, tr("Bake Lightmaps"));
    setCentralWidget(tabs_);

    // Right-click "Send to X" from the Asset Browser routes here.
    connect(asset_browser_, &AssetBrowserTab::send_asset_to_tab, this,
            &MainWindow::on_send_asset_to_tab);
    // The asset-index scan drives the load modal's progress bar so the
    // modal stays up for the entire open flow (BF open + indexing).
    connect(asset_browser_, &AssetBrowserTab::scan_progress, this,
            &MainWindow::on_index_progress);
    connect(asset_browser_, &AssetBrowserTab::scan_finished, this,
            &MainWindow::on_index_finished);
    // Right-click a texture in the Mesh Swap materials table → Texture Swap.
    connect(mesh_swap_tab_, &MeshSwapTab::send_texture_to_tab, this,
            &MainWindow::on_send_texture_to_swap);

    // Status bar
    status_ = new QStatusBar();
    setStatusBar(status_);
    status_->showMessage(
        tr("Ready — File → New Project, or Open Base Archive to browse."));
    refresh_title();
}

// ───────────────────────── title ──────────────────────────

void MainWindow::refresh_title() {
    QStringList parts{QStringLiteral("Jade Toolkit")};
    if (project_) {
        const QString dirty =
            project_->is_dirty() ? QStringLiteral("*") : QString();
        const QString name = project_->name.isEmpty()
                                 ? QStringLiteral("(unnamed)")
                                 : project_->name;
        parts.prepend(name + dirty);
    }
    if (!bf_path_.isEmpty()) parts.append(QFileInfo(bf_path_).fileName());
    setWindowTitle(parts.join(QStringLiteral("  —  ")));
}

// ─────────────────── confirm-discard guard ────────────────

bool MainWindow::confirm_discard_unsaved() {
    if (!project_ || !project_->is_dirty()) return true;
    const auto r = QMessageBox::question(
        this, tr("Unsaved changes"),
        tr("The project '%1' has unsaved changes. Discard them?")
            .arg(project_->name),
        QMessageBox::Discard | QMessageBox::Cancel);
    return r == QMessageBox::Discard;
}

void MainWindow::closeEvent(QCloseEvent* ev) {
    if (confirm_discard_unsaved())
        ev->accept();
    else
        ev->ignore();
}

// ───────────────────── base archive (read-only) ────────────

void MainWindow::on_open() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Open Base Archive (read-only)"), QString(),
        tr("BigFile (*.bf);;All Files (*)"));
    if (path.isEmpty()) return;
    load_bf(path);
}

void MainWindow::load_bf(const QString& path) {
    status_->showMessage(tr("Loading %1 …").arg(path));
    bf_path_ = path;

    // Application-modal busy dialog — blocks all interaction with the app
    // until the BigFile has finished loading.
    load_modal_ = new BusyModal(
        tr("Loading BigFile…\n%1").arg(QFileInfo(path).fileName()), this);
    load_modal_->show();

    loader_thread_ = new QThread(this);
    loader_ = new BFLoader(path);
    loader_->moveToThread(loader_thread_);
    connect(loader_thread_, &QThread::started, loader_, &BFLoader::run);
    connect(loader_, &BFLoader::finished, this, &MainWindow::on_bf_loaded);
    connect(loader_, &BFLoader::finished, loader_thread_, &QThread::quit);
    connect(loader_thread_, &QThread::finished, loader_,
            &QObject::deleteLater);
    loader_thread_->start();
}

void MainWindow::on_bf_loaded(const QString& err) {
    if (!err.isEmpty()) {
        // BF open failed — nothing further to wait for; tear the modal down.
        dismiss_load_modal();
        QMessageBox::critical(this, tr("Error"),
                              tr("Failed to open BigFile:\n%1").arg(err));
        status_->showMessage(tr("Error loading file."));
        return;
    }
    bf_ = loader_->take_result();
    const auto n_files = bf_->active_file_count();
    const auto n_dirs = bf_->active_dir_count();
    status_->showMessage(
        tr("Loaded %1  —  %2 directories, %3 files (read-only)")
            .arg(bf_path_)
            .arg(n_dirs)
            .arg(n_files));
    refresh_title();
    // Keep the modal up — the slow part (asset indexing) starts next.
    if (load_modal_) load_modal_->set_message(tr("Indexing sub-entries…"));
    asset_browser_->set_bigfile(bf_, bf_path_);
    zones_tab_->set_bigfile(bf_, bf_path_);
    create_zone_tab_->set_bigfile(bf_, bf_path_);
    edit_zone_deps_tab_->set_bigfile(bf_, bf_path_);
    placement_tab_->set_bigfile(bf_, bf_path_);
    mesh_swap_tab_->set_bigfile(bf_, bf_path_);
    texture_swap_tab_->set_bigfile(bf_, bf_path_);
    add_assets_tab_->set_bigfile(bf_, bf_path_);
    level_blender_tab_->set_bigfile(bf_, bf_path_);
    bake_lightmaps_tab_->set_bigfile(bf_, bf_path_);
    project_panel_->set_base_archive_path(bf_path_);
    project_panel_->set_bigfile(bf_);
    // on_index_finished() closes the modal once the asset index is built.
}

// ── Asset-index progress → load modal ──

void MainWindow::on_index_progress(int current, int total) {
    if (load_modal_) load_modal_->set_progress(current, total);
}

void MainWindow::on_index_finished() {
    dismiss_load_modal();
    // Hand the freshly-built asset index to the tabs that resolve materials
    // across bins (Shape/MAT split), so they don't rebuild it.
    auto idx = asset_browser_->asset_index();
    if (idx) mesh_swap_tab_->set_asset_index(idx);
}

void MainWindow::dismiss_load_modal() {
    if (load_modal_) {
        load_modal_->finish();
        load_modal_->deleteLater();
        load_modal_ = nullptr;
    }
}

// ───────────────────── project lifecycle ───────────────────

void MainWindow::on_new_project() {
    if (!confirm_discard_unsaved()) return;
    // 1) Pick the base archive.
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Pick base archive for the new project"), QString(),
        tr("BigFile (*.bf);;All Files (*)"));
    if (path.isEmpty()) return;
    const jade::gameprofiles::GameProfile* prof =
        jade::gameprofiles::detect(path.toStdString());
    // 2) Confirm/choose game profile.
    const auto& all = jade::gameprofiles::all();
    QStringList codes, labels;
    for (const auto& p : all) {
        codes << QString::fromStdString(p.code);
        labels << QStringLiteral("%1 — %2").arg(
            QString::fromStdString(p.code), QString::fromStdString(p.name));
    }
    int default_idx = 0;
    if (prof) default_idx = codes.indexOf(QString::fromStdString(prof->code));
    bool ok = false;
    const QString choice = QInputDialog::getItem(
        this, tr("Game profile"),
        tr("Detected: %1\nUse profile:")
            .arg(prof ? QString::fromStdString(prof->code) : tr("unknown")),
        labels, qMax(0, default_idx), false, &ok);
    if (!ok) return;
    const QString game_code = codes.at(labels.indexOf(choice));
    // 3) Project name.
    const QString default_name =
        QFileInfo(path).completeBaseName() + tr(" mod");
    const QString name = QInputDialog::getText(
        this, tr("Project name"), tr("Name:"), QLineEdit::Normal,
        default_name, &ok);
    if (!ok || name.isEmpty()) return;
    // 4) Create.
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString err;
    ProjectDoc* proj = ProjectDoc::create(name, game_code, path, this, &err);
    QApplication::restoreOverrideCursor();
    if (!proj) {
        QMessageBox::critical(this, tr("New Project"),
                              tr("Failed to create project:\n%1").arg(err));
        return;
    }
    // 5) Save immediately so the asset store has a path (asset imports
    // need it).
    QString save_dir = QFileDialog::getSaveFileName(
        this, tr("Save new project (a .jmod directory will be created)"),
        QFileInfo(path).dir().filePath(
            QString(name).replace(QLatin1Char(' '), QLatin1Char('_'))
            + QStringLiteral(".jmod")),
        tr("Mod project (*.jmod);;Any (*)"));
    if (save_dir.isEmpty()) return;
    if (!save_dir.endsWith(QStringLiteral(".jmod")))
        save_dir += QStringLiteral(".jmod");
    if (proj->save(save_dir, &err).isEmpty()) {
        QMessageBox::critical(this, tr("New Project"),
                              tr("Failed to create project:\n%1").arg(err));
        return;
    }
    set_project(proj);
    // Also load the base archive (read-only).
    load_bf(path);
    status_->showMessage(tr("Created project: %1").arg(proj->path));
}

void MainWindow::on_open_project() {
    if (!confirm_discard_unsaved()) return;
    const QString path = QFileDialog::getExistingDirectory(
        this, tr("Open project (.jmod directory)"));
    if (path.isEmpty()) return;
    if (!QFileInfo::exists(path + QStringLiteral("/project.json"))) {
        QMessageBox::warning(this, tr("Open Project"),
                             tr("No project.json in '%1'.").arg(path));
        return;
    }
    QString err;
    ProjectDoc* proj = ProjectDoc::load(path, this, &err);
    if (!proj) {
        QMessageBox::critical(this, tr("Open Project"), err);
        return;
    }
    set_project(proj);
    // Ask for the base archive on this machine.
    QMessageBox::information(
        this, tr("Base archive"),
        tr("Project loaded.\nNow pick the base archive on this machine "
           "(%1).")
            .arg(QString::fromStdString(proj->base.archive_name)));
    on_open();
    status_->showMessage(tr("Opened project: %1").arg(proj->path));
}

void MainWindow::on_save_project() {
    if (!project_) return;
    if (project_->path.isEmpty()) {
        on_save_as();
        return;
    }
    QString err;
    if (project_->save(QString(), &err).isEmpty()) {
        QMessageBox::critical(this, tr("Save Project"), err);
        return;
    }
    refresh_title();
    status_->showMessage(tr("Saved: %1").arg(project_->path));
}

void MainWindow::on_save_as() {
    if (!project_) return;
    QString suggest = project_->path;
    if (suggest.isEmpty()) {
        const QString stem =
            (project_->name.isEmpty() ? QStringLiteral("Mod")
                                      : project_->name)
                .replace(QLatin1Char(' '), QLatin1Char('_'));
        suggest = QFileInfo(bf_path_).dir().filePath(
            stem + QStringLiteral(".jmod"));
    }
    QString save_dir = QFileDialog::getSaveFileName(
        this, tr("Save Project As (.jmod directory)"), suggest,
        tr("Mod project (*.jmod);;Any (*)"));
    if (save_dir.isEmpty()) return;
    if (!save_dir.endsWith(QStringLiteral(".jmod")))
        save_dir += QStringLiteral(".jmod");
    QString err;
    if (project_->save(save_dir, &err).isEmpty()) {
        QMessageBox::critical(this, tr("Save Project"), err);
        return;
    }
    refresh_title();
    status_->showMessage(tr("Saved: %1").arg(project_->path));
}

void MainWindow::on_close_project() {
    if (!confirm_discard_unsaved()) return;
    set_project(nullptr);
    status_->showMessage(tr("Project closed."));
}

void MainWindow::set_project(ProjectDoc* proj) {
    // Disconnect previous (if any)
    if (project_) {
        disconnect(project_, &ProjectDoc::changed, this,
                   &MainWindow::refresh_title);
        project_->deleteLater();
    }
    project_ = proj;
    if (proj)
        connect(proj, &ProjectDoc::changed, this,
                &MainWindow::refresh_title);
    refresh_title();
    // Push to panels.
    project_panel_->set_project(proj, bf_path_);
    // Each tab that knows about projects gets it.
    asset_browser_->set_project(proj);
    texture_swap_tab_->set_project(proj);
    add_assets_tab_->set_project(proj);
    mesh_swap_tab_->set_project(proj);
    placement_tab_->set_project(proj);
    create_zone_tab_->set_project(proj);
    edit_zone_deps_tab_->set_project(proj);
    // Switch to the Project tab on open/create so the user sees state.
    if (proj) tabs_->setCurrentWidget(project_panel_);
}

// ── Right-click "Send to X" routing from the Asset Browser ──

void MainWindow::on_send_texture_to_swap(quint32 entry_idx,
                                         quint32 texture_key) {
    // Switch to the Texture Swap tab, pre-selecting the chosen texture.
    tabs_->setCurrentWidget(texture_swap_tab_);
    texture_swap_tab_->receive_asset(entry_idx, texture_key);
    const QString hex = QStringLiteral("%1").arg(texture_key, 8, 16,
                                                 QLatin1Char('0')).toUpper();
    status_->showMessage(tr("Sent texture 0x%1 to Texture Swap.").arg(hex));
}

void MainWindow::on_send_asset_to_tab(const QString& target,
                                      quint32 parent_index, quint32 key,
                                      const QString& category) {
    // Activate the named tab and hand it the chosen asset record.
    QWidget* tab = nullptr;
    if (target == QStringLiteral("texture_swap"))
        tab = texture_swap_tab_;
    else if (target == QStringLiteral("mesh_swap"))
        tab = mesh_swap_tab_;
    else if (target == QStringLiteral("place_objects"))
        tab = placement_tab_;
    if (!tab) return;
    tabs_->setCurrentWidget(tab);
    if (tab == texture_swap_tab_)
        texture_swap_tab_->receive_asset(parent_index, key);
    else if (tab == mesh_swap_tab_)
        mesh_swap_tab_->receive_asset(parent_index, key);
    else if (tab == placement_tab_)
        placement_tab_->receive_asset(parent_index, key);
    const QString hex = QStringLiteral("%1").arg(key, 8, 16,
                                                 QLatin1Char('0')).toUpper();
    status_->showMessage(tr("Sent %1 0x%2 to %3.")
                             .arg(category.toLower(), hex, target));
}
