#include "ui/gridplacementoverlay.h"
#include <QPainter>
#include <QPropertyAnimation>
#include <QWidget>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
class GridSnapOverlay;
GridSnapOverlay* gridFadeOverlay();

class GridSnapOverlay final : public QWidget
{
public:
    explicit GridSnapOverlay(bool fadeLayer = false)
        : m_fadeLayer(fadeLayer)
    {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                       | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        m_opacityAnimation = new QPropertyAnimation(this, "windowOpacity", this);
        m_opacityAnimation->setEasingCurve(QEasingCurve::InOutCubic);
        connect(m_opacityAnimation, &QPropertyAnimation::finished, this, [this]() {
            if (m_hideAfterFade) {
                m_hideAfterFade = false;
                hide();
            }
        });
    }

    // 放置提示通过原生窗口透明度弱化填充，保持白色轮廓清晰。
    static constexpr qreal kOverlayOpacity = 0.68;

    void display(const QRect& target)
    {
        if (!target.isValid())
            return;

        m_hideAfterFade = false;
        m_opacityAnimation->stop();
        if (isVisible() && geometry() != target) {
            // 旧轮廓使用独立透明窗口淡出，新轮廓立即定位到目标位置。
            gridFadeOverlay()->fadeFrom(geometry(), windowOpacity());
        }

        setGeometry(target);
        setWindowOpacity(kOverlayOpacity);
        if (!isVisible())
            show();

#ifdef Q_OS_WIN
        // 放置提示不抢夺焦点，并紧贴拖动卡片下方。
        placeBelowDraggedWidget();
#endif
        update();
    }

    void fadeFrom(const QRect& source, qreal sourceOpacity)
    {
        if (!m_fadeLayer || !source.isValid())
            return;
        m_hideAfterFade = false;
        m_opacityAnimation->stop();
        setGeometry(source);
        setWindowOpacity(qBound<qreal>(0.0, sourceOpacity, kOverlayOpacity));
        show();
#ifdef Q_OS_WIN
        placeBelowDraggedWidget();
#endif
        animateOpacity(windowOpacity(), 0.0, 95);
    }

    void dismiss()
    {
        if (!isVisible() || m_hideAfterFade)
            return;
        m_hideAfterFade = true;
        animateOpacity(windowOpacity(), 0.0, 85);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal radius = qMin<qreal>(28.0, qMin(width(), height()) * .12);
        const QRectF bounds = QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5);

        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, 245), 4.0));
        painter.drawRoundedRect(bounds, radius, radius);
    }

private:
    void placeBelowDraggedWidget()
    {
#ifdef Q_OS_WIN
        if (QWidget* grabber = QWidget::mouseGrabber())
            SetWindowPos(reinterpret_cast<HWND>(winId()),
                         reinterpret_cast<HWND>(grabber->window()->winId()), 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
#endif
    }

    void animateOpacity(qreal start, qreal end, int duration)
    {
        if (!m_opacityAnimation)
            return;
        m_opacityAnimation->stop();
        m_opacityAnimation->setDuration(qMax(1, duration));
        m_opacityAnimation->setStartValue(start);
        m_opacityAnimation->setEndValue(end);
        m_opacityAnimation->start();
    }

    QPropertyAnimation* m_opacityAnimation = nullptr;
    bool m_fadeLayer = false;
    bool m_hideAfterFade = false;
};

GridSnapOverlay* gridOverlay()
{
    static GridSnapOverlay* overlay = new GridSnapOverlay;
    return overlay;
}

GridSnapOverlay* gridFadeOverlay()
{
    static GridSnapOverlay* overlay = new GridSnapOverlay(true);
    return overlay;
}
}

void GridPlacementOverlay::display(const QRect& target) { gridOverlay()->display(target); }
void GridPlacementOverlay::dismiss()
{
    gridOverlay()->dismiss();
    gridFadeOverlay()->dismiss();
}
