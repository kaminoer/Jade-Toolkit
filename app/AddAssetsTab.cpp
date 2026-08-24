#include "AddAssetsTab.hpp"

#include <QBuffer>
#include <QByteArray>
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
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTextEdit>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Material.hpp"
#include "jade/ObjectPlacer.hpp"
#include "jade/SubEntry.hpp"
#include "jade/Texture.hpp"

#include "GuiUtil.hpp"
#include "ProjectDoc.hpp"
#include "Theme.hpp"

namespace {

// _FMT_NAMES = {0: 'BGRA', 1: 'PAL8', 5: 'DXT1', 6: 'DXT3', 7: 'DXT5',
//               11: '4bpp'} — .get(fmt, '?')
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

quint32 le32(const uint8_t* d) {
    return quint32(d[0]) | (quint32(d[1]) << 8) | (quint32(d[2]) << 16)
           | (quint32(d[3]) << 24);
}

// _pixmap_from_rgba(rgba)
QPixmap pixmap_from_rgba(const uint8_t* rgba, int w, int h) {
    const QImage img =
        QImage(rgba, w, h, w * 4, QImage::Format_RGBA8888).copy();
    return QPixmap::fromImage(img);
}

// Rich-text tooltip: a title line plus an embedded thumbnail.
//
// Same data-URI pattern as the Mesh Swap materials table — tooltips
// can't reference QPixmaps directly, so the thumb rides along as a
// base64 PNG. (_thumb_html)
QString thumb_html(const QString& title, const QPixmap& pix,
                   int max_side = 128) {
    if (pix.isNull())
        return QStringLiteral("<div>%1<br><i>(no preview)</i></div>")
            .arg(title);
    QPixmap p = pix;
    if (p.width() > max_side || p.height() > max_side)
        p = p.scaled(max_side, max_side, Qt::KeepAspectRatio,
                     Qt::SmoothTransformation);
    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QBuffer::WriteOnly);
    p.save(&buf, "PNG");
    const QString b64 = QString::fromLatin1(ba.toBase64());
    return QStringLiteral(
               "<div>%1<br><img src=\"data:image/png;base64,%2\"></div>")
        .arg(title, b64);
}

// asset_add.is_texture_full(data): a texture entry whose pixdata holds at
// least the base mip (i.e. not a header-only stub).
bool is_texture_full(const uint8_t* d, size_t n) {
    if (!jade::is_texture_entry(d, n)) return false;
    const jade::TexInfo t = jade::parse_texture(d, n);
    if (!t.valid) return false;
    const size_t pixlen = n > t.pix_start ? n - t.pix_start : 0;
    return !jade::is_placeholder(t, pixlen);
}

// asset_add.is_submaterial(sub): a retargetable sub-material record —
// gro_type 5 with a texture-key field at a known kind offset.
bool is_submaterial(const jade::SubEntry& s) {
    return !s.gro_null
           && s.gro_type == quint32(jade::GRO_TYPE_MAT_MULTITEXTURE)
           && jade::resolve_texture_key(s.data.data(), s.data.size()) != 0;
}

// DDS file -> RGBA QImage via the jade reader (tex_mod.read_dds).
QImage load_dds_rgba(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QImage();
    const QByteArray raw = f.readAll();
    const jade::DdsImage img = jade::read_dds(
        reinterpret_cast<const uint8_t*>(raw.constData()), size_t(raw.size()));
    if (!img.ok) return QImage();
    return QImage(img.rgba.data(), int(img.width), int(img.height),
                  int(img.width) * 4, QImage::Format_RGBA8888)
        .copy();
}

// ops_add_asset._load_source_rgba equivalent: QImage handles PNG/JPG/BMP/
// TGA; DDS goes through the jade reader.
QImage load_source_rgba(const QString& path) {
    if (path.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive))
        return load_dds_rgba(path);
    const QImage img(path);
    return img.isNull() ? QImage()
                        : img.convertToFormat(QImage::Format_RGBA8888);
}

// bool(np.any(np.array(img)[..., 3] < 255)) on an RGBA8888 image.
bool image_has_alpha(const QImage& img) {
    for (int y = 0; y < img.height(); ++y) {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < img.width(); ++x)
            if (row[x * 4 + 3] < 255) return true;
    }
    return false;
}

