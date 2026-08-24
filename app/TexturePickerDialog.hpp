// TexturePickerDialog.hpp — texture picker (port of
// gui/texture_picker_dialog.py): modal dialog that returns a chosen
// texture key.
//
// Used by the "Retarget material to another texture..." flow in the Mesh
// Swap tab. Lists every distinct texture sub-entry visible from a starting
// BF entry (and optionally the whole archive), with a key + dimensions +
// format summary per row plus a thumbnail preview on the right.
//
// Scope is deliberately small: no editing, no multi-select. Caller gets
// the key back via selected_key() (-1 if cancelled).
#pragma once

#include <QDialog>
#include <QImage>
#include <cstdint>
#include <vector>

class ProjectDoc;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;

// Pick a texture key from a BF.
//
// By default scans only the currently-selected entry (fast — the wow file
// the user is already editing). Tick "Search all BF entries" to widen the
// search to the entire archive (slower but lets you reuse a texture from
// elsewhere — e.g. a costume texture on a weapon).
class TexturePickerDialog : public QDialog {
    Q_OBJECT
public:
    // current_texture_key / entry_key: -1 == None. project may be null.
    TexturePickerDialog(const QString& bf_path, quint32 current_entry_idx,
                        long long current_texture_key = -1,
                        ProjectDoc* project = nullptr,
                        long long entry_key = -1,
                        QWidget* parent = nullptr);

    long long selected_key() const { return selected_; }

private slots:
    void reload();
    void on_selection();
    void on_dbl_click(QListWidgetItem* item);
    void apply_filter();

private:
    // One loaded (key, info) row. For textures minted by pending
    // add_texture ops the pixels come from the imported source image
    // (rgba) instead of a BF payload.
    struct Row {
        quint32 key = 0;
        QString width = QStringLiteral("?");   // "?" when unknown
        QString height = QStringLiteral("?");
        int format = 7;
        std::vector<uint8_t> payload;  // texture sub-entry bytes (BF rows)
        QImage rgba;                   // source pixels (project-added rows)
        QString added_by;              // op id when minted by add_texture
    };

    void populate_visible();
    static QString format_row(const Row& row);
    QImage decode_row(const Row& row) const;
    void project_added_rows(std::vector<Row>& rows) const;

    QString bf_path_;
    quint32 entry_idx_;
    long long current_key_;
    // Optional: the open project + the entry's FAT key, so textures minted
    // by pending add_texture ops (Add New Assets tab) appear in the list
    // and a material can be retargeted at them pre-Build.
    ProjectDoc* project_;
    long long entry_key_;
    long long selected_ = -1;

    std::vector<Row> all_rows_;  // populated by reload()

    QLineEdit* filter_ = nullptr;
    QCheckBox* scope_ = nullptr;
    QListWidget* list_ = nullptr;
    QLabel* preview_ = nullptr;
    QLabel* info_ = nullptr;
    QPushButton* ok_btn_ = nullptr;
};
