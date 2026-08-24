#include "TexturePickerDialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"
#include "Theme.hpp"

namespace {

QString fmt_name(int fmt) {
    switch (fmt) {
        case 0: return QStringLiteral("BGRA");
        case 1: return QStringLiteral("PAL8");
        case 5: return QStringLiteral("DXT1");
        case 6: return QStringLiteral("DXT3");
        case 7: return QStringLiteral("DXT5");
        case 11: return QStringLiteral("4bpp");
        default: return QStringLiteral("fmt%1").arg(fmt);
    }
}

// Load a project-imported source image (the ops_add_asset._load_source_rgba
// equivalent): QImage handles PNG/JPG/BMP/TGA; DDS goes through the jade
// reader.
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

// Collect unique textures in a decompressed BF entry, preferring the
// variant with the most pixel data (Jade ships a tiny stub first then the
// real texture) — _list_textures_in_entry.
void list_textures_in_entry(const std::vector<uint8_t>& dec,
                            std::map<quint32, size_t>& best_len,
                            std::vector<jade::SubEntry>& keep) {
    for (jade::SubEntry& s : jade::walk_sub_entries(dec)) {
        if (!jade::is_texture_entry(s.data.data(), s.data.size())) continue;
        const jade::TexInfo tp =
            jade::parse_texture(s.data.data(), s.data.size());
        if (!tp.valid) continue;
        const size_t pixlen =
            s.data.size() > tp.pix_start ? s.data.size() - tp.pix_start : 0;
        auto it = best_len.find(s.key);
        if (it == best_len.end() || pixlen > it->second) {
            best_len[s.key] = pixlen;
            keep.erase(std::remove_if(keep.begin(), keep.end(),
                                      [&s](const jade::SubEntry& e) {
                                          return e.key == s.key;
                                      }),
                       keep.end());
            keep.push_back(std::move(s));
        }
    }
}

}  // namespace

TexturePickerDialog::TexturePickerDialog(const QString& bf_path,
                                         quint32 current_entry_idx,
                                         long long current_texture_key,
                                         ProjectDoc* project,
                                         long long entry_key,
                                         QWidget* parent)
    : QDialog(parent),
      bf_path_(bf_path),
      entry_idx_(current_entry_idx),
      current_key_(current_texture_key),
      project_(project),
      entry_key_(entry_key) {
    setWindowTitle(tr("Pick a texture"));
    resize(720, 480);

    auto* root = new QVBoxLayout(this);

    auto* header = new QLabel(
        current_texture_key >= 0
            ? tr("Pick a texture key to retarget the material at. The "
                 "current texture key is <b>%1</b>.")
                  .arg(qs(hex_key_lower(quint32(current_texture_key))))
            : tr("Pick a texture key to retarget the material at."));
    header->setWordWrap(true);
    root->addWidget(header);

    auto* row = new QHBoxLayout();
    filter_ = new QLineEdit();
    filter_->setPlaceholderText(
        tr("Filter by key (hex) or dims (e.g. 256, dxt5)…"));
    connect(filter_, &QLineEdit::textChanged, this,
            &TexturePickerDialog::apply_filter);
    row->addWidget(filter_, 1);
    scope_ = new QCheckBox(tr("Search all BF entries (slower)"));
    connect(scope_, &QCheckBox::stateChanged, this,
            &TexturePickerDialog::reload);
    row->addWidget(scope_);
    root->addLayout(row);

    auto* split = new QSplitter(Qt::Horizontal);
    list_ = new QListWidget();
    list_->setIconSize(QSize(64, 64));
    connect(list_, &QListWidget::itemSelectionChanged, this,
            &TexturePickerDialog::on_selection);
    connect(list_, &QListWidget::itemDoubleClicked, this,
            &TexturePickerDialog::on_dbl_click);
    split->addWidget(list_);

    // Preview panel on the right
    auto* right = new QWidget();
    auto* rlay = new QVBoxLayout(right);
    rlay->setContentsMargins(8, 0, 0, 0);
    preview_ = new QLabel(tr("(select a texture)"));
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
    split->setStretchFactor(1, 0);
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

void TexturePickerDialog::reload() {
    list_->clear();
    all_rows_.clear();
    jade::BigFile bf;
    try {
        bf.open(bf_path_.toStdString());
    } catch (const std::exception&) {
        list_->addItem(tr("(could not open BF)"));
        return;
    }

    std::map<quint32, size_t> best_len;
    std::vector<jade::SubEntry> keep;
    if (scope_->isChecked()) {
        // Scan the whole archive — every entry that could carry textures.
        for (const auto& kv : bf.files) {
            const jade::BFFile& fi = kv.second;
            if (fi.length == 0) continue;
            const jade::LzoResult r =
                jade::decompress_lzo(bf.read_data(fi.index));
            if (!r.ok) continue;
            list_textures_in_entry(r.data, best_len, keep);
        }
    } else {
        const jade::LzoResult r =
            jade::decompress_lzo(bf.read_data(entry_idx_));
        if (r.ok) list_textures_in_entry(r.data, best_len, keep);
    }

    std::set<quint32> present;
    for (const jade::SubEntry& s : keep) {
        const jade::TexInfo tp =
            jade::parse_texture(s.data.data(), s.data.size());
        Row row;
        row.key = s.key;
        row.width = QString::number(tp.width);
        row.height = QString::number(tp.height);
        row.format = int(tp.format);
        row.payload = s.data;
        all_rows_.push_back(std::move(row));
        present.insert(s.key);
    }

    // Textures minted by pending add_texture ops on this entry.
    std::vector<Row> added;
    project_added_rows(added);
    for (Row& row : added)
        if (!present.count(row.key)) all_rows_.push_back(std::move(row));

    std::sort(all_rows_.begin(), all_rows_.end(),
              [](const Row& a, const Row& b) { return a.key < b.key; });
    populate_visible();
}

// Rows for textures minted by enabled add_texture ops targeting
// entry_key_. They exist only in the project until Build, so the picker
// fakes a parse-result row from the imported source image.
void TexturePickerDialog::project_added_rows(std::vector<Row>& rows) const {
    if (!project_ || entry_key_ < 0) return;
    for (const jade::json::Value& op : project_->operations) {
        const jade::json::Value* op_type = op.find("op");
        if (!op_type || op_type->str != "add_texture") continue;
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

        Row row;
        const jade::json::Value* nk = prm->find("new_key");
        if (!nk || !nk->is_str()) continue;
        row.key = QString::fromStdString(nk->str).toUInt(nullptr, 16);
        const jade::json::Value* enc = prm->find("encode");
        const QString enc_s =
            enc && enc->is_str() ? qs(enc->str) : QStringLiteral("7");
        bool enc_ok = false;
        const int enc_i = enc_s.toInt(&enc_ok);
        row.format = enc_ok ? enc_i : 7;
        const jade::json::Value* id = op.find("id");
        row.added_by = id && id->is_str() ? qs(id->str) : QString();
        const jade::json::Value* src = prm->find("source");
        if (src && src->is_str() && !src->str.empty()) {
            const QString path = project_->resolve_asset(qs(src->str));
            if (!path.isEmpty()) {
                const QImage rgba = load_source_rgba(path);
                if (!rgba.isNull()) {
                    row.rgba = rgba;
                    row.width = QString::number(rgba.width());
                    row.height = QString::number(rgba.height());
                }
            }
        }
        rows.push_back(std::move(row));
    }
}

void TexturePickerDialog::populate_visible() {
    const QString flt = filter_->text().trimmed().toLower();
    list_->clear();
    for (size_t i = 0; i < all_rows_.size(); ++i) {
        const Row& row = all_rows_[i];
        const QString label = format_row(row);
        if (!flt.isEmpty() && !label.toLower().contains(flt)) continue;
        auto* item = new QListWidgetItem(label);
        item->setData(Qt::UserRole, int(i));
        if (current_key_ >= 0 && row.key == quint32(current_key_)) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
            item->setText(label + tr("   ← current"));
        }
        list_->addItem(item);
    }
}

