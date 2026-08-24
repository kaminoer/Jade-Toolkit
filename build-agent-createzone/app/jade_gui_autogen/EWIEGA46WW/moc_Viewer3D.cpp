/****************************************************************************
** Meta object code from reading C++ file 'Viewer3D.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../app/Viewer3D.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'Viewer3D.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN14Viewer3DWidgetE_t {};
} // unnamed namespace

template <> constexpr inline auto Viewer3DWidget::qt_create_metaobjectdata<qt_meta_tag_ZN14Viewer3DWidgetE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "Viewer3DWidget",
        "time_updated",
        "",
        "current_time",
        "duration",
        "point_picked",
        "x",
        "y",
        "z",
        "gizmo_moved"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'time_updated'
        QtMocHelpers::SignalData<void(double, double)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 3 }, { QMetaType::Double, 4 },
        }}),
        // Signal 'point_picked'
        QtMocHelpers::SignalData<void(double, double, double)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 6 }, { QMetaType::Double, 7 }, { QMetaType::Double, 8 },
        }}),
        // Signal 'gizmo_moved'
        QtMocHelpers::SignalData<void(double, double, double)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 6 }, { QMetaType::Double, 7 }, { QMetaType::Double, 8 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<Viewer3DWidget, qt_meta_tag_ZN14Viewer3DWidgetE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject Viewer3DWidget::staticMetaObject = { {
    QMetaObject::SuperData::link<QOpenGLWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14Viewer3DWidgetE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14Viewer3DWidgetE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN14Viewer3DWidgetE_t>.metaTypes,
    nullptr
} };

void Viewer3DWidget::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<Viewer3DWidget *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->time_updated((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2]))); break;
        case 1: _t->point_picked((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3]))); break;
        case 2: _t->gizmo_moved((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (Viewer3DWidget::*)(double , double )>(_a, &Viewer3DWidget::time_updated, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (Viewer3DWidget::*)(double , double , double )>(_a, &Viewer3DWidget::point_picked, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (Viewer3DWidget::*)(double , double , double )>(_a, &Viewer3DWidget::gizmo_moved, 2))
            return;
    }
}

const QMetaObject *Viewer3DWidget::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *Viewer3DWidget::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14Viewer3DWidgetE_t>.strings))
        return static_cast<void*>(this);
    if (!strcmp(_clname, "QOpenGLFunctions_3_3_Core"))
        return static_cast< QOpenGLFunctions_3_3_Core*>(this);
    return QOpenGLWidget::qt_metacast(_clname);
}

int Viewer3DWidget::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QOpenGLWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 3)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 3;
    }
    return _id;
}

// SIGNAL 0
void Viewer3DWidget::time_updated(double _t1, double _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}

// SIGNAL 1
void Viewer3DWidget::point_picked(double _t1, double _t2, double _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2, _t3);
}

// SIGNAL 2
void Viewer3DWidget::gizmo_moved(double _t1, double _t2, double _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2, _t3);
}
namespace {
struct qt_meta_tag_ZN13Viewer3DPanelE_t {};
} // unnamed namespace

template <> constexpr inline auto Viewer3DPanel::qt_create_metaobjectdata<qt_meta_tag_ZN13Viewer3DPanelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "Viewer3DPanel",
        "anim_selected",
        "",
        "index",
        "collision_toggled",
        "on",
        "on_play",
        "on_stop",
        "on_tex_toggle",
        "checked",
        "on_grid_toggle",
        "on_frame",
        "on_reset_view",
        "on_home",
        "on_anim_combo_changed",
        "idx",
        "on_slider",
        "value",
        "on_time_updated",
        "current",
        "duration"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'anim_selected'
        QtMocHelpers::SignalData<void(int)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 },
        }}),
        // Signal 'collision_toggled'
        QtMocHelpers::SignalData<void(bool)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 5 },
        }}),
        // Slot 'on_play'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_stop'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_tex_toggle'
        QtMocHelpers::SlotData<void(bool)>(8, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 9 },
        }}),
        // Slot 'on_grid_toggle'
        QtMocHelpers::SlotData<void(bool)>(10, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 9 },
        }}),
        // Slot 'on_frame'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_reset_view'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_home'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_anim_combo_changed'
        QtMocHelpers::SlotData<void(int)>(14, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 15 },
        }}),
        // Slot 'on_slider'
        QtMocHelpers::SlotData<void(int)>(16, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 17 },
        }}),
        // Slot 'on_time_updated'
        QtMocHelpers::SlotData<void(double, double)>(18, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Double, 19 }, { QMetaType::Double, 20 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<Viewer3DPanel, qt_meta_tag_ZN13Viewer3DPanelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject Viewer3DPanel::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13Viewer3DPanelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13Viewer3DPanelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN13Viewer3DPanelE_t>.metaTypes,
    nullptr
} };

void Viewer3DPanel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<Viewer3DPanel *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->anim_selected((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 1: _t->collision_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 2: _t->on_play(); break;
        case 3: _t->on_stop(); break;
        case 4: _t->on_tex_toggle((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 5: _t->on_grid_toggle((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 6: _t->on_frame(); break;
        case 7: _t->on_reset_view(); break;
        case 8: _t->on_home(); break;
        case 9: _t->on_anim_combo_changed((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 10: _t->on_slider((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 11: _t->on_time_updated((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (Viewer3DPanel::*)(int )>(_a, &Viewer3DPanel::anim_selected, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (Viewer3DPanel::*)(bool )>(_a, &Viewer3DPanel::collision_toggled, 1))
            return;
    }
}

const QMetaObject *Viewer3DPanel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *Viewer3DPanel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13Viewer3DPanelE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int Viewer3DPanel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 12)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 12;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 12)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 12;
    }
    return _id;
}

// SIGNAL 0
void Viewer3DPanel::anim_selected(int _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void Viewer3DPanel::collision_toggled(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}
QT_WARNING_POP
