/****************************************************************************
** Meta object code from reading C++ file 'PlacementViewport.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../app/PlacementViewport.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'PlacementViewport.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN15PlacementCanvasE_t {};
} // unnamed namespace

template <> constexpr inline auto PlacementCanvas::qt_create_metaobjectdata<qt_meta_tag_ZN15PlacementCanvasE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "PlacementCanvas",
        "point_picked",
        "",
        "x",
        "y",
        "z",
        "gizmo_moved",
        "object_moved",
        "gao_key",
        "object_transformed",
        "object_selected",
        "tick_wasd",
        "sample_fps"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'point_picked'
        QtMocHelpers::SignalData<void(double, double, double)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 3 }, { QMetaType::Double, 4 }, { QMetaType::Double, 5 },
        }}),
        // Signal 'gizmo_moved'
        QtMocHelpers::SignalData<void(double, double, double)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 3 }, { QMetaType::Double, 4 }, { QMetaType::Double, 5 },
        }}),
        // Signal 'object_moved'
        QtMocHelpers::SignalData<void(qlonglong, double, double, double)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::LongLong, 8 }, { QMetaType::Double, 3 }, { QMetaType::Double, 4 }, { QMetaType::Double, 5 },
        }}),
        // Signal 'object_transformed'
        QtMocHelpers::SignalData<void(qlonglong)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::LongLong, 8 },
        }}),
        // Signal 'object_selected'
        QtMocHelpers::SignalData<void(qlonglong)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::LongLong, 8 },
        }}),
        // Slot 'tick_wasd'
        QtMocHelpers::SlotData<void()>(11, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'sample_fps'
        QtMocHelpers::SlotData<void()>(12, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PlacementCanvas, qt_meta_tag_ZN15PlacementCanvasE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject PlacementCanvas::staticMetaObject = { {
    QMetaObject::SuperData::link<QOpenGLWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15PlacementCanvasE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15PlacementCanvasE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN15PlacementCanvasE_t>.metaTypes,
    nullptr
} };

void PlacementCanvas::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PlacementCanvas *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->point_picked((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3]))); break;
        case 1: _t->gizmo_moved((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3]))); break;
        case 2: _t->object_moved((*reinterpret_cast<std::add_pointer_t<qlonglong>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[4]))); break;
        case 3: _t->object_transformed((*reinterpret_cast<std::add_pointer_t<qlonglong>>(_a[1]))); break;
        case 4: _t->object_selected((*reinterpret_cast<std::add_pointer_t<qlonglong>>(_a[1]))); break;
        case 5: _t->tick_wasd(); break;
        case 6: _t->sample_fps(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PlacementCanvas::*)(double , double , double )>(_a, &PlacementCanvas::point_picked, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementCanvas::*)(double , double , double )>(_a, &PlacementCanvas::gizmo_moved, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementCanvas::*)(qlonglong , double , double , double )>(_a, &PlacementCanvas::object_moved, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementCanvas::*)(qlonglong )>(_a, &PlacementCanvas::object_transformed, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementCanvas::*)(qlonglong )>(_a, &PlacementCanvas::object_selected, 4))
            return;
    }
}

const QMetaObject *PlacementCanvas::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PlacementCanvas::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15PlacementCanvasE_t>.strings))
        return static_cast<void*>(this);
    if (!strcmp(_clname, "QOpenGLFunctions_3_3_Core"))
        return static_cast< QOpenGLFunctions_3_3_Core*>(this);
    return QOpenGLWidget::qt_metacast(_clname);
}

int PlacementCanvas::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QOpenGLWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 7)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 7;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 7)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 7;
    }
    return _id;
}

// SIGNAL 0
void PlacementCanvas::point_picked(double _t1, double _t2, double _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3);
}

// SIGNAL 1
void PlacementCanvas::gizmo_moved(double _t1, double _t2, double _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2, _t3);
}

// SIGNAL 2
void PlacementCanvas::object_moved(qlonglong _t1, double _t2, double _t3, double _t4)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2, _t3, _t4);
}

// SIGNAL 3
void PlacementCanvas::object_transformed(qlonglong _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void PlacementCanvas::object_selected(qlonglong _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}
namespace {
struct qt_meta_tag_ZN17PlacementViewportE_t {};
} // unnamed namespace

template <> constexpr inline auto PlacementViewport::qt_create_metaobjectdata<qt_meta_tag_ZN17PlacementViewportE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "PlacementViewport",
        "point_picked",
        "",
        "x",
        "y",
        "z",
        "gizmo_moved",
        "object_moved",
        "gao_key",
        "object_transformed",
        "object_selected",
        "collision_toggled",
        "on",
        "markers_toggled",
        "on_collision_toggle",
        "checked",
        "on_camera_mode_changed",
        "idx"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'point_picked'
        QtMocHelpers::SignalData<void(double, double, double)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 3 }, { QMetaType::Double, 4 }, { QMetaType::Double, 5 },
        }}),
        // Signal 'gizmo_moved'
        QtMocHelpers::SignalData<void(double, double, double)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Double, 3 }, { QMetaType::Double, 4 }, { QMetaType::Double, 5 },
        }}),
        // Signal 'object_moved'
        QtMocHelpers::SignalData<void(qlonglong, double, double, double)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::LongLong, 8 }, { QMetaType::Double, 3 }, { QMetaType::Double, 4 }, { QMetaType::Double, 5 },
        }}),
        // Signal 'object_transformed'
        QtMocHelpers::SignalData<void(qlonglong)>(9, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::LongLong, 8 },
        }}),
        // Signal 'object_selected'
        QtMocHelpers::SignalData<void(qlonglong)>(10, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::LongLong, 8 },
        }}),
        // Signal 'collision_toggled'
        QtMocHelpers::SignalData<void(bool)>(11, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 12 },
        }}),
        // Signal 'markers_toggled'
        QtMocHelpers::SignalData<void(bool)>(13, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 12 },
        }}),
        // Slot 'on_collision_toggle'
        QtMocHelpers::SlotData<void(bool)>(14, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 15 },
        }}),
        // Slot 'on_camera_mode_changed'
        QtMocHelpers::SlotData<void(int)>(16, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 17 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<PlacementViewport, qt_meta_tag_ZN17PlacementViewportE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject PlacementViewport::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17PlacementViewportE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17PlacementViewportE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN17PlacementViewportE_t>.metaTypes,
    nullptr
} };

void PlacementViewport::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<PlacementViewport *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->point_picked((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3]))); break;
        case 1: _t->gizmo_moved((*reinterpret_cast<std::add_pointer_t<double>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3]))); break;
        case 2: _t->object_moved((*reinterpret_cast<std::add_pointer_t<qlonglong>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<double>>(_a[4]))); break;
        case 3: _t->object_transformed((*reinterpret_cast<std::add_pointer_t<qlonglong>>(_a[1]))); break;
        case 4: _t->object_selected((*reinterpret_cast<std::add_pointer_t<qlonglong>>(_a[1]))); break;
        case 5: _t->collision_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 6: _t->markers_toggled((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 7: _t->on_collision_toggle((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1]))); break;
        case 8: _t->on_camera_mode_changed((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (PlacementViewport::*)(double , double , double )>(_a, &PlacementViewport::point_picked, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementViewport::*)(double , double , double )>(_a, &PlacementViewport::gizmo_moved, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementViewport::*)(qlonglong , double , double , double )>(_a, &PlacementViewport::object_moved, 2))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementViewport::*)(qlonglong )>(_a, &PlacementViewport::object_transformed, 3))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementViewport::*)(qlonglong )>(_a, &PlacementViewport::object_selected, 4))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementViewport::*)(bool )>(_a, &PlacementViewport::collision_toggled, 5))
            return;
        if (QtMocHelpers::indexOfMethod<void (PlacementViewport::*)(bool )>(_a, &PlacementViewport::markers_toggled, 6))
            return;
    }
}

const QMetaObject *PlacementViewport::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PlacementViewport::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17PlacementViewportE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int PlacementViewport::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 9)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 9;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 9)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 9;
    }
    return _id;
}

// SIGNAL 0
void PlacementViewport::point_picked(double _t1, double _t2, double _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3);
}

// SIGNAL 1
void PlacementViewport::gizmo_moved(double _t1, double _t2, double _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2, _t3);
}

// SIGNAL 2
void PlacementViewport::object_moved(qlonglong _t1, double _t2, double _t3, double _t4)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 2, nullptr, _t1, _t2, _t3, _t4);
}

// SIGNAL 3
void PlacementViewport::object_transformed(qlonglong _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 3, nullptr, _t1);
}

// SIGNAL 4
void PlacementViewport::object_selected(qlonglong _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 4, nullptr, _t1);
}

// SIGNAL 5
void PlacementViewport::collision_toggled(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 5, nullptr, _t1);
}

// SIGNAL 6
void PlacementViewport::markers_toggled(bool _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 6, nullptr, _t1);
}
QT_WARNING_POP
