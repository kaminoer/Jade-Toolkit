#include "JtmodDialog.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "jade/GameProfiles.hpp"

#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"
#include "Theme.hpp"

namespace {

// build.jtmod_export.THUMB_SIZE — the card thumbnail side.
constexpr int THUMB_SIZE = 128;

QString safe_stem(const QString& name) {
    QString stem;
    for (const QChar c : name)
        stem += (c.isLetterOrNumber() || c == QLatin1Char('-')
                 || c == QLatin1Char('_') || c == QLatin1Char(' '))
                    ? c
                    : QLatin1Char('_');
    stem = stem.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
    return stem.isEmpty() ? QStringLiteral("mod") : stem;
}

}  // namespace

JtmodDialog::JtmodDialog(ProjectDoc* project,
                         const QString& base_archive_path, QWidget* parent)
    : QDialog(parent), project_(project) {
    setWindowTitle(tr("Create .jtmod mod file"));
    setMinimumWidth(560);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Export this project as a composable mod for the PoP Mod "
           "Manager. The details below are shown when the mod is loaded."));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* form = new QFormLayout();

    // Game (read-only) — pinned into the .jtmod for the manager's
    // cross-game refusal check.
    QString game_name;
    if (const jade::gameprofiles::GameProfile* prof =
            jade::gameprofiles::get(project->base.game))
        game_name = qs(prof->name);
    else
        game_name = project->base.game.empty() ? tr("(unknown)")
                                               : qs(project->base.game);
    auto* game_lbl = new QLabel(QStringLiteral("%1  ·  %2").arg(
        game_name, qs(project->base.archive_name)));
    game_lbl->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    form->addRow(tr("Game:"), game_lbl);

    name_edit_ = new QLineEdit(project->name);
    name_edit_->setPlaceholderText(tr("e.g. Dark Prince Reskin"));
    form->addRow(tr("Name:"), name_edit_);

    author_edit_ = new QLineEdit(project->author);
    author_edit_->setPlaceholderText(tr("Your name / handle"));
    form->addRow(tr("Author:"), author_edit_);

    version_edit_ = new QLineEdit(QStringLiteral("1.0"));
    form->addRow(tr("Version:"), version_edit_);

    desc_edit_ = new QPlainTextEdit(project->description);
    desc_edit_->setPlaceholderText(tr("What the mod does…"));
    desc_edit_->setMinimumHeight(100);
    form->addRow(tr("Description:"), desc_edit_);

    // Card image: optional, centre-cropped to a square + resized to a
    // fixed size so every mod card in the manager looks consistent. Live
    // preview.
    image_edit_ = new QLineEdit(QString());
    image_edit_->setPlaceholderText(
        tr("(optional) .png / .jpg shown on the card"));
    image_preview_ = new QLabel();
    image_preview_->setFixedSize(64, 64);
    image_preview_->setStyleSheet(
        QStringLiteral(
            "border: 1px solid %1; border-radius: 0; color: %2;")
            .arg(theme::BORDER, theme::DIM_TEXT));
    image_preview_->setAlignment(Qt::AlignCenter);
    image_preview_->setText(QStringLiteral("64²"));
    auto* img_browse = new QPushButton(tr("Browse…"));
    connect(img_browse, &QPushButton::clicked, this,
            &JtmodDialog::on_browse_image);
    auto* img_clear = new QPushButton(tr("Clear"));
    connect(img_clear, &QPushButton::clicked, this,
            [this] { set_image(QString()); });
    auto* img_col = new QVBoxLayout();
    auto* img_row = new QHBoxLayout();
    img_row->addWidget(image_edit_);
    img_row->addWidget(img_browse);
    img_row->addWidget(img_clear);
    img_col->addLayout(img_row);
    auto* img_hint = new QLabel(
        tr("Optional icon shown on the mod's card. Any image works — it's "
           "centre-cropped to a square and resized to %1×%2, so all cards "
           "stay the same size.")
            .arg(THUMB_SIZE)
            .arg(THUMB_SIZE));
    img_hint->setWordWrap(true);
    img_hint->setStyleSheet(
        QStringLiteral("color: %1; font-size: 11px;").arg(theme::DIM_TEXT));
    img_col->addWidget(img_hint);
    auto* img_widget = new QWidget();
    auto* img_outer = new QHBoxLayout(img_widget);
    img_outer->setContentsMargins(0, 0, 0, 0);
    img_outer->addWidget(image_preview_);
    img_outer->addLayout(img_col);
    form->addRow(tr("Card image:"), img_widget);

    // Output .jtmod path + browse.
    QString default_dir;
    if (!project->path.isEmpty())
        default_dir = QFileInfo(project->path).path();
    else if (!base_archive_path.isEmpty())
        default_dir = QFileInfo(base_archive_path).path();
    if (default_dir.isEmpty()) default_dir = QDir::currentPath();
    const QString default_out = QDir(default_dir).filePath(
        safe_stem(project->name) + QStringLiteral(".jtmod"));
    auto* path_row = new QHBoxLayout();
    out_edit_ = new QLineEdit(default_out);
    path_row->addWidget(out_edit_);
    auto* browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, &JtmodDialog::on_browse);
    path_row->addWidget(browse);
    form->addRow(tr("Output .jtmod:"), path_row);

    root->addLayout(form);

    auto* note = new QLabel(
        tr("The mod is tied to this game's stock archive. The Mod Manager "
           "verifies the archive and stacks non-conflicting mods; it flags "
           "mods that change the same thing so you can pick one."));
    note->setWordWrap(true);
    note->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    root->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                         | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create .jtmod…"));
    connect(buttons, &QDialogButtonBox::accepted, this,
            &JtmodDialog::on_accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void JtmodDialog::on_browse() {
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save .jtmod mod file"), out_edit_->text(),
        tr("PoP mod (*.jtmod);;All files (*.*)"));
    if (!path.isEmpty()) {
        if (!path.toLower().endsWith(QStringLiteral(".jtmod")))
            path += QStringLiteral(".jtmod");
        out_edit_->setText(path);
    }
}

