// TextureSwapTab.cpp — port of gui/texture_swap_tab.py (see the header for
// the module docstring).
#include "TextureSwapTab.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QVariant>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"
#include "Theme.hpp"

namespace {

// io_ops/texture_upscale.py header offsets — the +36/+40/+44 "actual"
// format / width / height fields of the 52-byte texture header.
constexpr size_t HDR_FMT_OFF = 36;
constexpr size_t HDR_W_OFF = 40;
constexpr size_t HDR_H_OFF = 44;

// _FMT_NAMES = {0:'BGRA', 1:'PAL8', 5:'DXT1', 6:'DXT3', 7:'DXT5', 11:'4bpp'}
QString fmt_name(quint32 fmt) {
    switch (fmt) {
        case 0: return QStringLiteral("BGRA");
        case 1: return QStringLiteral("PAL8");
        case 5: return QStringLiteral("DXT1");
        case 6: return QStringLiteral("DXT3");
        case 7: return QStringLiteral("DXT5");
        case 11: return QStringLiteral("4bpp");
        default: return QStringLiteral("?");
    }
}

// struct.unpack_from('<I', d, off).
quint32 rd_u32(const std::vector<uint8_t>& d, size_t off) {
    return quint32(d[off]) | (quint32(d[off + 1]) << 8)
           | (quint32(d[off + 2]) << 16) | (quint32(d[off + 3]) << 24);
}

// Python f"{n:,}" — thousands separators.
QString fmt_thousands(qulonglong n) {
    return QLocale(QLocale::English, QLocale::UnitedStates).toString(n);
}

// _decode_to_pixmap: decode a parsed texture to a QPixmap, or a null pixmap
// on failure (the Python None).
//
// Uses the texture's "actual" dims if they're set and differ from
// the header +8/+10 dims — third-party mods upscale by bumping
// +40/+44 only, so honoring those produces a faithful preview.
QPixmap decode_to_pixmap(const std::vector<uint8_t>& raw, jade::TexInfo ti,
                         const std::vector<uint8_t>& palette) {
    if (raw.size() >= 52) {
        const quint32 actual_w = rd_u32(raw, HDR_W_OFF);
        const quint32 actual_h = rd_u32(raw, HDR_H_OFF);
        const quint32 actual_fmt = rd_u32(raw, HDR_FMT_OFF);
        if (actual_w && actual_h
            && (actual_w != ti.width || actual_h != ti.height
                || actual_fmt != ti.format)) {
            ti.width = actual_w;
            ti.height = actual_h;
            ti.format = actual_fmt;
        }
    }
    const std::vector<uint8_t> rgba = jade::decode_texture(
        raw.data(), raw.size(), ti,
        palette.empty() ? nullptr : palette.data(), palette.size());
    if (rgba.empty()) return QPixmap();
    QImage img(rgba.data(), int(ti.width), int(ti.height), int(ti.width) * 4,
               QImage::Format_RGBA8888);
    return QPixmap::fromImage(img.copy());
}

// _list_textures: return TexRow rows for unique textures inside a BF entry.
//
// Pass an already-open ``bf`` to skip re-parsing the FAT — the Asset
// Browser reuses its open archive; the tab itself opens by path.
std::vector<TextureSwapTab::TexRow> list_textures(const QString& bf_path,
                                                  quint32 entry_idx,
                                                  jade::BigFile* bf) {
    jade::BigFile local;
    if (bf == nullptr) {
        local.open(bf_path.toStdString());
        bf = &local;
    }
    const std::vector<uint8_t> raw = bf->read_data(entry_idx);
    const jade::LzoResult dec = jade::decompress_lzo(raw);
    if (!dec.ok) return {};
    const std::vector<jade::SubEntry> subs = jade::walk_sub_entries(dec.data);

    // by_key: keep the same-key sub-entry with the most data (insertion order).
    std::vector<const jade::SubEntry*> keep;
    std::map<quint32, size_t> pos;
    for (const jade::SubEntry& s : subs) {
        const std::vector<uint8_t>& d = s.data;
        if (!jade::is_texture_entry(d.data(), d.size())) continue;
        const jade::TexInfo t = jade::parse_texture(d.data(), d.size());
        if (!t.valid) continue;
        const auto it = pos.find(s.key);
        if (it == pos.end()) {
            pos[s.key] = keep.size();
            keep.push_back(&s);
        } else if (d.size() > keep[it->second]->data.size()) {
            keep[it->second] = &s;
        }
    }

    std::vector<TextureSwapTab::TexRow> rows;
    for (const jade::SubEntry* sp : keep) {
        const std::vector<uint8_t>& d = sp->data;
        const jade::TexInfo t = jade::parse_texture(d.data(), d.size());
        const quint32 actual_w = d.size() >= 52 ? rd_u32(d, HDR_W_OFF) : 0;
        const quint32 actual_h = d.size() >= 52 ? rd_u32(d, HDR_H_OFF) : 0;
        const std::vector<uint8_t>* palette =
            jade::palette_for_texture(t, subs);
        TextureSwapTab::TexRow row;
        row.key = sp->key;
        row.logical_w = t.logical_width;
        row.logical_h = t.logical_height;
        row.actual_w = actual_w;
        row.actual_h = actual_h;
        row.format = t.format;
        row.payload_bytes = d.size();
        row.payload = d;
        row.tex_info = t;
        if (palette) row.palette = *palette;
        rows.push_back(std::move(row));
    }
    return rows;
}

}  // namespace

