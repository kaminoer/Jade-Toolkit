// LevelBlenderTab.hpp — Level <-> Blender tab (port of
// gui/level_blender_tab.py): export a zone for Cycles light-baking and
// reimport the baked per-vertex RLI back into the BF. See
// io_ops/level_blender.py and docs/jade_RLI_baked_lighting.md.
#pragma once

#include <QThread>
#include <QWidget>
#include <array>
#include <memory>

#include "jade/RliRescale.hpp"

namespace jade { class BigFile; }
class ProjectDoc;
class QCheckBox;
class QComboBox;
class QLabel;
class QPainter;
class QPushButton;
class QSlider;
class QTextEdit;

// Background worker base: one job per thread, Python's _Signals surface.
class LevelBlenderWorker : public QThread {
    Q_OBJECT
public:
    using QThread::QThread;
    jade::PaletteStats take_stats() { return std::move(stats_); }

signals:
    void log(const QString& line);
    void done(const QString& msg);
    void fail(const QString& msg);
    void result();  // sample stats ready (pull via take_stats)

protected:
    jade::PaletteStats stats_;
};

// Paints the zone's baked-lighting palette now vs after the grade: a big
// average swatch + a dark→bright percentile strip for each
// (_PalettePreview).
class PalettePreview : public QWidget {
    Q_OBJECT
public:
    explicit PalettePreview(QWidget* parent = nullptr);
    void set_data(const jade::PaletteStats& stats, jade::RliColorFn fn);

protected:
    void paintEvent(QPaintEvent* ev) override;

private:
    void draw_row(QPainter& p, int y, int rowH, const QString& title,
                  const jade::PaletteStats& st);

    jade::PaletteStats stats_;
    jade::RliColorFn fn_;
};

class LevelBlenderTab : public QWidget {
    Q_OBJECT
public:
    explicit LevelBlenderTab(QWidget* parent = nullptr);
    void set_bigfile(std::shared_ptr<jade::BigFile> bf, const QString& path);
    void set_project(ProjectDoc* proj);
    void receive_asset(quint32 parent_index, quint32 key);

private slots:
    void on_export();
    void on_reimport();
    void refresh_preview();
    void pick_tint();
    void reset_grade();
    void on_zone_changed();
    void on_sample();
    void on_rescale();
    void on_done(const QString& msg);
    void on_fail(const QString& msg);
    void on_sample_result();

private:
    void set_enabled(bool on);
    long long current_bin() const;   // -1 == None
    QString district_prefix() const; // empty == None
    jade::RliColorFn current_transform() const;
    void update_tint_swatch();
    void run(LevelBlenderWorker* worker);
    void finish();

    std::shared_ptr<jade::BigFile> bf_;
    QString bf_path_;
    ProjectDoc* project_ = nullptr;
    LevelBlenderWorker* worker_ = nullptr;
    jade::PaletteStats stats_;
    std::array<int, 3> tint_{255, 255, 255};

    QComboBox* zone_combo_ = nullptr;
    QCheckBox* lights_chk_ = nullptr;
    QCheckBox* srgb_chk_ = nullptr;
    QPushButton* export_btn_ = nullptr;
    QCheckBox* primary_chk_ = nullptr;
    QPushButton* reimport_btn_ = nullptr;
    PalettePreview* preview_ = nullptr;
    QSlider* bright_ = nullptr;
    QLabel* bright_lbl_ = nullptr;
    QSlider* contrast_ = nullptr;
    QLabel* contrast_lbl_ = nullptr;
    QCheckBox* lights_grade_chk_ = nullptr;
    QCheckBox* tex_grade_chk_ = nullptr;
    QPushButton* tint_btn_ = nullptr;
    QLabel* tint_swatch_ = nullptr;
    QPushButton* reset_btn_ = nullptr;
    QPushButton* sample_btn_ = nullptr;
    QPushButton* apply_btn_ = nullptr;
    QTextEdit* log_ = nullptr;
};
