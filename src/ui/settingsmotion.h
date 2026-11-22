#pragma once

#include "settingsappearance.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QEasingCurve>
#include <QEvent>
#include <QScrollBar>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWidget>
#include <functional>

// Settings motion. Durations and curves are measured from the reference recording
// of the macOS-style settings window: a page change is a pure cross-fade with no
// travel in roughly 110 ms, pointer feedback on the rail fades in about 180 ms,
// and wheel scrolling glides to a stop without overshooting. Nothing here animates
// geometry that Qt lays out, so native controls keep their own metrics, focus
// chain and accessibility.
//
// `motionSpeed` in the theme scales every duration; 0 turns animation off and is
// the only value that makes the window behave like the pre-animation build.
namespace SettingsAppearance {
namespace Motion {
// Milliseconds at speed 1.
constexpr int page = 130;       // outgoing page dissolve on a page change
constexpr int hover = 180;      // pointer entering or leaving a control
constexpr int press = 90;       // pointer down on a control
constexpr int state = 180;      // checked/unchecked, selection ink and toggle knob
constexpr int scroll = 260;     // shortest wheel glide
constexpr int scrollLongest = 420;
constexpr qreal scrollPerPixel = 0.5;   // extra glide time per scrolled pixel
// Select popup. The real-device reference is a spring with an initial velocity,
// not a tween: the panel is already half way within 40 ms, overshoots the final
// size by about 2 % around 120 ms and settles by roughly 250 ms. The initial
// velocity is what gives the fast attack; the under-damped ratio gives the
// rebound. Numbers below come from integrating this exact step response.
constexpr qreal popupScaleFrom = 0.60;        // grows from 60 % of its final size
constexpr qreal popupStiffnessIn = 450;       // presentation spring (slower, same shape)
constexpr qreal popupDampingIn = 26.5;        // zeta ~ 0.62 -> ~3 % overshoot at ~170 ms, settles by ~470 ms
constexpr qreal popupInitialVelocity = 5.0;   // normalized units per second
constexpr qreal popupStiffnessOut = 1200;     // dismissal spring, critically damped
constexpr qreal popupDampingOut = 70;
constexpr qreal popupOpacityBias = 0.10;      // opacity floor at the first frame
constexpr qreal popupOpacityGain = 0.90;      // opacity completes before the scale settles
}

inline qreal motionSpeed(const QWidget* window)
{
    const qreal value = values(window).value(QStringLiteral("motionSpeed"), 1).toDouble();
    return std::isfinite(value) ? qBound(qreal(0), value, qreal(4)) : 1;
}

// One duration knob for the whole window. Zero disables animation outright.
inline int motion(const QWidget* window, int milliseconds)
{
    const qreal speed = motionSpeed(window);
    return speed <= 0 ? 0 : qMax(1, qRound(milliseconds * speed));
}

// A single eased scalar driving repaints of one widget. Tweens read their value
// from paint code, so an in-flight animation costs one widget update per frame and
// nothing else; interrupted tweens continue from the value on screen.
class Tween final : public QObject
{
public:
    explicit Tween(QWidget* host, qreal value = 0) : QObject(host), m_host(host), m_value(value) {}

    void to(qreal target)
    {
        run(target, m_duration, m_curve);
    }
    void to(qreal target, int milliseconds, const QEasingCurve& curve)
    {
        m_duration = milliseconds;
        m_curve = curve;
        run(target, milliseconds, curve);
    }
    void snap(qreal target)
    {
        if (m_animation) m_animation->stop();
        m_value = target;
        if (m_host) m_host->update();
    }
    qreal value() const { return m_value; }

    std::function<void()> finished;

private:
    void run(qreal target, int milliseconds, const QEasingCurve& curve)
    {
        if (!m_host) return;
        // An interrupted tween must never keep running towards its old target: the
        // caller is about to state the state that is on screen now. Stopping a
        // QVariantAnimation rewinds it to its start value, so the value the paint
        // code is already using has to be restored afterwards.
        const qreal visible = m_value;
        if (m_animation) m_animation->stop();
        m_value = visible;
        if (milliseconds <= 0) {
            snap(target);
            if (finished) finished();
            return;
        }
        if (qFuzzyCompare(m_value + 1, target + 1)) return;
        if (!m_animation) {
            m_animation = new QVariantAnimation(m_host);
            connect(m_animation, &QVariantAnimation::valueChanged, m_host, [this](const QVariant& value) {
                m_value = value.toReal();
                if (m_host) m_host->update();
            });
            connect(m_animation, &QVariantAnimation::finished, m_host, [this] {
                if (finished) finished();
            });
        }
        m_animation->stop();
        m_animation->setStartValue(m_value);
        m_animation->setEndValue(target);
        m_animation->setDuration(milliseconds);
        m_animation->setEasingCurve(curve);
        m_animation->start(QAbstractAnimation::KeepWhenStopped);
    }