// "0x…"-string member of an op-dict object; false when absent/non-hex.
bool hex_field(const jade::json::Value* obj, const char* key, quint32* out) {
    if (!obj) return false;
    const jade::json::Value* v = obj->find(key);
    if (!v || !v->is_str()) return false;
    bool ok = false;
    const quint32 parsed = QString::fromStdString(v->str).toUInt(&ok, 16);
    if (ok) *out = parsed;
    return ok;
}

}  // namespace

AddAssetsTab::AddAssetsTab(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    bf_label_ = new QLabel(tr("No BigFile loaded."));
    root->addWidget(bf_label_);

    auto* entry_group = new QGroupBox(
        tr("Step 1 — Pick the BF entry that will host the new asset "
           "(filtered to *wow* files)"));
    auto* entry_form = new QFormLayout(entry_group);
    auto* row = new QHBoxLayout();
    entry_combo_ = new QComboBox();
    entry_combo_->setMinimumWidth(420);
    row->addWidget(entry_combo_, 1);
    list_btn_ = new QPushButton(tr("List assets"));
    connect(list_btn_, &QPushButton::clicked, this, &AddAssetsTab::on_list);
    row->addWidget(list_btn_);
    entry_form->addRow(tr("Entry:"), row);
    auto* note = new QLabel(
        tr("A new texture is only resolvable by materials inside the same "
           "bin — each bin streams self-contained. To use the texture in "
           "several zones, add it to each zone's bin."));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    entry_form->addRow(note);
    root->addWidget(entry_group);

    auto* splitter = new QSplitter(Qt::Horizontal);

    // Left: what the bin already ships.
    auto* have_box = new QGroupBox(tr("Already in this bin"));
    auto* have_lay = new QVBoxLayout(have_box);
    tex_tree_ = new QTreeWidget();
    tex_tree_->setHeaderLabels(
        {tr("Texture key"), tr("Dims"), tr("Fmt")});
    tex_tree_->setRootIsDecorated(false);
    tex_tree_->setAlternatingRowColors(true);
    have_lay->addWidget(tex_tree_, 2);
    mat_tree_ = new QTreeWidget();
    mat_tree_->setHeaderLabels(
        {tr("Sub-material key"), tr("Kind"), tr("Texture")});
    mat_tree_->setRootIsDecorated(false);
    mat_tree_->setAlternatingRowColors(true);
    have_lay->addWidget(mat_tree_, 1);
    splitter->addWidget(have_box);

    // Right: the two "new asset" panels.
    auto* right = new QWidget();
    auto* right_lay = new QVBoxLayout(right);
    right_lay->setContentsMargins(0, 0, 0, 0);

    auto* tex_box = new QGroupBox(tr("New texture"));
    auto* tex_lay = new QVBoxLayout(tex_box);
    auto* load_row = new QHBoxLayout();
    new_path_edit_ = new QLineEdit();
    new_path_edit_->setReadOnly(true);
    new_path_edit_->setPlaceholderText(
        tr("Load a PNG / JPG / DDS… (snapped to power-of-two)"));
    load_row->addWidget(new_path_edit_, 1);
    auto* load_btn = new QPushButton(tr("Load image…"));
    connect(load_btn, &QPushButton::clicked, this,
            &AddAssetsTab::on_load_image);
    load_row->addWidget(load_btn);
    tex_lay->addLayout(load_row);

    auto* fmt_row = new QHBoxLayout();
    fmt_row->addWidget(new QLabel(tr("Format:")));
    fmt_combo_ = new QComboBox();
    fmt_combo_->addItem(tr("7 — DXT5 (with alpha)"), QStringLiteral("7"));
    fmt_combo_->addItem(tr("5 — DXT1 (no alpha)"), QStringLiteral("5"));
    fmt_combo_->addItem(tr("0 — BGRA (uncompressed, large)"),
                        QStringLiteral("0"));
    fmt_row->addWidget(fmt_combo_, 1);
    tex_lay->addLayout(fmt_row);

    new_info_ = new QLabel(tr("No image loaded."));
    new_info_->setWordWrap(true);
    tex_lay->addWidget(new_info_);
    new_scroll_ = new QScrollArea();
    new_scroll_->setWidgetResizable(false);
    new_label_ = new QLabel();
    new_label_->setAlignment(Qt::AlignCenter);
    new_scroll_->setWidget(new_label_);
    tex_lay->addWidget(new_scroll_, 1);

    auto* tex_btn_row = new QHBoxLayout();
    tex_btn_row->addStretch(1);
    add_tex_btn_ = new QPushButton(tr("Add texture to Project"));
    add_tex_btn_->setMinimumWidth(180);
    add_tex_btn_->setToolTip(
        tr("Mint a fresh 0x7A key, import the image into the project's "
           "asset store, and record an add_texture operation. The texture "
           "is inserted into the bin at Build time. The minted key is "
           "shown in the log — use it to retarget materials."));
    connect(add_tex_btn_, &QPushButton::clicked, this,
            &AddAssetsTab::on_add_texture);
    tex_btn_row->addWidget(add_tex_btn_);
    tex_lay->addLayout(tex_btn_row);
    right_lay->addWidget(tex_box, 2);

    auto* mat_box =
        new QGroupBox(tr("New material (clone of a shipped sub-material)"));
    auto* mat_form = new QFormLayout(mat_box);
    donor_combo_ = new QComboBox();
    mat_form->addRow(tr("Clone from:"), donor_combo_);
    mat_tex_combo_ = new QComboBox();
    mat_form->addRow(tr("Point at texture:"), mat_tex_combo_);
    auto* mat_btn_row = new QHBoxLayout();
    mat_btn_row->addStretch(1);
    add_mat_btn_ = new QPushButton(tr("Add material to Project"));
    add_mat_btn_->setMinimumWidth(180);
    add_mat_btn_->setToolTip(
        tr("Clone the donor sub-material under a fresh 0x7A key, pointed "
           "at the chosen texture (shipped, or one you just added). "
           "Record an add_material operation; inserted at Build time."));
    connect(add_mat_btn_, &QPushButton::clicked, this,
            &AddAssetsTab::on_add_material);
    mat_btn_row->addWidget(add_mat_btn_);
    mat_form->addRow(mat_btn_row);
    right_lay->addWidget(mat_box, 1);

    auto* apply_row = new QHBoxLayout();
    project_hint_ = new QLabel(
        tr("<i>Open or create a Mod Project (File menu) to record edits.</i>"));
    project_hint_->setStyleSheet(
        QStringLiteral("color: %1;").arg(theme::DIM_TEXT));
    apply_row->addWidget(project_hint_, 1);
    right_lay->addLayout(apply_row);

    splitter->addWidget(right);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    log_ = new QTextEdit();
    log_->setReadOnly(true);
    log_->setFont(QFont(QStringLiteral("Consolas"), 9));
    log_->setMaximumHeight(140);
    root->addWidget(log_);

    update_enabled();
}

