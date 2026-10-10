#include "liquidglassmenu.h"
#include "rendering/liquidglasswidget.h"
#include "rendering/nativewindows.h"
#include "rendering/responsivelayout.h"

#include <QAction>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include "app/appsettings.h"
#include <QShowEvent>
#include <QtMath>

namespace {
constexpr qreal kRadius = 16;
constexpr auto kIconProperty = "liquidGlassMenuIcon";

void drawIcon(QPainter& painter, const QRectF& box, LiquidGlassMenu::Icon icon)
{
    painter.save();
    painter.translate(box.topLeft());
    painter.scale(box.width() / 18.0, box.height() / 18.0);
    QPen pen = painter.pen();
    pen.setWidthF(1.5);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    switch (icon) {
    case LiquidGlassMenu::Icon::Add:
        painter.drawRoundedRect(QRectF(2, 2, 14, 14), 4, 4);
        painter.drawLine(QPointF(5.5, 9), QPointF(12.5, 9));
        painter.drawLine(QPointF(9, 5.5), QPointF(9, 12.5));
        break;
    case LiquidGlassMenu::Icon::Search:
        painter.drawEllipse(QRectF(2.5, 2.5, 9, 9));
        painter.drawLine(QPointF(10.5, 10.5), QPointF(15.5, 15.5));
        break;
    case LiquidGlassMenu::Icon::Remove:
        painter.drawLine(QPointF(2.5, 4.5), QPointF(15.5, 4.5));
        painter.drawRoundedRect(QRectF(6, 1.5, 6, 3), 1, 1);
        painter.drawRoundedRect(QRectF(4, 4.5, 10, 12), 2, 2);
        painter.drawLine(QPointF(7, 8), QPointF(7, 13));
        painter.drawLine(QPointF(11, 8), QPointF(11, 13));
        break;
    }
    painter.restore();
}
}

class LiquidGlassMenuSurface final : public QtGlassFlowScene
{
public:
    explicit LiquidGlassMenuSurface(LiquidGlassMenu* menu)
        : QtGlassFlowScene(menu), m_menu(menu)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFocusPolicy(Qt::NoFocus);
        setAnimationEnabled(false);
        m_object = addGlassObject({}, QSizeF(menu->size()));
        setGlassObjectCornerRadius(m_object, float(kRadius));
    }
    void fit(const QRect& bounds)
    {
        setGeometry(bounds);
        setGlassObjectGeometry(m_object, {}, QSizeF(bounds.size()));
    }
protected:
    void paintOverlay(QPainter& painter) override { m_menu->paintContents(painter); }
private:
    LiquidGlassMenu* m_menu;
    int m_object = -1;
};

LiquidGlassMenu::LiquidGlassMenu(const QFont& textFont, QWidget* parent)
    : QMenu(parent)
{
    setObjectName(QStringLiteral("liquidGlassContextMenu"));
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFont(textFont);
    // QMenu 只负责布局和命中区域，所有像素由透明玻璃子窗口绘制。
    setStyleSheet(QStringLiteral(
        "QMenu { background: transparent; border: none; padding: 7px; }"
        "QMenu::item { padding: 8px 14px 8px 36px; min-width: 144px; }"
        "QMenu::separator { height: 9px; margin: 0px 12px; }"));
    AppSettings settings;
    m_renderScale = qBound(.5f, settings.value(QStringLiteral("performance/renderScale"), 0.60).toFloat(), 1.5f);
    m_liveBackdrop = settings.value(QStringLiteral("appearance/liveBackdrop"), true).toBool();
    m_captureTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_captureTimer, &QTimer::timeout, this, &LiquidGlassMenu::refreshBackdrop);
    connect(this, &QMenu::hovered, this, [this]() { if (m_surface) m_surface->update(); });
}

void LiquidGlassMenu::createSurface()
{
    if (m_surface)
        return;
    m_surface = new LiquidGlassMenuSurface(this);
    m_surface->setRenderScale(m_renderScale);
}

LiquidGlassMenu::~LiquidGlassMenu()
{
    releaseBackdrop();
    // 先销毁覆盖层，再销毁菜单动作列表。
    delete m_surface;
    m_surface = nullptr;
}

QAction* LiquidGlassMenu::addGlassAction(const QString& text, Icon icon)
{
    QAction* action = addAction(text);
    action->setProperty(kIconProperty, int(icon));
    return action;
}

void LiquidGlassMenu::showEvent(QShowEvent* event)
{
    QMenu::showEvent(event);
    createSurface();
    m_surface->fit(rect());
    m_registeredWindow = winId();
    NativeWindows::registerGlassWindow(m_registeredWindow);
    m_surface->show();
    if (m_liveBackdrop)
        m_captureTimer.start();
}

QAction* LiquidGlassMenu::exec(const QPoint& position)
{
    return QMenu::exec(preparePopup(position));
}

void LiquidGlassMenu::popup(const QPoint& position)
{
    QMenu::popup(preparePopup(position));
}

