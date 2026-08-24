/****************************************************************************
** Meta object code from reading C++ file 'PlacementTab.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../app/PlacementTab.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'PlacementTab.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN12PlacementTabE_t {};
} // unnamed namespace

template <> constexpr inline auto PlacementTab::qt_create_metaobjectdata<qt_meta_tag_ZN12PlacementTabE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "PlacementTab",
        "apply_filter",
        "",
        "on_entry_clicked",
        "QTreeWidgetItem*",
        "item",
        "column",
        "on_kind_changed",
        "on_pick_prim_color",
        "on_clear_prim_color",
        "on_collision_check_changed",
        "checked",
        "on_add_pending_object",
        "on_inspector_edited",
        "on_inspector_frame",
        "on_inspector_material_picked",
        "index",
        "on_flag_toggled",
        "on_light_edited",
        "on_add_collision_clicked",
        "on_commit_map",
        "on_discard_map",
        "on_viewer_object_selected",
        "gao_key",
        "on_viewer_object_transformed",
        "on_viewer_point_picked",
        "x",
        "y",
        "z",
        "on_viewer_gizmo_moved",
        "on_collision_preview_toggled",
        "on_markers_toggled",
        "on_replace_from_changed",
        "on_load_source_entry",
        "on_browse_model",
        "on_source_filter_changed",
        "text",
        "on_source_combo_user_picked",
        "on_draft_control_changed",
        "on_export_gao",
        "on_import_gao",
        "on_jgao_to_glb",
        "on_glb_to_jgao"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'apply_filter'
        QtMocHelpers::SlotData<void()>(1, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_entry_clicked'
        QtMocHelpers::SlotData<void(QTreeWidgetItem *, int)>(3, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 4, 5 }, { QMetaType::Int, 6 },
        }}),
        // Slot 'on_kind_changed'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_pick_prim_color'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_clear_prim_color'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_collision_check_changed'
        QtMocHelpers::SlotData<void(bool)>(10, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 11 },
        }}),
        // Slot 'on_add_pending_object'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_inspector_edited'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_inspector_frame'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_inspector_material_picked'
        QtMocHelpers::SlotData<void(int)>(15, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 16 },
        }}),
        // Slot 'on_flag_toggled'
        QtMocHelpers::SlotData<void(bool)>(17, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 11 },
        }}),
        // Slot 'on_light_edited'
        QtMocHelpers::SlotData<void()>(18, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_add_collision_clicked'
        QtMocHelpers::SlotData<void()>(19, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_commit_map'
        QtMocHelpers::SlotData<void()>(20, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_discard_map'
        QtMocHelpers::SlotData<void()>(21, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_viewer_object_selected'
        QtMocHelpers::SlotData<void(qlonglong)>(22, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::LongLong, 23 },
        }}),
        // Slot 'on_viewer_object_transformed'
        QtMocHelpers::SlotData<void(qlonglong)>(24, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::LongLong, 23 },
        }}),
        // Slot 'on_viewer_point_picked'
        QtMocHelpers::SlotData<void(double, double, double)>(25, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Double, 26 }, { QMetaType::Double, 27 }, { QMetaType::Double, 28 },
        }}),
        // Slot 'on_viewer_gizmo_moved'
        QtMocHelpers::SlotData<void(double, double, double)>(29, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Double, 26 }, { QMetaType::Double, 27 }, { QMetaType::Double, 28 },
        }}),
        // Slot 'on_collision_preview_toggled'
        QtMocHelpers::SlotData<void(bool)>(30, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 11 },
        }}),
        // Slot 'on_markers_toggled'
        QtMocHelpers::SlotData<void(bool)>(31, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 11 },
        }}),
        // Slot 'on_replace_from_changed'
        QtMocHelpers::SlotData<void()>(32, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_load_source_entry'
        QtMocHelpers::SlotData<void()>(33, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_browse_model'
        QtMocHelpers::SlotData<void()>(34, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_source_filter_changed'
        QtMocHelpers::SlotData<void(const QString &)>(35, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 36 },
        }}),
        // Slot 'on_source_combo_user_picked'
        QtMocHelpers::SlotData<void(int)>(37, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 16 },
        }}),
        // Slot 'on_draft_control_changed'
        QtMocHelpers::SlotData<void()>(38, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_export_gao'
        QtMocHelpers::SlotData<void()>(39, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_import_gao'
        QtMocHelpers::SlotData<void()>(40, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_jgao_to_glb'
        QtMocHelpers::SlotData<void()>(41, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_glb_to_jgao'
        QtMocHelpers::SlotData<void()>(42, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PlacementTab, qt_meta_tag_ZN12PlacementTabE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject PlacementTab::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12PlacementTabE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12PlacementTabE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN12PlacementTabE_t>.metaTypes,
    nullptr
} };

void PlacementTab::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PlacementTab *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->apply_filter(); break;
        case 1: _t->on_entry_clicked((*reinterpret_cast<std::add_pointer_t<QTreeWidgetItem*>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2]))); break;
        case 2: _t->on_kind_changed(); break;
        case 3: _t->on_pick_prim_color(); break;
        case 4: _t->on_clear_prim_color(); break;
        case 5: _t->on_collision_check_changed((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 6: _t->on_add_pending_object(); break;
        case 7: _t->on_inspector_edited(); break;
        case 8: _t->on_inspector_frame(); break;
        case 9: _t->on_inspector_material_picked((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 10: _t->on_flag_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 11: _t->on_light_edited(); break;
        case 12: _t->on_add_collision_clicked(); break;
        case 13: _t->on_commit_map(); break;
        case 14: _t->on_discard_map(); break;
        case 15: _t->on_viewer_object_selected((*reinterpret_cast<std::add_pointer_t<qlonglong>>(_a[1]))); break;
        case 16: _t->on_viewer_object_transformed((*reinterpret_cast<std::add_pointer_t<qlonglong>>(_a[1]))); break;
        case 17: _t->on_viewer_point_picked((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3]))); break;
        case 18: _t->on_viewer_gizmo_moved((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3]))); break;
        case 19: _t->on_collision_preview_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 20: _t->on_markers_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 21: _t->on_replace_from_changed(); break;
        case 22: _t->on_load_source_entry(); break;
        case 23: _t->on_browse_model(); break;
        case 24: _t->on_source_filter_changed((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 25: _t->on_source_combo_user_picked((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 26: _t->on_draft_control_changed(); break;
        case 27: _t->on_export_gao(); break;
        case 28: _t->on_import_gao(); break;
        case 29: _t->on_jgao_to_glb(); break;
        case 30: _t->on_glb_to_jgao(); break;
        default: ;
        }
    }
}

const QMetaObject *PlacementTab::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PlacementTab::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12PlacementTabE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int PlacementTab::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 31)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 31;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 31)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 31;
    }
    return _id;
}
QT_WARNING_POP
