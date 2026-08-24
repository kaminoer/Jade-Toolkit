/****************************************************************************
** Meta object code from reading C++ file 'ProjectPanel.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../app/ProjectPanel.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ProjectPanel.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN12BuildScannerE_t {};
} // unnamed namespace

template <> constexpr inline auto BuildScanner::qt_create_metaobjectdata<qt_meta_tag_ZN12BuildScannerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "BuildScanner",
        "progress",
        "",
        "done",
        "total",
        "phase",
        "result"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'progress'
        QtMocHelpers::SignalData<void(int, int, const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { QMetaType::Int, 4 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'result'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<BuildScanner, qt_meta_tag_ZN12BuildScannerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject BuildScanner::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12BuildScannerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12BuildScannerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN12BuildScannerE_t>.metaTypes,
    nullptr
} };

void BuildScanner::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<BuildScanner *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->progress((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 1: _t->result(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (BuildScanner::*)(int , int , const QString & )>(_a, &BuildScanner::progress, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (BuildScanner::*)()>(_a, &BuildScanner::result, 1))
            return;
    }
}

const QMetaObject *BuildScanner::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *BuildScanner::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12BuildScannerE_t>.strings))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int BuildScanner::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QThread::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 2)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 2;
    }
    return _id;
}

// SIGNAL 0
void BuildScanner::progress(int _t1, int _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3);
}

// SIGNAL 1
void BuildScanner::result()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}
namespace {
struct qt_meta_tag_ZN15ExePatchScannerE_t {};
} // unnamed namespace

template <> constexpr inline auto ExePatchScanner::qt_create_metaobjectdata<qt_meta_tag_ZN15ExePatchScannerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "ExePatchScanner",
        "progress",
        "",
        "done",
        "total",
        "phase",
        "result"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'progress'
        QtMocHelpers::SignalData<void(int, int, const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { QMetaType::Int, 4 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'result'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<ExePatchScanner, qt_meta_tag_ZN15ExePatchScannerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject ExePatchScanner::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15ExePatchScannerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15ExePatchScannerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN15ExePatchScannerE_t>.metaTypes,
    nullptr
} };

void ExePatchScanner::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<ExePatchScanner *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->progress((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 1: _t->result(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (ExePatchScanner::*)(int , int , const QString & )>(_a, &ExePatchScanner::progress, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (ExePatchScanner::*)()>(_a, &ExePatchScanner::result, 1))
            return;
    }
}

const QMetaObject *ExePatchScanner::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ExePatchScanner::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15ExePatchScannerE_t>.strings))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int ExePatchScanner::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QThread::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 2)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 2;
    }
    return _id;
}

// SIGNAL 0
void ExePatchScanner::progress(int _t1, int _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3);
}

// SIGNAL 1
void ExePatchScanner::result()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}
namespace {
struct qt_meta_tag_ZN12JtmodScannerE_t {};
} // unnamed namespace

template <> constexpr inline auto JtmodScanner::qt_create_metaobjectdata<qt_meta_tag_ZN12JtmodScannerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "JtmodScanner",
        "progress",
        "",
        "done",
        "total",
        "phase",
        "result"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'progress'
        QtMocHelpers::SignalData<void(int, int, const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { QMetaType::Int, 4 }, { QMetaType::QString, 5 },
        }}),
        // Signal 'result'
        QtMocHelpers::SignalData<void()>(6, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<JtmodScanner, qt_meta_tag_ZN12JtmodScannerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject JtmodScanner::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12JtmodScannerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12JtmodScannerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN12JtmodScannerE_t>.metaTypes,
    nullptr
} };

void JtmodScanner::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<JtmodScanner *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->progress((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 1: _t->result(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (JtmodScanner::*)(int , int , const QString & )>(_a, &JtmodScanner::progress, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (JtmodScanner::*)()>(_a, &JtmodScanner::result, 1))
            return;
    }
}

const QMetaObject *JtmodScanner::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *JtmodScanner::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12JtmodScannerE_t>.strings))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int JtmodScanner::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QThread::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 2)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 2;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 2)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 2;
    }
    return _id;
}

// SIGNAL 0
void JtmodScanner::progress(int _t1, int _t2, const QString & _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3);
}

// SIGNAL 1
void JtmodScanner::result()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}
namespace {
struct qt_meta_tag_ZN12ProjectPanelE_t {};
} // unnamed namespace

template <> constexpr inline auto ProjectPanel::qt_create_metaobjectdata<qt_meta_tag_ZN12ProjectPanelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "ProjectPanel",
        "refresh",
        "",
        "update_enabled",
        "on_strict_changed",
        "Qt::CheckState",
        "state",
        "on_remove",
        "on_undo",
        "on_redo",
        "on_op_hover",
        "row",
        "col",
        "on_op_context_menu",
        "QPoint",
        "pos",
        "on_build",
        "on_progress",
        "done",
        "total",
        "phase",
        "on_result",
        "on_thread_done",
        "on_make_exe_patch",
        "on_patch_progress",
        "on_patch_result",
        "on_patch_thread_done",
        "on_make_jtmod",
        "on_jtmod_result",
        "on_jtmod_thread_done",
        "on_deploy",
        "on_restore_stock"
    };

    QtMocHelpers::UintData qt_methods {
        // Slot 'refresh'
        QtMocHelpers::SlotData<void()>(1, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'update_enabled'
        QtMocHelpers::SlotData<void()>(3, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_strict_changed'
        QtMocHelpers::SlotData<void(Qt::CheckState)>(4, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 5, 6 },
        }}),
        // Slot 'on_remove'
        QtMocHelpers::SlotData<void()>(7, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_undo'
        QtMocHelpers::SlotData<void()>(8, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_redo'
        QtMocHelpers::SlotData<void()>(9, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_op_hover'
        QtMocHelpers::SlotData<void(int, int)>(10, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 11 }, { QMetaType::Int, 12 },
        }}),
        // Slot 'on_op_context_menu'
        QtMocHelpers::SlotData<void(const QPoint &)>(13, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 14, 15 },
        }}),
        // Slot 'on_build'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_progress'
        QtMocHelpers::SlotData<void(int, int, const QString &)>(17, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 18 }, { QMetaType::Int, 19 }, { QMetaType::QString, 20 },
        }}),
        // Slot 'on_result'
        QtMocHelpers::SlotData<void()>(21, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_thread_done'
        QtMocHelpers::SlotData<void()>(22, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_make_exe_patch'
        QtMocHelpers::SlotData<void()>(23, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_patch_progress'
        QtMocHelpers::SlotData<void(int, int, const QString &)>(24, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 18 }, { QMetaType::Int, 19 }, { QMetaType::QString, 20 },
        }}),
        // Slot 'on_patch_result'
        QtMocHelpers::SlotData<void()>(25, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_patch_thread_done'
        QtMocHelpers::SlotData<void()>(26, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_make_jtmod'
        QtMocHelpers::SlotData<void()>(27, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_jtmod_result'
        QtMocHelpers::SlotData<void()>(28, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_jtmod_thread_done'
        QtMocHelpers::SlotData<void()>(29, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_deploy'
        QtMocHelpers::SlotData<void()>(30, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_restore_stock'
        QtMocHelpers::SlotData<void()>(31, 2, QMC::AccessPrivate, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<ProjectPanel, qt_meta_tag_ZN12ProjectPanelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject ProjectPanel::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12ProjectPanelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12ProjectPanelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN12ProjectPanelE_t>.metaTypes,
    nullptr
} };

void ProjectPanel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<ProjectPanel *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->refresh(); break;
        case 1: _t->update_enabled(); break;
        case 2: _t->on_strict_changed((*reinterpret_cast<std::add_pointer_t<Qt::CheckState>>(_a[1]))); break;
        case 3: _t->on_remove(); break;
        case 4: _t->on_undo(); break;
        case 5: _t->on_redo(); break;
        case 6: _t->on_op_hover((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2]))); break;
        case 7: _t->on_op_context_menu((*reinterpret_cast<std::add_pointer_t<QPoint>>(_a[1]))); break;
        case 8: _t->on_build(); break;
        case 9: _t->on_progress((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 10: _t->on_result(); break;
        case 11: _t->on_thread_done(); break;
        case 12: _t->on_make_exe_patch(); break;
        case 13: _t->on_patch_progress((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[3]))); break;
        case 14: _t->on_patch_result(); break;
        case 15: _t->on_patch_thread_done(); break;
        case 16: _t->on_make_jtmod(); break;
        case 17: _t->on_jtmod_result(); break;
        case 18: _t->on_jtmod_thread_done(); break;
        case 19: _t->on_deploy(); break;
        case 20: _t->on_restore_stock(); break;
        default: ;
        }
    }
}

const QMetaObject *ProjectPanel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ProjectPanel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN12ProjectPanelE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int ProjectPanel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 21)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 21;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 21)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 21;
    }
    return _id;
}
QT_WARNING_POP