// -- public --

void AddAssetsTab::set_bigfile(std::shared_ptr<jade::BigFile> bf,
                               const QString& path) {
    bf_ = std::move(bf);
    bf_path_ = path;
    bf_label_->setText(tr("BigFile: %1").arg(path));
    subs_.clear();
    tex_keys_.clear();
    submat_rows_.clear();
    tex_tooltips_.clear();
    tex_tree_->clear();
    mat_tree_->clear();
    donor_combo_->clear();
    mat_tex_combo_->clear();
    populate_entry_combo();
    log_->append(tr("Loaded BigFile: %1").arg(path));
    update_enabled();
}

void AddAssetsTab::set_project(ProjectDoc* proj) {
    project_ = proj;
    if (proj == nullptr) {
        project_hint_->setText(tr(
            "<i>Open or create a Mod Project (File menu) to record edits.</i>"));
    } else {
        project_hint_->setText(
            tr("<i>Recording into project: <b>%1</b></i>")
                .arg(proj->name.isEmpty() ? tr("(unnamed)") : proj->name));
    }
    update_enabled();
}

void AddAssetsTab::receive_asset(quint32, quint32) {}

// -- helpers --

void AddAssetsTab::populate_entry_combo() {
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
    if (rows.empty()) {
        for (const auto& kv : bf_->files) {
            const QString name = qs(kv.second.name);
            if (!name.isEmpty()) rows.push_back({kv.first, name});
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<quint32, QString>& a,
                 const std::pair<quint32, QString>& b) {
                  return a.second.toLower() < b.second.toLower();
              });
    for (const auto& r : rows)
        entry_combo_->addItem(
            QStringLiteral("%1  (#%2)").arg(r.second).arg(r.first), r.first);
    entry_combo_->blockSignals(false);
}

