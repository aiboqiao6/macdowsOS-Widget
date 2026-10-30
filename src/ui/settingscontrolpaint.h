#pragma once
#include "settingsappearance.h"
#include "rendering/cleartypetext.h"
#include <QCheckBox>
#include <QComboBox>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QEvent>
#include <QFocusEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QStyleOptionSlider>

namespace SettingsAppearance {
inline QRectF selectorRect(const QWidget* window, const QWidget* widget)
{
    const qreal diameter = px(window, "selectorDiameter");
    return {widget->width() - diameter, (widget->height() - diameter) / 2, diameter, diameter};
}

inline QRectF toggleRect(const QWidget* window, const QWidget* widget)
{
    const QSizeF size(px(window, "toggleWidth"), px(window, "toggleHeight"));
    return {QPointF(widget->width() - size.width(), (widget->height() - size.height()) / 2), size};
}

inline QPainterPath focusOutline(const QWidget* window, const QWidget* control)
{
    const qreal stroke = qMax(qreal(1) / window->devicePixelRatioF(), em(window) * number(window, "stroke"));
    QRectF bounds = QRectF(control->rect()).adjusted(stroke, stroke, -stroke, -stroke);
    qreal radius = px(window, "controlRadius");
    const QString kind = control->property("settingsPaint").toString();
    if (kind == QStringLiteral("toggle")) {
        bounds = toggleRect(window, control).adjusted(stroke, stroke, -stroke, -stroke);
        radius = bounds.height() / 2;
    } else if (kind == QStringLiteral("navigation")) {
        radius = px(window, "navigationRadius");
    } else if (kind == QStringLiteral("traffic")) {
        radius = bounds.height() / 2;
    } else if (auto* slider = qobject_cast<const QSlider*>(control)) {
        QStyleOptionSlider option;
        option.initFrom(slider);
        option.orientation = slider->orientation();
        if (option.orientation == Qt::Horizontal) option.state |= QStyle::State_Horizontal;
        option.direction = Qt::LeftToRight;
        option.minimum = slider->minimum();
        option.maximum = slider->maximum();
        option.tickPosition = slider->tickPosition();
        option.tickInterval = slider->tickInterval();
        option.singleStep = slider->singleStep();
        option.pageStep = slider->pageStep();
        option.sliderPosition = slider->sliderPosition();
        option.sliderValue = slider->value();
        option.upsideDown = option.orientation == Qt::Horizontal
            ? slider->invertedAppearance() != (slider->layoutDirection() == Qt::RightToLeft)
            : !slider->invertedAppearance();
        bounds = slider->style()->subControlRect(QStyle::CC_Slider, &option, QStyle::SC_SliderHandle, slider);
        bounds.adjust(stroke, stroke, -stroke, -stroke);
        radius = qMin(bounds.width(), bounds.height()) / 2;
    } else if (auto* combo = qobject_cast<const QComboBox*>(control)) {
        const QRectF selector = selectorRect(window, control);
        const qreal textRight = selector.left() - px(window, "controlGap");
        const qreal textWidth = qMin(qMax(qreal(0), textRight),
            QFontMetricsF(combo->font()).horizontalAdvance(combo->currentText()));
        bounds.setLeft(qMax(stroke, textRight - textWidth - px(window, "controlInset")));
    } else if (auto* number = qobject_cast<const QAbstractSpinBox*>(control)) {
        if (auto* editor = number->findChild<QLineEdit*>()) {
            const qreal textWidth = QFontMetricsF(editor->font()).horizontalAdvance(editor->displayText());
            bounds.setLeft(qMax(stroke, editor->geometry().right() - textWidth - px(window, "controlInset")));
        }
    } else if (kind == QStringLiteral("history")) {
        radius = bounds.height() / 2;
    }
    QPainterPath path;
    if (!bounds.isValid()) return path;
    path.addRoundedRect(bounds, radius, radius);
    return path;
}

inline void paintFocus(QPainter& painter, QWidget* window, QWidget* control)
{
    QWidget* focused = QApplication::focusWidget();
    if (!control->isEnabled() || !(control->hasFocus() || (focused && control->isAncestorOf(focused)))) return;
    if (control->property("traffic").toBool() && !control->property("settingsKeyboardFocus").toBool()) return;
    const bool selected = control->property("navigation").toBool()
        && qobject_cast<QPushButton*>(control)->isChecked();
    painter.setPen(QPen(color(window, selected ? "onAccent" : "accent"),
        qMax(qreal(1) / window->devicePixelRatioF(), em(window) * number(window, "stroke"))));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(focusOutline(window, control));
}

// Geometry only: native Qt controls retain their models, keyboard input and accessibility.
inline void paintControl(QWidget* window, QWidget* widget)
{
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setFont(widget->font());
    const QString kind = widget->property("settingsPaint").toString();
    const qreal stroke = qMax(qreal(1) / window->devicePixelRatioF(), em(window) * number(window, "stroke"));
    const auto ink = [&](const char* role) {
        QColor c = color(window, role);
        if (!widget->isEnabled()) c.setAlphaF(c.alphaF() / 2);
        return c;
    };
    const auto chevron = [&](QPointF center, qreal extent, int direction, const QColor& c, bool vertical) {
        painter.setPen(QPen(c, stroke * 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        QPolygonF points;
        if (vertical) points = {center + QPointF(-extent / 2, -direction * extent / 4),
            center + QPointF(0, direction * extent / 4), center + QPointF(extent / 2, -direction * extent / 4)};
        else points = {center + QPointF(-direction * extent / 4, -extent / 2),
            center + QPointF(direction * extent / 4, 0), center + QPointF(-direction * extent / 4, extent / 2)};
        painter.drawPolyline(points);
    };
    const auto textAt = [&](QRectF area, const QString& text, bool rightAligned, const QColor& c) {
        const QFontMetricsF metrics(painter.font());
        const QString visible = metrics.elidedText(text, Qt::ElideRight, qMax(qreal(0), area.width()));
        const qreal x = rightAligned ? area.right() - metrics.horizontalAdvance(visible) : area.left();
        const QPointF baseline(x, area.center().y() + (metrics.ascent() - metrics.descent()) / 2);
        painter.setPen(c);
        ClearTypeText::draw(painter, baseline, visible);
    };
    if (kind == QStringLiteral("history")) {
        if (qobject_cast<QPushButton*>(widget)->isDown()) {
            painter.fillPath(focusOutline(window, widget), color(window, "hover"));
        }
        QColor c = color(window, "foreground");
        if (!widget->isEnabled()) c.setAlpha(65);
        const qreal extent = em(window) * number(window, "historyGlyph");
        const int direction = widget->property("historyDirection").toInt();
        painter.setPen(QPen(c, stroke * 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const QPointF center = QRectF(widget->rect()).center();
        const QPolygonF points = {center + QPointF(-direction * extent / 4, -extent / 2),
            center + QPointF(direction * extent / 4, 0), center + QPointF(-direction * extent / 4, extent / 2)};
        painter.drawPolyline(points);
    } else if (kind == QStringLiteral("selector") || kind == QStringLiteral("stepper")) {
        auto* combo = qobject_cast<QComboBox*>(widget);
        const QRectF circle = selectorRect(window, widget);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink("selector"));
        painter.drawEllipse(circle);
        const qreal glyph = em(window) * number(window, "selectorGlyph");
        chevron(circle.center() - QPointF(0, glyph / 2), glyph, -1, ink("foreground"), true);
        chevron(circle.center() + QPointF(0, glyph / 2), glyph, 1, ink("foreground"), true);
        QRectF textRect(widget->rect());
        textRect.setRight(circle.left() - px(window, "controlGap"));
        if (combo) textAt(textRect, combo->currentText(), true, ink("foreground"));
    } else if (kind == QStringLiteral("toggle")) {
        auto* checkbox = qobject_cast<QCheckBox*>(widget);
        const QRectF track = toggleRect(window, widget);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink(checkbox->isChecked() ? "accent" : "toggleOff"));
        painter.drawRoundedRect(track, track.height() / 2, track.height() / 2);
        const qreal inset = em(window) * number(window, "toggleInset");
        const qreal knob = track.height() - inset * 2;
        const qreal knobWidth = qMin(track.width() - inset * 2,
            em(window) * number(window, "toggleKnobWidth"));
        if (checkbox->isChecked()) {
            painter.setPen(QPen(ink("onAccent"), stroke * 2, Qt::SolidLine, Qt::RoundCap));
            const qreal x = track.left() + (track.width() - knobWidth - inset) / 2;
            painter.drawLine(QPointF(x, track.center().y() - knob * .18),
                QPointF(x, track.center().y() + knob * .18));
        }
        const qreal x = checkbox->isChecked() ? track.right() - knobWidth - inset : track.left() + inset;
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink("toggleKnob"));
        painter.drawRoundedRect(QRectF(x, track.top() + inset, knobWidth, knob), knob / 2, knob / 2);
    } else if (kind == QStringLiteral("navigation")) {
        auto* button = qobject_cast<QPushButton*>(widget);
        const bool checked = button->isChecked();
        painter.setPen(Qt::NoPen);
        QColor fill = checked ? color(window, "accent") : button->underMouse() ? color(window, "hover") : QColor(Qt::transparent);
        if (button->isDown()) fill = checked ? fill.darker(115) : color(window, "hover");
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(widget->rect()), px(window, "navigationRadius"), px(window, "navigationRadius"));
        const qreal inset = px(window, "navigationInset"), extent = px(window, "navigationIcon");
        const QRectF icon(inset, (widget->height() - extent) / 2, extent, extent);
        button->icon().paint(&painter, icon.toRect());
        QRectF copy(widget->rect());
        copy.setLeft(icon.right() + px(window, "controlGap"));
        copy.setRight(copy.right() - inset);
        QFont textFont = button->font(); textFont.setWeight(checked ? QFont::DemiBold : QFont::Medium);
        painter.setFont(textFont);
        const QColor navigationInk = button->property("settingsNavigationInk").value<QColor>();
        textAt(copy, button->text(), false, checked ? color(window, "onAccent")
            : navigationInk.isValid() ? navigationInk
            : color(window, window->property("settingsDark").toBool() ? "navigationLight" : "navigationDark"));
    } else if (kind == QStringLiteral("traffic")) {
        const char* roles[] = {"trafficClose", "trafficMinimize", "trafficZoom"};
        const int index = widget->property("trafficIndex").toInt();
        const bool pressed = qobject_cast<QPushButton*>(widget)->isDown();
        const QColor base = color(window, roles[qBound(0, index, 2)]);
        const qreal inset = stroke / 2 + (pressed ? qMax(stroke, em(window) * .04) : 0);
        const QRectF circle = QRectF(widget->rect()).adjusted(inset, inset, -inset, -inset);
        painter.setPen(Qt::NoPen);
        painter.setBrush(pressed ? base.darker(130) : base);
        painter.drawEllipse(circle);
        if (widget->underMouse() || pressed || (widget->hasFocus() && widget->property("settingsKeyboardFocus").toBool())) {
            painter.setPen(QPen(base.darker(220), stroke, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = circle.center(); const qreal d = circle.width() / 5;
            if (index == 0) {painter.drawLine(c + QPointF(-d, -d), c + QPointF(d, d)); painter.drawLine(c + QPointF(-d, d), c + QPointF(d, -d));}
            else {painter.drawLine(c + QPointF(-d, 0), c + QPointF(d, 0)); if (index == 2) painter.drawLine(c + QPointF(0, -d), c + QPointF(0, d));}
        }
    }
    paintFocus(painter, window, widget);
}

class PaintButton final : public QPushButton
{
public:
    PaintButton(QWidget* theme, QWidget* parent) : QPushButton(parent), m_theme(theme) {}
protected:
    void paintEvent(QPaintEvent*) override { paintControl(m_theme, this); }
    void focusInEvent(QFocusEvent* event) override
    {
        setProperty("settingsKeyboardFocus", event->reason() == Qt::TabFocusReason
            || event->reason() == Qt::BacktabFocusReason || event->reason() == Qt::ShortcutFocusReason);
        QPushButton::focusInEvent(event);
    }
    bool hitButton(const QPoint& position) const override
    {
        if (!property("traffic").toBool()) return QPushButton::hitButton(position);
        QPainterPath circle;
        circle.addEllipse(QRectF(rect()));
        return circle.contains(position);
    }
private:
    QWidget* m_theme;
};

// Numeric fields and sliders retain native painting and input. This transparent
// overlay follows their actual value/handle rather than framing empty row space.
class FocusIndicator final : public QWidget
{
public:
    FocusIndicator(QWidget* theme, QWidget* control) : QWidget(control), m_theme(theme)
    {
        setObjectName(QStringLiteral("settingsFocusIndicator"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setGeometry(control->rect());
        control->installEventFilter(this);
        if (auto* editor = control->findChild<QLineEdit*>()) {
            editor->installEventFilter(this);
            connect(editor, &QLineEdit::textChanged, this, [this] { update(); });
        }
        if (auto* slider = qobject_cast<QSlider*>(control))
            connect(slider, &QSlider::valueChanged, this, [this] { update(); });
        raise();
        show();
    }
protected:
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (object == parentWidget() && event->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
            raise();
        }
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut
            || event->type() == QEvent::EnabledChange || event->type() == QEvent::FontChange) update();
        return QWidget::eventFilter(object, event);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        paintFocus(painter, m_theme, parentWidget());
    }
private:
    QWidget* m_theme;
};

class StepperIndicator final : public QWidget
{
public:
    StepperIndicator(QWidget* theme, QWidget* parent) : QWidget(parent), m_theme(theme)
    {
        setObjectName(QStringLiteral("settingsStepperIndicator"));
        setProperty("settingsPaint", QStringLiteral("stepper"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
    }
protected:
    void paintEvent(QPaintEvent*) override { paintControl(m_theme, this); }
private:
    QWidget* m_theme;
};

}
