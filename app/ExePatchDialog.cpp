#include "ExePatchDialog.hpp"

#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "jade/GameProfiles.hpp"

#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"
#include "Theme.hpp"

namespace {

QString safe_stem(const QString& name) {
    QString stem;
    for (const QChar c : name)
        stem += (c.isLetterOrNumber() || c == QLatin1Char('-')
                 || c == QLatin1Char('_') || c == QLatin1Char(' '))
                    ? c
                    : QLatin1Char('_');
    stem = stem.trimmed().replace(QLatin1Char(' '), QLatin1Char('_'));
    return stem.isEmpty() ? QStringLiteral("patch") : stem;
}

// build.exe_patch._EXTRA_TARGET_NAMES.
QStringList extra_target_names(const QString& game) {
    if (game == QLatin1String("SoT") || game == QLatin1String("WW")
        || game == QLatin1String("WW_PS2"))
        return {QStringLiteral("prince.bf")};
    if (game == QLatin1String("T2T")) return {QStringLiteral("pop3.bf")};
    return {};
}

// build.exe_patch.accepted_target_names: target filenames the patcher
// should auto-detect, de-duplicated (case-insensitive, order kept).
QStringList accepted_target_names(const ProjectDoc* project,
                                  const QString& base_archive_path) {
    QStringList names;
    QString loaded = qs(project->base.archive_name);
    if (loaded.isEmpty())
        loaded = QFileInfo(base_archive_path).fileName();
    loaded = loaded.trimmed();
    if (!loaded.isEmpty()) names << loaded;
    if (const jade::gameprofiles::GameProfile* prof =
            jade::gameprofiles::get(project->base.game))
        for (const std::string& n : prof->archive_filenames) names << qs(n);
    names << extra_target_names(qs(project->base.game));
    QStringList out;
    QSet<QString> seen;
    for (const QString& n : names) {
        const QString nl = n.toLower();
        if (!n.isEmpty() && !seen.contains(nl)) {
            seen.insert(nl);
            out << n;
        }
    }
    return out;
}

}  // namespace