long long AddAssetsTab::current_entry_idx() const {
    const QVariant v = entry_combo_->currentData();
    return v.isValid() ? (long long)(v.toUInt()) : -1;
}

long long AddAssetsTab::current_entry_key() const {
    const long long idx = current_entry_idx();
    if (!bf_ || idx < 0) return -1;
    const auto it = bf_->files.find(quint32(idx));
    return it == bf_->files.end() ? -1 : (long long)(it->second.key);
}

void AddAssetsTab::update_enabled() {
    const bool has_bf = bf_ != nullptr;
    const bool has_proj = project_ != nullptr && !project_->path.isEmpty();
    const bool listed = !subs_.empty();
    const bool has_img =
        !new_image_path_.isEmpty() && QFile::exists(new_image_path_);
    list_btn_->setEnabled(has_bf);
    add_tex_btn_->setEnabled(has_bf && has_proj && listed && has_img
                             && !tex_keys_.empty());
    add_mat_btn_->setEnabled(has_bf && has_proj && listed
                             && donor_combo_->count() > 0
                             && mat_tex_combo_->count() > 0);
}

std::unordered_set<quint32> AddAssetsTab::used_keys() const {
    std::unordered_set<quint32> used;
    for (const jade::SubEntry& s : subs_) used.insert(s.key);
    if (project_ != nullptr) {
        for (const jade::json::Value& op : project_->operations) {
            // getattr(op, 'new_key', None) — any op type that mints a key
            // carries it as params.new_key in the dict form.
            quint32 nk = 0;
            if (hex_field(op.find("params"), "new_key", &nk)) used.insert(nk);
        }
    }
    return used;
}

std::vector<AddAssetsTab::AddedTexture>
AddAssetsTab::project_added_textures(quint32 entry_key) const {
    std::vector<AddedTexture> out;
    if (project_ == nullptr) return out;
    for (const jade::json::Value& op : project_->operations) {
        const jade::json::Value* op_type = op.find("op");
        if (!op_type || !op_type->is_str() || op_type->str != "add_texture")
            continue;
        const jade::json::Value* en = op.find("enabled");
        if (en && en->type == jade::json::Value::Type::Bool && !en->b)
            continue;
        quint32 op_entry_key = 0;
        if (!hex_field(op.find("target"), "entry_key", &op_entry_key)
            || op_entry_key != entry_key)
            continue;
        quint32 new_key = 0;
        if (!hex_field(op.find("params"), "new_key", &new_key)) continue;
        const jade::json::Value* id = op.find("id");
        AddedTexture at;
        at.key = new_key;
        at.op_id = id && id->is_str() ? qs(id->str) : QString();
        at.label = QStringLiteral("%1  (added by %2)")
                       .arg(qs(hex_key_lower(new_key)), at.op_id);
        out.push_back(std::move(at));
    }
    return out;
}

// -- slots --