QString TexturePickerDialog::format_row(const Row& row) {
    const QString added =
        row.added_by.isEmpty()
            ? QString()
            : QStringLiteral("   [new — %1]").arg(row.added_by);
    return QStringLiteral("%1   %2x%3   %4%5")
        .arg(qs(hex_key_lower(row.key)), row.width, row.height,
             fmt_name(row.format), added);
}

QImage TexturePickerDialog::decode_row(const Row& row) const {
    // Project-added textures aren't in the BF yet — their preview pixels
    // come straight from the imported source image.
    if (!row.rgba.isNull()) return row.rgba;
    if (row.payload.empty()) return QImage();
    const jade::TexInfo ti =
        jade::parse_texture(row.payload.data(), row.payload.size());
    if (!ti.valid) return QImage();
    const std::vector<uint8_t> rgba =
        jade::decode_texture(row.payload.data(), row.payload.size(), ti);
    if (rgba.empty()) return QImage();
    QImage img(rgba.data(), int(ti.width), int(ti.height),
               int(ti.width) * 4, QImage::Format_RGBA8888);
    return img.copy();
}

// ── selection / preview ──

void TexturePickerDialog::on_selection() {
    const auto items = list_->selectedItems();
    if (items.isEmpty()) {
        ok_btn_->setEnabled(false);
        preview_->setText(tr("(select a texture)"));
        preview_->setPixmap(QPixmap());
        info_->setText(QString());
        selected_ = -1;
        return;
    }
    const int idx = items.first()->data(Qt::UserRole).toInt();
    if (idx < 0 || size_t(idx) >= all_rows_.size()) return;
    const Row& row = all_rows_[size_t(idx)];
    selected_ = row.key;
    ok_btn_->setEnabled(true);
    const QImage img = decode_row(row);
    if (img.isNull()) {
        preview_->setPixmap(QPixmap());
        preview_->setText(tr("(no preview)"));
    } else {
        const QPixmap scaled = QPixmap::fromImage(img).scaled(
            preview_->size(), Qt::KeepAspectRatio,
            Qt::SmoothTransformation);
        preview_->setPixmap(scaled);
        preview_->setText(QString());
    }
    info_->setText(QStringLiteral("<b>%1</b><br>%2×%3, %4")
                       .arg(qs(hex_key_lower(row.key)), row.width,
                            row.height, fmt_name(row.format)));
}

void TexturePickerDialog::on_dbl_click(QListWidgetItem*) {
    if (selected_ >= 0) accept();
}

void TexturePickerDialog::apply_filter() { populate_visible(); }
