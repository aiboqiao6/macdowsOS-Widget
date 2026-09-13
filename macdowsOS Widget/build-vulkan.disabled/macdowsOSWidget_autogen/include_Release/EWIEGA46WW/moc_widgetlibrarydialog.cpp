/****************************************************************************
** Meta object code from reading C++ file 'widgetlibrarydialog.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../widgetlibrarydialog.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'widgetlibrarydialog.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN19WidgetLibraryDialogE_t {};
} // unnamed namespace

template <> constexpr inline auto WidgetLibraryDialog::qt_create_metaobjectdata<qt_meta_tag_ZN19WidgetLibraryDialogE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "WidgetLibraryDialog",
        "widgetDropped",
        "",
        "kind",
        "QPoint",
        "globalPos",
        "widgetDragPreview",
        "visible"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'widgetDropped'
        QtMocHelpers::SignalData<void(int, const QPoint &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { 0x80000000 | 4, 5 },
        }}),
        // Signal 'widgetDragPreview'
        QtMocHelpers::SignalData<void(int, const QPoint &, bool)>(6, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::Int, 3 }, { 0x80000000 | 4, 5 }, { QMetaType::Bool, 7 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<WidgetLibraryDialog, qt_meta_tag_ZN19WidgetLibraryDialogE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject WidgetLibraryDialog::staticMetaObject = { {
    QMetaObject::SuperData::link<LiquidGlassWidget::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN19WidgetLibraryDialogE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN19WidgetLibraryDialogE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN19WidgetLibraryDialogE_t>.metaTypes,
    nullptr
} };

void WidgetLibraryDialog::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<WidgetLibraryDialog *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->widgetDropped((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QPoint>>(_a[2]))); break;
        case 1: _t->widgetDragPreview((*reinterpret_cast<std::add_pointer_t<int>>(_a[1])),(*reinterpret_cast<std::add_pointer_t<QPoint>>(_a[2])),(*reinterpret_cast<std::add_pointer_t<bool>>(_a[3]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (WidgetLibraryDialog::*)(int , const QPoint & )>(_a, &WidgetLibraryDialog::widgetDropped, 0))
            return;
        if (QtMocHelpers::indexOfMethod<void (WidgetLibraryDialog::*)(int , const QPoint & , bool )>(_a, &WidgetLibraryDialog::widgetDragPreview, 1))
            return;
    }
}

const QMetaObject *WidgetLibraryDialog::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *WidgetLibraryDialog::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN19WidgetLibraryDialogE_t>.strings))
        return static_cast<void*>(this);
    return LiquidGlassWidget::qt_metacast(_clname);
}

int WidgetLibraryDialog::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = LiquidGlassWidget::qt_metacall(_c, _id, _a);
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
void WidgetLibraryDialog::widgetDropped(int _t1, const QPoint & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}

// SIGNAL 1
void WidgetLibraryDialog::widgetDragPreview(int _t1, const QPoint & _t2, bool _t3)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 1, nullptr, _t1, _t2, _t3);
}
QT_WARNING_POP