void AddAssetsTab::on_list() {
    if (!bf_) return;
    const long long idx = current_entry_idx();
    if (idx < 0) {
        QMessageBox::warning(this, tr("Add New Assets"),
                             tr("Pick a BF entry first."));
        return;
    }
    const auto fit = bf_->files.find(quint32(idx));
    const QString entry_name =
        fit != bf_->files.end() ? qs(fit->second.name) : QString();
    log_->append(QStringLiteral("\n=== Listing assets in entry %1 (%2) ===")
                     .arg(idx)
                     .arg(entry_name));
    subs_.clear();
    tex_keys_.clear();
    submat_rows_.clear();
    tex_tooltips_.clear();
    tex_tree_->clear();
    mat_tree_->clear();

    try {
        const std::vector<uint8_t> raw = bf_->read_data(quint32(idx));
        const jade::LzoResult dec = jade::decompress_lzo(raw);
        if (!dec.ok) throw std::runtime_error("entry did not decompress");
        subs_ = jade::walk_sub_entries(dec.data);
    } catch (const std::exception& e) {
        log_->append(QStringLiteral("  ERROR: %1")
                         .arg(QString::fromUtf8(e.what())));
        update_enabled();
        return;
    }

    std::map<quint32, size_t> seen_full;  // key -> index into subs_
    for (size_t i = 0; i < subs_.size(); ++i) {
        const jade::SubEntry& s = subs_[i];
        const uint8_t* d = s.data.data();
        const size_t n = s.data.size();
        if (is_texture_full(d, n)) {
            seen_full[s.key] = i;
        } else if (is_submaterial(s)) {
            SubmatRow row;
            row.key = s.key;
            row.kind = le32(d);
            row.tex_key = jade::resolve_texture_key(d, n);
            submat_rows_.push_back(row);
        }
    }

    tex_keys_.clear();
    for (const auto& kv : seen_full) tex_keys_.push_back(kv.first);
    for (quint32 k : tex_keys_) {
        const jade::SubEntry& s = subs_[seen_full[k]];
        const jade::TexInfo t =
            jade::parse_texture(s.data.data(), s.data.size());
        const QString title =
            QStringLiteral("%1 &nbsp; %2×%3 %4")
                .arg(qs(hex_key_lower(k)))
                .arg(t.width)
                .arg(t.height)
                .arg(fmt_name(t.format));
        QPixmap pix;
        const std::vector<uint8_t>* pal =
            jade::palette_for_texture(t, subs_);
        const std::vector<uint8_t> rgba = jade::decode_texture(
            s.data.data(), s.data.size(), t, pal ? pal->data() : nullptr,
            pal ? pal->size() : 0);
        if (!rgba.empty())
            pix = pixmap_from_rgba(rgba.data(), int(t.width), int(t.height));
        tex_tooltips_[k] = thumb_html(title, pix);
        auto* item = new QTreeWidgetItem(
            {qs(hex_key_lower(k)),
             QStringLiteral("%1x%2").arg(t.width).arg(t.height),
             QStringLiteral("%1 (%2)").arg(t.format).arg(fmt_name(t.format))});
        for (int col = 0; col < 3; ++col)
            item->setToolTip(col, tex_tooltips_[k]);
        tex_tree_->addTopLevelItem(item);
    }
    for (const SubmatRow& r : submat_rows_) {
        auto* item = new QTreeWidgetItem(
            {qs(hex_key_lower(r.key)), QString::number(r.kind),
             r.tex_key ? qs(hex_key_lower(r.tex_key)) : QStringLiteral("-")});
        const QString tip = submat_tooltip(r.key, r.kind, r.tex_key);
        for (int col = 0; col < 3; ++col) item->setToolTip(col, tip);
        mat_tree_->addTopLevelItem(item);
    }
    tex_tree_->resizeColumnToContents(0);
    mat_tree_->resizeColumnToContents(0);

    refresh_mat_combos();
    log_->append(QStringLiteral("  %1 textures, %2 sub-materials")
                     .arg(tex_keys_.size())
                     .arg(submat_rows_.size()));
    if (tex_keys_.empty()) {
        log_->append(tr(
            "  !! this bin ships no textures — it cannot host a new one "
            "(no wave to insert into); pick another bin"));
    }
    update_enabled();
}

// Material hover preview = the texture it points at. (_submat_tooltip)
QString AddAssetsTab::submat_tooltip(quint32 key, quint32 kind,
                                     quint32 tex_key) const {
    const QString title =
        QStringLiteral("sub-material %1 (kind %2)<br>→ texture %3")
            .arg(qs(hex_key_lower(key)))
            .arg(kind)
            .arg(tex_key ? qs(hex_key_lower(tex_key))
                         : QStringLiteral("(none)"));
    if (tex_tooltips_.contains(tex_key)) {
        // Reuse the texture's tooltip body, swapping in the material
        // title — the html is "<div>title<br><img …></div>".
        const QString tip = tex_tooltips_.value(tex_key);
        const int br = tip.indexOf(QStringLiteral("<br>"));
        const QString body = br < 0 ? tip : tip.mid(br + 4);
        return QStringLiteral("<div>%1<br>%2").arg(title, body);
    }
    if (tex_key)
        return QStringLiteral(
                   "<div>%1<br><i>(texture not in this bin)</i></div>")
            .arg(title);
    return QStringLiteral("<div>%1</div>").arg(title);
}