// ── free helpers (shared with the Asset Browser's texture export) ──────────

std::pair<quint32, quint32> save_texture_png(
    const std::vector<uint8_t>& payload, jade::TexInfo tex_info,
    const uint8_t* palette, size_t pal_len, const QString& out_path,
    quint32 actual_w, quint32 actual_h) {
    if (actual_w && actual_h
        && (actual_w != tex_info.width || actual_h != tex_info.height)) {
        tex_info.width = actual_w;
        tex_info.height = actual_h;
    }
    const std::vector<uint8_t> rgba = jade::decode_texture(
        payload.data(), payload.size(), tex_info, palette, pal_len);
    if (rgba.empty())
        throw std::runtime_error(
            "decode returned no pixels — format not supported");
    QImage img(rgba.data(), int(tex_info.width), int(tex_info.height),
               int(tex_info.width) * 4, QImage::Format_RGBA8888);
    if (!img.copy().save(out_path, "PNG"))
        throw std::runtime_error("could not write "
                                 + out_path.toStdString());
    return {tex_info.width, tex_info.height};
}

std::pair<quint32, quint32> extract_texture_to_png(
    const QString& bf_path, quint32 entry_idx, quint32 tex_key,
    const QString& out_path, jade::BigFile* bf) {
    const std::vector<TextureSwapTab::TexRow> rows =
        list_textures(bf_path, entry_idx, bf);
    const TextureSwapTab::TexRow* info = nullptr;
    for (const TextureSwapTab::TexRow& r : rows)
        if (r.key == tex_key) { info = &r; break; }
    if (info == nullptr)
        throw std::runtime_error(
            QStringLiteral("texture %1 not found in BF entry %2")
                .arg(qs(hex_key_lower(tex_key)))
                .arg(entry_idx)
                .toStdString());
    return save_texture_png(info->payload, info->tex_info,
                            info->palette.empty() ? nullptr
                                                  : info->palette.data(),
                            info->palette.size(), out_path, info->actual_w,
                            info->actual_h);
}

// ── the tab ────────────────────────────────────────────────────────────────

