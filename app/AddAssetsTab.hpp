// AddAssetsTab.hpp — Add New Assets tab (port of gui/add_assets_tab.py):
// mint brand-new textures (and materials) into a bin.
//
// Unlike Texture Swap (which replaces pixels under an existing key), this
// tab inserts completely new sub-entries under fresh 0x7A-range keys, using
// the in-game-proven key-sorted in-bin insertion (core/asset_add).
//
// Layout:
//   • Top:    BF entry picker (filtered to *wow* file names) — the bin that
//             will host the new asset. A texture is only visible to
//             materials in the SAME bin (each bin streams self-contained).
//   • Left:   what the bin already ships: textures and sub-materials.
//   • Right:  "New texture" (image + format → add_texture op) and
//             "New material" (donor clone + texture pick → add_material op).
//
// Both record operations into the open Mod Project; the .bf is written at
// Build time from the Project panel. After adding a texture here, point a
// material at it via Mesh Swap → right-click → "Retarget material…" (the
// picker lists project-added textures) or create a new material below.
#pragma once

#include <QHash>
#include <QSize>
#include <QString>
#include <QWidget>
#include <memory>
#include <unordered_set>
#include <vector>

#include "jade/SubEntry.hpp"

namespace jade { class BigFile; }
class ProjectDoc;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTextEdit;
class QTreeWidget;

// Insert brand-new textures / sub-materials into a shipped bin.
class AddAssetsTab : public QWidget {
    Q_OBJECT
public:
    explicit AddAssetsTab(QWidget* parent = nullptr);

    // -- public --

    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& path);
    void set_project(ProjectDoc* proj);
    // Kept for MainWindow's send-to-tab routing surface; the Python tab
    // defines no receive_asset, so this is a no-op.
    void receive_asset(quint32 parent_index, quint32 key);

private slots:
    void on_list();
    void on_load_image();
    void on_add_texture();
    void on_add_material();

private:
    // One row of the sub-material table: (key, kind, tex_key).
    struct SubmatRow {
        quint32 key = 0;
        quint32 kind = 0;
        quint32 tex_key = 0;  // 0 == Python None (no resolvable texture)
    };
    // One "[(key, label, op_id)]" tuple from _project_added_textures().
    struct AddedTexture {
        quint32 key = 0;
        QString label;
        QString op_id;
    };

    // -- helpers --

    void populate_entry_combo();
    long long current_entry_idx() const;  // -1 == Python None
    long long current_entry_key() const;  // -1 == Python None
    void update_enabled();
    // Keys we must not collide with: every sub-entry key in the listed
    // entry plus every key minted by ops already in the project.
    std::unordered_set<quint32> used_keys() const;
    // [(key, label, op_id)] minted by add_texture ops for this entry.
    std::vector<AddedTexture> project_added_textures(quint32 entry_key) const;
    QString submat_tooltip(quint32 key, quint32 kind, quint32 tex_key) const;
    QString added_texture_tooltip(quint32 key, const QString& op_id) const;
    void refresh_mat_combos();

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    ProjectDoc* project_ = nullptr;
    std::vector<jade::SubEntry> subs_;    // walk of the listed entry
    std::vector<quint32> tex_keys_;       // full-texture keys in the entry
    std::vector<SubmatRow> submat_rows_;  // (key, kind, tex_key)
    QHash<quint32, QString> tex_tooltips_;  // tex key -> rich tooltip html
    QString new_image_path_;
    QSize new_image_size_;                // invalid == Python None

    QLabel* bf_label_ = nullptr;
    QComboBox* entry_combo_ = nullptr;
    QPushButton* list_btn_ = nullptr;
    QTreeWidget* tex_tree_ = nullptr;
    QTreeWidget* mat_tree_ = nullptr;
    QLineEdit* new_path_edit_ = nullptr;
    QComboBox* fmt_combo_ = nullptr;
    QLabel* new_info_ = nullptr;
    QScrollArea* new_scroll_ = nullptr;
    QLabel* new_label_ = nullptr;
    QPushButton* add_tex_btn_ = nullptr;
    QComboBox* donor_combo_ = nullptr;
    QComboBox* mat_tex_combo_ = nullptr;
    QPushButton* add_mat_btn_ = nullptr;
    QLabel* project_hint_ = nullptr;
    QTextEdit* log_ = nullptr;
};