QPoint LiquidGlassMenu::preparePopup(const QPoint& position)
{
    ++m_generation;
    m_cache.clear();
    m_capturePending = false;
    QScreen* output = QGuiApplication::screenAt(position);
    if (!output)
        output = QGuiApplication::primaryScreen();
    QPoint origin = position;
    if (output) {
        ensurePolished();
        const QSize extent = sizeHint();
        const QRect available = output->availableGeometry();
        origin.setX(qBound(available.left(), origin.x(), qMax(available.left(), available.right() - extent.width() + 1)));
        origin.setY(qBound(available.top(), origin.y(), qMax(available.top(), available.bottom() - extent.height() + 1)));
        const QRect captureArea(origin, extent);
        // 弹出原生菜单前捕获背景，避免读到未绘制的黑色表面；捕获密度不超过屏幕原生像素。
        const qreal scale = output->devicePixelRatio() * qMin(1.0f, m_renderScale);
        m_fallback = QImage(QSize(qMax(1, qRound(extent.width() * scale)),
                                 qMax(1, qRound(extent.height() * scale))), QImage::Format_RGB32);
        m_fallback.fill(QColor(38, 47, 65));
        QImage backdrop = m_cache.merge(DesktopCapture::grab(captureArea, scale),
                                       [this]() { return m_fallback; });
        if (backdrop.isNull())
            backdrop = m_cache.merge(DesktopCapture::grab(captureArea, scale),
                                     [this]() { return m_fallback; });
        if (backdrop.isNull())
            backdrop = m_fallback;
        // 回退背景不包含玻璃合成结果，防止反馈。
        if (!backdrop.isNull())
            m_fallback = backdrop;
        LiquidGlassWidget::compositeDesktopGlass(backdrop, captureArea);
        m_lastBackdrop = backdrop;
        createSurface();
        m_surface->setBackgroundImage(backdrop);
        m_captureTimer.setInterval(qBound(8, qRound(1000.0 / qMax(30.0, output->refreshRate())), 33));
    }
    return origin;
}

void LiquidGlassMenu::releaseBackdrop()
{
    m_captureTimer.stop();
    ++m_generation;
    m_capturePending = false;
    NativeWindows::unregisterGlassWindow(m_registeredWindow);
    m_registeredWindow = 0;
    m_cache.clear();
    m_fallback = {};
    m_lastBackdrop = {};
}

void LiquidGlassMenu::hideEvent(QHideEvent* event)
{
    releaseBackdrop();
    QMenu::hideEvent(event);
}

void LiquidGlassMenu::resizeEvent(QResizeEvent* event)
{
    QMenu::resizeEvent(event);
    if (m_surface)
        m_surface->fit(rect());
}

void LiquidGlassMenu::moveEvent(QMoveEvent* event)
{
    QMenu::moveEvent(event);
    if (isVisible() && m_registeredWindow)
        refreshBackdrop();
}

void LiquidGlassMenu::refreshBackdrop()
{
    if (!isVisible() || !m_registeredWindow || m_capturePending)
        return;
    m_capturePending = true;
    const quint64 generation = m_generation;
    const QRect area = geometry();
    const qreal scale = devicePixelRatioF() * m_surface->renderScale();
    DesktopCapture::request(area, scale, m_registeredWindow, this,
        [this, generation, area](DesktopCapture::Frame frame) {
            if (generation != m_generation)
                return;
            m_capturePending = false;
            if (!isVisible() || area != geometry())
                return;
            QImage backdrop = m_cache.merge(std::move(frame), [this]() { return m_fallback; });
            LiquidGlassWidget::compositeDesktopGlass(backdrop, area, m_registeredWindow);
            if (!backdrop.isNull() && backdrop != m_lastBackdrop) {
                m_lastBackdrop = backdrop;
                m_surface->setBackgroundImage(backdrop);
            }
        });
}

void LiquidGlassMenu::paintEvent(QPaintEvent*)
{
    // 子窗口独立绘制，父窗口不能再次调度子窗口更新，以免形成重绘循环。
}

bool LiquidGlassMenu::event(QEvent* event)
{
    const bool handled = QMenu::event(event);
    switch (event->type()) {
    case QEvent::KeyPress:
    case QEvent::MouseMove:
    case QEvent::Leave:
    case QEvent::ActionChanged:
    case QEvent::ActionAdded:
    case QEvent::ActionRemoved:
        if (m_surface)
            m_surface->update();
        break;
    default:
        break;
    }
    return handled;
}

void LiquidGlassMenu::paintContents(QPainter& painter)
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing,
        !(font().styleStrategy() & QFont::NoAntialias));
    painter.setFont(font());
    for (const QAction* action : actions()) {
        const QRectF item = actionGeometry(const_cast<QAction*>(action));
        if (!action->isVisible() || item.isEmpty())
            continue;
        if (action->isSeparator()) {
            painter.setPen(QPen(QColor(235, 243, 255, 30), 1));
            painter.drawLine(QPointF(20, item.center().y()),
                             QPointF(width() - 20, item.center().y()));
            continue;
        }
        const bool selected = action == activeAction() && action->isEnabled();
        if (selected) {
            QLinearGradient highlight(item.topLeft(), item.bottomLeft());
            highlight.setColorAt(0, QColor(255, 255, 255, 43));
            highlight.setColorAt(1, QColor(220, 237, 255, 22));
            painter.setBrush(highlight);
            painter.setPen(QPen(QColor(255, 255, 255, 37), .8));
            painter.drawRoundedRect(item.adjusted(.5, .5, -.5, -.5), 9, 9);
        }
        const auto icon = static_cast<Icon>(action->property(kIconProperty).toInt());
        const QColor color = !action->isEnabled() ? QColor(235, 240, 250, 88)
            : icon == Icon::Remove ? QColor(255, 169, 173) : QColor(246, 249, 255);
        painter.setPen(color);
        drawIcon(painter, QRectF(item.left() + 11, item.center().y() - 8, 16, 16), icon);
        ResponsiveLayout::drawSingleLine(painter, item.adjusted(36, 0, -14, 0),
                                        Qt::AlignLeft | Qt::AlignVCenter, action->text());
    }
    painter.restore();
}