TextureSwapTab::TextureSwapTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);

    bf_label_ = new QLabel(tr("No BigFile loaded."));
    root->addWidget(bf_label_);

    // Entry picker (same filter convention as Mesh Swap).
    auto* entry_group =
        new QGroupBox(tr("Step 1 — Pick a BF entry (filtered to *wow* files)"));
    auto* entry_form = new QFormLayout(entry_group);
    auto* row = new QHBoxLayout();
    entry_combo_ = new QComboBox();
    entry_combo_->setMinimumWidth(420);
    row->addWidget(entry_combo_, 1);
    list_btn_ = new QPushButton(tr("List textures"));
    connect(list_btn_, &QPushButton::clicked, this, &TextureSwapTab::on_list);
    row->addWidget(list_btn_);
    entry_form->addRow(tr("Entry:"), row);
    root->addWidget(entry_group);

    // Splitter: textures list (left) | current preview (middle) | new preview (right)
    auto* splitter = new QSplitter(Qt::Horizontal);

    tex_tree_ = new QTreeWidget();
    tex_tree_->setHeaderLabels({tr("Key"), tr("Logical"), tr("Actual"),
                                tr("Fmt"), tr("Bytes")});
    tex_tree_->setRootIsDecorated(false);
    tex_tree_->setAlternatingRowColors(true);
    connect(tex_tree_, &QTreeWidget::itemSelectionChanged, this,
            &TextureSwapTab::on_tex_selected);
    tex_tree_->setMinimumWidth(280);
    splitter->addWidget(tex_tree_);

    // Middle: current texture preview + extract.
    auto* cur_box = new QGroupBox(tr("Current (in BF)"));
    auto* cur_lay = new QVBoxLayout(cur_box);
    cur_info_ = new QLabel(tr("Select a texture to preview."));
    cur_info_->setWordWrap(true);
    cur_lay->addWidget(cur_info_);
    cur_scroll_ = new QScrollArea();
    cur_scroll_->setWidgetResizable(false);
    cur_label_ = new QLabel();
    cur_label_->setAlignment(Qt::AlignCenter);
    cur_scroll_->setWidget(cur_label_);
    cur_lay->addWidget(cur_scroll_, 1);
    auto* extract_row = new QHBoxLayout();
    extract_row->addStretch(1);
    extract_btn_ = new QPushButton(tr("Extract to PNG…"));
    extract_btn_->setToolTip(
        tr("Save the selected texture (with its current dims and decoded "
           "pixels) to a PNG file on disk. Edit it in any image editor and "
           "load it back via the right-hand 'New' panel to record a "
           "replace_texture op."));
    connect(extract_btn_, &QPushButton::clicked, this,
            &TextureSwapTab::on_extract);
    extract_row->addWidget(extract_btn_);
    cur_lay->addLayout(extract_row);
    splitter->addWidget(cur_box);

    // Right: new image preview + load button + apply.
    auto* new_box = new QGroupBox(tr("New (from disk)"));
    auto* new_lay = new QVBoxLayout(new_box);
    auto* load_row = new QHBoxLayout();
    new_path_edit_ = new QLineEdit();
    new_path_edit_->setReadOnly(true);
    new_path_edit_->setPlaceholderText(tr("Load a PNG / JPG / etc to preview"));
    load_row->addWidget(new_path_edit_, 1);
    auto* load_btn = new QPushButton(tr("Load image…"));
    connect(load_btn, &QPushButton::clicked, this,
            &TextureSwapTab::on_load_image);
    load_row->addWidget(load_btn);
    new_lay->addLayout(load_row);

    auto* fmt_row = new QHBoxLayout();
    fmt_row->addWidget(new QLabel(tr("Target format:")));
    fmt_combo_ = new QComboBox();
    // Item data: invalid QVariant == the Python None ("auto").
    fmt_combo_->addItem(tr("Auto (keep, PAL8→DXT5)"));
    fmt_combo_->addItem(tr("0 — BGRA (uncompressed)"), 0);
    fmt_combo_->addItem(tr("5 — DXT1 (no alpha)"), 5);
    fmt_combo_->addItem(tr("7 — DXT5 (with alpha)"), 7);
    fmt_row->addWidget(fmt_combo_, 1);
    new_lay->addLayout(fmt_row);

    new_info_ = new QLabel(tr("No image loaded."));
    new_info_->setWordWrap(true);
    new_lay->addWidget(new_info_);
    new_scroll_ = new QScrollArea();
    new_scroll_->setWidgetResizable(false);
    new_label_ = new QLabel();
    new_label_->setAlignment(Qt::AlignCenter);
    new_scroll_->setWidget(new_label_);
    new_lay->addWidget(new_scroll_, 1);

    auto* apply_row = new QHBoxLayout();
    project_hint_ = new QLabel(
        tr("<i>Open or create a Mod Project (File menu) to record edits.</i>"));
    // Python: "color: #888;" — theme dim text.
    project_hint_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    apply_row->addWidget(project_hint_, 1);
    apply_btn_ = new QPushButton(tr("Add to Project"));
    apply_btn_->setMinimumWidth(160);
    connect(apply_btn_, &QPushButton::clicked, this,
            &TextureSwapTab::on_apply);
    apply_btn_->setToolTip(
        tr("Append a replace_texture operation to the open Mod Project. "
           "The image is copied into the project's asset store; nothing is "
           "written to a .bf — that happens when you Build from the "
           "Project tab."));
    apply_row->addWidget(apply_btn_);
    new_lay->addLayout(apply_row);

    splitter->addWidget(new_box);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 2);
    splitter->setStretchFactor(2, 2);
    root->addWidget(splitter, 1);

    // Log
    log_ = new QTextEdit();
    log_->setReadOnly(true);
    log_->setFont(QFont(QStringLiteral("Consolas"), 9));
    log_->setMaximumHeight(140);
    root->addWidget(log_);

    update_enabled();
}

