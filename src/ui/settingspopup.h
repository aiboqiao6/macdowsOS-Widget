#pragma once

#include "settingsappearance.h"
#include "settingsbackdrop.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <cmath>

// A select popup that behaves like the reference recording: it pops out of the
// selector corner on a spring (one overshoot, then settle), the surface is glass -
// a blurred copy of whatever sits behind it plus a sheen and a hairline rim - and
// it collapses elastically on the way out. Rows come from the combo's own model,
// so the combo keeps its value, signals and keyboard focus; this widget only
// presents the list and receives the pick.
namespace SettingsAppearance {
class SelectorPopup final : public QWidget
{
public:
    SelectorPopup(QWidget* theme, QComboBox* combo)
        : QWidget(theme), m_theme(theme), m_combo(combo)
    {
        setObjectName(QStringLiteral("settingsSelectorPopup"));
        setFocusPolicy(Qt::NoFocus);   // the combo keeps the keyboard
        setMouseTracking(true);
        hide();
        m_timer.setTimerType(Qt::PreciseTimer);
        m_timer.setInterval(8);
        connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    }

    ~SelectorPopup() override { releaseApplicationFilter(); }

    bool isOpen() const { return m_open; }

    // Re-render after the window theme changed while the popup stays open.
    void refresh()
    {
        if (!m_open) return;
        buildImage();
        update();
    }

    void toggle(const QPoint& globalClick = QPoint())
    {
        if (m_open) close();
        else open(globalClick);
    }

    void open(const QPoint& globalClick = QPoint())
    {
        if (m_open || !m_combo || !m_theme) return;
        if (m_combo->count() <= 0) return;
        m_open = true;
        m_hovered = -1;   // iPadOS shows the check mark only until the pointer arrives
        buildGeometry();
        m_glass = {};
        buildImage();
        if (m_panelImage.isNull()) {
            m_open = false;
            return;
        }
        // The pop is centred on the point that was actually pressed, so the panel
        // grows out of the finger/cursor rather than out of a fixed corner.
        m_anchorGlobal = globalClick.isNull() ? selectorCentreGlobal() : globalClick;
        raise();
        show();
        // A spring with initial velocity: the panel is half way in 40 ms and
        // rebounds slightly instead of easing in from rest.
        m_progress = 0;
        m_velocity = Motion::popupInitialVelocity;
        m_timer.start();
        // Install the dismissal filter after this click has been delivered, or it
        // would see the very press that opened the popup and close it again.
        QTimer::singleShot(0, this, [this] { if (m_open) installApplicationFilter(); });
        if (m_theme) m_theme->setProperty("settingsPopupOpen", true);
        update();
    }

    void close()
    {
        if (!m_open) return;
        m_open = false;
        // Keep whatever momentum is on screen; the stiffer out-spring takes over.
        m_timer.start();
    }

