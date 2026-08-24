/****************************************************************************
** Meta object code from reading C++ file 'MeshSwapTab.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../app/MeshSwapTab.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'MeshSwapTab.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN23MeshSwapExportGlbWorkerE_t {};
} // unnamed namespace

template <> constexpr inline auto MeshSwapExportGlbWorker::qt_create_metaobjectdata<qt_meta_tag_ZN23MeshSwapExportGlbWorkerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "MeshSwapExportGlbWorker",
        "log",
        "",
        "msg",
        "finished",
        "ok",
        "message",
        "run"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'log'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'finished'
        QtMocHelpers::SignalData<void(bool, const QString &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 5 }, { QMetaType::QString, 6 },
        }}),
        // Slot 'run'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<MeshSwapExportGlbWorker, qt_meta_tag_ZN23MeshSwapExportGlbWorkerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject MeshSwapExportGlbWorker::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN23MeshSwapExportGlbWorkerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN23MeshSwapExportGlbWorkerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN23MeshSwapExportGlbWorkerE_t>.metaTypes,
    nullptr
} };

void MeshSwapExportGlbWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<MeshSwapExportGlbWorker *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->log((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->finished((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 2: _t->run(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (MeshSwapExportGlbWorker::*)(const QString & )>(_a, &MeshSwapExportGlbWorker::log, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (MeshSwapExportGlbWorker::*)(bool , const QString & )>(_a, &MeshSwapExportGlbWorker::finished, 1))
            return;
    }
}

const QMetaObject *MeshSwapExportGlbWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MeshSwapExportGlbWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN23MeshSwapExportGlbWorkerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int MeshSwapExportGlbWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
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
void MeshSwapExportGlbWorker::log(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void MeshSwapExportGlbWorker::finished(bool _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}
namespace {
struct qt_meta_tag_ZN27MeshSwapExportGlbHierWorkerE_t {};
} // unnamed namespace

template <> constexpr inline auto MeshSwapExportGlbHierWorker::qt_create_metaobjectdata<qt_meta_tag_ZN27MeshSwapExportGlbHierWorkerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "MeshSwapExportGlbHierWorker",
        "log",
        "",
        "msg",
        "finished",
        "ok",
        "message",
        "run"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'log'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
        // Signal 'finished'
        QtMocHelpers::SignalData<void(bool, const QString &)>(4, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Bool, 5 }, { QMetaType::QString, 6 },
        }}),
        // Slot 'run'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<MeshSwapExportGlbHierWorker, qt_meta_tag_ZN27MeshSwapExportGlbHierWorkerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject MeshSwapExportGlbHierWorker::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN27MeshSwapExportGlbHierWorkerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN27MeshSwapExportGlbHierWorkerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN27MeshSwapExportGlbHierWorkerE_t>.metaTypes,
    nullptr
} };

void MeshSwapExportGlbHierWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<MeshSwapExportGlbHierWorker *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->log((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 1: _t->finished((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        case 2: _t->run(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (MeshSwapExportGlbHierWorker::*)(const QString & )>(_a, &MeshSwapExportGlbHierWorker::log, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (MeshSwapExportGlbHierWorker::*)(bool , const QString & )>(_a, &MeshSwapExportGlbHierWorker::finished, 1))
            return;
    }
}

const QMetaObject *MeshSwapExportGlbHierWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MeshSwapExportGlbHierWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN27MeshSwapExportGlbHierWorkerE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int MeshSwapExportGlbHierWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
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
void MeshSwapExportGlbHierWorker::log(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}

// SIGNAL 1
void MeshSwapExportGlbHierWorker::finished(bool _t1, const QString & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}
namespace {
struct qt_meta_tag_ZN11MeshSwapTabE_t {};
} // unnamed namespace

template <> constexpr inline auto MeshSwapTab::qt_create_metaobjectdata<qt_meta_tag_ZN11MeshSwapTabE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "MeshSwapTab",
        "send_texture_to_tab",
        "",
        "entry_idx",
        "texture_key",
        "on_list",
        "refresh_materials",
        "on_mat_cell_hover",
        "row",
        "col",
        "on_mat_context_menu",
        "QPoint",
        "pos",
        "on_browse_glb",
        "on_apply",
        "on_stub",
        "on_export_glb",
        "on_export_glb_hier",
        "on_log",
        "msg",
        "on_done",
        "success",
        "message"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'send_texture_to_tab'
        QtMocHelpers::SignalData<void(quint32, quint32)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::UInt, 3 }, { QMetaType::UInt, 4 },
        }}),
        // Slot 'on_list'
        QtMocHelpers::SlotData<void()>(5, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'refresh_materials'
        QtMocHelpers::SlotData<void()>(6, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_mat_cell_hover'
        QtMocHelpers::SlotData<void(int, int)>(7, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 8 }, { QMetaType::Int, 9 },
        }}),
        // Slot 'on_mat_context_menu'
        QtMocHelpers::SlotData<void(const QPoint &)>(10, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 11, 12 },
        }}),
        // Slot 'on_browse_glb'
        QtMocHelpers::SlotData<void()>(13, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_apply'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_stub'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_export_glb'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_export_glb_hier'
        QtMocHelpers::SlotData<void()>(17, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_log'
        QtMocHelpers::SlotData<void(const QString &)>(18, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 19 },
        }}),
        // Slot 'on_done'
        QtMocHelpers::SlotData<void(bool, const QString &)>(20, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Bool, 21 }, { QMetaType::QString, 22 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<MeshSwapTab, qt_meta_tag_ZN11MeshSwapTabE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject MeshSwapTab::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11MeshSwapTabE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11MeshSwapTabE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN11MeshSwapTabE_t>.metaTypes,
    nullptr
} };

void MeshSwapTab::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<MeshSwapTab *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->send_texture_to_tab((*reinterpret_cast<std::add_pointer_t<quint32>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<quint32>>(_a[2]))); break;
        case 1: _t->on_list(); break;
        case 2: _t->refresh_materials(); break;
        case 3: _t->on_mat_cell_hover((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2]))); break;
        case 4: _t->on_mat_context_menu((*reinterpret_cast<std::add_pointer_t<QPoint>>(_a[1]))); break;
        case 5: _t->on_browse_glb(); break;
        case 6: _t->on_apply(); break;
        case 7: _t->on_stub(); break;
        case 8: _t->on_export_glb(); break;
        case 9: _t->on_export_glb_hier(); break;
        case 10: _t->on_log((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 11: _t->on_done((*reinterpret_cast<std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[2]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (MeshSwapTab::*)(quint32 , quint32 )>(_a, &MeshSwapTab::send_texture_to_tab, 0))
            return;
    }
}

const QMetaObject *MeshSwapTab::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *MeshSwapTab::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11MeshSwapTabE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int MeshSwapTab::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void MeshSwapTab::send_texture_to_tab(quint32 _t1, quint32 _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}
QT_WARNING_POP