// ── public ──

void TextureSwapTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                                 const QString& bf_path) {
    bf_ = std::move(bf);
    bf_path_ = bf_path;
    bf_label_->setText(tr("BigFile: %1").arg(bf_path));
    tex_rows_.clear();
    tex_tree_->clear();
    cur_info_->setText(tr("Select a texture to preview."));
    cur_label_->setPixmap(QPixmap());
    populate_entry_combo();
    log_->append(tr("Loaded BigFile: %1").arg(bf_path));
    update_enabled();
}

void TextureSwapTab::set_project(ProjectDoc* project) {
    project_ = project;
    if (project == nullptr) {
        project_hint_->setText(tr(
            "<i>Open or create a Mod Project (File menu) to record edits.</i>"));
    } else {
        project_hint_->setText(
            tr("<i>Recording into project: <b>%1</b></i>")
                .arg(project->name.isEmpty() ? tr("(unnamed)")
                                             : project->name));
    }
    update_enabled();
}

// ── helpers ──

void TextureSwapTab::populate_entry_combo() {
    entry_combo_->blockSignals(true);
    entry_combo_->clear();
    if (!bf_ || bf_->files.empty()) {
        entry_combo_->blockSignals(false);
        return;
    }
    std::vector<std::pair<quint32, QString>> rows;
    for (const auto& kv : bf_->files) {
        const QString name = qs(kv.second.name);
        if (!name.isEmpty()
            && name.toLower().contains(QStringLiteral("wow")))
            rows.push_back({kv.first, name});
    }
    bool fallback = false;
    if (rows.empty()) {
        fallback = true;
        for (const auto& kv : bf_->files)
            if (!kv.second.name.empty())
                rows.push_back({kv.first, qs(kv.second.name)});
    }
    std::stable_sort(rows.begin(), rows.end(),
                     [](const std::pair<quint32, QString>& a,
                        const std::pair<quint32, QString>& b) {
                         return a.second.toLower() < b.second.toLower();
                     });
    for (const auto& r : rows)
        entry_combo_->addItem(QStringLiteral("%1  (#%2)").arg(r.second)
                                  .arg(r.first),
                              r.first);
    for (int i = 0; i < entry_combo_->count(); ++i) {
        if (entry_combo_->itemData(i).isValid()
            && entry_combo_->itemData(i).toUInt() == 1912u) {
            entry_combo_->setCurrentIndex(i);
            break;
        }
    }
    entry_combo_->blockSignals(false);
    log_->append(QStringLiteral("  %1: %2")
                     .arg(fallback ? tr("fallback: all entries")
                                   : tr("matched *wow* entries"))
                     .arg(rows.size()));
}

long long TextureSwapTab::current_entry_idx() const {
    const QVariant data = entry_combo_->currentData();
    return data.isValid() ? (long long)(data.toUInt()) : -1;
}

// Programmatic entry: open ``parent_index`` and pre-select ``sub_key``.
//
// Used by the Asset Browser's right-click "Send to Texture Swap"
// action. We try to find the entry in the combo first; if it isn't
// there (it might not match the 'wow' filter), we don't refresh
// the combo — too disruptive — and just bail.
// (The Python sub_key=None "no selection" case has no C++ caller —
// MainWindow always passes a key; an unmatched key selects nothing.)
void TextureSwapTab::receive_asset(quint32 parent_index, quint32 sub_key) {
    bool found = false;
    for (int i = 0; i < entry_combo_->count(); ++i) {
        if (entry_combo_->itemData(i).isValid()
            && entry_combo_->itemData(i).toUInt() == parent_index) {
            entry_combo_->setCurrentIndex(i);
            found = true;
            break;
        }
    }
    if (!found) return;
    on_list();
    for (int j = 0; j < tex_tree_->topLevelItemCount(); ++j) {
        QTreeWidgetItem* it = tex_tree_->topLevelItem(j);
        if (it->data(0, Qt::UserRole).toUInt() == sub_key) {
            tex_tree_->setCurrentItem(it);
            tex_tree_->scrollToItem(it);
            on_tex_selected();
            break;
        }
    }
}