    // Per-frame spring integration; `m_open` picks the stiffer dismissal setup.
    void tick()
    {
        const qreal dt = 0.008;
        const qreal target = m_open ? 1.0 : 0.0;
        const qreal stiffness = m_open ? Motion::popupStiffnessIn : Motion::popupStiffnessOut;
        // After the crest the spring switches to critical damping, so the panel
        // eases back onto its final size instead of swinging under it again. The
        // result is exactly one rebound, which is what the device recording shows.
        const bool returning = m_open && m_progress > 1.0 && m_velocity <= 0;
        const qreal damping = !m_open ? Motion::popupDampingOut
            : returning ? 2.0 * std::sqrt(stiffness) : Motion::popupDampingIn;
        const qreal before = m_progress;
        m_velocity += (stiffness * (target - m_progress) - damping * m_velocity) * dt;
        m_progress += m_velocity * dt;
        update();
        if (m_open) {
            if ((before > 1.0 && m_progress <= 1.0)
                || (qAbs(m_progress - 1) < 0.0015 && qAbs(m_velocity) < 0.04)) {
                m_progress = 1;
                m_velocity = 0;
                m_timer.stop();
                update();
            }
        } else if (m_progress < 0.02) {
            m_progress = 0;
            m_timer.stop();
            hide();
            releaseApplicationFilter();
            m_panelImage = {};
            if (m_theme) m_theme->setProperty("settingsPopupOpen", false);
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        if (m_panelImage.isNull()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setOpacity(qBound(qreal(0), Motion::popupOpacityBias + m_progress * Motion::popupOpacityGain, qreal(1)));
        // The spring is anchored on the corner that faces the selector, so the panel
        // grows out of the chevron instead of out of its own middle.
        const QPointF anchor = tipPoint();
        painter.translate(anchor);
        const qreal scale = qMax(qreal(0.02), popupScale());
        painter.scale(scale, scale);
        painter.translate(-anchor);
        // The pixmap carries the shadow margin already, so it belongs at the origin.
        painter.drawPixmap(QPointF(0, 0), m_panelImage);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        const int row = rowAt(event->position().toPoint());
        if (row != m_hovered) {
            m_hovered = row;
            update();
        }
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        // The widget also covers the shadow margin; a press there is "outside the
        // menu" and must dismiss it, otherwise the panel looks stuck.
        const QPointF anchor = tipPoint();
        const QPointF local = anchor + (QPointF(event->position()) - anchor)
            / qMax(qreal(0.02), popupScale());
        if (!bodyRect().contains(local)) {
            close();
            return;
        }
        const int row = rowAt(event->position().toPoint());
        if (row < 0) return;
        // Apply the pick and keep the menu up: this list is a picker, not a command
        // that dismisses itself. Only a press outside, Esc or the selector close it.
        if (m_combo && row != m_combo->currentIndex()) {
            m_combo->setCurrentIndex(row);
            buildImage();   // re-render so the check mark moves to the new row
            update();
        }
    }

    void leaveEvent(QEvent*) override
    {
        if (m_hovered != -1) {
            m_hovered = -1;
            update();
        }
    }

    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (!m_open) return QObject::eventFilter(object, event);
        if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            const QPoint global = mouse->globalPosition().toPoint();
            // Only the panel counts as inside; the shadow margin around it does not,
            // so a press next to the menu dismisses it like any other outside press.
            if (panelGlobalRect().contains(global))
                return QObject::eventFilter(object, event);
            // A press on the selector belongs to the combo, which toggles this popup;
            // closing here as well would make it close and immediately reopen.
            if (m_combo && QRect(m_combo->mapToGlobal(QPoint(0, 0)), m_combo->size()).contains(global))
                return QObject::eventFilter(object, event);
            close();
            return QObject::eventFilter(object, event);
        }
        if (event->type() == QEvent::KeyPress) {
            auto* key = static_cast<QKeyEvent*>(event);
            const int count = m_combo ? m_combo->count() : 0;
            if (key->key() == Qt::Key_Escape) {
                close();
                return true;
            }
            if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
                const int step = key->key() == Qt::Key_Down ? 1 : -1;
                const int from = m_hovered < 0 ? (m_combo ? m_combo->currentIndex() : 0) : m_hovered;
                m_hovered = qBound(0, from + step, qMax(0, count - 1));
                update();
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                if (m_hovered >= 0 && m_combo) m_combo->setCurrentIndex(m_hovered);
                close();
                return true;
            }
        }
        return QObject::eventFilter(object, event);
    }

private:
    static constexpr int kShadow = 60;   // must exceed 3x blur radius + drop, or the tail gets clipped

    // Panel inside this widget, inset by the shadow margin.
    QRectF bodyRect() const
    {
        return QRectF(kShadow, kShadow, m_panelWidth, m_panelHeight);
    }

    // Point the pop is centred on: the press position, or the selector glyph when
    // the popup was opened from the keyboard.
    QPointF tipPoint() const
    {
        return QPointF(mapFromGlobal(m_anchorGlobal));
    }

    QPoint selectorCentreGlobal() const
    {
        if (!m_combo) return mapToGlobal(QPoint(kShadow, kShadow));
        const QPoint local(m_combo->width() - px(m_theme, "selectorDiameter") / 2,
            m_combo->height() / 2);
        return m_combo->mapToGlobal(local);
    }

    // Panel rectangle in screen coordinates, at the scale currently on screen.
    QRectF panelGlobalRect() const
    {
        const QRectF body = bodyRect();
        const QPointF anchor = tipPoint();
        const qreal scale = qMax(qreal(0.02), popupScale());
        const QPointF topLeft = anchor + (body.topLeft() - anchor) * scale;
        return QRectF(topLeft, body.size() * scale).translated(mapToGlobal(QPoint(0, 0)));
    }

    QPainterPath outlinePath() const
    {
        const qreal radius = qMax(qreal(12), em(m_theme) * 1.1);   // iOS menu corner
        QPainterPath path;
        path.addRoundedRect(bodyRect(), radius, radius);
        return path;
    }

    qreal popupScale() const { return Motion::popupScaleFrom + (1 - Motion::popupScaleFrom) * m_progress; }

    int rowAt(const QPoint& position) const
    {
        // Undo the animated scale so hit testing follows what is on screen.
        const QPointF anchor = tipPoint();
        const QPointF local = anchor + (QPointF(position) - anchor) / qMax(qreal(0.02), popupScale());
        const QRectF panel = bodyRect();
        if (!panel.contains(local)) return -1;
        const int row = int((local.y() - panel.top() - m_padding) / m_rowHeight);
        return row >= 0 && row < m_rows ? row : -1;
    }