    QWidget* m_host = nullptr;
    qreal m_value = 0;
    int m_duration = Motion::state;
    QEasingCurve m_curve = QEasingCurve::OutCubic;
    QVariantAnimation* m_animation = nullptr;
};

// Animated channels shared by the painted control kinds. They live on the control
// itself so a control's lifetime bounds its animation, and they are created only
// for controls that actually animate.
class ControlMotion final : public QObject
{
public:
    explicit ControlMotion(QWidget* host)
        : QObject(host), hover(host), press(host), state(host) {}

    Tween hover;    // 0..1 pointer feedback
    Tween press;    // 0..1 pressed feedback
    Tween state;    // 0..1 checked, knob position and selection ink
};

inline ControlMotion* motionFor(QWidget* widget)
{
    const auto stored = widget->property("settingsControlMotion").value<quintptr>();
    if (stored) return reinterpret_cast<ControlMotion*>(stored);
    auto* created = new ControlMotion(widget);
    widget->setProperty("settingsControlMotion", QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(created)));
    return created;
}

// Wheel input for a settings page. Precise trackpad deltas pass straight through,
// notched wheels glide to the requested offset so the content never teleports.
class SmoothScroll final : public QObject
{
public:
    explicit SmoothScroll(QAbstractScrollArea* area);
    ~SmoothScroll() override;

protected:
    bool eventFilter(QObject* object, QEvent* event) override;

private:
    void glide(qreal delta);
    void stop();
    QAbstractScrollArea* m_area;
    QVariantAnimation* m_animation = nullptr;
    qreal m_target = 0;
};

inline SmoothScroll::SmoothScroll(QAbstractScrollArea* area) : QObject(area), m_area(area)
{
    area->viewport()->installEventFilter(this);
    area->installEventFilter(this);
    connect(area->verticalScrollBar(), &QScrollBar::sliderPressed, this, [this] { stop(); });
    connect(area->verticalScrollBar(), &QScrollBar::actionTriggered, this, [this] { stop(); });
}

inline SmoothScroll::~SmoothScroll()
{
    if (m_animation) m_animation->stop();
}

inline void SmoothScroll::stop()
{
    if (m_animation) m_animation->stop();
    m_target = m_area->verticalScrollBar()->value();
}

inline void SmoothScroll::glide(qreal delta)
{
    auto* bar = m_area->verticalScrollBar();
    const qreal reach = bar->maximum() - bar->minimum();
    if (reach <= 0) return;
    const qreal base = m_animation && m_animation->state() == QAbstractAnimation::Running
        ? m_target : bar->value();
    m_target = qBound<qreal>(bar->minimum(), base + delta, bar->maximum());
    const qreal distance = m_target - bar->value();
    if (qFuzzyIsNull(distance)) {
        stop();
        return;
    }
    if (!m_animation) {
        m_animation = new QVariantAnimation(this);
        connect(m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
            m_area->verticalScrollBar()->setValue(qRound(value.toReal()));
        });
        connect(m_animation, &QVariantAnimation::finished, this, [this] {
            m_area->verticalScrollBar()->setValue(qRound(m_target));
        });
    }
    m_animation->stop();
    m_animation->setStartValue(qreal(bar->value()));
    m_animation->setEndValue(m_target);
    m_animation->setDuration(motion(m_area,
        qBound(Motion::scroll, qRound(Motion::scroll + qAbs(distance) * Motion::scrollPerPixel),
            Motion::scrollLongest)));
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    m_animation->start();
}

inline bool SmoothScroll::eventFilter(QObject* object, QEvent* event)
{
    if (event->type() != QEvent::Wheel) return QObject::eventFilter(object, event);
    if (object != m_area && object != m_area->viewport()) return QObject::eventFilter(object, event);
    auto* wheel = static_cast<QWheelEvent*>(event);
    // Leave zooming and horizontal gestures to the widget that owns them.
    if (wheel->modifiers().testFlag(Qt::ControlModifier)
        || wheel->modifiers().testFlag(Qt::ShiftModifier)
        || wheel->modifiers().testFlag(Qt::AltModifier))
        return QObject::eventFilter(object, event);
    auto* bar = m_area->verticalScrollBar();
    if (bar->maximum() <= bar->minimum()) return QObject::eventFilter(object, event);
    const QPoint pixels = wheel->pixelDelta();
    if (!pixels.isNull()) {
        // A precise device already emits a per-frame stream; follow it directly.
        stop();
        bar->setValue(qBound(bar->minimum(), bar->value() - pixels.y(), bar->maximum()));
        return true;
    }
    const QPoint angle = wheel->angleDelta();
    if (!angle.y()) return QObject::eventFilter(object, event);
    int lines = QApplication::wheelScrollLines();
    if (lines <= 0) lines = 3;
    glide(-angle.y() / 120. * lines * qMax(1, bar->singleStep()));
    return true;
}
}