void TextureSwapTab::update_enabled() {
    const bool has_bf = bf_ != nullptr;
    const bool has_tex = current_key_ >= 0;
    const bool has_new =
        !new_image_path_.isEmpty() && QFile::exists(new_image_path_);
    const bool has_proj = project_ != nullptr && !project_->path.isEmpty();
    list_btn_->setEnabled(has_bf);
    extract_btn_->setEnabled(has_bf && has_tex);
    apply_btn_->setEnabled(has_bf && has_tex && has_new && has_proj);
    if (!has_proj) {
        apply_btn_->setToolTip(
            tr("Open or create a Mod Project (File menu) first. The tab no "
               "longer writes to .bf files directly — every edit is recorded "
               "in the project and applied at Build time."));
    } else {
        apply_btn_->setToolTip(
            tr("Append a replace_texture operation to the open project."));
    }
}

// ── slots ──

void TextureSwapTab::on_list() {
    if (!bf_) return;
    const long long idx = current_entry_idx();
    if (idx < 0) {
        QMessageBox::warning(this, tr("Texture Swap"),
                             tr("Pick a BF entry first."));
        return;
    }
    const auto fit = bf_->files.find(quint32(idx));
    if (fit == bf_->files.end()) {
        QMessageBox::warning(this, tr("Texture Swap"),
                             tr("Entry %1 not found in BigFile.").arg(idx));
        return;
    }
    log_->append(tr("\n=== Listing textures in entry %1 (%2) ===")
                     .arg(idx)
                     .arg(qs(fit->second.name)));
    tex_tree_->clear();
    tex_rows_.clear();
    current_key_ = -1;
    cur_label_->setPixmap(QPixmap());
    cur_info_->setText(tr("Select a texture to preview."));

    std::vector<TexRow> rows;
    try {
        rows = list_textures(bf_path_, quint32(idx), nullptr);
    } catch (const std::exception& e) {
        log_->append(tr("  ERROR: %1").arg(e.what()));
        update_enabled();
        return;
    }

    std::stable_sort(rows.begin(), rows.end(),
                     [](const TexRow& a, const TexRow& b) {
                         return a.payload_bytes > b.payload_bytes;
                     });
    tex_rows_ = std::move(rows);
    for (const TexRow& row : tex_rows_) {
        auto* it = new QTreeWidgetItem(QStringList{
            qs(hex_key_lower(row.key)),
            QStringLiteral("%1x%2").arg(row.logical_w).arg(row.logical_h),
            QStringLiteral("%1x%2").arg(row.actual_w).arg(row.actual_h),
            QStringLiteral("%1 (%2)").arg(row.format).arg(fmt_name(row.format)),
            fmt_thousands(row.payload_bytes),
        });
        it->setData(0, Qt::UserRole, row.key);
        tex_tree_->addTopLevelItem(it);
    }
    tex_tree_->resizeColumnToContents(0);
    tex_tree_->resizeColumnToContents(1);
    tex_tree_->resizeColumnToContents(2);
    log_->append(tr("  found %1 textures").arg(tex_rows_.size()));
    update_enabled();
}

void TextureSwapTab::on_tex_selected() {
    const auto items = tex_tree_->selectedItems();
    if (items.isEmpty()) return;
    const quint32 key = items.first()->data(0, Qt::UserRole).toUInt();
    const TexRow* info = nullptr;
    for (const TexRow& r : tex_rows_)
        if (r.key == key) { info = &r; break; }
    if (info == nullptr) return;
    current_key_ = key;

    // Render current.
    QPixmap pix;
    try {
        pix = decode_to_pixmap(info->payload, info->tex_info, info->palette);
    } catch (const std::exception& e) {
        pix = QPixmap();
        log_->append(tr("  decode error: %1").arg(e.what()));
    }

    const quint32 fmt = info->format;
    cur_info_->setText(
        tr("<b>%1</b> &nbsp; "
           "logical %2×%3 &nbsp;|&nbsp; "
           "actual %4×%5 &nbsp;|&nbsp; "
           "fmt %6 (%7) &nbsp;|&nbsp; "
           "%8 B")
            .arg(qs(hex_key_lower(key)))
            .arg(info->logical_w)
            .arg(info->logical_h)
            .arg(info->actual_w)
            .arg(info->actual_h)
            .arg(fmt)
            .arg(fmt_name(fmt))
            .arg(fmt_thousands(info->payload_bytes)));
    if (!pix.isNull()) {
        cur_label_->setPixmap(pix);
        cur_label_->adjustSize();
    } else {
        cur_label_->setPixmap(QPixmap());
        cur_info_->setText(cur_info_->text()
                           + tr("<br><i>Decode failed.</i>"));
    }
    update_enabled();
}

