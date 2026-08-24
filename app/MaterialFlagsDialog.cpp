#include "MaterialFlagsDialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "jade/Material.hpp"

#include "GuiUtil.hpp"

namespace {

// core.material._MAT_FLAG_BITS / _MAT_BLEND_NAMES / _MAT_COLOROP_NAMES —
// module-private tables in the Python core (the dialog imported them); the
// native jade::decode_render_flags keeps its own private copies, so they are
// duplicated here verbatim.
struct BitName { int bit; const char* name; };

const BitName MAT_FLAG_BITS[] = {
    {0, "TileU"}, {1, "TileV"}, {2, "Bilinear"}, {3, "Trilinear"},
    {4, "AlphaTest"}, {5, "HideAlpha"}, {6, "HideColor"}, {7, "InvertAlpha"},
    {8, "ZEqual"}, {9, "NoZWrite"}, {10, "UseLocalAlpha"}, {11, "Inactive"},
};
const char* const MAT_BLEND_NAMES[] = {
    "Copy", "Alpha", "AlphaPremult", "AlphaDest", "AlphaDestPremult",
    "Add", "Sub", "Glow", "PSX2Shadow", "SpecialContrast",
};
constexpr int N_BLEND_NAMES =
    int(sizeof(MAT_BLEND_NAMES) / sizeof(MAT_BLEND_NAMES[0]));
const char* const MAT_COLOROP_NAMES[] = {
    "Diffuse", "Specular", "Disable", "RLI", "FullLight", "InvertDiffuse",
    "Diffuse2X", "SpecularColor", "DiffuseColor", "ConstantColor",
    "XeAlphaAdd", "XeModulateColor",
};
constexpr int N_COLOROP_NAMES =
    int(sizeof(MAT_COLOROP_NAMES) / sizeof(MAT_COLOROP_NAMES[0]));

// Bits the dialog does NOT edit — preserved from the source value.
constexpr quint32 PRESERVE_MASK = 0x00F00000;  // UVSource (20-23)

// Extra single-bit toggles surfaced beyond the bits 0-11 group: the dynamic
// UV-transform / dynamic-transparency enables (the engine's MATDraw UV
// decompress path reads these; the blend path does not).
const BitName DYN_BITS[] = {
    {30, "UDynamicTransEnable"}, {31, "VDynamicTransEnable"},
};

}  // namespace

