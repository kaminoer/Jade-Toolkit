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
struct qt_meta_tag_ZN15AssetTableModelE_t {};
} // unnamed namespace

template <> constexpr inline auto AssetTableModel::qt_create_metaobjectdata<qt_meta_tag_ZN15AssetTableModelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "AssetTableModel"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AssetTableModel, qt_meta_tag_ZN15AssetTableModelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject AssetTableModel::staticMetaObject = { {
    QMetaObject::SuperData::link<QAbstractTableModel::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15AssetTableModelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15AssetTableModelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN15AssetTableModelE_t>.metaTypes,
    nullptr
} };

void AssetTableModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AssetTableModel *>(_o);
    (void)_t;
    (void)_c;
    (void)_id;
    (void)_a;
}

const QMetaObject *AssetTableModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AssetTableModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN15AssetTableModelE_t>.strings))
        return static_cast<void*>(this);
    return QAbstractTableModel::qt_metacast(_clname);
}

int AssetTableModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QAbstractTableModel::qt_metacall(_c, _id, _a);
    return _id;
}
namespace {
struct qt_meta_tag_ZN14AssetGridModelE_t {};
} // unnamed namespace

template <> constexpr inline auto AssetGridModel::qt_create_metaobjectdata<qt_meta_tag_ZN14AssetGridModelE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "AssetGridModel"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AssetGridModel, qt_meta_tag_ZN14AssetGridModelE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject AssetGridModel::staticMetaObject = { {
    QMetaObject::SuperData::link<QAbstractListModel::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14AssetGridModelE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14AssetGridModelE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN14AssetGridModelE_t>.metaTypes,
    nullptr
} };

void AssetGridModel::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AssetGridModel *>(_o);
    (void)_t;
    (void)_c;
    (void)_id;
    (void)_a;
}

const QMetaObject *AssetGridModel::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AssetGridModel::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN14AssetGridModelE_t>.strings))
        return static_cast<void*>(this);
    return QAbstractListModel::qt_metacast(_clname);
}

int AssetGridModel::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QAbstractListModel::qt_metacall(_c, _id, _a);
    return _id;
}
namespace {
struct qt_meta_tag_ZN17AssetIndexScannerE_t {};
} // unnamed namespace

template <> constexpr inline auto AssetIndexScanner::qt_create_metaobjectdata<qt_meta_tag_ZN17AssetIndexScannerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "AssetIndexScanner",
        "progress",
        "",
        "current",
        "total",
        "done",
        "error"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'progress'
        QtMocHelpers::SignalData<void(int, int)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { QMetaType::Int, 4 },
        }}),
        // Signal 'done'
        QtMocHelpers::SignalData<void(const QString &)>(5, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 6 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AssetIndexScanner, qt_meta_tag_ZN17AssetIndexScannerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject AssetIndexScanner::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17AssetIndexScannerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17AssetIndexScannerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN17AssetIndexScannerE_t>.metaTypes,
    nullptr
} };

void AssetIndexScanner::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AssetIndexScanner *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->progress((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2]))); break;
        case 1: _t->done((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (AssetIndexScanner::*)(int , int )>(_a, &AssetIndexScanner::progress, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (AssetIndexScanner::*)(const QString & )>(_a, &AssetIndexScanner::done, 1))
            return;
    }
}

const QMetaObject *AssetIndexScanner::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AssetIndexScanner::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN17AssetIndexScannerE_t>.strings))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int AssetIndexScanner::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
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
void AssetIndexScanner::progress(int _t1, int _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}

// SIGNAL 1
void AssetIndexScanner::done(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1);
}
namespace {
struct qt_meta_tag_ZN16AssetEntryLoaderE_t {};
} // unnamed namespace

template <> constexpr inline auto AssetEntryLoader::qt_create_metaobjectdata<qt_meta_tag_ZN16AssetEntryLoaderE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "AssetEntryLoader",
        "loaded",
        "",
        "error"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'loaded'
        QtMocHelpers::SignalData<void(const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AssetEntryLoader, qt_meta_tag_ZN16AssetEntryLoaderE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject AssetEntryLoader::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN16AssetEntryLoaderE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN16AssetEntryLoaderE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN16AssetEntryLoaderE_t>.metaTypes,
    nullptr
} };

void AssetEntryLoader::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AssetEntryLoader *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->loaded((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (AssetEntryLoader::*)(const QString & )>(_a, &AssetEntryLoader::loaded, 0))
            return;
    }
}

const QMetaObject *AssetEntryLoader::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AssetEntryLoader::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN16AssetEntryLoaderE_t>.strings))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int AssetEntryLoader::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QThread::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 1)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 1;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 1)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 1;
    }
    return _id;
}

// SIGNAL 0
void AssetEntryLoader::loaded(const QString & _t1)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1);
}
namespace {
struct qt_meta_tag_ZN20AssetThumbnailWorkerE_t {};
} // unnamed namespace

template <> constexpr inline auto AssetThumbnailWorker::qt_create_metaobjectdata<qt_meta_tag_ZN20AssetThumbnailWorkerE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "AssetThumbnailWorker",
        "thumb_ready",
        "",
        "row",
        "QImage",
        "img"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'thumb_ready'
        QtMocHelpers::SignalData<void(int, const QImage &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { 0x80000000 | 4, 5 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<AssetThumbnailWorker, qt_meta_tag_ZN20AssetThumbnailWorkerE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject AssetThumbnailWorker::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN20AssetThumbnailWorkerE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN20AssetThumbnailWorkerE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN20AssetThumbnailWorkerE_t>.metaTypes,
    nullptr
} };

void AssetThumbnailWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<AssetThumbnailWorker *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->thumb_ready((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QImage>>(_a[2]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (AssetThumbnailWorker::*)(int , const QImage & )>(_a, &AssetThumbnailWorker::thumb_ready, 0))
            return;
    }
}

const QMetaObject *AssetThumbnailWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *AssetThumbnailWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN20AssetThumbnailWorkerE_t>.strings))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int AssetThumbnailWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QThread::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 1)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 1;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 1)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 1;
    }
    return _id;
}

// SIGNAL 0
void AssetThumbnailWorker::thumb_ready(int _t1, const QImage & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}
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
        "scan_finished",
        "on_scan_progress",
        "on_scan_done",
        "err",
        "apply_sidebar_filter",
        "on_filter_changed",
        "apply_filter",
        "on_display_changed",
        "idx",
        "on_table_clicked",
        "QModelIndex",
        "index",
        "on_grid_clicked",
        "on_export_selected",
        "on_ref_clicked",
        "QTreeWidgetItem*",
        "item",
        "col",
        "refresh_modified",
        "defer_viewport_thumbs",
        "enqueue_visible_thumbs",
        "on_thumb_ready",
        "row",
        "QImage",
        "img",
        "on_thumb_done",
        "on_entry_loaded",
        "error",
        "on_loader_done"
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
        // Slot 'on_scan_progress'
        QtMocHelpers::SlotData<void(int, int)>(11, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 8 }, { QMetaType::Int, 9 },
        }}),
        // Slot 'on_scan_done'
        QtMocHelpers::SlotData<void(const QString &)>(12, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 13 },
        }}),
        // Slot 'apply_sidebar_filter'
        QtMocHelpers::SlotData<void()>(14, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_filter_changed'
        QtMocHelpers::SlotData<void()>(15, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'apply_filter'
        QtMocHelpers::SlotData<void()>(16, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_display_changed'
        QtMocHelpers::SlotData<void(int)>(17, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 18 },
        }}),
        // Slot 'on_table_clicked'
        QtMocHelpers::SlotData<void(const QModelIndex &)>(19, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 20, 21 },
        }}),
        // Slot 'on_grid_clicked'
        QtMocHelpers::SlotData<void(const QModelIndex &)>(22, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 20, 21 },
        }}),
        // Slot 'on_export_selected'
        QtMocHelpers::SlotData<void()>(23, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_ref_clicked'
        QtMocHelpers::SlotData<void(QTreeWidgetItem *, int)>(24, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { 0x80000000 | 25, 26 }, { QMetaType::Int, 27 },
        }}),
        // Slot 'refresh_modified'
        QtMocHelpers::SlotData<void()>(28, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'defer_viewport_thumbs'
        QtMocHelpers::SlotData<void()>(29, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'enqueue_visible_thumbs'
        QtMocHelpers::SlotData<void()>(30, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_thumb_ready'
        QtMocHelpers::SlotData<void(int, const QImage &)>(31, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::Int, 32 }, { 0x80000000 | 33, 34 },
        }}),
        // Slot 'on_thumb_done'
        QtMocHelpers::SlotData<void()>(35, 2, QMC::AccessPrivate, QMetaType::Void),
        // Slot 'on_entry_loaded'
        QtMocHelpers::SlotData<void(const QString &)>(36, 2, QMC::AccessPrivate, QMetaType::Void, {{
            { QMetaType::QString, 37 },
        }}),
        // Slot 'on_loader_done'
        QtMocHelpers::SlotData<void()>(38, 2, QMC::AccessPrivate, QMetaType::Void),
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
        case 3: _t->on_scan_progress((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2]))); break;
        case 4: _t->on_scan_done((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 5: _t->apply_sidebar_filter(); break;
        case 6: _t->on_filter_changed(); break;
        case 7: _t->apply_filter(); break;
        case 8: _t->on_display_changed((*reinterpret_cast<std::add_pointer_t<int>>(_a[1]))); break;
        case 9: _t->on_table_clicked((*reinterpret_cast<std::add_pointer_t<QModelIndex>>(_a[1]))); break;
        case 10: _t->on_grid_clicked((*reinterpret_cast<std::add_pointer_t<QModelIndex>>(_a[1]))); break;
        case 11: _t->on_export_selected(); break;
        case 12: _t->on_ref_clicked((*reinterpret_cast<std::add_pointer_t<QTreeWidgetItem*>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<int>>(_a[2]))); break;
        case 13: _t->refresh_modified(); break;
        case 14: _t->defer_viewport_thumbs(); break;
        case 15: _t->enqueue_visible_thumbs(); break;
        case 16: _t->on_thumb_ready((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QImage>>(_a[2]))); break;
        case 17: _t->on_thumb_done(); break;
        case 18: _t->on_entry_loaded((*reinterpret_cast<std::add_pointer_t<QString>>(_a[1]))); break;
        case 19: _t->on_loader_done(); break;
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
        if (_id < 20)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 20;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 20)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 20;
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