void TextureSwapTab::on_load_image() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load image"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tga *.dds);;All Files (*)"));
    if (path.isEmpty()) return;
    new_image_path_ = path;
    new_path_edit_->setText(path);
    // Python: PIL Image.open(path).convert('RGBA'). QImage covers
    // PNG/JPG/BMP (+TGA via the imageformats plugin); DDS goes through the
    // jade reader (same approach as TexturePickerDialog).
    QImage img;
    if (path.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray raw = f.readAll();
            const jade::DdsImage dds = jade::read_dds(
                reinterpret_cast<const uint8_t*>(raw.constData()),
                size_t(raw.size()));
            if (dds.ok) {
                QImage tmp(dds.rgba.data(), int(dds.width), int(dds.height),
                           int(dds.width) * 4, QImage::Format_RGBA8888);
                img = tmp.copy();
            }
        }
    } else {
        QImage loaded(path);
        if (!loaded.isNull())
            img = loaded.convertToFormat(QImage::Format_RGBA8888);
    }
    if (!img.isNull()) {
        const QPixmap pix = QPixmap::fromImage(img);
        new_label_->setPixmap(pix);
        new_label_->adjustSize();
        new_info_->setText(tr("<b>%1</b> &nbsp;|&nbsp; %2×%3")
                               .arg(QFileInfo(path).fileName())
                               .arg(img.width())
                               .arg(img.height()));
    } else {
        new_info_->setText(tr("Failed to open image: %1")
                               .arg(tr("unsupported or unreadable format")));
        new_label_->setPixmap(QPixmap());
    }
    update_enabled();
}

// Return list of BF entry indices whose sub-entry table holds
// ``sub_key`` as a texture. Same-key duplicates across costume
// entries (SoT prince texture pattern) are how a swap can land in
// the BF yet not show in-game when the engine resolves the texture
// from a different entry than the one the user picked.
//
// Two-pass: a fast raw-byte grep over the COMPRESSED entry bytes
// narrows the 3000+ entries to a handful of candidates, then we
// decompress and confirm via walk_sub_entries (compressed
// LZO usually preserves key bytes verbatim because each sub-entry
// header is a unique 12-byte run that doesn't dictionary-compress
// away). Returns indices in FAT order.
std::vector<quint32> TextureSwapTab::find_entries_with_texture_key(
    quint32 sub_key) const {
    if (!bf_ || bf_path_.isEmpty()) return {};
    const char key_bytes[4] = {
        char(sub_key & 0xFF), char((sub_key >> 8) & 0xFF),
        char((sub_key >> 16) & 0xFF), char((sub_key >> 24) & 0xFF)};
    const QByteArray needle(key_bytes, 4);
    std::vector<quint32> candidates;
    QFile f(bf_path_);
    if (!f.open(QIODevice::ReadOnly)) return {};
    for (const auto& kv : bf_->files) {
        const jade::BFFile& fi = kv.second;
        if (fi.length == 0) continue;
        if (!f.seek(fi.pos)) return {};
        const QByteArray raw = f.read(fi.length);
        if (raw.contains(needle)) candidates.push_back(kv.first);
    }

    std::vector<quint32> confirmed;
    for (quint32 idx : candidates) {
        try {
            const std::vector<uint8_t> raw = bf_->read_data(idx);
            if (raw.empty()) continue;
            const jade::LzoResult dec = jade::decompress_lzo(raw);
            if (!dec.ok || dec.data.empty()) continue;
            for (const jade::SubEntry& s :
                 jade::walk_sub_entries(dec.data)) {
                if (s.key != sub_key) continue;
                if (jade::is_texture_entry(s.data.data(), s.data.size())) {
                    confirmed.push_back(idx);
                    break;
                }
            }
        } catch (const std::exception&) {
            continue;
        }
    }
    return confirmed;
}

