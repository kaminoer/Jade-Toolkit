#include "MaterialPickerDialog.hpp"

#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Material.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"
#include "Theme.hpp"

namespace {

QPixmap pixmap_from_rgba(const QImage& rgba) {
    return QPixmap::fromImage(rgba);
}

// A retargetable sub-material record: gro_type 5 with a texture-key field
// at a known kind offset (asset_add.is_submaterial).
bool is_submaterial(const jade::SubEntry& s) {
    return !s.gro_null
           && int(s.gro_type) == jade::GRO_TYPE_MAT_MULTITEXTURE
           && jade::resolve_texture_key(s.data.data(), s.data.size()) != 0;
}

// Decoded pixels of the largest real texture under `texk` (_texture_rgba).
QImage texture_rgba(const std::vector<jade::SubEntry>& subs, quint32 texk) {
    const jade::SubEntry* best = nullptr;
    size_t best_pixlen = 0;
    jade::TexInfo best_ti;
    for (const jade::SubEntry& s : subs) {
        if (s.key != texk) continue;
        if (!jade::is_texture_entry(s.data.data(), s.data.size())) continue;
        const jade::TexInfo tp =
            jade::parse_texture(s.data.data(), s.data.size());
        if (!tp.valid) continue;
        const size_t pixlen =
            s.data.size() > tp.pix_start ? s.data.size() - tp.pix_start : 0;
        if (!best || pixlen > best_pixlen) {
            best = &s;
            best_pixlen = pixlen;
            best_ti = tp;
        }
    }
    if (!best) return QImage();
    const std::vector<uint8_t>* pal =
        jade::palette_for_texture(best_ti, subs);
    const std::vector<uint8_t> rgba = jade::decode_texture(
        best->data.data(), best->data.size(), best_ti,
        pal ? pal->data() : nullptr, pal ? pal->size() : 0);
    if (rgba.empty()) return QImage();
    QImage img(rgba.data(), int(best_ti.width), int(best_ti.height),
               int(best_ti.width) * 4, QImage::Format_RGBA8888);
    return img.copy();
}

QImage load_source_rgba(const QString& path) {
    if (path.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QImage();
        const QByteArray raw = f.readAll();
        const jade::DdsImage img = jade::read_dds(
            reinterpret_cast<const uint8_t*>(raw.constData()),
            size_t(raw.size()));
        if (!img.ok) return QImage();
        QImage out(img.rgba.data(), int(img.width), int(img.height),
                   int(img.width) * 4, QImage::Format_RGBA8888);
        return out.copy();
    }
    QImage img(path);
    return img.isNull() ? QImage()
                        : img.convertToFormat(QImage::Format_RGBA8888);
}

}  // namespace