// Preview for a project-added texture, decoded from its imported source
// image (it isn't in the BF until Build). (_added_texture_tooltip)
QString AddAssetsTab::added_texture_tooltip(quint32 key,
                                            const QString& op_id) const {
    const QString title = QStringLiteral("%1 &nbsp; [new — %2]")
                              .arg(qs(hex_key_lower(key)), op_id);
    QPixmap pix;
    if (project_ != nullptr) {
        // next(o for o in operations if o.new_key == key)
        for (const jade::json::Value& op : project_->operations) {
            const jade::json::Value* prm = op.find("params");
            quint32 nk = 0;
            if (!hex_field(prm, "new_key", &nk) || nk != key) continue;
            const jade::json::Value* src = prm->find("source");
            if (src && src->is_str() && !src->str.empty()) {
                const QString path = project_->resolve_asset(qs(src->str));
                if (!path.isEmpty()) {
                    const QImage rgba = load_source_rgba(path);
                    if (!rgba.isNull()) pix = QPixmap::fromImage(rgba);
                }
            }
            break;
        }
    }
    return thumb_html(title, pix);
}

void AddAssetsTab::refresh_mat_combos() {
    donor_combo_->clear();
    mat_tex_combo_->clear();
    for (const SubmatRow& r : submat_rows_) {
        donor_combo_->addItem(
            QStringLiteral("%1  (kind %2, tex %3)")
                .arg(qs(hex_key_lower(r.key)))
                .arg(r.kind)
                .arg(r.tex_key ? qs(hex_key_lower(r.tex_key))
                               : QStringLiteral("-")),
            r.key);
        donor_combo_->setItemData(donor_combo_->count() - 1,
                                  submat_tooltip(r.key, r.kind, r.tex_key),
                                  Qt::ToolTipRole);
    }
    const long long ek = current_entry_key();
    for (quint32 k : tex_keys_) {
        mat_tex_combo_->addItem(qs(hex_key_lower(k)), k);
        mat_tex_combo_->setItemData(
            mat_tex_combo_->count() - 1,
            tex_tooltips_.value(k, qs(hex_key_lower(k))), Qt::ToolTipRole);
    }
    if (ek >= 0) {
        for (const AddedTexture& at : project_added_textures(quint32(ek))) {
            mat_tex_combo_->addItem(at.label, at.key);
            mat_tex_combo_->setItemData(
                mat_tex_combo_->count() - 1,
                added_texture_tooltip(at.key, at.op_id), Qt::ToolTipRole);
        }
    }
}

void AddAssetsTab::on_load_image() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Load image"), QString(),
        tr("Images (*.png *.jpg *.jpeg *.bmp *.tga *.dds);;All Files (*)"));
    if (path.isEmpty()) return;
    new_image_path_ = path;
    new_path_edit_->setText(path);
    QString fail;
    QImage img;
    if (path.endsWith(QStringLiteral(".dds"), Qt::CaseInsensitive)) {
        img = load_dds_rgba(path);
        if (img.isNull()) fail = tr("unsupported DDS layout");
    } else {
        const QImage raw(path);
        if (raw.isNull())
            fail = tr("could not read image");
        else
            img = raw.convertToFormat(QImage::Format_RGBA8888);
    }
    if (fail.isEmpty()) {
        const int w = img.width(), h = img.height();
        new_image_size_ = QSize(w, h);
        new_label_->setPixmap(QPixmap::fromImage(img));
        new_label_->adjustSize();
        const bool has_alpha = image_has_alpha(img);
        new_info_->setText(
            QStringLiteral("<b>%1</b> &nbsp;|&nbsp; %2×%3%4")
                .arg(QFileInfo(path).fileName())
                .arg(w)
                .arg(h)
                .arg(has_alpha ? QStringLiteral(" &nbsp;|&nbsp; has alpha")
                               : QString()));
        // Preselect a sensible format.
        fmt_combo_->setCurrentIndex(has_alpha ? 0 : 1);
    } else {
        new_info_->setText(tr("Failed to open image: %1").arg(fail));
        new_label_->setPixmap(QPixmap());
        new_image_path_.clear();
    }
    update_enabled();
}

