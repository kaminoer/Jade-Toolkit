/****************************************************************************
** Meta object code from reading C++ file 'AssetBrowserTab.hpp'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../app/AssetBrowserTab.hpp"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'AssetBrowserTab.hpp' doesn't include <QObject>."
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
struct qt_meta_tag_ZN15AssetBrowserTabE_t {};
} // unnamed namespace

template <> constexpr inline auto AssetBrowserTab::qt_create_metaobjectdata<qt_meta_tag_ZN15AssetBrowserTabE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "AssetBrowserTab",
        "send_asset_to_tab",
        "",
        "target",
        "parent_index",
        "key",
        "category",
        "scan_progress",
        "current",
        "total",
        "scan_finished"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'send_asset_to_tab'
        QtMocHelpers::SignalData<void(const QString &, quint32, quint32, const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 }, { QMetaType::UInt, 4 }, { QMetaType::UInt, 5 }, { QMetaType::QString, 6 },
        }}),
        // Signal 'scan_progress'
        QtMocHelpers::SignalData<void(int, int)>(7, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 8 }, { QMetaType::Int, 9 },
        }}),
        // Signal 'scan_finished'
        QtMocHelpers::SignalData<void()>(10, 2, QMC::AccessPublic, QMetaType::Void),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AssetBrowserTab, qt_meta_tag_ZN15AssetBrowserTabE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject AssetBrowserTab::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15AssetBrowserTabE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15AssetBrowserTabE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN15AssetBrowserTabE_t>.metaTypes,
    nullptr
} };

void AssetBrowserTab::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AssetBrowserTab *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->send_asset_to_tab((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<quint32>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<quint32>>(_a[3])),(*reinterpret_cast<std::add_pointer_t<QString>>(_a[4]))); break;
        case 1: _t->scan_progress((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2]))); break;
        case 2: _t->scan_finished(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (AssetBrowserTab::*)(const QString & , quint32 , quint32 , const QString & )>(_a, &AssetBrowserTab::send_asset_to_tab, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (AssetBrowserTab::*)(int , int )>(_a, &AssetBrowserTab::scan_progress, 1))
            return;
        if (QtMocHelpers::indexOfMethod<void (AssetBrowserTab::*)()>(_a, &AssetBrowserTab::scan_finished, 2))
            return;
    }
}

const QMetaObject *AssetBrowserTab::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AssetBrowserTab::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15AssetBrowserTabE_t>.strings))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int AssetBrowserTab::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
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
void AssetBrowserTab::send_asset_to_tab(const QString & _t1, quint32 _t2, quint32 _t3, const QString & _t4)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2, _t3, _t4);
}

// SIGNAL 1
void AssetBrowserTab::scan_progress(int _t1, int _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2);
}

// SIGNAL 2
void AssetBrowserTab::scan_finished()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}
QT_WARNING_POP