// Append a ``replace_texture`` operation to the open project.
//
// Phase 1: the tab never writes to a ``.bf``. The image is copied into
// the project's asset store; the actual splice happens at Build time
// from the Project panel.
//
// SoT (and to a lesser extent T2T) duplicates per-actor textures
// across multiple BF entries — a face skin can live in 7 costume
// variants. When the user picks one entry but the texture also lives
// in others, ask whether to mirror the replacement across all of
// them. Without this, the swap goes into a single entry that the
// engine probably isn't loading at runtime (e.g. user picked
// Prince_wow but the active costume is PrinceCostume01_wow).
void TextureSwapTab::on_apply() {
    if (!bf_ || current_key_ < 0) return;
    if (new_image_path_.isEmpty() || !QFile::exists(new_image_path_)) {
        QMessageBox::warning(this, tr("Texture Swap"),
                             tr("Load a replacement image first."));
        return;
    }
    const long long idx = current_entry_idx();
    if (idx < 0) {
        QMessageBox::warning(this, tr("Texture Swap"),
                             tr("Pick a BF entry first."));
        return;
    }
    if (project_ == nullptr || project_->path.isEmpty()) {
        QMessageBox::warning(
            this, tr("Texture Swap"),
            tr("No project open. Use File → New Project or Open Project "
               "first; the tab records edits into a project rather than "
               "writing to the .bf directly."));
        return;
    }

    const auto fit = bf_->files.find(quint32(idx));
    if (fit == bf_->files.end()) {
        QMessageBox::warning(this, tr("Texture Swap"),
                             tr("Entry %1 not found in BigFile.").arg(idx));
        return;
    }
    const jade::BFFile& fi = fit->second;
    const quint32 sub_key = quint32(current_key_);
    // forward-compat; today: not honoured (invalid QVariant == None/auto)
    const QVariant fmt = fmt_combo_->currentData();

    // Cross-entry scan: which entries also contain this texture key?
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::vector<quint32> all_entries = find_entries_with_texture_key(sub_key);
    QApplication::restoreOverrideCursor();

    // Always include the current entry even if the scan somehow missed
    // it (corruption, transient read error).
    if (std::find(all_entries.begin(), all_entries.end(), quint32(idx))
        == all_entries.end())
        all_entries.insert(all_entries.begin(), quint32(idx));
    // Dedup + keep FAT order.
    {
        std::set<quint32> seen;
        std::vector<quint32> deduped;
        for (quint32 e : all_entries)
            if (seen.insert(e).second) deduped.push_back(e);
        all_entries = std::move(deduped);
    }

    std::vector<quint32> target_entries{quint32(idx)};
    if (all_entries.size() > 1) {
        std::vector<quint32> others;
        for (quint32 e : all_entries)
            if (e != quint32(idx)) others.push_back(e);
        QStringList preview_lines;
        preview_lines << tr("  • entry %1: %2  (the one you selected)")
                             .arg(idx)
                             .arg(qs(fi.name));
        for (size_t i = 0; i < others.size() && i < 8; ++i) {
            const auto ofit = bf_->files.find(others[i]);
            if (ofit != bf_->files.end())
                preview_lines << tr("  • entry %1: %2")
                                     .arg(others[i])
                                     .arg(qs(ofit->second.name));
        }
        if (others.size() > 8)
            preview_lines << tr("  • … and %1 more").arg(others.size() - 8);

        QMessageBox box(this);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Texture in multiple entries"));
        box.setText(tr("Texture %1 is present in %2 BF entries.")
                        .arg(qs(hex_key_lower(sub_key)))
                        .arg(all_entries.size()));
        box.setInformativeText(
            tr("Replacing it in just one entry often won't show in-game "
               "because the engine may load the actor from a different "
               "entry (e.g. SoT loads PrinceCostume01_wow at default, "
               "not Prince_wow).\n\n%1\n\nReplace this texture in ALL "
               "entries, or only the one you selected?")
                .arg(preview_lines.join(QLatin1Char('\n'))));
        QPushButton* btn_all = box.addButton(
            tr("Replace All (%1)").arg(all_entries.size()),
            QMessageBox::AcceptRole);
        QPushButton* btn_one =
            box.addButton(tr("Only This Entry"), QMessageBox::DestructiveRole);
        QPushButton* btn_cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(btn_all);
        box.exec();
        QAbstractButton* clicked = box.clickedButton();
        if (clicked == btn_cancel || clicked == nullptr) return;
        if (clicked == btn_all) target_entries = all_entries;
        (void)btn_one;
    }

    QString err;
    const QString asset_ref = project_->import_asset(new_image_path_, &err);
    if (asset_ref.isEmpty()) {
        QMessageBox::critical(
            this, tr("Texture Swap"),
            tr("Failed to import image into project: %1").arg(err));
        return;
    }

    const QString encode =
        fmt.isValid() ? QString::number(fmt.toInt()) : QStringLiteral("auto");
    int added = 0;
    for (quint32 tgt : target_entries) {
        const auto tit = bf_->files.find(tgt);
        if (tit == bf_->files.end()) continue;
        const jade::BFFile& tgt_fi = tit->second;
        // ReplaceTexture(...) — dict shape from project/ops_texture.py
        // ReplaceTexture._params_to_dict():
        //   {"op": "replace_texture",
        //    "target": {"entry_key": "0x%08x", "sub_key": "0x%08x"},
        //    "params": {"source": "asset:<hash>",
        //               "encode": "auto"|"<fmt>", "mips": true}}
        // (id/enabled/created are filled by ProjectDoc::add_operation.)
        jade::json::Value op = jade::json::make_obj();
        op.obj["op"] = jade::json::make_str("replace_texture");
        jade::json::Value target = jade::json::make_obj();
        target.obj["entry_key"] =
            jade::json::make_str(hex_key_lower(tgt_fi.key));
        target.obj["sub_key"] = jade::json::make_str(hex_key_lower(sub_key));
        op.obj["target"] = std::move(target);
        jade::json::Value params = jade::json::make_obj();
        params.obj["source"] = jade::json::make_str(asset_ref.toStdString());
        params.obj["encode"] = jade::json::make_str(encode.toStdString());
        params.obj["mips"] = jade::json::make_bool(true);
        op.obj["params"] = std::move(params);
        op.obj["label"] = jade::json::make_str(
            QStringLiteral("texture %1 in %2 ← %3")
                .arg(qs(hex_key_lower(sub_key)), qs(tgt_fi.name),
                     QFileInfo(new_image_path_).fileName())
                .toStdString());
        const QString op_id = project_->add_operation(std::move(op));
        log_->append(tr("[+] added replace_texture %1: entry %2 "
                        "(%3) sub %4 ← %5…")
                         .arg(op_id, qs(hex_key_lower(tgt_fi.key)),
                              qs(tgt_fi.name), qs(hex_key_lower(sub_key)),
                              asset_ref.left(18)));
        added += 1;
    }
    if (added > 1) {
        log_->append(tr("\n=> queued %1 replace_texture ops "
                        "for the same texture %2")
                         .arg(added)
                         .arg(qs(hex_key_lower(sub_key))));
    }
    update_enabled();
}

