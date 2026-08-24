// ExePatchDialog.hpp — dialog for authoring a standalone C++ byte-patcher
// exe (port of gui/exe_patch_dialog.py).
//
// Collects the mod metadata baked into the patcher's GUI — Title, Mod
// description, Author, Version — plus the target filenames the patcher
// will auto-detect and the output .exe path. The patch payload itself is
// the byte diff produced by the build's make_exe_patch.
#pragma once

#include <QDialog>
#include <QStringList>

class ProjectDoc;
class QLineEdit;
class QPlainTextEdit;

// Modal dialog returning the patcher metadata + output path.
class ExePatchDialog : public QDialog {
    Q_OBJECT
public:
    ExePatchDialog(ProjectDoc* project, const QString& base_archive_path,
                   QWidget* parent = nullptr);

    struct Values {
        QString title, description, author, version;
        QStringList accepted_names;
        QString icon_path, image_path, out_exe_path;
    };
    // (title, description, author, version, accepted_names, icon_path,
    // image_path, out_exe_path) after Accept.
    Values values() const;

private slots:
    void on_browse();
    void on_browse_icon();
    void on_browse_image();
    void on_accept();

private:
    ProjectDoc* project_;
    QLineEdit* title_edit_ = nullptr;
    QPlainTextEdit* desc_edit_ = nullptr;
    QLineEdit* author_edit_ = nullptr;
    QLineEdit* version_edit_ = nullptr;
    QLineEdit* targets_edit_ = nullptr;
    QLineEdit* icon_edit_ = nullptr;
    QLineEdit* image_edit_ = nullptr;
    QLineEdit* out_edit_ = nullptr;
};