void JtmodDialog::on_browse_image() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose card image"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.gif);;All files (*)"));
    if (!path.isEmpty()) set_image(path);
}

void JtmodDialog::set_image(const QString& path) {
    image_edit_->setText(path);
    QPixmap pm = path.isEmpty() ? QPixmap() : QPixmap(path);
    if (!pm.isNull()) {
        // Mirror the export's centre-crop-to-square so the preview matches
        // what the card will show.
        const int side = qMin(pm.width(), pm.height());
        pm = pm.copy((pm.width() - side) / 2, (pm.height() - side) / 2,
                     side, side)
                 .scaled(64, 64, Qt::KeepAspectRatio,
                         Qt::SmoothTransformation);
        image_preview_->setPixmap(pm);
    } else {
        image_preview_->setPixmap(QPixmap());
        image_preview_->setText(QStringLiteral("64²"));
    }
}

void JtmodDialog::on_accept() {
    if (name_edit_->text().trimmed().isEmpty())
        name_edit_->setText(project_->name.isEmpty() ? tr("Untitled mod")
                                                     : project_->name);
    if (out_edit_->text().trimmed().isEmpty()) return;
    accept();
}

JtmodDialog::Values JtmodDialog::values() const {
    Values v;
    v.out_path = out_edit_->text().trimmed();
    if (!v.out_path.isEmpty()
        && !v.out_path.toLower().endsWith(QStringLiteral(".jtmod")))
        v.out_path += QStringLiteral(".jtmod");
    v.name = name_edit_->text().trimmed();
    v.author = author_edit_->text().trimmed();
    v.version = version_edit_->text().trimmed();
    v.description = desc_edit_->toPlainText().trimmed();
    v.image_path = image_edit_->text().trimmed();
    return v;
}
