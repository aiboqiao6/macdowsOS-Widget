#include "widgets/desktopwidget.h"
#include "widgets/musiccard.h"
#include <QPainter>
#include <QPainterPath>

void DesktopWidget::paintOverlay(QPainter& painter)
{
    const qreal scale = qMax<qreal>(0.1, m_widgetScale);
    painter.save();
    if (darkModeEnabled())
        painter.setOpacity(painter.opacity() * .90);
    painter.scale(scale, scale);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, globalFontSmoothing() > 0);

    // 内容按 100% 参考坐标绘制，只缩放一次；玻璃覆盖完整窗口，避免额外内缩产生外圈。
    const QSizeF logicalSize(width() / scale, height() / scale);
    const QRectF card(QPointF(0.0, 0.0), logicalSize);
    const qreal radius = LiquidGlassStyle::kCardCornerRadius;

    // 内容与着色器共享圆角裁剪，轮廓之外保持透明。
    QPainterPath cardClip;
    cardClip.addRoundedRect(card, radius, radius);
    painter.setClipPath(cardClip, Qt::IntersectClip);

    switch (m_cardKind) {
    case CardKind::Battery: paintBatteryCard(painter, card); break;
    case CardKind::Weather: paintWeatherCard(painter, card); break;
    case CardKind::Clock: paintClockCard(painter, card); break;
    case CardKind::Dictionary: paintDictionaryCard(painter, card); break;
    case CardKind::Music: MusicCard::paint(painter, card, SystemMediaSession::instance()->snapshot()); break;
    }
    painter.restore();
}