void AddAssetsTab::on_add_texture() {
    const long long ek = current_entry_key();
    if (ek < 0 || project_ == nullptr) return;
    QString err;
    const QString asset_ref = project_->import_asset(new_image_path_, &err);
    if (asset_ref.isEmpty()) {
        QMessageBox::critical(
            this, tr("Add New Assets"),
            tr("Failed to import image into project: %1").arg(err));
        return;
    }
    // Exact core/asset_add.py allocator semantics (distinct from the placement
    // allocator): random.Random(None), randrange starts at one, no counter XOR.
    jade::AssetKeyAllocator alloc(used_keys());
    const quint32 new_key = alloc.next();
    const QString encode = fmt_combo_->currentData().toString();
    const auto fit = bf_->files.find(quint32(current_entry_idx()));
    const QString entry_name =
        fit != bf_->files.end() ? qs(fit->second.name) : QString();
    const QString basename = QFileInfo(new_image_path_).fileName();

    // AddTexture op dict (project/ops_add_asset.py AddTexture):
    //   {"op": "add_texture", "target": {"entry_key": "0x%08x"},
    //    "params": {"new_key": "0x%08x", "source": "asset:<hash>",
    //               "encode": "7"|"5"|"0"}}
    // (id/enabled/created are filled in by ProjectDoc::add_operation.)
    jade::json::Value op = jade::json::make_obj();
    op.obj["op"] = jade::json::make_str("add_texture");
    op.obj["label"] = jade::json::make_str(
        QStringLiteral("new texture %1 in %2 ← %3")
            .arg(qs(hex_key_lower(new_key)), entry_name, basename)
            .toStdString());
    jade::json::Value target = jade::json::make_obj();
    target.obj["entry_key"] =
        jade::json::make_str(hex_key_lower(quint32(ek)));
    op.obj["target"] = std::move(target);
    jade::json::Value params = jade::json::make_obj();
    params.obj["new_key"] = jade::json::make_str(hex_key_lower(new_key));
    params.obj["source"] = jade::json::make_str(asset_ref.toStdString());
    params.obj["encode"] = jade::json::make_str(encode.toStdString());
    op.obj["params"] = std::move(params);
    const QString op_id = project_->add_operation(std::move(op));

    log_->append(
        QStringLiteral("[+] added add_texture %1: entry %2 new key %3 fmt "
                       "%4 ← %5")
            .arg(op_id, qs(hex_key_lower(quint32(ek))),
                 qs(hex_key_lower(new_key)), encode, basename));
    log_->append(
        QStringLiteral("    -> use key %1 to retarget materials "
                       "(Mesh Swap → right-click a material) or create a new "
                       "material below")
            .arg(qs(hex_key_lower(new_key))));
    refresh_mat_combos();
    update_enabled();
}

void AddAssetsTab::on_add_material() {
    const long long ek = current_entry_key();
    if (ek < 0 || project_ == nullptr) return;
    const QVariant donor_data = donor_combo_->currentData();
    const QVariant tex_data = mat_tex_combo_->currentData();
    if (!donor_data.isValid() || !tex_data.isValid()) return;
    const quint32 donor_key = donor_data.toUInt();
    const quint32 tex_key = tex_data.toUInt();
    jade::AssetKeyAllocator alloc(used_keys());
    const quint32 new_key = alloc.next();
    const auto fit = bf_->files.find(quint32(current_entry_idx()));
    const QString entry_name =
        fit != bf_->files.end() ? qs(fit->second.name) : QString();

    // AddMaterial op dict (project/ops_add_asset.py AddMaterial):
    //   {"op": "add_material", "target": {"entry_key": "0x%08x"},
    //    "params": {"new_key": "0x%08x", "texture_key": "0x%08x",
    //               "donor_key": "0x%08x"}}
    jade::json::Value op = jade::json::make_obj();
    op.obj["op"] = jade::json::make_str("add_material");
    op.obj["label"] = jade::json::make_str(
        QStringLiteral("new material %1 → tex %2 in %3")
            .arg(qs(hex_key_lower(new_key)), qs(hex_key_lower(tex_key)),
                 entry_name)
            .toStdString());
    jade::json::Value target = jade::json::make_obj();
    target.obj["entry_key"] =
        jade::json::make_str(hex_key_lower(quint32(ek)));
    op.obj["target"] = std::move(target);
    jade::json::Value params = jade::json::make_obj();
    params.obj["new_key"] = jade::json::make_str(hex_key_lower(new_key));
    params.obj["texture_key"] = jade::json::make_str(hex_key_lower(tex_key));
    params.obj["donor_key"] = jade::json::make_str(hex_key_lower(donor_key));
    op.obj["params"] = std::move(params);
    const QString op_id = project_->add_operation(std::move(op));

    log_->append(
        QStringLiteral("[+] added add_material %1: entry %2 new key %3 "
                       "(donor %4) -> texture %5")
            .arg(op_id, qs(hex_key_lower(quint32(ek))),
                 qs(hex_key_lower(new_key)), qs(hex_key_lower(donor_key)),
                 qs(hex_key_lower(tex_key))));
    update_enabled();
}
