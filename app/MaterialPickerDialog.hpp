// MaterialPickerDialog.hpp — material picker (port of
// gui/material_picker_dialog.py): modal dialog that returns a chosen
// (sub-)material key.
//
// Used by the "Use different material for matId N…" flow in the Mesh Swap
// tab. Lists every retargetable sub-material in a BF entry (gro_type 5
// records with a known texture-key offset) plus any materials minted by
// pending add_material ops on the same entry, with a preview of the
// texture each material points at.
//
// Caller gets the material key via selected_key() (-1 if cancelled).
#pragma once

#include <QDialog>
#include <QImage>
#include <cstdint>
#include <vector>

class ProjectDoc;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// Pick a (sub-)material key from a BF entry.
//
// Each row is a material; the preview pane shows the texture it points
// at. Materials minted by pending add_material ops are listed too, tagged
// "[new]" — picking one wires the slot to a material that will exist
// after Build.
class MaterialPickerDialog : public QDialog {
    Q_OBJECT
public:
    // current_material_key / entry_key: -1 == None. project may be null.
    MaterialPickerDialog(const QString& bf_path, quint32 entry_idx,
                         long long current_material_key = -1,
                         ProjectDoc* project = nullptr,
                         long long entry_key = -1,
                         QWidget* parent = nullptr);

    long long selected_key() const { return selected_; }

private slots:
    void populate_visible();
    void on_selection();
    void on_dbl_click(QListWidgetItem* item);

private:
    // (key, info) — kind (-1 == None), tex_key (0 == none), rgba, added_by.
    struct Row {
        quint32 key = 0;
        long long kind = -1;
        quint32 tex_key = 0;
        QImage rgba;
        QString added_by;
    };

    void reload();
    static QString format_row(const Row& row);

    QString bf_path_;
    quint32 entry_idx_;
    long long current_key_;
    ProjectDoc* project_;
    long long entry_key_;
    long long selected_ = -1;
    std::vector<Row> rows_;

    QLineEdit* filter_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* info_ = nullptr;
    QPushButton* ok_btn_ = nullptr;
};
