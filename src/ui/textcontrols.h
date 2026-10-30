#pragma once

#include "rendering/textpaintdevice.h"
#include <QAbstractScrollArea>
#include <QAbstractItemView>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QSpinBox>

namespace AppText {
// Redirect only for the duration of the base control's paintEvent. Its native
// editor, hit testing, IME, accessibility and cursor logic are left intact.
template<class Base>
class Control : public Base
{
public:
    using Base::Base;
protected:
    void paintEvent(QPaintEvent* event) override
    {
        QPainter destination(this);
        PaintDevice device(destination);
        m_textDevice = &device;
        Base::paintEvent(event);
        m_textDevice = nullptr;
    }
    QPaintDevice* redirected(QPoint* offset) const override
    {
        if (m_textDevice) { if (offset) *offset = {}; return m_textDevice; }
        return Base::redirected(offset);
    }
    QPainter* sharedPainter() const override
    { return m_textDevice ? nullptr : Base::sharedPainter(); }
private:
    QPaintDevice* m_textDevice = nullptr;
};
using Label = Control<QLabel>;
using LineEdit = Control<QLineEdit>;

template<class Base>
class NumberControl : public Base
{
public:
    explicit NumberControl(QWidget* parent = nullptr) : Base(parent)
    { this->setLineEdit(new LineEdit(this)); }
};
using SpinBox = NumberControl<QSpinBox>;
using DoubleSpinBox = NumberControl<QDoubleSpinBox>;
using DateEdit = NumberControl<QDateEdit>;

// Scroll-area content is painted on the viewport. This also covers Qt-created
// combo popups, item views and rich-text browsers, without a second layout.
class Viewport final : public QWidget
{
public:
    explicit Viewport(QWidget* parent = nullptr) : QWidget(parent) {}
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched != this || event->type() != QEvent::Paint || m_textDevice)
            return false;
        QPainter destination(this);
        PaintDevice device(destination);
        m_textDevice = &device;
        QCoreApplication::sendEvent(this, event);
        m_textDevice = nullptr;
        return true;
    }
protected:
    QPaintDevice* redirected(QPoint* offset) const override
    {
        if (m_textDevice) { if (offset) *offset = {}; return m_textDevice; }
        return QWidget::redirected(offset);
    }
    QPainter* sharedPainter() const override
    { return m_textDevice ? nullptr : QWidget::sharedPainter(); }
private:
    QPaintDevice* m_textDevice = nullptr;
};
inline void installViewport(QAbstractScrollArea* area)
{
    // QScrollArea owns application content under its viewport. Replacing that
    // viewport would delete the page; only adapt surfaces that paint text there.
    if (!qobject_cast<QAbstractItemView*>(area) && !qobject_cast<QPlainTextEdit*>(area)
        && !qobject_cast<QTextEdit*>(area)) return;
    if (dynamic_cast<Viewport*>(area->viewport())) return;
    QWidget* previous = area->viewport();
    auto* viewport = new Viewport(area);
    viewport->setMouseTracking(previous->hasMouseTracking());
    viewport->setAcceptDrops(previous->acceptDrops());
    viewport->setAutoFillBackground(previous->autoFillBackground());
    viewport->setBackgroundRole(previous->backgroundRole());
    viewport->setForegroundRole(previous->foregroundRole());
    viewport->setPalette(previous->palette());
    viewport->setFont(previous->font());
    viewport->setFocusPolicy(previous->focusPolicy());
    viewport->setAttribute(Qt::WA_InputMethodEnabled, previous->testAttribute(Qt::WA_InputMethodEnabled));
    for (auto* child : previous->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) {
        const bool hidden = child->isHidden();
        child->setParent(viewport);
        child->setVisible(!hidden);
    }
    // QTextLayout retains its metric device between paints. Rebind and invalidate
    // it before setViewport deletes the previous metric device.
    QTextDocument* document = nullptr;
    if (auto* editor = qobject_cast<QPlainTextEdit*>(area)) document = editor->document();
    else if (auto* editor = qobject_cast<QTextEdit*>(area)) document = editor->document();
    if (document) {
        document->documentLayout()->setPaintDevice(viewport);
        document->markContentsDirty(0, document->characterCount());
    }
    area->setViewport(viewport);
    viewport->installEventFilter(viewport);
}
}
