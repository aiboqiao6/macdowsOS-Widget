#include "widgets/desktopwidget.h"
#include "widgets/cardmetrics.h"
#include "app/fontsettings.h"
#include "rendering/responsivelayout.h"
#include <QPainter>
#include <QPainterPath>
#include <QtMath>

using namespace CardMetrics;
using AppFonts::displayFont;

void DesktopWidget::drawBatteryIcon(QPainter& painter, const QRectF& rect, int level, bool charging) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
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
                                   bool showCaption, qreal strokeMultiplier) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF ring(center.x() - radius, center.y() - radius,
                      radius * 2.0, radius * 2.0);
    const ResponsiveLayout::Metrics metrics(ring);
    const qreal stroke = metrics.stroke(painter, .0525)
                       * qMax<qreal>(0.1, strokeMultiplier);

    QPen track(QColor(238, 242, 250, connected ? 38 : 24), stroke,
               Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(track);
    painter.drawEllipse(ring);

    const int clamped = qBound(0, level, 100);
    if (connected && level >= 0 && clamped > 0) {
        const qreal span = -360.0 * clamped / 100.0;
        QPen progress(QColor(255, 255, 255, 242), stroke,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(progress);
        constexpr qreal startAngle = 90.0;
        painter.drawArc(ring, qRound(startAngle * 16.0), qRound(span * 16.0));

    }

    // 使用中性线条设备图标，未连接位置保留空环。
    if (iconKind == 0) {
        const QRectF screen(center.x() - radius * .34, center.y() - radius * .19,
                            radius * .68, radius * .44);
        painter.setPen(QPen(QColor(226, 232, 244, connected ? 165 : 55), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(screen, radius * .06, radius * .06);
        painter.drawLine(QPointF(center.x() - radius * .45, center.y() + radius * .29),
                   QPointF(center.x() + radius * .45, center.y() + radius * .29));
        painter.drawLine(QPointF(center.x() - radius * .28, center.y() + radius * .15),
                   QPointF(center.x() + radius * .28, center.y() + radius * .15));
    } else if (iconKind == 1) {
        const QRectF mouse(center.x() - radius * .19, center.y() - radius * .40,
                           radius * .38, radius * .74);
        painter.setPen(QPen(QColor(226, 232, 244, connected ? 165 : 55), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(QColor(226, 232, 244, connected ? 138 : 45));
        painter.drawRoundedRect(mouse, radius * .18, radius * .18);
        painter.drawLine(QPointF(center.x(), mouse.top() + radius * .08),
                   QPointF(center.x(), mouse.top() + radius * .29));
    } else if (iconKind == 2 && connected) {
        const QRectF keyboard(center.x() - radius * .39, center.y() - radius * .21,
                              radius * .78, radius * .42);
        painter.setPen(QPen(QColor(226, 232, 244, 165), stroke * .18,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(QColor(226, 232, 244, 34));
        painter.drawRoundedRect(keyboard, radius * .07, radius * .07);
        painter.drawLine(keyboard.bottomLeft() + QPointF(radius * .12, -radius * .10),
                   keyboard.bottomRight() + QPointF(-radius * .12, -radius * .10));
    } else if (iconKind == 3 && connected) {
        painter.setPen(QPen(QColor(226, 232, 244, 165), stroke * .20,
                      Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawArc(QRectF(center.x() - radius * .31, center.y() - radius * .31,
                         radius * .62, radius * .62), 0, 180 * 16);
        painter.drawLine(QPointF(center.x() - radius * .31, center.y()),
                   QPointF(center.x() - radius * .31, center.y() + radius * .28));
        painter.drawLine(QPointF(center.x() + radius * .31, center.y()),
                   QPointF(center.x() + radius * .31, center.y() + radius * .28));
    } else if (iconKind == 4 && connected) {
        painter.setPen(QPen(QColor(226, 232, 244, 165), stroke * .18));
        painter.setBrush(QColor(226, 232, 244, 34));
        painter.drawRoundedRect(QRectF(center.x() - radius * .24, center.y() - radius * .34,
                                 radius * .48, radius * .68), radius * .10, radius * .10);
    }

    // 空槽不显示说明，仅真实连接设备显示文字。
    if (showCaption && (connected || iconKind == 0)) {
        painter.setFont(displayFont(qMax(1, qRound(radius * .44)), QFont::Normal));
        painter.setPen(QColor(235, 240, 250, connected ? 182 : 66));
        ResponsiveLayout::drawSingleLine(painter, QRectF(center.x() - radius * .72,
                                 center.y() + radius * 1.55,
                                 radius * 1.44, radius * .85),
                       Qt::AlignHCenter | Qt::AlignTop,
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
        const bool hasPeripheral = !m_connectedDevices.isEmpty();
        const ConnectedDevice firstPeripheral = hasPeripheral ? m_connectedDevices.first() : ConnectedDevice{};
        // 仅绘制系统实际报告的设备和电量，空槽保留安静的玻璃圆环。
        const qreal ringRadius = qMin(card.width(), card.height()) * .190;
        if (m_cardVariant == 0) {
            // 四个圆环向卡片边缘展开，保持等距留白。
            const qreal left = card.left() + card.width() * .265;
            const qreal right = card.left() + card.width() * .735;
            const qreal top = card.top() + card.height() * .265;
            const qreal bottom = card.top() + card.height() * .735;
            drawDeviceRing(painter, QPointF(left, top), ringRadius,
                           m_hasBattery ? m_batteryLevel : -1, 0, m_hasBattery,
                           false, 1.30);
            drawDeviceRing(painter, QPointF(right, top), ringRadius,
                           hasPeripheral ? firstPeripheral.level : -1,
                           hasPeripheral ? firstPeripheral.iconKind : -1, hasPeripheral,
                           false, 1.30);
            drawDeviceRing(painter, QPointF(left, bottom), ringRadius,
                           -1, -1, false, false, 1.30);
            drawDeviceRing(painter, QPointF(right, bottom), ringRadius,
                           -1, -1, false, false, 1.30);
        } else {
            const qreal sideInset = card.width() * .145;
            const qreal firstX = card.left() + sideInset;
            const qreal spacing = (card.width() - sideInset * 2.0) / 3.0;
            const qreal ringY = card.top() + card.height() * .42;
            drawDeviceRing(painter, QPointF(firstX, ringY), ringRadius,
                           m_hasBattery ? m_batteryLevel : -1, 0, m_hasBattery);
            for (int slot = 0; slot < 3; ++slot) {
                const bool connected = slot < m_connectedDevices.size();
                const ConnectedDevice device = connected ? m_connectedDevices.at(slot)
                                                          : ConnectedDevice{};
                drawDeviceRing(painter, QPointF(firstX + spacing * (slot + 1), ringY),
                               ringRadius, device.level,
                               connected ? device.iconKind : -1, connected);
            }
        }
        painter.restore();
        return;
    }

    if (m_cardVariant == 2) {
        // 设备列表采用紧凑行高和适度边距，避免文字过大导致拥挤。
        const ResponsiveLayout::Metrics cardMetrics(card);
        const qreal padding = cardMetrics.size(.036);
        const QRectF content = card.adjusted(padding, padding, -padding, -padding);
        const auto row = [&painter](const QRectF& area, const QString& name,
                              int level, int iconKind) {
            const ResponsiveLayout::Metrics rowMetrics(area);
            const qreal s = area.height();
            painter.setPen(QPen(QColor(244, 248, 255, 210),
                          rowMetrics.stroke(painter, .015), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            const QPointF iconCenter(area.left() + s * .365, area.center().y());
            if (iconKind == 0) {
                painter.drawRoundedRect(QRectF(iconCenter.x() - s * .163,
                                         iconCenter.y() - s * .106,
                                         s * .327, s * .212),
                                  s * .019, s * .019);
                painter.drawLine(QPointF(iconCenter.x() - s * .231,
                                   iconCenter.y() + s * .154),
                           QPointF(iconCenter.x() + s * .231,
                                   iconCenter.y() + s * .154));
            } else if (iconKind == 1) {
                painter.drawRoundedRect(QRectF(iconCenter.x() - s * .096,
                                         iconCenter.y() - s * .163,
                                         s * .192, s * .327),
                                  s * .077, s * .077);
                painter.drawLine(QPointF(iconCenter.x(), iconCenter.y() - s * .106),
                           QPointF(iconCenter.x(), iconCenter.y() - s * .019));
            } else if (iconKind == 2) {
                painter.drawRoundedRect(QRectF(iconCenter.x() - s * .192,
                                         iconCenter.y() - s * .115,
                                         s * .385, s * .231),
                                  s * .029, s * .029);
                painter.drawLine(QPointF(iconCenter.x() - s * .135,
                                   iconCenter.y() + s * .048),
                           QPointF(iconCenter.x() + s * .135,
                                   iconCenter.y() + s * .048));
            } else if (iconKind == 3) {
                painter.drawArc(QRectF(iconCenter.x() - s * .163,
                                 iconCenter.y() - s * .163,
                                 s * .327, s * .327), 0, 180 * 16);
                painter.drawLine(QPointF(iconCenter.x() - s * .163, iconCenter.y()),
                           QPointF(iconCenter.x() - s * .163,
                                   iconCenter.y() + s * .135));
                painter.drawLine(QPointF(iconCenter.x() + s * .163, iconCenter.y()),
                           QPointF(iconCenter.x() + s * .163,
                                   iconCenter.y() + s * .135));
            } else {
                painter.drawRoundedRect(QRectF(iconCenter.x() - s * .115,
                                         iconCenter.y() - s * .163,
                                         s * .231, s * .327),
                                  s * .038, s * .038);
            }
            painter.setPen(QColor(246, 249, 255, 235));
            painter.setFont(displayFont(qMax(1, qRound(s * .192)), QFont::DemiBold));
            const int nameWidth = qMax(qRound(s * .77),
                                       qRound(area.width() - s * 2.31));
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(area.left() + s * .942, area.top(),
                          nameWidth, area.height()),
                Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(painter.font()).elidedText(name, Qt::ElideRight, nameWidth));
            painter.setFont(displayFont(qMax(1, qRound(s * .173)), QFont::Normal));
            painter.setPen(QColor(246, 249, 255, 235));
            ResponsiveLayout::drawSingleLine(
                painter, QRectF(area.right() - s * 1.471, area.top(),
                          s * .50, area.height()),
                Qt::AlignRight | Qt::AlignVCenter,
                level >= 0 ? QString::number(qBound(0, level, 100)) + QChar('%')
                           : QStringLiteral("—"));
            painter.setPen(QPen(QColor(255, 255, 255, 235),
                          rowMetrics.stroke(painter, .019), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            const QRectF batteryRect(area.right() - s * .769,
                                     area.center().y() - s * .077,
                                     s * .346, s * .154);
            painter.drawRoundedRect(batteryRect, s * .038, s * .038);
            if (level >= 0) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 235));
                const qreal inset = s * .038;
                const qreal fillWidth = (batteryRect.width() - inset * 2.0)
                    * qBound(0, level, 100) / 100.0;
                if (fillWidth > 0.0)
                    painter.drawRoundedRect(QRectF(batteryRect.left() + inset,
                                             batteryRect.top() + s * .029,
                                             fillWidth,
                                             batteryRect.height() - s * .058),
                                      s * .019, s * .019);
            }
            painter.setPen(QPen(QColor(255, 255, 255, 235),
                          rowMetrics.stroke(painter, .019), Qt::SolidLine,
                          Qt::RoundCap, Qt::RoundJoin));
            painter.drawLine(QPointF(area.right() - s * .385,
                               area.center().y() - s * .029),
                       QPointF(area.right() - s * .327,
                               area.center().y() - s * .029));
        };
        // 六行等高，只有真实设备填充内容，剩余行保持空白。
        const qreal rowHeight = content.height() / kBatteryListRows;
        const QRectF firstRow(content.left(), content.top(), content.width(), rowHeight);
        const QRectF secondRow(content.left(), content.top() + rowHeight,
                               content.width(), rowHeight);
        row(firstRow, m_hostDeviceName,
            m_hasBattery ? m_batteryLevel : -1, 0);
        for (int slot = 0; slot < qMin(kBatteryListRows - 1,
                                       int(m_connectedDevices.size())); ++slot) {
            const ConnectedDevice& device = m_connectedDevices.at(slot);
            const QRectF deviceRow(content.left(), secondRow.top() + rowHeight * slot,
                                   content.width(), rowHeight);
            row(deviceRow,
                device.name.isEmpty() ? QStringLiteral("已连接设备") : device.name,
                device.level, device.iconKind);
        }
        painter.setPen(QPen(QColor(255, 255, 255, 34),
                      cardMetrics.stroke(painter, .0015)));
        for (int i = 1; i < kBatteryListRows; ++i) {
            const qreal y = content.top() + rowHeight * i;
            painter.drawLine(QPointF(content.left() + rowHeight * .942, y),
                       QPointF(content.right(), y));
        }
        painter.restore();
        return;
    }

    painter.restore();
}
