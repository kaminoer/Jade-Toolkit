// MaterialFlagsDialog.hpp — material render-flags editor (port of
// gui/material_flags_dialog.py): change a (sub-)material's ul_Flags.
//
// A kind-9 material's render mode (opaque / alpha-test / alpha-blend /
// additive / glow) and its individual flag bits live in a single packed
// 32-bit word, ul_Flags (see jade::parse_material_render_flags for the bit
// layout). This modal exposes that word through dropdowns and checkboxes
// and returns the rebuilt value, which the mesh-swap tab records as a
// SetMaterialFlags op.
//
// Only the editable fields are surfaced; the UV-source (bits 20-23) is
// preserved verbatim from the original so the edit never clobbers it. The
// dynamic-transform enables (bits 30-31) ARE exposed as checkboxes — they're
// the scroll/dynamic-transparency toggles and matching them is sometimes the
// whole point of a port (e.g. a SoT material with VDynamicTransEnable set).
#pragma once

#include <QDialog>
#include <map>

class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;

// Edit a material's ul_Flags. result_flags() returns the new 32-bit value
// after the dialog is accepted.
class MaterialFlagsDialog : public QDialog {
    Q_OBJECT
public:
    // material_key < 0 mirrors the Python None (no key in the title).
    explicit MaterialFlagsDialog(quint32 ul_flags, long long material_key = -1,
                                 QWidget* parent = nullptr);

    quint32 result_flags() const;

private slots:
    void update_preview();

private:
    quint32 compute() const;

    quint32 orig_ = 0;
    QComboBox* blend_ = nullptr;
    QComboBox* colorop_ = nullptr;
    QCheckBox* alpha_test_ = nullptr;
    QSpinBox* thresh_ = nullptr;
    std::map<int, QCheckBox*> bit_checks_;
    QLabel* preview_ = nullptr;
};