// Decode the selected texture and save it as a PNG on disk.
//
// Read-only operation — never touches the BF. The modder edits the
// PNG in their tool of choice, loads it back via 'Load image…' and
// clicks 'Add to Project' to record a replace_texture op.
void TextureSwapTab::on_extract() {
    if (!bf_ || current_key_ < 0) return;
    const TexRow* info = nullptr;
    for (const TexRow& r : tex_rows_)
        if (r.key == quint32(current_key_)) { info = &r; break; }
    if (info == nullptr) return;
    const long long idx = current_entry_idx();
    if (idx < 0) return;
    const auto fit = bf_->files.find(quint32(idx));
    const QString ent_part =
        (fit != bf_->files.end() && !fit->second.name.empty())
            ? QStringLiteral("_") + qs(fit->second.name)
            : QString();
    const QString default_name =
        QStringLiteral("texture_%1%2.png")
            .arg(qs(hex_key_lower(quint32(current_key_))), ent_part);
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Extract texture to PNG"), default_name,
        tr("PNG image (*.png);;All Files (*)"));
    if (path.isEmpty()) return;
    try {
        extract_texture_to_png(bf_path_, quint32(idx),
                               quint32(current_key_), path, bf_.get());
    } catch (const std::exception& e) {
        // Python logs f"{type(e).__name__}: {e}" — C++ has no exception
        // class name at hand; the message alone is logged.
        log_->append(tr("  extract failed: %1").arg(e.what()));
        QMessageBox::critical(this, tr("Extract texture"),
                              tr("Could not extract: %1").arg(e.what()));
        return;
    }
    log_->append(tr("\n[->] extracted %1 (%2x%3 fmt%4) to %5")
                     .arg(qs(hex_key_lower(quint32(current_key_))))
                     .arg(info->actual_w)
                     .arg(info->actual_h)
                     .arg(info->format)
                     .arg(path));
}

void TextureSwapTab::on_log(const QString& msg) { log_->append(msg); }