MaterialFlagsDialog::MaterialFlagsDialog(quint32 ul_flags,
                                         long long material_key,
                                         QWidget* parent)
    : QDialog(parent), orig_(ul_flags) {
    setWindowTitle(
        material_key >= 0
            ? tr("Render flags — material %1")
                  .arg(qs(hex_key_lower(quint32(material_key))))
            : tr("Render flags"));

    auto* root = new QVBoxLayout(this);

    // ── Blend / colour op dropdowns ───────────────────────────────
    auto* form = new QFormLayout();
    blend_ = new QComboBox();
    for (int i = 0; i < N_BLEND_NAMES; ++i)
        blend_->addItem(QStringLiteral("%1 — %2").arg(i).arg(
                            QLatin1String(MAT_BLEND_NAMES[i])),
                        i);
    const int blend = int((orig_ >> 16) & 0xF);
    blend_->setCurrentIndex(blend < N_BLEND_NAMES ? blend : 0);
    blend_->setToolTip(
        tr("Copy = opaque · Alpha = transparency · Add/Glow = additive.\n"
           "NOTE: shipped static materials are all Copy; FX blend is applied\n"
           "by the runtime sprite system, not the material."));
    form->addRow(tr("Blend mode:"), blend_);

    colorop_ = new QComboBox();
    for (int i = 0; i < N_COLOROP_NAMES; ++i)
        colorop_->addItem(QStringLiteral("%1 — %2").arg(i).arg(
                              QLatin1String(MAT_COLOROP_NAMES[i])),
                          i);
    const int co = int((orig_ >> 12) & 0xF);
    colorop_->setCurrentIndex(co < N_COLOROP_NAMES ? co : 0);
    form->addRow(tr("Colour op:"), colorop_);
    root->addLayout(form);

    // ── Alpha test ────────────────────────────────────────────────
    auto* atrow = new QHBoxLayout();
    alpha_test_ = new QCheckBox(tr("Alpha test (cutout)"));
    alpha_test_->setChecked((orig_ & (1u << 4)) != 0);
    atrow->addWidget(alpha_test_);
    atrow->addWidget(new QLabel(tr("threshold:")));
    thresh_ = new QSpinBox();
    thresh_->setRange(0, 252);
    thresh_->setSingleStep(4);
    thresh_->setValue(int(((orig_ >> 24) & 0x3F) << 2));
    atrow->addWidget(thresh_);
    atrow->addStretch(1);
    root->addLayout(atrow);

    // ── Misc flag bits (everything in bits 0-11 except AlphaTest) ──
    auto* box = new QGroupBox(tr("Flags"));
    auto* grid = new QGridLayout(box);
    int col = 0, row = 0;
    std::vector<BitName> bits(std::begin(MAT_FLAG_BITS),
                              std::end(MAT_FLAG_BITS));
    bits.insert(bits.end(), std::begin(DYN_BITS), std::end(DYN_BITS));
    for (const BitName& bn : bits) {
        if (bn.bit == 4)  // AlphaTest has its own row above
            continue;
        auto* cb = new QCheckBox(QLatin1String(bn.name));
        cb->setChecked((orig_ & (1u << bn.bit)) != 0);
        connect(cb, &QCheckBox::checkStateChanged, this,
                &MaterialFlagsDialog::update_preview);
        bit_checks_[bn.bit] = cb;
        grid->addWidget(cb, row, col);
        col += 1;
        if (col >= 3) {
            col = 0;
            row += 1;
        }
    }
    root->addWidget(box);

    // ── Live preview ──────────────────────────────────────────────
    preview_ = new QLabel();
    preview_->setTextFormat(Qt::RichText);
    root->addWidget(preview_);
    for (QComboBox* w : {blend_, colorop_})
        connect(w, &QComboBox::currentIndexChanged, this,
                &MaterialFlagsDialog::update_preview);
    connect(alpha_test_, &QCheckBox::checkStateChanged, this,
            &MaterialFlagsDialog::update_preview);
    connect(thresh_, &QSpinBox::valueChanged, this,
            &MaterialFlagsDialog::update_preview);
    update_preview();

    // ── Buttons ───────────────────────────────────────────────────
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok
                                    | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);
}

quint32 MaterialFlagsDialog::compute() const {
    quint32 v = orig_ & PRESERVE_MASK;  // keep UV source + dyn-trans
    v |= (quint32(blend_->currentData().toUInt()) & 0xF) << 16;
    v |= (quint32(colorop_->currentData().toUInt()) & 0xF) << 12;
    if (alpha_test_->isChecked()) v |= (1u << 4);
    v |= ((quint32(thresh_->value()) >> 2) & 0x3F) << 24;
    for (const auto& [bit, cb] : bit_checks_)
        if (cb->isChecked()) v |= (1u << bit);
    return v;
}

void MaterialFlagsDialog::update_preview() {
    const quint32 v = compute();
    const jade::MatRenderFlags rf = jade::decode_render_flags(v);
    const QString mode = rf.ok ? qs(rf.mode) : QStringLiteral("?");
    const QString changed = v != orig_ ? QStringLiteral(" · <b>changed</b>")
                                       : QString();
    preview_->setText(
        tr("Result: <b>%1</b> &nbsp; ul_Flags <tt>%2</tt> (was <tt>%3</tt>)%4")
            .arg(mode, hex_key(v), hex_key(orig_), changed));
}

quint32 MaterialFlagsDialog::result_flags() const { return compute(); }
