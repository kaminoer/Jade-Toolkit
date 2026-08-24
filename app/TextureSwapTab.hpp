// TextureSwapTab.hpp — Texture Swap tab (port of gui/texture_swap_tab.py):
// browse and preview textures, record a replacement op.
//
// Layout:
//   • Top:    BF entry picker (filtered to *wow* file names).
//   • Left:   list of texture sub-entries with key, dims, format.
//   • Middle: preview of the texture currently in the BF.
//   • Right:  preview of the candidate replacement image, with Add to Project.
//
// Clicking "Add to Project" copies the image into the project's AssetStore and
// appends a ``replace_texture`` operation to the open ModProject. The build runs
// from the Project panel; the shared image core performs source decoding,
// power-of-two snapping, Lanczos resizing, format override, and mip handling.
#pragma once

#include <QWidget>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "jade/Texture.hpp"

namespace jade { class BigFile; }
class ProjectDoc;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QTextEdit;
class QTreeWidget;

// ── free helpers (shared with the Asset Browser's texture export) ──────────

// save_texture_png — decode a parsed texture and write it as a PNG.
//
// Honors the +40/+44 "actual" dimensions when they differ from the
// +8/+10 logical header dims (third-party upscales bump only the
// former) — the same rule the Texture Swap preview/extract uses.
// Returns the ``(width, height)`` actually written.
//
// `payload` is the texture sub-entry bytes `tex_info` was parsed from (the
// Python tex_info dict carried them as 'full_data'; jade::TexInfo does not).
// Throws std::runtime_error on failure — the Python ValueError.
std::pair<quint32, quint32> save_texture_png(
    const std::vector<uint8_t>& payload, jade::TexInfo tex_info,
    const uint8_t* palette, size_t pal_len, const QString& out_path,
    quint32 actual_w = 0, quint32 actual_h = 0);

// extract_texture_to_png — extract texture ``tex_key`` from BF entry
// ``entry_idx`` to a PNG.
//
// Single source of truth for texture→PNG extraction, shared by the
// Texture Swap tab's "Extract to PNG…" and the Asset Browser's export.
// Pass an open ``bf`` to reuse it instead of opening ``bf_path``.
// Returns the ``(width, height)`` written. Throws std::runtime_error on
// failure — the Python ValueError.
std::pair<quint32, quint32> extract_texture_to_png(
    const QString& bf_path, quint32 entry_idx, quint32 tex_key,
    const QString& out_path, jade::BigFile* bf = nullptr);

// ── the tab ────────────────────────────────────────────────────────────────

// Browse / preview / replace textures inside a BF entry.
class TextureSwapTab : public QWidget {
    Q_OBJECT
public:
    // One (key, info) row from _list_textures: unique texture in a BF entry.
    struct TexRow {
        quint32 key = 0;
        // The +8/+10 logical dims, NOT parse_texture's resolved
        // width/height (which is the +40/+44 *actual* dim when an
        // upscale mod bumps it). Sourcing 'logical' from t['width']
        // made both columns read identical when log != actual.
        quint32 logical_w = 0, logical_h = 0;
        quint32 actual_w = 0, actual_h = 0;
        quint32 format = 0;
        size_t payload_bytes = 0;
        std::vector<uint8_t> payload;   // sub-entry bytes ('tex_info'/'full_data')
        jade::TexInfo tex_info;
        std::vector<uint8_t> palette;   // 1024-byte PAL8 palette; empty == None
    };

    explicit TextureSwapTab(QWidget* parent = nullptr);
    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& bf_path);
    // Bind the tab to the current ModProject (or nullptr).
    void set_project(ProjectDoc* project);
    void receive_asset(quint32 parent_index, quint32 sub_key);

private slots:
    void on_list();
    void on_tex_selected();
    void on_load_image();
    void on_apply();
    void on_extract();
    void on_log(const QString& msg);

private:
    void populate_entry_combo();
    long long current_entry_idx() const;  // -1 == Python None
    void update_enabled();
    std::vector<quint32> find_entries_with_texture_key(quint32 sub_key) const;

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    std::vector<TexRow> tex_rows_;
    long long current_key_ = -1;  // -1 == Python None
    QString new_image_path_;
    ProjectDoc* project_ = nullptr;

    QLabel* bf_label_ = nullptr;
    QComboBox* entry_combo_ = nullptr;
    QPushButton* list_btn_ = nullptr;
    QTreeWidget* tex_tree_ = nullptr;
    QLabel* cur_info_ = nullptr;
    QScrollArea* cur_scroll_ = nullptr;
    QLabel* cur_label_ = nullptr;
    QPushButton* extract_btn_ = nullptr;
    QLineEdit* new_path_edit_ = nullptr;
    QComboBox* fmt_combo_ = nullptr;
    QLabel* new_info_ = nullptr;
    QScrollArea* new_scroll_ = nullptr;
    QLabel* new_label_ = nullptr;
    QLabel* project_hint_ = nullptr;
    QPushButton* apply_btn_ = nullptr;
    QTextEdit* log_ = nullptr;
};