    void installApplicationFilter()
    {
        if (m_filtered) return;
        if (auto* app = QCoreApplication::instance()) {
            app->installEventFilter(this);
            m_filtered = true;
        }
    }

    void releaseApplicationFilter()
    {
        if (!m_filtered) return;
        if (auto* app = QCoreApplication::instance()) app->removeEventFilter(this);
        m_filtered = false;
    }

    // Geometry only: runs once per opening, from the selector outwards.
    void buildGeometry()
    {
        m_rows = m_combo->count();
        m_padding = qMax(2, px(m_theme, "controlInset"));
        m_rowHeight = qMax(qRound(m_theme->fontMetrics().height() + m_padding * 2.6),
            qRound(px(m_theme, "controlHeight") * 1.15));
        const QFontMetricsF metrics(m_theme->font());
        qreal widest = px(m_theme, "selectorDiameter") * 1.5;
        for (int i = 0; i < m_rows; ++i)
            widest = qMax(widest, metrics.horizontalAdvance(m_combo->itemText(i)));
        m_panelWidth = qMax(qRound(widest + m_padding * 6), qRound(em(m_theme) * 15));
        m_panelHeight = m_rows * m_rowHeight + m_padding * 2;

        const QRect comboRect(m_combo->mapTo(m_theme, QPoint(0, 0)), m_combo->size());
        const int gap = qMax(3, px(m_theme, "controlGap") / 2);
        const bool below = comboRect.bottom() + m_panelHeight + gap < m_theme->height();
        // Right edge flush with the selector chevron, which is what reads as
        // "attached to the control" in the reference.
        m_panelLeft = qBound(0, comboRect.right() - m_panelWidth + 1,
            qMax(0, m_theme->width() - m_panelWidth));
        m_panelTop = below ? comboRect.bottom() + gap : comboRect.top() - gap - m_panelHeight;
        setGeometry(m_panelLeft - kShadow, m_panelTop - kShadow,
            m_panelWidth + kShadow * 2, m_panelHeight + kShadow * 2);
    }

