// BakeLightmapsTab.hpp — Bake Lightmaps tab (port of gui/bake_lightmaps_tab.py):
// export a zone, bake a per-pixel lightmap atlas in Blender, reimport the 2nd
// UV + atlas into the BF. SEPARATE from the vertex-colour RLI bake ("Bake
// lights" tab) — this writes the lightmap uv1 (RLI extra block +4) and a shared
// lightmap texture, leaving the RLI vertex colours untouched. See
// io_ops/lightmap_bake.py and docs/jade_lightmap_baking.md.
//
// PORT GAP: both background workers of the Python tab depend on Python-only
// code, so the worker classes (_Signals/_ExportWorker/_ReimportWorker) and the
// _run/_finish thread plumbing are not ported — the two action buttons keep
// their full dialog flow and then surface a "not ported yet" message box where
// the worker would launch:
//   * _ExportWorker: io_ops/level_blender.export_level_to_glb(...,
//     lightmap_atlas=..., lightmap_mode='blender', log_fn=...) —
//     jade::levelblend::export_level_to_glb (jade/LevelBlender.hpp) exists but
//     explicitly excludes the lightmap_atlas modes (no TEXCOORD_1 =
//     cooked-vertex-index passthrough, no 'lmplan' in the manifest).
//   * _ReimportWorker: io_ops/lightmap_bake.apply_lightmap_to_bf — no native
//     port of io_ops/lightmap_bake.py exists.
#pragma once

#include <QWidget>
#include <memory>

namespace jade { class BigFile; }
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTextEdit;

class BakeLightmapsTab : public QWidget {
    Q_OBJECT
public:
    explicit BakeLightmapsTab(QWidget* parent = nullptr);

    // ── wiring ──
    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& path);

private slots:
    // ── actions ──
    void on_export();
    void pick_uv();
    void pick_png();
    void on_reimport();

private:
    void set_enabled(bool on);
    long long current_bin() const;      // None → -1
    QString district_prefix() const;    // null QString = None

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    QString uv_json_;    // empty = None
    QString atlas_png_;  // empty = None

    QComboBox* zone_combo_ = nullptr;
    QPushButton* export_btn_ = nullptr;
    QPushButton* uv_btn_ = nullptr;
    QLabel* uv_lbl_ = nullptr;
    QPushButton* png_btn_ = nullptr;
    QLabel* png_lbl_ = nullptr;
    QComboBox* size_combo_ = nullptr;
    QCheckBox* neutralize_chk_ = nullptr;
    QPushButton* apply_btn_ = nullptr;
    QTextEdit* log_ = nullptr;
};