ExePatchDialog::ExePatchDialog(ProjectDoc* project,
                               const QString& base_archive_path,
                               QWidget* parent)
    : QDialog(parent), project_(project) {
    setWindowTitle(tr("Make standalone EXE patch"));
    setMinimumWidth(560);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Build a standalone patcher. The fields below are shown in the "
           "patcher's window."));
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto* form = new QFormLayout();

    title_edit_ = new QLineEdit(project->name);
    title_edit_->setPlaceholderText(tr("e.g. Dark Prince Reskin"));
    form->addRow(tr("Title:"), title_edit_);

    desc_edit_ = new QPlainTextEdit(project->description);
    desc_edit_->setPlaceholderText(tr("What the mod does…"));
    desc_edit_->setMinimumHeight(100);
    form->addRow(tr("Mod description:"), desc_edit_);

    author_edit_ = new QLineEdit(project->author);
    author_edit_->setPlaceholderText(tr("Your name / handle"));
    form->addRow(tr("Author:"), author_edit_);

    version_edit_ = new QLineEdit(QStringLiteral("1.0"));
    form->addRow(tr("Version:"), version_edit_);

    // Target filenames the patcher will look for in its own folder.
    QStringList default_targets =
        accepted_target_names(project, base_archive_path);
    if (default_targets.isEmpty() && !project->base.archive_name.empty())
        default_targets << qs(project->base.archive_name);
    targets_edit_ = new QLineEdit(default_targets.join(QStringLiteral(", ")));
    targets_edit_->setToolTip(
        tr("Comma-separated filenames the patcher auto-detects next to "
           "itself. The patch verifies by SHA-256, so the exact filename "
           "isn't forced and any file with matching contents works."));
    form->addRow(tr("Target file name(s):"), targets_edit_);

    // Optional patcher exe icon.
    icon_edit_ = new QLineEdit(QString());
    icon_edit_->setPlaceholderText(tr("(optional) .ico / .png / .jpg"));
    auto* icon_row = new QHBoxLayout();
    icon_row->addWidget(icon_edit_);
    auto* icon_btn = new QPushButton(tr("Browse…"));
    connect(icon_btn, &QPushButton::clicked, this,
            &ExePatchDialog::on_browse_icon);
    icon_row->addWidget(icon_btn);
    form->addRow(tr("Patcher icon:"), icon_row);

    // Optional splash image shown under the description.
    image_edit_ = new QLineEdit(QString());
    image_edit_->setPlaceholderText(
        tr("(optional) image shown under the description"));
    auto* image_row = new QHBoxLayout();
    image_row->addWidget(image_edit_);
    auto* image_btn = new QPushButton(tr("Browse…"));
    connect(image_btn, &QPushButton::clicked, this,
            &ExePatchDialog::on_browse_image);
    image_row->addWidget(image_btn);
    form->addRow(tr("Splash image:"), image_row);

    // Output exe path + browse.
    QString default_dir = project->path;
    if (default_dir.isEmpty() && !base_archive_path.isEmpty())
        default_dir = QFileInfo(base_archive_path).path();
    if (default_dir.isEmpty()) default_dir = QDir::currentPath();
    const QString default_exe = QDir(default_dir).filePath(
        safe_stem(project->name) + QStringLiteral("_patcher.exe"));
    auto* path_row = new QHBoxLayout();
    out_edit_ = new QLineEdit(default_exe);
    path_row->addWidget(out_edit_);
    auto* browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, &ExePatchDialog::on_browse);
    path_row->addWidget(browse);
    form->addRow(tr("Output .exe:"), path_row);

    root->addLayout(form);

    auto* note = new QLabel(
        tr("The patcher auto-detects the target in its own folder, can make "
           "a .bak backup, can skip verification, and can restore the "
           "original (from backup or by un-patching)."));
    note->setWordWrap(true);
    note->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    root->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok
                                         | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Build patcher…"));
    connect(buttons, &QDialogButtonBox::accepted, this,
            &ExePatchDialog::on_accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(buttons);
}

void ExePatchDialog::on_browse() {
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save patcher exe"), out_edit_->text(),
        tr("Executable (*.exe)"));
    if (!path.isEmpty()) {
        if (!path.toLower().endsWith(QStringLiteral(".exe")))
            path += QStringLiteral(".exe");
        out_edit_->setText(path);
    }
}

void ExePatchDialog::on_browse_icon() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose patcher icon"), QString(),
        tr("Images (*.ico *.png *.jpg *.jpeg *.bmp);;All files (*)"));
    if (!path.isEmpty()) icon_edit_->setText(path);
}

void ExePatchDialog::on_browse_image() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose splash image"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.gif);;All files (*)"));
    if (!path.isEmpty()) image_edit_->setText(path);
}

void ExePatchDialog::on_accept() {
    if (title_edit_->text().trimmed().isEmpty())
        title_edit_->setText(project_->name.isEmpty()
                                 ? tr("BigFile Patch")
                                 : project_->name);
    if (out_edit_->text().trimmed().isEmpty()) return;
    accept();
}

ExePatchDialog::Values ExePatchDialog::values() const {
    Values v;
    v.out_exe_path = out_edit_->text().trimmed();
    if (!v.out_exe_path.isEmpty()
        && !v.out_exe_path.toLower().endsWith(QStringLiteral(".exe")))
        v.out_exe_path += QStringLiteral(".exe");
    for (const QString& n :
         targets_edit_->text().split(QLatin1Char(',')))
        if (!n.trimmed().isEmpty()) v.accepted_names << n.trimmed();
    v.title = title_edit_->text().trimmed();
    v.description = desc_edit_->toPlainText().trimmed();
    v.author = author_edit_->text().trimmed();
    v.version = version_edit_->text().trimmed();
    v.icon_path = icon_edit_->text().trimmed();
    v.image_path = image_edit_->text().trimmed();
    return v;
}