    // The glass backdrop only depends on geometry and on the theme, so it is
    // captured once per (opening, theme) and reused when the selection changes
    // while the popup stays open. Re-grabbing then would photograph the popup.
    void buildImage()
    {
        const qreal dpr = m_theme->devicePixelRatioF();
        const bool dark = m_theme->property("settingsDark").toBool();
        const QRectF body = bodyRect();
        const QRectF unionRect(kShadow, kShadow, m_panelWidth, m_panelHeight);
        const QSize pixelSize(qCeil(unionRect.width() * dpr), qCeil(unionRect.height() * dpr));
        if (m_glass.isNull() || m_glassDark != dark) {
            // Hide ourselves for the capture: the grab renders child widgets too, so
            // a visible popup would end up in its own frosted backdrop.
            const bool wasVisible = isVisible();
            if (wasVisible) hide();
            QImage behind = m_theme->grab(QRect(m_panelLeft, m_panelTop,
                m_panelWidth, m_panelHeight)).toImage();
            if (behind.isNull()) {
                behind = QImage(pixelSize, QImage::Format_RGB32);
                behind.fill(color(m_theme, "canvas"));
            }
            behind.setDevicePixelRatio(1);
            behind = behind.scaled(pixelSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            m_glass = SettingsBackdrop::blurred(behind,
                qMax(1, qRound(px(m_theme, "sidebarBlurRadius") * dpr * 1.8)));
            m_glassDark = dark;
            if (wasVisible) show();
        }
        const QImage& glass = m_glass;

        // The pixmap carries the device ratio, so its logical size is exactly the
        // widget size and painting below stays in logical units.
        QPixmap panel(QSize(qCeil(unionRect.width()) + kShadow * 2,
            qCeil(unionRect.height()) + kShadow * 2) * dpr);
        panel.setDevicePixelRatio(dpr);
        panel.fill(Qt::transparent);
        QPainter painter(&panel);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);

        // Shadow. The silhouette is already offset downwards inside its own buffer
        // and then blurred once with the sidebar's separable box blur, so the
        // falloff is a real radius and nothing is sheared off at the widget bounds -
        // shifting the layer while drawing left a straight cut wherever the tail
        // met the edge of the buffer.
        const QPainterPath outline = outlinePath();
        {
            const bool darkShadow = m_theme->property("settingsDark").toBool();
            const qreal unit = em(m_theme);
            const qreal drop = unit * 0.42;
            const int blurRadius = qMax(2, qRound(unit * 0.65 * dpr));   // 3 passes -> support ~3R
            QImage silhouette(panel.size(), QImage::Format_RGB32);
            silhouette.setDevicePixelRatio(dpr);
            silhouette.fill(Qt::white);
            {
                QPainter mask(&silhouette);
                mask.setRenderHint(QPainter::Antialiasing);
                mask.translate(0, drop);
                mask.setPen(Qt::NoPen);
                mask.setBrush(Qt::black);
                mask.drawPath(outline);
            }
            const QImage soft = SettingsBackdrop::blurred(silhouette, blurRadius);
            QImage shadow(soft.size(), QImage::Format_ARGB32_Premultiplied);
            shadow.setDevicePixelRatio(dpr);
            const int peak = darkShadow ? 150 : 40;   // light theme gets a faint shadow
            for (int y = 0; y < soft.height(); ++y) {
                const QRgb* src = reinterpret_cast<const QRgb*>(soft.constScanLine(y));
                auto* dst = reinterpret_cast<QRgb*>(shadow.scanLine(y));
                for (int x = 0; x < soft.width(); ++x)
                    dst[x] = qRgba(0, 0, 0, (255 - qRed(src[x])) * peak / 255);
            }
            painter.drawImage(QPointF(0, 0), shadow);
        }
        painter.setPen(Qt::NoPen);

        painter.save();
        painter.setClipPath(outline);
        // iOS menu material is only two things: the blurred backdrop, and one light
        // neutral layer on top of it. No accent, no theme colour, no border - every
        // bit of colour comes from what is behind the panel.
        painter.drawImage(unionRect, glass);
        QColor tint(255, 255, 255);
        tint.setAlphaF(dark ? 0.10f : 0.62f);
        painter.fillRect(unionRect, tint);
        painter.restore();

        // Rows: iPadOS puts the check mark in a leading gutter and separates rows
        // with a hairline, so nothing is tinted with the accent colour.
        const qreal inset = m_padding * 0.55;
        const qreal gutter = em(m_theme) * 1.5;   // room for the leading check mark
        const qreal radius = qMax(qreal(12), em(m_theme) * 1.1);
        const QColor highlight = dark ? QColor(255, 255, 255, 20) : QColor(0, 0, 0, 14);
        const QColor divider = dark ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 14);
        for (int i = 0; i < m_rows; ++i) {
            const QRectF row(body.left() + inset, body.top() + m_padding + i * m_rowHeight,
                body.width() - inset * 2, m_rowHeight);
            const bool selected = i == m_combo->currentIndex();
            if (i == m_hovered) {
                painter.setBrush(highlight);
                painter.drawRoundedRect(row.adjusted(0, 1, 0, -1), radius * 0.55, radius * 0.55);
                painter.setBrush(Qt::NoBrush);
            }
            if (i > 0) {
                painter.setPen(QPen(divider, qMax(qreal(1) / dpr, qreal(px(m_theme, "stroke")))));
                painter.drawLine(QPointF(row.left() + m_padding * 0.7, row.top() + 1),
                    QPointF(row.right() - m_padding * 0.7, row.top() + 1));
                painter.setPen(Qt::NoPen);
            }
            painter.setFont(m_theme->font());
            painter.setPen(color(m_theme, "foreground"));
            const QFontMetricsF rowMetrics(m_theme->font());
            const QString text = rowMetrics.elidedText(m_combo->itemText(i), Qt::ElideRight,
                row.width() - m_padding * 1.2 - gutter);
            painter.drawText(QPointF(row.left() + m_padding + gutter,
                row.center().y() + (rowMetrics.ascent() - rowMetrics.descent()) / 2), text);
            if (selected) {
                // Leading check mark, drawn as two strokes for a crisp small size.
                const QPointF c(row.left() + m_padding + gutter * 0.35, row.center().y());
                const qreal unit = em(m_theme) * 0.24;
                painter.setPen(QPen(color(m_theme, "foreground"), qMax(qreal(1.7), em(m_theme) * 0.085),
                    Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                QPolygonF check{c + QPointF(-unit * 1.1, 0), c + QPointF(-unit * 0.3, unit * 0.85),
                    c + QPointF(unit * 1.2, -unit * 0.95)};
                painter.drawPolyline(check);
                painter.setPen(Qt::NoPen);
            }
        }
        m_panelImage = panel;
    }

    QWidget* m_theme;
    QPointer<QComboBox> m_combo;
    QTimer m_timer;
    QPixmap m_panelImage;
    QImage m_glass;          // cached backdrop blur, valid per opening+theme
    bool m_glassDark = false;
    int m_panelLeft = 0;
    int m_panelTop = 0;
    int m_panelWidth = 0;
    int m_panelHeight = 0;
    int m_rows = 0;
    int m_rowHeight = 0;
    int m_padding = 0;
    int m_hovered = -1;
    QPoint m_origin;
    QPoint m_anchorGlobal;   // press point, in screen coordinates
    qreal m_progress = 0;
    qreal m_velocity = 0;
    bool m_open = false;
    bool m_filtered = false;
};
}
