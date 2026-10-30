#include "widgets/desktopwidget.h"
#include "widgets/cardmetrics.h"
#include "app/fontsettings.h"
#include "rendering/responsivelayout.h"
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

using namespace CardMetrics;
using AppFonts::displayFont;

namespace {
const QColor kBatteryForeground(226, 229, 228, 203);
const QColor kBatteryProgress(226, 232, 234, 239);

// 内容坐标使用逻辑像素，避免磅字号额外放大文字。
QFont batteryFont(qreal pixels)
{
    QFont font = displayFont(1, QFont::Normal);
    font.setPixelSize(qMax(1, qRound(pixels)));
    return font;
}

// 三种电池布局共用设备图标，尺寸随圆环或列表行高缩放。
void drawDeviceIcon(QPainter& painter, const QPointF& center, qreal radius,
                    int iconKind, bool connected)
{
    if (!connected && iconKind != 0)
        return;
    painter.save();
    const ResponsiveLayout::Metrics metrics(QRectF(center.x() - radius,
        center.y() - radius, radius * 2.0, radius * 2.0));
    const QColor color = connected ? kBatteryForeground : QColor(226, 229, 228, 55);
    painter.setPen(QPen(color, metrics.stroke(painter, .0275), Qt::SolidLine,
                       Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    if (iconKind == 0) {
        const QRectF screen(center.x() - radius * .415, center.y() - radius * .28,
                            radius * .83, radius * .56);
        painter.setBrush(QColor(226, 229, 228, connected ? 34 : 12));
        painter.drawRoundedRect(screen, radius * .035, radius * .035);
        painter.drawLine(QPointF(center.x() - radius * .065, screen.top() + radius * .025),
                         QPointF(center.x() + radius * .065, screen.top() + radius * .025));
        painter.setPen(QPen(color, metrics.stroke(painter, .04), Qt::SolidLine,
                           Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(QPointF(center.x() - radius * .52, screen.bottom()),
                         QPointF(center.x() + radius * .52, screen.bottom()));
    } else if (iconKind == 1) {
        const QRectF mouse(center.x() - radius * .27, center.y() - radius * .42,
                           radius * .54, radius * .84);
        QPainterPath body;
        body.addRoundedRect(mouse, radius * .26, radius * .26);
        // 镂空按键分隔，让现有玻璃背景透过图标。
        QPainterPath seams;
        seams.addRect(QRectF(mouse.left(), center.y() - radius * .16,
                             mouse.width(), radius * .04));
        seams.addRect(QRectF(center.x() - radius * .02, mouse.top(),
                             radius * .04, radius * .30));
        painter.fillPath(body.subtracted(seams), color);
    } else if (iconKind == 2) {
        const QRectF keyboard(center.x() - radius * .39, center.y() - radius * .21,
                              radius * .78, radius * .42);
        painter.setBrush(QColor(226, 229, 228, 34));
        painter.drawRoundedRect(keyboard, radius * .07, radius * .07);
        painter.drawLine(keyboard.bottomLeft() + QPointF(radius * .12, -radius * .10),
                         keyboard.bottomRight() + QPointF(-radius * .12, -radius * .10));
    } else if (iconKind == 3) {
        painter.drawArc(QRectF(center.x() - radius * .31, center.y() - radius * .31,
                              radius * .62, radius * .62), 0, 180 * 16);
        painter.drawLine(QPointF(center.x() - radius * .31, center.y()),
                         QPointF(center.x() - radius * .31, center.y() + radius * .28));
        painter.drawLine(QPointF(center.x() + radius * .31, center.y()),
                         QPointF(center.x() + radius * .31, center.y() + radius * .28));
    } else if (iconKind >= 0) {
        painter.setBrush(QColor(226, 229, 228, 34));
        painter.drawRoundedRect(QRectF(center.x() - radius * .24, center.y() - radius * .34,
                                      radius * .48, radius * .68), radius * .10, radius * .10);
    }
    painter.restore();
}
}

void DesktopWidget::drawBatteryIcon(QPainter& painter, const QRectF& rect, int level, bool charging) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, GraphicsQuality::enabled());
    const ResponsiveLayout::Metrics metrics(rect);

    const qreal radius = rect.height() * 0.21;
    const qreal edge = metrics.stroke(painter, .012);
    QRectF body = rect.adjusted(edge, edge, -rect.height() * 0.105, -edge);
    QPainterPath bodyPath;
    bodyPath.addRoundedRect(body, radius, radius);

    QLinearGradient shell(0, body.top(), 0, body.bottom());
    shell.setColorAt(0.0, QColor(255, 255, 255, 220));
    shell.setColorAt(0.45, QColor(221, 227, 236, 165));
    shell.setColorAt(1.0, QColor(157, 166, 180, 160));
    painter.fillPath(bodyPath, shell);

    const qreal inset = metrics.size(.09);
    QRectF levelRect = body.adjusted(inset, inset, -inset, -inset);
    levelRect.setWidth(levelRect.width() * level / 100.0);
    if (level > 0) {
        QPainterPath levelPath;
        levelPath.addRoundedRect(levelRect, radius * 0.68, radius * 0.68);
        QLinearGradient fill(levelRect.topLeft(), levelRect.bottomRight());
        if (level <= 20) {
            fill.setColorAt(0.0, QColor(255, 142, 91));
            fill.setColorAt(1.0, QColor(235, 58, 82));
        } else if (charging) {
            fill.setColorAt(0.0, QColor(255, 255, 255, 235));
            fill.setColorAt(1.0, QColor(255, 255, 255, 235));
        } else {
            fill.setColorAt(0.0, QColor(255, 255, 255, 235));
            fill.setColorAt(1.0, QColor(255, 255, 255, 235));
        }
        painter.fillPath(levelPath, fill);
    }

    QRectF terminal(body.right() + metrics.size(.02),
                    body.center().y() - rect.height() * .16,
                    rect.height() * .105, rect.height() * .32);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(208, 215, 225, 190));
    painter.drawRoundedRect(terminal, terminal.width() * .5, terminal.width() * .5);

    painter.setPen(QPen(QColor(255, 255, 255, 160),
                        metrics.stroke(painter, .012)));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(bodyPath);

    if (charging) {
        QPainterPath bolt;
        const qreal cx = body.center().x();
        const qreal cy = body.center().y();
        bolt.moveTo(cx + rect.width() * .015, cy - rect.height() * .24);
        bolt.lineTo(cx - rect.width() * .095, cy + rect.height() * .015);
        bolt.lineTo(cx - rect.width() * .005, cy + rect.height() * .015);
        bolt.lineTo(cx - rect.width() * .065, cy + rect.height() * .25);
        bolt.lineTo(cx + rect.width() * .12, cy - rect.height() * .055);
        bolt.lineTo(cx + rect.width() * .025, cy - rect.height() * .055);
        bolt.closeSubpath();
        painter.setBrush(QColor(255, 255, 255, 235));
        painter.setPen(Qt::NoPen);
        painter.drawPath(bolt);
    }
    painter.restore();
}

void DesktopWidget::drawDeviceRing(QPainter& painter, const QPointF& center, qreal radius,
                                   int level, int iconKind, bool connected,
                                   bool showCaption) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, GraphicsQuality::enabled());
    const QRectF ring(center.x() - radius, center.y() - radius,
                      radius * 2.0, radius * 2.0);
    const ResponsiveLayout::Metrics metrics(ring);
    // 两种圆环卡片保持相同粗细，切换外观时不改变布局。
    const qreal stroke = metrics.stroke(painter, .102);

    QPen track(QColor(238, 240, 240, connected ? 48 : 25), stroke,
               Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(track);
    painter.drawEllipse(ring);

    const int clamped = qBound(0, level, 100);
    if (connected && level >= 0 && clamped > 0) {
        const qreal span = -360.0 * clamped / 100.0;
        QPen progress(kBatteryProgress, stroke,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(progress);
        constexpr qreal startAngle = 90.0;
        painter.drawArc(ring, qRound(startAngle * 16.0), qRound(span * 16.0));

    }

    drawDeviceIcon(painter, center, radius, iconKind, connected);

    // 空槽不显示说明，仅真实连接设备显示文字。
    if (showCaption && (connected || iconKind == 0)) {
        painter.setFont(batteryFont(radius * .60));
        painter.setPen(connected ? kBatteryForeground : QColor(226, 229, 228, 66));
        ResponsiveLayout::drawSingleLine(painter, QRectF(center.x() - radius * .72,
                                 center.y() + radius * 1.52,
                                 radius * 1.44, radius * .80),
                       Qt::AlignHCenter | Qt::AlignVCenter,
                       !connected ? QStringLiteral("—")
                                  : (level >= 0
                                         ? QString::number(clamped) + QChar('%')
                                         : QStringLiteral("已连接")));
    }
    painter.restore();
}

void DesktopWidget::paintBatteryCard(QPainter& painter, const QRectF& card) const
{
    painter.save();
    if (m_cardVariant == 0 || m_cardVariant == 1) {
        const bool compact = m_cardVariant == 0;
        const qreal ringRadius = qMin(card.width(), card.height()) * .178;
        // 本机与前三个真实外设依次占位；方形两行两列，横向一行四列。
        for (int slot = 0; slot < 4; ++slot) {
            const qreal xRatio = compact ? (slot % 2 == 0 ? .270 : .730)
                                        : .148 + .704 * slot / 3.0;
            const qreal yRatio = compact ? (slot < 2 ? .270 : .730) : .407;
            const QPointF center(card.left() + card.width() * xRatio,
                                 card.top() + card.height() * yRatio);
            if (slot == 0) {
                drawDeviceRing(painter, center, ringRadius,
                               m_hasBattery ? m_batteryLevel : -1, 0, m_hasBattery, !compact);
                continue;
            }
            const bool connected = slot - 1 < m_connectedDevices.size();
            const ConnectedDevice device = connected ? m_connectedDevices.at(slot - 1)
                                                      : ConnectedDevice{};
            drawDeviceRing(painter, center, ringRadius, device.level,
                           connected ? device.iconKind : -1, connected, !compact);
        }
        painter.restore();
        return;
    }

    if (m_cardVariant == 2) {
        // 六行铺满卡片，图标、设备名、电量和电池分别共用固定对齐列。
        const ResponsiveLayout::Metrics cardMetrics(card);
        const auto row = [&painter](const QRectF& area, const QString& name,
                              int level, int iconKind) {
            const ResponsiveLayout::Metrics rowMetrics(area);
            const qreal s = area.height();
            drawDeviceIcon(painter, QPointF(area.left() + s * .44, area.center().y()),
                           s * .32, iconKind, true);
            painter.setPen(kBatteryForeground);
            painter.setFont(batteryFont(s * .20));
            const int nameWidth = qMax(1, qFloor(area.width() - s * 2.55));
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(area.left() + s * .88, area.top(),
                          nameWidth, area.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(painter.font()).elidedText(name, Qt::ElideRight, nameWidth));
            painter.setFont(batteryFont(s * .19));
            painter.setPen(kBatteryForeground);
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(area.right() - s * 1.59, area.top(),
                          s * .65, area.height()),
                Qt::AlignRight | Qt::AlignVCenter,
                level >= 0 ? QString::number(qBound(0, level, 100)) + QChar('%')
                           : QStringLiteral("—"));
            const qreal height = s * .15;
            const qreal stroke = qMin(height * .18, rowMetrics.stroke(painter, .014));
            const QRectF bounds(area.right() - s * .71,
                                area.center().y() - height * .5, s * .35, height);
            const QRectF body = bounds.adjusted(stroke * .5, stroke * .5,
                                                -height * .24 - stroke * .5, -stroke * .5);
            const qreal radius = height * .20;
            painter.setPen(QPen(kBatteryForeground, stroke));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(body, radius, radius);
            if (level >= 0) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(kBatteryProgress);
                const qreal inset = stroke * 1.5;
                QRectF fill = body.adjusted(inset, inset, -inset, -inset);
                fill.setWidth(fill.width() * qBound(0, level, 100) / 100.0);
                if (fill.width() > 0.0 && fill.height() > 0.0) {
                    const qreal fillRadius = qMin(radius * .5, fill.width() * .5);
                    painter.drawRoundedRect(fill, fillRadius, fillRadius);
                }
            }
            // 端子使用垂直居中的短胶囊，与外壳保留间隙，避免看起来像悬浮的减号。
            const QRectF terminal(bounds.right() - height * .14,
                                  bounds.center().y() - height * .18,
                                  height * .14, height * .36);
            painter.setPen(Qt::NoPen);
            painter.setBrush(kBatteryForeground);
            painter.drawRoundedRect(terminal, terminal.width() * .5, terminal.width() * .5);
        };
        // 六行等高，只有真实设备填充内容，剩余行保持空白。
        const qreal rowHeight = card.height() / kBatteryListRows;
        const QRectF firstRow(card.left(), card.top(), card.width(), rowHeight);
        const QRectF secondRow(card.left(), card.top() + rowHeight,
                               card.width(), rowHeight);
        row(firstRow, m_hostDeviceName,
            m_hasBattery ? m_batteryLevel : -1, 0);
        for (int slot = 0; slot < qMin(kBatteryListRows - 1,
                                       int(m_connectedDevices.size())); ++slot) {
            const ConnectedDevice& device = m_connectedDevices.at(slot);
            const QRectF deviceRow(card.left(), secondRow.top() + rowHeight * slot,
                                   card.width(), rowHeight);
            row(deviceRow,
                device.name.isEmpty() ? QStringLiteral("已连接设备") : device.name,
                device.level, device.iconKind);
        }
        painter.setPen(QPen(QColor(238, 240, 240, 25),
                      cardMetrics.stroke(painter, .0015)));
        for (int i = 1; i < kBatteryListRows; ++i) {
            const qreal y = card.top() + rowHeight * i;
            painter.drawLine(QPointF(card.left() + rowHeight * .88, y),
                             QPointF(card.right(), y));
        }
        painter.restore();
        return;
    }

    painter.restore();
}
