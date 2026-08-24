// JtmodDialog.hpp — dialog for authoring a .jtmod mod file (port of
// gui/jtmod_dialog.py).
//
// Collects the mod metadata the PoP Mod Manager displays when the mod is
// loaded — Name, Author, Version, Description — plus the output path. The
// game and base archive are shown read-only: a .jtmod is pinned to the
// game + exact stock archive it was built against, and the manager refuses
// to apply it to a different game (e.g. a SoT mod onto a T2T pop3.bf).
#pragma once

#include <QDialog>

class ProjectDoc;
class QLabel;
class QLineEdit;
class QPlainTextEdit;

// Modal dialog returning the .jtmod metadata + output path.
class JtmodDialog : public QDialog {
    Q_OBJECT
public:
    JtmodDialog(ProjectDoc* project, const QString& base_archive_path,
                QWidget* parent = nullptr);

    struct Values {
        QString name, author, version, description, image_path, out_path;
    };
    // (name, author, version, description, image_path, out_path).
    Values values() const;

private slots:
    void on_browse();
    void on_browse_image();
    void on_accept();

private:
    void set_image(const QString& path);

    ProjectDoc* project_;
    QLineEdit* name_edit_ = nullptr;
    QLineEdit* author_edit_ = nullptr;
    QLineEdit* version_edit_ = nullptr;
    QPlainTextEdit* desc_edit_ = nullptr;
    QLineEdit* image_edit_ = nullptr;
    QLabel* image_preview_ = nullptr;
    QLineEdit* out_edit_ = nullptr;
};
