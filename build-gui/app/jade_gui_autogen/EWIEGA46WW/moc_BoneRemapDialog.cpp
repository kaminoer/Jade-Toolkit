/****************************************************************************
** Meta object code from reading C++ file 'BoneRemapDialog.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../app/BoneRemapDialog.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'BoneRemapDialog.hpp' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN15BoneRemapDialogE_t {};
} // unnamed namespace

template <> constexpr inline auto BoneRemapDialog::qt_create_metaobjectdata<qt_meta_tag_ZN15BoneRemapDialogE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "BoneRemapDialog",
        "on_bulk_drop",
        "",
        "on_action_changed",
        "joint",
        "on_target_changed",
        "refresh_diagnostics",
        "refresh_issue_panel",
        "refresh_view",
        "on_view_bone_clicked",
        "bi",
        "on_view_joint_clicked",
        "j",
        "on_left_selected",
        "on_right_selected",
        "apply_filter",
        "on_apply_preset",
        "on_save_preset",
        "on_delete_preset",
        "fold_unmapped_to_nearest",
        "reset_all_to_auto",
        "on_keep_original_skin"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'on_bulk_drop'
        QtMocHelpers::SlotData<void()>(1, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_action_changed'
        QtMocHelpers::SlotData<void(int)>(3, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 4 },
        }}),
        // Slot 'on_target_changed'
        QtMocHelpers::SlotData<void(int)>(5, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 4 },
        }}),
        // Slot 'refresh_diagnostics'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'refresh_issue_panel'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'refresh_view'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_view_bone_clicked'
        QtMocHelpers::SlotData<void(int)>(9, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 10 },
        }}),
        // Slot 'on_view_joint_clicked'
        QtMocHelpers::SlotData<void(int)>(11, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 12 },
        }}),
        // Slot 'on_left_selected'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_right_selected'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'apply_filter'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_apply_preset'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_save_preset'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_delete_preset'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'fold_unmapped_to_nearest'
        QtMocHelpers::SlotData<void()>(19, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'reset_all_to_auto'
        QtMocHelpers::SlotData<void()>(20, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_keep_original_skin'
        QtMocHelpers::SlotData<void()>(21, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<BoneRemapDialog, qt_meta_tag_ZN15BoneRemapDialogE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject BoneRemapDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<QDialog::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15BoneRemapDialogE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15BoneRemapDialogE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN15BoneRemapDialogE_t>.metaTypes,
    nullptr
} };

void BoneRemapDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<BoneRemapDialog *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->on_bulk_drop(); break;
        case 1: _t->on_action_changed((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 2: _t->on_target_changed((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 3: _t->refresh_diagnostics(); break;
        case 4: _t->refresh_issue_panel(); break;
        case 5: _t->refresh_view(); break;
        case 6: _t->on_view_bone_clicked((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 7: _t->on_view_joint_clicked((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 8: _t->on_left_selected(); break;
        case 9: _t->on_right_selected(); break;
        case 10: _t->apply_filter(); break;
        case 11: _t->on_apply_preset(); break;
        case 12: _t->on_save_preset(); break;
        case 13: _t->on_delete_preset(); break;
        case 14: _t->fold_unmapped_to_nearest(); break;
        case 15: _t->reset_all_to_auto(); break;
        case 16: _t->on_keep_original_skin(); break;
        default: ;
        }
    }
}

const QMetaObject *BoneRemapDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *BoneRemapDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15BoneRemapDialogE_t>.strings))
        return static_cast<void*>(this);
    return QDialog::qt_metacast(_clname);
}

int BoneRemapDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QDialog::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 17)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 17;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 17)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 17;
    }
    return _id;
}
QT_WARNING_POP