MaterialPickerDialog::MaterialPickerDialog(const QString& bf_path,
                                           quint32 entry_idx,
                                           long long current_material_key,
                                           ProjectDoc* project,
                                           long long entry_key,
                                           QWidget* parent)
    : QDialog(parent),
      bf_path_(bf_path),
      entry_idx_(entry_idx),
      current_key_(current_material_key),
      project_(project),
      entry_key_(entry_key) {
    setWindowTitle(tr("Pick a material"));
    resize(760, 480);

    auto* root = new QVBoxLayout(this);
    auto* header = new QLabel(
        current_material_key >= 0
            ? tr("Pick the material this matId slot should use. The slot "
                 "currently holds <b>%1</b>.")
                  .arg(qs(hex_key_lower(quint32(current_material_key))))
            : tr("Pick the material this matId slot should use."));
    header->setWordWrap(true);
    root->addWidget(header);

    filter_ = new QLineEdit();
    filter_->setPlaceholderText(tr("Filter by key (hex)…"));
    connect(filter_, &QLineEdit::textChanged, this,
            &MaterialPickerDialog::populate_visible);
    root->addWidget(filter_);

    auto* split = new QSplitter(Qt::Horizontal);
    list_ = new QListWidget();
    list_->setIconSize(QSize(64, 64));
    connect(list_, &QListWidget::itemSelectionChanged, this,
            &MaterialPickerDialog::on_selection);
    connect(list_, &QListWidget::itemDoubleClicked, this,
            &MaterialPickerDialog::on_dbl_click);
    split->addWidget(list_);

    auto* right = new QWidget();
    auto* rlay = new QVBoxLayout(right);
    rlay->setContentsMargins(8, 0, 0, 0);
    preview_ = new QLabel(tr("(select a material)"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setMinimumSize(256, 256);
    preview_->setStyleSheet(QStringLiteral("background: %1; color: %2;")
                                .arg(theme::BASE_BG, theme::DIM_TEXT));
    rlay->addWidget(preview_, 1);
    info_ = new QLabel(QString());
    info_->setWordWrap(true);
    rlay->addWidget(info_);
    split->addWidget(right);
    split->setStretchFactor(0, 1);
    root->addWidget(split, 1);

    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok
                                      | QDialogButtonBox::Cancel);
    ok_btn_ = btns->button(QDialogButtonBox::Ok);
    ok_btn_->setEnabled(false);
    connect(btns, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(btns);

    reload();
}

// ── data ──

void MaterialPickerDialog::reload() {
    rows_.clear();
    std::vector<jade::SubEntry> subs;
    try {
        jade::BigFile bf;
        bf.open(bf_path_.toStdString());
        const jade::LzoResult r =
            jade::decompress_lzo(bf.read_data(entry_idx_));
        if (r.ok) subs = jade::walk_sub_entries(r.data);
    } catch (const std::exception&) {
    }

    for (const jade::SubEntry& s : subs) {
        if (!is_submaterial(s)) continue;
        Row row;
        row.key = s.key;
        row.kind = s.data.size() >= 4
                       ? (qint64(s.data[0]) | (qint64(s.data[1]) << 8)
                          | (qint64(s.data[2]) << 16)
                          | (qint64(s.data[3]) << 24))
                       : 0;
        row.tex_key = jade::resolve_texture_key(s.data.data(), s.data.size());
        if (row.tex_key) row.rgba = texture_rgba(subs, row.tex_key);
        rows_.push_back(std::move(row));
    }

    // Materials minted by pending add_material ops on this entry.
    if (project_ && entry_key_ >= 0) {
        std::set<quint32> present;
        for (const Row& r : rows_) present.insert(r.key);
        for (const jade::json::Value& op : project_->operations) {
            const jade::json::Value* op_type = op.find("op");
            if (!op_type || op_type->str != "add_material") continue;
            const jade::json::Value* en = op.find("enabled");
            if (en && en->type == jade::json::Value::Type::Bool && !en->b)
                continue;
            const jade::json::Value* tgt = op.find("target");
            const jade::json::Value* prm = op.find("params");
            if (!tgt || !prm) continue;
            const jade::json::Value* ek = tgt->find("entry_key");
            if (!ek || !ek->is_str()) continue;
            bool okp = false;
            const quint32 op_entry_key =
                QString::fromStdString(ek->str).toUInt(&okp, 16);
            if (!okp || op_entry_key != quint32(entry_key_)) continue;
            const jade::json::Value* nk = prm->find("new_key");
            if (!nk || !nk->is_str()) continue;
            const quint32 new_key =
                QString::fromStdString(nk->str).toUInt(nullptr, 16);
            if (present.count(new_key)) continue;

            Row row;
            row.key = new_key;
            const jade::json::Value* tk = prm->find("texture_key");
            row.tex_key = tk && tk->is_str()
                              ? QString::fromStdString(tk->str).toUInt(
                                    nullptr, 16)
                              : 0;
            const jade::json::Value* id = op.find("id");
            row.added_by = id && id->is_str() ? qs(id->str) : QString();
            if (row.tex_key) {
                row.rgba = texture_rgba(subs, row.tex_key);
                if (row.rgba.isNull()) {
                    // Texture might itself be a pending add_texture.
                    for (const jade::json::Value& top :
                         project_->operations) {
                        const jade::json::Value* tt = top.find("op");
                        if (!tt || tt->str != "add_texture") continue;
                        const jade::json::Value* ten = top.find("enabled");
                        if (ten
                            && ten->type == jade::json::Value::Type::Bool
                            && !ten->b)
                            continue;
                        const jade::json::Value* tprm = top.find("params");
                        if (!tprm) continue;
                        const jade::json::Value* tnk =
                            tprm->find("new_key");
                        if (!tnk || !tnk->is_str()) continue;
                        if (QString::fromStdString(tnk->str).toUInt(
                                nullptr, 16)
                            != row.tex_key)
                            continue;
                        const jade::json::Value* src =
                            tprm->find("source");
                        if (src && src->is_str()) {
                            const QString path =
                                project_->resolve_asset(qs(src->str));
                            if (!path.isEmpty())
                                row.rgba = load_source_rgba(path);
                        }
                        break;
                    }
                }
            }
            rows_.push_back(std::move(row));
        }
    }

    std::sort(rows_.begin(), rows_.end(),
              [](const Row& a, const Row& b) { return a.key < b.key; });
    populate_visible();
}

void MaterialPickerDialog::populate_visible() {
    const QString flt = filter_->text().trimmed().toLower();
    list_->clear();
    for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        const QString label = format_row(row);
        if (!flt.isEmpty() && !label.toLower().contains(flt)) continue;
        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, int(i));
        if (!row.rgba.isNull())
            item->setIcon(QIcon(pixmap_from_rgba(row.rgba).scaled(
                64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        if (current_key_ >= 0 && row.key == quint32(current_key_)) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            item->setText(label + tr("   ← current"));
        }
        list_->addItem(item);
    }
}

QString MaterialPickerDialog::format_row(const Row& row) {
    const QString tex =
        row.tex_key ? QStringLiteral("tex %1").arg(qs(hex_key_lower(row.tex_key)))
                    : QStringLiteral("no tex");
    const QString kind = row.kind >= 0
                             ? QStringLiteral("kind %1").arg(row.kind)
                             : QStringLiteral("new");
    const QString added =
        row.added_by.isEmpty()
            ? QString()
            : QStringLiteral("   [new — %1]").arg(row.added_by);
    return QStringLiteral("%1   %2   %3%4")
        .arg(qs(hex_key_lower(row.key)), kind, tex, added);
}

// ── selection / preview ──

void MaterialPickerDialog::on_selection() {
    const auto items = list_->selectedItems();
    if (items.isEmpty()) {
        ok_btn_->setEnabled(false);
        preview_->setText(tr("(select a material)"));
        preview_->setPixmap(QPixmap());
        info_->setText(QString());
        selected_ = -1;
        return;
    }
    const int idx = items.first()->data(Qt::UserRole).toInt();
    if (idx < 0 || size_t(idx) >= rows_.size()) return;
    const Row& row = rows_[size_t(idx)];
    selected_ = row.key;
    ok_btn_->setEnabled(true);
    if (!row.rgba.isNull()) {
        const QPixmap pix = pixmap_from_rgba(row.rgba);
        preview_->setPixmap(pix.scaled(preview_->size(),
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
        preview_->setText(QString());
    } else {
        preview_->setPixmap(QPixmap());
        preview_->setText(tr("(no texture preview)"));
    }
    QStringList bits{QStringLiteral("<b>%1</b>").arg(
        qs(hex_key_lower(row.key)))};
    if (row.kind >= 0) bits << QStringLiteral("kind %1").arg(row.kind);
    if (!row.added_by.isEmpty())
        bits << tr("new material from op %1").arg(row.added_by);
    if (row.tex_key)
        bits << tr("texture %1").arg(qs(hex_key_lower(row.tex_key)));
    info_->setText(bits.join(QStringLiteral("<br>")));
}

void MaterialPickerDialog::on_dbl_click(QListWidgetItem*) {
    if (selected_ >= 0) accept();
}
